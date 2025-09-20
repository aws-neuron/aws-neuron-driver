// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>

#include "udma.h"
#include "../neuron_arch.h"

/* Note on terminology:
 * for historical reasons the code uses both m2s/s2m and Tx/Rx terminology
 * m2s (memory to stream) is the same as Tx (and source) and s2m (stream to memory) is the same
 * as Rx (and destination)
 * The use cases here always copy between two memory locations by creating paired m2s and s2m
 * descriptors. Thus the functions are called ..._m2m_... 
 */

/* meta control (per descriptor) */
union tdma_m2s_meta_ctrl {
	struct {
		u32 block_attr_is_last_block : 1;
		u32 block_attr_is_first_block : 1;
		u32 block_attr_rsvd : 1;
		u32 cached_crc_iv_index : 3;

		u32 endianness_op_result_byte_swap : 1;
		u32 endianness_op_result_bit_swap : 1;
		u32 endianness_op_intr_val_byte_swap : 1;
		u32 endianness_op_intr_val_bit_swap : 1;
		u32 endianness_op_src_byte_swap : 1;
		u32 endianness_op_src_bit_swap : 1;
		u32 endianness_op_init_byte_swap : 1;
		u32 endianness_op_init_bit_swap : 1;

		u32 validate_crc : 1;
		u32 use_stored_crc_iv : 1;
		u32 send_crc_result : 1;
		u32 store_crc_result : 1;
		u32 store_source_crc : 1;
		u32 copy_source_data : 1;
		u32 op_type : 3;
		u32 ssmae_op : 3;
		u32 rsvd0 : 6;
	};
	u32 uint32_data;
};

/* meta is currently unmodifiable, use the same value for all descriptors */
static const union tdma_m2s_meta_ctrl tdma_m2s_meta_ctrl_default_value = { {
	.block_attr_is_last_block = 1,
	.block_attr_is_first_block = 1,
	.block_attr_rsvd = 0,

	.cached_crc_iv_index = 0,

	.endianness_op_result_byte_swap = 0,
	.endianness_op_result_bit_swap = 0,
	.endianness_op_intr_val_byte_swap = 0,
	.endianness_op_intr_val_bit_swap = 0,
	.endianness_op_src_byte_swap = 0,
	.endianness_op_src_bit_swap = 0,
	.endianness_op_init_byte_swap = 0,
	.endianness_op_init_bit_swap = 0,

	.validate_crc = 0,
	.use_stored_crc_iv = 0,
	.send_crc_result = 0,
	.store_crc_result = 0,
	.store_source_crc = 0,
	.copy_source_data = 1,
	.op_type = 0,
	.ssmae_op = 0b010,

	.rsvd0 = 0,
} };

struct sdma_cme_desc_word {
	uint32_t block_attr : 3;
	uint32_t rsvd_5_to_3 : 3;
	uint32_t endian_type : 8;
	uint32_t validate_crc : 1;
	uint32_t rsvd_15 : 1;
	uint32_t send_crc : 1;
	uint32_t rsvd_18_to_17 : 2;
	uint32_t copy_src_data : 1;
	uint32_t optype : 3;
	uint32_t op : 3;
	uint32_t write_barrier : 1;
	uint32_t netag0_rsvd : 5;
} __attribute__((__packed__));

static void sdma_m2s_set_write_barrier(uint32_t *meta_ctrl)
{
	((struct sdma_cme_desc_word *)meta_ctrl)->write_barrier = 1;
	return;
}

