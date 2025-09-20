// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fault-inject.h>
#include <linux/time.h>
#include <linux/timekeeping.h>

/* Only this file should create trace points, anywhere else just include neuron_trace.h*/
#define CREATE_TRACE_POINTS
#include "neuron_trace.h"
#include "neuron_cdev.h"
#include "neuron_pci.h"

MODULE_DESCRIPTION("Neuron Driver, built from SHA: bab563e32c62d9dd615a42079e5bbd8e1a6327b1");
MODULE_LICENSE("GPL");
MODULE_VERSION("2.24.7.0");
MODULE_ALIAS("pci:v00001d0fd00007064sv*sd*bc*sc*i*");

const char driver_version[] = "2.24.7.0";
const char driver_revision[] = "bab563e32c62d9dd615a42079e5bbd8e1a6327b1";

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

static const char * month2name[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

static int __init neuron_module_init(void)
{
	int ret;
	struct tm tm;
	struct timespec64 tv;

	ktime_get_real_ts64(&tv);
	time64_to_tm(tv.tv_sec, 0, &tm);
	tm.tm_mon = ((unsigned int)tm.tm_mon > 11) ? 0:tm.tm_mon;
	
	// use ERR log level so driver version will be logged to the serial console
	printk(KERN_ERR "Neuron Driver Started with Version:%s-%s at: %4ld-%3s-%02d %02d:%02d:%02d.%06d", driver_version, driver_revision,
			tm.tm_year+1900, month2name[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_nsec/1000));

	nmetric_init_constants_metrics();

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
