// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * Shared helpers for the metric payloads carried by the firmware HBM layout.
 * The protocol (neuron_fw_layout.c) never interprets payload bytes; the
 * parsers live per platform in vN/neuron_fw_metrics.c and build on these.
 */

#ifndef NEURON_FW_METRICS_COMMON_H
#define NEURON_FW_METRICS_COMMON_H

#include <linux/types.h>

/**
 * fw_metrics_gen_guard_ok() - check the u8 generation guards of a payload
 * @payload: payload bytes as copied out of the layout cache
 * @length: payload length in bytes
 *
 * Gen-guarded types carry the same generation byte first and last.  A mismatch
 * means the copy raced a firmware write: the parse fails for this tick.
 *
 * Return: true if the guards match.
 */
bool fw_metrics_gen_guard_ok(const u8 *payload, u32 length);

#endif

