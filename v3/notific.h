// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#pragma once

/** Neuron device can generate 4 type of notifications during program execution
 * 1. Errors - Any accelerator generated error such as infinity, NaN
 * 2. Events - Accelerator set or cleared an event.
 * 3. Explicit - Program had an instruction which explicitly generated notification.
 * 4. Implicit - If configured all instructions would generate notification.
 *
 * Each NeuronCore has one error and event notification queue and multiple
 * implicit and explicit notification queues.
 *
 * The notifications are stored in a circular buffer.
 * The following functions enables setting up the circular buffer.
 */

#include "address_map.h"
#include "../neuron_nq.h"
#include "../neuron_reg_access.h"

/** Returns NOTIFIC relative offset for given the DMA engine for given NC.
 */
u64 notific_get_relative_offset_sdma_v3(int nc_id, int eng_id);

/** Returns NOTIFIC relative offset for given the NC.
 */
static inline u64 notific_get_relative_offset_v3(int nc_idx)
{
	const u64 se_relbases[V3_SENG_PER_DEVICE] = {
		V3_APB_IO_0_SE_0_RELBASE,
		V3_APB_IO_0_SE_1_RELBASE,
		V3_APB_IO_1_SE_0_RELBASE,
		V3_APB_IO_1_SE_1_RELBASE
	};
	uint8_t seng_id = nc_idx / V3_NC_PER_SENG;
	u64 offset = se_relbases[seng_id];

	offset += V3_PCIE_BAR0_APB_IO_0_OFFSET +
			  (V3_PCIE_BAR0_APB_IO_DIST * (nc_idx / V3_NC_PER_DIE)) +
			  V3_APB_IO_0_SE_0_TPB_0_SIZE * (nc_idx % V3_NC_PER_SENG) +
			  V3_APB_IO_0_SE_0_TPB_TOP_RELBASE +
              V3_APB_IO_0_SE_0_TPB_TOP_NOTIFIC_RELBASE;
	return offset;
}

/** Returns NOTIFIC relative offset for given TOP_SP.
 */
static inline u64 notific_get_relative_offset_topsp_v3(int ts_idx)
{
	u64 offset = V3_PCIE_BAR0_APB_IO_0_OFFSET +
			(V3_PCIE_BAR0_APB_IO_DIST * (ts_idx / V3_TS_PER_DIE)) +
			V3_APB_IO_0_USER_IO_RELBASE +
            V3_APB_IO_0_USER_IO_TOP_SP_0_RELBASE +
            V3_APB_IO_0_USER_IO_TOP_SP_0_SIZE * (ts_idx % V3_TS_PER_DIE) +
            V3_APB_IO_0_USER_IO_TOP_SP_0_NOTIFIC_RELBASE;
	return offset;
}


int notific_decode_nq_head_reg_access_v3(u64 offset, u8 *nc_id, u32 *nq_type, u8 *instance,
				      bool *is_top_sp);

