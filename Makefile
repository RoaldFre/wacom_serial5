obj-m += wacom_serial.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

ins:
	sync
	insmod wacom_serial.ko
	sync
rm:
	sync
	rmmod wacom_serial
	sync
