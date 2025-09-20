// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

/** Interrupt and abort configurations.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "udma.h"
#include "udma_regs.h"

/*
 * IOFIC group description
 * INT_GROUP_A		Summary of the below events
 * INT_GROUP_B		RX completion queues
 * INT_GROUP_C		TX completion queues
 * INT_GROUP_D		Misc
 */

/** Summary of secondary interrupt controller, group A) */
#define INT_GROUP_D_M2S BIT(8)
/** Summary of secondary interrupt controller, group B) */
#define INT_GROUP_D_S2M BIT(9)

/** Bits 8 - 11 are connected to the secondary level iofic's group A - D */
#define INT_UDMA_V4_GROUP_D_2ND_IOFIC_GROUP_C BIT(10)

/** MSIX Bus generator response error */
#define INT_2ND_GROUP_A_M2S_MSIX_RESP BIT(27)
/** * MSIx timeout for the MSIx write transaction */
#define INT_2ND_GROUP_A_M2S_MSIX_TO BIT(26)
/** Prefetch header buffer parity error */
#define INT_2ND_GROUP_A_M2S_PREFETCH_HDR_PARITY BIT(25)
/** Prefetch descriptor buffer parity error */
#define INT_2ND_GROUP_A_M2S_PREFETCH_DESC_PARITY BIT(24)
/** Data buffer parity error */
#define INT_2ND_GROUP_A_M2S_DATA_PARITY BIT(23)
/** Data header buffer parity error */
#define INT_2ND_GROUP_A_M2S_HDR_PARITY BIT(22)
/** Completion coalescing buffer parity error */
#define INT_2ND_GROUP_A_M2S_COMPL_COAL_PARITY BIT(21)
/** UNACK packets buffer parity error */
#define INT_2ND_GROUP_A_M2S_UNACK_PKT_PARITY BIT(20)
/** ACK packets buffer parity error */
#define INT_2ND_GROUP_A_M2S_ACK_PKT_PARITY BIT(19)
/** AXI data buffer parity error */
#define INT_2ND_GROUP_A_M2S_AXI_DATA_PARITY BIT(18)

/** Prefetch Ring ID error - A wrong RingID was received while prefetching submission descriptor.*/
#define INT_2ND_GROUP_A_M2S_PREFETCH_RING_ID BIT(17)
/** Error in last bit indication of the descriptor */
#define INT_2ND_GROUP_A_M2S_PREFETCH_LAST BIT(16)
/** Error in first bit indication of the descriptor */
#define INT_2ND_GROUP_A_M2S_PREFETCH_FIRST BIT(15)
/** Number of descriptors per packet exceeds the maximum descriptors per packet. */
#define INT_2ND_GROUP_A_M2S_PREFETCH_MAX_DESC BIT(14)
/** Packet length exceeds the configurable maximum packet size. */
#define INT_2ND_GROUP_A_M2S_PKT_LEN BIT(13)
/** Bus request to I/O Fabric timeout error */
#define INT_2ND_GROUP_A_M2S_PREFETCH_AXI_TO BIT(12)
/** Bus response from I/O Fabric error */
#define INT_2ND_GROUP_A_M2S_PREFETCH_AXI_RESP BIT(11)
/** Bus parity error on descriptor being prefetched */
#define INT_2ND_GROUP_A_M2S_PREFETCH_AXI_PARITY BIT(10)
/** Bus request to I/O Fabric timeout error */
#define INT_2ND_GROUP_A_M2S_DATA_AXI_TO BIT(9)
/** Bus response from I/O Fabric error */
#define INT_2ND_GROUP_A_M2S_DATA_AXI_RESP BIT(8)
/** Bus parity error on data being read */
#define INT_2ND_GROUP_A_M2S_DATA_AXI_PARITY BIT(7)
/** * Bus request to I/O Fabric timeout error */
#define INT_2ND_GROUP_A_M2S_COMPL_AXI_TO BIT(6)
/** Bus response from I/O Fabric error */
#define INT_2ND_GROUP_A_M2S_COMPL_AXI_RESP BIT(5)
/** Bus generator internal SRAM parity error */
#define INT_2ND_GROUP_A_M2S_COMP_AXI_PARITY BIT(4)
/** Application stream interface timeout */
#define INT_2ND_GROUP_A_M2S_STRM_TO BIT(3)
/** Application stream interface response error */
#define INT_2ND_GROUP_A_M2S_STRM_RESP BIT(2)
/** Application stream interface parity error */
#define INT_2ND_GROUP_A_M2S_STRM_PARITY BIT(1)
/** Application stream interface, packet serial mismatch error*/
#define INT_2ND_GROUP_A_M2S_STRM_COMPL_MISMATCH BIT(0)

