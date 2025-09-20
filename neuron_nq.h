// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_NQ_H
#define NEURON_NQ_H

#include <linux/kernel.h>
#include <linux/types.h>
#include "neuron_device.h"

/**
 * nnq_init() - Initialize notification queue for NeuronCore
 *
 * @nd: neuron device
 * @nc_id: core index in the device
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @size: size of queue in bytes
 * @on_host_memory: if true, NQ is created in host memory
 * @dram_channel: If NQ is created on device memory which DRAM channel to use.
 * @dram_region: If NQ is created on device memory which DRAM region to use.
 * @force_alloc_mem: If true, force allocate new memory (and delete already allocated memory, if any)
 * @nq_mc[out]: memchunk used by the NQ will be written here
 * @mc_ptr[out]: Pointer to memchunk backing this NQ
 *
 * Return: 0 on if initialization succeeds, a negative error code otherwise.
 */
int nnq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size,
	       u32 on_host_memory, u32 dram_channel, u32 dram_region,
	       bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset);

/**
 * nnq_destroy_nc() - Disable notification in the device
 *
 * @nd: neuron device
 * @nc_id: neuron core
 *
 */
void nnq_destroy_nc(struct neuron_device *nd, u8 nc_id);

/**
 * nnq_destroy_all() - Disable notification in the device
 *
 * @nd: neuron device
 *
 */
void nnq_destroy_all(struct neuron_device *nd);

#endif