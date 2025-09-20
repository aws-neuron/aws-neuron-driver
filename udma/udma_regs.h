// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

/** UDMA hardware register offset definitions
 */

#ifndef __UDMA__REG_H
#define __UDMA__REG_H

#include <linux/types.h>

struct udma_axi_m2s {
	u32 comp_wr_cfg_1;
	u32 comp_wr_cfg_2;
	u32 data_rd_cfg_1;
	u32 data_rd_cfg_2;
	u32 desc_rd_cfg_1;
	u32 desc_rd_cfg_2;
	/* [0x18] Data read master configuration */
	u32 data_rd_cfg;
	u32 desc_rd_cfg_3;
	u32 desc_wr_cfg_1;
	/* [0x24] AXI outstanding configuration */
	u32 ostand_cfg;
	u32 reserved2[54];
};

struct udma_m2s {
	/*
	 * [0x0] DMA state.
	 * 00  - No pending tasks
	 * 01 - Normal (active)
	 * 10 - Abort (error condition)
	 * 11 - Reserved
	 */
	u32 state;
	/* [0x4] CPU request to change DMA state */
	u32 change_state;
	u32 reserved0;
	u32 err_log_mask;
	u32 reserved1[17];
	/* [0x54] M2S packet length configuration */
	u32 cfg_len;
	/* [0x58] Stream interface configuration */
	u32 stream_cfg;
	u32 reserved2[41];
};

struct udma_m2s_rd {
	/* [0x0] M2S descriptor prefetch configuration */
	u32 desc_pref_cfg_1;
	/* [0x4] M2S descriptor prefetch configuration */
	u32 desc_pref_cfg_2;
	/* [0x8] M2S descriptor prefetch configuration */
	u32 desc_pref_cfg_3;
	u32 reserved0;
	/* [0x10] Data burst read configuration */
	u32 data_cfg;
	u32 reserved1[11];
};

struct udma_rlimit_common {
	u32 reserved0[5];
	/*
	 * [0x14] Mask the different types of rate limiter.
	 * 0 - Rate limit is active.
	 * 1 - Rate limit is masked.
	 */
	u32 mask;
};

struct udma_m2s_stream_rate_limiter {
	struct udma_rlimit_common rlimit;
	u32 reserved0[10];
};

struct udma_m2s_comp {
	u32 reserved0[2];
	/* [0x8] Completion controller application acknowledge configuration */
	u32 cfg_application_ack;
	u32 reserved1[61];
};

struct udma_m2s_feature {
	u32 reserved0;
	u32 dma_version;
	u32 reserved1[62];
};

struct udma_m2s_q {
	/* [0x0] M2S descriptor prefetch configuration */
	u32 desc_pref_cfg;
	u32 reserved0[7];
	/* [0x20] M2S descriptor ring configuration */
	u32 cfg;
	/* [0x24] M2S descriptor ring status and information */
	u32 status;
	/* [0x28] TX Descriptor Ring Base Pointer [31:6] */
	u32 tdrbp_low;
	/* [0x2c] TX Descriptor Ring Base Pointer [63:32] */
	u32 tdrbp_high;
	/* [0x30] TX Descriptor Ring Length[23:2] */
	u32 tdrl;
	/* [0x34] TX Descriptor Ring Head Pointer */
	u32 tdrhp;
	/* [0x38] Tx Descriptor Tail Pointer increment */
	u32 tdrtp_inc;
	/* [0x3c] Tx Descriptor Tail Pointer */
	u32 tdrtp;
	/* [0x40] TX Descriptor Current Pointer */
	u32 tdcp;
	/* [0x44] Tx Completion Ring Base Pointer [31:6] */
	u32 tcrbp_low;
	/* [0x48] TX Completion Ring Base Pointer [63:32] */
	u32 tcrbp_high;
	/* [0x4c] TX Completion Ring Head Pointer */
	u32 tcrhp;
	u32 reserved1[4];
	/* [0x60] Rate limit configuration */
	struct udma_rlimit_common rlimit;
	u32 reserved2[10];
	/* [0xa0] Completion controller configuration */
	u32 comp_cfg;
	u32 reserved3[3];
	/* [0xb0] SW control  */
	u32 q_sw_ctrl;
	u32 reserved6[979];
};

