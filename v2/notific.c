// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <share/neuron_driver_shared.h>

#include "notific.h"

#define NOTIFIC_NQ_SIZE 0x28   // total size of the NQ register space
#define NOTIFIC_NQ_HEAD_OFFSET 0x10c

static u64 seng_sdma_base[V2_MMAP_TPB_COUNT][V2_NUM_DMA_ENGINES_PER_TPB] = {
	{ V2_APB_SENG_0_SDMA_0_BASE, V2_APB_SENG_0_SDMA_1_BASE, V2_APB_SENG_0_SDMA_2_BASE,
		V2_APB_SENG_0_SDMA_3_BASE, V2_APB_SENG_0_SDMA_4_BASE, V2_APB_SENG_0_SDMA_5_BASE,
		V2_APB_SENG_0_SDMA_6_BASE, V2_APB_SENG_0_SDMA_7_BASE, V2_APB_SENG_0_SDMA_8_BASE,
		V2_APB_SENG_0_SDMA_9_BASE, V2_APB_SENG_0_SDMA_10_BASE, V2_APB_SENG_0_SDMA_11_BASE,
		V2_APB_SENG_0_SDMA_12_BASE, V2_APB_SENG_0_SDMA_13_BASE,
		V2_APB_SENG_0_SDMA_14_BASE, V2_APB_SENG_0_SDMA_15_BASE },
	{ V2_APB_SENG_1_SDMA_0_BASE, V2_APB_SENG_1_SDMA_1_BASE, V2_APB_SENG_1_SDMA_2_BASE,
		V2_APB_SENG_1_SDMA_3_BASE, V2_APB_SENG_1_SDMA_4_BASE, V2_APB_SENG_1_SDMA_5_BASE,
		V2_APB_SENG_1_SDMA_6_BASE, V2_APB_SENG_1_SDMA_7_BASE, V2_APB_SENG_1_SDMA_8_BASE,
		V2_APB_SENG_1_SDMA_9_BASE, V2_APB_SENG_1_SDMA_10_BASE, V2_APB_SENG_1_SDMA_11_BASE,
		V2_APB_SENG_1_SDMA_12_BASE, V2_APB_SENG_1_SDMA_13_BASE,
		V2_APB_SENG_1_SDMA_14_BASE, V2_APB_SENG_1_SDMA_15_BASE }
};


u64 notific_get_relative_offset_sdma_v2(int nc_id, int eng_id)
{
	const u64 seng_sdma_relbase = seng_sdma_base[nc_id][eng_id] - V2_APB_BASE;
	u64 sdma_base = V2_PCIE_BAR0_APB_OFFSET + seng_sdma_relbase;
	return sdma_base + V2_APB_SENG_0_SDMA_0_NOTIFIC_RELBASE;
}

static int notific_decode_sdma_nq_head_reg_access(u64 offset, u8 nc_id, u32 *nq_type, u8 *nq_id)
{
	u64 sdma_start_offset = seng_sdma_base[nc_id][0];
	u64 sdma_block_size = seng_sdma_base[nc_id][1] - sdma_start_offset;
	u64 sdma_relative_offset = offset - sdma_start_offset;
	u64 nq_relative_offset;

	*nq_type = NQ_TYPE_TRACE_DMA;
	*nq_id = sdma_relative_offset / sdma_block_size;

	sdma_relative_offset %= sdma_block_size;
	if (sdma_relative_offset < NOTIFIC_NQ_HEAD_OFFSET)
		return -EINVAL;
	nq_relative_offset = sdma_relative_offset - NOTIFIC_NQ_HEAD_OFFSET;
	if (nq_relative_offset % NOTIFIC_NQ_SIZE)
		return -EINVAL;

	return 0;
}

static int notific_decode_nc_nq_head_reg_access(u64 offset, u8 nc_id, u32 *nq_type, u8 *nq_id)
{
	u64 nc_start_offset = notific_get_relative_offset_v2(nc_id);
	u64 nq_relative_offset = offset - nc_start_offset;
	int nq_instance;

	if (nq_relative_offset < NOTIFIC_NQ_HEAD_OFFSET)
		return -EINVAL;
	nq_relative_offset -= NOTIFIC_NQ_HEAD_OFFSET;
	if (nq_relative_offset % NOTIFIC_NQ_SIZE)
		return -EINVAL;
	nq_instance = nq_relative_offset / NOTIFIC_NQ_SIZE;
	*nq_type = nq_instance / V2_MAX_NQ_QUEUES;
	*nq_id = nq_instance % V2_MAX_NQ_QUEUES;
	return 0;
}

static int notific_decode_topsp_nq_head_reg_access(u64 offset, u8 topsp_id, u32 *nq_type, u8 *nq_id)
{
	u64 start_offset = notific_get_relative_offset_topsp_v2(topsp_id);
	u64 nq_relative_offset = offset - start_offset;
	int nq_instance;

	if (nq_relative_offset < NOTIFIC_NQ_HEAD_OFFSET)
		return -EINVAL;
	nq_relative_offset -= NOTIFIC_NQ_HEAD_OFFSET;
	if (nq_relative_offset % NOTIFIC_NQ_SIZE)
		return -EINVAL;
	nq_instance = nq_relative_offset / NOTIFIC_NQ_SIZE;
	*nq_type = nq_instance / V2_MAX_NQ_QUEUES;
	*nq_id = nq_instance % V2_MAX_NQ_QUEUES;
	return 0;
}

int notific_decode_nq_head_reg_access_v2(u64 offset, u8 *nc_id, u32 *nq_type, u8 *instance, bool *is_top_sp)
{
	int i;

	*is_top_sp = false;

	for (i = 0; i < V2_MMAP_TPB_COUNT; i++) {
		u64 start_offset = seng_sdma_base[i][0];
		u64 dma_block_size = seng_sdma_base[i][1] - start_offset;
		u64 end_offset = seng_sdma_base[i][V2_NUM_DMA_ENGINES_PER_TPB-1] + dma_block_size;
		if (offset >= start_offset && offset < end_offset) {
			*nc_id = i;
			return notific_decode_sdma_nq_head_reg_access(offset, i, nq_type, instance);
		}
	}

	for (i = 0; i < V2_MMAP_TPB_COUNT; i++) {
		u64 start_offset = notific_get_relative_offset_v2(i);
		u64 end_offset = start_offset + V2_APB_SENG_0_TPB_NOTIFIC_SIZE;
		if (offset >= start_offset && offset < end_offset) {
			*nc_id = i;
			return notific_decode_nc_nq_head_reg_access(offset, i, nq_type, instance);
		}
	}

	for (i = 0; i < V2_TS_PER_DEVICE; i++) {
		u64 start_offset = notific_get_relative_offset_topsp_v2(i);
		u64 end_offset = start_offset + V2_APB_IOFAB_TOP_SP_0_SIZE;
		if (offset >= start_offset && offset < end_offset) {
			*nc_id = i;
			*is_top_sp = true;
			return notific_decode_topsp_nq_head_reg_access(offset, i, nq_type, instance);
		}
	}

	return -EINVAL;
}
