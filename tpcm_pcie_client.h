/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tpcm_pcie_client.h — Host 侧 PCIe 双向通信客户端接口
 *
 * 提供两个正交的原语：
 *   pcie_write_once() — 向 A55 写入一段数据（异步，不等结果）
 *   pcie_read_once()  — 读取 A55 处理后的结果（阻塞，带超时）
 *
 * 典型用法（顺序调用）：
 *
 *   int fd = pcie_open();
 *
 *   // 写入待处理数据
 *   ssize_t w = pcie_write_once(fd, data, data_len);
 *
 *   // 读取 A55 返回的结果（可变长度）
 *   uint8_t result[PCIE_MAX_READ_LEN];
 *   ssize_t r = pcie_read_once(fd, result, sizeof(result));
 *
 *   pcie_close(fd);
 *
 * 线程安全：同一 fd 不可多线程并发 write/read；
 *           多线程请各自调用 pcie_open() 获取独立 fd。
 */

#ifndef _TPCM_PCIE_CLIENT_H
#define _TPCM_PCIE_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>  /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * 常量（与 tcm_pcie_protocol.h 中的 PCIE_DMA_* 对应）
 * ========================================================= */

/** 单次最大写入字节数（128 KB） */
#define PCIE_CLIENT_MAX_WRITE  (128 * 1024)

/** 单次最大读出字节数（128 KB） */
#define PCIE_CLIENT_MAX_READ   (128 * 1024)

/** 默认超时（毫秒） */
#define PCIE_DEFAULT_TIMEOUT_MS  5000

/* =========================================================
 * 错误码（负数，可直接用 pcie_strerror() 转字符串）
 * ========================================================= */
#define PCIE_OK            0
#define PCIE_ERR_OPEN     -1   /* 设备打开 / mmap 失败 */
#define PCIE_ERR_PARAM    -2   /* 参数非法（NULL / 超长） */
#define PCIE_ERR_FULL     -3   /* ring buffer 已满，稍后重试 */
#define PCIE_ERR_TIMEOUT  -4   /* 等待 A55 结果超时 */
#define PCIE_ERR_HW       -5   /* A55 侧处理返回错误 */
#define PCIE_ERR_SEQ      -6   /* 调用顺序错误（未先写就读） */

/* =========================================================
 * 生命周期
 * ========================================================= */

/**
 * pcie_open() — 打开 PCIe 通道
 *
 * 内部执行：open("/dev/pci_bar4_driver") + mmap BAR4
 *
 * 成功返回非负整数 fd（作为后续调用的通道标识）。
 * 失败返回 -1，errno 已设置。
 *
 * 每个 fd 维护独立的发送/接收上下文，多线程请各自 open。
 */
int pcie_open(void);

/**
 * pcie_close() — 关闭 PCIe 通道，释放资源
 *
 * @fd: pcie_open() 返回的通道标识
 */
void pcie_close(int fd);

/* =========================================================
 * 核心收发接口
 * ========================================================= */

/**
 * pcie_write_once() — 向 A55 写入一段数据（立即返回，不等结果）
 *
 * @fd:      pcie_open() 返回的通道标识
 * @buf:     待发送数据首地址
 * @buf_len: 数据字节数，范围 [1, PCIE_CLIENT_MAX_WRITE]
 *
 * 成功返回实际写入字节数（== buf_len）。
 * 失败返回负数错误码：
 *   PCIE_ERR_PARAM   — buf 为 NULL 或 buf_len 超限
 *   PCIE_ERR_FULL    — 当前 ring buffer 已满
 *
 * 注意：本函数只负责把数据送入 ring buffer 并通知 A55，
 *       不保证 A55 已处理完毕。需调用 pcie_read_once() 获取结果。
 */
ssize_t pcie_write_once(int fd, const void *buf, size_t buf_len);

/**
 * pcie_read_once() — 读取 A55 处理后的结果
 *
 * @fd:      pcie_open() 返回的通道标识（须已成功调用 pcie_write_once）
 * @buf:     接收缓冲区首地址
 * @buf_len: 缓冲区大小（字节），范围 [1, PCIE_CLIENT_MAX_READ]
 *
 * 阻塞等待 A55 完成，使用 pcie_set_timeout() 设置的超时（默认 5000ms）。
 *
 * 成功返回 A55 实际写入的字节数（可能小于 buf_len）。
 * 失败返回负数错误码：
 *   PCIE_ERR_SEQ     — 未先调用 pcie_write_once
 *   PCIE_ERR_TIMEOUT — 超时
 *   PCIE_ERR_HW      — A55 侧返回错误
 */
ssize_t pcie_read_once(int fd, void *buf, size_t buf_len);

/* =========================================================
 * 可选配置
 * ========================================================= */

/**
 * pcie_set_timeout() — 设置该通道的读超时（毫秒）
 *
 * @fd:         通道标识
 * @timeout_ms: 毫秒，0 恢复默认值 PCIE_DEFAULT_TIMEOUT_MS
 */
void pcie_set_timeout(int fd, int timeout_ms);

/* =========================================================
 * 工具函数
 * ========================================================= */

/**
 * pcie_strerror() — 将错误码转为可读字符串
 */
const char *pcie_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* _TPCM_PCIE_CLIENT_H */
