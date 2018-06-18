ifneq (${KERNELRELEASE},)
obj-m += pvshm.o
else
KERNEL_SOURCE := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C ${KERNEL_SOURCE} SUBDIRS=$(PWD) modules
clean :
	$(MAKE) -C ${KERNEL_SOURCE} SUBDIRS=$(PWD) clean
	rm -f  *~ *.o *.ko  *.o.ur-safe"
endif
