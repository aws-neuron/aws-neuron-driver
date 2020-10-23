obj-m += neuron.o

neuron-objs := neuron_module.o neuron_pci.o neuron_mempool.o neuron_dma.o neuron_ring.o
neuron-objs += neuron_core.o neuron_cdev.o
neuron-objs += udma/udma_iofic.o udma/udma_m2m.o udma/udma_main.o v1/fw_io.o

ccflags-y += -O3 -Wall -Werror -Wno-declaration-after-statement -Wunused-macros -Wunused-local-typedefs
ccflags-y += -I$(src)/
