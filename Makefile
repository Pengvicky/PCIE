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
.PHONY: all clean install remove uninstall test producer example_module help