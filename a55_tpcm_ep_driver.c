// SPDX-License-Identifier: GPL-2.0
/*
 * a55_tpcm_ep_driver.c — Hi1712 A55 侧 PCIe Endpoint TPCM 度量驱动
 *
 * 项目：基于 PCIe 共享内存的 TPCM 高速动态度量
 * 运行侧：BMC Hi1712 A55 核心（PCIe Endpoint，运行 Linux）
 * 对端：鲲鹏 920/950 服务器（PCIe Root Complex）
 *
 * 通信方式：纯共享内存轮询（无中断）
 *   ┌──────────────────────────────────────────────────────┐
 *   │  Host (950)                    A55 (Hi1712)          │
 *   │                                                      │
 *   │  1. 写指令到 cmds[head]         轮询线程检测 head≠tail│
 *   │  2. status = PENDING           读指令，执行度量       │
 *   │  3. smp_wmb()                  写 hash_result        │
 *   │  4. 推进 head                  smp_wmb()             │
 *   │  5. 轮询 status==DONE ◀──────  status = DONE         │
 *   │  6. 读 hash_result             推进 tail             │
 *   └──────────────────────────────────────────────────────┘
 *
 * 内存屏障策略（ARM64 弱内存序）：
 *   - A55 读 head 前：smp_rmb()，确保看到 Host 最新写入的 cmds[]
 *   - A55 写 hash_result 后：smp_wmb()，确保结果对 Host 可见后再写 DONE
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/spinlock.h>

#include "tcm_pcie_protocol.h"

/* =========================================================
 * 模块元信息
 * ========================================================= */
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("TPCM Team");
MODULE_DESCRIPTION("Hi1712 A55 PCIe Endpoint TPCM 度量驱动（共享内存轮询模式）");
MODULE_VERSION("2.0.0");

/* =========================================================
 * 模块参数
 * ========================================================= */

/* 共享内存物理基址，可在 insmod 时覆盖 */
static unsigned long shared_phys_base = A55_SHARED_PHYS_BASE;
module_param(shared_phys_base, ulong, 0444);
MODULE_PARM_DESC(shared_phys_base,
    "A55 shared memory physical base address (default: " __stringify(A55_SHARED_PHYS_BASE) ")");

/* 轮询间隔（微秒），运行时可通过 /sys/module/a55_tpcm_ep_driver/parameters/ 调整 */
static unsigned int poll_interval_us = TPCM_POLL_INTERVAL_US;
module_param(poll_interval_us, uint, 0644);
MODULE_PARM_DESC(poll_interval_us,
    "Ring buffer poll interval in microseconds (default: " __stringify(TPCM_POLL_INTERVAL_US) ")");

/* =========================================================
 * 驱动私有上下文
 * ========================================================= */

/**
 * struct a55_tpcm_dev — 驱动全局上下文
 *
 * @shared_base:  32MB 共享内存的内核虚拟地址（ioremap_wc 映射）
 * @ring:         指向共享内存起始处的 Ring Buffer 控制块
 * @poll_thread:  轮询内核线程（kthread）
 * @initialized:  初始化完成标志，防止线程在资源释放后访问
 */
struct a55_tpcm_dev {
    void __iomem            *shared_base;
    struct pcie_ring_buffer *ring;
    struct task_struct      *poll_thread;
    bool                     initialized;
};

/* 驱动全局单例 */
static struct a55_tpcm_dev *g_tpcm_dev;

