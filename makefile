# Makefile for PCI BAR4 driver
obj-m += pci_bar4_driver.o
obj-m += example_kernel_module.o

# Get the kernel build environment
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Default target
all: test
	make -C $(KDIR) M=$(PWD) modules

# Clean target
clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f pci_bar4_test

# Install target
install:
	sudo insmod pci_bar4_driver.ko

# Remove target
remove:
	sudo rmmod pci_bar4_driver

# Compile user space test program
test: pci_bar4_driver.c
	gcc -o pci_bar4_test pci_bar4_test.c

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
	@echo "  example_module - Compile example kernel module"
	@echo "  help   - Show this help message"

# Default phony targets
.PHONY: all clean install remove uninstall test example_module help