/** Transaction table info parity error (V4)*/
#define INT_2ND_GROUP_B_S2M_TRNSCTN_TBL_INFO_PARITY BIT(31)
/** Prefetch descriptor buffer parity error */
#define INT_2ND_GROUP_B_S2M_PREFETCH_DESC_PARITY BIT(30)
/** Completion coalescing buffer parity error */
#define INT_2ND_GROUP_B_S2M_COMPL_COAL_PARITY BIT(29)
/** PRE-UNACK packets buffer parity error */
#define INT_2ND_GROUP_B_S2M_PRE_UNACK_PKT_PARITY BIT(28)
/** UNACK packets buffer parity error */
#define INT_2ND_GROUP_B_S2M_UNACK_PKT_PARITY BIT(27)
/** Data buffer parity error */
#define INT_2ND_GROUP_B_S2M_DATA_PARITY BIT(26)
/** Data header buffer parity error */
#define INT_2ND_GROUP_B_S2M_DATA_HDR_PARITY BIT(25)

/** Application stream interface, Data counter length mismatch with metadata packet length */
#define INT_2ND_GROUP_B_S2M_PKT_LEN BIT(24)
/** Application stream interface, error in Last bit indication */
#define INT_2ND_GROUP_B_S2M_STRM_LAST BIT(23)
/**Application stream interface error in first bit indication */
#define INT_2ND_GROUP_B_S2M_STRM_FIRST BIT(22)
/** Application stream interface, error indication during data transaction */
#define INT_2ND_GROUP_B_S2M_STRM_DATA BIT(21)
/** Application stream interface, parity error during data transaction */
#define INT_2ND_GROUP_B_S2M_STRM_DATA_PARITY BIT(20)
/** Application stream interface, error indication during header transaction */
#define INT_2ND_GROUP_B_S2M_STRM_HDR BIT(19)
/** Application stream interface, parity error during header transaction */
#define INT_2ND_GROUP_B_S2M_STRM_HDR_PARITY BIT(18)
/** Completion write, UNACK timeout due to completion FIFO back pressure */
#define INT_2ND_GROUP_B_S2M_COMPL_UNACK BIT(17)
/** Completion write, UNACK timeout due to stream ACK FIFO back pressure */
#define INT_2ND_GROUP_B_S2M_COMPL_STRM BIT(16)
/** Bus request to I/O Fabric timeout error */
#define INT_2ND_GROUP_B_S2M_COMPL_AXI_TO BIT(15)
/** Bus response from I/O Fabric error */
#define INT_2ND_GROUP_B_S2M_COMPL_AXI_RESP BIT(14)
/** Completion Bus generator internal SRAM parity error */
#define INT_2ND_GROUP_B_S2M_COMPL_AXI_PARITY BIT(13)

/**Prefetch engine, Ring ID is not matching the expected RingID. */
#define INT_2ND_GROUP_B_S2M_PREFETCH_RING_ID BIT(11)
/** Bus request to I/O Fabric timeout error */
#define INT_2ND_GROUP_B_S2M_PREFETCH_AXI_TO BIT(10)
/** Bus response from I/O Fabric error */
#define INT_2ND_GROUP_B_S2M_PREFETCH_AXI_RESP BIT(9)
/** Bus parity error on descriptor being prefetched */
#define INT_2ND_GROUP_B_S2M_PREFETCH_AXI_PARITY BIT(8)

/** Bus request to I/O Fabric timeout error */
#define INT_2ND_GROUP_B_S2M_DATA_AXI_TO BIT(2)
/** Bus response from I/O Fabric error */
#define INT_2ND_GROUP_B_S2M_DATA_AXI_RESP BIT(1)
/** Bus parity error on data being read */
#define INT_2ND_GROUP_B_S2M_DATA_AXI_PARITY BIT(0)

