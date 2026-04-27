obj-m += presence.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

deploy:
	sudo cp presence.ko /lib/modules/6.12.47+rpt-rpi-v8/extra/
	sudo depmod -a