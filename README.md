# TPCM PCIe 高速动态度量 Demo

基于 PCIe DMA 的 TPCM 高速动态度量项目驱动代码。

## 硬件架构

```
鲲鹏 920 服务器                          Hi1712 BMC
(PCIe Root Complex)                    (PCIe Endpoint, A55 核心)

  Host 用户态程序
      │ ioctl(PCI_BAR4_WRITE)
      ▼
  pci_bar4_driver.ko
      │ memcpy_toio() 写 BAR4
      ▼
  PCIe 总线 ──────────────────────────▶ Inbound ATU 地址转换
                                              │
                                              ▼
                                        A55 本地 DDR
                                        (0x8cd00000, 32MB)
                                              │
                                        a55_tpcm_ep_driver.ko
                                        收到门铃 IRQ 45
                                              │
                                        工作队列处理度量指令
                                              │
                                        写 MSI 触发寄存器
                                              │
  Host 收到 MSI ◀────────────────────────────┘
  读取 hash_result
```

## 文件说明

| 文件 | 运行侧 | 说明 |
|------|--------|------|
| `tcm_pcie_protocol.h` | 双侧共用 | 通信协议定义：Ring Buffer 结构体、指令格式、状态码 |
| `pci_bar4_driver.c` | Host (鲲鹏920) | PCIe BAR4 内核驱动，提供字符设备 `/dev/pci_bar4_driver` |
| `host_tpcm_producer.c` | Host (鲲鹏920) | Ring Buffer 生产者测试程序，投递度量指令并等待结果 |
| `a55_tpcm_ep_driver.c` | A55 (Hi1712 BMC) | PCIe Endpoint 消费者驱动，处理度量指令并回填 hash |
| `Makefile.a55` | A55 编译环境 | A55 侧驱动 out-of-tree 编译脚本 |
| `makefile` | Host 编译环境 | Host 侧驱动和测试程序编译脚本 |
| `pci_bar4_test.c` | Host (鲲鹏920) | 原始 BAR4 裸读写测试程序（不含 Ring Buffer 逻辑） |
| `example_kernel_module.c` | Host (鲲鹏920) | 内核模块间调用示例（EXPORT_SYMBOL 演示） |
| `devmem2.c` | 通用 | 物理内存直接读写工具（参考用） |

---

## 通信协议

### Ring Buffer 内存布局（BAR4 起始处）

```
BAR4 偏移      内容                    大小
0x0000         head 指针 (Host 维护)   64 字节 (独占 Cache Line)
0x0040         tail 指针 (A55 维护)    64 字节 (独占 Cache Line)
0x0080         cmds[128]              128 × 64 = 8192 字节
0x2080         剩余空间               供 DMA 数据区使用
0x10000        门铃寄存器偏移          写任意值触发 A55 IRQ
```

### 单条度量指令格式（64 字节，独占一条 Cache Line）

```c
struct tcm_measure_cmd {
    uint32_t cmd_id;            // 流水号，Host 单调递增
    uint8_t  status;            // 状态码（见下表）
    uint8_t  payload_len_hi;    // payload 长度高 8 位
    uint16_t payload_len;       // 待度量数据长度（字节）
    uint64_t host_phys_addr;    // Host 侧待度量数据物理地址
    uint8_t  hash_result32[32]; // A55 回填的 SHA-256 结果
    uint8_t  _pad[12];          // 填充至 64 字节
};
```

### 指令状态码

| 值 | 宏 | 含义 |
|----|----|------|
| `0x00` | `TCM_STATUS_PENDING` | Host 已投递，等待 A55 处理 |
| `0x01` | `TCM_STATUS_PROCESSING` | A55 正在执行度量 |
| `0x02` | `TCM_STATUS_DONE` | 度量完成，hash_result 有效 |
| `0xFF` | `TCM_STATUS_ERROR` | 度量失败 |

### 通信时序

```
Host                                    A55
 │                                       │
 │  1. 写指令字段到 cmds[head]            │
 │  2. 写 status = PENDING               │
 │  3. __sync_synchronize() 内存屏障     │
 │  4. 推进 head 指针                    │
 │  5. 写门铃寄存器 ──────────────────▶  │ 触发 IRQ 45
 │                                       │ ISR: schedule_work()
 │                                       │ 工作队列: smp_rmb()
 │                                       │ 读 head，消费指令
 │                                       │ 执行 TCM 度量
 │                                       │ 写 hash_result
 │                                       │ smp_wmb()
 │                                       │ 写 status = DONE
 │                                       │ 推进 tail
 │                                       │ smp_mb()
 │  收到 MSI ◀────────────────────────── │ 写 MSI 触发寄存器
 │  6. 轮询 status == DONE               │
 │  7. 读取 hash_result                  │
```

---

## 硬件参数常量

> **联调前必须根据真实硬件手册核对以下参数**

