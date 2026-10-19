// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/*
 * neuron_dma_diag.c — DMA engine diagnostic dump module.
 *
 * Provides a reusable diagnostic dump that can be called from any DMA
 * error path (scrub timeout, h2d timeout, etc.). The dump performs only
 * register reads and formatted prints; it does not sleep, allocate, or
 * take locks, so it is safe to invoke from error/timeout context.
 *
 * Callers should gate invocation through NDMA_DIAG_DUMP_ENGINE() (see
 * neuron_dma_diag.h) so the dump — and all of its register reads — sits
 * behind a dynamic debug static-key branch that is off by default.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "udma/udma.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_ring.h"
#include "neuron_reg_access.h"
#include "neuron_dma_diag.h"

/*
 * Index into udma.gen_int_regs->secondary_iofic_ctrl[], i.e. which of the two
 * secondary IOFIC groups a cause bit belongs to.
 */
enum ndma_diag_sec_grp {
	NDMA_DIAG_SEC_GRP_A = 0,	/* M2S error conditions */
	NDMA_DIAG_SEC_GRP_B = 1,	/* S2M error conditions */
};

struct ndma_diag_error_bit {
	u8 grp;			/* enum ndma_diag_sec_grp */
	u8 bit;			/* bit position within that group's int_cause_grp */
	const char *name;
};

/*
 * Secondary IOFIC error cause bits, keyed on (group, bit position) exactly as
 * the driver's own interrupt unmask code defines them -- see the
 * INT_2ND_GROUP_A_M2S_* / INT_2ND_GROUP_B_S2M_* macros in udma/udma_iofic.c,
 * which are the source of truth for this table.
 *
 * Do not translate these into the flat SDMA INTC trigger numbering used by the
 * SoC bringup validation tables. That numbering is not a uniform offset per
 * group: group A occupies only 29 flat slots (its bits 0-28 are triggers
 * 58-86) while group B bit 0 is trigger 87, so a single base applied to both
 * groups silently misdecodes every S2M cause. Keeping (group, bit) here means
 * this table cannot drift out of step with udma_iofic.c.
 *
 * Only error conditions likely to indicate a DMA failure root cause are listed;
 * gaps are bit positions the hardware does not define, or non-error hints.
 */
