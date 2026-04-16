#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

// ioctl command
#define PCI_BAR4_ACCESS   _IOWR('k', 1, struct bar4_access_args)
#define PCI_BAR4_READ     _IOWR('k', 1, struct bar4_access_args)
#define PCI_BAR4_WRITE    _IOWR('k', 2, struct bar4_access_args)

// Structure for ioctl arguments
struct bar4_access_args {
    uint64_t offset;  // Offset within BAR4 space
    uint64_t length;  // Number of bytes to read/write
    void *buffer;    // User space buffer
    // int is_write;   // 1 for write, 0 for read
};

int main() {
    int fd;
    struct bar4_access_args args;

    // Open the device
    fd = open("/dev/pci_bar4_driver", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return -1;
    }

    printf("Device opened successfully\n");

    // Test write operation
    printf("\nTesting write operation...\n");
    uint8_t *test_buffer = malloc(256 * sizeof(uint8_t));
    printf("test_buffer=%p\n", test_buffer);

    args.offset = 0x200; // Example offset
    args.length = 256;
    args.buffer = (void *)test_buffer;
    // args.is_write = 1;

    // Fill the buffer with test data
    for (int i = 0; i < 256; i++) {
        test_buffer[i] = i % 256;
        // test_buffer[i] = 255 - i;
    }

    if (ioctl(fd, PCI_BAR4_WRITE, &args) < 0) {
        perror("Write ioctl failed");
        close(fd);
        return -1;
    }

    printf("Write operation completed\n");

    // Test read operation
    printf("\nTesting read operation...\n");
    args.offset = 0x200; // Same offset as write
    args.length = 256;
    args.buffer = (void *)test_buffer;
    // args.is_write = 0;

    // Clear the buffer before reading
    memset(test_buffer, 0, sizeof(test_buffer));

    if (ioctl(fd, PCI_BAR4_READ, &args) < 0) {
        perror("Read ioctl failed");
        close(fd);
        return -1;
    }

    printf("Read operation completed\n");

    // Print first few bytes to verify
    printf("First 16 bytes read: ");
    for (int i = 0; i < 16; i++) {
        printf("%d ", test_buffer[i]);
    }
    printf("\n");

    // Close the device
    close(fd);
    printf("\nDevice closed\n");

    return 0;
}