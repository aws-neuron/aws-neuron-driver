// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * v3-specific firmware metrics; shared code lives in ../neuron_fw_metrics_common.h.
 */

#ifndef NEURON_FW_METRICS_V3_H
#define NEURON_FW_METRICS_V3_H

// highest misc-ram version whose metrics this driver parses on v3 (sent via SET_MAX_VERSION)
#define V3_FW_METRICS_MAX_MISC_RAM_VERSION 0x00 // none adopted yet

void fw_metrics_init_reader_v3(void);

#endif

