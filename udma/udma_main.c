// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>

#include "udma.h"
#include "udma_regs.h"
#include "neuron_arch.h"
#include "../neuron_dhal.h"

/** get field out of 32 bit register */
#define REG_FIELD_GET(reg, mask, shift) (((reg) & (mask)) >> (shift))

#define ADDR_LOW(x) ((u32)((dma_addr_t)(x)))
#define ADDR_HIGH(x) ((u32)((((dma_addr_t)(x)) >> 16) >> 16))

#define UDMA_STATE_NORMAL 0x1
#define UDMA_STATE_ABORT 0x2

const char *const udma_states_name[] = { "Disable", "Idle", "Normal", "Abort", "Reset" };

/** M2S max packet size configuration */
struct udma_m2s_pkt_len_conf {
	u32 max_pkt_size;
	bool encode_64k_as_zero;
};


/*  dma_q flags */
#define UDMA_Q_FLAGS_NO_COMP_UPDATE BIT(1)

/* M2S packet len configuration, configure maximum DMA packets size, i.e. 
 * the max size of the sum of all descriptors in a packet.  Configure 
 * whether len=0 encodes len=64k
 */
static int udma_m2s_packet_size_cfg_set(struct udma *udma, struct udma_m2s_pkt_len_conf *conf)
{
	u32 reg = 0;
	u32 max_supported_size = UDMA_M2S_CFG_LEN_MAX_PKT_SIZE_MASK;

	if (conf->max_pkt_size > max_supported_size) {
		pr_err("%s: requested max_pkt_size %#x exceeds limit %#x\n", udma->name,
		       conf->max_pkt_size, max_supported_size);
		return -EINVAL;
	}

	if (conf->encode_64k_as_zero)
		reg |= UDMA_M2S_CFG_LEN_ENCODE_64K;
	else
		reg &= ~UDMA_M2S_CFG_LEN_ENCODE_64K;

	reg &= ~UDMA_M2S_CFG_LEN_MAX_PKT_SIZE_MASK;
	reg |= conf->max_pkt_size;

	reg_write32(&udma->udma_regs_m2s->m2s.cfg_len, reg);
	return 0;
}