/* set maximum number descriptors per one DMA packet */
static int udma_set_max_descs_and_prefetch(struct udma *udma, u8 max_descs)
{
	// Due to DGE bug on V3 (https://tiny.amazon.com/tfw2hept)
	// Min burst must equal Max burst, which is 8

	u32 pref_thr = max_descs;
	static const u32 min_burst_above_thr = 8;
	static const u32 max_burst = 8;
	static const u32 always_break_on_max_boundary = 1;
	u32 value;
	// max_descs is a large number (currently >= 64) make sure it's so
	// and configure min/max prefetch to always be 8
	if (max_descs < 8 || max_descs > UDMA_M2S_MAX_ALLOWED_DESCS_PER_PACKET_V4) {
		pr_err("invalid number of descriptors %d(max %d)\n", max_descs,
		       UDMA_M2S_MAX_ALLOWED_DESCS_PER_PACKET_V4);
		return -1;
	}

	value = (max_descs << UDMA_M2S_RD_DESC_PREF_CFG_2_MAX_DESC_PER_PKT_SHIFT) |
		(1 << UDMA_M2S_RD_DESC_PREF_CFG_2_PERF_FORCE_RR_SHIFT);
	reg_write32(&udma->udma_regs_m2s->m2s_rd.desc_pref_cfg_2, value);

	value = (pref_thr << UDMA_M2S_RD_DESC_PREF_CFG_3_PREF_THR_SHIFT) |
		(min_burst_above_thr << UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_SHIFT) |
		(1 << UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_BELOW_THR_SHIFT);
	reg_write32(&udma->udma_regs_m2s->m2s_rd.desc_pref_cfg_3, value);

	// likely harmless, but just in case, keep the old V1 behavior where 
	// we did not change default for s2m.  V1 support is on the way out,
	// once it's deprecated just remove this comment and the "if"
	if (narch_get_arch() != NEURON_ARCH_V1) {
		value = (pref_thr << UDMA_S2M_RD_DESC_PREF_CFG_3_PREF_THR_SHIFT) |
			(min_burst_above_thr << UDMA_S2M_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_SHIFT) |
			(1 << UDMA_S2M_RD_DESC_PREF_CFG_3_MIN_BURST_BELOW_THR_SHIFT);
		reg_write32(&udma->udma_regs_s2m->s2m_rd.desc_pref_cfg_3, value);
		// configure max_burst for both m2s and s2m
		value = (max_burst << UDMA_AXI_M2S_DESC_RD_CFG_3_MAX_AXI_BEATS_SHIFT) |
			(always_break_on_max_boundary << UDMA_AXI_M2S_DESC_RD_CFG_3_ALWAYS_BREAK_ON_MAX_BOUNDARY_SHIFT);
		reg_write32(&udma->udma_regs_m2s->axi_m2s.desc_rd_cfg_3, value);
		value = (max_burst << UDMA_AXI_S2M_DESC_RD_CFG_3_MAX_AXI_BEATS_SHIFT) |
			(always_break_on_max_boundary << UDMA_AXI_S2M_DESC_RD_CFG_3_ALWAYS_BREAK_ON_MAX_BOUNDARY_SHIFT);
		reg_write32(&udma->udma_regs_s2m->axi_s2m.desc_rd_cfg_3, value);
	}
	return 0;
}

/* initialize one DMA queue on one DMA engine */
int udma_m2m_init_queue(struct udma *udma, int qid, u32 eng_id, u32 m2s_ring_size, u32 s2m_ring_size,
			bool allocatable, struct udma_ring_ptr *m2s_ring,
			struct udma_ring_ptr *s2m_ring, struct udma_ring_ptr *s2m_compl_ring)
{
	int ret;
	struct udma_q_params qp;

	BUG_ON(udma == NULL);
	/* the h/w only supports rings base addr and end addr that are 256 byte aligned, check both m2s & s2m */
	if (m2s_ring != NULL && HAS_ALIGNMENT(m2s_ring->addr, UDMA_QUEUE_ADDR_BYTE_ALIGNMENT) == false) {
		pr_err("invalid m2s ring alignment. Start addr must be %u byte aligned. base addr: 0x%llx\n", UDMA_QUEUE_ADDR_BYTE_ALIGNMENT, m2s_ring->addr);
		return -1;
	}
	if ((m2s_ring_size % (UDMA_QUEUE_ADDR_BYTE_ALIGNMENT / sizeof(union udma_desc))) != 0) {
		pr_err("invalid m2s ring size. Ring size must be a multiple of %lu. ring size: %u descs\n", UDMA_QUEUE_ADDR_BYTE_ALIGNMENT / sizeof(union udma_desc), m2s_ring_size);
		return -1;
	}
	if (s2m_ring != NULL && HAS_ALIGNMENT(s2m_ring->addr, UDMA_QUEUE_ADDR_BYTE_ALIGNMENT) == false) {
		pr_err("invalid s2m ring alignment. Start addr must be %u byte aligned. base addr: 0x%llx\n", UDMA_QUEUE_ADDR_BYTE_ALIGNMENT, s2m_ring->addr);
		return -1;
	}
	if ((s2m_ring_size % (UDMA_QUEUE_ADDR_BYTE_ALIGNMENT / sizeof(union udma_desc))) != 0) {
		pr_err("invalid s2m ring size. Ring size must be a multiple of %lu. ring size: %u descs\n", UDMA_QUEUE_ADDR_BYTE_ALIGNMENT / sizeof(union udma_desc), s2m_ring_size);
		return -1;
	}

