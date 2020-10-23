// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019-2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef __FWIO_H__
#define __FWIO_H__

#include <linux/types.h>

#include "address_map.h"

/**
 * fw_io_setup() - Setup new FWIO for given device.
 *
 * @device_index: Device index for which fwio context needs to be created
 * @bar0: BAR0 virtual address
 * @bar0_size: Size of BAR0
 * @bar2: BAR2 virtual address
 * @bar2_size: Size of BAR2
 *
 * Return: fwio context on success, NULL on failure.
 */
struct fw_io_ctx *fw_io_setup(int device_index, void __iomem *bar0, u64 bar0_size,
			      void __iomem *bar2, u64 bar2_size);

/**
 * fw_io_destroy() - Removes previously setup FWIO.
 *
 * @ctx: fwio context
 */
void fw_io_destroy(struct fw_io_ctx *ctx);

/**
 * fw_io_post_metric() - Post given block data as metric to FWIO
 *
 * @ctx: fwio context
 * @data: data to post
 * @size: size of the data
 *
 * Return: 0 if metric is successfully posted, a negative error code otherwise.
 */
int fw_io_post_metric(struct fw_io_ctx *ctx, u8 *data, u32 size);

/**
 * fw_io_read_csr_array() - Read CSR(s) and return the value(s).
 *
 * @ptrs: Array of register address to read
 * @values: Read values stored here
 * @num_csrs: Number of CSRs to read
 *
 * Return: 0 if CSR read is successful, a negative error code otherwise.
 */
int fw_io_read_csr_array(void **ptrs, u32 *values, u32 num_csrs);

/**
 * fw_io_initiate_reset() - Initiate device local reset.
 *
 * @bar0: Device's BAR0 base address
 */
void fw_io_initiate_reset(void __iomem *bar0);

/**
 * fw_io_is_reset_initiated() - Check if local reset is initiated or not.
 *
 * @bar0: Device's BAR0 base address
 *
 * Return: true if reset is initiated, false if reset is not yet started.
 */
bool fw_io_is_reset_initiated(void __iomem *bar0);

/**
 * fw_io_is_device_ready() - Checks if the device is ready.
 *
 * @bar0 - Device's BAR0 base address
 *
 * Return: true if device is ready, false if device is still coming out of reset.
 */
bool fw_io_is_device_ready(void __iomem *bar0);

#define MAX_CONNECTED_DEVICES 16
/**
 * fw_io_topology() - Discovers devices connected to the given device.
 *
 * @ctx: FWIO context of the device for which topology
 * @device_ids:  Connected device IDs are stored here.
 * @count: Number of devices connected to the given device.
 *
 * Return: 0 on success.
 *
 */
int fw_io_topology(struct fw_io_ctx *ctx, u32 *device_ids, int *count);

/**
 * fw_io_read_counters() - Reads hardware counters
 *
 * @ctx - FWIO context of the device for which counters are read.
 * @addr_in: hardware counter addresses to read
 * @val_out: counters values
 * @num_ctrs: number of counters to read
 *
 * Return: 0 on success.
 *
 */
int fw_io_read_counters(struct fw_io_ctx *ctx, uint64_t addr_in[], uint32_t val_out[],
			uint32_t num_counters);

#endif
