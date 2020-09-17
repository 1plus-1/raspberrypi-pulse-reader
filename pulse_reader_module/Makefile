PWD := $(shell pwd)

obj-m := pulse_reader.o
pulse_reader-y := module.o

all:
	make -C $(KERNEL) M=$(PWD) modules

clean:
	make -C $(KERNEL) M=$(PWD) clean

