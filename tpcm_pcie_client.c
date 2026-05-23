// SPDX-License-Identifier: GPL-2.0
/*
 * tpcm_pcie_client.c — Host 侧 PCIe 双向通信客户端实现
 *
 * 对外暴露两个核心原语：
 *   pcie_write_once() — 写数据到 A55（push cmd + 推进 head）
 *   pcie_read_once()  — 读 A55 返回的结果（轮询 status + 拷贝数据）
 *
 * BAR4 内存布局（与 a55_tpcm_ep_driver.c / tcm_pcie_protocol.h 一致）：
 *
 *   [0x000000]  pcie_ring_buffer 控制块（head/tail + cmds[]）
 *   [0x010000]  DMA 区起始（slot 0）
 *     每槽 256 KB，前 128 KB = 写入区（Host → A55），
 *                  后 128 KB = 读出区（A55 → Host）
 *
 * 调用顺序限制（每个 fd 独立）：
 *   pcie_write_once() 先于 pcie_read_once()
 *   write 后会记录 slot 索引，read 从同一 slot 读结果
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>

#include "tcm_pcie_protocol.h"
#include "tpcm_pcie_client.h"

/* =========================================================
 * 内部常量
 * ========================================================= */

/** BAR4 总映射大小（32 MB） */
#define BAR4_MMAP_SIZE      (32 * 1024 * 1024UL)

/** PCIe 设备节点 */
#define PCIE_DEV_PATH       "/dev/pci_bar4_driver"

/** 轮询间隔（微秒） */
#define POLL_INTERVAL_US    200

/** 最大并发通道数（fd 数量上限） */
#define MAX_CHANNELS        16

/* =========================================================
 * 通道上下文
 * ========================================================= */

/**
 * struct pcie_channel — 每个 fd 对应的通道上下文
 *
 * @dev_fd:     /dev/pci_bar4_driver 的文件描述符（-1 = 未使用）
 * @bar4:       BAR4 mmap 虚拟地址
 * @ring:       指向 BAR4 起始的 Ring Buffer 控制块
 * @cmd_id_seq: 本通道累计发出的指令计数（单调递增）
 * @last_slot:  最近一次 write_once 使用的 slot 索引
 * @has_pending:上次 write_once 的结果是否尚未被 read_once 消费
 * @timeout_ms: read_once 超时（毫秒）
 */
struct pcie_channel {
    int                      dev_fd;
    volatile void           *bar4;
    struct pcie_ring_buffer *ring;
    uint32_t                 cmd_id_seq;
    uint32_t                 last_slot;
    int                      has_pending;
    int                      timeout_ms;
};

/* 全局通道表（按 fd 索引，fd 即下标 + 偏移） */
static struct pcie_channel g_channels[MAX_CHANNELS];
static int g_initialized;

/* =========================================================
 * 内部工具
 * ========================================================= */

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/** 将 fd（pcie_open 返回值）转换为内部 channel 指针；非法返回 NULL */
static struct pcie_channel *fd_to_ch(int fd)
{
    if (fd < 0 || fd >= MAX_CHANNELS)
        return NULL;
    if (g_channels[fd].dev_fd < 0)
        return NULL;
    return &g_channels[fd];
}

/** 计算 slot N 的写入区在 BAR4 内的字节偏移 */
static inline uint64_t write_area_offset(uint32_t slot)
{
    return PCIE_DMA_BASE_OFFSET + (uint64_t)slot * PCIE_DMA_SLOT_SIZE;
}

/** 计算 slot N 的读出区在 BAR4 内的字节偏移 */
static inline uint64_t read_area_offset(uint32_t slot)
{
    return write_area_offset(slot) + PCIE_DMA_WRITE_SIZE;
}

/* =========================================================
 * 公共接口实现
 * ========================================================= */

/* 初始化全局通道表（首次调用时） */
static void init_table(void)
{
    int i;
    if (g_initialized)
        return;
    for (i = 0; i < MAX_CHANNELS; i++)
        g_channels[i].dev_fd = -1;
    g_initialized = 1;
}