struct udma_m2s_regs_v4 {
	u32 reserved0[64];
	struct udma_axi_m2s axi_m2s; /* [0x100] */
	struct udma_m2s m2s; /* [0x200] */
	struct udma_m2s_rd m2s_rd; /* [0x300] */
	u32 reserved1[32]; /* [0x340] */
	struct udma_m2s_stream_rate_limiter m2s_stream_rate_limiter; /* [0x3c0] */
	struct udma_m2s_comp m2s_comp; /* [0x400] */
	u32 reserved2[64]; /* [0x500] */
	struct udma_m2s_feature m2s_feature; /* [0x600] */
	u32 reserved3[576];
	struct udma_m2s_q m2s_q[16]; /* [0x1000] */
};

/* Maximum packet size for the M2S */
#define UDMA_M2S_CFG_LEN_MAX_PKT_SIZE_MASK 0x000FFFFF
#define UDMA_M2S_CFG_LEN_MAX_PKT_SIZE_SHIFT 0

/* Length encoding for 64K.
 * 0 - length 0x0000 = 0
 * 1 - length 0x0000 = 64k
 */
#define UDMA_M2S_CFG_LEN_ENCODE_64K (1 << 24)

/**** stream_cfg register ****/
/*
 * Disables the stream interface operation.
 * Changing to 1 stops at the end of packet transmission.
 */
#define UDMA_M2S_STREAM_CFG_DISABLE  (1 << 0)
/*
 * Configuration of the stream FIFO read control.
 * 0 - Cut through
 * 1 - Threshold based
 */
#define UDMA_M2S_STREAM_CFG_RD_MODE  (1 << 1)
/* Minimum number of beats to start packet transmission. */
#define UDMA_M2S_STREAM_CFG_RD_TH_MASK 0x0007FF00
#define UDMA_M2S_STREAM_CFG_RD_TH_SHIFT 8

#define UDMA_M2S_RD_DESC_PREF_CFG_2_PERF_FORCE_RR_SHIFT 16
/* Maximum number of descriptors per packet */
#define UDMA_M2S_RD_DESC_PREF_CFG_2_MAX_DESC_PER_PKT_SHIFT 0

/* Maximum descriptor burst size axi_m2s/s2m->desc_rd_cfg_3
*/
#define UDMA_AXI_M2S_DESC_RD_CFG_3_MAX_AXI_BEATS_SHIFT 0  // confusingly named, actually number of descs
#define UDMA_AXI_M2S_DESC_RD_CFG_3_ALWAYS_BREAK_ON_MAX_BOUNDARY_SHIFT 16
#define UDMA_AXI_S2M_DESC_RD_CFG_3_MAX_AXI_BEATS_SHIFT 0  // confusingly named, actually number of descs
#define UDMA_AXI_S2M_DESC_RD_CFG_3_ALWAYS_BREAK_ON_MAX_BOUNDARY_SHIFT 16

/* Minimum descriptor burst size when prefetch FIFO level is above the descriptor prefetch threshold
 */
#define UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_MASK 0x000000F0
#define UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_SHIFT 4
#define UDMA_S2M_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_SHIFT 4

#define UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_BELOW_THR_SHIFT 0
#define UDMA_S2M_RD_DESC_PREF_CFG_3_MIN_BURST_BELOW_THR_SHIFT 0

/* Descriptor fetch threshold.
 * Used as a threshold to determine the allowed minimum descriptor burst size.
 * (Must be at least max_desc_per_pkt)
 */
#define UDMA_M2S_RD_DESC_PREF_CFG_3_PREF_THR_MASK 0x0000FF00
#define UDMA_M2S_RD_DESC_PREF_CFG_3_PREF_THR_SHIFT 8
#define UDMA_S2M_RD_DESC_PREF_CFG_3_PREF_THR_SHIFT 8

/* Maximum number of data beats in the data read FIFO.
 * Defined based on data FIFO size. (default FIFO size 16KB - 512 beats) (V4)
 */
