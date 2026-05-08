/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tpcm_pcie_client.h — Host 侧 TPCM PCIe 度量客户端接口
 *
 * 业务方只需包含本头文件 + 链接 libtpcm_pcie.so，无需了解
 * PCIe BAR4、ring buffer 等任何底层细节。
 *
 * 用法示例:
 *   void *h = tpcm_open();
 *   uint8_t hash[32];
 *   tpcm_measure(h, data, len, hash, 5000);
 *   tpcm_close(h);
 */

#ifndef _TPCM_PCIE_CLIENT_H
#define _TPCM_PCIE_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * 返回值
 * ========================================================= */
#define TPCM_OK             0
#define TPCM_ERR_OPEN      -1   /* 设备打开 / mmap 失败 */
#define TPCM_ERR_PARAM     -2   /* 参数非法（NULL / 超长）*/
#define TPCM_ERR_FULL      -3   /* ring buffer 已满 */
#define TPCM_ERR_TIMEOUT   -4   /* 等待度量结果超时 */
#define TPCM_ERR_HW        -5   /* A55 侧度量硬件错误 */
#define TPCM_ERR_IO        -6   /* 文件读取失败 */

/* 单次最大度量数据长度：256 KB */
#define TPCM_MAX_DATA_LEN  (256 * 1024)

/* =========================================================
 * 生命周期
 * ========================================================= */

/**
 * tpcm_open() — 初始化，打开 /dev/pci_bar4_driver 并 mmap
 *
 * 返回不透明 handle，失败返回 NULL（errno 已设置）。
 * 线程安全：每个线程应持有独立 handle。
 */
void *tpcm_open(void);

/**
 * tpcm_close() — 释放 handle（munmap + close fd）
 */
void tpcm_close(void *handle);

/* =========================================================
 * 度量接口
 * ========================================================= */

/**
 * tpcm_measure() — 将一段内存数据发给 A55 TPCM 做 SHA-256 度量
 *
 * @handle:     tpcm_open() 返回的句柄
 * @data:       待度量数据起始地址
 * @len:        数据字节数，不超过 TPCM_MAX_DATA_LEN
 * @hash_out:   输出缓冲区，调用方分配，至少 32 字节
 * @timeout_ms: 等待超时（毫秒），0 使用默认值 5000 ms
 *
 * 成功返回 TPCM_OK，并将 32 字节 SHA-256 写入 hash_out。
 */
int tpcm_measure(void       *handle,
                 const void *data,
                 size_t      len,
                 uint8_t     hash_out[32],
                 int         timeout_ms);

/**
 * tpcm_measure_file() — 对指定路径的文件做整体度量
 *
 * @handle:     tpcm_open() 返回的句柄
 * @filepath:   文件绝对路径
 * @hash_out:   输出缓冲区，至少 32 字节
 * @timeout_ms: 等待超时（毫秒），0 使用默认值 5000 ms
 *
 * 大文件（> TPCM_MAX_DATA_LEN）会分段哈希后合并。
 */
int tpcm_measure_file(void       *handle,
                      const char *filepath,
                      uint8_t     hash_out[32],
                      int         timeout_ms);

/* =========================================================
 * 工具函数
 * ========================================================= */

/**
 * tpcm_hash_to_hex() — 将 32 字节哈希转为 64 字符十六进制串
 *
 * @hash:   32 字节输入
 * @hexbuf: 输出缓冲区，至少 65 字节（含 '\0'）
 */
void tpcm_hash_to_hex(const uint8_t hash[32], char hexbuf[65]);

/**
 * tpcm_strerror() — 将错误码转为可读字符串
 */
const char *tpcm_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* _TPCM_PCIE_CLIENT_H */