	if (s2m_compl_ring != NULL) { // completion ring not supported
		pr_err("Completion ring not supported\n");
		return -1;
	}
	/* either don't use the completion ring or the ring must exist */
	if (s2m_compl_ring && s2m_compl_ring->ptr == NULL) {
		pr_err("invalid completion ring\n");
		return -1;
	}
	/* note we never use completion for m2s and this function does not support it */

	memset(&qp, 0, sizeof(qp));
	qp.adapter_rev_id = 0;
	qp.type = UDMA_TX;
	qp.size = m2s_ring_size;
	if (m2s_ring) {
		qp.desc_base = m2s_ring->ptr;
		qp.desc_phy_base = m2s_ring->addr;
	}
	qp.cdesc_base = NULL; // no completion for TX
	qp.cdesc_phy_base = 0;
	qp.allocatable = allocatable;
	qp.eng_id = eng_id;
	ret = udma_q_init(udma, qid, &qp);
	if (ret) {
		pr_err("failed to init m2s queue %d, err: %d\n", qid, ret);
		return ret;
	}

	memset(&qp, 0, sizeof(qp));
	qp.adapter_rev_id = 0;
	qp.type = UDMA_RX;
	qp.size = s2m_ring_size;
	if (s2m_ring) {
		qp.desc_base = s2m_ring->ptr;
		qp.desc_phy_base = s2m_ring->addr;
	}

	/* completion for RX is optional, the caller can pass NULL to disable */
	if (s2m_compl_ring) {
		qp.cdesc_base = s2m_compl_ring->ptr;
		qp.cdesc_phy_base = s2m_compl_ring->addr;
	} else {
		qp.cdesc_base = NULL;
		qp.cdesc_phy_base = 0;
	}
	qp.allocatable = allocatable;
	ret = udma_q_init(udma, qid, &qp);
	if (ret) {
		pr_err("failed to init s2m queue %d, err: %d\n", qid, ret);
		return ret;
	}
	return ret;
}

/* initialize one DMA engine */
int udma_m2m_init_engine(struct udma *udma, void __iomem *regs_base, int num_queues, char *eng_name,
			 int disable_phase_bit, int max_desc_per_packet, bool reserve_max_read_axi_id)
{
	int ret;
	struct udma_params params;

	if (udma == NULL || regs_base == NULL) {
		return -1;
	}

	params.udma_regs_base = regs_base;
	params.num_of_queues = num_queues;
	params.name = eng_name;
	params.cdesc_size = UDMA_CDESC_SIZE;
	params.flags = 0;

	if (disable_phase_bit) {
		params.flags |= UDMA_ENG_CFG_FLAGS_DISABLE_ERROR;
		params.flags |= UDMA_ENG_CFG_FLAGS_DISABLE_ABORT;
	}

	params.reserve_max_read_axi_id = reserve_max_read_axi_id;

	ret = udma_init(udma, &params);
	if (ret) {
		pr_err("failed to init engine: %p, err: %d\n", regs_base, ret);
		return ret;
	}

	/* set packet size to defined MAX. */
	return udma_set_max_descs_and_prefetch(udma, max_desc_per_packet);
}

