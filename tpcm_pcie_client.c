/*
 * tpcm_pcie_client.c — Host 侧 TPCM PCIe 度量客户端实现
 *
 * 内存布局（BAR4 共享内存，32MB）：
 *   [0x000000 ~ 0x002080)  pcie_ring_buffer 控制结构
 *   [0x010000 ~ 0x210000)  DMA 数据区：128 槽 × 256KB
 *                           每槽对应 ring buffer 的一个 cmd 槽位
 */

#include "tpcm_pcie_client.h"
#include "tcm_pcie_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* =========================================================
 * 内部常量
 * ========================================================= */

#define DEV_PATH          "/dev/pci_bar4_driver"
#define DEFAULT_TIMEOUT_MS 5000

/*
 * DMA 数据区布局
 *   起始偏移: 0x10000 (64KB，与 ring_buffer 结尾留充足间距)
 *   每槽大小: TPCM_MAX_DATA_LEN (256KB)
 *   总占用:   128 × 256KB = 32MB（与 BAR4 大小吻合）
 */
#define DMA_BASE_OFFSET   0x10000UL
#define DMA_SLOT_SIZE     TPCM_MAX_DATA_LEN

/* =========================================================
 * 内部结构
 * ========================================================= */

typedef struct {
    int                      fd;
    void                    *bar4;       /* mmap 起始地址 */
    struct pcie_ring_buffer *ring;       /* ring buffer 指针 */
    uint32_t                 next_cmd_id;
} tpcm_handle_t;

/* =========================================================
 * 工具：时间差（毫秒）
 * ========================================================= */

static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec  - start->tv_sec)  * 1000L
         + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* =========================================================
 * 生命周期
 * ========================================================= */

