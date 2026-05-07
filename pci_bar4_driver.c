#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PCI Driver Developer");
MODULE_DESCRIPTION("PCI driver with BAR4 space access for devices already claimed by other drivers");

#define DEVICE_NAME "pci_bar4_driver"
#define CLASS_NAME  "pci_bar4"

// Updated per lspci -vvv -s 21:00.7 on Kunpeng 950
#define TARGET_DOMAIN 0x0000
#define TARGET_BUS    0x21
#define TARGET_DEV    0x00
#define TARGET_FUNC   0x7

// ioctl command
// #define PCI_BAR4_ACCESS _IOWR('k', 1, struct bar4_access_args)
#define PCI_BAR4_READ  _IOWR('k', 1, struct bar4_access_args)
#define PCI_BAR4_WRITE _IOWR('k', 2, struct bar4_access_args)

// Structure for ioctl arguments
struct bar4_access_args {
    uint64_t offset;    // Offset within BAR4 space
    uint64_t length;    // Number of bytes to read/write
    void __user *buffer;// User space buffer
    // int is_write;     // 1 for write, 0 for read
};

// Device specific data
static struct pci_dev *pdev = NULL;
static void __iomem *bar4_mapped = NULL;
static unsigned long bar4_size = 0;
static unsigned long bar4_start = 0;

// Device and driver structures
static dev_t dev_num;
static struct class *pci_bar4_class = NULL;
static struct device *pci_bar4_device = NULL;
static struct cdev cdev;

// Internal function to access BAR4 space
static int bar4_access_internal(unsigned long offset, size_t length, void *buffer, int is_write)
{
    // Validate parameters
    if (!bar4_mapped) {
        printk(KERN_ERR "bar4 not mapped\n");
        return -EIO;
    }

    if (offset + length > bar4_size) {
        printk(KERN_ERR "offset + length > bar4_size\n");
        return -EINVAL;
    }

    if (length == 0) {
        printk(KERN_ERR "length == 0\n");
        return -EINVAL;
    }

    if (is_write) {
        // Write to BAR4 space
        memcpy_toio(bar4_mapped + offset, buffer, length);
    } else {
        // Read from BAR4 space
        memcpy_fromio(buffer, bar4_mapped + offset, length);
    }

    return 0;
}

// Exported function for other kernel modules to read from BAR4 space
int pci_bar4_read(unsigned long offset, size_t length, void *buffer)
{
    if (!buffer) {
        printk(KERN_ERR "buffer is NULL\n");
        return -EINVAL;
    }

    return bar4_access_internal(offset, length, buffer, 0);
}
EXPORT_SYMBOL(pci_bar4_read);

// Exported function for other kernel modules to write to BAR4 space
int pci_bar4_write(unsigned long offset, size_t length, void *buffer)
{
    if (!buffer) {
        printk(KERN_ERR "buffer is NULL\n");
        return -EINVAL;
    }

    return bar4_access_internal(offset, length, buffer, 1);
}
EXPORT_SYMBOL(pci_bar4_write);

// Open function
static int pci_bar4_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "PCI BAR4 driver opened\n");
    return 0;
}

// Release function
static int pci_bar4_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "PCI BAR4 driver closed\n");
    return 0;
}