/* build one m2s (TX) descriptor */
static int udma_m2m_build_tx_descriptor(union udma_desc *tx_desc_ptr, u32 tx_ring_id,
					dma_addr_t s_addr, u32 size, u32 meta_ctrl, u32 flags)
{
	union udma_desc tx_desc = {};
	u32 flags_len = flags;

	if (size > MAX_DMA_DESC_SIZE) {
		return -1;
	}
	/* use DMA feature to transmit max 64K of data in one descriptor instead of 64k-1 */
	if (size == MAX_DMA_DESC_SIZE) {
		size = 0;
	}

	flags_len |= tx_ring_id << M2S_DESC_RING_ID_SHIFT;
	flags_len |= size & M2S_DESC_LEN_MASK;

	tx_desc.tx.len_ctrl = flags_len;
	tx_desc.tx.buf_ptr = s_addr;
	tx_desc.tx.meta_ctrl = meta_ctrl;
	memcpy(tx_desc_ptr, (u8 *)&tx_desc, sizeof(union udma_desc));
	return 0;
}

/* build one s2m (RX) descriptor */
static int udma_m2m_build_rx_descriptor(union udma_desc *rx_desc_ptr, u32 rx_ring_id,
					dma_addr_t d_addr, u32 size, u32 flags)
{
	union udma_desc rx_desc = {};
	u32 flags_len = flags;

	if (size > MAX_DMA_DESC_SIZE) {
		return -1;
	}
	/* use DMA feature to transmit max 64K of data in one descriptor instead of 64k-1 */
	if (size == MAX_DMA_DESC_SIZE) {
		size = 0;
	}

	flags_len |= rx_ring_id << M2S_DESC_RING_ID_SHIFT;
	flags_len |= size & M2S_DESC_LEN_MASK;

	rx_desc.rx.len_ctrl = flags_len;
	rx_desc.rx.buf1_ptr = d_addr;
	memcpy(rx_desc_ptr, (u8 *)&rx_desc, sizeof(union udma_desc));
	return 0;
}

/** Build DMA descriptors in the given buffers.
 *
 * This API allows building descriptors in local memory and then dumping to queue at once.
 *
 * @param rx_desc[in] - Pointer to rx descriptor.
 * @param tx_desc[in] - Pointer to tx descriptor.
 * @param rx_ring_id[in] - Phase ID to use for the tx descriptor.
 * @param tx_ring_id[in] - Phase ID to use for the rx descriptor.
 * @param s_addr[in] - Source address.
 * @param d_addr[in] - Destination address.
 * @param size[in] - Size of the transfer
 *
 * @return 0 on success
 */
static int udma_m2m_build_descriptor(union udma_desc *rx_desc_ptr, union udma_desc *tx_desc_ptr,
				     u32 rx_ring_id, u32 tx_ring_id, dma_addr_t s_addr,
				     dma_addr_t d_addr, u32 size, int barrier_type, bool set_dst_int)
{
	int ret;
	u32 rx_flags = 0;
	uint32_t meta_ctrl = tdma_m2s_meta_ctrl_default_value.uint32_data;
	/* Just one descriptor in packet - set appropriate first/last flags */
	u32 tx_flags = M2S_DESC_FIRST | M2S_DESC_LAST;

	switch (barrier_type) {
		case UDMA_M2M_BARRIER_DMB:
			tx_flags |= M2S_DESC_DMB;
			break;
		case UDMA_M2M_BARRIER_WRITE_BARRIER:
			sdma_m2s_set_write_barrier(&meta_ctrl);
			break;
		case UDMA_M2M_BARRIER_NONE:
			break;
		default:
			pr_err("Invalid m2m barrier is given");
			return -EINVAL;
	}

	ret = udma_m2m_build_tx_descriptor(tx_desc_ptr, tx_ring_id, s_addr, size,
					   meta_ctrl, tx_flags);
	if (ret)
		return ret;

	/* if rx should generate an interrupt make it so */
	if (unlikely(set_dst_int))
		rx_flags = S2M_DESC_INT_EN;

	return udma_m2m_build_rx_descriptor(rx_desc_ptr, rx_ring_id, d_addr, size, rx_flags);
}

/* Create a pair of descriptors to copy the data from the source to the destination
 * this is a simple case of one m2s and one s2m descriptor in DMA packet
 */