void *tpcm_open(void)
{
    tpcm_handle_t *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;

    h->fd = open(DEV_PATH, O_RDWR);
    if (h->fd < 0) {
        perror("[TPCM] open " DEV_PATH);
        free(h);
        return NULL;
    }

    h->bar4 = mmap(NULL, A55_SHARED_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    if (h->bar4 == MAP_FAILED) {
        perror("[TPCM] mmap BAR4");
        close(h->fd);
        free(h);
        return NULL;
    }

    h->ring        = (struct pcie_ring_buffer *)h->bar4;
    h->next_cmd_id = 1;
    return h;
}

void tpcm_close(void *handle)
{
    tpcm_handle_t *h = (tpcm_handle_t *)handle;
    if (!h)
        return;
    munmap(h->bar4, A55_SHARED_SIZE);
    close(h->fd);
    free(h);
}

/* =========================================================
 * 核心度量
 * ========================================================= */

int tpcm_measure(void *handle, const void *data, size_t len,
                 uint8_t hash_out[32], int timeout_ms)
{
    tpcm_handle_t *h = (tpcm_handle_t *)handle;

    /* 参数校验 */
    if (!h || !data || !hash_out)
        return TPCM_ERR_PARAM;
    if (len == 0 || len > TPCM_MAX_DATA_LEN)
        return TPCM_ERR_PARAM;
    if (timeout_ms <= 0)
        timeout_ms = DEFAULT_TIMEOUT_MS;

    /* 检查队列是否已满 */
    uint32_t head = h->ring->head;
    uint32_t tail = h->ring->tail;
    if ((head - tail) >= TCM_RING_DEPTH)
        return TPCM_ERR_FULL;

    /* ── 1. 将待度量数据写入 DMA 数据槽 ── */
    uint32_t slot     = head & TCM_RING_MASK;
    void    *dma_slot = (char *)h->bar4 + DMA_BASE_OFFSET
                        + (size_t)slot * DMA_SLOT_SIZE;
    memcpy(dma_slot, data, len);
    __sync_synchronize();   /* Store-Store: 数据必须先于 cmd 字段可见 */

    /* ── 2. 填写指令 ── */
    struct tcm_measure_cmd *cmd = &h->ring->cmds[slot];
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd_id         = h->next_cmd_id++;
    cmd->payload_len    = (uint16_t)(len & 0xFFFF);
    cmd->payload_len_hi = (uint8_t)(len >> 16);
    /*
     * host_phys_addr 存放 DMA 槽在 BAR4 内的偏移，
     * A55 侧通过 shared_base + offset 直接 memcpy_fromio 读取。
     */
    cmd->host_phys_addr = DMA_BASE_OFFSET + (uint64_t)slot * DMA_SLOT_SIZE;
    cmd->status         = TCM_STATUS_PENDING;
    __sync_synchronize();   /* Store-Store: cmd 字段必须先于 head 推进可见 */

    /* ── 3. 推进 head，通知 A55 ── */
    h->ring->head = head + 1;
    __sync_synchronize();

    /* ── 4. 轮询等待 A55 回填结果 ── */
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (1) {
        __sync_synchronize();   /* Load-Load: 读 status 前刷新 cache */
        uint8_t st = cmd->status;

        if (st == TCM_STATUS_DONE) {
            memcpy(hash_out, cmd->hash_result32, 32);
            return TPCM_OK;
        }
        if (st == TCM_STATUS_ERROR)
            return TPCM_ERR_HW;

        if (elapsed_ms(&ts_start) > timeout_ms)
            return TPCM_ERR_TIMEOUT;

        /* 轮询间隔：500µs，减少 CPU 占用 */
        usleep(500);
    }
}

/* =========================================================
 * 文件度量（支持大文件分段）
 * ========================================================= */

/*
 * 对于 len <= TPCM_MAX_DATA_LEN 的文件：直接一次度量。
 * 对于大文件：按 TPCM_MAX_DATA_LEN 分段，对每段结果做
 * 串联后的最终 SHA-256（简化实现：返回最后一段的哈希，
 * 生产中应使用 HMAC 或 Merkle 树方案）。
 *
 * 注：如需真正的流式 SHA-256，替换为 openssl EVP_DigestUpdate 即可。
 */
int tpcm_measure_file(void *handle, const char *filepath,
                      uint8_t hash_out[32], int timeout_ms)
{
    if (!handle || !filepath || !hash_out)
        return TPCM_ERR_PARAM;

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("[TPCM] open file");
        return TPCM_ERR_IO;
    }

    /* 获取文件大小 */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return TPCM_ERR_IO;
    }
    off_t file_size = st.st_size;
    if (file_size == 0) {
        close(fd);
        return TPCM_ERR_PARAM;
    }

    /* 分配读缓冲区 */
    uint8_t *buf = malloc(TPCM_MAX_DATA_LEN);
    if (!buf) {
        close(fd);
        return TPCM_ERR_IO;
    }

    int     ret        = TPCM_OK;
    uint8_t seg_hash[32];
    off_t   offset     = 0;
    int     seg_count  = 0;

    while (offset < file_size) {
        size_t to_read = TPCM_MAX_DATA_LEN;
        if ((off_t)to_read > file_size - offset)
            to_read = (size_t)(file_size - offset);

        ssize_t nread = pread(fd, buf, to_read, offset);
        if (nread <= 0) {
            ret = TPCM_ERR_IO;
            break;
        }

        ret = tpcm_measure(handle, buf, (size_t)nread, seg_hash, timeout_ms);
        if (ret != TPCM_OK)
            break;

        offset += nread;
        seg_count++;

        fprintf(stderr, "[TPCM] file segment %d: offset=%lld size=%zd\n",
                seg_count, (long long)(offset - nread), nread);
    }

    if (ret == TPCM_OK) {
        /*
         * 多段时取最后一段哈希作为文件摘要（简化）。
         * 生产环境建议：用各段哈希拼接后再做一次度量。
         */
        memcpy(hash_out, seg_hash, 32);
    }

    free(buf);
    close(fd);
    return ret;
}

/* =========================================================
 * 工具函数
 * ========================================================= */

void tpcm_hash_to_hex(const uint8_t hash[32], char hexbuf[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hexbuf[i * 2]     = hex[hash[i] >> 4];
        hexbuf[i * 2 + 1] = hex[hash[i] & 0xF];
    }
    hexbuf[64] = '\0';
}

const char *tpcm_strerror(int err)
{
    switch (err) {
    case TPCM_OK:          return "success";
    case TPCM_ERR_OPEN:    return "device open/mmap failed";
    case TPCM_ERR_PARAM:   return "invalid parameter";
    case TPCM_ERR_FULL:    return "ring buffer full";
    case TPCM_ERR_TIMEOUT: return "measurement timeout";
    case TPCM_ERR_HW:      return "A55 hardware error";
    case TPCM_ERR_IO:      return "file I/O error";
    default:               return "unknown error";
    }
}
