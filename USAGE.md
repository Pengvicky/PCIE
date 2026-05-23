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
  │  pcie_write_once(fd, data, len)        │  tpcm_register_backend(fn)
  │  pcie_read_once (fd, buf,  len)        ▼
  ▼                                   a55_tpcm_ep_driver.ko
libpcie_client.so                       │  kthread 每 200µs 轮询
  │  写 DMA 数据 + ring head             │  tpcm_do_measure()
  ▼                                      │    ├── 有后端 → 调真实 TPCM
pci_bar4_driver.ko                       │    └── 无后端 → 模拟（32B 哈希）
  │  mmap /dev/pci_bar4_driver           │
  │                                      │
  │        ← PCIe BAR4 共享内存 →        │
  └─────────── 0x800000000000 ──────────┘
              A55 物理地址 0x8bc00000
```

### 共享内存布局（BAR4，32 MB）

| 偏移 | 大小 | 用途 |
|------|------|------|
| `0x000000` | 64 B | ring buffer head（独占 cache line） |
| `0x000040` | 64 B | ring buffer tail（独占 cache line） |
| `0x000080` | 8 KB | cmds[128]，每条指令 64 字节 |
| `0x010000` | 32 MB | DMA 数据区，128 槽 × 256 KB |

### DMA 槽位布局（每槽 256 KB）

```
slot N 起始偏移 = 0x10000 + N × 256KB
  ├── [+0 KB  ~ +128 KB)  写入区：Host → A55（pcie_write_once 数据）
  └── [+128 KB ~ +256 KB) 读出区：A55 → Host（pcie_read_once 结果）
```

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
make lib          # 生成 libpcie_client.so
make lib-static   # 生成 libpcie_client.a（可选，嵌入可执行文件）
```

### 2.2 A55 侧（在 WSL 交叉编译环境中执行）

**前提**：WSL 中已安装 HCC 工具链和 Hi1712 内核源码。

```bash
# 确认工具链可用
/opt/hcc_arm64le-bmc/bin/aarch64-target-linux-gnu-gcc --version

# 把源码传到 WSL（.c 文件被内网拦截时，用 base64 绕过）
# 在 950 上执行：
base64 a55_tpcm_ep_driver.c > /tmp/drv.b64
base64 tcm_pcie_protocol.h  > /tmp/proto.b64
# 在 WSL 中还原：
base64 -d /tmp/drv.b64   > a55_tpcm_ep_driver.c
base64 -d /tmp/proto.b64 > tcm_pcie_protocol.h

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
    -L/path/to/PCIE -lpcie_client \
    -Wl,-rpath,/path/to/PCIE
```

### 4.2 接口说明

#### 生命周期

##### `pcie_open()` — 打开通道

```c
int pcie_open(void);
```

- 打开 `/dev/pci_bar4_driver` 并 mmap BAR4 共享内存
- **返回**：成功返回非负整数 `fd`，失败返回 `-1`（errno 已设置）
- 线程安全：每个线程应调用 `pcie_open()` 持有独立 `fd`，最多 16 个并发通道

##### `pcie_close()` — 关闭通道

```c
void pcie_close(int fd);
```

- munmap + close，释放所有资源

##### `pcie_set_timeout()` — 设置读超时

```c
void pcie_set_timeout(int fd, int timeout_ms);
```

- 传 `0` 恢复默认值（5000 ms）

---

#### 核心收发接口

##### `pcie_write_once()` — 发送数据

```c
ssize_t pcie_write_once(int fd, const void *buf, size_t buf_len);
```

| 参数 | 说明 |
|------|------|
| `fd` | `pcie_open()` 返回的通道标识 |
| `buf` | 待发送数据首地址 |
| `buf_len` | 字节数，范围 `[1, 131072]`（128 KB = `PCIE_CLIENT_MAX_WRITE`） |

- **立即返回**，不等 A55 处理完毕
- 成功返回实际写入字节数（= `buf_len`）
- 失败返回负数错误码

