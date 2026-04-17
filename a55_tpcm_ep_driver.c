// SPDX-License-Identifier: GPL-2.0
/*
 * a55_tpcm_ep_driver.c — Hi1712 A55 侧 PCIe Endpoint TPCM 度量驱动
 *
 * 项目：基于 PCIe DMA 的 TPCM 高速动态度量
 * 运行侧：BMC Hi1712 A55 核心（PCIe Endpoint，运行 Linux）
 * 对端：鲲鹏 920 服务器（PCIe Root Complex）
 *
 * 工作流程：
 *   1. 初始化时 ioremap_wc 映射 32MB 共享内存 + MSI 触发寄存器
 *   2. 申请门铃中断 (IRQ 45)，920 写 BAR4 后触发此中断通知 A55
 *   3. ISR 仅唤醒工作队列，避免在中断上下文执行耗时操作
 *   4. 工作队列消费 Ring Buffer：读指令 → 度量 → 回填 hash → 推进 tail
 *   5. 批处理完成后写 MSI 触发寄存器，反向通知 920
 *
 * 内存屏障策略（ARM64 弱内存序）：
 *   - 读 head 前：smp_rmb()，确保看到 Host 最新写入的指令内容
 *   - 写 hash_result 后：smp_wmb()，确保结果落盘再更新 tail
 *   - 写 tail 后：smp_mb()，确保 tail 更新对 Host 可见后再触发 MSI
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
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
MODULE_AUTHOR("TPCM Team <tpcm@example.com>");
MODULE_DESCRIPTION("Hi1712 A55 PCIe Endpoint TPCM 动态度量驱动");
MODULE_VERSION("1.0.0");

/* =========================================================
 * 驱动私有上下文结构体
 * ========================================================= */

/**
 * struct a55_tpcm_dev — 驱动全局上下文，贯穿整个模块生命周期
 *
 * @shared_base:    32MB 共享内存的内核虚拟地址（ioremap_wc 映射）
 * @msi_trigger:    MSI 触发寄存器的内核虚拟地址
 * @ring:           指向共享内存起始处的 Ring Buffer 控制块
 * @work:           延迟工作项，由 ISR 调度，在进程上下文执行消费逻辑
 * @doorbell_irq:   已申请的门铃中断号
 * @initialized:    初始化完成标志，防止工作队列在资源释放后访问
 */
struct a55_tpcm_dev {
    void __iomem                *shared_base;
    void __iomem                *msi_trigger;
    struct pcie_ring_buffer     *ring;
    struct work_struct           work;
    int                          doorbell_irq;
    bool                         initialized;
};

/* 驱动全局单例（本驱动只管理一个 EP 设备） */
static struct a55_tpcm_dev *g_tpcm_dev;

/* =========================================================
 * 前向声明
 * ========================================================= */
static void tpcm_process_work(struct work_struct *work);
static irqreturn_t tpcm_doorbell_isr(int irq, void *dev_id);

/* =========================================================
 * 内部辅助函数
 * ========================================================= */

/**
 * tpcm_simulate_hash() — 模拟对指定物理地址数据的 SHA-256 度量
 *
 * 真实场景中应通过 DMA 读取 host_phys_addr 处的数据并调用硬件 TCM 引擎。
 * 此处用 msleep + 固定填充模拟耗时加密计算，便于功能验证。
 *
 * @cmd:    待处理的度量指令（直接操作共享内存中的槽位）
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

    /* 标记为处理中，让 Host 侧可以感知进度 */
    writeb(TCM_STATUS_PROCESSING, &cmd->status);

    /*
     * 模拟 TCM 硬件度量耗时（实际应替换为：
     *   1. 配置 DMA 从 host_phys_addr 搬运数据到本地缓冲
     *   2. 调用 TCM 硬件 SHA-256 引擎
     *   3. 等待硬件完成中断或轮询状态寄存器
     */
    msleep(2);

    /* 生成模拟哈希：用 cmd_id 的各字节填充，真实场景替换为硬件结果 */
    for (i = 0; i < 32; i++)
        hash_buf[i] = ((__u8)(cmd_id >> ((i % 4) * 8))) ^ ((__u8)i);

    /* 将哈希结果写回共享内存（memcpy_toio 保证 MMIO 写序） */
    memcpy_toio(cmd->hash_result32, hash_buf, 32);

    pr_info("[TPCM] 度量完成: cmd_id=0x%08x, hash[0..3]=%02x%02x%02x%02x\n",
            cmd_id,
            hash_buf[0], hash_buf[1], hash_buf[2], hash_buf[3]);
}