/* =========================================================
 * 度量后端回调机制
 *
 * 原理（Linux EXPORT_SYMBOL 插件模式）：
 *
 *   本驱动维护一个函数指针 g_tpcm_backend，初始为 NULL。
 *   外部模块（tpcm_hw.ko）在自身 init() 里调用
 *   tpcm_register_backend() 注册真实度量函数；
 *   本驱动在处理每条指令时判断指针是否非 NULL：
 *     非 NULL → 调真实 TPCM（tpcm_hw.ko 提供）
 *     NULL    → 内置模拟（内联在 tpcm_do_measure）
 *
 * 这样 tpcm_hw.ko 可以独立编译、独立加载/卸载，
 * 本驱动无需任何修改即可切换真实/模拟两种后端。
 *
 * 回调原型：
 *   int fn(const u8 *in,  size_t in_len,
 *          u8  *out, size_t out_buf_len, size_t *out_actual_len)
 *   成功返回 0，并通过 out_actual_len 告知实际写入字节数；
 *   失败返回负数。
 * ========================================================= */

typedef int (*tpcm_backend_fn_t)(const u8 *in,   size_t in_len,
                                  u8       *out,  size_t out_buf_len,
                                  size_t   *out_actual_len);

static tpcm_backend_fn_t g_tpcm_backend;          /* 初始为 NULL，即模拟模式 */
static DEFINE_SPINLOCK(g_backend_lock);            /* 保护函数指针的并发读写 */

/**
 * tpcm_register_backend() — 注册 / 注销真实 TPCM 度量后端
 *
 * 传入非 NULL 函数指针：注册真实后端，后续度量调真实 TPCM。
 * 传入 NULL：注销后端，回退到内置模拟模式。
 *
 * EXPORT_SYMBOL 使该符号对其他内核模块可见，
 * tpcm_hw.ko 链接时可直接调用，无需修改本文件。
 */
void tpcm_register_backend(tpcm_backend_fn_t fn)
{
    unsigned long flags;
    spin_lock_irqsave(&g_backend_lock, flags);
    g_tpcm_backend = fn;
    spin_unlock_irqrestore(&g_backend_lock, flags);
    pr_info("[TPCM] 度量后端%s\n",
            fn ? "已注册（真实 TPCM 模式）" : "已注销（回退模拟模式）");
}
EXPORT_SYMBOL(tpcm_register_backend);

/* =========================================================
 * 内置模拟后端已内联至 tpcm_do_measure() 中，此处不再单独声明。
 * ========================================================= */

/* =========================================================
 * 统一度量入口：判断后端 → 调真实 TPCM 或模拟
 * ========================================================= */
static void tpcm_do_measure(struct a55_tpcm_dev *dev,
                            struct tcm_measure_cmd __iomem *cmd)
{
    tpcm_backend_fn_t backend;
    unsigned long     flags;
    u64   w_off, r_off;
    u16   in_len, out_buf_len;
    u8   *in_buf = NULL;
    u8   *out_buf = NULL;
    size_t out_actual = 0;
    int   ret;

    /* 原子读取后端指针 */
    spin_lock_irqsave(&g_backend_lock, flags);
    backend = g_tpcm_backend;
    spin_unlock_irqrestore(&g_backend_lock, flags);

    writeb(TCM_STATUS_PROCESSING, &cmd->status);

    w_off       = readq(&cmd->write_data_offset);
    r_off       = readq(&cmd->read_data_offset);
    in_len      = readw(&cmd->write_len);
    out_buf_len = readw(&cmd->read_buf_len);

    /* 参数合法性检查 */
    if (!in_len || in_len > PCIE_DMA_WRITE_SIZE ||
        !out_buf_len || out_buf_len > PCIE_DMA_READ_SIZE ||
        w_off + in_len      > A55_SHARED_SIZE ||
        r_off + out_buf_len > A55_SHARED_SIZE) {
        pr_err("[TPCM] 参数非法 w_off=0x%llx in=%u r_off=0x%llx out_buf=%u\n",
               (unsigned long long)w_off, in_len,
               (unsigned long long)r_off, out_buf_len);
        writeb(TCM_STATUS_ERROR, &cmd->status);
        return;
    }

