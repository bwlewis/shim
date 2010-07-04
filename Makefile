ifneq (${KERNELRELEASE},)
obj-m += pvshm.o
else
KERNEL_SOURCE := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C ${KERNEL_SOURCE} SUBDIRS=$(PWD) modules
clean :
	$(MAKE) -C ${KERNEL_SOURCE} SUBDIRS=$(PWD) clean
	rm -f *.o *.ko testmmap synch msync
endif

test:
	cc -Wall -o synch synch.c
	cc -Wall -o msync msync.c
	cc -Wall -o testmmap testmmap.c
