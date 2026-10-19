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
	if (nq_id >= ndhal->ndhal_address_map.nq_per_nc)
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
	if (nq_id >= ndhal->ndhal_address_map.nq_per_nc)
		return -EINVAL;

	if (nd->nq_mc[nc_id][nq_id] == NULL) {
		return 0;
	}

	mc_free(&nd->nq_mc[nc_id][nq_id]);

	return 0;
}

void nnq_destroy_storage(struct neuron_device *nd)
{
	int i;

	if (!nd)
		return;

	for (i = 0; i < MAX_NC_PER_DEVICE; i++) {
		kfree(nd->nq_mc[i]);
		nd->nq_mc[i] = NULL;
	}
	for (i = 0; i < MAX_TS_PER_DEVICE; i++) {
		kfree(nd->ts_nq_mc[i]);
		nd->ts_nq_mc[i] = NULL;
	}
}

int nnq_init_storage(struct neuron_device *nd)
{
	u16 sz = ndhal->ndhal_address_map.nq_per_nc;
	int i;

	if (sz == 0) {
		pr_err("ndhal->ndhal_address_map.nq_per_nc is 0; ndhal not initialized\n");
		return -EINVAL;
	}

	for (i = 0; i < MAX_NC_PER_DEVICE; i++) {
		nd->nq_mc[i] = kcalloc(sz, sizeof(struct mem_chunk *), GFP_KERNEL);
		if (!nd->nq_mc[i])
			goto fail;
	}
	for (i = 0; i < MAX_TS_PER_DEVICE; i++) {
		nd->ts_nq_mc[i] = kcalloc(sz, sizeof(struct mem_chunk *), GFP_KERNEL);
		if (!nd->ts_nq_mc[i])
			goto fail;
	}
	return 0;
fail:
	nnq_destroy_storage(nd);
	return -ENOMEM;
}


int nnq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size,
	     u32 on_host_memory, u32 hbm_index, bool force_alloc_mem,
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
	if (nq_id >= ndhal->ndhal_address_map.nq_per_nc)
		return -EINVAL;

	struct mem_chunk *mc = nd->nq_mc[nc_id][nq_id];
	if (mc == NULL || force_alloc_mem) {
		struct mem_chunk *_mc = NULL;
		int ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, size, (on_host_memory) ? 0 : size, on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
			       hbm_index, nc_id, on_host_memory ? NEURON_MEMALLOC_TYPE_NOTIFICATION_HOST : NEURON_MEMALLOC_TYPE_NOTIFICATION_DEVICE, &_mc);
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
	u8 nq_queue_count = ndhal->ndhal_nq.nnq_get_nq_queue_count();

	for (eng_index = 0; eng_index < nq_queue_count; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			nnq_halt(nd, nc_id, eng_index, nq_type);
		}
	}

	// wait for halted notific queues to drain
	msleep(1);

	for (eng_index = 0; eng_index < nq_queue_count; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			nnq_destroy(nd, nc_id, eng_index, nq_type);
		}
	}

	u8 ts_per_nc = ndhal->ndhal_address_map.ts_per_device / ndhal->ndhal_address_map.nc_per_device;
	for (ts_id = nc_id * ts_per_nc; ts_id < (nc_id + 1) * ts_per_nc; ts_id++) {
		ndhal->ndhal_topsp.ts_nq_destroy_one(nd, ts_id);
	}
}

