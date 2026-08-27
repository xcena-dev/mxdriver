PWD := $(shell pwd)
MULTIARCH_INCLUDE ?= /usr/include/$(shell $(CC) -print-multiarch)

BUILDSYSTEM_DIR ?= /lib/modules/$(shell uname -r)/build
INSTALL_MOD_PATH ?=
INSTALL_MOD_PATH_ARG := $(if $(strip $(INSTALL_MOD_PATH)),INSTALL_MOD_PATH="$(INSTALL_MOD_PATH)",)

ifneq ($(KERNELRELEASE),)
	obj-m += mx_dma.o
	mx_dma-objs := init.o fops.o lease.o helper.o transfer.o mbox.o ioctl.o core_common.o core_v1.o core_v2.o
	ccflags-y += -I$(src)/include/uapi
ifeq ($(WO_CXL),1)
	EXTRA_CFLAGS += -DCONFIG_WO_CXL
endif
ifeq ($(MX_DMA_DISABLE_TRACE),1)
	EXTRA_CFLAGS += -DMX_DMA_DISABLE_TRACE
else
	mx_dma-objs += trace.o
	CFLAGS_trace.o := -I$(src)
	CFLAGS_ioctl.o := -I$(src)
	CFLAGS_transfer.o := -I$(src)
	CFLAGS_core_common.o := -I$(src)
endif
else
all:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules
install: all
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules_install $(INSTALL_MOD_PATH_ARG) INSTALL_MOD_DIR=updates DEPMOD=/bin/true
clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean
	@/bin/rm -f *.ko modules.order *.mod.c *.o *.o.ur-safe .*.o.cmd
test-lease:
	$(CC) -std=c11 -Wall -Wextra -Werror -I$(PWD) -I$(PWD)/include/uapi \
		tests/lease_state_machine_test.c -o /tmp/mx_lease_state_machine_test
	/tmp/mx_lease_state_machine_test
test-ofd:
	$(CC) -std=c11 -Wall -Wextra -Werror -I$(PWD) -I$(PWD)/include/uapi \
		tests/lease_ofd_lifetime_test.c -o /tmp/mx_lease_ofd_lifetime_test
	/tmp/mx_lease_ofd_lifetime_test
test-uapi:
	$(CC) -std=c11 -Wall -Wextra -Werror -I$(PWD) -I$(PWD)/include/uapi \
		tests/lease_uapi_layout_test.c -o /tmp/mx_lease_uapi_layout_test
	/tmp/mx_lease_uapi_layout_test
	$(CC) -m32 -std=c11 -Wall -Wextra -Werror -I$(PWD) -I$(PWD)/include/uapi \
		-I$(MULTIARCH_INCLUDE) \
		-c tests/lease_uapi_layout_test.c -o /tmp/mx_lease_uapi_layout_test_32.o
test-source:
	bash tests/lifecycle_source_test.sh
	bash tests/dkms_source_layout_test.sh
endif
