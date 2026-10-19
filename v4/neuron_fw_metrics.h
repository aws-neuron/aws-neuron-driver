// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * v4-specific firmware metrics; shared code lives in ../neuron_fw_metrics_common.h.
 */

#ifndef NEURON_FW_METRICS_V4_H
#define NEURON_FW_METRICS_V4_H

// highest misc-ram version whose metrics this driver parses on v4 (sent via SET_MAX_VERSION)
#define V4_FW_METRICS_MAX_MISC_RAM_VERSION 0x15

void fw_metrics_init_reader_v4(void);

#endif