#define UDMA_M2S_RD_DATA_CFG_DATA_FIFO_DEPTH_MASK 0x00000FFF
#define UDMA_M2S_RD_DATA_CFG_DATA_FIFO_DEPTH_SHIFT 0

/** Maximum number of packets in the data read FIFO.
 *  Defined based on header FIFO size.
 */
#define UDMA_M2S_RD_DATA_CFG_MAX_PKT_LIMIT_SHIFT 16
#define UDMA_M2S_RD_DATA_CFG_MAX_PKT_LIMIT_RESET_VALUE 0x100

/* Enable prefetch operation of this queue.*/
#define UDMA_M2S_Q_CFG_EN_PREF (1 << 16)
/* Enable schedule operation of this queue.*/
#define UDMA_M2S_Q_CFG_EN_SCHEDULING (1 << 17)

/* M2S Descriptor Ring Base address [31:6].
 * [5:0] - 0 - 64B alignment is enforced
 * ([11:6] should be 0 for 4KB alignment)
 */
#define UDMA_M2S_Q_TDRBP_LOW_ADDR_MASK 0xFFFFFFC0
#define UDMA_M2S_Q_TDRBP_LOW_ADDR_SHIFT 6

/* Length of the descriptor ring.*/
#define UDMA_M2S_Q_TDRL_OFFSET_MASK 0x00FFFFFF
#define UDMA_M2S_Q_TDRL_OFFSET_SHIFT 0

/* Relative offset of the next descriptor that needs to be read into the prefetch FIFO.
 * Incremented when the DMA reads valid descriptors from the host memory to the prefetch FIFO.
 * Note that this is the offset in # of descriptors and not in byte address.
 */
#define UDMA_M2S_Q_TDRHP_OFFSET_MASK 0x00FFFFFF
#define UDMA_M2S_Q_TDRHP_OFFSET_SHIFT 0
/* Ring ID */
#define UDMA_M2S_Q_TDRHP_RING_ID_MASK 0xC0000000
#define UDMA_M2S_Q_TDRHP_RING_ID_SHIFT 30

/* Increments the value in Q_TDRTP (descriptors) */
#define UDMA_M2S_Q_TDRTP_INC_VAL_MASK 0x00FFFFFF
#define UDMA_M2S_Q_TDRTP_INC_VAL_SHIFT 0

/* Relative offset of the next free descriptor in the host memory.
 * Note that this is the offset in # of descriptors and not in byte address.
 */
#define UDMA_M2S_Q_TDRTP_OFFSET_MASK 0x00FFFFFF
#define UDMA_M2S_Q_TDRTP_OFFSET_SHIFT 0
/* Ring ID */
#define UDMA_M2S_Q_TDRTP_RING_ID_MASK 0xC0000000
#define UDMA_M2S_Q_TDRTP_RING_ID_SHIFT 30

/* M2S Descriptor Ring Base address [31:6].
 * [5:0] - 0 - 64B alignment is enforced
 * ([11:6] should be 0 for 4KB alignment)
 */
#define UDMA_M2S_Q_TCRBP_LOW_ADDR_MASK 0xFFFFFFC0
#define UDMA_M2S_Q_TCRBP_LOW_ADDR_SHIFT 6

/* Relative offset of the next descriptor that needs to be updated by the completion controller.
 * Note: This is in descriptors and not in byte address.
 */
#define UDMA_M2S_Q_TCRHP_OFFSET_MASK 0x00FFFFFF
#define UDMA_M2S_Q_TCRHP_OFFSET_SHIFT 0
/* Ring ID */
#define UDMA_M2S_Q_TCRHP_RING_ID_MASK 0xC0000000
#define UDMA_M2S_Q_TCRHP_RING_ID_SHIFT 30

/* Relative offset of the next descriptor that needs to be updated by the completion controller.
 * Note: This is in descriptors and not in byte address.
 */