int udma_m2m_copy_prepare_one(struct udma *udma, u32 qid, dma_addr_t s_addr, dma_addr_t d_addr,
			      u32 size, int barrier_type, bool set_dst_int)
{
	u32 ndesc;
	struct udma_q *txq;
	struct udma_q *rxq;
	int ret;

	BUG_ON(udma == NULL);
	if (qid >= udma->num_of_queues) {
		return -1;
	}

	if (size > MAX_DMA_DESC_SIZE) {
		return -1;
	}
	if (size == MAX_DMA_DESC_SIZE) {
		size = 0;
	}
	ret = udma_q_handle_get(udma, qid, UDMA_TX, &txq);
	if (ret)
		return ret;
	ret = udma_q_handle_get(udma, qid, UDMA_RX, &rxq);
	if (ret)
		return ret;
	/* is there enough room and the m2s and s2m rings ? */
	ndesc = udma_available_get(txq);
	if (ndesc < 1) {
		pr_err("not enough room in TX queue %d\n", txq->qid);
		return -ENOMEM;
	}
	ndesc = udma_available_get(rxq);
	if (ndesc < 1) {
		pr_err("not enough room in RX queue %d\n", rxq->qid);
		return -ENOMEM;
	}

	union udma_desc *rx_desc = udma_desc_get(rxq);
	union udma_desc *tx_desc = udma_desc_get(txq);
	return udma_m2m_build_descriptor(rx_desc, tx_desc, udma_ring_id_get(rxq),
					 udma_ring_id_get(txq), s_addr, d_addr, size, barrier_type, set_dst_int);
}

/* Start DMA data transfer for m2s_count/s2m_count number or descriptors.
 * Can also be called to prefetch s2m ahead of time.  In which case m2s_count
 * is 0.
 */
int udma_m2m_copy_start(struct udma *udma, u32 qid, u32 m2s_count, u32 s2m_count)
{
	struct udma_q *txq;
	struct udma_q *rxq;
	int ret;

	BUG_ON(udma == NULL);

	if (qid >= udma->num_of_queues) {
		return -1;
	}
	
	//BUG_ON((num == 0) || (s2m_count > txq->size));

	ret = udma_q_handle_get(udma, qid, UDMA_TX, &txq);
	if (ret)
		return ret;
	if (m2s_count > txq->size) {
		return -1;
	}
	ret = udma_q_handle_get(udma, qid, UDMA_RX, &rxq);
	if (ret)
		return ret;
	if (s2m_count > rxq->size) {
		return -1;
	}
	udma_desc_action_add(rxq, s2m_count);
	if (m2s_count > 0) {
		udma_desc_action_add(txq, m2s_count);
	}
	return ret;
}

// for more info, reference the ticket system NRT-315 and NRT-2619
void udma_m2m_set_axi_error_abort(struct udma *udma)
{
	unsigned int i, q;
	struct udma_gen_regs_v4 *gen_regs = (struct udma_gen_regs_v4 *)udma->gen_regs;

	// step 1: program axi error detection table (skip msix)
	for (i = 0; i < 6; i++) {
		reg_write32(&gen_regs->axi_error_detection_table[i].addr1, 0xf);
		reg_write32(&gen_regs->axi_error_detection_table[i].addr2, 0xf);
		reg_write32(&gen_regs->axi_error_detection_table[i].addr3, 0xf);
		reg_write32(&gen_regs->axi_error_detection_table[i].addr9, 0xf);
		reg_write32(&gen_regs->axi_error_detection_table[i].addr10, 0xf);
		reg_write32(&gen_regs->axi_error_detection_table[i].addr11, 0xf);
	}

	// step 2: program axi error control
	for (i = 0; i < 6; i++) {
		for (q = 0; q < DMA_MAX_Q_MAX; q++) {
			reg_write32(&gen_regs->axi_error_control[i].table_addr, (q << 3) | 0x7);
			reg_write32(&gen_regs->axi_error_control[i].table_data, 0x10);
		}
	}

	// step 3: unmask intc interrupts
	// TODO: which interrupts to unmask? just do all of them for now
	udma_iofic_error_ints_unmask_one((struct iofic_grp_ctrl *)&gen_regs->iofic_base_m2s_desc_rd, 0xffffffff);
	udma_iofic_error_ints_unmask_one((struct iofic_grp_ctrl *)&gen_regs->iofic_base_m2s_data_rd, 0xffffffff);
	udma_iofic_error_ints_unmask_one((struct iofic_grp_ctrl *)&gen_regs->iofic_base_m2s_cmpl_wr, 0xffffffff);
	udma_iofic_error_ints_unmask_one((struct iofic_grp_ctrl *)&gen_regs->iofic_base_s2m_desc_rd, 0xffffffff);
	udma_iofic_error_ints_unmask_one((struct iofic_grp_ctrl *)&gen_regs->iofic_base_s2m_data_wr, 0xffffffff);
	udma_iofic_error_ints_unmask_one((struct iofic_grp_ctrl *)&gen_regs->iofic_base_s2m_cmpl_wr, 0xffffffff);

	// step 4: update dma timeout config
	reg_write32(&udma->gen_axi_regs->cfg_1, 0xffffffff);
}

