obj-m += neuron.o

neuron-objs := neuron_arch.o neuron_dhal.o
neuron-objs += neuron_reg_access.o
neuron-objs += neuron_module.o neuron_pci.o neuron_mempool.o neuron_dma.o neuron_ring.o neuron_ds.o
neuron-objs += neuron_core.o neuron_crwl.o neuron_cdev.o neuron_topsp.o neuron_pid.o
neuron-objs += neuron_reset.o neuron_cinit.o neuron_mmap.o neuron_p2p.o
neuron-objs += neuron_nq.o
neuron-objs += neuron_mc_handle.o
neuron-objs += neuron_metrics.o neuron_sysfs_metrics.o
neuron-objs += udma/udma_iofic.o udma/udma_m2m.o udma/udma_main.o
neuron-objs += neuron_fw_io.o
neuron-objs += neuron_dmabuf.o
neuron-objs += neuron_log.o
neuron-objs += neuron_power.o
neuron-objs += vc/neuron_dhal_vc.o
neuron-objs += v1/fw_io.o v1/putils.o v1/neuron_dhal_v1.o
neuron-objs += v2/notific.o v2/neuron_dhal_v2.o
neuron-objs += v3/notific.o v3/neuron_dhal_v3.o v3/neuron_pelect.o

ccflags-y += -O3 -Wall -Werror -Wno-declaration-after-statement -Wunused-macros -Wunused-local-typedefs
ccflags-y += -I$(src)/
ccflags-y += $(call cc-option,-march=armv8.2-a)