#define UDMA_M2S_Q_TCRHP_INTERNAL_OFFSET_MASK 0x00FFFFFF
#define UDMA_M2S_Q_TCRHP_INTERNAL_OFFSET_SHIFT 0
/* Ring ID */
#define UDMA_M2S_Q_TCRHP_INTERNAL_RING_ID_MASK 0xC0000000
#define UDMA_M2S_Q_TCRHP_INTERNAL_RING_ID_SHIFT 30

/* Enable writing to the completion ring */
#define UDMA_M2S_Q_COMP_CFG_EN_COMP_RING_UPDATE (1 << 0)
/* Disable the completion coalescing function. */
#define UDMA_M2S_Q_COMP_CFG_DIS_COMP_COAL (1 << 1)

/* Reset the queue */
#define UDMA_M2S_Q_SW_CTRL_RST_Q (1 << 8)

struct udma_axi_s2m {
	u32 data_wr_cfg_1;
	u32 data_wr_cfg_2;
	u32 desc_rd_cfg_4;
	u32 desc_rd_cfg_5;
	u32 comp_wr_cfg_1;
	u32 comp_wr_cfg_2;
	u32 data_wr_cfg;
	u32 desc_rd_cfg_3;
	u32 desc_wr_cfg_1;	
	/* [0x24] AXI outstanding read configuration */
	u32 ostand_cfg_rd;
	/* [0x28] AXI outstanding write configuration */
	u32 ostand_cfg_wr;
	/* [0x2c] AXI outstanding write configuration 2 */
	u32 ostand_cfg_wr_2;
	u32 reserved1[52];
};

struct udma_s2m {
	/* [0x0] DMA state
	 *
	 * 00  - No pending tasks
	 * 01 - Normal (active)
	 * 10 - Abort (error condition)
	 * 11 - Reserved
	 */
	u32 state;
	/* [0x4] CPU request to change DMA state */
	u32 change_state;
	u32 reserved0;
	u32 err_log_mask;
	u32 reserved1[16];
	/* [0x50] Stream interface configuration */
	u32 stream_cfg;
	u32 reserved2[43];
};

struct udma_s2m_rd {
	/* [0x0] S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_1;
	/* [0x4] S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_2;
	/* [0x8] S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_3;
	/* [0xc] S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_4;
	u32 reserved0[12];
};

struct udma_s2m_comp {
	/* [0x0] Completion controller configuration */
	u32 cfg_1c;
	u32 reserved0[2];
	/*
	 * [0xc] Completion controller application acknowledge configuration
	 * Acknowledge timeout timer.
	 * ACK from the application through the stream interface)
	 */
	u32 cfg_application_ack;
	u32 reserved1[12];
};

struct udma_s2m_q {
	u32 reserved0[8];
	/* [0x20] S2M Descriptor ring configuration */
	u32 cfg;
	/* [0x24] S2M Descriptor ring status and information */
	u32 status;
	/* [0x28] Rx Descriptor Ring Base Pointer [31:6] */
	u32 rdrbp_low;
	/* [0x2c] Rx Descriptor Ring Base Pointer [63:32]*/
	u32 rdrbp_high;
	/* [0x30] Rx Descriptor Ring Length[23:2] */
	u32 rdrl;
	/* [0x34] RX Descriptor Ring Head Pointer */
	u32 rdrhp;
	/* [0x38] Rx Descriptor Tail Pointer increment */
	u32 rdrtp_inc;
	/* [0x3c] Rx Descriptor Tail Pointer */
	u32 rdrtp;
	/* [0x40] RX Descriptor Current Pointer */
	u32 rdcp;
	/* [0x44] Rx Completion Ring Base Pointer [31:6] */
	u32 rcrbp_low;
	/* * [0x48] Rx Completion Ring Base Pointer [63:32] */
	u32 rcrbp_high;
	/* [0x4c] Rx Completion Ring Head Pointer */
	u32 rcrhp;
	u32 reserved1;
	/* [0x54] Completion controller configuration for the queue */
	u32 comp_cfg;
	u32 reserved2[3];
	/* [0x64] DMB software control */
	u32 q_sw_ctrl;
	u32 reserved3[998];
};