static void nnq_h2d_destroy(struct neuron_device *nd, pid_t owner)
{
	int h2d_nqid;
	if (nd == NULL)
		return;

	mutex_lock(&nd->lock);
	// assert nqs are quiesced before freeing memchunks
	// quiesced by writing disable reg and reading back value
	for (h2d_nqid = 0; h2d_nqid < NUM_H2D_DMA_PER_DEVICE; h2d_nqid++) {
		if (nd->h2d_nq_mc[h2d_nqid] == NULL)
			continue;
		if (owner != 0 && nd->h2d_nq_mc[h2d_nqid]->pid != owner)
			continue;

		uint32_t h2d_engine_id = ndhal->ndhal_nq.nnq_get_h2d_id(h2d_nqid);
		ndhal->ndhal_nq.nnq_set_nq_h2d_enable(nd, h2d_engine_id, NOTIFIC_NQ_ENABLE_RESET_VALUE);
		ndhal->ndhal_nq.nnq_set_h2d_hwaddr(nd, h2d_engine_id, 0, 0);
	}

	for (h2d_nqid = 0; h2d_nqid < NUM_H2D_DMA_PER_DEVICE; h2d_nqid++) {
		if (nd->h2d_nq_mc[h2d_nqid] == NULL)
			continue;
		if (owner != 0 && nd->h2d_nq_mc[h2d_nqid]->pid != owner)
			continue;

		uint32_t h2d_engine_id = ndhal->ndhal_nq.nnq_get_h2d_id(h2d_nqid);
		u32 enable_val = 0xdeadbeef;
		ndhal->ndhal_nq.nnq_get_nq_h2d_enable(nd, h2d_engine_id, &enable_val);
		if (enable_val != NOTIFIC_NQ_ENABLE_RESET_VALUE) {
			pr_warn("Failed to disable H2D Notification Queue %u (enable=0x%x). Cleaning Notification queue without disabling", h2d_engine_id, enable_val);
		}
		mc_free(&nd->h2d_nq_mc[h2d_nqid]);
		nd->h2d_nq_mc[h2d_nqid] = NULL;
	}
	mutex_unlock(&nd->lock);

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
	nnq_h2d_destroy(nd, 0);
}

int nnq_h2d_init(struct neuron_device *nd, uint32_t h2d_eng_id, u32 nc_id,
		 u32 size, u32 on_host_memory, u32 hbm_index,
		 bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	int h2d_nqid, ret;
	struct mem_chunk *mc;
	pid_t owner = task_tgid_nr(current);

	if (size & (size - 1)) {
		pr_err("notification ring size must be power of 2\n");
		return -EINVAL;
	}

	if (!ndhal->ndhal_nq.nnq_set_h2d_hwaddr)
		return -ENOSYS;

	h2d_nqid = ndhal->ndhal_nq.nnq_get_nq_h2d_id(h2d_eng_id);
	if (h2d_nqid < 0 || h2d_nqid >= NUM_H2D_DMA_PER_DEVICE)
		return -EINVAL;

	mutex_lock(&nd->lock);

	if (nd->h2d_nq_mc[h2d_nqid] != NULL && nd->h2d_nq_mc[h2d_nqid]->pid != owner) {
		mutex_unlock(&nd->lock);
		return -EBUSY;
	}

	mc = nd->h2d_nq_mc[h2d_nqid];
	if (mc == NULL || force_alloc_mem) {
		struct mem_chunk *_mc = NULL;

		ret = mc_alloc_align(nd, MC_LIFESPAN_CUR_PROCESS, size, on_host_memory ? 0 : size,
					 on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
					 hbm_index, nc_id,
					 on_host_memory ? NEURON_MEMALLOC_TYPE_NOTIFICATION_HOST
							: NEURON_MEMALLOC_TYPE_NOTIFICATION_DEVICE,
					 &_mc);
		if (ret) {
			mutex_unlock(&nd->lock);
			return ret;
		}

		ret = ndhal->ndhal_nq.nnq_set_h2d_hwaddr(nd, h2d_eng_id, size, _mc->pa);
		if (ret) {
			mc_free(&_mc);
			mutex_unlock(&nd->lock);
			return ret;
		}

		if (mc)
			mc_free(&mc);
		nd->h2d_nq_mc[h2d_nqid] = _mc;
		mc = _mc;
	}

	*mmap_offset = (mc->mem_location == MEM_LOC_HOST) ? nmmap_offset(mc) : (u64)-1;
	*nq_mc = mc;

	mutex_unlock(&nd->lock);
	return 0;
}

void nnq_h2d_handle_process_exit(struct neuron_device *nd)
{
	pid_t owner = task_tgid_nr(current);
	nnq_h2d_destroy(nd, owner);
}

