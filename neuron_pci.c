// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Manages Neuron device's PCI configuration such as BAR access and MSI interrupt setup. */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/umh.h>

#include "neuron_ds.h"
#include "neuron_reg_access.h"
#include "neuron_metrics.h"
#include "v1/fw_io.h"
#include "neuron_dma.h"
#include "neuron_dhal.h"
#include "neuron_nq.h"
#include "neuron_pci.h"
#include "neuron_log.h"
#include "neuron_cdev.h"


static struct pci_device_id neuron_pci_dev_ids[] = {
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID0) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID1) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID2) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID3) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, TRN1_DEVICE_ID0) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF2_DEVICE_ID0) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, TRN2_DEVICE_ID0) },
	{
		0,
	},
};

// some old kernels do not have pci_info defined.
#ifndef pci_info
#define pci_info(pdev, fmt, arg...) dev_info(&(pdev)->dev, fmt, ##arg)
#endif

int dup_helper_enable = 1;
module_param(dup_helper_enable, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dup_helper_enable, "enable duplicate routing id unload helper");

int wc_enable = 1;
module_param(wc_enable, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(wc_enable, "enable write combining");

// number of devices managed
static atomic_t device_count = ATOMIC_INIT(0);

struct neuron_device *neuron_devices[MAX_NEURON_DEVICE_COUNT] = { 0 };
int total_neuron_devices = 0;

extern void ndmar_preinit(struct neuron_device *nd);

struct neuron_device *neuron_pci_get_device(u8 device_index)
{
	BUG_ON(device_index >= MAX_NEURON_DEVICE_COUNT);
	return neuron_devices[device_index];
}

static int neuron_pci_device_init(struct neuron_device *nd)
{
	int i, ret;

	if (nd == NULL)
		return -1;

	// neuron devices are 64bit, so set dma mask to 64bit so that kernel can allocate memory from Normal zone
	ret = dma_set_mask(&nd->pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask(&nd->pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&nd->pdev->dev, "No usable DMA configuration.\n");
			return ret;
		} else {
			dma_set_coherent_mask(&nd->pdev->dev, DMA_BIT_MASK(32));
			dev_err(&nd->pdev->dev, "Using 32bit DMA configuration.\n");
		}
	} else {
		dma_set_coherent_mask(&nd->pdev->dev, DMA_BIT_MASK(64));
	}

	ret = nr_create_thread(nd);
	if (ret)
		return ret;

	// Set the core init state to invalid
	nci_reset_state(nd);

	ndmar_preinit(nd);

	// Initialize the mc handle map
	ret = nmch_handle_init(nd);
	if (ret) 
		goto fail_mch;

	// Initialize the device mpset
	memset(&nd->mpset, 0, sizeof(struct mempool_set));

	// Initialize the host portion in mpset
	ret = mpset_constructor(&nd->mpset, &(nd->pdev->dev), nd);
	if (ret)
		goto fail_mpset;

	// Initialize CRWL struct
	for (i = 0; i < MAX_NC_PER_DEVICE; i++)
		mutex_init(&nd->crwl[i].lock);

	ret = ncdev_create_device_node(nd);
	if (ret) {
		pci_info(nd->pdev, "create device node failed\n");
		goto fail_chardev;
	}

	ret = nr_start_ncs(nd, NEURON_NC_MAP_DEVICE, NEURON_RESET_REQUEST_ALL);
	if (ret)
		return ret;

	return 0;

fail_chardev:
	mpset_destructor(&nd->mpset);
fail_mpset:
	nmch_handle_cleanup(nd);
fail_mch:
	if (nd->fw_io_ctx)
		fw_io_destroy((struct fw_io_ctx *)nd->fw_io_ctx);

	nd->fw_io_ctx = NULL;
	return ret;
}