struct udma_s2m_regs_v4 {
	u32 reserved0[64];
	struct udma_axi_s2m axi_s2m; /* [0x100] */
	struct udma_s2m s2m; /* [0x200] */
	struct udma_s2m_rd s2m_rd; /* [0x300] */
	u32 reserved1[16];
	struct udma_s2m_comp s2m_comp; /* [0x380] */
	u32 reserved2[784];
	struct udma_s2m_q s2m_q[16]; /* [0x1000] */
};

/* Maximum number of outstanding descriptor reads to the AXI.*/
#define UDMA_AXI_S2M_OSTAND_CFG_RD_MAX_DESC_RD_OSTAND_MASK 0x000000FF
#define UDMA_AXI_S2M_OSTAND_CFG_RD_MAX_DESC_RD_OSTAND_SHIFT 0
/* Maximum number of outstanding stream acknowledges. */
#define UDMA_AXI_S2M_OSTAND_CFG_RD_MAX_STREAM_ACK_SHIFT 16

/* Maximum number of outstanding data writes to the AXI. */
#define UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_DATA_WR_OSTAND_SHIFT 0
/* Maximum number of outstanding descriptor writes to the AXI. */
#define UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_DATA_BEATS_WR_SHIFT 8
/* Maximum number of outstanding data beats for data write to AXI. (AXI beats). */
#define UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_COMP_REQ_SHIFT 16
/* Maximum number of outstanding data beats for descriptor write to AXI. (AXI beats). */
#define UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_COMP_DATA_WR_SHIFT 24

/* Maximum number of outstanding data writes to the AXI */
#define UDMA_AXI_S2M_OSTAND_CFG_WR_2_MAX_DATA_WR_OSTAND_MASK 0x000003FF
#define UDMA_AXI_S2M_OSTAND_CFG_WR_2_MAX_DATA_WR_OSTAND_SHIFT 0

/* Disables the stream interface operation.
 * Changing to 1 stops at the end of packet reception.
 */
#define UDMA_S2M_STREAM_CFG_DISABLE (1 << 0)

/* Flush the stream interface operation.
 * Changing to 1 stops at the end of packet reception and assert ready to the stream I/F.
 */
#define UDMA_S2M_STREAM_CFG_FLUSH (1 << 4)

/* Stop descriptor prefetch when the stream is disabled and the S2M is idle. */
#define UDMA_S2M_STREAM_CFG_STOP_PREFETCH (1 << 8)

/* Enable promotion of the current queue in progress in the completion write scheduler. */
#define UDMA_S2M_COMP_CFG_1C_Q_PROMOTION_SHIFT 12

/* Completion descriptor size(words). */
#define UDMA_S2M_COMP_CFG_1C_DESC_SIZE_SHIFT 0x0

/* Reset the queue */
#define UDMA_S2M_Q_SW_CTRL_RST_Q (1 << 8)

/* Enables the reception of packets from the stream to this queue */
#define UDMA_S2M_Q_CFG_EN_STREAM (1 << 17)

/* Maximum number of outstanding data reads to the AXI (AXI transactions) - up to 128 */
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_DATA_RD_MASK 0x000000FF
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_DATA_RD_SHIFT 0

/* Maximum number of outstanding descriptor reads to the AXI (AXI transactions) - up to 128 */
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_DESC_RD_MASK 0x0000FF00
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_DESC_RD_SHIFT 8

/* Maximum number of outstanding descriptor writes to the AXI (AXI transactions) - up to 128 */
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_REQ_MASK 0x00FF0000
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_REQ_SHIFT 16

/* Maximum number of outstanding data beats for descriptor write to AXI (AXI beats) - up to 32 */
#define UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_DATA_WR_SHIFT 24

/* Completion control */
#define UDMA_M2S_STATE_COMP_CTRL_MASK 0x00000003
#define UDMA_M2S_STATE_COMP_CTRL_SHIFT 0
/* Stream interface */
#define UDMA_M2S_STATE_STREAM_IF_MASK 0x00000030
#define UDMA_M2S_STATE_STREAM_IF_SHIFT 4
/* Data read control */
#define UDMA_M2S_STATE_DATA_RD_CTRL_MASK 0x00000300
#define UDMA_M2S_STATE_DATA_RD_CTRL_SHIFT 8
/* Descriptor prefetch */
#define UDMA_M2S_STATE_DESC_PREF_MASK 0x00003000
#define UDMA_M2S_STATE_DESC_PREF_SHIFT 12

