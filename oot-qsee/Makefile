obj-m += qsee_probe_fp5.o
obj-m += qsee_ta_load.o
obj-m += qsee_ta_talk.o
obj-m += qsee_cmd_scan.o
obj-m += qsee_hdr_probe.o
obj-m += qsee_session.o
obj-m += qsee_hdr_probe.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
obj-m += qsee_focal_ok.o
obj-m += qsee_explore.o
obj-m += qsee_fingerpr.o
