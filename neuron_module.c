// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fault-inject.h>

/* Only this file should create trace points, anywhere else just include neuron_trace.h*/
#define CREATE_TRACE_POINTS
#include "neuron_trace.h"

MODULE_DESCRIPTION("Neuron Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_ALIAS("pci:v00001d0fd00007064sv*sd*bc*sc*i*");

const char driver_version[] = "local-build";

extern int ncdev_module_init(void);
extern void ncdev_module_exit(void);
extern int neuron_pci_module_init(void);
extern void neuron_pci_module_exit(void);

#ifdef CONFIG_FAULT_INJECTION

extern struct fault_attr neuron_fail_nc_mmap;
extern struct fault_attr neuron_fail_dma_wait;
extern struct fault_attr neuron_fail_mc_alloc;
extern struct fault_attr neuron_fail_fwio_read;
extern struct fault_attr neuron_fail_fwio_post_metric;

static struct dentry *dbgfs_root;

static void neuron_module_init_debugfs(void)
{
	dbgfs_root = debugfs_create_dir("neuron", NULL);
	fault_create_debugfs_attr("fail_nc_mmap", dbgfs_root, &neuron_fail_nc_mmap);
	fault_create_debugfs_attr("fail_dma_wait", dbgfs_root, &neuron_fail_dma_wait);
	fault_create_debugfs_attr("fail_mc_alloc", dbgfs_root, &neuron_fail_mc_alloc);
	fault_create_debugfs_attr("fail_fwio_read", dbgfs_root, &neuron_fail_fwio_read);
	fault_create_debugfs_attr("fail_fwio_post_metric", dbgfs_root,
				  &neuron_fail_fwio_post_metric);
}

static void neuron_module_free_debugfs(void)
{
	debugfs_remove_recursive(dbgfs_root);
	dbgfs_root = NULL;
}
#endif

static int __init neuron_module_init(void)
{
	int ret;

	printk(KERN_INFO "Neuron Driver Started with Version:%s", driver_version);

#ifdef CONFIG_FAULT_INJECTION
	neuron_module_init_debugfs();
#endif

	ret = ncdev_module_init();
	if (ret)
		return ret;

	ret = neuron_pci_module_init();
	if (ret)
		return ret;

	return 0;
}

static void __exit neuron_module_exit(void)
{
#ifdef CONFIG_FAULT_INJECTION
	neuron_module_free_debugfs();
#endif
	neuron_pci_module_exit();
	ncdev_module_exit();
}

module_init(neuron_module_init);
module_exit(neuron_module_exit);