void udma_m2m_mask_ring_id_error(struct udma *udma, void __iomem *intc_base) {
	uint32_t ring_id_mask = 0;
	void __iomem *int_mask_addr = NULL;
	struct udma_gen_regs_v4 *gen_regs = (struct udma_gen_regs_v4 *)udma->gen_regs;

	// mask ring id errors from the udma secondary intc
	// tx
	reg_read32(&gen_regs->interrupt_regs.secondary_iofic_ctrl[0].int_mask_grp, &ring_id_mask);
	ring_id_mask |= UDMA_M2S_ERR_LOG_MASK_PREF_RING_ID;
	reg_write32(&gen_regs->interrupt_regs.secondary_iofic_ctrl[0].int_mask_grp, ring_id_mask);
	reg_write32(&gen_regs->interrupt_regs.secondary_iofic_ctrl[0].int_abort_msk_grp, ring_id_mask);
	// rx
	reg_read32(&gen_regs->interrupt_regs.secondary_iofic_ctrl[1].int_mask_grp, &ring_id_mask);
	ring_id_mask |= UDMA_S2M_ERR_LOG_MASK_PREF_RING_ID;
	reg_write32(&gen_regs->interrupt_regs.secondary_iofic_ctrl[1].int_mask_grp, ring_id_mask);
	reg_write32(&gen_regs->interrupt_regs.secondary_iofic_ctrl[1].int_abort_msk_grp, ring_id_mask);

	// mask ring id errors from the top-level intc
	// tx
	int_mask_addr = intc_base + 2 * sizeof(struct iofic_grp_ctrl) + offsetof(struct iofic_grp_ctrl, int_mask_grp);
	reg_read32(int_mask_addr, &ring_id_mask);
	ring_id_mask |= INT_CONTROL_GRP_UDMA_M2S_PREF_RING_ID;
	reg_write32(int_mask_addr, ring_id_mask);
	// // rx
	int_mask_addr = intc_base + 3 * sizeof(struct iofic_grp_ctrl) + offsetof(struct iofic_grp_ctrl, int_mask_grp);
	reg_read32(int_mask_addr, &ring_id_mask);
	ring_id_mask |= INT_CONTROL_GRP_UDMA_S2M_PREF_RING_ID;
	reg_write32(int_mask_addr, ring_id_mask);

	// mask ring id errors from the error logs
	// tx
	reg_read32(&udma->udma_regs_m2s->m2s.err_log_mask, &ring_id_mask);
	ring_id_mask |= UDMA_M2S_ERR_LOG_MASK_PREF_RING_ID;
	reg_write32(&udma->udma_regs_m2s->m2s.err_log_mask, ring_id_mask);
	// rx
	reg_read32(&udma->udma_regs_s2m->s2m.err_log_mask, &ring_id_mask);
	ring_id_mask |= UDMA_S2M_ERR_LOG_MASK_PREF_RING_ID;
	reg_write32(&udma->udma_regs_s2m->s2m.err_log_mask, ring_id_mask);
}

