// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019-2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef FW_IO_H
#define FW_IO_H

#include "../neuron_fw_io.h"

// offsets in MISC RAM for FWIO
enum {
	V1_FW_IO_REG_FW_STATUS_OFFSET = 0x1c,
	V1_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK = 0x80000000,
	V1_FW_IO_REG_FW_STATUS_EAST_LINK_MASK = 0x1,
	V1_FW_IO_REG_FW_STATUS_WEST_LINK_MASK = 0x2,
};

/**
 * fw_io_is_device_ready_v1() - Checks if the device is ready.
 *
 * @bar0 - Device's BAR0 base address
 *
 * Return: true if device is ready, false if device is still coming out of reset.
 */
bool fw_io_is_device_ready_v1(void __iomem *bar0);

/** Wait for device to become ready.
 *
 * @param ctx		- FWIO context of the device.
 * @return true		- if device is ready.
 *         false	- if device is not ready even after waiting.
 */
bool fw_io_wait_for_device_ready_v1(struct fw_io_ctx *ctx, u32 *reg_val);

#endif