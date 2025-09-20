KERNEL_SRC_DIR  ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KERNEL_SRC_DIR) M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC_DIR) M=$(PWD) clean

