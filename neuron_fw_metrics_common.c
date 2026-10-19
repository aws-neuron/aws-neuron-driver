// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * Shared helpers for the firmware HBM layout metric payloads
 * (see neuron_fw_metrics_common.h).
 */

#include <linux/kernel.h>

#include "neuron_fw_metrics_common.h"

// both guards come from one cache snapshot rather than a fenced tail-then-head
// HBM read, so a write landing inside the ~us bulk read can pass.  Accepted:
// rare against a ~2Hz writer, and it costs one 50ms sample
bool fw_metrics_gen_guard_ok(const u8 *payload, u32 length)
{
	return length >= 2 && payload[0] == payload[length - 1];
}

