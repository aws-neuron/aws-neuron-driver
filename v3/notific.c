// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <share/neuron_driver_shared.h>

#include "notific.h"

#define NOTIFIC_NQ_HEAD_OFFSET 0x10c

static u64 get_sdma_misc_base(int nc_id, int eng_id)
{
	int seng_id = nc_id / V3_NC_PER_SENG;
	const u64 sdma_bases[V3_SENG_PER_DEVICE] = {
		V3_APB_SE_0_SDMA_0_BASE,
		V3_APB_SE_1_SDMA_0_BASE,
		V3_APB_SE_2_SDMA_0_BASE,
		V3_APB_SE_3_SDMA_0_BASE
	};
	int seng_eng_id = eng_id + ((nc_id % V3_NC_PER_SENG) * V3_DMA_ENG_PER_NC);
	return sdma_bases[seng_id] + seng_eng_id * V3_APB_SDMA_DIST + V3_APB_SDMA_MISC_OFFSET;
}


u64 notific_get_relative_offset_sdma_v3(int nc_id, int eng_id)
{
	const u64 se_bases[V3_SENG_PER_DEVICE] = {
		V3_APB_SE_0_BASE,
		V3_APB_SE_1_BASE,
		V3_APB_SE_2_BASE,
		V3_APB_SE_3_BASE
	};
	uint8_t seng_id = nc_id / V3_NC_PER_SENG;
	const u64 seng_sdma_misc_relbase = get_sdma_misc_base(nc_id, eng_id) - se_bases[seng_id];
	return V3_PCIE_BAR0_APB_SE_0_OFFSET +
		   V3_PCIE_BAR0_APB_SE_DIST * seng_id +
		   seng_sdma_misc_relbase +
		   V3_APB_SENG_0_SDMA_0_NOTIFIC_RELBASE;
}

static int notific_decode_sdma_nq_head_reg_access_v3(u64 offset, u8 nc_id, u32 *nq_type, u8 *nq_id)
{
	u64 sdma_start_offset = get_sdma_misc_base(nc_id, 0);
	u64 sdma_block_size = V3_APB_SDMA_DIST;
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

static int notific_decode_nc_nq_head_reg_access_v3(u64 offset, u8 nc_id, u32 *nq_type, u8 *nq_id)
{
	u64 nc_start_offset = notific_get_relative_offset_v3(nc_id);
	u64 nq_relative_offset = offset - nc_start_offset;
	int nq_instance;

	if (nq_relative_offset < NOTIFIC_NQ_HEAD_OFFSET)
		return -EINVAL;
	nq_relative_offset -= NOTIFIC_NQ_HEAD_OFFSET;
	if (nq_relative_offset % NOTIFIC_NQ_SIZE)
		return -EINVAL;
	nq_instance = nq_relative_offset / NOTIFIC_NQ_SIZE;
	*nq_type = nq_instance / V3_MAX_NQ_QUEUES;
	*nq_id = nq_instance % V3_MAX_NQ_QUEUES;
	return 0;
}

static int notific_decode_topsp_nq_head_reg_access_v3(u64 offset, u8 topsp_id, u32 *nq_type, u8 *nq_id)
{
	u64 start_offset = notific_get_relative_offset_topsp_v3(topsp_id);
	u64 nq_relative_offset = offset - start_offset;
	int nq_instance;

	if (nq_relative_offset < NOTIFIC_NQ_HEAD_OFFSET)
		return -EINVAL;
	nq_relative_offset -= NOTIFIC_NQ_HEAD_OFFSET;
	if (nq_relative_offset % NOTIFIC_NQ_SIZE)
		return -EINVAL;
	nq_instance = nq_relative_offset / NOTIFIC_NQ_SIZE;
	*nq_type = nq_instance / V3_MAX_NQ_QUEUES;
	*nq_id = nq_instance % V3_MAX_NQ_QUEUES;
	return 0;
}

int notific_decode_nq_head_reg_access_v3(u64 offset, u8 *nc_id, u32 *nq_type, u8 *instance, bool *is_top_sp)
{
	int i;

	*is_top_sp = false;

	for (i = 0; i < V3_MMAP_TPB_COUNT; i++) {
		u64 start_offset = get_sdma_misc_base(i, 0);
		u64 dma_block_size = V3_APB_SDMA_DIST;
		u64 end_offset = get_sdma_misc_base(i, V3_NUM_DMA_ENGINES_PER_TPB-1) + dma_block_size;
		if (offset >= start_offset && offset < end_offset) {
			*nc_id = i;
			return notific_decode_sdma_nq_head_reg_access_v3(offset, i, nq_type, instance);
		}
	}

	for (i = 0; i < V3_MMAP_TPB_COUNT; i++) {
		u64 start_offset = notific_get_relative_offset_v3(i);
		u64 end_offset = start_offset + V3_APB_IO_0_SE_0_TPB_NOTIFIC_SIZE;
		if (offset >= start_offset && offset < end_offset) {
			*nc_id = i;
			return notific_decode_nc_nq_head_reg_access_v3(offset, i, nq_type, instance);
		}
	}

	for (i = 0; i < V3_TS_PER_DEVICE; i++) {
		u64 start_offset = notific_get_relative_offset_topsp_v3(i);
		u64 end_offset = start_offset + V3_APB_IO_0_USER_IO_TOP_SP_0_SIZE;
		if (offset >= start_offset && offset < end_offset) {
			*nc_id = i;
			*is_top_sp = true;
			return notific_decode_topsp_nq_head_reg_access_v3(offset, i, nq_type, instance);
		}
	}

	return -EINVAL;
}