/** S2M AXI request fifo descriptor prefetch parity error */
#define INT_2ND_GROUP_C_S2M_DESC_PRE_AXI_REQ_PARITY BIT(11)
/** S2M AXI command fifo descriptor prefetch parity error */
#define INT_2ND_GROUP_C_S2M_DESC_PRE_AXI_CMD_PARITY BIT(10)
/** S2M AXI write completion request fifo parity error */
#define INT_2ND_GROUP_C_S2M_CMP_WR_AXI_REQ_PARITY BIT(9)
/** S2M AXI write completion data fifo parity error */
#define INT_2ND_GROUP_C_S2M_CMP_WR_AXI_DATA_PARITY BIT(8)
/** S2M AXI data write request fifo parity error */
#define INT_2ND_GROUP_C_S2M_DATA_WR_AXI_REQ_PARITY BIT(7)
/** S2M AXI data write data fifo parity error */
#define INT_2ND_GROUP_C_S2M_DATA_WR_AXI_DATA_PARITY BIT(6)
/** M2S AXI write completion request fifo parity error */
#define INT_2ND_GROUP_C_M2S_CMP_WR_AXI_REQ_PARITY BIT(5)
/** M2S AXI write completion command fifo parity error */
#define INT_2ND_GROUP_C_M2S_CMP_WR_AXI_CMD_PARITY BIT(4)
/** M2S AXI descriptor prefetch request fifo parity error */
#define INT_2ND_GROUP_C_M2S_DESC_PRE_AXI_REQ_PARITY BIT(3)
/** M2S AXI descriptor prefetch command fifo parity error */
#define INT_2ND_GROUP_C_M2S_DESC_PRE_AXI_CMD_PARITY BIT(2)
/** M2S AXI data fetch request fifo parity error */
#define INT_2ND_GROUP_C_M2S_DATA_FTCH_AXI_REQ_PARITY BIT(1)
/** M2S AXI data fetch command fifo parity error */
#define INT_2ND_GROUP_C_M2S_DATA_FTCH_AXI_CMD_PARITY BIT(0)

/*******************************************************************************
 * error interrupts
 ******************************************************************************/
#define UDMA_IOFIC_2ND_GROUP_A_ERROR_INTS                                                          \
	(INT_2ND_GROUP_A_M2S_MSIX_RESP | INT_2ND_GROUP_A_M2S_MSIX_TO |                             \
	 INT_2ND_GROUP_A_M2S_PREFETCH_HDR_PARITY | INT_2ND_GROUP_A_M2S_PREFETCH_DESC_PARITY |      \
	 INT_2ND_GROUP_A_M2S_DATA_PARITY | INT_2ND_GROUP_A_M2S_HDR_PARITY |                        \
	 INT_2ND_GROUP_A_M2S_COMPL_COAL_PARITY | INT_2ND_GROUP_A_M2S_UNACK_PKT_PARITY |            \
	 INT_2ND_GROUP_A_M2S_ACK_PKT_PARITY | INT_2ND_GROUP_A_M2S_AXI_DATA_PARITY |                \
	 INT_2ND_GROUP_A_M2S_PREFETCH_RING_ID | INT_2ND_GROUP_A_M2S_PREFETCH_LAST |                \
	 INT_2ND_GROUP_A_M2S_PREFETCH_FIRST | INT_2ND_GROUP_A_M2S_PREFETCH_MAX_DESC |              \
	 INT_2ND_GROUP_A_M2S_PKT_LEN | INT_2ND_GROUP_A_M2S_PREFETCH_AXI_TO |                       \
	 INT_2ND_GROUP_A_M2S_PREFETCH_AXI_RESP | INT_2ND_GROUP_A_M2S_PREFETCH_AXI_PARITY |         \
	 INT_2ND_GROUP_A_M2S_DATA_AXI_TO | INT_2ND_GROUP_A_M2S_DATA_AXI_PARITY |                   \
	 INT_2ND_GROUP_A_M2S_DATA_AXI_RESP | INT_2ND_GROUP_A_M2S_COMPL_AXI_TO |                    \
	 INT_2ND_GROUP_A_M2S_COMPL_AXI_RESP | INT_2ND_GROUP_A_M2S_COMP_AXI_PARITY |                \
	 INT_2ND_GROUP_A_M2S_STRM_TO | INT_2ND_GROUP_A_M2S_STRM_RESP |                             \
	 INT_2ND_GROUP_A_M2S_STRM_PARITY | INT_2ND_GROUP_A_M2S_STRM_COMPL_MISMATCH)

