// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

/** TDMA block is a wrapper around the UDMA block and provides Read Reorder Buffer (ROB),
 *  Write Reorder Buffer (WOB) and Event acceleration APIs.
 */

#pragma once

#include "../neuron_reg_access.h"

/* tdma_rob_cfg:
 *        0:0 - en
 *        1:1 - force_inorder
 *        2:2 - clear
 *        3:3 - use_rid_base
 *        4:10 - rid_base
 *        31:31 - powerdown
 */
#define REG_TDMA_ROB_CFG_OFFSET 0x1c

/* tdma_wob_cfg:
 *        0:0 - en
 *        1:1 - force_inorder
 *        2:2 - clear
 *        3:3 - use_wid_base
 *        4:10 - wid_base
 */
#define REG_TDMA_WOB_CFG_OFFSET 0x20

/* 0:0 - en
 *
 * enables the write reorder buffer
 *
 */
#define REG_TDMA_WOB_CFG_EN_MASK 0x1

/* tdma_event_accel:
 *        0:0 - en
 */
#define REG_TDMA_EVENT_ACCEL_OFFSET 0x0

/** Initialize the TDMA.
 *
 * @param[in,out] tdma  - TDMA handle to initialize
 * @param[in] regs_base - MMIO base address of APB interface.
 * @param[in] eng_name  - Name of TDMA engine to initialize.
 *
 * @return 0            - Success.
 *         -EFAULT      - One of the parameter is invalid.
 *         -EINVAL      - Fail to initialize.
 */
static inline int tdma_init_engine(void __iomem *regs_base)
{
	if (regs_base == NULL)
		return -EFAULT;

	// enable ROB
	reg_write32(regs_base + REG_TDMA_ROB_CFG_OFFSET, 1);

	// WOB **must** be disabled on V1 because of a hw bug
	reg_write32(regs_base + REG_TDMA_WOB_CFG_OFFSET, ~REG_TDMA_WOB_CFG_EN_MASK);

	// enable event acceleration
	reg_write32(regs_base + REG_TDMA_EVENT_ACCEL_OFFSET, 1);

	return 0;
}
