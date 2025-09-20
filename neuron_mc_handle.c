
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/**
 * This module maps an memchunk to a user space memchunk handle.
 *    
 * The module employs a 2 level map to create an index to memchunk handle.
 *
 * Indices are allocated as follows: 
 * - the free list points to the next free index in the table/map.
 * - free entries are linked together through the table entries in lifo order.
 * - the last free entry in the list, which by definition must be the highest number index, point to 
 *   the invalid index (0). This informs the allocator that the next free index is that last free entry + 1. 
 *
 * The table is per neuron device and currently has a limit of 4M entries per device or 
 * 512K per core on a Trn2.
 *
 * The table is mutex protected for insert/remove but not for search.
 *
 * The table is expanded dynamically since we don't want to needlessly consume memory.
 *
 * The sanity checks in free probably aren't necessary, but will keep them in for the moment.
 *
 * Note:
 *    Never dump values from the table to the syslog, otherwise caller could use bad values to leak table data.  Only report handles
 *    In the future we may want to add nc_id to the handle and create separate maps per core to avoid lock contention
 *
 */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include "neuron_device.h"
#include "neuron_dhal.h"
#include "neuron_mc_handle.h"

// mc handle to mc  mapping table sizes
//
#define NMCH_L1_TBL_SZ 512
#define NMCH_L2_TBL_SZ 8192
#define NMCH_TBL_MAX_ENT (NMCH_L1_TBL_SZ * NMCH_L2_TBL_SZ)

// zero idx is invalid.
// 
#define NMCH_INVALID_IDX 0

#define NMCH_IDX_2L1_IDX(i) (i / NMCH_L2_TBL_SZ)
#define NMCH_IDX_2L2_IDX(i) (i % NMCH_L2_TBL_SZ)

static inline uint64_t nmch_handle_to_idx(neuron_mc_handle_t mc_handle) 
{
	return mc_handle;
}

static inline neuron_mc_handle_t nmch_idx_to_handle(uint64_t idx)
{
	return idx;
}

static inline bool nmch_l2_tbl_entry_valid(nmch_map_ent_t ent)
{
	// ent is a union, if the value is a small number it's a handle of the next free element, otherwise its a memchunk pointer.
	return (ent.value >= NMCH_TBL_MAX_ENT);
}

static inline bool nmch_idx_valid(uint64_t idx)
{
	return ((idx < NMCH_TBL_MAX_ENT) && (idx != NMCH_INVALID_IDX));
}

static inline bool nmch_service_is_down(struct neuron_device *nd)
{
	return (nd->nmch.free == NMCH_INVALID_IDX);
}

static inline void nmch_service_set_down(struct neuron_device *nd)
{
	nd->nmch.free = NMCH_INVALID_IDX;
}

static int nmch_l2tbl_alloc(nmch_map_ent_t **pl2_tbl)
{
	*pl2_tbl = (nmch_map_ent_t *)kzalloc(sizeof(nmch_map_ent_t) * NMCH_L2_TBL_SZ, GFP_KERNEL);
	if (*pl2_tbl == NULL) {
		pr_err("memory alloc failed for l2 mc handle map");
		return -1;
	}
	return 0;
}

struct mem_chunk* mc_handle_find(struct neuron_device *nd, neuron_mc_handle_t mc_handle)
{
	nmch_map_ent_t *l2_tbl;
	nmch_map_ent_t ent;
	uint64_t idx = nmch_handle_to_idx(mc_handle);
	
	if (nmch_service_is_down(nd)) {
		return NULL;
	}

	if (!nmch_idx_valid(idx)) {
		return NULL;
	}

	l2_tbl = nd->nmch.l1_tbl[NMCH_IDX_2L1_IDX(idx)];
	if (l2_tbl == NULL) {
		pr_err("nd%d: invalid handle %llx", nd->device_index, mc_handle);
		return NULL;
	}

	ent = l2_tbl[NMCH_IDX_2L2_IDX(idx)];

	if (!nmch_l2_tbl_entry_valid(ent)) {
		pr_err("nd%d: invalid handle %llx", nd->device_index, mc_handle);
		return NULL;
	}
	return ent.mc;
}