/**
 * tpcm_trigger_msi() — 向 920 发送 MSI 中断，通知批处理完成
 *
 * 写入 MSI 触发寄存器前必须确保所有写操作（hash_result、status、tail）
 * 已经对 Host 侧可见，因此调用前需保证 smp_mb() 已执行。
 */
static void tpcm_trigger_msi(struct a55_tpcm_dev *dev)
{
    /*
     * 全内存屏障：确保 tail 指针的更新在 MSI 触发之前
     * 对 Host 侧完全可见，防止 Host 读到旧 tail 后误判队列状态。
     */
    smp_mb();

    writel(1, dev->msi_trigger);

    pr_info("[TPCM] MSI 已触发，通知 Host 侧批处理完成\n");
}

/* =========================================================
 * 中断服务程序（上半部）
 * ========================================================= */

/**
 * tpcm_doorbell_isr() — 门铃中断 ISR（上半部）
 *
 * 920 向 BAR4 写入数据后触发此中断。
 * ISR 中严禁执行任何可能睡眠或耗时的操作，
 * 仅通过 schedule_work 将消费逻辑推迟到工作队列（进程上下文）执行。
 */
static irqreturn_t tpcm_doorbell_isr(int irq, void *dev_id)
{
    struct a55_tpcm_dev *dev = (struct a55_tpcm_dev *)dev_id;

    if (unlikely(!dev || !dev->initialized)) {
        pr_warn("[TPCM] ISR: 设备未初始化，忽略中断 irq=%d\n", irq);
        return IRQ_NONE;
    }

    pr_info("[TPCM] 收到门铃中断 irq=%d，调度工作队列\n", irq);

    /*
     * schedule_work 是原子安全的，可在中断上下文调用。
     * 若工作项已在队列中（上次未处理完），此调用为空操作，不会重复入队。
     */
    schedule_work(&dev->work);

    return IRQ_HANDLED;
}

/* =========================================================
 * 工作队列任务（下半部）
 * ========================================================= */

/**
 * tpcm_process_work() — Ring Buffer 消费者主逻辑（工作队列上下文）
 *
 * 内存屏障使用说明（ARM64 弱内存序下的正确性保证）：
 *
 *   [读取 head 前] smp_rmb()
 *     → 确保在读取 head 值之前，Host 写入 cmds[] 的数据已对本核可见。
 *       ARM64 下 Store-Load 乱序可能导致先看到新 head 但 cmds[] 仍是旧值。
 *
 *   [写入 hash_result 后] smp_wmb()
 *     → 确保 hash_result32 和 status 的写入在 tail 更新之前对所有核可见。
 *       防止 Host 看到 tail 推进后读取到未完成的哈希结果。
 *
 *   [写入 tail 后，触发 MSI 前] smp_mb()（在 tpcm_trigger_msi 中执行）
 *     → 全屏障，确保 tail 写入对 Host 完全可见后再发送 MSI。
 */
static void tpcm_process_work(struct work_struct *work)
{
    struct a55_tpcm_dev     *dev;
    struct pcie_ring_buffer *ring;
    __u32                    head, tail, idx;
    int                      processed = 0;

    dev = container_of(work, struct a55_tpcm_dev, work);

    if (unlikely(!dev->initialized)) {
        pr_warn("[TPCM] 工作队列：设备已销毁，退出\n");
        return;
    }

    ring = dev->ring;

    /*
     * 读取 tail（本侧维护，无需屏障）
     * 注意：tail 是 A55 自己写的，直接读即可。
     */
    tail = readl(&ring->tail);

    /*
     * [关键屏障 1] 读取 head 前插入读屏障
     *
     * 保证：在读取 head 之前，Host 侧对 cmds[] 的所有写操作
     * 已经对本核可见。ARM64 的 Load-Load 乱序要求此屏障。
     */
    smp_rmb();

    head = readl(&ring->head);

    if (head == tail) {
        pr_info("[TPCM] 工作队列：Ring Buffer 为空，无需处理\n");
        return;
    }

    pr_info("[TPCM] 工作队列启动：head=%u, tail=%u, 待处理=%u 条\n",
            head, tail, (head - tail) & TCM_RING_MASK);

    /* ---- 批量消费循环 ---- */
    while (tail != head) {
        struct tcm_measure_cmd __iomem *cmd;

        idx = tail & TCM_RING_MASK;
        cmd = &ring->cmds[idx];

        /* 安全检查：指令状态必须是 PENDING，否则跳过（防止重复处理） */
        if (readb(&cmd->status) != TCM_STATUS_PENDING) {
            pr_warn("[TPCM] 槽位 %u 状态异常 (0x%02x)，跳过\n",
                    idx, readb(&cmd->status));
            tail++;
            continue;
        }

        /* 执行度量（模拟 TCM 硬件计算） */
        tpcm_simulate_hash(cmd);

        /*
         * [关键屏障 2] 写入 hash_result 后插入写屏障
         *
         * 保证：hash_result32 和 status=DONE 的写入
         * 在 tail 指针更新之前对所有观察者（Host 核）可见。
         * 防止 Host 看到 tail 推进后读到未完成的哈希数据。
         */
        smp_wmb();

        /* 回填最终状态：DONE */
        writeb(TCM_STATUS_DONE, &cmd->status);

        /* 推进消费指针 */
        tail++;
        processed++;

        pr_info("[TPCM] 槽位 %u 处理完成，tail 推进至 %u\n", idx, tail);
    }

    if (processed == 0) {
        pr_info("[TPCM] 工作队列：本轮无有效指令\n");
        return;
    }

    /*
     * 将更新后的 tail 写回共享内存。
     * writel 本身在 ARM64 上具有 release 语义（相当于 smp_wmb + store），
     * 但为了代码可移植性和明确性，我们在 tpcm_trigger_msi 中再加 smp_mb()。
     */
    writel(tail, &ring->tail);

    pr_info("[TPCM] tail 已更新至 %u，共处理 %d 条指令，准备触发 MSI\n",
            tail, processed);

    /* 反向触发 MSI，通知 Host 侧批处理完成 */
    tpcm_trigger_msi(dev);
}

