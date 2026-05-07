# TPCM PCIe 高速动态度量 Demo

基于 PCIe 共享内存的 TPCM 高速动态度量项目驱动代码。

## 硬件架构

```
鲲鹏 950 服务器                          Hi1712 BMC
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
                                        (0x8bc00000, 32MB)
                                              │
                                        a55_tpcm_ep_driver.ko
                                        kthread 轮询 head 指针
                                              │
                                        处理度量指令，回填 hash
                                              │
  Host 轮询 status == DONE ◀─────────────────┘
  读取 hash_result
```

**通信方式：纯共享内存轮询（无中断）**
- Host 写入度量指令并推进 head 指针
- A55 内核线程（默认 200µs 间隔）轮询 head，检测到新指令后消费
- A55 回填 hash_result，写 `status=DONE`，推进 tail
- Host 轮询 status 字段感知完成，无需 MSI 中断

---

## 文件说明

| 文件 | 运行侧 | 说明 |
|------|--------|------|
| `tcm_pcie_protocol.h` | 双侧共用 | 通信协议定义：Ring Buffer 结构体、指令格式、状态码 |
| `pci_bar4_driver.c` | Host (鲲鹏950) | PCIe BAR4 内核驱动，提供字符设备 `/dev/pci_bar4_driver` |
| `host_tpcm_producer.c` | Host (鲲鹏950) | Ring Buffer 生产者测试程序，投递度量指令并等待结果 |
| `a55_tpcm_ep_driver.c` | A55 (Hi1712 BMC) | PCIe Endpoint 消费者驱动，kthread 轮询处理度量指令 |
| `Makefile.a55` | A55 编译环境 | A55 侧驱动 out-of-tree 编译脚本 |
| `Makefile` | Host 编译环境 | Host 侧驱动和测试程序编译脚本 |
| `pci_bar4_test.c` | Host (鲲鹏950) | 原始 BAR4 裸读写测试程序（不含 Ring Buffer 逻辑） |
| `devmem2.c` | 通用 | 物理内存直接读写工具（调试用） |

---

## 通信协议

### Ring Buffer 内存布局（BAR4 起始处 = A55 DDR 0x8bc00000）

```
BAR4 偏移      内容                    大小
0x0000         head 指针 (Host 维护)   64 字节 (独占 Cache Line)
0x0040         tail 指针 (A55 维护)    64 字节 (独占 Cache Line)
0x0080         cmds[128]              128 × 64 = 8192 字节
0x2080         剩余空间               供 DMA 数据区使用
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

### 通信时序（纯轮询模式）

```
Host                                    A55 (kthread, 200µs 轮询)
 │                                       │
 │  1. 写指令字段到 cmds[head]            │  轮询 head 指针
 │  2. 写 status = PENDING               │
 │  3. __sync_synchronize() 内存屏障     │
 │  4. 推进 head 指针                    │
 │                                       │  检测到 head != tail
 │                                       │  smp_rmb()
 │                                       │  读指令，执行 TCM 度量
 │                                       │  写 hash_result
 │                                       │  smp_wmb()
 │                                       │  写 status = DONE
 │                                       │  推进 tail，writel()
 │  5. 轮询 status == DONE ◀─────────── │
 │  6. 读取 hash_result                  │
```

---

## 硬件参数常量

### 已确认参数（950 + A55 实测）

| 参数 | 值 | 说明 | 确认来源 |
|------|----|------|---------|
| `TARGET_BUS/DEV/FUNC` | `21:00.7` | Hi1712 PCIe 设备 BDF | `lspci -vvv -s 21:00.7` |
| BAR4 Host 地址 | `0x800000000000` / 32MB | Host 侧 BAR4 物理地址 | lspci，驱动通过 `pci_resource_start()` 自动读取 |
| `A55_SHARED_PHYS_BASE` | `0x8bc00000` | A55 本地 DDR，BAR4 Inbound ATU 目标地址 | A55 DDR 布局确认 |
| `TPCM_POLL_INTERVAL_US` | `200` | A55 kthread 轮询间隔（微秒） | 可通过模块参数运行时调整 |

### 已移除参数（轮询模式不再需要）

| 参数 | 原值 | 移除原因 |
|------|------|---------|
| `A55_DOORBELL_IRQ` | `50` | 轮询模式无需门铃中断 |
| `A55_MSI_TRIGGER_REG_PHYS` | `0x1A000040` | 轮询模式无需 MSI 回调 |
| `DOORBELL_BAR4_OFFSET` | `0x10000` | 轮询模式无需 Host 触发中断 |

---

## 编译与部署

### Host 侧（鲲鹏 950）

```bash
# 编译内核驱动模块
make all

# 编译用户态测试程序
make producer

