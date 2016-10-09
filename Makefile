ifeq ($(KERNELRELEASE),)

all:
	make ARCH=arm -C /home/bobo/code/linux-at91-linux-3.10-at91 M=$(shell pwd) modules

clean:
	make ARCH=arm -C /home/bobo/code/linux-at91-linux-3.10-at91 M=$(shell pwd) clean

help:
	make ARCH=arm -C /home/bobo/code/linux-at91-linux-3.10-at91 M=$(shell pwd) help

.PHONY: all clean help

else
	ccflags-y := -std=gnu99 -Wno-declaration-after-statement
	obj-m:=usb_disp.o
	usb_disp-y := usb_display.o f_display.o pixcir_i2c_ts.o
endif
