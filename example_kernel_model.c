#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>   // 内核整数类型(uint8_t/uint16_t等)定义必需
#include <linux/string.h>  // memset/memcmp等内存操作函数必需

// 声明从pci_bar4_driver模块导出的读写函数
extern int pci_bar4_read(unsigned long offset, size_t length, void *buffer);
extern int pci_bar4_write(unsigned long offset, size_t length, void *buffer);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kernel Module Developer");
MODULE_DESCRIPTION("Example module using exported functions from PCI BAR4 driver");

// ==================== 读写结构体测试Demo ====================
// 嵌套子结构体
typedef struct inside_data_s {
    uint8_t i_a;
    uint16_t i_b;
    uint32_t i_c;
} inside_data_s;

// 主测试结构体
typedef struct demo_data_s {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint8_t info[64];
    inside_data_s inside;
} demo_data_s;

static int read_write_struct(void) {
    int ret;

    // 1. 填充测试结构体数据
    demo_data_s data;
    data.a = 0x10001000;
    data.b = 0x10001001;
    data.c = 0x10001002;
    for (int i = 0; i < 64; i++) {
        data.info[i] = 0xf0 + i;
    }
    data.inside.i_a = 0xff;
    data.inside.i_b = 0xbeef;
    data.inside.i_c = 0xbeefbeef;

    // 2. 调用导出的写函数，写入BAR4偏移0x100
    ret = pci_bar4_write(0x100, sizeof(demo_data_s), &data);
    if (ret) {
        printk(KERN_ERR "Failed to write to BAR4 space: %d\n", ret);
        return ret;
    }
    printk(KERN_INFO "Successfully wrote to BAR4 space (struct demo)\n");

    // 3. 调用导出的读函数，从BAR4偏移0x100回读数据
    printk(KERN_INFO "Reading from BAR4 space (struct demo)...\n");
    demo_data_s data_read;
    memset(&data_read, 0, sizeof(demo_data_s));

    ret = pci_bar4_read(0x100, sizeof(demo_data_s), &data_read);
    if (ret) {
        printk(KERN_ERR "Failed to read from BAR4 space: %d\n", ret);
        return ret;
    }
    printk(KERN_INFO "Successfully read from BAR4 space (struct demo)\n");

    // 4. 打印读取到的结构体字段
    printk(KERN_INFO "data.a=0x%x\n", data_read.a);
    printk(KERN_INFO "data.b=0x%x\n", data_read.b);
    printk(KERN_INFO "data.c=0x%x\n", data_read.c);
    for (int i = 0; i < 8; i++) {
        printk(KERN_INFO "data.info[%d]=0x%x\n", i, data_read.info[i]);
    }
    printk(KERN_INFO "data.inside.i_a=0x%x\n", data_read.inside.i_a);
    printk(KERN_INFO "data.inside.i_b=0x%x\n", data_read.inside.i_b);
    printk(KERN_INFO "data.inside.i_c=0x%x\n", data_read.inside.i_c);

    // 5. 验证读写数据一致性
    ret = memcmp(&data, &data_read, sizeof(demo_data_s));
    if (ret != 0) {
        printk(KERN_ERR "Struct data write/read mismatch! ret=%d\n", ret);
    } else {
        printk(KERN_INFO "Struct data write/read match ✅\n");
    }
    return ret;
}

// ==================== 读写缓冲区测试Demo ====================
static int read_write_buffer(void) {
    char *buffer_write;
    char *buffer_read;
    int ret;

    // 1. 分配256字节内核缓冲区
    buffer_write = kzalloc(256, GFP_KERNEL);
    buffer_read = kzalloc(256, GFP_KERNEL);
    if (!buffer_write || !buffer_read) {
        return -ENOMEM;
    }

    // 2. 填充缓冲区测试数据
    printk(KERN_INFO "Writing to BAR4 space (buffer demo)...\n");
    int i;
    for (i = 0; i < 256; i++) {
        // buffer_write[i] = i % 256;  // 备用递增填充逻辑
        buffer_write[i] = 256 - i;  // 当前递减填充逻辑
    }

    // 3. 调用导出的写函数，写入BAR4偏移0x300
    ret = pci_bar4_write(0x300, 256, buffer_write);
    if (ret) {
        printk(KERN_ERR "Failed to write to BAR4 space: %d\n", ret);
        kfree(buffer_write);
        kfree(buffer_read);
        return ret;
    }
    printk(KERN_INFO "Successfully wrote to BAR4 space (buffer demo)\n");

    // 4. 调用导出的读函数，从BAR4偏移0x300回读数据
    printk(KERN_INFO "Reading from BAR4 space (buffer demo)...\n");
    ret = pci_bar4_read(0x300, 256, buffer_read);
    if (ret) {
        printk(KERN_ERR "Failed to read from BAR4 space: %d\n", ret);
        kfree(buffer_write);
        kfree(buffer_read);
        return ret;
    }
    printk(KERN_INFO "Successfully read from BAR4 space (buffer demo)\n");

    // 5. 打印前8字节缓冲区数据，验证读写一致性
    ret = memcmp(buffer_read, buffer_write, 256);
    for (i = 0; i < 8; i++) {
        printk(KERN_INFO "buffer_read[%d]=%d\n", i, buffer_read[i]);
    }
    if (ret != 0) {
        printk(KERN_ERR "Buffer data write/read mismatch! ret=%d\n", ret);
    } else {
        printk(KERN_INFO "Buffer data write/read match ✅\n");
    }

    // 6. 释放内核缓冲区
    kfree(buffer_write);
    kfree(buffer_read);
    return 0;
}

// ==================== 模块初始化/退出 ====================
static int __init example_module_init(void)
{
    printk(KERN_INFO "=== Loading example module (uses PCI BAR4 exported functions) ===\n");
    printk(KERN_INFO "\n");

    // 先执行缓冲区读写测试
    printk(KERN_INFO "========== Buffer Read/Write Test ==========\n");
    read_write_buffer();
    printk(KERN_INFO "========== Buffer Read/Write Test End ==========\n");
    printk(KERN_INFO "\n");

    // 再执行结构体读写测试
    printk(KERN_INFO "========== Struct Read/Write Test ==========\n");
    read_write_struct();
    printk(KERN_INFO "========== Struct Read/Write Test End ==========\n");
    printk(KERN_INFO "\n");

    printk(KERN_INFO "=== Example module loaded successfully ===\n");
    return 0;
}

static void __exit example_module_exit(void)
{
    printk(KERN_INFO "=== Unloading example module ===\n");
}

module_init(example_module_init);
module_exit(example_module_exit);