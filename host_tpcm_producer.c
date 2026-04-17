/*
 * host_tpcm_producer.c — Host 侧 Ring Buffer 生产者测试程序
 *
 * 运行侧：鲲鹏 920（PCIe Root Complex），用户态程序
 * 依赖：pci_bar4_driver.ko 已加载，/dev/pci_bar4_driver 可访问
 *
 * 功能：
 *   1. 按 tcm_pcie_protocol.h 定义的格式向 BAR4 写入度量指令
 *   2. 推进 Ring Buffer head 指针
 *   3. 通过 BAR4 偏移写门铃寄存器触发 A55 中断（需硬件支持）
 *   4. 轮询等待 A55 回填 hash_result，打印结果
 *
 * 编译：
 *   gcc -O2 -Wall -o host_tpcm_producer host_tpcm_producer.c
 *
 * 运行：
 *   ./host_tpcm_producer [指令条数]   # 默认发送 4 条
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

/* =========================================================
 * 用户态协议定义（与 tcm_pcie_protocol.h 保持完全一致）
 * ========================================================= */

#define TCM_RING_DEPTH          128
#define TCM_RING_MASK           (TCM_RING_DEPTH - 1)
#define TCM_CACHE_LINE_SIZE     64

/* 指令状态码 */
#define TCM_STATUS_PENDING      0x00
#define TCM_STATUS_PROCESSING   0x01
#define TCM_STATUS_DONE         0x02
#define TCM_STATUS_ERROR        0xFF

/*
 * Ring Buffer 内存布局（BAR4 起始偏移）：
 *   [0x000] head  (64 字节)
 *   [0x040] tail  (64 字节)
 *   [0x080] cmds[128]  (128 × 64 = 8192 字节)
 *   [0x2080] 门铃寄存器偏移（见 DOORBELL_BAR4_OFFSET）
 */
#define RING_HEAD_OFFSET        0x000
#define RING_TAIL_OFFSET        0x040
#define RING_CMDS_OFFSET        0x080
#define CMD_SLOT_SIZE           64      /* 每条指令占 64 字节（一个 Cache Line） */

/*
 * 门铃寄存器在 BAR4 内的偏移。
 * Host 写这个偏移 → 触发 A55 IRQ 45。
 * 实际偏移需查 Hi1712 硬件手册，此处为占位值，联调前必须确认。
 */
#define DOORBELL_BAR4_OFFSET    0x10000

/* 单条指令在 BAR4 内的字节偏移 */
#define CMD_OFFSET(idx)  (RING_CMDS_OFFSET + ((idx) & TCM_RING_MASK) * CMD_SLOT_SIZE)

/* 指令字段在槽位内的偏移（与结构体字段顺序一致） */
#define FIELD_CMD_ID            0   /* u32, offset 0  */
#define FIELD_STATUS            4   /* u8,  offset 4  */
#define FIELD_PAYLOAD_LEN_HI    5   /* u8,  offset 5  */
#define FIELD_PAYLOAD_LEN       6   /* u16, offset 6  */
#define FIELD_HOST_PHYS_ADDR    8   /* u64, offset 8  */
#define FIELD_HASH_RESULT       16  /* u8[32], offset 16 */

/* =========================================================
 * ioctl 接口（与 pci_bar4_driver.c 保持一致）
 * ========================================================= */

#define PCI_BAR4_READ   _IOWR('k', 1, struct bar4_access_args)
#define PCI_BAR4_WRITE  _IOWR('k', 2, struct bar4_access_args)

struct bar4_access_args {
    uint64_t offset;
    uint64_t length;
    void    *buffer;
};

/* =========================================================
 * BAR4 读写封装
 * ========================================================= */

static int g_fd = -1;

/**
 * bar4_write() — 向 BAR4 指定偏移写入数据
 */