/* set default configuration of one DMA engine */
static int udma_set_defaults(struct udma *udma)
{
	int ret = 0;
	struct udma_gen_ex_regs __iomem *gen_ex_regs;
	unsigned int i;
	u32 value;

	// Init TX
	struct udma_m2s_pkt_len_conf conf = {
		.encode_64k_as_zero = true, // encode len=64k as len=0
		.max_pkt_size = UDMA_M2S_CFG_LEN_MAX_PKT_SIZE_MASK,
	};

	/* Setting the data fifo depth.  For example, to 32K (1024 beats of 256 bits)
	 * allows the UDMA to have 128 for outstanding writes
	 */
	value = (ndhal->ndhal_udma.num_beats << UDMA_M2S_RD_DATA_CFG_DATA_FIFO_DEPTH_SHIFT) |
		(UDMA_M2S_RD_DATA_CFG_MAX_PKT_LIMIT_RESET_VALUE << UDMA_M2S_RD_DATA_CFG_MAX_PKT_LIMIT_SHIFT);
	reg_write32(&udma->udma_regs_m2s->m2s_rd.data_cfg, value);

	if (udma->reserve_max_read_axi_id) {
		value = (111 << UDMA_AXI_M2S_OSTAND_CFG_MAX_DATA_RD_SHIFT) |
			(8 << UDMA_AXI_M2S_OSTAND_CFG_MAX_DESC_RD_SHIFT) |
			(0 << UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_REQ_SHIFT) |
			(0 << UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_DATA_WR_SHIFT);
	} else {
		value = (128 << UDMA_AXI_M2S_OSTAND_CFG_MAX_DATA_RD_SHIFT) |
			(128 << UDMA_AXI_M2S_OSTAND_CFG_MAX_DESC_RD_SHIFT) |
			(128 << UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_REQ_SHIFT) |
			(32 << UDMA_AXI_M2S_OSTAND_CFG_MAX_COMP_DATA_WR_SHIFT);
	}
	/* Set M2S max number of outstanding transactions */
	reg_write32(&udma->udma_regs_m2s->axi_m2s.ostand_cfg, value);

	/* set AXI timeout to 100M (~118ms) */
	reg_write32(&udma->gen_axi_regs->cfg_1, 100 * 1000 * 1000);

	/* Ack time out */
	reg_write32(&udma->udma_regs_m2s->m2s_comp.cfg_application_ack, 0);

	/* set max packet size to maximum */
	udma_m2s_packet_size_cfg_set(udma, &conf);

	/* set packet stream cfg:
		rd_mode: This will enable threshold mode for the stream
		rd_th: This is threshold for number of data beats before data can be sent to the stream. Set default to 1.
	*/
	reg_write32(&udma->udma_regs_m2s->m2s.stream_cfg,
		UDMA_M2S_STREAM_CFG_RD_MODE | ((1 << UDMA_M2S_STREAM_CFG_RD_TH_SHIFT) & UDMA_M2S_STREAM_CFG_RD_TH_MASK));

	/* Set addr_hi selectors */
	gen_ex_regs = (struct udma_gen_ex_regs __iomem *)udma->gen_ex_regs;
	for (i = 0; i < DMA_MAX_Q_V4; i++)
		reg_write32(&gen_ex_regs->vmpr_v4[i].tx_sel, 0xffffffff);

	/* Set M2S data read master configuration */
	ndhal->ndhal_udma.udma_m2s_data_rd_cfg_boundaries_set(udma);

	/* Ack time out */
	reg_write32(&udma->udma_regs_s2m->s2m_comp.cfg_application_ack, 0);

	/* Set addr_hi selectors */
	gen_ex_regs = (struct udma_gen_ex_regs __iomem *)udma->gen_ex_regs;
	for (i = 0; i < DMA_MAX_Q_V4; i++) {
		reg_write32(&gen_ex_regs->vmpr_v4[i].rx_sel[0], 0xffffffff);
		reg_write32(&gen_ex_regs->vmpr_v4[i].rx_sel[1], 0xffffffff);
		reg_write32(&gen_ex_regs->vmpr_v4[i].rx_sel[2], 0xffffffff);
	}

	/* Set S2M max number of outstanding transactions */
	const u32 s2m_ostand_rd_max_desc = (udma->reserve_max_read_axi_id) ? 8 : 128;
	value = (s2m_ostand_rd_max_desc << UDMA_AXI_S2M_OSTAND_CFG_RD_MAX_DESC_RD_OSTAND_SHIFT) |
            (0x40 << UDMA_AXI_S2M_OSTAND_CFG_RD_MAX_STREAM_ACK_SHIFT);
	reg_write32(&udma->udma_regs_s2m->axi_s2m.ostand_cfg_rd, value);

	value = (128 << UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_DATA_WR_OSTAND_SHIFT) |
		(0x40 << UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_DATA_BEATS_WR_SHIFT) |
		(128 << UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_COMP_REQ_SHIFT) |
		(0x40 << UDMA_AXI_S2M_OSTAND_CFG_WR_MAX_COMP_DATA_WR_SHIFT);
	reg_write32(&udma->udma_regs_s2m->axi_s2m.ostand_cfg_wr, value);

	// Enable the completion ring head reporting by disabling bit0
	struct udma_gen_regs_v4 __iomem *gen_regs = udma->gen_regs;
	if (ndhal->arch == NEURON_ARCH_V1) {
		// Keep completion disabled for V1
		// V1 requires this fix to avoid race-condition when resetting the NC instruction buffers
		value = 0x1ul;
	} else {
		ret = reg_read32(&gen_regs->spare_reg.zeroes0, &value);
		if (ret) {
			return ret;
		}
		value &= (~0x1ul);
 	}
	reg_write32(&gen_regs->spare_reg.zeroes0, value);

	return 0;
}

#define M2S_CFG_RESET_VALUE (3<<24)
#define S2M_CFG_RESET_VALUE (3<<24)
#define M2S_RATE_LIMIT_RESET_VALUE 0b111

/* Cache frequently use CSR values.
 *
 * CSR reads are very slow and only one application(neuron) is using the DMA.
 * So instead of reading CSR use hardware reset value(from datasheet) as
 * default value.
 */