    if (!backend) {
        /* ── 无真实后端：模拟路径 ──
         * 从写入区读数据，生成 32 字节模拟哈希写到读出区。
         */
        u32  cmd_id = readl(&cmd->cmd_id);
        u8   hash[32];
        int  i;

        pr_info("[TPCM] [模拟] cmd_id=0x%08x in_len=%u\n", cmd_id, in_len);
        msleep(2);
        for (i = 0; i < 32; i++)
            hash[i] = (u8)(cmd_id >> ((i % 4) * 8)) ^ (u8)i;

        memcpy_toio((char __iomem *)dev->shared_base + r_off, hash, 32);
        writel(32, &cmd->read_actual_len);

        pr_info("[TPCM] [模拟] 完成 cmd_id=0x%08x hash[0..3]=%02x%02x%02x%02x\n",
                cmd_id, hash[0], hash[1], hash[2], hash[3]);
        return;
    }

    /* ── 有真实后端：从 DMA 写入区拷贝输入，分配输出缓冲 ── */
    in_buf  = kmalloc(in_len,      GFP_KERNEL);
    out_buf = kmalloc(out_buf_len, GFP_KERNEL);
    if (!in_buf || !out_buf) {
        kfree(in_buf);
        kfree(out_buf);
        writeb(TCM_STATUS_ERROR, &cmd->status);
        return;
    }

    memcpy_fromio(in_buf, (char __iomem *)dev->shared_base + w_off, in_len);

    pr_info("[TPCM] 调用真实 TPCM: cmd_id=0x%08x in_len=%u out_buf=%u\n",
            readl(&cmd->cmd_id), in_len, out_buf_len);

    ret = backend(in_buf, in_len, out_buf, out_buf_len, &out_actual);
    kfree(in_buf);

    if (ret == 0 && out_actual > 0) {
        if (out_actual > out_buf_len)
            out_actual = out_buf_len;  /* 截断防溢出 */
        memcpy_toio((char __iomem *)dev->shared_base + r_off,
                    out_buf, out_actual);
        writel((u32)out_actual, &cmd->read_actual_len);
        pr_info("[TPCM] 真实度量完成: cmd_id=0x%08x out=%zu\n",
                readl(&cmd->cmd_id), out_actual);
    } else {
        pr_err("[TPCM] 真实度量失败: ret=%d\n", ret);
        writeb(TCM_STATUS_ERROR, &cmd->status);
    }
    kfree(out_buf);
}

/* =========================================================
 * Ring Buffer 消费逻辑
 * ========================================================= */

/**
 * tpcm_process_ring() — 消费 Ring Buffer 中的所有待处理指令
 *
 * 由轮询线程调用，每次调用处理当前所有可用指令（批处理）。
 *
 * 内存屏障：
 *   [读 head 前] smp_rmb()
 *     → 确保 Host 写入 cmds[] 的内容在本核读到新 head 之前已可见
 *   [写 hash_result 后] smp_wmb()
 *     → 确保哈希数据在 status=DONE 写入之前对 Host 完全可见
 */