static int bar4_write(uint64_t offset, const void *buf, uint64_t len)
{
    struct bar4_access_args args;
    args.offset = offset;
    args.length = len;
    args.buffer = (void *)buf;  /* ioctl 内部会 copy_from_user */

    if (ioctl(g_fd, PCI_BAR4_WRITE, &args) < 0) {
        fprintf(stderr, "[HOST] bar4_write 失败: offset=0x%lx len=%lu err=%s\n",
                offset, len, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * bar4_read() — 从 BAR4 指定偏移读取数据
 */
static int bar4_read(uint64_t offset, void *buf, uint64_t len)
{
    struct bar4_access_args args;
    args.offset = offset;
    args.length = len;
    args.buffer = buf;

    if (ioctl(g_fd, PCI_BAR4_READ, &args) < 0) {
        fprintf(stderr, "[HOST] bar4_read 失败: offset=0x%lx len=%lu err=%s\n",
                offset, len, strerror(errno));
        return -1;
    }
    return 0;
}

/* 读写单个 u32 */
static int bar4_write_u32(uint64_t offset, uint32_t val)
{
    return bar4_write(offset, &val, sizeof(val));
}

static uint32_t bar4_read_u32(uint64_t offset)
{
    uint32_t val = 0;
    bar4_read(offset, &val, sizeof(val));
    return val;
}

/* 读写单个 u8 */
static int bar4_write_u8(uint64_t offset, uint8_t val)
{
    return bar4_write(offset, &val, sizeof(val));
}

static uint8_t bar4_read_u8(uint64_t offset)
{
    uint8_t val = 0;
    bar4_read(offset, &val, sizeof(val));
    return val;
}

/* =========================================================
 * Ring Buffer 生产者操作
 * ========================================================= */

/**
 * ring_read_head() — 读取当前 head 指针（Host 自己维护，本地读即可）
 * 注意：实际部署中 head 应由 Host 在内存中维护，不需要每次从 BAR4 读回。
 * 此处为了演示清晰，每次都从 BAR4 读取。
 */
static uint32_t ring_read_head(void)
{
    return bar4_read_u32(RING_HEAD_OFFSET);
}

/**
 * ring_read_tail() — 读取 A55 维护的 tail 指针
 */
static uint32_t ring_read_tail(void)
{
    return bar4_read_u32(RING_TAIL_OFFSET);
}

/**
 * ring_is_full() — 判断队列是否已满（不能再投递新指令）
 */
static int ring_is_full(uint32_t head, uint32_t tail)
{
    return ((head - tail) & (TCM_RING_DEPTH * 2 - 1)) >= TCM_RING_DEPTH;
}

/**
 * ring_submit_cmd() — 向 Ring Buffer 投递一条度量指令
 *
 * @head:           当前 head 值（调用后会 +1）
 * @cmd_id:         指令流水号
 * @host_phys_addr: 待度量数据的 Host 物理地址
 * @payload_len:    待度量数据长度
 *
 * 写入顺序：先写指令内容，再写 head（生产者协议）。
 * 内存屏障：用户态无法直接调用 smp_wmb()，
 * 通过 __sync_synchronize()（GCC 内建全屏障）替代。
 */
static int ring_submit_cmd(uint32_t *head, uint32_t cmd_id,
                           uint64_t host_phys_addr, uint16_t payload_len)
{
    uint64_t slot_base = CMD_OFFSET(*head);
    uint8_t  zero_hash[32] = {0};

    printf("[HOST] 投递指令: cmd_id=0x%08x, slot=%u, offset=0x%lx\n",
           cmd_id, *head & TCM_RING_MASK, slot_base);

    /* Step 1: 清零 hash_result 区域，防止读到上轮残留数据 */
    if (bar4_write(slot_base + FIELD_HASH_RESULT, zero_hash, 32) < 0)
        return -1;

    /* Step 2: 写指令字段（除 status 外） */
    if (bar4_write_u32(slot_base + FIELD_CMD_ID, cmd_id) < 0)
        return -1;

    /* host_phys_addr 是 u64，拆成两个 u32 写入（避免对齐问题） */
    uint32_t addr_lo = (uint32_t)(host_phys_addr & 0xFFFFFFFF);
    uint32_t addr_hi = (uint32_t)(host_phys_addr >> 32);
    if (bar4_write_u32(slot_base + FIELD_HOST_PHYS_ADDR,     addr_lo) < 0)
        return -1;
    if (bar4_write_u32(slot_base + FIELD_HOST_PHYS_ADDR + 4, addr_hi) < 0)
        return -1;

    /* payload_len */
    uint8_t len_buf[2] = { (uint8_t)(payload_len & 0xFF),
                           (uint8_t)(payload_len >> 8) };
    if (bar4_write(slot_base + FIELD_PAYLOAD_LEN, len_buf, 2) < 0)
        return -1;

    /*
     * Step 3: 最后写 status = PENDING
     * 这是 A55 判断"有新指令"的标志，必须在所有字段写完后再写。
     */
    if (bar4_write_u8(slot_base + FIELD_STATUS, TCM_STATUS_PENDING) < 0)
        return -1;

    /*
     * Step 4: 全内存屏障
     * 确保上面所有写操作（指令内容）在 head 更新之前对 A55 可见。
     * 用户态用 GCC 内建屏障替代内核的 smp_wmb()。
     */
    __sync_synchronize();

    /* Step 5: 推进 head 指针，通知 A55 有新指令 */
    (*head)++;
    if (bar4_write_u32(RING_HEAD_OFFSET, *head) < 0)
        return -1;

    printf("[HOST] head 已推进至 %u\n", *head);
    return 0;
}

/**
 * ring_trigger_doorbell() — 敲门铃，触发 A55 IRQ 45
 *
 * 向 BAR4 内的门铃寄存器偏移写入任意值即可触发中断。
 * 具体偏移 DOORBELL_BAR4_OFFSET 需查 Hi1712 硬件手册确认。
 */
static void ring_trigger_doorbell(void)
{
    uint32_t val = 1;
    printf("[HOST] 敲门铃: BAR4 offset=0x%x\n", DOORBELL_BAR4_OFFSET);
    if (bar4_write_u32(DOORBELL_BAR4_OFFSET, val) < 0) {
        fprintf(stderr, "[HOST] 警告：门铃写入失败，A55 可能不会收到中断\n");
    }
}

/**
 * ring_wait_result() — 轮询等待指定槽位的度量结果
 *
 * @slot_idx:   槽位索引（head & MASK）
 * @cmd_id:     期望的指令流水号（用于校验）
 * @timeout_ms: 超时时间（毫秒）
 *
 * 返回 0 表示成功读到结果，-1 表示超时或错误。
 */
static int ring_wait_result(uint32_t slot_idx, uint32_t cmd_id,
                            int timeout_ms)
{
    uint64_t slot_base = RING_CMDS_OFFSET + slot_idx * CMD_SLOT_SIZE;
    int      elapsed   = 0;
    uint8_t  status;

    printf("[HOST] 等待槽位 %u 的度量结果 (超时 %d ms)...\n",
           slot_idx, timeout_ms);

    while (elapsed < timeout_ms) {
        status = bar4_read_u8(slot_base + FIELD_STATUS);

        if (status == TCM_STATUS_DONE) {
            /* 读屏障：确保读 status=DONE 之后再读 hash_result */
            __sync_synchronize();

            uint8_t hash[32];
            if (bar4_read(slot_base + FIELD_HASH_RESULT, hash, 32) < 0)
                return -1;

            /* 校验 cmd_id */
            uint32_t ret_id = bar4_read_u32(slot_base + FIELD_CMD_ID);
            if (ret_id != cmd_id) {
                fprintf(stderr, "[HOST] 警告：cmd_id 不匹配！期望 0x%08x，实际 0x%08x\n",
                        cmd_id, ret_id);
            }

            printf("[HOST] 度量完成！cmd_id=0x%08x, hash[0..7]: "
                   "%02x%02x%02x%02x %02x%02x%02x%02x\n",
                   cmd_id,
                   hash[0], hash[1], hash[2], hash[3],
                   hash[4], hash[5], hash[6], hash[7]);
            return 0;

        } else if (status == TCM_STATUS_PROCESSING) {
            /* A55 正在处理，继续等待 */
        } else if (status == TCM_STATUS_ERROR) {
            fprintf(stderr, "[HOST] A55 报告度量错误: cmd_id=0x%08x\n", cmd_id);
            return -1;
        }

        usleep(5000);   /* 每 5ms 轮询一次 */
        elapsed += 5;
    }

    fprintf(stderr, "[HOST] 等待超时: cmd_id=0x%08x, slot=%u, 最终 status=0x%02x\n",
            cmd_id, slot_idx, status);
    return -1;
}

/* =========================================================
 * 主函数
 * ========================================================= */

int main(int argc, char *argv[])
{
    int      num_cmds  = 4;     /* 默认发送 4 条指令 */
    uint32_t head      = 0;
    uint32_t cmd_id    = 1;
    int      ret       = 0;
    int      i;

    if (argc > 1) {
        num_cmds = atoi(argv[1]);
        if (num_cmds <= 0 || num_cmds > TCM_RING_DEPTH) {
            fprintf(stderr, "指令条数必须在 1~%d 之间\n", TCM_RING_DEPTH);
            return 1;
        }
    }

    printf("========================================\n");
    printf("  Host TPCM Ring Buffer 生产者测试\n");
    printf("  发送指令数: %d\n", num_cmds);
    printf("========================================\n\n");

    /* ---- 打开字符设备 ---- */
    g_fd = open("/dev/pci_bar4_driver", O_RDWR);
    if (g_fd < 0) {
        perror("[HOST] 打开 /dev/pci_bar4_driver 失败");
        fprintf(stderr, "请确认 pci_bar4_driver.ko 已加载\n");
        return 1;
    }
    printf("[HOST] 设备打开成功: /dev/pci_bar4_driver\n");

    /* ---- 读取当前 Ring Buffer 状态 ---- */
    head = ring_read_head();
    uint32_t tail = ring_read_tail();
    printf("[HOST] 初始状态: head=%u, tail=%u\n\n", head, tail);

    /* ---- 批量投递度量指令 ---- */
    for (i = 0; i < num_cmds; i++) {
        uint32_t cur_tail = ring_read_tail();

        /* 队列满则等待 A55 消费 */
        if (ring_is_full(head, cur_tail)) {
            printf("[HOST] Ring Buffer 已满，等待 A55 消费...\n");
            int wait = 0;
            while (ring_is_full(head, ring_read_tail()) && wait < 5000) {
                usleep(10000);
                wait += 10;
            }
            if (ring_is_full(head, ring_read_tail())) {
                fprintf(stderr, "[HOST] 等待超时，A55 可能未响应\n");
                ret = 1;
                goto out;
            }
        }

        /*
         * host_phys_addr：真实场景中应填待度量数据的实际物理地址。
         * 此处用一个模拟地址（0xDEAD0000 + i * 0x1000）作为占位。
         * 实际使用时通过 virt_to_phys() 或 DMA API 获取真实物理地址。
         */
        uint64_t fake_phys = 0xDEAD0000ULL + (uint64_t)i * 0x1000;
        uint16_t payload_len = 256;

        printf("[HOST] --- 投递第 %d 条指令 ---\n", i + 1);
        if (ring_submit_cmd(&head, cmd_id, fake_phys, payload_len) < 0) {
            fprintf(stderr, "[HOST] 投递指令失败\n");
            ret = 1;
            goto out;
        }

        cmd_id++;
    }

    /* ---- 批量投递完成后统一敲一次门铃 ---- */
    printf("\n[HOST] 所有指令已投递，触发门铃中断通知 A55\n");
    ring_trigger_doorbell();

    /* ---- 逐条等待结果 ---- */
    printf("\n[HOST] 开始等待 A55 回填度量结果...\n\n");

    uint32_t result_head = ring_read_head();
    uint32_t start_slot  = (result_head - num_cmds) & TCM_RING_MASK;

    for (i = 0; i < num_cmds; i++) {
        uint32_t slot = (start_slot + i) & TCM_RING_MASK;
        uint32_t expected_id = (uint32_t)(1 + i);   /* cmd_id 从 1 开始 */

        if (ring_wait_result(slot, expected_id, 10000) < 0) {
            fprintf(stderr, "[HOST] 指令 %u 等待结果失败\n", expected_id);
            ret = 1;
        }
    }

    printf("\n========================================\n");
    if (ret == 0)
        printf("  全部 %d 条指令度量完成，测试通过！\n", num_cmds);
    else
        printf("  部分指令失败，请检查 A55 侧 dmesg\n");
    printf("========================================\n");

out:
    close(g_fd);
    return ret;
}