static int udma_cache_defaults(struct udma *udma)
{
	int i;
	for (i = 0; i < DMA_MAX_Q_V4; i++) {
		struct udma_q *q = &udma->udma_q_m2s[i];
		q->cfg = M2S_CFG_RESET_VALUE;
		q->rlimit_mask = M2S_RATE_LIMIT_RESET_VALUE;
		q = &udma->udma_q_s2m[i];
		q->cfg = S2M_CFG_RESET_VALUE;
	}
	return 0;
}

/** set the queue's completion configuration register
 */
static int udma_q_config_compl(struct udma_q *udma_q)
{
	u32 __iomem *reg_addr;
	u32 val;

	if (udma_q->type == UDMA_TX)
		reg_addr = &udma_q->q_regs->m2s_q.comp_cfg;
	else
		reg_addr = &udma_q->q_regs->s2m_q.comp_cfg;

	/* if completion is disabled don't bother with read/modify/write
	 * just set comp_cfg to 0
	 */
	if (udma_q->flags & UDMA_Q_FLAGS_NO_COMP_UPDATE) {
		reg_write32(reg_addr, 0);
	} else {
		int ret;
		ret = reg_read32(reg_addr, &val);
		if (ret)
			return -EIO;
		val |= UDMA_M2S_Q_COMP_CFG_EN_COMP_RING_UPDATE;
		val |= UDMA_M2S_Q_COMP_CFG_DIS_COMP_COAL;
		if (!ndhal->ndhal_ndmar.ndmar_is_nx_ring(udma_q->eng_id, udma_q->qid)) {
			val &= ~UDMA_S2M_DMA_RING_DISABLE;
		}

		reg_write32(reg_addr, val);
	}
	return 0;
}

/** reset the queues pointers (Head, Tail, etc) and set the base addresses
 */
static int udma_q_set_pointers(struct udma_q *udma_q)
{
	/* reset the descriptors ring pointers */

	BUG_ON((ADDR_LOW(udma_q->desc_phy_base) & ~UDMA_M2S_Q_TDRBP_LOW_ADDR_MASK));
	reg_write32(&udma_q->q_regs->rings.drbp_low, ADDR_LOW(udma_q->desc_phy_base));
	reg_write32(&udma_q->q_regs->rings.drbp_high, ADDR_HIGH(udma_q->desc_phy_base));

	reg_write32(&udma_q->q_regs->rings.drl, udma_q->size);

	/* if completion ring update disabled */
	if (udma_q->cdesc_base_ptr == NULL) {
		udma_q->flags |= UDMA_Q_FLAGS_NO_COMP_UPDATE;
	} else {
		/* reset the completion descriptors ring pointers */
		/* assert completion base address aligned. */
		BUG_ON((ADDR_LOW(udma_q->cdesc_phy_base) & ~UDMA_M2S_Q_TCRBP_LOW_ADDR_MASK));
		reg_write32(&udma_q->q_regs->rings.crbp_low, ADDR_LOW(udma_q->cdesc_phy_base));
		reg_write32(&udma_q->q_regs->rings.crbp_high, ADDR_HIGH(udma_q->cdesc_phy_base));
	}
	udma_q_config_compl(udma_q);
	return 0;
}

/** enable/disable udma queue
 */
static void udma_q_enable(struct udma_q *udma_q, int enable)
{
	u32 reg;

	BUG_ON(udma_q == NULL);

	reg = udma_q->cfg;
	if (enable) {
		reg |= (UDMA_M2S_Q_CFG_EN_PREF | UDMA_M2S_Q_CFG_EN_SCHEDULING);
		udma_q->status = QUEUE_ENABLED;
	} else {
		reg &= ~(UDMA_M2S_Q_CFG_EN_PREF | UDMA_M2S_Q_CFG_EN_SCHEDULING);
		udma_q->status = QUEUE_DISABLED;
	}
	reg_write32(&udma_q->q_regs->rings.cfg, reg);
	udma_q->cfg = reg;
}

/** Initialize UDMA handle and allow to read current statuses from registers
 */