#define UDMA_IOFIC_2ND_GROUP_B_ERROR_INTS                                                          \
	(INT_2ND_GROUP_B_S2M_PREFETCH_DESC_PARITY | INT_2ND_GROUP_B_S2M_COMPL_COAL_PARITY |        \
	 INT_2ND_GROUP_B_S2M_PRE_UNACK_PKT_PARITY | INT_2ND_GROUP_B_S2M_UNACK_PKT_PARITY |         \
	 INT_2ND_GROUP_B_S2M_DATA_PARITY | INT_2ND_GROUP_B_S2M_DATA_HDR_PARITY |                   \
	 INT_2ND_GROUP_B_S2M_PKT_LEN | INT_2ND_GROUP_B_S2M_STRM_LAST |                             \
	 INT_2ND_GROUP_B_S2M_STRM_FIRST | INT_2ND_GROUP_B_S2M_STRM_DATA |                          \
	 INT_2ND_GROUP_B_S2M_STRM_DATA_PARITY | INT_2ND_GROUP_B_S2M_STRM_HDR |                     \
	 INT_2ND_GROUP_B_S2M_STRM_HDR_PARITY | INT_2ND_GROUP_B_S2M_COMPL_UNACK |                   \
	 INT_2ND_GROUP_B_S2M_COMPL_STRM | INT_2ND_GROUP_B_S2M_COMPL_AXI_TO |                       \
	 INT_2ND_GROUP_B_S2M_COMPL_AXI_RESP | INT_2ND_GROUP_B_S2M_COMPL_AXI_PARITY |               \
	 INT_2ND_GROUP_B_S2M_PREFETCH_RING_ID | INT_2ND_GROUP_B_S2M_PREFETCH_AXI_TO |              \
	 INT_2ND_GROUP_B_S2M_PREFETCH_AXI_RESP | INT_2ND_GROUP_B_S2M_PREFETCH_AXI_PARITY |         \
	 INT_2ND_GROUP_B_S2M_DATA_AXI_TO | INT_2ND_GROUP_B_S2M_DATA_AXI_RESP |                     \
	 INT_2ND_GROUP_B_S2M_DATA_AXI_PARITY)

#define UDMA_V4_IOFIC_2ND_GROUP_B_ERROR_INTS                                                       \
	(UDMA_IOFIC_2ND_GROUP_B_ERROR_INTS | INT_2ND_GROUP_B_S2M_TRNSCTN_TBL_INFO_PARITY)

#define UDMA_V4_IOFIC_2ND_GROUP_C_M2S_ERROR_INTS                                                   \
	(INT_2ND_GROUP_C_M2S_DATA_FTCH_AXI_CMD_PARITY |                                            \
	 INT_2ND_GROUP_C_M2S_DATA_FTCH_AXI_REQ_PARITY |                                            \
	 INT_2ND_GROUP_C_M2S_DESC_PRE_AXI_CMD_PARITY |                                             \
	 INT_2ND_GROUP_C_M2S_DESC_PRE_AXI_REQ_PARITY | INT_2ND_GROUP_C_M2S_CMP_WR_AXI_CMD_PARITY | \
	 INT_2ND_GROUP_C_M2S_CMP_WR_AXI_REQ_PARITY)

#define UDMA_V4_IOFIC_2ND_GROUP_C_S2M_ERROR_INTS                                                   \
	(INT_2ND_GROUP_C_S2M_DATA_WR_AXI_DATA_PARITY |                                             \
	 INT_2ND_GROUP_C_S2M_DATA_WR_AXI_REQ_PARITY | INT_2ND_GROUP_C_S2M_CMP_WR_AXI_DATA_PARITY | \
	 INT_2ND_GROUP_C_S2M_CMP_WR_AXI_REQ_PARITY | INT_2ND_GROUP_C_S2M_DESC_PRE_AXI_CMD_PARITY | \
	 INT_2ND_GROUP_C_S2M_DESC_PRE_AXI_REQ_PARITY)

