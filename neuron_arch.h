// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_ARCH_H
#define NEURON_ARCH_H

#include <linux/bug.h>
#include <linux/types.h>

enum neuron_arch {
	NEURON_ARCH_INVALID,
	NEURON_ARCH_V1 = 1,
	NEURON_ARCH_V2 = 2,
	NEURON_ARCH_V3 = 3,
	NEURON_ARCH_NUM
};

/**
 * narch_init() - Set neuron devices architecture and revision.
 *
 * @arch: architecture of the neuron devices in this system
 * @revision: revision id of the neuron devices in this system
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
void narch_init(enum neuron_arch arch, u8 revision);

/**
 * narch_get_arch() - Get architecture of neuron devices present in the system.
 *
 * Return: architecture.
 */
enum neuron_arch narch_get_arch(void);

/**
 * narch_get_revision() - Get revision id of neuron devices present in the system.
 *
 * Return: revision.
 */
u8 narch_get_revision(void);

/**
 * narch_is_qemu() - Checks if running on qemu.
 *
 * Return: True if running on qemu.
 */
bool narch_is_qemu(void);

/**
 * narch_is_emu() - Checks if running on hardware emulator.
 *
 * Return: True if running on emulator.
 */
bool narch_is_emu(void);

#endif
