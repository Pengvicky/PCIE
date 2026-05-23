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
/* 实测确认值： A55 本地 DDR 0x8bc00000 */
#define A55_SHARED_PHYS_BASE        0x8bc00000UL

/* 共享内存总大小：32MB */
#define A55_SHARED_SIZE             (32 * 1024 * 1024)

/*
 * 通信模式：纯共享内存轮询，无中断
 *   - Host 写入指令并更新 head 指针
 *   - A55 内核线程以 TPCM_POLL_INTERVAL_US 间隔轮询 head
 *   - Host 轮询 status 字段感知完成，无需 MSI 回调
 */
#define TPCM_POLL_INTERVAL_US       200   /* A55 轮询间隔（微秒，可通过模块参数覆盖） */

/* =========================================================
 * Ring Buffer 参数
 * ========================================================= */

/* 指令队列深度，必须是 2 的幂，方便取模用位与 */
#define TCM_RING_DEPTH              128

/* 取模掩码 */
#define TCM_RING_MASK               (TCM_RING_DEPTH - 1)

/* Cache Line 大小（ARM64 / x86_64 均为 64 字节） */
#define TCM_CACHE_LINE_SIZE         64

/* DMA 数据区布局（BAR4 内）
 *   具体定义位于 tpcm_pcie_client.c / a55_tpcm_ep_driver.c，
 *   此处定义公共常量供两侧共用。
 *
 *   内存布局：
 *     [0x000000 ~ 0x002080)  pcie_ring_buffer 控制结构
 *     [0x010000 ~ ...]       DMA 数据区，每槽 256 KB，前 128 KB 写入，后 128 KB 读出
 */
#define PCIE_DMA_BASE_OFFSET        0x10000UL         /* DMA 区起始偏移 */
#define PCIE_DMA_SLOT_SIZE          (256 * 1024UL)    /* 每槽总大小 */
#define PCIE_DMA_WRITE_SIZE         (PCIE_DMA_SLOT_SIZE / 2)  /* 每槽写入区 128 KB */
#define PCIE_DMA_READ_SIZE          (PCIE_DMA_SLOT_SIZE / 2)  /* 每槽读出区 128 KB */

/* 单次最大写入 / 读出数据长度 */
#define PCIE_MAX_WRITE_LEN          PCIE_DMA_WRITE_SIZE
#define PCIE_MAX_READ_LEN           PCIE_DMA_READ_SIZE

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
 * struct tcm_measure_cmd — 单条度量指令（双向动态长度版）
 *
 * Host 处填写字段：
 *   cmd_id / status / write_len / read_buf_len /
 *   write_data_offset / read_data_offset
 *
 * A55 回填字段：
 *   read_actual_len（实际写入结果的字节数）
 *   status → TCM_STATUS_DONE / TCM_STATUS_ERROR
 *
 * DMA 布局（每个槽位对应 PCIE_DMA_SLOT_SIZE）：
 *   write_data_offset = PCIE_DMA_BASE_OFFSET + slot * PCIE_DMA_SLOT_SIZE
 *   read_data_offset  = write_data_offset + PCIE_DMA_WRITE_SIZE
 *
 * 整体大小：恰好 64 字节，独占一条 Cache Line。
 */
struct tcm_measure_cmd {
    __u32   cmd_id;                 /* 指令流水号（Host 单调递增） */
    __u8    status;                 /* 状态码，见 TCM_STATUS_* */
    __u8    _pad0[3];               /* 对齐填充 */
    __u16   write_len;              /* Host 写入数据的字节数 */
    __u16   read_buf_len;           /* Host 为读出结果分配的缓冲大小 */
    __u32   read_actual_len;        /* A55 实际写入结果的字节数（A55 回填）*/
    __u64   write_data_offset;      /* BAR4 内写入数据起始偏移 */
    __u64   read_data_offset;       /* BAR4 内读出结果起始偏移 */
    __u8    _pad1[28];              /* 填充至 64 字节 */
} __attribute__((packed, aligned(TCM_CACHE_LINE_SIZE)));

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