/* =========================================================
 * 模块初始化
 * ========================================================= */

/**
 * a55_tpcm_init() — 驱动入口，完成所有资源申请和初始化
 *
 * 错误处理采用 goto 标签展开，确保任何失败路径都能正确释放已申请资源，
 * 防止内存泄漏和资源悬挂。
 */
static int __init a55_tpcm_init(void)
{
    struct a55_tpcm_dev *dev;
    int ret;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动初始化开始 =====\n");
    pr_info("[TPCM] 共享内存物理基址: 0x%08lx, 大小: %u MB\n",
            (unsigned long)A55_SHARED_PHYS_BASE,
            A55_SHARED_SIZE / (1024 * 1024));
    pr_info("[TPCM] 门铃中断号: %d, MSI 触发寄存器: 0x%08lx\n",
            A55_DOORBELL_IRQ, (unsigned long)A55_MSI_TRIGGER_REG_PHYS);

    /* ---- Step 1: 分配驱动上下文 ---- */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        pr_err("[TPCM] 分配驱动上下文失败（内存不足）\n");
        return -ENOMEM;
    }
    dev->doorbell_irq = -1;
    g_tpcm_dev = dev;

    /* ---- Step 2: ioremap_wc 映射 32MB 共享内存 ---- */
    /*
     * 使用 ioremap_wc（Write-Combining）而非 ioremap：
     *   - 跨 PCIe 的内存访问不经过 CPU Cache，必须绕过 Cache 一致性协议
     *   - WC 模式允许 CPU 将多次写操作合并为一次 PCIe 事务，提升吞吐量
     *   - 对于读操作，WC 保证每次都从设备侧读取最新值
     */
    dev->shared_base = ioremap_wc(A55_SHARED_PHYS_BASE, A55_SHARED_SIZE);
    if (!dev->shared_base) {
        pr_err("[TPCM] ioremap_wc 共享内存失败: phys=0x%08lx, size=%u\n",
               (unsigned long)A55_SHARED_PHYS_BASE, A55_SHARED_SIZE);
        ret = -ENOMEM;
        goto err_free_dev;
    }
    pr_info("[TPCM] 共享内存映射成功: phys=0x%08lx -> virt=%p\n",
            (unsigned long)A55_SHARED_PHYS_BASE, dev->shared_base);

    /* Ring Buffer 控制块位于共享内存起始处 */
    dev->ring = (struct pcie_ring_buffer *)dev->shared_base;

    /* ---- Step 3: ioremap 映射 MSI 触发寄存器 ---- */
    /*
     * MSI 触发寄存器是设备寄存器，使用普通 ioremap（Strongly-Ordered）
     * 确保写操作立即生效，不被 CPU 重排或合并。
     */
    dev->msi_trigger = ioremap(A55_MSI_TRIGGER_REG_PHYS, sizeof(__u32));
    if (!dev->msi_trigger) {
        pr_err("[TPCM] ioremap MSI 触发寄存器失败: phys=0x%08lx\n",
               (unsigned long)A55_MSI_TRIGGER_REG_PHYS);
        ret = -ENOMEM;
        goto err_unmap_shared;
    }
    pr_info("[TPCM] MSI 触发寄存器映射成功: phys=0x%08lx -> virt=%p\n",
            (unsigned long)A55_MSI_TRIGGER_REG_PHYS, dev->msi_trigger);

    /* ---- Step 4: 初始化工作队列任务 ---- */
    INIT_WORK(&dev->work, tpcm_process_work);
    pr_info("[TPCM] 工作队列初始化完成\n");

    /* ---- Step 5: 申请门铃中断 ---- */
    /*
     * IRQF_SHARED：允许与其他驱动共享此中断线（视硬件实际情况决定是否保留）
     * 若硬件保证此 IRQ 专用，可改为 0 以获得更好的性能。
     */
    ret = request_irq(A55_DOORBELL_IRQ,
                      tpcm_doorbell_isr,
                      IRQF_SHARED,
                      "tpcm_doorbell",
                      dev);
    if (ret) {
        pr_err("[TPCM] 申请门铃中断失败: irq=%d, ret=%d\n",
               A55_DOORBELL_IRQ, ret);
        goto err_unmap_msi;
    }
    dev->doorbell_irq = A55_DOORBELL_IRQ;
    pr_info("[TPCM] 门铃中断申请成功: irq=%d\n", A55_DOORBELL_IRQ);

    /* ---- Step 6: 初始化 Ring Buffer（清零控制指针） ---- */
    /*
     * 仅清零 head/tail 控制区，不清零整个 32MB（避免长时间阻塞）。
     * 实际部署时，Host 侧驱动负责在通信开始前协商初始化顺序。
     */
    writel(0, &dev->ring->head);
    writel(0, &dev->ring->tail);
    smp_mb(); /* 确保初始化写入对 Host 可见 */
    pr_info("[TPCM] Ring Buffer 控制指针已清零\n");

    /* ---- 初始化完成 ---- */
    dev->initialized = true;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动初始化完成，等待 Host 侧指令 =====\n");
    return 0;

    /* ---- 错误回滚路径 ---- */
