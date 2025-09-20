// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Neuron driver supports only single type of chip in a system.
 *  So the driver caches the first device's arch type and uses it as the arch.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "neuron_arch.h"

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