// ioctl function
static long pci_bar4_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct bar4_access_args args;
    void __user *user_buffer;
    void *kernel_buffer = NULL;
    int ret = 0;

    // Validate parameters
    if (!bar4_mapped) {
        printk(KERN_ERR "BAR4 space not mapped\n");
        return -EIO;
    }
    // Check command
    if (_IOC_TYPE(cmd) != 'k' || (_IOC_NR(cmd) != 1 && _IOC_NR(cmd) != 2))
    {
        printk(KERN_ERR "Cmd not ok\n");
        return -ENOTTY;
    }
    // Copy arguments from user space
    if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
        printk(KERN_ERR "Copy form user failed\n");
        return -EFAULT;
    }
    if (args.offset + args.length > bar4_size) {
        printk(KERN_ERR "Access beyond BAR4 space boundary\n");
        return -EINVAL;
    }
    if (args.length == 0) {
        printk(KERN_ERR "length == 0\n");
        return -EINVAL;
    }
    // Allocate kernel buffer to safely handle user data
    kernel_buffer = kzalloc(args.length, GFP_KERNEL);
    if (!kernel_buffer) {
        printk(KERN_ERR "kmalloc failed\n");
        return -ENOMEM;
    }

    printk(KERN_INFO "Read/Write: offset=%llu, length=%llu, user_buf=%px\n",
           args.offset, args.length, args.buffer);

    user_buffer = args.buffer;
    switch (cmd) {
        case PCI_BAR4_WRITE:
            printk(KERN_INFO "cmd =PCI_BAR4_READ\n");
            // Copy data from user space to kernel
            if (copy_from_user(kernel_buffer, user_buffer, args.length))
            {
                printk(KERN_ERR "copy_from_user failed\n");
                kfree(kernel_buffer);
                return -EFAULT;
            }
            // Call internal function to write to BAR4 space
            ret = bar4_access_internal(args.offset, args.length, kernel_buffer, 1);
            break;
        case PCI_BAR4_READ:
            printk(KERN_INFO "cmd =PCI_BAR4_WRITE\n");
            // Call internal function to read from BAR4 space
            ret = bar4_access_internal(args.offset, args.length, kernel_buffer, 0);
            if (ret == 0) {
                // Copy data from kernel space to user space
                if (copy_to_user(user_buffer, kernel_buffer, args.length)) {
                    printk(KERN_ERR "copy_to_user failed\n");
                    kfree(kernel_buffer);
                    return -EFAULT;
                }
            }
            break;
        default:
            printk(KERN_ERR "PCI BAR4 Driver: Unknown ioctl command\n");
            ret = -ENOTTY;
            goto cleanup;
    }

cleanup:
    // printk(KERN_ERR "f\n");
    kfree(kernel_buffer);
    return ret;
}

// Read function for character device
static ssize_t pci_bar4_read_func(struct file *file,
char __user *buf, size_t count, loff_t *ppos)
{
    void *kernel_buffer = NULL;
    int ret = 0;

    // Validate parameters
    if (!bar4_mapped) {
        return -EIO;
    }

    if (*ppos + count > bar4_size) {
        if (*ppos >= bar4_size) {
            return 0; // End of device
        }
        count = bar4_size - *ppos;
    }

    if (count == 0) {
        return 0;
    }

    // Allocate kernel buffer
    kernel_buffer = kmalloc(count, GFP_KERNEL);
    if (!kernel_buffer) {
        return -ENOMEM;
    }

    // Read from BAR4 space
    ret = bar4_access_internal(*ppos, count, kernel_buffer, 0);
    if (ret) {
        kfree(kernel_buffer);
        return ret;
    }

    // Copy data to user space
    if (copy_to_user(buf, kernel_buffer, count)) {
        kfree(kernel_buffer);
        return -EFAULT;
    }

    kfree(kernel_buffer);
    *ppos += count;
    return count;
}

// Write function for character device
static ssize_t pci_bar4_write_func(struct file *file,
const char __user *buf, size_t count, loff_t *ppos)
{
    void *kernel_buffer = NULL;
    int ret = 0;

    // Validate parameters
    if (!bar4_mapped) {
        return -EIO;
    }

    if (*ppos + count > bar4_size) {
        if (*ppos >= bar4_size) {
            return -EFBIG; // Beyond device size
        }
        count = bar4_size - *ppos;
    }

    if (count == 0) {
        return 0;
    }

    // Allocate kernel buffer
    kernel_buffer = kmalloc(count, GFP_KERNEL);
    if (!kernel_buffer) {
        return -ENOMEM;
    }

    // Copy data from user space
    if (copy_from_user(kernel_buffer, buf, count))
    {
        kfree(kernel_buffer);
        return -EFAULT;
    }

    // Write to BAR4 space
    ret = bar4_access_internal(*ppos, count, kernel_buffer, 1);
    kfree(kernel_buffer);
    if (ret) {
        return ret;
    }

    *ppos += count;
    return count;
}

// File operations structure
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = pci_bar4_open,
    .release = pci_bar4_release,
    .read = pci_bar4_read_func,
    .write = pci_bar4_write_func,
    .unlocked_ioctl = pci_bar4_ioctl,
    .llseek = default_llseek,
};