err_unmap_msi:
    iounmap(dev->msi_trigger);
    dev->msi_trigger = NULL;

err_unmap_shared:
    iounmap(dev->shared_base);
    dev->shared_base = NULL;

err_free_dev:
    kfree(dev);
    g_tpcm_dev = NULL;

    pr_err("[TPCM] 驱动初始化失败，已回滚所有资源，ret=%d\n", ret);
    return ret;
}

/* =========================================================
 * 模块卸载
 * ========================================================= */

/**
 * a55_tpcm_exit() — 驱动卸载，按申请的逆序释放所有资源
 *
 * 释放顺序：
 *   1. 标记 initialized=false，阻止新的工作队列任务访问资源
 *   2. 释放中断，确保 ISR 不再调度新任务
 *   3. flush_work 等待已在队列中的任务执行完毕
 *   4. 解除内存映射
 *   5. 释放上下文内存
 */
static void __exit a55_tpcm_exit(void)
{
    struct a55_tpcm_dev *dev = g_tpcm_dev;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动开始卸载 =====\n");

    if (!dev) {
        pr_warn("[TPCM] 驱动上下文为空，可能未成功初始化\n");
        return;
    }

    /* Step 1: 标记为未初始化，阻止 ISR 调度新任务 */
    dev->initialized = false;
    smp_mb(); /* 确保标志位写入对所有核可见 */

    /* Step 2: 释放门铃中断 */
    if (dev->doorbell_irq >= 0) {
        free_irq(dev->doorbell_irq, dev);
        pr_info("[TPCM] 门铃中断已释放: irq=%d\n", dev->doorbell_irq);
    }

    /* Step 3: 等待工作队列中已调度的任务执行完毕，防止 use-after-free */
    flush_work(&dev->work);
    pr_info("[TPCM] 工作队列已 flush 完毕\n");

    /* Step 4: 解除 MSI 触发寄存器映射 */
    if (dev->msi_trigger) {
        iounmap(dev->msi_trigger);
        dev->msi_trigger = NULL;
        pr_info("[TPCM] MSI 触发寄存器映射已释放\n");
    }

    /* Step 5: 解除共享内存映射 */
    if (dev->shared_base) {
        iounmap(dev->shared_base);
        dev->shared_base = NULL;
        pr_info("[TPCM] 共享内存映射已释放\n");
    }

    /* Step 6: 释放驱动上下文 */
    kfree(dev);
    g_tpcm_dev = NULL;

    pr_info("[TPCM] ===== A55 TPCM EP 驱动卸载完成 =====\n");
}

module_init(a55_tpcm_init);
module_exit(a55_tpcm_exit);
