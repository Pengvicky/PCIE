/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcm_pcie_protocol.h — Host/A55 双侧共享通信协议定义
 *
 * 项目：基于 PCIe DMA 的 TPCM 高速动态度量
 * 架构：鲲鹏920 (Root Complex) <──PCIe BAR4──> Hi1712 A55 (Endpoint)
 *
 * 本头文件被 Host 侧驱动和 A55 侧驱动共同包含，
 * 任何修改必须保证两侧同步更新，否则会导致内存布局错位。
 */

#ifndef _TCM_PCIE_PROTOCOL_H
#define _TCM_PCIE_PROTOCOL_H

#include <linux/types.h>

/* =========================================================
 * 硬件参数常量
 * ========================================================= */

/* A55 侧 32MB 共享内存物理基址（Inbound ATU 路由目标） */
#define A55_SHARED_PHYS_BASE        0x8cd00000UL

/* 共享内存总大小：32MB */
#define A55_SHARED_SIZE             (32 * 1024 * 1024)

/* 920 敲击 A55 触发的硬件门铃中断号 */
#define A55_DOORBELL_IRQ            45

/* A55 敲击 920 发送 MSI 中断的触发寄存器物理地址 */
#define A55_MSI_TRIGGER_REG_PHYS    0x1A000040UL

/* =========================================================
 * Ring Buffer 参数
 * ========================================================= */

/* 指令队列深度，必须是 2 的幂，方便取模用位与 */
#define TCM_RING_DEPTH              128

/* 取模掩码 */
#define TCM_RING_MASK               (TCM_RING_DEPTH - 1)

/* Cache Line 大小（ARM64 / x86_64 均为 64 字节） */
#define TCM_CACHE_LINE_SIZE         64

/* =========================================================
 * 度量指令状态码
 * ========================================================= */

#define TCM_STATUS_PENDING          0x00  /* 指令已投递，等待 A55 处理 */
#define TCM_STATUS_PROCESSING       0x01  /* A55 正在执行度量 */
#define TCM_STATUS_DONE             0x02  /* 度量完成，hash_result 有效 */
#define TCM_STATUS_ERROR            0xFF  /* 度量失败 */

/* =========================================================
 * 核心数据结构
 * ========================================================= */

/**
 * struct tcm_measure_cmd — 单条度量指令
 *
 * @cmd_id:         流水号，由 Host 侧单调递增，用于匹配请求与响应
 * @status:         指令状态，见 TCM_STATUS_* 宏
 * @payload_len:    待度量数据的字节长度
 * @reserved:       对齐填充，保留供后续扩展
 * @host_phys_addr: Host 侧待度量数据的物理地址（A55 通过 DMA 读取）
 * @hash_result32:  A55 计算完成后回填的 32 字节哈希结果（SHA-256）
 *
 * 整体大小：4+1+1+2+8+32 = 48 字节，不足 64 字节 Cache Line，
 * 由编译器自然对齐；Ring Buffer 数组本身按 Cache Line 对齐。
 */
struct tcm_measure_cmd {
    __u32   cmd_id;                 /* 指令流水号 */
    __u8    status;                 /* 状态码 */
    __u8    payload_len_hi;         /* payload_len 高 8 位（大端兼容） */
    __u16   payload_len;            /* 待度量数据长度（字节，低 16 位） */
    __u64   host_phys_addr;         /* Host 侧数据物理地址 */
    __u8    hash_result32[32];      /* SHA-256 哈希结果（A55 回填） */
    __u8    _pad[12];               /* 填充至 64 字节，独占一条 Cache Line */
} __attribute__((packed, aligned(TCM_CACHE_LINE_SIZE)));

/* 编译期断言：确保结构体恰好 64 字节 */
#define TCM_MEASURE_CMD_SIZE_CHECK \
    _Static_assert(sizeof(struct tcm_measure_cmd) == 64, \
        "tcm_measure_cmd must be exactly 64 bytes (one cache line)")

/**
 * struct pcie_ring_buffer — 无锁环形缓冲区控制块
 *
 * 生产者（Host）写 head，消费者（A55）写 tail。
 * head 和 tail 各自独占一条 64 字节 Cache Line，
 * 防止伪共享（False Sharing）导致的性能劣化。
 *
 * 内存布局（位于共享内存起始处）：
 *   [0x000] head        (64 字节 Cache Line)
 *   [0x040] tail        (64 字节 Cache Line)
 *   [0x080] cmds[128]   (128 × 64 = 8192 字节)
 *   [0x2080] ...        剩余空间供 DMA 数据区使用
 */
struct pcie_ring_buffer {
    /* --- 生产者写，消费者读 --- */
    union {
        volatile __u32  head;           /* 下一个可写槽位索引（Host 维护） */
        __u8            _head_pad[TCM_CACHE_LINE_SIZE];
    } __attribute__((aligned(TCM_CACHE_LINE_SIZE)));

    /* --- 消费者写，生产者读 --- */
    union {
        volatile __u32  tail;           /* 下一个待消费槽位索引（A55 维护） */
        __u8            _tail_pad[TCM_CACHE_LINE_SIZE];
    } __attribute__((aligned(TCM_CACHE_LINE_SIZE)));

    /* --- 指令队列 --- */
    struct tcm_measure_cmd cmds[TCM_RING_DEPTH]
        __attribute__((aligned(TCM_CACHE_LINE_SIZE)));

} __attribute__((aligned(TCM_CACHE_LINE_SIZE)));

/* =========================================================
 * 辅助内联函数
 * ========================================================= */

/**
 * tcm_ring_used() — 返回当前队列中待消费的指令数
 */
static inline __u32 tcm_ring_used(const struct pcie_ring_buffer *rb)
{
    return (rb->head - rb->tail) & (TCM_RING_DEPTH * 2 - 1);
}

/**
 * tcm_ring_empty() — 队列是否为空
 */
static inline int tcm_ring_empty(const struct pcie_ring_buffer *rb)
{
    return rb->head == rb->tail;
}

/**
 * tcm_ring_full() — 队列是否已满
 */
static inline int tcm_ring_full(const struct pcie_ring_buffer *rb)
{
    return tcm_ring_used(rb) >= TCM_RING_DEPTH;
}

#endif /* _TCM_PCIE_PROTOCOL_H */