/** interrupt controller level (primary/secondary) */
enum udma_iofic_level { UDMA_IOFIC_LEVEL_PRIMARY, UDMA_IOFIC_LEVEL_SECONDARY };

#define INT_GROUP_A 0
#define INT_GROUP_B 1
#define INT_GROUP_C 2
#define INT_GROUP_D 3

/* When SET_ON_POSEDGE=1, the bits in the Interrupt Cause register are set on the posedge of the
 * interrupt source, i.e., when interrupt source=1 and Interrupt Status = 0.
 * When SET_ON_POSEDGE=0, the bits in the Interrupt Cause register are set when the
 * interrupt source=1.
 */
#define INT_CONTROL_GRP_SET_ON_POSEDGE (1 << 3)

/* When MASK_MSI_X=1, no MSI-X from this group is sent. This bit must be set to 1 when the
 * associated summary bit in this group is used to generate a single MSI-X for this group.
 */
#define INT_CONTROL_GRP_MASK_MSI_X (1 << 5)

/** Configure the interrupt controller registers.
 *
 * @param regs_base regs pointer to interrupt controller registers
 * @param group the interrupt group.
 * @param flags flags of Interrupt Control Register
 *
 * @note Interrupts are still masked.
 */
static void iofic_config(void __iomem *regs_base, int group, u32 flags)
{
	union iofic_regs __iomem *regs = (union iofic_regs __iomem *)(regs_base);
	reg_write32(&regs->ctrl[group].int_control_grp, flags);
}

/** Unmask specific interrupts for a given group.
 *
 * @param regs_base pointer to unit registers
 * @param group the interrupt group
 * @param mask bitwise of interrupts to unmask, set bits will be unmasked.
 */
static void iofic_unmask(void __iomem *regs_base, int group, u32 mask)
{
	union iofic_regs __iomem *regs = (union iofic_regs __iomem *)(regs_base);
	reg_write32(&regs->ctrl[group].int_mask_clear_grp, ~mask);
}

/** Clear specific interrupts in the abort mask for a given group
 *
 * This will result in UDMA aborting on the specific interrupts
 *
 * @param regs_base pointer to unit registers
 * @param group the interrupt group
 * @param mask bitwise of interrupts to clear
 */
static void iofic_abort_mask_clear(void __iomem *regs_base, int group, u32 mask)
{
	union iofic_regs __iomem *regs = (union iofic_regs __iomem *)(regs_base);
	reg_write32(&regs->ctrl[group].int_abort_msk_grp, ~mask);
}

/** Get the interrupt controller base address.
 *
 * Returns base address for either primary or secondary interrupt controller
 *
 * @param udma - pointer to UDMA handle
 * @param level - the interrupt controller level (primary / secondary)
 *
 * @returns	The interrupt controller base address
 */
static void __iomem *udma_iofic_reg_base_get_adv(struct udma *udma, enum udma_iofic_level level)
{
	struct udma_iofic_regs __iomem *int_regs = udma->gen_int_regs;

	if (level == UDMA_IOFIC_LEVEL_PRIMARY)
		return &int_regs->main_iofic;
	else
		return &int_regs->secondary_iofic_ctrl;
}

/** Check the interrupt controller level/group validity
 *
 * @param udma	- udma handle
 * @param level	- the interrupt controller level (primary / secondary)
 * @param group	- the interrupt group ('AL_INT_GROUP_*')
 *
 * @returns	0 - invalid, 1 - valid
 *
 */
static int udma_iofic_level_and_group_valid(struct udma *udma, enum udma_iofic_level level,
					    int group)
{
	int sec_group;

	if (udma->rev_id < UDMA_REV_ID_4)
		sec_group = 2;
	else
		sec_group = 3;

	if (((level == UDMA_IOFIC_LEVEL_PRIMARY) && (group >= 0) && (group < 4)) ||
	    ((level == UDMA_IOFIC_LEVEL_SECONDARY) && (group >= 0) && (group < sec_group)))
		return 1;

	return 0;
}

/** Unmask specific interrupts for a given group
 *
 * @param udma	- pointer to UDMA handle
 * @param level	- the interrupt controller level (primary / secondary)
 * @param group	- the interrupt group
 * @param mask	- bitwise of interrupts to unmask, set bits will be unmasked.
 *
 * @note  This functions uses the interrupt mask clear register to guarantee atomicity
 *         It's safe to call it while the mask is changed by the HW (auto mask) or another cpu.
 */