static int udma_handle_init_aux(struct udma *udma, struct udma_params *udma_params)
{
	int i;

	/* note, V1 hardware uses DMA rev4, no need to support other version */
	udma->rev_id = UDMA_REV_ID_4;
	udma->num_of_queues_max = DMA_MAX_Q_V4;
	udma->reserve_max_read_axi_id = udma_params->reserve_max_read_axi_id;

	if (udma_params->num_of_queues == UDMA_NUM_QUEUES_MAX)
		udma->num_of_queues = udma->num_of_queues_max;
	else
		udma->num_of_queues = udma_params->num_of_queues;

	if (udma->num_of_queues > udma->num_of_queues_max) {
		pr_err("invalid num_of_queues parameter: %d, max: %d\n", udma->num_of_queues,
		       udma->num_of_queues_max);
		return -EINVAL;
	}

	struct unit_regs_v4 __iomem *unit_regs;
	unit_regs = (struct unit_regs_v4 __iomem *)udma_params->udma_regs_base;
	udma->unit_regs_base = (void __iomem *)unit_regs;
	udma->gen_regs = &unit_regs->gen;
	udma->gen_axi_regs = &unit_regs->gen.axi;
	udma->gen_int_regs = &unit_regs->gen.interrupt_regs;
	udma->gen_ex_regs = &unit_regs->gen_ex;
	udma->udma_regs_m2s = (struct udma_m2s_regs_v4 __iomem *)&unit_regs->m2s;
	udma->udma_regs_s2m = (struct udma_s2m_regs_v4 __iomem *)&unit_regs->s2m;

	if (udma_params->name == NULL)
		strscpy(udma->name, "NONE", UDMA_INSTANCE_NAME_LEN);
	else
		strscpy(udma->name, udma_params->name, UDMA_INSTANCE_NAME_LEN);

	for (i = 0; i < udma->num_of_queues_max; i++) {
		udma->udma_q_m2s[i].q_regs =
			(union udma_q_regs __iomem *)&udma->udma_regs_m2s->m2s_q[i];
		udma->udma_q_s2m[i].q_regs =
			(union udma_q_regs __iomem *)&udma->udma_regs_s2m->s2m_q[i];
		udma->udma_q_m2s[i].udma = udma;
		udma->udma_q_s2m[i].udma = udma;
		udma->udma_q_m2s[i].status = QUEUE_ENABLED;
		udma->udma_q_s2m[i].status = QUEUE_ENABLED;

		// We enable the DMA queues on init and never disable them
		udma_q_enable(&udma->udma_q_m2s[i], 1);
		udma_q_enable(&udma->udma_q_s2m[i], 1);
	}

	udma->state_m2s = UDMA_DISABLE;
	udma->state_s2m = UDMA_DISABLE;

	return 0;
}

int udma_init(struct udma *udma, struct udma_params *udma_params)
{
	int ret;
	u32 val;

	BUG_ON(udma == NULL);
	BUG_ON(udma_params == NULL);

	ret = udma_handle_init_aux(udma, udma_params);
	if (ret)
		return ret;

	/* initialize configuration registers to correct values */
	ret = udma_set_defaults(udma);
	if (ret)
		return ret;

	/* unmask error interrupts */
	udma_iofic_m2s_error_ints_unmask(udma);
	udma_iofic_s2m_error_ints_unmask(udma);

	udma->cdesc_size = udma_params->cdesc_size;
	val = (1 << UDMA_S2M_COMP_CFG_1C_Q_PROMOTION_SHIFT) |
	      (udma_params->cdesc_size >> 2) << UDMA_S2M_COMP_CFG_1C_DESC_SIZE_SHIFT; /* the register expects it to be in words */
	reg_write32(&udma->udma_regs_s2m->s2m_comp.cfg_1c, val);
	ret = udma_cache_defaults(udma);
	if (ret)
		return ret;

	pr_debug("%s initialized. base m2s: %p, s2m: %p\n", udma->name, udma->udma_regs_m2s,
		 udma->udma_regs_s2m);
	return 0;
}

/** Helper to validate parameters passed queue init/reinit functions.
 */