/**
 * pcie_open() — 打开 PCIe 通道
 *
 * 返回值：成功返回 [0, MAX_CHANNELS) 范围内整数作为 fd，失败返回 -1。
 *
 * 实现：
 *   1. 在 g_channels[] 中找一个空槽
 *   2. open(PCIE_DEV_PATH)
 *   3. mmap BAR4_MMAP_SIZE 字节
 */
int pcie_open(void)
{
    int i, dev_fd;
    void *bar4;
    struct pcie_channel *ch = NULL;

    init_table();

    /* 找空槽 */
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channels[i].dev_fd < 0) {
            ch = &g_channels[i];
            break;
        }
    }
    if (!ch) {
        errno = EMFILE;
        return -1;
    }

    dev_fd = open(PCIE_DEV_PATH, O_RDWR);
    if (dev_fd < 0)
        return -1;

    bar4 = mmap(NULL, BAR4_MMAP_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, dev_fd, 0);
    if (bar4 == MAP_FAILED) {
        close(dev_fd);
        return -1;
    }

    ch->dev_fd      = dev_fd;
    ch->bar4        = bar4;
    ch->ring        = (struct pcie_ring_buffer *)bar4;
    ch->cmd_id_seq  = 0;
    ch->last_slot   = 0;
    ch->has_pending = 0;
    ch->timeout_ms  = PCIE_DEFAULT_TIMEOUT_MS;

    return i;  /* fd = 通道表下标 */
}

/**
 * pcie_close() — 释放通道资源
 */
void pcie_close(int fd)
{
    struct pcie_channel *ch = fd_to_ch(fd);
    if (!ch)
        return;
    munmap((void *)ch->bar4, BAR4_MMAP_SIZE);
    close(ch->dev_fd);
    ch->dev_fd      = -1;
    ch->bar4        = NULL;
    ch->has_pending = 0;
}

/**
 * pcie_set_timeout() — 设置读超时
 */
void pcie_set_timeout(int fd, int timeout_ms)
{
    struct pcie_channel *ch = fd_to_ch(fd);
    if (!ch)
        return;
    ch->timeout_ms = (timeout_ms <= 0) ? PCIE_DEFAULT_TIMEOUT_MS : timeout_ms;
}

/**
 * pcie_write_once() — 将数据写入 BAR4 DMA 区，推送指令到 ring buffer
 *
 * 实现步骤：
 *   1. 参数校验
 *   2. 检查 ring buffer 是否有空位
 *   3. 计算本次使用的 slot 和 cmd 位置
 *   4. 将 buf 复制到 BAR4 写入区（mmap 地址）
 *   5. 填写 cmd 字段（write_len, write_data_offset, read_data_offset...）
 *   6. 写屏障后推进 head（令 A55 看到新指令）
 *   7. 记录 last_slot，标记 has_pending
 */
ssize_t pcie_write_once(int fd, const void *buf, size_t buf_len)
{
    struct pcie_channel     *ch;
    struct pcie_ring_buffer *ring;
    struct tcm_measure_cmd  *cmd;
    uint32_t                 head, tail, slot;
    volatile uint8_t        *write_area;
    uint64_t                 w_off, r_off;

    if (!buf || buf_len == 0 || buf_len > PCIE_CLIENT_MAX_WRITE)
        return PCIE_ERR_PARAM;

    ch = fd_to_ch(fd);
    if (!ch)
        return PCIE_ERR_PARAM;

    ring = ch->ring;

    /* 检查队列是否已满（head - tail >= DEPTH） */
    __sync_synchronize();
    head = *(volatile uint32_t *)&ring->head;
    tail = *(volatile uint32_t *)&ring->tail;
    if ((uint32_t)(head - tail) >= TCM_RING_DEPTH)
        return PCIE_ERR_FULL;

    /* 本次使用的 slot 和 cmd 位置 */
    slot = head & TCM_RING_MASK;
    cmd  = &ring->cmds[slot];

    /* 计算 DMA 偏移（相对于 BAR4 起始） */
    w_off = write_area_offset(slot);
    r_off = read_area_offset(slot);

    /* 写入区：将数据复制到 BAR4 */
    write_area = (volatile uint8_t *)ch->bar4 + w_off;
    memcpy((void *)write_area, buf, buf_len);

    /* 填写指令字段（写屏障前完成所有数据写入） */
    cmd->cmd_id            = ++ch->cmd_id_seq;
    cmd->status            = TCM_STATUS_PENDING;
    cmd->write_len         = (uint16_t)buf_len;
    cmd->read_buf_len      = (uint16_t)PCIE_CLIENT_MAX_READ;
    cmd->read_actual_len   = 0;
    cmd->write_data_offset = w_off;
    cmd->read_data_offset  = r_off;

    /*
     * 写屏障：确保上方所有写操作（DMA 数据 + cmd 字段）
     * 在推进 head 之前对 A55 可见。
     */
    __sync_synchronize();

    /* 推进 head，让 A55 感知到新指令 */
    *(volatile uint32_t *)&ring->head = head + 1;
    __sync_synchronize();

    /* 记录上下文，供 pcie_read_once 使用 */
    ch->last_slot   = slot;
    ch->has_pending = 1;

    return (ssize_t)buf_len;
}

