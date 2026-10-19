// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Neuron driver supports only single type of chip in a system.
 *  So the driver caches the first device's arch type and uses it as the arch.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "neuron_arch.h"
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/kernel_read_file.h>
#endif

struct neuron_arch_info {
	enum neuron_arch arch;
	u32 revision;
};
static  struct neuron_arch_info arch_info = {
	.arch = NEURON_ARCH_INVALID,
	.revision = 0
}; // expect same parameters for all devices.

#define REVID_EMU 240
#define REVID_QEMU 255

void narch_init(enum neuron_arch arch, u8 revision)
{
	// set only during first device init.
	if (arch_info.arch != NEURON_ARCH_INVALID)
		return;
	arch_info.arch = arch;
	arch_info.revision = revision;
}

enum neuron_arch narch_get_arch(void)
{
	BUG_ON(arch_info.arch == NEURON_ARCH_INVALID);
	return arch_info.arch;
}

u8 narch_get_revision(void)
{
	BUG_ON(arch_info.arch == NEURON_ARCH_INVALID);
	return arch_info.revision;
}

bool narch_is_qemu(void)
{
	BUG_ON(arch_info.arch == NEURON_ARCH_INVALID);
	return arch_info.revision == REVID_QEMU;
}

bool narch_is_emu(void)
{
	BUG_ON(arch_info.arch == NEURON_ARCH_INVALID);
	return arch_info.revision == REVID_EMU;
}

int narch_get_instance_type_name(char *instance_type_name, size_t instance_type_name_size) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    ssize_t len;
    ssize_t file_size;
    void *buf = kzalloc(PAGE_SIZE, GFP_KERNEL);

    if (buf == NULL) {
        pr_err("failed to allocate buffer to read instance type");
        return -ENOMEM;
    }

    len = kernel_read_file_from_path("/sys/class/dmi/id/product_name",
                                     0, &buf, 64, &file_size, READING_UNKNOWN);
    if (!len) {
        pr_err("read instance type failed");
        kfree(buf);
        return -EIO;
    }

    snprintf(instance_type_name, instance_type_name_size, "%s", (char *)buf);

    kfree(buf);
    return 0;
#else
	return -ENOSYS;
#endif
}

