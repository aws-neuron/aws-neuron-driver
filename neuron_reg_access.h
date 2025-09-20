// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef __NEURON_REG_ACCESS__H_
#define __NEURON_REG_ACCESS__H_

#include <linux/kernel.h>
#include <linux/slab.h>
#include <asm/bug.h>
#include <asm/barrier.h>
#include <asm/delay.h>
#include <asm/io.h>

/**
 * reg_read32() - read a 32bit register.
 * 
 * @addr: register address.
 * @value: read value would be stored here.
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
int reg_read32(const u32 __iomem *addr, u32 *value);

/**
 * reg_write32() - write to a 32bit register
 *
 * @addr: register address.
 * @value: value to write.
 */
void reg_write32(u32 __iomem *addr, u32 value);

/**
 * reg_write32_masked() - change selected bits in a register.
 *
 * @addr: register address
 * @mask: mask to apply. Bits not selected(1) by mask will be left unchanged
 * @data: value to write
 *
 * Return: 0 if modification succeeds, a negative error code otherwise.
 */
int reg_write32_masked(u32 __iomem *addr, u32 mask, u32 data);

#endif