static void tpcm_process_ring(struct a55_tpcm_dev *dev)
{
    struct pcie_ring_buffer *ring = dev->ring;
    __u32 head, tail, idx;
    int   processed = 0;

    tail = readl(&ring->tail);

    /*
     * [关键屏障 1] 读 head 前的读屏障
     * ARM64 弱内存序：防止 Load-Load 乱序，确保先看到 cmds[] 内容再看到新 head。
     */
    smp_rmb();

    head = readl(&ring->head);

    if (head == tail)
        return;  /* 队列为空，快速返回，避免打印日志刷屏 */

    pr_info("[TPCM] 检测到新指令: head=%u, tail=%u, 待处理=%u 条\n",
            head, tail, (head - tail) & TCM_RING_MASK);

    /* ---- 批量消费循环 ---- */
    while (tail != head) {
        struct tcm_measure_cmd __iomem *cmd;

        idx = tail & TCM_RING_MASK;
        cmd = &ring->cmds[idx];

        /* 状态校验：防止重复处理 */
        if (readb(&cmd->status) != TCM_STATUS_PENDING) {
            pr_warn("[TPCM] 槽位 %u 状态异常 (0x%02x)，跳过\n",
                    idx, readb(&cmd->status));
            tail++;
            continue;
        }

        /* 执行度量（有注册后端调真实 TPCM，否则模拟） */
        tpcm_do_measure(dev, cmd);

        /*
         * [关键屏障 2] 写 hash_result 后的写屏障
         * 确保哈希结果和 status=DONE 在 tail 更新之前对 Host 完全可见。
         * 防止 Host 看到 tail 推进后读到未完成的哈希数据。
         */
        smp_wmb();

        /* 回填最终状态 */
        writeb(TCM_STATUS_DONE, &cmd->status);

        tail++;
        processed++;

        pr_info("[TPCM] 槽位 %u 处理完成，tail 推进至 %u\n", idx, tail);
    }

    if (processed == 0)
        return;

    /*
     * 更新 tail 指针，通知 Host 侧本批处理完成。
     * writel 在 ARM64 上具有 release 语义，相当于内嵌 smp_wmb。
     */
    writel(tail, &ring->tail);

    pr_info("[TPCM] tail 已更新至 %u，本轮共处理 %d 条指令\n", tail, processed);
}

/* =========================================================
 * 轮询内核线程
 * ========================================================= */

/**
 * tpcm_poll_thread() — A55 侧 Ring Buffer 轮询主循环
 *
 * 替代原有的中断+工作队列机制，以固定间隔轮询共享内存中的 head 指针。
 * 优点：实现简单、无中断依赖、便于调试
 * 代价：CPU 占用约 0.02%（200us 轮询，2ms 度量，通常空载）
 *
 * poll_interval_us 默认 200us（约 5000 次/秒），空载时每次轮询
 * 仅读一个 u32 寄存器，CPU 开销极低。
 */
static int tpcm_poll_thread(void *data)
{
    struct a55_tpcm_dev *dev = data;

    pr_info("[TPCM] 轮询线程启动，间隔 %u us\n", poll_interval_us);

    while (!kthread_should_stop()) {
        if (likely(dev->initialized))
            tpcm_process_ring(dev);

        /* 轮询间隔：使用 usleep_range 允许内核在此点抢占，降低调度影响 */
        usleep_range(poll_interval_us, poll_interval_us * 2);
    }

    pr_info("[TPCM] 轮询线程已退出\n");
    return 0;
}

/* =========================================================
 * 模块初始化
 * ========================================================= */

/**
 * a55_tpcm_init() — 驱动入口
 *
 * 初始化顺序：
 *   1. 分配驱动上下文
 *   2. ioremap_wc 映射共享内存
 *   3. 清零 Ring Buffer 控制指针
 *   4. 启动轮询内核线程
 */