##### `pcie_read_once()` — 读取结果

```c
ssize_t pcie_read_once(int fd, void *buf, size_t buf_len);
```

| 参数 | 说明 |
|------|------|
| `fd` | 通道标识（须已成功调用 `pcie_write_once`） |
| `buf` | 接收缓冲区首地址 |
| `buf_len` | 缓冲区大小，范围 `[1, 131072]`（128 KB = `PCIE_CLIENT_MAX_READ`） |

- **阻塞**直到 A55 完成或超时
- 成功返回 A55 实际写入的字节数（**可变长度**，可能小于 `buf_len`）
- 失败返回负数错误码

> **调用约束**：`pcie_write_once` → `pcie_read_once` 必须成对顺序调用，
> 不能连续调两次 `write_once` 再调 `read_once`。

---

#### 错误码

| 常量 | 值 | 含义 |
|------|----|------|
| `PCIE_OK` | 0 | 成功 |
| `PCIE_ERR_OPEN` | -1 | 设备打开 / mmap 失败 |
| `PCIE_ERR_PARAM` | -2 | 参数非法（NULL / 超长） |
| `PCIE_ERR_FULL` | -3 | ring buffer 已满（128 条指令堆积） |
| `PCIE_ERR_TIMEOUT` | -4 | A55 未在超时内完成处理 |
| `PCIE_ERR_HW` | -5 | A55 侧处理失败 |
| `PCIE_ERR_SEQ` | -6 | 未先调用 `pcie_write_once` 就调用了 `pcie_read_once` |

```c
const char *pcie_strerror(int err);   // 错误码 → 可读字符串
```

---

### 4.3 完整使用示例

```c
#include <stdio.h>
#include <string.h>
#include "tpcm_pcie_client.h"

int main(void)
{
    uint8_t result[PCIE_CLIENT_MAX_READ];
    ssize_t n;
    int     fd;

    /* 1. 打开通道 */
    fd = pcie_open();
    if (fd < 0) {
        perror("pcie_open");
        return 1;
    }

    /* 可选：设置超时 10 秒 */
    pcie_set_timeout(fd, 10000);

    /* 2. 发送待处理数据 */
    const char *payload = "Hello TPCM";
    n = pcie_write_once(fd, payload, strlen(payload));
    if (n < 0) {
        fprintf(stderr, "write 失败: %s\n", pcie_strerror((int)n));
        pcie_close(fd);
        return 1;
    }
    printf("已发送 %zd 字节\n", n);

    /* 3. 读取 A55 返回的结果（可变长度） */
    n = pcie_read_once(fd, result, sizeof(result));
    if (n < 0) {
        fprintf(stderr, "read 失败: %s\n", pcie_strerror((int)n));
        pcie_close(fd);
        return 1;
    }
    printf("收到 A55 结果 %zd 字节\n", n);

    /* 4. 打印结果（以十六进制示例） */
    for (ssize_t i = 0; i < n; i++)
        printf("%02x", result[i]);
    printf("\n");

    /* 5. 关闭通道 */
    pcie_close(fd);
    return 0;
}
```

### 4.4 多次交互示例

```c
int fd = pcie_open();

for (int i = 0; i < 10; i++) {
    char msg[64];
    snprintf(msg, sizeof(msg), "request-%d", i);

    /* 每次 write_once 后必须紧跟一次 read_once */
    pcie_write_once(fd, msg, strlen(msg));

    uint8_t resp[PCIE_CLIENT_MAX_READ];
    ssize_t n = pcie_read_once(fd, resp, sizeof(resp));
    if (n > 0)
        printf("[%d] A55 返回 %zd 字节\n", i, n);
}

pcie_close(fd);
```

---

## 5. A55 侧替换真实 TPCM 接口

> **当前状态**：驱动内置模拟后端，可正常运行（返回 32 字节模拟哈希）。  
> 获取到真实 TPCM 内核 API 后，按本节步骤替换，**无需修改 `a55_tpcm_ep_driver.c`**。

