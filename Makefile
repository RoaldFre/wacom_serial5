obj-m += wacom_serial5.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

debug:
	make -C /lib/modules/$(shell uname -r)/build KBUILD_CFLAGS+="-g -O0" M=$(shell pwd)  modules

ins:
	sync
	sudo insmod wacom_serial5.ko
	sync
rm:
	sync
	sudo rmmod wacom_serial5
	sync