static const struct ndma_diag_error_bit known_error_bits[] = {
	/* Group A -- M2S error conditions */
	{ NDMA_DIAG_SEC_GRP_A, 0, "stream_completion_mismatch" },
	{ NDMA_DIAG_SEC_GRP_A, 1, "stream_parity" },
	{ NDMA_DIAG_SEC_GRP_A, 2, "stream_response" },
	{ NDMA_DIAG_SEC_GRP_A, 3, "stream_timeout" },
	{ NDMA_DIAG_SEC_GRP_A, 4, "completion_axi_parity" },
	{ NDMA_DIAG_SEC_GRP_A, 5, "completion_axi_response" },
	{ NDMA_DIAG_SEC_GRP_A, 6, "completion_axi_timeout" },
	{ NDMA_DIAG_SEC_GRP_A, 7, "data_axi_parity" },
	{ NDMA_DIAG_SEC_GRP_A, 8, "data_axi_response" },
	{ NDMA_DIAG_SEC_GRP_A, 9, "data_axi_timeout" },
	{ NDMA_DIAG_SEC_GRP_A, 10, "prefetch_axi_parity" },
	{ NDMA_DIAG_SEC_GRP_A, 11, "prefetch_axi_response" },
	{ NDMA_DIAG_SEC_GRP_A, 12, "prefetch_axi_timeout" },
	{ NDMA_DIAG_SEC_GRP_A, 13, "packet_length" },
	{ NDMA_DIAG_SEC_GRP_A, 14, "prefetch_max_descriptors" },
	{ NDMA_DIAG_SEC_GRP_A, 17, "prefetch_ring_id_error" },
	{ NDMA_DIAG_SEC_GRP_A, 26, "msix_timeout" },
	{ NDMA_DIAG_SEC_GRP_A, 27, "msix_response" },
	/* Group B -- S2M error conditions */
	{ NDMA_DIAG_SEC_GRP_B, 0, "data_axi_parity_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 1, "data_axi_response_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 2, "data_axi_timeout_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 6, "no_descriptors_timeout" },
	{ NDMA_DIAG_SEC_GRP_B, 7, "no_descriptors_hint" },
	{ NDMA_DIAG_SEC_GRP_B, 8, "prefetch_axi_parity_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 9, "prefetch_axi_response_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 10, "prefetch_axi_timeout_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 11, "prefetch_ring_id_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 13, "completion_axi_parity_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 14, "completion_axi_response_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 15, "completion_axi_timeout_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 16, "completion_stream" },
	{ NDMA_DIAG_SEC_GRP_B, 17, "completion_unack" },
	{ NDMA_DIAG_SEC_GRP_B, 18, "stream_header_parity" },
	{ NDMA_DIAG_SEC_GRP_B, 19, "stream_header_error" },
	{ NDMA_DIAG_SEC_GRP_B, 20, "stream_data_parity" },
	{ NDMA_DIAG_SEC_GRP_B, 21, "stream_data" },
	{ NDMA_DIAG_SEC_GRP_B, 24, "packet_length_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 26, "data_parity_s2m" },
	{ NDMA_DIAG_SEC_GRP_B, 30, "prefetch_desc_parity_s2m" },
};

/* --- Diagnostic dump sub-functions --- */

#define DIAG_PFX "NEURON_DMA_DIAG"

static void ndma_diag_eng_state(struct ndma_eng *eng,
				const char *reason)
{
	struct neuron_dma_eng_state state;
	u32 m2s_state = 0, s2m_state = 0;
	int ret;

	ret = ndmar_eng_get_state(eng->nd, eng->eng_id, &state);
	if (ret) {
		pr_err(DIAG_PFX "[%s] nd%02d %s: engine state read failed (%d)\n",
		       reason, eng->nd->device_index, eng->udma.name, ret);
		return;
	}

	/* Read raw M2S/S2M state registers for additional context */
	if (eng->udma.udma_regs_m2s && eng->udma.udma_regs_s2m) {
		reg_read32(&eng->udma.udma_regs_m2s->m2s.state, &m2s_state);
		reg_read32(&eng->udma.udma_regs_s2m->s2m.state, &s2m_state);
	}

	pr_err(DIAG_PFX "[%s] nd%02d %s: tx_state=%u rx_state=%u rev_id=%u num_q=%u raw_m2s=0x%x raw_s2m=0x%x\n",
	       reason, eng->nd->device_index, eng->udma.name,
	       state.tx_state, state.rx_state, state.revision_id,
	       state.num_queues, m2s_state, s2m_state);
}

static void ndma_diag_failing_queue(struct ndma_eng *eng,
				    struct ndma_ring *ring,
				    u32 desc_count,
				    const char *reason)
{
	struct neuron_dma_queue_state tx, rx;
	struct udma_q *m2s_q, *s2m_q;
	u32 tx_dcp, rx_dcp, tx_status_raw, rx_status_raw;
	u32 tx_drhp_raw, rx_drhp_raw;
	int ret;

	ret = ndmar_queue_get_state(eng->nd, eng->eng_id, ring->qid, &tx, &rx);
	if (ret) {
		pr_err(DIAG_PFX "[%s] nd%02d %s: q%d state read failed (%d)\n",
		       reason, eng->nd->device_index, eng->udma.name, ring->qid, ret);
		return;
	}

	pr_err(DIAG_PFX "[%s] nd%02d %s: q%d desc_count=%u TX hw_status=%u sw_status=%u head=%u tail=%u comp_head=%u length=%u base_addr=0x%llx\n",
	       reason, eng->nd->device_index, eng->udma.name, ring->qid,
	       desc_count, tx.hw_status, tx.sw_status, tx.head_pointer,
	       tx.tail_pointer, tx.completion_head, tx.length,
	       tx.base_addr);
	pr_err(DIAG_PFX "[%s] nd%02d %s: q%d RX hw_status=%u sw_status=%u head=%u tail=%u comp_head=%u length=%u base_addr=0x%llx\n",
	       reason, eng->nd->device_index, eng->udma.name, ring->qid,
	       rx.hw_status, rx.sw_status, rx.head_pointer,
	       rx.tail_pointer, rx.completion_head, rx.length,
	       rx.base_addr);

	/*
	 * Read the Descriptor Current Pointer (DCP) — this tells us where
	 * the engine is currently *executing*, distinct from head (prefetched).
	 * If DCP == head, engine consumed all prefetched descs and is stalled
	 * waiting for prefetch. If DCP < head, data path itself is stalled.
	 *
	 * Also re-read the raw DRHP to capture the Ring ID bits [31:30].
	 */
	m2s_q = &eng->udma.udma_q_m2s[ring->qid];
	s2m_q = &eng->udma.udma_q_s2m[ring->qid];

	if (!m2s_q->q_regs || !s2m_q->q_regs)
		return;

	tx_dcp = 0; rx_dcp = 0;
	tx_drhp_raw = 0; rx_drhp_raw = 0;
	tx_status_raw = 0; rx_status_raw = 0;

	reg_read32(&m2s_q->q_regs->rings.dcp, &tx_dcp);
	reg_read32(&s2m_q->q_regs->rings.dcp, &rx_dcp);
	reg_read32(&m2s_q->q_regs->rings.drhp, &tx_drhp_raw);
	reg_read32(&s2m_q->q_regs->rings.drhp, &rx_drhp_raw);
	reg_read32(&m2s_q->q_regs->rings.status, &tx_status_raw);
	reg_read32(&s2m_q->q_regs->rings.status, &rx_status_raw);

	pr_err(DIAG_PFX "[%s] nd%02d %s: q%d TX dcp=%u drhp=0x%08x status=0x%08x RX dcp=%u drhp=0x%08x status=0x%08x\n",
	       reason, eng->nd->device_index, eng->udma.name, ring->qid,
	       tx_dcp & UDMA_M2S_Q_TDRHP_OFFSET_MASK, tx_drhp_raw, tx_status_raw,
	       rx_dcp & UDMA_M2S_Q_TDRHP_OFFSET_MASK, rx_drhp_raw, rx_status_raw);
}

static void ndma_diag_interrupts(struct ndma_eng *eng, const char *reason)
{
	struct udma_iofic_regs __iomem *int_regs = eng->udma.gen_int_regs;
	u32 grp[4];
	u32 sec[2];
	int ret, i;

	if (!int_regs)
		return;

	/* Read all 4 main IOFIC groups */
	for (i = 0; i < 4; i++) {
		ret = reg_read32(&int_regs->main_iofic.ctrl[i].int_cause_grp, &grp[i]);
		if (ret) {
			pr_err(DIAG_PFX "[%s] nd%02d %s: main_iofic group %d read failed (%d)\n",
			       reason, eng->nd->device_index, eng->udma.name, i, ret);
			grp[i] = 0;
		}
	}

	pr_err(DIAG_PFX "[%s] nd%02d %s: main_iofic grp0=0x%08x grp1=0x%08x grp2=0x%08x grp3=0x%08x\n",
	       reason, eng->nd->device_index, eng->udma.name,
	       grp[0], grp[1], grp[2], grp[3]);

	/* Read secondary IOFIC (2 groups) */
	for (i = 0; i < 2; i++) {
		ret = reg_read32(&int_regs->secondary_iofic_ctrl[i].int_cause_grp, &sec[i]);
		if (ret) {
			pr_err(DIAG_PFX "[%s] nd%02d %s: secondary_iofic group %d read failed (%d)\n",
			       reason, eng->nd->device_index, eng->udma.name, i, ret);
			sec[i] = 0;
		}
	}

	if (!sec[0] && !sec[1])
		return;

	pr_err(DIAG_PFX "[%s] nd%02d %s: secondary_iofic grpA=0x%08x grpB=0x%08x\n",
	       reason, eng->nd->device_index, eng->udma.name, sec[0], sec[1]);

	/*
	 * Decode the named error causes. These live in the two secondary IOFIC
	 * groups, not in the main IOFIC -- the main IOFIC only carries a
	 * per-group summary bit for each of them.
	 */
	for (i = 0; i < ARRAY_SIZE(known_error_bits); i++) {
		const struct ndma_diag_error_bit *eb = &known_error_bits[i];

		/* Guard against a future table entry naming an unknown group */
		if (eb->grp >= ARRAY_SIZE(sec))
			continue;

		if (!(sec[eb->grp] & BIT(eb->bit)))
			continue;

		pr_err(DIAG_PFX "[%s] nd%02d %s:   grp%c bit %u: %s\n",
		       reason, eng->nd->device_index, eng->udma.name,
		       eb->grp == NDMA_DIAG_SEC_GRP_A ? 'A' : 'B',
		       eb->bit, eb->name);
	}
}

static void ndma_diag_prefetch_and_fifo(struct ndma_eng *eng, const char *reason)
{
	u32 pref_cfg1 = 0, pref_cfg2 = 0, pref_cfg3 = 0;
	u32 data_cfg = 0, stream_cfg = 0, axi_ostand = 0;

	if (!eng->udma.udma_regs_m2s)
		return;

	reg_read32(&eng->udma.udma_regs_m2s->m2s_rd.desc_pref_cfg_1, &pref_cfg1);
	reg_read32(&eng->udma.udma_regs_m2s->m2s_rd.desc_pref_cfg_2, &pref_cfg2);
	reg_read32(&eng->udma.udma_regs_m2s->m2s_rd.desc_pref_cfg_3, &pref_cfg3);
	reg_read32(&eng->udma.udma_regs_m2s->m2s_rd.data_cfg, &data_cfg);
	reg_read32(&eng->udma.udma_regs_m2s->axi_m2s.ostand_cfg, &axi_ostand);
	reg_read32(&eng->udma.udma_regs_m2s->m2s.stream_cfg, &stream_cfg);

	pr_err(DIAG_PFX "[%s] nd%02d %s: pref_cfg=0x%x/0x%x/0x%x data_cfg=0x%x axi_ostand=0x%x stream_cfg=0x%x\n",
	       reason, eng->nd->device_index, eng->udma.name,
	       pref_cfg1, pref_cfg2, pref_cfg3, data_cfg, axi_ostand, stream_cfg);
}

/**
 * ndma_diag_dump_engine() - Dump DMA engine diagnostic state on error.
 *
 * Public entry point called from any DMA error/timeout path (normally via
 * the dynamic-debug-gated NDMA_DIAG_DUMP_ENGINE() wrapper). Emits
 * diagnostic state via pr_err. Must not sleep, allocate, or take locks.
 *
 * @eng:        Pointer to the failing DMA engine (provides nd, eng_id, udma)
 * @ring:       Pointer to the failing ring (provides qid)
 * @desc_count: Descriptor count that timed out
 * @reason:     Caller-provided tag for the dump, naming the code path
 *              (e.g. "dma_completion_timeout")
 */
void ndma_diag_dump_engine(struct ndma_eng *eng, struct ndma_ring *ring,
			   u32 desc_count, const char *reason)
{
	if (!eng || !ring || !eng->nd) {
		pr_err(DIAG_PFX "[%s] invalid eng/ring/nd pointer, aborting dump\n",
		       reason ? reason : "unknown");
		return;
	}

	ndma_diag_eng_state(eng, reason);
	ndma_diag_failing_queue(eng, ring, desc_count, reason);
	ndma_diag_interrupts(eng, reason);
	ndma_diag_prefetch_and_fifo(eng, reason);
}