static void udma_iofic_unmask_adv(struct udma *udma, enum udma_iofic_level level, int group,
				  u32 mask)
{
	if (!udma_iofic_level_and_group_valid(udma, level, group)) {
		pr_err("invalid iofic level(%d) or group(%d)\n", level, group);
		return;
	}

	iofic_unmask(udma_iofic_reg_base_get_adv(udma, level), group, mask);
}

void udma_iofic_m2s_error_ints_unmask(struct udma *udma)
{
	u32 primary_grp_mask;

	primary_grp_mask = INT_GROUP_D_M2S | INT_UDMA_V4_GROUP_D_2ND_IOFIC_GROUP_C;

	/* config IOFIC */
	iofic_config(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY), INT_GROUP_A,
		     INT_CONTROL_GRP_SET_ON_POSEDGE | INT_CONTROL_GRP_MASK_MSI_X);

	iofic_config(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_PRIMARY), INT_GROUP_D,
		     INT_CONTROL_GRP_SET_ON_POSEDGE | INT_CONTROL_GRP_MASK_MSI_X);

	/* abort on these interrupts */
	iofic_abort_mask_clear(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY),
			       INT_GROUP_A, UDMA_IOFIC_2ND_GROUP_A_ERROR_INTS);

	iofic_abort_mask_clear(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY),
			       INT_GROUP_C, UDMA_V4_IOFIC_2ND_GROUP_C_M2S_ERROR_INTS);

	/* unmask interrupts */
	udma_iofic_unmask_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY, INT_GROUP_A,
			      UDMA_IOFIC_2ND_GROUP_A_ERROR_INTS);

	udma_iofic_unmask_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY, INT_GROUP_C,
			      UDMA_V4_IOFIC_2ND_GROUP_C_M2S_ERROR_INTS);

	udma_iofic_unmask_adv(udma, UDMA_IOFIC_LEVEL_PRIMARY, INT_GROUP_D, primary_grp_mask);
}

void udma_iofic_s2m_error_ints_unmask(struct udma *udma)
{
	u32 primary_grp_mask;
	u32 sec_b;

	primary_grp_mask = INT_GROUP_D_S2M | INT_UDMA_V4_GROUP_D_2ND_IOFIC_GROUP_C;
	sec_b = UDMA_V4_IOFIC_2ND_GROUP_B_ERROR_INTS;

	/* config IOFIC */
	iofic_config(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY), INT_GROUP_B,
		     INT_CONTROL_GRP_SET_ON_POSEDGE | INT_CONTROL_GRP_MASK_MSI_X);

	iofic_config(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_PRIMARY), INT_GROUP_D,
		     INT_CONTROL_GRP_SET_ON_POSEDGE | INT_CONTROL_GRP_MASK_MSI_X);

	/* abort on these interrupts */
	iofic_abort_mask_clear(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY),
			       INT_GROUP_B, sec_b);

	iofic_abort_mask_clear(udma_iofic_reg_base_get_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY),
			       INT_GROUP_C, UDMA_V4_IOFIC_2ND_GROUP_C_S2M_ERROR_INTS);

	/* unmask interrupts */
	udma_iofic_unmask_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY, INT_GROUP_B, sec_b);

	udma_iofic_unmask_adv(udma, UDMA_IOFIC_LEVEL_SECONDARY, INT_GROUP_C,
			      UDMA_V4_IOFIC_2ND_GROUP_C_S2M_ERROR_INTS);

	udma_iofic_unmask_adv(udma, UDMA_IOFIC_LEVEL_PRIMARY, INT_GROUP_D, primary_grp_mask);
}

void udma_iofic_error_ints_unmask_one(struct iofic_grp_ctrl *iofic_ctrl, uint32_t mask)
{
	iofic_config(iofic_ctrl, 0, INT_CONTROL_GRP_SET_ON_POSEDGE | INT_CONTROL_GRP_MASK_MSI_X);
	iofic_abort_mask_clear(iofic_ctrl, 0, mask);
	iofic_unmask(iofic_ctrl, 0, mask);
}

