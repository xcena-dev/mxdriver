PWD := $(shell pwd)

BUILDSYSTEM_DIR ?= /lib/modules/$(shell uname -r)/build
INSTALL_MOD_PATH ?=
INSTALL_MOD_PATH_ARG := $(if $(strip $(INSTALL_MOD_PATH)),INSTALL_MOD_PATH="$(INSTALL_MOD_PATH)",)

ifneq ($(KERNELRELEASE),)
	obj-m += mx_dma.o
	mx_dma-objs := init.o fops.o helper.o transfer.o mbox.o ioctl.o core_common.o core_v1.o core_v2.o
ifeq ($(WO_CXL),1)
	EXTRA_CFLAGS += -DCONFIG_WO_CXL
endif
ifeq ($(MX_DMA_DISABLE_TRACE),1)
	EXTRA_CFLAGS += -DMX_DMA_DISABLE_TRACE
else
	mx_dma-objs += trace.o
	CFLAGS_trace.o := -I$(src)
	CFLAGS_ioctl.o := -I$(src)
endif
else
all:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules
install: all
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules_install $(INSTALL_MOD_PATH_ARG) INSTALL_MOD_DIR=updates DEPMOD=/bin/true
clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean
	@/bin/rm -f *.ko modules.order *.mod.c *.o *.o.ur-safe .*.o.cmd
endif
