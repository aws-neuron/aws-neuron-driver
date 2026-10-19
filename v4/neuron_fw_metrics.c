// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * v4-specific firmware metrics: the supported misc-ram version and the payload
 * parsers (shared helpers in ../neuron_fw_metrics_common.h).
 */

#include <linux/kernel.h>

#include "../neuron_dhal.h"
#include "neuron_fw_metrics.h"

// no payload schema is adopted on v4 yet
static int fw_metrics_parse_payload_v4(struct neuron_device *nd, u16 type_id, const u8 *payload, u32 length)
{
	return -ENOTSUPP;
}

void fw_metrics_init_reader_v4(void)
{
	ndhal->ndhal_fw_metrics.max_misc_ram_version = V4_FW_METRICS_MAX_MISC_RAM_VERSION;
	ndhal->ndhal_fw_metrics.parse_payload = fw_metrics_parse_payload_v4;
}