static int neuron_pci_device_close(struct neuron_device *nd)
{
	int ret;

	ret = ncdev_delete_device_node(nd);
	if (ret) {
		pci_info(nd->pdev, "delete device node failed\n");
		return ret;
	}
	// disable NQ after disabling PCI device so that the device cant DMA anything after this
	nnq_destroy_all(nd);
	ndmar_close(nd);
	neuron_ds_destroy(&nd->datastore);
	mpset_destructor(&nd->mpset);
	nmch_handle_cleanup(nd);

	if (nd->fw_io_ctx)
		fw_io_destroy((struct fw_io_ctx *)nd->fw_io_ctx);

	nd->fw_io_ctx = NULL;
	return 0;
}

static void neuron_pci_set_device_architecture(struct neuron_device *nd)
{
	unsigned short device = nd->pdev->device;
	enum neuron_arch arch;
	u8 revision;
	pci_read_config_byte(nd->pdev, PCI_REVISION_ID, &revision);

	switch(device) {
		case TRN1_DEVICE_ID0:
		case INF2_DEVICE_ID0:
			arch = NEURON_ARCH_V2;
			break;
		case TRN2_DEVICE_ID0:
			arch = NEURON_ARCH_V3;
			break;
		default:
			arch = NEURON_ARCH_V1;
	}
	narch_init(arch, revision);
}

static int neuron_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int ret = 0;
	struct neuron_device *nd;

	nd = kzalloc(sizeof(struct neuron_device), GFP_KERNEL);
	if (nd == NULL) {
		pci_info(dev, "Can't allocate memory for neuron_device\n");
		goto fail_alloc_nd_mem;
	}

    nmetric_init_driver_metrics(nd);
	
	if (neuron_log_init(nd)) {
		pci_warn(dev, "Warning: Can't allocate memory for neuron log\n");
	}

	nd->pdev = dev;
	pci_set_drvdata(dev, nd);

	ret = pci_enable_device(dev);
	if (ret) {
		pci_info(dev, "Can't enable the device\n");
		goto fail_enable;
	}

	pci_set_master(dev);

	// set the architecture
	neuron_pci_set_device_architecture(nd);

	ret = neuron_dhal_init(dev->device);
	if (ret) {
		pci_info(dev, "Failed to init neuron_dhal\n");
		goto fail_dhal_init;
	}

	// map apb bar
	ret = ndhal->ndhal_pci.neuron_pci_reserve_bar(dev, ndhal->ndhal_pci.apb_bar, "APB");
	if (ret) {
		goto fail_bar0_map;
	}
	ret = ndhal->ndhal_pci.neuron_pci_set_npdev(dev, ndhal->ndhal_pci.apb_bar, "APB", &nd->npdev.bar0_pa, &nd->npdev.bar0, &nd->npdev.bar0_size);
	if (ret) {
		goto fail_bar0_resource;
	}

	// map bar2
	ret = ndhal->ndhal_pci.neuron_pci_reserve_bar(dev,  ndhal->ndhal_pci.axi_bar, "AXI");
	if (ret) {
		goto fail_bar2_map;
	}
	ret = ndhal->ndhal_pci.neuron_pci_set_npdev(dev, ndhal->ndhal_pci.axi_bar, "AXI", &nd->npdev.bar2_pa, &nd->npdev.bar2, &nd->npdev.bar2_size);
	if (ret) {
		goto fail_bar2_resource;
	}

	// map bar4
	ret = ndhal->ndhal_pci.neuron_pci_reserve_bar(dev, ndhal->ndhal_pci.dram_bar, "BAR4");
	if (ret) {
		goto fail_bar4_map;
	}
	ret = ndhal->ndhal_pci.neuron_pci_set_npdev(dev, ndhal->ndhal_pci.dram_bar, "BAR4", &nd->npdev.bar4_pa, &nd->npdev.bar4, &nd->npdev.bar4_size);
	if (ret) {
		goto fail_bar4_resource;
	}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	nd->device_index = atomic_fetch_add(1, &device_count);
#else
	nd->device_index = atomic_add_return(1, &device_count) - 1;