static int __init a55_tpcm_init(void)
{
    struct a55_tpcm_dev *dev;
    int ret;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动初始化开始（共享内存轮询模式）=====\n");
    pr_info("[TPCM] 共享内存: phys=0x%08lx, size=%u MB\n",
            (unsigned long)shared_phys_base,
            A55_SHARED_SIZE / (1024 * 1024));
    pr_info("[TPCM] 轮询间隔: %u us（约 %u 次/秒）\n",
            poll_interval_us, 1000000 / poll_interval_us);

    /* ---- Step 1: 分配驱动上下文 ---- */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        pr_err("[TPCM] 分配驱动上下文失败\n");
        return -ENOMEM;
    }
    g_tpcm_dev = dev;

    /* ---- Step 2: ioremap_wc 映射 32MB 共享内存 ---- */
    /*
     * 使用 ioremap_wc（Write-Combining）：
     *   - PCIe 共享内存不经过 CPU Cache，必须绕过 Cache 一致性协议
     *   - WC 模式允许多次写操作合并为一次 PCIe 事务，提升写吞吐
     *   - 读操作每次直接从设备侧取最新值
     */
    dev->shared_base = ioremap_wc(shared_phys_base, A55_SHARED_SIZE);
    if (!dev->shared_base) {
        pr_err("[TPCM] ioremap_wc 失败: phys=0x%08lx, size=%u MB\n",
               (unsigned long)shared_phys_base,
               A55_SHARED_SIZE / (1024 * 1024));
        ret = -ENOMEM;
        goto err_free_dev;
    }
    pr_info("[TPCM] 共享内存映射成功: phys=0x%08lx -> virt=%p\n",
            (unsigned long)shared_phys_base, dev->shared_base);

    /* Ring Buffer 控制块位于共享内存起始处 */
    dev->ring = (struct pcie_ring_buffer *)dev->shared_base;

    /* ---- Step 3: 清零 Ring Buffer 控制指针 ---- */
    /*
     * 确保 Host 侧加载驱动后看到干净的初始状态（head=0, tail=0）。
     * 只清零控制区（两个 u32），不清整个 32MB，避免长时间阻塞。
     */
    writel(0, &dev->ring->head);
    writel(0, &dev->ring->tail);
    smp_mb();  /* 全屏障：确保清零对 Host 侧可见后再继续 */
    pr_info("[TPCM] Ring Buffer 控制指针已清零\n");

    /* ---- Step 4: 启动轮询内核线程 ---- */
    dev->initialized = true;

    dev->poll_thread = kthread_run(tpcm_poll_thread, dev, "tpcm_poll");
    if (IS_ERR(dev->poll_thread)) {
        pr_err("[TPCM] 创建轮询线程失败: ret=%ld\n", PTR_ERR(dev->poll_thread));
        ret = PTR_ERR(dev->poll_thread);
        dev->poll_thread = NULL;
        goto err_unmap_shared;
    }
    pr_info("[TPCM] 轮询线程已启动: pid=%d\n", dev->poll_thread->pid);

    pr_info("[TPCM] ===== A55 TPCM EP 驱动初始化完成，等待 Host 侧指令 =====\n");
    return 0;

/* ---- 错误回滚路径 ---- */
err_unmap_shared:
    dev->initialized = false;
    iounmap(dev->shared_base);
    dev->shared_base = NULL;

err_free_dev:
    kfree(dev);
    g_tpcm_dev = NULL;
    return ret;
}

/* =========================================================
 * 模块卸载
 * ========================================================= */

/**
 * a55_tpcm_exit() — 驱动卸载
 *
 * 释放顺序（与申请顺序相反）：
 *   1. 标记 initialized=false，让线程感知退出信号
 *   2. kthread_stop 等待轮询线程退出
 *   3. 解除共享内存映射
 *   4. 释放上下文内存
 */
static void __exit a55_tpcm_exit(void)
{
    struct a55_tpcm_dev *dev = g_tpcm_dev;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动开始卸载 =====\n");

    if (!dev) {
        pr_warn("[TPCM] 驱动上下文为空，可能未成功初始化\n");
        return;
    }

    /* Step 1: 标记为未初始化，让线程在下次循环感知到并停止处理 */
    dev->initialized = false;
    smp_mb();

    /* Step 2: 停止轮询线程（阻塞等待线程退出） */
    if (dev->poll_thread) {
        kthread_stop(dev->poll_thread);
        dev->poll_thread = NULL;
        pr_info("[TPCM] 轮询线程已停止\n");
    }

    /* Step 3: 解除共享内存映射 */
    if (dev->shared_base) {
        iounmap(dev->shared_base);
        dev->shared_base = NULL;
        pr_info("[TPCM] 共享内存映射已释放\n");
    }

    /* Step 4: 释放驱动上下文 */
    kfree(dev);
    g_tpcm_dev = NULL;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动卸载完成 =====\n");
}

module_init(a55_tpcm_init);
module_exit(a55_tpcm_exit);