# 加载驱动
sudo insmod pci_bar4_driver.ko

# 确认 BAR4 映射成功
dmesg | grep "BAR4"
# 期望输出: BAR4 start: 0x800000000000, size: 33554432 bytes

# 运行生产者测试（发送 4 条度量指令）
./host_tpcm_producer

# 发送指定条数
./host_tpcm_producer 10

# 卸载驱动
sudo rmmod pci_bar4_driver
```

### A55 侧（Hi1712 BMC）

```bash
# 在开发机上交叉编译（需要 Hi1712 内核源码）
make -f Makefile.a55 \
     KDIR=/path/to/hi1712-kernel \
     ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu-

# 或在 A55 板子上本地编译（需要板子上有内核头文件）
make -f Makefile.a55

# 拷贝到 A55
scp a55_tpcm_ep_driver.ko root@<bmc-ip>:/tmp/

# 在 A55 上加载驱动（默认参数）
insmod /tmp/a55_tpcm_ep_driver.ko

# 自定义参数（不重新编译直接调参）
insmod /tmp/a55_tpcm_ep_driver.ko \
    shared_phys_base=0x8bc00000 \
    poll_interval_us=200

# 运行时动态调整轮询间隔（驱动加载后）
echo 500 > /sys/module/a55_tpcm_ep_driver/parameters/poll_interval_us

# 查看驱动日志
cat /dev/kmsg 2>/dev/null | grep TPCM | tail -20
```

### 联调验证流程

```bash
# 步骤 1：A55 侧先加载驱动（必须先于 Host 侧，否则 Ring Buffer 未初始化）
ssh root@<bmc-ip> "insmod /tmp/a55_tpcm_ep_driver.ko"
# 期望日志: ===== A55 TPCM EP 驱动初始化完成，等待 Host 侧指令 =====
# 期望日志: 轮询线程已启动: pid=xxx

# 步骤 2：Host 侧加载驱动
sudo insmod pci_bar4_driver.ko
dmesg | grep BAR4
# 期望: BAR4 start: 0x800000000000, size: 33554432 bytes

# 步骤 3：Host 侧发送度量指令
./host_tpcm_producer 4

# 步骤 4：观察两侧日志
# Host 侧期望输出:
#   [HOST] 投递指令: cmd_id=0x00000001 ...
#   [HOST] 度量完成！cmd_id=0x00000001, hash[0..7]: xxxxxxxx xxxxxxxx

# A55 侧期望日志:
#   [TPCM] 检测到新指令: head=4, tail=0, 待处理=4 条
#   [TPCM] 开始度量: cmd_id=0x00000001 ...
#   [TPCM] 度量完成: cmd_id=0x00000001 ...
#   [TPCM] tail 已更新至 4，共处理 4 条指令
```

---

## 内存屏障说明

ARM64 是弱内存序架构，跨 PCIe 的共享内存访问必须严格使用内存屏障。

| 位置 | 屏障 | 原因 |
|------|------|------|
| Host 写完指令内容，推进 head 前 | `__sync_synchronize()` | 防止 Store-Store 乱序：确保 cmds[] 内容在 head 更新前对 A55 可见 |
| A55 读 head 前 | `smp_rmb()` | 防止 Load-Load 乱序：确保看到 Host 最新写入的 cmds[] |
| A55 写完 hash_result，写 status=DONE 前 | `smp_wmb()` | 防止 Store-Store 乱序：确保哈希数据在状态标志更新前对 Host 可见 |
| A55 写 tail（writel） | 隐含 release 语义 | ARM64 的 writel 相当于内嵌 smp_wmb，tail 更新对 Host 原子可见 |

---

## 注意事项

1. **加载顺序**：A55 侧驱动必须先于 Host 侧驱动加载，否则共享内存 head/tail 未清零
2. **ioremap 类型**：共享内存用 `ioremap_wc`（Write-Combining），提升 PCIe 写吞吐
3. **轮询开销**：200µs 间隔下 A55 kthread CPU 占用极低（空载时每次仅读一个 u32）；
   如需降低延迟可调小 `poll_interval_us`，如需降低 CPU 占用可调大
4. **host_phys_addr**：测试程序使用模拟地址 `0xDEAD0000`，真实场景需通过 `virt_to_phys()` 或 DMA API 获取
5. **A55 侧模拟度量**：`a55_tpcm_ep_driver.c` 中用 `msleep(2)` 模拟 TCM 硬件计算，
   真实部署需替换为 DMA 搬运 + TCM 硬件引擎调用
6. **Host 驱动占用冲突**：BAR4 已被 `edma_drv` 占用，`request_mem_region` 已注释掉，
   使用 `ioremap_wc` 强制映射，功能不受影响
