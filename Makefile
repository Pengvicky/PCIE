# Makefile for PCI BAR4 driver
obj-m += pci_bar4_driver.o
# example_kernel_module is a demo only; source is example_kernel_model.c (not built by default)

# Get the kernel build environment
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Default target
all: test
	make -C $(KDIR) M=$(PWD) modules

# Clean target
clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f pci_bar4_test host_tpcm_producer
	rm -f libtpcm_pcie.so libtpcm_pcie.a tpcm_pcie_client.o
	rm -f example_tpcm_measure

# Install target
install:
	sudo insmod pci_bar4_driver.ko

# Remove target
remove:
	sudo rmmod pci_bar4_driver

# Compile user space test program
test: pci_bar4_test.c
	gcc -o pci_bar4_test pci_bar4_test.c

# Compile Host TPCM Ring Buffer producer test
producer:
	gcc -O2 -Wall -o host_tpcm_producer host_tpcm_producer.c

# ── 客户端库 ────────────────────────────────────────────────
# 动态库：libtpcm_pcie.so（业务方 -ltpcm_pcie 链接）
lib: tpcm_pcie_client.c tpcm_pcie_client.h
	gcc -O2 -Wall -fPIC -shared \
		-o libtpcm_pcie.so tpcm_pcie_client.c

# 静态库：libtpcm_pcie.a（嵌入到可执行文件，无运行时依赖）
lib-static: tpcm_pcie_client.c tpcm_pcie_client.h
	gcc -O2 -Wall -c -o tpcm_pcie_client.o tpcm_pcie_client.c
	ar rcs libtpcm_pcie.a tpcm_pcie_client.o
	rm -f tpcm_pcie_client.o

# 示例程序（演示如何使用客户端库）
example: example_tpcm_measure.c libtpcm_pcie.so
	gcc -O2 -Wall -o example_tpcm_measure example_tpcm_measure.c \
		-L. -ltpcm_pcie -Wl,-rpath,.

# Compile example kernel module
# example_module: example_kernel_module.c
# 	make -C $(KDIR) M=$(PWD) modules

# Uninstall target
uninstall: remove

# Help target
help:
	@echo "Available targets:"
	@echo "  all    - Build all kernel modules"
	@echo "  clean  - Clean build files"
	@echo "  install - Install the main PCI BAR4 driver module"
	@echo "  remove  - Remove the main PCI BAR4 driver module"
	@echo "  uninstall - Same as remove"
	@echo "  test   - Compile user space test application"
	@echo "  producer - Compile Host TPCM Ring Buffer producer test"
	@echo "  help   - Show this help message"

# Default phony targets
.PHONY: all clean install remove uninstall test producer lib lib-static example help