// Module initialization
static int __init pci_bar4_init(void)
{
    int ret;

    printk(KERN_INFO "Loading PCI BAR4 driver\n");

    // 1. Find the PCI device
    pdev = pci_get_domain_bus_and_slot(TARGET_DOMAIN, TARGET_BUS, PCI_DEVFN(TARGET_DEV, TARGET_FUNC));
    if (!pdev) {
        printk(KERN_ERR "PCI device not found\n");
        return -ENODEV;
    }

    // enable pci device
    ret = pci_enable_device(pdev);
    if (ret) {
        printk(KERN_ERR "PCI BAR4 Driver: Failed to enable PCI device\n");
        return ret;
    }

    printk(KERN_INFO "Found PCI device: %04x:%04x\n", pdev->vendor, pdev->device);

    // 2. Check if BAR4 is valid
    if (!(pci_resource_flags(pdev, 4) & IORESOURCE_MEM)) {
        printk(KERN_ERR "BAR4 is not memory mapped\n");
        ret = -ENODEV;
        goto error_put_pci;
    }

    bar4_start = pci_resource_start(pdev, 4);
    bar4_size = pci_resource_len(pdev, 4);

    if (!bar4_start || !bar4_size) {
        pr_err("Invalid BAR4\n");
        ret = -EINVAL;
        goto error_put_pci;
    }

    printk(KERN_INFO "BAR4 start: 0x%lx, size: %lu bytes\n", bar4_start, bar4_size);

    // 3. Request the BAR4 resource (已注释，用于已被其他驱动占用的BAR4)
    // if (!request_mem_region(bar4_start, bar4_size, DEVICE_NAME)) {
    //     printk(KERN_ERR "Failed to request BAR4 memory region\n");
    //     ret = -EBUSY;
    //     goto error_put_pci;
    // }

    // 4. iMap the BAR4 space to virtual memory
    // Use ioremap_wc (Write-Combining) for PCIe shared memory to improve throughput
    bar4_mapped = ioremap_wc(bar4_start, bar4_size);
    if (!bar4_mapped) {
        printk(KERN_ERR "Failed to map BAR4 space\n");
        ret = -ENOMEM;
        goto error_map;
    }

    // 5. Allocate device number
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret) {
        printk(KERN_ERR "Failed to allocate device number\n");
        goto error_alloc;
    }

    // 6. Initialize and add the character device
    cdev_init(&cdev, &fops);
    cdev.owner = THIS_MODULE;
    ret = cdev_add(&cdev, dev_num, 1);
    if (ret) {
        printk(KERN_ERR "Failed to add character device\n");
        goto error_cdev;
    }

    // 7. Create device class
    pci_bar4_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(pci_bar4_class)) {
        printk(KERN_ERR "Failed to create device class\n");
        ret = PTR_ERR(pci_bar4_class);
        goto error_class;
    }

    // 8. Create device
    pci_bar4_device = device_create(pci_bar4_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(pci_bar4_device))
    {
        printk(KERN_ERR "Failed to create device\n");
        ret = PTR_ERR(pci_bar4_device);
        goto error_device;
    }

    printk(KERN_INFO "PCI BAR4 driver loaded successfully\n");
    return 0;

error_device:
    class_destroy(pci_bar4_class);
error_class:
    cdev_del(&cdev);
error_cdev:
    unregister_chrdev_region(dev_num, 1);
error_alloc:
    iounmap(bar4_mapped);
error_map:
    /* request_mem_region is not used (commented out), nothing to release here */
error_put_pci:
    pci_dev_put(pdev);
    return ret;
}

// Module exit
static void __exit pci_bar4_exit(void)
{
    printk(KERN_INFO "Unloading PCI BAR4 driver\n");

    if (pci_bar4_device) {
        device_destroy(pci_bar4_class, dev_num);
    }

    if (pci_bar4_class) {
        class_destroy(pci_bar4_class);
    }

    cdev_del(&cdev);
    unregister_chrdev_region(dev_num, 1);

    if (bar4_mapped) {
        iounmap(bar4_mapped);
    }

    // 恢复请求的内存资源（如果之前请求过）
    // release_mem_region(bar4_start, bar4_size);

    if (pdev) {
        pci_disable_device(pdev);
        pci_dev_put(pdev);
    }

    printk(KERN_INFO "PCI BAR4 driver unloaded\n");
}

module_init(pci_bar4_init);
module_exit(pci_bar4_exit);