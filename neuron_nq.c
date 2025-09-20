// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Notification Queues:
 *
 * As the engines execute instructions they produce messages in notification queue.
 * These messages are used by applications for monitoring completion of program and
 * also for profiling the program.
 *
 * Notification queue is a circular buffer in host memory - hardware writes to the buffer and
 * applications consumes it by memory mapping the area.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>

#include "neuron_mempool.h"
#include "neuron_mmap.h"
#include "neuron_topsp.h"
#include "neuron_nq.h"
#include "neuron_dhal.h"

static int nnq_halt(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type)
{
	u8 nq_id;

	if (nd == NULL || nc_id >= ndhal->ndhal_address_map.nc_per_device ||
		((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0)
		return -EINVAL;

	nq_id = ndhal->ndhal_nq.nnq_get_nqid(nd, nc_id, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	if (nd->nq_mc[nc_id][nq_id] == NULL) {
		return 0;
	}

	ndhal->ndhal_nq.nnq_set_hwaddr(nd, nc_id, eng_index, nq_type, 0, 0);
	
	return 0;
}

static int nnq_destroy(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type)
{
	u8 nq_id;

	if (nd == NULL || nc_id >= ndhal->ndhal_address_map.nc_per_device ||
		((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0)
		return -EINVAL;

	nq_id = ndhal->ndhal_nq.nnq_get_nqid(nd, nc_id, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	if (nd->nq_mc[nc_id][nq_id] == NULL) {
		return 0;
	}

	mc_free(&nd->nq_mc[nc_id][nq_id]);

	return 0;
}


int nnq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size,
	     u32 on_host_memory, u32 dram_channel, u32 dram_region, bool force_alloc_mem,
	     struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	// Check that size is power of 2
	if (size & (size - 1)) {
		pr_err("notification ring size must be power of 2");
		return -EINVAL;
	}
	if (nd == NULL || nc_id >= ndhal->ndhal_address_map.nc_per_device ||
		((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0)
		return -EINVAL;

	u8 nq_id = ndhal->ndhal_nq.nnq_get_nqid(nd, nc_id, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	struct mem_chunk *mc = nd->nq_mc[nc_id][nq_id];
	if (mc == NULL || force_alloc_mem) {
		struct mem_chunk *_mc = NULL;
		int ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, size, (on_host_memory) ? 0 : size, on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
			       dram_channel, dram_region, nc_id, on_host_memory ? NEURON_MEMALLOC_TYPE_NOTIFICATION_HOST : NEURON_MEMALLOC_TYPE_NOTIFICATION_DEVICE, &_mc);
		if (ret)
			return ret;
		ndhal->ndhal_nq.nnq_set_hwaddr(nd, nc_id, eng_index, nq_type, size, _mc->pa);
		nd->nq_mc[nc_id][nq_id] = _mc;
		if (mc) {
			mc_free(&mc);
		}
		mc = _mc;
	}
	if (mc->mem_location == MEM_LOC_HOST)
		*mmap_offset = nmmap_offset(mc);
	else
		*mmap_offset = -1;
	*nq_mc = mc;

	return 0;
}

void nnq_destroy_nc(struct neuron_device *nd, u8 nc_id)
{
	u8 eng_index;
	u8 nq_type;
	u8 ts_id;

	for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			nnq_halt(nd, nc_id, eng_index, nq_type);
		}
	}

	// wait for halted notific queues to drain
	msleep(1);

	for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			nnq_destroy(nd, nc_id, eng_index, nq_type);
		}
	}

	u8 ts_per_nc = ndhal->ndhal_address_map.ts_per_device / ndhal->ndhal_address_map.nc_per_device;
	for (ts_id = nc_id * ts_per_nc; ts_id < (nc_id + 1) * ts_per_nc; ts_id++) {
		ndhal->ndhal_topsp.ts_nq_destroy_one(nd, ts_id);
	}
}

void nnq_destroy_all(struct neuron_device *nd)
{
	u8 nc_id;

	for (nc_id = 0; nc_id < ndhal->ndhal_address_map.nc_per_device; nc_id++) {
		if (((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0) {
			continue;
		}
		nnq_destroy_nc(nd, nc_id);
	}
}