#endif 
	nd->fw_io_ctx = fw_io_setup(nd->npdev.bar0, nd->npdev.bar0_size,
				    nd->npdev.bar2, nd->npdev.bar2_size);
	if (nd->fw_io_ctx == NULL) {
		pr_err("readless read initialization failed");
		ret = -ENODEV;
		goto fail_bar4_resource;
	}

	ret = ndhal->ndhal_pci.neuron_pci_get_device_id(nd, dev); // get device id from the device for V2
	if (ret)
		goto fail_bar4_resource;

	ret = neuron_pci_device_init(nd);
	if (ret)
		goto fail_bar4_resource;

	// initialize datastore
	ret = neuron_ds_init(&nd->datastore, nd);
	if (ret)
		goto fail_nds_resource;

	ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, NDMA_QUEUE_DUMMY_RING_SIZE, 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_NCDEV_HOST,
		       &nd->ndma_q_dummy_mc);
	if (ret)
		goto fail_nds_resource;

	// allocate memset mc (if datastore succeeded)
	ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, MEMSET_HOST_BUF_SIZE, 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_NCDEV_HOST,
		       &nd->memset_mc);
	if (ret)
		goto fail_memset_mc;

	// initialize metric aggregation and posting
 	ret = nmetric_init(nd);
 	if (ret)
 		goto fail_nmetric_resource;

	mutex_init(&nd->memset_lock);

	BUG_ON(neuron_devices[nd->device_index] != NULL);
	neuron_devices[nd->device_index] = nd;

	return 0;

fail_nmetric_resource:
	nmetric_stop_thread(nd);
	mc_free(&nd->memset_mc);
fail_memset_mc:
	mc_free(&nd->ndma_q_dummy_mc);
fail_nds_resource:
	neuron_ds_destroy(&nd->datastore);
fail_bar4_resource:
	ndhal->ndhal_pci.neuron_pci_release_bar(dev, ndhal->ndhal_pci.dram_bar);
fail_bar4_map:
fail_bar2_resource:
	ndhal->ndhal_pci.neuron_pci_release_bar(dev, ndhal->ndhal_pci.axi_bar);
fail_bar2_map:
fail_bar0_resource:
	ndhal->ndhal_pci.neuron_pci_release_bar(dev, ndhal->ndhal_pci.apb_bar);
fail_bar0_map:
	pci_disable_device(dev);
fail_dhal_init:
fail_enable:
	neuron_log_destroy( nd);
	kfree(nd);
fail_alloc_nd_mem:
	pci_set_drvdata(dev, NULL);
	return ret;
}

static void neuron_pci_remove(struct pci_dev *dev)
{
	struct neuron_device *nd;

	nd = pci_get_drvdata(dev);
	if (nd == NULL)
		return;

	nr_stop_thread(nd);

	nmetric_stop_thread(nd);

    ndhal->ndhal_ext_cleanup();

	ndhal->ndhal_pci.neuron_pci_release_bar(dev, ndhal->ndhal_pci.apb_bar);

	ndhal->ndhal_pci.neuron_pci_release_bar(dev, ndhal->ndhal_pci.axi_bar);

	ndhal->ndhal_pci.neuron_pci_release_bar(dev, ndhal->ndhal_pci.dram_bar);

	pci_disable_device(dev);

	neuron_pci_device_close(nd);

	if (nd->npdev.bar0) {
		pci_iounmap(dev, nd->npdev.bar0);
	}
	if (nd->npdev.bar2) {
		pci_iounmap(dev, nd->npdev.bar2);
	}
	if (nd->npdev.bar4) {
		pci_iounmap(dev, nd->npdev.bar4);
	}

	neuron_log_destroy(nd);

	kfree(nd);
}

static struct pci_driver neuron_pci_driver = {
	.name = "neuron-driver",
	.id_table = neuron_pci_dev_ids,
	.probe = neuron_pci_probe,
	.remove = neuron_pci_remove,
};

int neuron_pci_module_init(void)
{
	int ret;

	ret = pci_register_driver(&neuron_pci_driver);
	if (ret != 0) {
		pr_err("Failed to register neuron inf driver %d\n", ret);
		return ret;
	}
	total_neuron_devices = atomic_read(&device_count);
	return 0;
}

void neuron_pci_module_exit(void)
{
	neuron_dhal_cleanup();
	pci_unregister_driver(&neuron_pci_driver);
	neuron_dhal_free();
}