int nmch_handle_alloc(struct neuron_device *nd, struct mem_chunk *mc, neuron_mc_handle_t *mc_handle)
{
	int ret;
	uint64_t idx;
	uint64_t nextfree;
	nmch_map_ent_t *l2_tbl;
	nmch_map_ent_t *pent;

	mutex_lock(&nd->nmch.lock);

	if (nmch_service_is_down(nd)) {
		ret = -ENOENT;
		goto done;
	}

	idx = nd->nmch.free;

	// grab l2 table (that has already been preallocated)
	//
	l2_tbl = nd->nmch.l1_tbl[NMCH_IDX_2L1_IDX(idx)];
	pent = &(l2_tbl[NMCH_IDX_2L2_IDX(idx)]);
	nextfree = pent->value;

	// if the next free link is invalid, then this is a new entry and the next free is idx+1
	if (nextfree == NMCH_INVALID_IDX) {
		nmch_map_ent_t **pnextfree_l2_tbl;

		nextfree = idx+1;
		// if free entries have been exhausted, flag the service as broken
		if (nextfree >= NMCH_TBL_MAX_ENT) {
			pr_err("nd%d: memchunk handle map out of entries", nd->device_index);
			nmch_service_set_down(nd);
			ret = -ENOENT;
			goto done;
		}

		pnextfree_l2_tbl = &(nd->nmch.l1_tbl[NMCH_IDX_2L1_IDX(nextfree)]);

		// new table allocate
		if (*pnextfree_l2_tbl == NULL) {
			if (nmch_l2tbl_alloc(pnextfree_l2_tbl)) {
				nmch_service_set_down(nd);
				ret = -ENOENT;
				goto done;
			}
		}
	}
	
	// update free index
	nd->nmch.free = nextfree;

	// insert the mc 
	pent->mc = mc;
	*mc_handle = nmch_idx_to_handle(idx);
	ret = 0;

done:
	mutex_unlock(&nd->nmch.lock);
	return ret;
}

int nmch_handle_free(struct neuron_device *nd, neuron_mc_handle_t mc_handle)
{
	int ret;
	uint64_t idx;
	nmch_map_ent_t *l2_tbl;
	nmch_map_ent_t *pent;

	mutex_lock(&nd->nmch.lock);

	if (nmch_service_is_down(nd)) {
		ret = -ENOENT;
		goto done;
	}

	idx = nmch_handle_to_idx(mc_handle);

	// check if passed handle is bad
	if (!nmch_idx_valid(idx)) {
		ret = -ENOENT;
		goto done;
	}
	
	l2_tbl = nd->nmch.l1_tbl[NMCH_IDX_2L1_IDX(idx)];

	if (l2_tbl == NULL) {
		pr_err("nd%d: invalid handle %llx", nd->device_index, mc_handle);
		ret = -ENOENT;
		goto done;
	}
	
	pent = &(l2_tbl[NMCH_IDX_2L2_IDX(idx)]);

	// sanity check entry is valid so we're not screwing up the free list
	if (!nmch_l2_tbl_entry_valid(*pent)) {
		pr_err("nd%d: entry for memchunk handle is invalid %llx", nd->device_index, mc_handle);
		ret = -ENOENT;
		goto done;
	}

	pent->value = nd->nmch.free;
	nd->nmch.free = idx;
	ret = 0;	

done:
	mutex_unlock(&nd->nmch.lock);
	return ret;
}

int nmch_handle_init(struct neuron_device *nd)
{
	nd->nmch.l1_tbl = (nmch_map_ent_t **)kzalloc(sizeof(nmch_map_ent_t **) * NMCH_L1_TBL_SZ, GFP_KERNEL);

	if (( nd->nmch.l1_tbl == NULL) || (nmch_l2tbl_alloc(&nd->nmch.l1_tbl[0]))) {
		pr_err("nd%d:failed to initialize mc handle map", nd->device_index);
		nmch_service_set_down(nd);
		return -1;
	}

	mutex_init(&nd->nmch.lock);
	nd->nmch.free = 1;
	return 0;
}

void nmch_handle_cleanup(struct neuron_device *nd)
{
	int i;
	
	if (nd->nmch.l1_tbl != NULL) {
		for (i=0; i < NMCH_L1_TBL_SZ; i++) {
			if (nd->nmch.l1_tbl[i] != NULL) {
				kfree(nd->nmch.l1_tbl[i]);
			}
		}
		kfree(nd->nmch.l1_tbl);
	}
	nmch_service_set_down(nd);
}
