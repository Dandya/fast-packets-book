KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := bpftest.o
bpftest-objs := bpftest_main.o bpftest_bpf.o bpftest_ioctl.o bpftest_mem.o
# EXTRA_CFLAGS += -DDEBUG

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
