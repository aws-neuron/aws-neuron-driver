// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

/** SDMA block is a wrapper around the UDMA block and provides Read Reorder Buffer (ROB),
 *  Write Reorder Buffer (WOB) and Event acceleration APIs.
 */

#pragma once

#include "../neuron_reg_access.h"
#include "address_map.h"

/* sdma_rob_cfg:
 *        0:0 - en
 *        1:1 - force_inorder
 *        2:2 - clear
 *        3:3 - use_rid_base
 *        4:10 - rid_base
 *        31:31 - powerdown
 */
#define REG_SDMA_ROB_CFG_OFFSET 0x1c

/* sdma_wob_cfg:
 *        0:0 - en
 *        1:1 - force_inorder
 *        2:2 - clear
 *        3:3 - use_wid_base
 *        4:10 - wid_base
 */
#define REG_SDMA_WOB_CFG_OFFSET 0x20

/* 0:0 - en
 *
 * enables the write reorder buffer
 *
 */
#define REG_SDMA_WOB_CFG_EN_MASK 0x1

/* tdma_event_accel:
 *        0:0 - en
 */
#define REG_SDMA_EVENT_ACCEL_OFFSET 0x0

/* tdma_model_robert_txdf:
 *        0:9 - overhead_beats_outstanding
 */
#define REG_SDMA_MODEL_ROBERT_TXDF 0x800

/** Initialize the SDMA.
 *
 * @param[in,out] sdma  - SDMA handle to initialize
 * @param[in] regs_base - MMIO base address of APB interface.
 * @param[in] eng_name  - Name of TDMA engine to initialize.
 *
 * @return 0            - Success.
 *         -EFAULT      - One of the parameter is invalid.
 *         -EINVAL      - Fail to initialize.
 */
static inline int sdma_init_engine(void __iomem *regs_base)
{
	if (regs_base == NULL)
		return -EFAULT;

	// enable ROB
	reg_write32(regs_base + REG_SDMA_ROB_CFG_OFFSET, 1);

	// enable WOB
	reg_write32(regs_base + REG_SDMA_WOB_CFG_OFFSET, 1);

	// enable event acceleration
	reg_write32(regs_base + REG_SDMA_EVENT_ACCEL_OFFSET, 1);

	// adjust number of beats outstanding config for backpressure
	reg_write32(regs_base + REG_SDMA_MODEL_ROBERT_TXDF, 0x26);

	return 0;
}

#define AWS_REG_V3_TDMA_MODEL_TDMA_BROADCAST_CFG_GROUP_OFFSET 0x100
#define AWS_REG_V3_TDMA_MODEL_TDMA_BROADCAST_CFG_LAST_NODE_OFFSET 0x104

/** Initialize the Broadcast.
 *
 * @param[in] regs_base - MMIO base address of APB interface.
 * @param[in] eng_id    - eng id that needs to be initialized
 *
 * @return 0            - Success.
 *         -EFAULT      - One of the parameter is invalid.
 *         -EINVAL      - Fail to initialize.
 */
static inline int sdma_configure_broadcast_v3(void __iomem *regs_base, int eng_id)
{
	u16 mask = U16_MAX;
	u16 i;
	u16 clr_bit;
	bool is_last_node;
	u16 last_node;
	void __iomem *base = regs_base + V3_APB_SENG_0_SDMA_0_APP_RELBASE;

	for (i = 0; i <= eng_id; i++) {
		clr_bit = 1 << (16 - i);
		mask &= ~(clr_bit);
	}
	is_last_node = (eng_id == 0 || eng_id == 3 || eng_id == 6 || eng_id == 9 || eng_id == 15);
	last_node = (is_last_node) ? U16_MAX : 0;

	reg_write32(base + AWS_REG_V3_TDMA_MODEL_TDMA_BROADCAST_CFG_GROUP_OFFSET, mask);
	reg_write32(base + AWS_REG_V3_TDMA_MODEL_TDMA_BROADCAST_CFG_LAST_NODE_OFFSET, last_node);

	return 0;
}