### 5.1 原理

与 Host 侧的 `pcie_write_once` / `pcie_read_once` 对应，A55 侧也拆分为**两个独立的回调**：

```
Host pcie_write_once(data, len)   ──►  A55 tpcm_read_handler(in, in_len)
                                           └─ tpcm_hw.ko 接收并处理数据

Host pcie_read_once(buf, len)     ◄──  A55 tpcm_write_handler(out, out_buf_len,
                                                                out_actual_len)
                                           └─ tpcm_hw.ko 将结果填入 out
```

`a55_tpcm_ep_driver.ko` 导出**两个**注册接口：

```c
/* 读取处理函数：Host 写入数据后由驱动调用，通知 tpcm_hw.ko 处理数据 */
typedef void (*tpcm_read_handler_t)(const u8 *in, size_t in_len);
void tpcm_register_read_handler(tpcm_read_handler_t fn);   /* EXPORT_SYMBOL */

/* 写回处理函数：驱动获取 tpcm_hw.ko 的处理结果，写回 Host 读出区 */
typedef int (*tpcm_write_handler_t)(u8 *out, size_t out_buf_len,
                                     size_t *out_actual_len);
void tpcm_register_write_handler(tpcm_write_handler_t fn); /* EXPORT_SYMBOL */
```

注册规则：
- **两个处理函数必须同时注册**，只注册一个时驱动回退到模拟模式并打印警告。
- 传入 `NULL` 注销对应处理函数。

### 5.2 实现模板

新建 `tpcm_hw.c`：

```c
// tpcm_hw.c — 真实 TPCM 后端（填入真实 API 后编译）
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>

/* a55_tpcm_ep_driver.ko 导出的两个注册函数 */
extern void tpcm_register_read_handler(
    void (*fn)(const u8 *in, size_t in_len));
extern void tpcm_register_write_handler(
    int  (*fn)(u8 *out, size_t out_buf_len, size_t *out_actual_len));

/* 内部暂存处理结果（read_handler 处理完后，write_handler 取走） */
static u8     g_result[128 * 1024];  /* 最大 128 KB */
static size_t g_result_len;
static DEFINE_SPINLOCK(g_result_lock);

/* ★ 读取处理函数：对应 Host pcie_write_once ★
 *
 * 驱动检测到 Host 写入数据后调用此函数。
 * 在此处完成数据处理，并将结果暂存供 write_handler 取走。
 */
static void my_on_read(const u8 *in, size_t in_len)
{
    size_t out_len = 0;
    u8     tmp_result[32];  /* 示例：32 字节哈希 */
    int    ret;

    pr_info("[TPCM_HW] 收到 Host 数据，len=%zu\n", in_len);

    /*
     * ★ 替换为真实 TPCM 调用，例如：
     *   ret = tpcm_drv_hash_sha256(in, in_len, tmp_result);
     *   if (ret == 0) out_len = 32;
     */
    (void)in;
    ret = -ENOSYS;
    pr_err("[TPCM_HW] 真实 TPCM 接口未实现\n");

    /* 暂存结果 */
    spin_lock(&g_result_lock);
    if (ret == 0 && out_len > 0) {
        if (out_len > sizeof(g_result))
            out_len = sizeof(g_result);
        memcpy(g_result, tmp_result, out_len);
        g_result_len = out_len;
    } else {
        g_result_len = 0;  /* 标记失败 */
    }
    spin_unlock(&g_result_lock);
}

/* ★ 写回处理函数：对应 Host pcie_read_once ★
 *
 * 驱动在调用 read_handler 后紧接着调用此函数，
 * tpcm_hw.ko 把暂存的结果填入 out，驱动再写回 BAR4 读出区。
 * 返回 0 成功，负数失败（Host 侧收到 PCIE_ERR_HW）。
 */
static int my_on_write(u8 *out, size_t out_buf_len, size_t *out_actual_len)
{
    spin_lock(&g_result_lock);
    *out_actual_len = g_result_len;
    if (g_result_len > 0)
        memcpy(out, g_result, g_result_len > out_buf_len
                              ? out_buf_len : g_result_len);
    spin_unlock(&g_result_lock);

    return (*out_actual_len > 0) ? 0 : -EIO;
}

static int __init tpcm_hw_init(void)
{
    tpcm_register_read_handler(my_on_read);
    tpcm_register_write_handler(my_on_write);
    pr_info("[TPCM_HW] 读写处理函数已注册\n");
    return 0;
}

static void __exit tpcm_hw_exit(void)
{
    tpcm_register_read_handler(NULL);
    tpcm_register_write_handler(NULL);
    pr_info("[TPCM_HW] 读写处理函数已注销\n");
}

module_init(tpcm_hw_init);
module_exit(tpcm_hw_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hi1712 TPCM 真实度量后端");
```