static int udma_q_init_validate(struct udma *udma, u32 qid, struct udma_q_params *q_params)
{
	struct udma_q *udma_q;

	BUG_ON(udma == NULL);
	BUG_ON(q_params == NULL);

	if (qid >= udma->num_of_queues) {
		pr_err("invalid queue id (%d)\n", qid);
		return -EINVAL;
	}

	if (q_params->size < UDMA_MIN_Q_SIZE) {
		pr_err("queue%d size(%d) too small\n", qid, q_params->size);
		return -EINVAL;
	}

	if (q_params->size > UDMA_MAX_Q_SIZE) {
		pr_err("queue%d size(%d) too large\n", qid, q_params->size);
		return -EINVAL;
	}

	// the h/w only supports rings base addr and end addr that are 256 byte aligned
	size_t queue_size_bytes = q_params->size * sizeof(union udma_desc);
	if ((HAS_ALIGNMENT(q_params->desc_phy_base, UDMA_QUEUE_ADDR_BYTE_ALIGNMENT) && HAS_ALIGNMENT(q_params->desc_phy_base + queue_size_bytes, UDMA_QUEUE_ADDR_BYTE_ALIGNMENT)) == false) {
		pr_err("queue%d has invalid alignment (start addr and end addr must be 256 byte aligned): base addr: 0x%llx ring size: %lu bytes\n", qid, q_params->desc_phy_base, q_params->size * sizeof(union udma_desc));
		return -EINVAL;
	}

	udma_q = (q_params->type == UDMA_TX) ? &udma->udma_q_m2s[qid] : &udma->udma_q_s2m[qid];
	if (udma_q->cdesc_base_ptr && udma->cdesc_size == 0) {
		pr_err("queue%d completion not configured for the engine\n", qid);
		return -EIO;
	}

	return 0;
}

int udma_q_pause(struct udma_q *udma_q)
{
	udma_q_enable(udma_q, 0);
	return 0;
}

/*
 * Reset a udma queue
 */
static int udma_q_reset(struct udma_q *udma_q)
{
	u32 __iomem *q_sw_ctrl_reg;

	BUG_ON(udma_q->cdesc_size != 0);

	udma_q_pause(udma_q);

	/* Assert the queue reset */
	if (udma_q->type == UDMA_TX) {
		q_sw_ctrl_reg = &udma_q->q_regs->m2s_q.q_sw_ctrl;
		reg_write32(q_sw_ctrl_reg, UDMA_M2S_Q_SW_CTRL_RST_Q);
	} else {
		q_sw_ctrl_reg = &udma_q->q_regs->s2m_q.q_sw_ctrl;
		reg_write32(q_sw_ctrl_reg, UDMA_S2M_Q_SW_CTRL_RST_Q);
	}
	return 0;
}


/** Initializes the udma queue data structure.
 */
static void udma_q_init_internal(struct udma *udma, u32 qid, struct udma_q_params *q_params)
{
	struct udma_q *udma_q;

	udma_q = (q_params->type == UDMA_TX) ? &udma->udma_q_m2s[qid] : &udma->udma_q_s2m[qid];
	udma_q->type = q_params->type;
	udma_q->adapter_rev_id = q_params->adapter_rev_id;
	udma_q->size = q_params->size;
	udma_q->size_mask = q_params->size - 1;
	udma_q->desc_base_ptr = q_params->desc_base;
	udma_q->desc_phy_base = q_params->desc_phy_base;
	udma_q->cdesc_base_ptr = q_params->cdesc_base;
	udma_q->cdesc_phy_base = q_params->cdesc_phy_base;
	udma_q->eng_id = q_params->eng_id;
	if (udma_q->cdesc_base_ptr == NULL) // completion is disabled
		udma_q->cdesc_size = 0;
	else
		udma_q->cdesc_size = udma->cdesc_size; // the size is per engine
	udma_q->next_desc_idx = 0;
	udma_q->is_allocatable = q_params->allocatable;
	udma_q->next_cdesc_idx = 0;
	udma_q->end_cdesc_ptr =
		(u8 *)udma_q->cdesc_base_ptr + (udma_q->size - 1) * udma_q->cdesc_size;
	udma_q->comp_head_idx = 0;
	udma_q->comp_head_ptr = (union udma_cdesc *)udma_q->cdesc_base_ptr;
	udma_q->desc_ring_id = UDMA_INITIAL_RING_ID;
	udma_q->comp_ring_id = UDMA_INITIAL_RING_ID;
	udma_q->pkt_crnt_descs = 0;
	udma_q->flags = 0;
	udma_q->status = QUEUE_DISABLED;
	udma_q->udma = udma;
	udma_q->qid = qid;

	ndhal->ndhal_udma.udma_q_config(udma_q);

	/* clear all queue ptrs */
	udma_q_reset(udma_q);

	/* reset the queue pointers */
	udma_q_set_pointers(udma_q);

	udma_q_enable(udma_q, 1);
}