/**
 * pcie_read_once() — 等待 A55 完成上一次写入的指令，读取结果
 *
 * 实现步骤：
 *   1. 参数校验 + 检查 has_pending
 *   2. 轮询 cmd->status，直到 DONE / ERROR 或超时
 *   3. 若 DONE：将 BAR4 读出区的数据复制到 buf
 *   4. 清除 has_pending
 */
ssize_t pcie_read_once(int fd, void *buf, size_t buf_len)
{
    struct pcie_channel     *ch;
    struct pcie_ring_buffer *ring;
    struct tcm_measure_cmd  *cmd;
    uint64_t                 deadline;
    uint8_t                  status;
    uint32_t                 actual_len;
    size_t                   copy_len;
    volatile uint8_t        *read_area;

    if (!buf || buf_len == 0 || buf_len > PCIE_CLIENT_MAX_READ)
        return PCIE_ERR_PARAM;

    ch = fd_to_ch(fd);
    if (!ch)
        return PCIE_ERR_PARAM;

    if (!ch->has_pending)
        return PCIE_ERR_SEQ;

    ring     = ch->ring;
    cmd      = &ring->cmds[ch->last_slot];
    deadline = now_ms() + (uint64_t)ch->timeout_ms;

    /* 轮询等待 A55 完成 */
    while (1) {
        __sync_synchronize();
        status = *(volatile uint8_t *)&cmd->status;

        if (status == TCM_STATUS_DONE || status == TCM_STATUS_ERROR)
            break;

        if (now_ms() >= deadline)
            return PCIE_ERR_TIMEOUT;

        usleep(POLL_INTERVAL_US);
    }

    /* 清除 pending 标记（无论成功与否） */
    ch->has_pending = 0;

    if (status == TCM_STATUS_ERROR)
        return PCIE_ERR_HW;

    /* 读取 A55 实际写入的字节数 */
    __sync_synchronize();
    actual_len = *(volatile uint32_t *)&cmd->read_actual_len;

    if (actual_len == 0)
        return 0;

    /* 从 BAR4 读出区拷贝结果（取 actual_len 与 buf_len 的最小值） */
    copy_len  = (actual_len < buf_len) ? actual_len : buf_len;
    read_area = (volatile uint8_t *)ch->bar4 + cmd->read_data_offset;
    memcpy(buf, (const void *)read_area, copy_len);

    return (ssize_t)copy_len;
}

/* =========================================================
 * 工具函数
 * ========================================================= */

const char *pcie_strerror(int err)
{
    switch (err) {
    case PCIE_OK:          return "success";
    case PCIE_ERR_OPEN:    return "device open/mmap failed";
    case PCIE_ERR_PARAM:   return "invalid parameter";
    case PCIE_ERR_FULL:    return "ring buffer full, retry later";
    case PCIE_ERR_TIMEOUT: return "timeout waiting for A55 result";
    case PCIE_ERR_HW:      return "A55 hardware processing error";
    case PCIE_ERR_SEQ:     return "pcie_read_once called before pcie_write_once";
    default:               return "unknown error";
    }
}
