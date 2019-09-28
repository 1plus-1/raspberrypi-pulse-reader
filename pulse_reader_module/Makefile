PWD := $(shell pwd)

obj-m := pulse_reader.o
pulse_reader-objs := module.o

all:
	make -C $(KERNEL) SUBDIRS=$(PWD) modules

clean:
	make -C $(KERNEL) SUBDIRS=$(PWD) clean

