// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <share/neuron_driver_shared.h>
#include "putils.h"

#define PU_ERROR_NOTIFICATION_HEAD_OFFSET 0x108c
#define PU_EVENT_NOTIFICATION_HEAD_OFFSET 0x10ac
#define PU_EXPL_NOTIFICATION_HEAD_OFFSET_START 0xc14
#define PU_IMPL_NOTIFICATION_HEAD_OFFSET_START 0xc94

#define PU_IMPL_NOTIFICATION_OFFSET_TO_INSTANCE(offset) ((offset) / PU_INSTANCE_SIZE)
#define PU_IMPL_NOTIFICATION_OFFSET_TO_QUEUE(offset) 	\
	(((offset) % PU_INSTANCE_SIZE) / PU_IMPL_NOTIFICATION_QUEUE_SIZE)

#define PU_EXPL_NOTIFICATION_OFFSET_TO_INSTANCE(offset) ((offset) / PU_INSTANCE_SIZE)
#define PU_EXPL_NOTIFICATION_OFFSET_TO_QUEUE(offset)	\
	(((offset) % PU_INSTANCE_SIZE) / PU_EXPL_NOTIFICATION_QUEUE_SIZE)

#define PU_EXPL_NOTIFICATION_HEAD_CSR_START (PU_EXPL_NOTIFICATION_HEAD_OFFSET_START + PU_EXPL_NOTIFICATION_OFFSET(0, 0))
#define PU_EXPL_NOTIFICATION_HEAD_CSR_END (PU_EXPL_NOTIFICATION_HEAD_OFFSET_START + PU_EXPL_NOTIFICATION_OFFSET(4, 0))

#define PU_IMPL_NOTIFICATION_HEAD_CSR_START (PU_IMPL_NOTIFICATION_HEAD_OFFSET_START + PU_IMPL_NOTIFICATION_OFFSET(0, 0))
#define PU_IMPL_NOTIFICATION_HEAD_CSR_END (PU_IMPL_NOTIFICATION_HEAD_OFFSET_START + PU_IMPL_NOTIFICATION_OFFSET(4, 0))

int pu_decode_nq_head_reg_access(u64 offset, u8 *nc_id, u32 *nq_type, u8 *instance)
{
	u64 putils_offset, putils_start_offset, putils_end_offset;
	u64 block_size = P_0_APB_NC_1_PUTILS_RELBASE - P_0_APB_NC_0_PUTILS_RELBASE;
	u8 queue;

	// check if the register falls within putils blocks
	putils_start_offset = pu_get_relative_offset(0);
	putils_end_offset = pu_get_relative_offset(V1_NC_PER_DEVICE);
	if (offset < putils_start_offset || offset >= putils_end_offset)
		return -EINVAL;

	*nc_id = (offset - pu_get_relative_offset(0)) / block_size;
	putils_offset = offset - pu_get_relative_offset(*nc_id);

	// for events and errors, there is only one instance.
	*instance = 0;
	// check if the register is pointing to head of event or error notification queue
	if (putils_offset == PU_EVENT_NOTIFICATION_HEAD_OFFSET) {
		*nq_type = NQ_TYPE_EVENT;
		return 0;
	} else if (putils_offset == PU_ERROR_NOTIFICATION_HEAD_OFFSET) {
		*nq_type = NQ_TYPE_ERROR;
		return 0;
	}

	// check if the register fall within IMPLICIT notifications(traces)
	if (putils_offset >= PU_IMPL_NOTIFICATION_HEAD_CSR_START &&
	    putils_offset < PU_IMPL_NOTIFICATION_HEAD_CSR_END) {
		*nq_type = NQ_TYPE_TRACE;
		*instance = PU_IMPL_NOTIFICATION_OFFSET_TO_INSTANCE(putils_offset - PU_IMPL_NOTIFICATION_HEAD_OFFSET_START);
		queue = PU_IMPL_NOTIFICATION_OFFSET_TO_QUEUE(putils_offset - PU_IMPL_NOTIFICATION_HEAD_OFFSET_START);
		u64 expected_off = PU_IMPL_NOTIFICATION_OFFSET(*instance, queue) +
				   PU_IMPL_NOTIFICATION_HEAD_OFFSET_START;
		if (expected_off == putils_offset && queue == 0 && *instance <= 4)
			return 0;
	}

	// check if the register fall within EXPLICIT notifications
	if (putils_offset >= PU_EXPL_NOTIFICATION_HEAD_CSR_START &&
	    putils_offset <= PU_EXPL_NOTIFICATION_HEAD_CSR_END) {
		*nq_type = NQ_TYPE_NOTIFY;
		*instance = PU_EXPL_NOTIFICATION_OFFSET_TO_INSTANCE(putils_offset - PU_EXPL_NOTIFICATION_HEAD_OFFSET_START);
		queue = PU_EXPL_NOTIFICATION_OFFSET_TO_QUEUE(putils_offset - PU_EXPL_NOTIFICATION_HEAD_OFFSET_START);
		u64 expected_off = PU_EXPL_NOTIFICATION_OFFSET(*instance, queue) + PU_EXPL_NOTIFICATION_HEAD_OFFSET_START;
		if (expected_off == putils_offset && queue == 0 && *instance <= 4)
			return 0;
	}

	// the offset is not for NQ head pointer.
	return -EINVAL;
}