/* Start normal operation */
#define UDMA_M2S_CHANGE_STATE_NORMAL (1 << 0)
/* Stop normal operation */
#define UDMA_M2S_CHANGE_STATE_DIS (1 << 1)
/* Stop all internal engines */
#define UDMA_M2S_CHANGE_STATE_ABORT (1 << 2)

/* Ring ID m2s error */
#define UDMA_M2S_ERR_LOG_MASK_PREF_RING_ID (1 << 17)
/* Ring ID s2m error */
#define UDMA_S2M_ERR_LOG_MASK_PREF_RING_ID (1 << 11)

/* Interrupt controller level ring id m2s error */
#define INT_CONTROL_GRP_UDMA_M2S_PREF_RING_ID (1 << 11)
/* Interrupt controller level ring id s2m error */
#define INT_CONTROL_GRP_UDMA_S2M_PREF_RING_ID (1 << 2)

/* Disables the stream interface operation. */
#define UDMA_S2M_DMA_RING_DISABLE (1 << 0)


struct iofic_grp_ctrl {
	/* [0x0] Interrupt Cause Register */
	u32 int_cause_grp;
	u32 reserved0;
	/** [0x8] Interrupt Cause Set Register
	 * Writing 1 to a bit in this register sets its corresponding cause bit, enabling software
	 * to generate a hardware interrupt. Write 0 has no effect.
	 */
	u32 int_cause_set_grp;
	u32 reserved1;
	/* [0x10] Interrupt Mask Register */
	u32 int_mask_grp;
	u32 reserved2;
	/* [0x18] Interrupt Mask Clear Register */
	u32 int_mask_clear_grp;
	u32 reserved3;
	/* [0x20] Interrupt Status Register */
	u32 int_status_grp;
	u32 reserved4;
	/* [0x28] Interrupt Control Register */
	u32 int_control_grp;
	u32 reserved5;
	/* [0x30] Interrupt Mask Register */
	u32 int_abort_msk_grp;
	u32 reserved7[3];
};

union iofic_regs {
	struct iofic_grp_ctrl ctrl[16];
	u32 reserved0[0x400 >> 2];
};

struct udma_iofic_regs {
	union iofic_regs main_iofic;
	u32 reserved0[(0x1c00) >> 2];
	struct iofic_grp_ctrl secondary_iofic_ctrl[2];
};

struct udma_gen_axi {
	/* [0x0] Configuration of the AXI masters - Timeout value for all transactions on the AXI */
	u32 cfg_1;
	u32 reserved0[63];
};

struct udma_gen_axi_error_detection_table {
	/* [0x0] Addr to table is {axi_parity_error,axi_timeout_error,axi_response_error} */
	uint32_t addr0;
	/* [0x4] */
	uint32_t addr1;
	/* [0x8] */
	uint32_t addr2;
	/* [0xc] */
	uint32_t addr3;
	/* [0x10] */
	uint32_t addr4;
	/* [0x14] */
	uint32_t addr5;
	/* [0x18] */
	uint32_t addr6;
	/* [0x1c] */
	uint32_t addr7;
	/* [0x20] */
	uint32_t addr8;
	/* [0x24] */
	uint32_t addr9;
	/* [0x28] */
	uint32_t addr10;
	/* [0x2c] */
	uint32_t addr11;
	/* [0x30] */
	uint32_t addr12;
	/* [0x34] */
	uint32_t addr13;
	/* [0x38] */
	uint32_t addr14;
	/* [0x3c] */
	uint32_t addr15;
};

struct udma_gen_axi_error_control {
	/* [0x0] */
	uint32_t table_addr;
	/* [0x4] */
	uint32_t table_data;
};

