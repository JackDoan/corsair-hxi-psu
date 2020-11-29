obj-m := corsair-hxi-psu.o


ifndef KERNELRELEASE
KRELEASE := $(shell uname -r)
else
KRELEASE := $(KERNELRELEASE)
endif

KDIR := /usr/lib/modules/$(KRELEASE)/build
PWD := $(shell pwd)


all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
