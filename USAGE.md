# TPCM PCIe 通信系统 — 操作与接口文档

## 目录
1. [系统架构](#1-系统架构)
2. [编译指南](#2-编译指南)
3. [部署与加载](#3-部署与加载)
4. [Host 侧接口使用](#4-host-侧接口使用)
5. [A55 侧替换真实 TPCM 接口](#5-a55-侧替换真实-tpcm-接口)
6. [故障排查](#6-故障排查)

---

## 1. 系统架构

```
Kunpeng 950 (Host / PCIe RC)          Hi1712 A55 BMC (PCIe EP)
─────────────────────────────          ────────────────────────────────
业务程序                                 tpcm_hw.ko（待实现，可选）
  │  tpcm_measure(data, len, hash)         │  tpcm_register_backend(fn)
  ▼                                        ▼
libtpcm_pcie.so                        a55_tpcm_ep_driver.ko
  │  写 DMA 数据 + ring head               │  kthread 每 200µs 轮询
  ▼                                        │  tpcm_do_measure()
pci_bar4_driver.ko                         │    ├── 有后端 → 调真实 TPCM
  │  mmap /dev/pci_bar4_driver             │    └── 无后端 → 模拟哈希
  │                                        │
  │        ← PCIe BAR4 共享内存 →          │
  └─────────── 0x800000000000 ────────────┘
              A55 物理地址 0x8bc00000
```

### 共享内存布局（BAR4，32MB）

| 偏移 | 大小 | 用途 |
|------|------|------|
| `0x000000` | 64 B | ring buffer head（独占 cache line） |
| `0x000040` | 64 B | ring buffer tail（独占 cache line） |
| `0x000080` | 8 KB | cmds[128]，每条指令 64 字节 |
| `0x010000` | 32 MB | DMA 数据区，128 槽 × 256 KB |

---

## 2. 编译指南

### 2.1 Host 侧（在 Kunpeng 950 上执行）

```bash
cd /path/to/PCIE

# 编译 Host 内核驱动
make all

# 编译用户态测试程序
make producer

# 编译共享库（业务方接入用）
make lib          # 生成 libtpcm_pcie.so
make lib-static   # 生成 libtpcm_pcie.a（可选，嵌入可执行文件）
```

### 2.2 A55 侧（在 WSL 交叉编译环境中执行）

**前提**：WSL 中已安装 HCC 工具链和 Hi1712 内核源码。

```bash
# 确认工具链可用
/opt/hcc_arm64le-bmc/bin/aarch64-target-linux-gnu-gcc --version

# 把源码传到 WSL（.c 文件被内网拦截时，用 base64 绕过）


# 交叉编译（在 WSL 中执行）
export PATH=/opt/hcc_arm64le/bin:$PATH

make -f Makefile.a55 \
  ARCH=arm64 \
  CROSS_COMPILE=aarch64-target-linux-gnu- \
  KDIR=/opt/RTOS-bmc/208.11.0/arm64le_5.10_ek_preempt_pro

# 产物：a55_tpcm_ep_driver.ko
```

---

## 3. 部署与加载

### 3.1 Host 侧（Kunpeng 950）

```bash
# 1. 加载 Host PCIe 驱动
insmod pci_bar4_driver.ko

# 确认设备节点出现
ls -la /dev/pci_bar4_driver

# 2. 确认驱动日志
dmesg | grep -i "pci_bar4\|BAR4"

# 卸载
rmmod pci_bar4_driver
```

### 3.2 A55 侧（Hi1712 BMC）

```bash
# 把 .ko 传到 A55（内网传 .ko 不受限）
scp a55_tpcm_ep_driver.ko root@<bmc-ip>:/tmp/

# A55 上加载驱动（BMC 通常 insmod 在 /sbin/）
/sbin/insmod /tmp/a55_tpcm_ep_driver.ko

# 确认加载成功
cat /proc/modules | grep tpcm
# 预期输出：a55_tpcm_ep_driver ...

# 查看驱动日志
cat /dev/kmsg | grep TPCM
# 预期输出：
# [TPCM] ===== A55 TPCM EP 驱动初始化完成，等待 Host 侧指令 =====
# [TPCM] 轮询线程已启动: pid=xxx

# 运行时调整轮询间隔（无需重载驱动）
echo 500 > /sys/module/a55_tpcm_ep_driver/parameters/poll_interval_us

# 卸载
/sbin/rmmod a55_tpcm_ep_driver
```

#### 可选模块参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `shared_phys_base` | `0x8bc00000` | 共享内存物理基址 |
| `poll_interval_us` | `200` | 轮询间隔（µs），运行时可改 |

```bash
# 指定参数加载示例
/sbin/insmod /tmp/a55_tpcm_ep_driver.ko shared_phys_base=0x8bc00000 poll_interval_us=200
```

---

## 4. Host 侧接口使用

### 4.1 引入库

```c
#include "tpcm_pcie_client.h"
```

编译时链接共享库：

```bash
gcc -O2 -o myapp myapp.c \
    -I/path/to/PCIE \
    -L/path/to/PCIE -ltpcm_pcie \
    -Wl,-rpath,/path/to/PCIE
```

### 4.2 接口说明

#### `tpcm_open()` — 初始化

```c
void *tpcm_open(void);
```

- 打开 `/dev/pci_bar4_driver` 并 mmap BAR4 共享内存
- **返回**：成功返回不透明 handle，失败返回 `NULL`（errno 已设置）
- 线程安全：每个线程应持有独立 handle

#### `tpcm_close()` — 释放

```c
void tpcm_close(void *handle);
```

- munmap + close fd，释放所有资源

#### `tpcm_measure()` — 内存数据度量

```c
int tpcm_measure(void       *handle,
                 const void *data,
                 size_t      len,
                 uint8_t     hash_out[32],
                 int         timeout_ms);
```

| 参数 | 说明 |
|------|------|
| `handle` | `tpcm_open()` 返回的句柄 |
| `data` | 待度量数据起始地址 |
| `len` | 数据字节数，≤ 256 KB（`TPCM_MAX_DATA_LEN`） |
| `hash_out` | 输出缓冲区，调用方分配，至少 32 字节 |
| `timeout_ms` | 超时（毫秒），传 0 使用默认值 5000 ms |

**返回值**：`TPCM_OK(0)` 成功，负数为错误码（见下表）

#### `tpcm_measure_file()` — 文件度量

```c
int tpcm_measure_file(void       *handle,
                      const char *filepath,
                      uint8_t     hash_out[32],
                      int         timeout_ms);
```

- 自动读取文件内容并度量
- 大文件（> 256 KB）按段度量，取最后段哈希（生产环境可改为 Merkle 树）

#### 错误码

| 常量 | 值 | 含义 |
|------|----|------|
| `TPCM_OK` | 0 | 成功 |
| `TPCM_ERR_OPEN` | -1 | 设备打开 / mmap 失败 |
| `TPCM_ERR_PARAM` | -2 | 参数非法 |
| `TPCM_ERR_FULL` | -3 | ring buffer 已满（128 条指令堆积） |
| `TPCM_ERR_TIMEOUT` | -4 | A55 未在超时内完成度量 |
| `TPCM_ERR_HW` | -5 | A55 硬件度量失败 |
| `TPCM_ERR_IO` | -6 | 文件读取失败 |

#### 工具函数

```c
// 32 字节哈希 → 64 字符十六进制字符串
void tpcm_hash_to_hex(const uint8_t hash[32], char hexbuf[65]);

// 错误码 → 可读字符串
const char *tpcm_strerror(int err);
```

### 4.3 完整使用示例

```c
#include <stdio.h>
#include "tpcm_pcie_client.h"

int main(void)
{
    uint8_t hash[32];
    char    hexstr[65];
    int     ret;

    /* 1. 初始化 */
    void *h = tpcm_open();
    if (!h) {
        perror("tpcm_open");
        return 1;
    }

    /* 2a. 对一段内存做度量 */
    const char *msg = "Hello TPCM";
    ret = tpcm_measure(h, msg, strlen(msg), hash, 5000);
    if (ret == TPCM_OK) {
        tpcm_hash_to_hex(hash, hexstr);
        printf("内存度量结果: %s\n", hexstr);
    } else {
        fprintf(stderr, "度量失败: %s\n", tpcm_strerror(ret));
    }

    /* 2b. 对文件做度量 */
    ret = tpcm_measure_file(h, "/boot/vmlinuz", hash, 10000);
    if (ret == TPCM_OK) {
        tpcm_hash_to_hex(hash, hexstr);
        printf("文件度量结果: %s\n", hexstr);
    } else {
        fprintf(stderr, "文件度量失败: %s\n", tpcm_strerror(ret));
    }

    /* 3. 释放 */
    tpcm_close(h);
    return ret;
}
```

---

## 5. A55 侧替换真实 TPCM 接口

> **当前状态**：驱动内置模拟后端，可正常运行。  
> 获取到真实 TPCM 内核 API 后，按本节步骤替换，**无需修改 `a55_tpcm_ep_driver.c`**。

### 5.1 原理

`a55_tpcm_ep_driver.ko` 导出了一个注册接口：

```c
// 回调原型
typedef int (*tpcm_backend_fn_t)(const u8 *data, size_t len, u8 hash_out[32]);

// 注册 / 注销
void tpcm_register_backend(tpcm_backend_fn_t fn);  // EXPORT_SYMBOL
```

只需编写一个独立的 `tpcm_hw.ko`，在其 init 中注册真实度量函数即可。

### 5.2 实现模板

新建 `tpcm_hw.c`：

```c
// tpcm_hw.c — 真实 TPCM 后端（填入真实 API 后编译）
#include <linux/module.h>
#include <linux/kernel.h>

/* a55_tpcm_ep_driver.ko 导出的注册函数 */
extern void tpcm_register_backend(
    int (*fn)(const u8 *data, size_t len, u8 hash_out[32]));

/* ★ 替换此函数体为真实 TPCM 调用 ★ */
static int my_tpcm_sha256(const u8 *data, size_t len, u8 hash_out[32])
{
    /*
     * 示例：调用 Hi1712 TPCM 内核驱动导出的 SHA-256 接口
     *
     * 可能的接口形式（以实际头文件为准）：
     *   return tpcm_drv_hash_sha256(data, len, hash_out);
     *   return tpcm_extend_pcr(0, data, len, hash_out);
     *
     * 确认 TPCM 驱动接口后填入，目前留空返回 -ENOSYS。
     */
    (void)data; (void)len; (void)hash_out;
    pr_err("[TPCM_HW] 真实 TPCM 接口未实现，请替换此函数\n");
    return -ENOSYS;
}

static int __init tpcm_hw_init(void)
{
    tpcm_register_backend(my_tpcm_sha256);
    pr_info("[TPCM_HW] 真实 TPCM 后端已注册\n");
    return 0;
}

static void __exit tpcm_hw_exit(void)
{
    tpcm_register_backend(NULL);  /* 注销，恢复模拟模式 */
    pr_info("[TPCM_HW] 真实 TPCM 后端已注销\n");
}

module_init(tpcm_hw_init);
module_exit(tpcm_hw_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hi1712 TPCM 真实度量后端");
```

### 5.3 编译 tpcm_hw.ko

在 `Makefile.a55` 旁边新建 `Kbuild.hw`（或直接复用 `Makefile.a55`）：

```bash
# 与 a55_tpcm_ep_driver.ko 同环境编译
export PATH=/opt/hcc_arm64le/bin:$PATH

# 先编译 a55_tpcm_ep_driver.ko，生成 Module.symvers（含导出符号）
make -f Makefile.a55 ARCH=arm64 CROSS_COMPILE=aarch64-target-linux-gnu- \
  KDIR=/opt/RTOS/208.11.0/arm64le_5.10_ek_preempt_pro

# 编译 tpcm_hw.ko（需要 Module.symvers 解析 tpcm_register_backend）
echo "obj-m := tpcm_hw.o" > Kbuild
make -C /opt/RTOS/208.11.0/arm64le_5.10_ek_preempt_pro \
  M=$(pwd) ARCH=arm64 CROSS_COMPILE=aarch64-target-linux-gnu- \
  KBUILD_EXTRA_SYMBOLS=$(pwd)/Module.symvers modules
rm Kbuild
```

### 5.4 加载顺序

```bash
# A55 上按顺序加载
/sbin/insmod /tmp/a55_tpcm_ep_driver.ko   # 先加载基础驱动
/sbin/insmod /tmp/tpcm_hw.ko              # 后加载后端（自动注册）

# 验证日志
cat /dev/kmsg | grep TPCM
# 预期：[TPCM_HW] 真实 TPCM 后端已注册
# 预期：[TPCM] 度量后端已注册（真实 TPCM）
```

---

## 6. 故障排查

| 现象 | 原因 | 解决 |
|------|------|------|
| `insmod: invalid module format` | .ko 与运行内核版本不匹配 | 用 Hi1712 5.10.0 内核源码重新交叉编译 |
| `tpcm_open()` 返回 NULL | `/dev/pci_bar4_driver` 不存在 | 先在 950 上加载 `pci_bar4_driver.ko` |
| `TPCM_ERR_TIMEOUT` | A55 驱动未加载或处理慢 | 检查 A55 上 `cat /proc/modules \| grep tpcm` |
| `TPCM_ERR_FULL` | ring buffer 满（128 条积压） | 等 A55 消费后重试，或加大超时 |
| A55 日志无输出 | dmesg 权限不足 | 用 `cat /dev/kmsg` 代替 dmesg |
| 编译报 `arch//Makefile not found` | ARCH 参数为空 | 显式传 `ARCH=arm64` |
| 编译报 `No rule to make target *.o` | .c 源文件缺失 | 用 base64 绕过内网将 .c 文件传入 |