/** Validates and Initializes the udma queue data structure and hardware.
 */
int udma_q_init(struct udma *udma, u32 qid, struct udma_q_params *q_params)
{
	int ret;

	ret = udma_q_init_validate(udma, qid, q_params);
	if (ret)
		return ret;
	udma_q_init_internal(udma, qid, q_params);

	return 0;
}

/**
 * check if hardware state indicated a queue is enabled
 *
 * @param udma_q
 *
 * @return true if queue is enabled, false otherwise
 */
static bool udma_q_is_enabled(struct udma_q *udma_q)
{
	u32 reg;
	int ret;

	ret = reg_read32(&udma_q->q_regs->rings.cfg, &reg);
	if (ret)
		return false;

	if (reg & (UDMA_M2S_Q_CFG_EN_PREF | UDMA_M2S_Q_CFG_EN_SCHEDULING))
		return true;

	return false;
}

/*
 * return (by reference) a pointer to a specific queue date structure.
 */
int udma_q_handle_get(struct udma *udma, u32 qid, enum udma_type type, struct udma_q **q_handle)
{
	if (unlikely(qid >= udma->num_of_queues)) {
		pr_err("%s: invalid queue id (%d)\n", udma->name, qid);
		return -EINVAL;
	}
	if (type == UDMA_TX)
		*q_handle = &udma->udma_q_m2s[qid];
	else
		*q_handle = &udma->udma_q_s2m[qid];
	return 0;
}

/*
 * Change the UDMA's state
 */
int udma_state_set(struct udma *udma, enum udma_state state)
{
	u32 reg;

	BUG_ON(udma == NULL);

	reg = 0;
	switch (state) {
	case UDMA_DISABLE:
		reg |= UDMA_M2S_CHANGE_STATE_DIS;
		break;
	case UDMA_NORMAL:
		reg |= UDMA_M2S_CHANGE_STATE_NORMAL;
		break;
	case UDMA_ABORT:
		reg |= UDMA_M2S_CHANGE_STATE_ABORT;
		break;
	default:
		pr_err("invalid state %d\n", state);
		return -EINVAL;
	}

	reg_write32(&udma->udma_regs_m2s->m2s.change_state, reg);
	reg_write32(&udma->udma_regs_s2m->s2m.change_state, reg);

	udma->state_m2s = state;
	udma->state_s2m = state;
	return 0;
}

#define UDMA_S2M_STREAM_FLUSH                                                                      \
	(UDMA_S2M_STREAM_CFG_DISABLE | UDMA_S2M_STREAM_CFG_FLUSH |                                 \
	 UDMA_S2M_STREAM_CFG_STOP_PREFETCH)

/**Determine if stream is enabled or disabled (flushing new packets)
 */
static bool udma_s2m_stream_status_get(struct udma *udma)
{
	unsigned int i;
	u32 stream_cfg = 0;
	int rc = 0;
	bool queue_stream_status;
	bool queue_stream_status_valid = false;
	bool stream_status = true;

	BUG_ON(udma == NULL);

	reg_read32(&udma->udma_regs_s2m->s2m.stream_cfg, &stream_cfg);
	stream_cfg &= UDMA_S2M_STREAM_FLUSH;

	if (stream_cfg == UDMA_S2M_STREAM_FLUSH)
		stream_status = false; /** stream is disabled */

	queue_stream_status = false;
	for (i = 0; i < udma->num_of_queues; i++) {
		struct udma_q *dma_q = NULL;
		u32 __iomem *reg_addr;
		u32 reg_val;

		rc = udma_q_handle_get(udma, i, UDMA_RX, &dma_q);
		if (rc != 0 || !udma_q_is_enabled(dma_q))
			continue;

		queue_stream_status_valid = true;

		reg_addr = &dma_q->q_regs->s2m_q.cfg;
		reg_read32(reg_addr, &reg_val);

		/** Need all queues stream interface disabled */
		if (reg_val & UDMA_S2M_Q_CFG_EN_STREAM) {
			queue_stream_status = true;
			break;
		}
	}

	if (queue_stream_status_valid && (queue_stream_status != stream_status)) {
		pr_err("bad config, stream & queue stream interface status are different! assuming stream is enabled\n");
		/** Need both interfaces disabled to stop stream */
		return true;
	}

	return stream_status;
}

