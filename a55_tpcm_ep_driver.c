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
 * 度量模拟
 * ========================================================= */

/**
 * tpcm_simulate_hash() — 模拟 SHA-256 度量
 *
 * 真实部署需替换为：
 *   1. DMA 从 host_phys_addr 搬运数据到本地缓冲
 *   2. 调用 TCM 硬件 SHA-256 引擎
 *   3. 等待硬件完成（轮询状态寄存器或等待硬件中断）
 */
static void tpcm_simulate_hash(struct tcm_measure_cmd __iomem *cmd)
{
    __u32 cmd_id;
    __u8  hash_buf[32];
    int   i;

    cmd_id = readl(&cmd->cmd_id);

    pr_info("[TPCM] 开始度量: cmd_id=0x%08x, host_phys=0x%016llx, len=%u\n",
            cmd_id,
            (unsigned long long)readq(&cmd->host_phys_addr),
            (unsigned int)readw(&cmd->payload_len));

    /* 标记为处理中 */
    writeb(TCM_STATUS_PROCESSING, &cmd->status);

    /* 模拟 TCM 硬件度量耗时（2ms） */
    msleep(2);

    /* 生成模拟哈希：用 cmd_id 各字节异或填充（真实场景替换为硬件结果） */
    for (i = 0; i < 32; i++)
        hash_buf[i] = ((__u8)(cmd_id >> ((i % 4) * 8))) ^ ((__u8)i);

    /* 将哈希结果写回共享内存 */
    memcpy_toio(cmd->hash_result32, hash_buf, 32);

    pr_info("[TPCM] 度量完成: cmd_id=0x%08x, hash[0..3]=%02x%02x%02x%02x\n",
            cmd_id, hash_buf[0], hash_buf[1], hash_buf[2], hash_buf[3]);
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

        /* 执行度量 */
        tpcm_simulate_hash(cmd);

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