struct udma_gen_axi_queue {
	/* [0x0] this register can change axi queue state ACTIVE/NON_ACTIVE */
	uint32_t state_request;
	/* [0x4] This register is read on clear on read */
	uint32_t error_status;
	/* [0x8] */
	uint32_t cfg;
};

struct udma_gen_spare_reg {
	u32 zeroes0;
	u32 zeroes1;
	u32 ones0;
	u32 ones1;
};

struct udma_gen_regs_v4 {
	struct udma_iofic_regs interrupt_regs;		     /* [0x0000] */
	uint32_t rsrvd_0[160];
	struct udma_gen_axi axi;                             /* [0x2300] */
	uint32_t rsrvd_1[320];
	struct udma_gen_axi_error_detection_table axi_error_detection_table[7]; /* [0x2900] */
	uint32_t rsrvd_2[16];
	struct udma_gen_axi_error_control axi_error_control[7]; /* [0x2b00] */
	uint32_t rsrvd_3[50];
	struct udma_gen_axi_queue axi_queue[16];             /* [0x2c00] */
	uint32_t rsrvd_4[16];
	uint32_t iofic_base_m2s_desc_rd;			/* [0x2d00] */
	uint32_t rsrvd_5[63];
	uint32_t iofic_base_m2s_data_rd;			/* [0x2e00] */
	uint32_t rsrvd_6[63];
	uint32_t iofic_base_m2s_cmpl_wr;			/* [0x2f00] */
	uint32_t rsrvd_7[63];
	uint32_t iofic_base_s2m_desc_rd;			/* [0x3000] */
	uint32_t rsrvd_8[63];
	uint32_t iofic_base_s2m_data_wr;			/* [0x3100] */
	uint32_t rsrvd_9[63];
	uint32_t iofic_base_s2m_cmpl_wr;			/* [0x3200] */
	uint32_t rsrvd_10[63];
	uint32_t iofic_base_msix;				    /* [0x3300] */
	uint32_t rsrvd_11[87];
	struct udma_gen_spare_reg spare_reg;                 /* [0x3460] */
};

struct udma_gen_ex_vmpr_v4 {
	u32 tx_sel;
	u32 rx_sel[3];
	u32 reserved0[12];
};

struct udma_gen_ex_regs {
	u32 reserved0[0x100];
	struct udma_gen_ex_vmpr_v4 vmpr_v4[16]; /* [0x400] */
};

struct unit_regs_v4 {
	struct udma_m2s_regs_v4 m2s;
	u32 reserved0[(0x20000 - sizeof(struct udma_m2s_regs_v4)) >> 2];
	struct udma_s2m_regs_v4 s2m;
	u32 reserved1[((0x38000 - 0x20000) - sizeof(struct udma_s2m_regs_v4)) >> 2];
	struct udma_gen_regs_v4 gen;
	u32 reserved2[((0x3c000 - 0x38000) - sizeof(struct udma_gen_regs_v4)) >> 2];
	struct udma_gen_ex_regs gen_ex;
};

/** UDMA submission and completion registers, M2S and S2M UDMAs have same structure */
struct udma_rings_regs {
	u32 reserved0[8];
	u32 cfg; /* Descriptor ring configuration */
	u32 status; /* Descriptor ring status and information */
	u32 drbp_low; /* Descriptor Ring Base Pointer [31:4] */
	u32 drbp_high; /* Descriptor Ring Base Pointer [63:32] */
	u32 drl; /* Descriptor Ring Length[23:2] */
	u32 drhp; /* Descriptor Ring Head Pointer */
	u32 drtp_inc; /* Descriptor Tail Pointer increment */
	u32 drtp; /* Descriptor Tail Pointer */
	u32 dcp; /* Descriptor Current Pointer */
	u32 crbp_low; /* Completion Ring Base Pointer [31:4] */
	u32 crbp_high; /* Completion Ring Base Pointer [63:32] */
	u32 crhp; /* Completion Ring Head Pointer */
	u32 reserved1;
};

/** M2S and S2M generic structure of Q registers */
union udma_q_regs {
	struct udma_rings_regs rings;
	struct udma_m2s_q m2s_q;
	struct udma_s2m_q s2m_q;
};

#endif