| 参数 | 当前值 | 含义 | 确认方式 |
|------|--------|------|---------|
| `TARGET_BUS/DEV/FUNC` | `02:01.1` | Hi1712 PCIe 设备 BDF 地址 | `lspci -vvv` 查看 |
| `A55_SHARED_PHYS_BASE` | `0x8cd00000` | A55 侧 32MB 共享内存物理基址 | Hi1712 手册 → PCIe Inbound ATU Target Address |
| `A55_DOORBELL_IRQ` | `45` | 920 敲击 A55 触发的门铃中断号 | A55 上 `cat /proc/interrupts` |
| `A55_MSI_TRIGGER_REG_PHYS` | `0x1A000040` | A55 向 920 发 MSI 的触发寄存器地址 | Hi1712 手册 → PCIe EP DBI 寄存器 |
| `DOORBELL_BAR4_OFFSET` | `0x10000` | Host 写门铃的 BAR4 内偏移 | Hi1712 手册 → PCIe Outbound Doorbell |

### lspci 能看到什么

```bash
lspci -vvv -s 02:01.1
```

- **能看到**：BAR4 的 Host 侧物理地址和大小（代码通过 `pci_resource_start()` 自动读取，无需手填）
- **看不到**：后四个参数，均在 A55/Hi1712 内部，必须查硬件手册

---

## 编译与部署

### Host 侧（鲲鹏 920）

```bash
# 编译内核驱动模块 + 用户态测试程序
make all
make producer

# 加载驱动
make install
# 等价于: sudo insmod pci_bar4_driver.ko

# 确认 BAR4 映射成功
dmesg | grep "BAR4"
# 期望输出: BAR4 start: 0x..., size: 33554432 bytes

# 运行生产者测试（发送 4 条度量指令）
./host_tpcm_producer

# 发送指定条数
./host_tpcm_producer 10

# 卸载驱动
make remove
```

### A55 侧（Hi1712 BMC）

```bash
# 在开发机上交叉编译
make -f Makefile.a55 \
     KDIR=/path/to/hi1712-kernel \
     ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu-

# 拷贝到 A55 板子
scp a55_tpcm_ep_driver.ko root@<bmc-ip>:/tmp/

# 在 A55 上加载驱动
ssh root@<bmc-ip>
insmod /tmp/a55_tpcm_ep_driver.ko

# 确认初始化成功
dmesg | grep TPCM
# 期望输出: ===== A55 TPCM EP 驱动初始化完成，等待 Host 侧指令 =====
```

### 联调验证流程

```bash
# 步骤 1：A55 侧先加载驱动（必须先于 Host 侧）
ssh root@<bmc-ip> "insmod /tmp/a55_tpcm_ep_driver.ko"

# 步骤 2：Host 侧加载驱动
sudo insmod pci_bar4_driver.ko

# 步骤 3：Host 侧发送度量指令
./host_tpcm_producer 4

# 步骤 4：同时观察两侧日志
# Host 侧
dmesg | grep -E "BAR4|TPCM" | tail -20

# A55 侧
ssh root@<bmc-ip> "dmesg | grep TPCM | tail -20"
# 期望看到: 收到门铃中断 irq=45 → 工作队列启动 → 度量完成 → MSI 已触发
```

---

## 内存屏障说明

ARM64 是弱内存序架构，跨 PCIe 的共享内存访问必须严格使用内存屏障，否则会出现数据可见性问题。

| 位置 | 屏障 | 原因 |
|------|------|------|
| Host 写完指令内容，推进 head 前 | `__sync_synchronize()` | 防止 Store-Store 乱序：确保 cmds[] 内容在 head 更新前对 A55 可见 |
| A55 读 head 前 | `smp_rmb()` | 防止 Load-Load 乱序：确保看到 Host 最新写入的 cmds[] |
| A55 写完 hash_result，推进 tail 前 | `smp_wmb()` | 防止 Store-Store 乱序：确保结果在 tail 更新前对 Host 可见 |
| A55 推进 tail 后，触发 MSI 前 | `smp_mb()` | 全屏障：确保 tail 对 Host 完全可见后再发 MSI |

---

## 注意事项

1. **加载顺序**：A55 侧驱动必须先于 Host 侧驱动加载，否则共享内存未初始化
2. **ioremap 类型**：共享内存用 `ioremap_wc`（Write-Combining），MSI 寄存器用 `ioremap`（Strongly-Ordered），不可混用
3. **门铃偏移**：`DOORBELL_BAR4_OFFSET = 0x10000` 是占位值，联调前必须查 Hi1712 手册确认
4. **host_phys_addr**：测试程序中使用模拟地址 `0xDEAD0000`，真实场景需通过 `virt_to_phys()` 或 DMA API 获取实际物理地址
5. **A55 侧模拟度量**：`a55_tpcm_ep_driver.c` 中用 `msleep(2)` 模拟 TCM 硬件计算，真实部署需替换为 DMA 搬运 + TCM 硬件引擎调用
