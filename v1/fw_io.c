// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "address_map.h"
#include "../neuron_reg_access.h"
#include "../neuron_device.h"
#include "../neuron_arch.h"
#include "../neuron_dhal.h"
#include "fw_io.h"


// Max wait time seconds for the device to be ready
#define DEVICE_MAX_READY_WAIT 60

bool fw_io_wait_for_device_ready_v1(struct fw_io_ctx *ctx, u32 *reg_val)
{
	int i, ret;
	u64 addr = P_0_APB_MISC_RAM_BASE + V1_FW_IO_REG_FW_STATUS_OFFSET;
	for (i = 0; i < DEVICE_MAX_READY_WAIT; i++) {
		ret = fw_io_read(ctx, &addr, reg_val, 1);
		if (ret) {
			pr_err("failed to read device ready state\n");
			return false;
		}
		if (*reg_val & V1_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK)
			return true;
		msleep(1000);
	}
	return false;
}

bool fw_io_is_device_ready_v1(void __iomem *bar0)
{
	void *address;
	int ret;
	u32 val;

	address = bar0 + V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + V1_FW_IO_REG_FW_STATUS_OFFSET;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array((void **)&address, &val, 1, false);
	if (ret)
		return false;
	if (val & V1_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK)
		return true;
	return false;
}