/** return the current UDMA hardware state
 */
enum udma_state udma_state_get(struct udma *udma, enum udma_type type)
{
	u32 state_reg;
	u32 comp_ctrl;
	u32 stream_if;
	u32 data_rd;
	u32 desc_pref;
	bool stream_enabled;

	if (type == UDMA_TX) {
		reg_read32(&udma->udma_regs_m2s->m2s.state, &state_reg);
		stream_enabled = true;
	} else {
		reg_read32(&udma->udma_regs_s2m->s2m.state, &state_reg);
		stream_enabled = udma_s2m_stream_status_get(udma);
	}

	comp_ctrl = REG_FIELD_GET(state_reg, UDMA_M2S_STATE_COMP_CTRL_MASK,
				  UDMA_M2S_STATE_COMP_CTRL_SHIFT);
	stream_if = REG_FIELD_GET(state_reg, UDMA_M2S_STATE_STREAM_IF_MASK,
				  UDMA_M2S_STATE_STREAM_IF_SHIFT);
	data_rd = REG_FIELD_GET(state_reg, UDMA_M2S_STATE_DATA_RD_CTRL_MASK,
				UDMA_M2S_STATE_DATA_RD_CTRL_SHIFT);
	desc_pref = REG_FIELD_GET(state_reg, UDMA_M2S_STATE_DESC_PREF_MASK,
				  UDMA_M2S_STATE_DESC_PREF_SHIFT);

	/* Due to a HW bug, in case stream is disabled but there are packets waiting to enter
	 * the UDMA, the stream_if might be "stuck" at "1"
	 *
	 * We can ignore the stream_if indication if :
	 * 1. The stream is disabled for UDMA and for each queue -
	 *    new packets cannot enter the UDMA or its queues.
	 * 2. We waited at least 1us for the FIFO's to be emptied
	 */

	/* if any of the states is abort then return abort */
	if (stream_enabled) {
		if ((comp_ctrl == UDMA_STATE_ABORT) || (stream_if == UDMA_STATE_ABORT) ||
		    (data_rd == UDMA_STATE_ABORT) || (desc_pref == UDMA_STATE_ABORT))
			return UDMA_ABORT;
	} else {
		if ((comp_ctrl == UDMA_STATE_ABORT) || (desc_pref == UDMA_STATE_ABORT) ||
		    (data_rd == UDMA_STATE_ABORT))
			return UDMA_ABORT;
	}

	if (stream_enabled) {
		/* if any of the states is normal then return normal */
		if ((comp_ctrl == UDMA_STATE_NORMAL) || (stream_if == UDMA_STATE_NORMAL) ||
		    (data_rd == UDMA_STATE_NORMAL) || (desc_pref == UDMA_STATE_NORMAL))
			return UDMA_NORMAL;
	} else {
		if ((comp_ctrl == UDMA_STATE_NORMAL) || (desc_pref == UDMA_STATE_NORMAL) ||
		    (data_rd == UDMA_STATE_NORMAL))
			return UDMA_NORMAL;
	}

	return UDMA_IDLE;
}

/* Increment tail pointer of a DMA queue, that starts data transfer by the queue */
void udma_desc_action_add(struct udma_q *udma_q, u32 num)
{
	u32 __iomem *addr;

	BUG_ON(udma_q == NULL);
	BUG_ON((num == 0) || (num > udma_q->size));

	addr = &udma_q->q_regs->rings.drtp_inc;
	mb(); // to make sure data written to the descriptors will be visible to the DMA
	reg_write32(addr, num);
}
