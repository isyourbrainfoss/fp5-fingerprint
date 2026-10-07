# Out-of-tree FT9362 foothold module for pmOS sc7280 7.1.2
# Build (from host, after modules_prepare on matched tree):
#   make -C $KDIR M=$PWD ARCH=arm64 LLVM=1 modules
#   (ensure clang 22 + PATH has llvm-ar/nm/objcopy wrappers)

obj-m += focaltech_fp_life.o
obj-m += fp_spi_se14.o
obj-m += fp_pinmux_se14.o
obj-m += fp_unreserve_se14.o
obj-m += fp_bitbang_se14.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