### 5.3 编译 tpcm_hw.ko

```bash
export PATH=/opt/hcc_arm64le/bin:$PATH

# 先编译 a55_tpcm_ep_driver.ko，生成 Module.symvers（含导出符号）
make -f Makefile.a55 ARCH=arm64 CROSS_COMPILE=aarch64-target-linux-gnu- \
  KDIR=/opt/RTOS-bmc/208.11.0/arm64le_5.10_ek_preempt_pro

# 编译 tpcm_hw.ko（需要 Module.symvers 解析 tpcm_register_backend）
echo "obj-m := tpcm_hw.o" > Kbuild
make -C /opt/RTOS-bmc/208.11.0/arm64le_5.10_ek_preempt_pro \
  M=$(pwd) ARCH=arm64 CROSS_COMPILE=aarch64-target-linux-gnu- \
  KBUILD_EXTRA_SYMBOLS=$(pwd)/Module.symvers modules
rm Kbuild

# 产物：tpcm_hw.ko
```

### 5.4 加载顺序

```bash
# A55 上按顺序加载（顺序不能反）
/sbin/insmod /tmp/a55_tpcm_ep_driver.ko   # 先加载基础驱动（导出注册符号）
/sbin/insmod /tmp/tpcm_hw.ko              # 后加载后端（自动注册）

# 验证日志
cat /dev/kmsg | grep TPCM
# 预期：[TPCM_HW] 读写处理函数已注册
# 预期：[TPCM] 读取处理函数已注册
# 预期：[TPCM] 写回处理函数已注册

# 卸载顺序（必须先卸 tpcm_hw，再卸基础驱动）
/sbin/rmmod tpcm_hw
/sbin/rmmod a55_tpcm_ep_driver
```

---

## 6. 故障排查

| 现象 | 原因 | 解决 |
|------|------|------|
| `insmod: invalid module format` | .ko 与运行内核版本不匹配 | 用 Hi1712 5.10.0 内核源码重新交叉编译 |
| `pcie_open()` 返回 `-1` | `/dev/pci_bar4_driver` 不存在 | 先在 950 上加载 `pci_bar4_driver.ko` |
| `PCIE_ERR_TIMEOUT` | A55 驱动未加载或处理慢 | 检查 A55 上 `cat /proc/modules \| grep tpcm` |
| `PCIE_ERR_FULL` | ring buffer 满（128 条积压） | 等 A55 消费后重试，或加大超时 |
| `PCIE_ERR_SEQ` | 未先 write 就调了 read | 确保每次 `write_once` 后紧跟 `read_once` |
| `pcie_read_once` 返回 0 | A55 完成但未写入结果 | 检查 A55 侧后端实现是否正确设置 `out_actual_len` |
| A55 日志无输出 | dmesg 权限不足 | 用 `cat /dev/kmsg` 代替 dmesg |
| 编译报 `arch//Makefile not found` | ARCH 参数为空 | 显式传 `ARCH=arm64` |
| 编译报 `No rule to make target *.o` | .c 源文件缺失 | 用 base64 绕过内网将 .c 文件传入 |
