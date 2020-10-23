// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/types.h>

#include "udma/udma.h"
#include "v1/address_map.h"

#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"

static struct ndma_eng *ndmar_acquire_engine(struct neuron_device *nd, u32 eng_id)
{
	if (eng_id >= NUM_DMA_ENG_PER_DEVICE)
		return NULL;
	mutex_lock(&nd->ndma_engine[eng_id].lock);
	return &nd->ndma_engine[eng_id];
}

static void ndmar_release_engine(struct ndma_eng *eng)
{
	mutex_unlock(&eng->nd->ndma_engine[eng->eng_id].lock);
}

static struct ndma_queue *ndmar_get_queue(struct ndma_eng *eng, u32 qid)
{
	return &eng->queues[qid];
}

static struct ndma_ring *ndmar_get_ring(struct ndma_queue *queue)
{
	return &queue->ring_info;
}

u32 ndmar_ring_get_desc_count(u32 v)
{
	if (v < 32) {
		return 64;
	}
	v += UDMA_MAX_NUM_CDESC_PER_CACHE_LINE;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

/**
 * ndmar_ring_set_mem_chunk() - Set memory chunk backing the queue's physical memory(descriptor ring buffer)
 * @eng: dma engine
 * @qid: dma queue id in the engine for which the mc is being set.
 * @mc: backing memory chunk
 * @port: which axi port(0 or 1) to access the DRAM(for performance)
 * @queue_type: type of the queue(rx, tx, or completion)
 */
static void ndmar_ring_set_mem_chunk(struct ndma_eng *eng, u32 qid, struct mem_chunk *mc, u32 port,
				     enum neuron_dma_queue_type queue_type)
{
	struct ndma_queue *queue = ndmar_get_queue(eng, qid);
	struct ndma_ring *ring = ndmar_get_ring(queue);

	switch (queue_type) {
	case NEURON_DMA_QUEUE_TYPE_TX:
		ring->tx_mc = mc;
		ring->tx.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->tx.addr = virt_to_phys(ring->tx.ptr) | PCIEX8_0_BASE;
		} else {
			ring->tx.addr = mc->pa;
			if (port) {
				ring->tx.addr |= P_1_BASE;
			}
		}
		break;
	case NEURON_DMA_QUEUE_TYPE_RX:
		ring->rx_mc = mc;
		ring->rx.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->rx.addr = virt_to_phys(ring->rx.ptr) | PCIEX8_0_BASE;
		} else {
			ring->rx.addr = mc->pa;
			if (port) {
				ring->rx.addr |= P_1_BASE;
			}
		}
		break;
	case NEURON_DMA_QUEUE_TYPE_COMPLETION:
		ring->has_compl = true;
		ring->rxc_mc = mc;
		ring->rxc.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->rxc.addr = virt_to_phys(ring->rx.ptr) | PCIEX8_0_BASE;
		} else {
			ring->rxc.addr = mc->pa;
			if (port) {
				ring->rxc.addr |= P_1_BASE;
			}
		}
		break;
	default:
		break;
	}
}

int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, u32 port)
{
	int ret = -1;
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	queue = ndmar_get_queue(eng, qid);
	ring = ndmar_get_ring(queue);

	queue->eng_id = eng_id;
	queue->qid = qid;
	ring->qid = qid;

	trace_dma_queue_init(nd, eng_id, qid, tx_desc_count, rx_desc_count, tx_mc, rx_mc, rxc_mc,
			     port);

	ndmar_ring_set_mem_chunk(eng, qid, tx_mc, port, NEURON_DMA_QUEUE_TYPE_TX);
	ndmar_ring_set_mem_chunk(eng, qid, rx_mc, port, NEURON_DMA_QUEUE_TYPE_RX);
	if (rxc_mc)
		ndmar_ring_set_mem_chunk(eng, qid, rxc_mc, port, NEURON_DMA_QUEUE_TYPE_COMPLETION);

	ret = udma_m2m_init_queue(&eng->udma, qid, tx_desc_count, rx_desc_count, false, &ring->tx,
				  &ring->rx, rxc_mc != NULL ? &ring->rxc : NULL);

	ndmar_release_engine(eng);
	return ret;
}

int ndmar_ack_completed(struct neuron_device *nd, u32 eng_id, u32 qid, u32 count)
{
	struct ndma_eng *eng;
	struct udma_q *rxq, *txq;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_ack_completed(nd, eng_id, qid, count);

	udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_queue_copy_start(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
			   u32 rx_desc_count)
{
	int ret = -1;
	struct ndma_eng *eng;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_queue_copy_start(nd, eng_id, qid, tx_desc_count, rx_desc_count);

	ret = udma_m2m_copy_start(&eng->udma, qid, tx_desc_count, rx_desc_count);
	if (ret)
		pr_err("M2M copy start failed - %d\n", ret);

	ndmar_release_engine(eng);

	return ret;
}

int ndmar_queue_release(struct neuron_device *nd, u32 eng_id, u32 qid)
{
	trace_dma_queue_release(nd, eng_id, qid);
	// inf1 does not need any special handling
	return 0;
}

int ndmar_h2t_ring_alloc(struct neuron_device *nd, int nc_id)
{
	int ret = 0;
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	int eng_id = DMA_ENG_IDX_H2T + (nc_id * V1_DMA_ENG_PER_NC);
	int ndesc = DMA_H2T_DESC_COUNT;
	u32 ring_size = ndmar_ring_get_desc_count(ndesc) * sizeof(union udma_desc);
	int qid = MAX_DMA_RINGS - 1;
	struct mem_chunk *rx_mc = NULL, *tx_mc = NULL;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	eng->used_for_h2t = true;
	queue = &eng->queues[MAX_DMA_RINGS - 1];
	queue->qid = qid;
	queue->eng_id = eng_id;
	ring = &queue->ring_info;
	ring->qid = qid;
	ring->size = ring_size;
	ring->has_compl = false;

	ret = mc_alloc(&nd->mpset, &rx_mc, ring_size, MEM_LOC_HOST, 0, 0, nc_id);
	if (ret) {
		pr_err("can't allocate rx queue for H2T - size %d\n", ring_size);
		goto error;
	}

	ret = mc_alloc(&nd->mpset, &tx_mc, ring_size, MEM_LOC_HOST, 0, 0, nc_id);
	if (ret) {
		pr_err("can't allocate tx queue for H2T - size %d\n", ring_size);
		goto error;
	}

	ndmar_ring_set_mem_chunk(eng, qid, tx_mc, 0, NEURON_DMA_QUEUE_TYPE_TX);
	ndmar_ring_set_mem_chunk(eng, qid, rx_mc, 0, NEURON_DMA_QUEUE_TYPE_RX);

	mutex_init(&eng->h2t_ring_lock);

	ndmar_release_engine(eng);

	return 0;

error:
	ndmar_release_engine(eng);

	if (rx_mc)
		mc_free(&rx_mc);
	if (tx_mc)
		mc_free(&tx_mc);

	return ret;
}

static int ndmar_h2t_ring_init(struct ndma_eng *eng)
{
	int ret = -1;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	int qid = MAX_DMA_RINGS - 1;
	int ndesc = DMA_H2T_DESC_COUNT;
	u32 alloced_desc = ndmar_ring_get_desc_count(ndesc);

	queue = &eng->queues[MAX_DMA_RINGS - 1];
	ring = &queue->ring_info;
	ret = udma_m2m_init_queue(&eng->udma, qid, alloced_desc, alloced_desc, true, &ring->tx,
				  &ring->rx, NULL);
	return ret;
}

int ndmar_eng_set_state(struct neuron_device *nd, int eng_id, u32 state)
{
	struct ndma_eng *eng;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	udma_state_set(&eng->udma, state);

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_eng_get_state(struct neuron_device *nd, int eng_id, struct neuron_dma_eng_state *state)
{
	struct ndma_eng *eng;
	struct udma *udma;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	udma = &eng->udma;

	state->tx_state = udma_state_get(udma, UDMA_TX);
	state->rx_state = udma_state_get(udma, UDMA_RX);
	state->max_queues = udma->num_of_queues_max;
	state->num_queues = udma->num_of_queues;
	state->revision_id = udma->rev_id;

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_queue_get_descriptor_mc(struct neuron_device *nd, u8 eng_id, u8 qid,
				  struct mem_chunk **tx, struct mem_chunk **rx, u32 *tx_size,
				  u32 *rx_size)
{
	struct ndma_eng *eng;
	struct ndma_queue *q;
	struct udma *udma;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	q = &eng->queues[qid];
	udma = &eng->udma;

	*tx_size = udma->udma_q_m2s[qid].size;
	*rx_size = udma->udma_q_s2m[qid].size;
	*rx = q->ring_info.rx_mc;
	*tx = q->ring_info.tx_mc;

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_eng_init(struct neuron_device *nd, int eng_id)
{
	struct ndma_eng *eng;
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	void __iomem *udma_base[NUM_DMA_ENG_PER_DEVICE];
	void __iomem *tdma_base[NUM_DMA_ENG_PER_DEVICE];
	const u64 teng_udma_base[] = { P_0_APB_TENG_0_UDMA_0_RELBASE, P_0_APB_TENG_1_UDMA_0_RELBASE,
				       P_0_APB_TENG_2_UDMA_0_RELBASE,
				       P_0_APB_TENG_3_UDMA_0_RELBASE };
	const u64 teng_tdma_base[] = { P_0_APB_TENG_0_TDMA_0_RELBASE, P_0_APB_TENG_1_TDMA_0_RELBASE,
				       P_0_APB_TENG_2_TDMA_0_RELBASE,
				       P_0_APB_TENG_3_TDMA_0_RELBASE };

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_engine_init(nd, eng_id);

	udma_base[eng_id] = (void __iomem *)(nd->npdev.bar0) +
			    teng_udma_base[eng_id / V1_DMA_ENG_PER_NC] +
			    ((eng_id % V1_DMA_ENG_PER_NC) * P_0_APB_TENG_0_UDMA_0_SIZE);
	tdma_base[eng_id] = (void __iomem *)(nd->npdev.bar0) +
			    teng_tdma_base[eng_id / V1_DMA_ENG_PER_NC] +
			    ((eng_id % V1_DMA_ENG_PER_NC) * P_0_APB_TENG_0_TDMA_0_SIZE);

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(&eng->udma, udma_base[eng_id], DMA_MAX_Q_MAX, udma_name, 0);
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = tdma_init_engine(tdma_base[eng_id]);
	if (ret) {
		pr_err("TDMA ENG:%d init failed\n", eng_id);
		goto done;
	}

	if (eng->used_for_h2t) {
		// Reinitialize the h2t queue
		ret = ndmar_h2t_ring_init(eng);
		if (ret)
			pr_err("could not reinitialize the h2t queue\n");
	}
done:
	ndmar_release_engine(eng);
	return ret;
}

void ndmar_preinit(struct neuron_device *nd)
{
	int i;
	struct ndma_eng *eng;

	for (i = 0; i < NUM_DMA_ENG_PER_DEVICE; i++) {
		eng = &nd->ndma_engine[i];
		eng->nd = nd;
		eng->eng_id = i;
		mutex_init(&eng->lock);
	}
}

int ndmar_init(struct neuron_device *nd)
{
	int ret = 0;
	int nc_id = 0;

	for (nc_id = 0; nc_id < V1_NC_PER_DEVICE; nc_id++) {
		struct ndma_eng *eng;
		int eng_id = DMA_ENG_IDX_H2T + (nc_id * V1_DMA_ENG_PER_NC);

		ret = ndmar_h2t_ring_alloc(nd, nc_id);
		if (ret) {
			pr_err("H2T ring allocation failed - %d\n", ret);
			return ret;
		}

		ret = ndmar_eng_init(nd, eng_id);
		if (ret) {
			pr_err("H2T engine init failed - %d\n", ret);
			return ret;
		}

		eng = ndmar_acquire_engine(nd, eng_id);
		if (eng == NULL)
			return -EINVAL;
		ret = ndmar_h2t_ring_init(eng);
		ndmar_release_engine(eng);
		if (ret) {
			pr_err("H2T ring init failed - %d\n", ret);
			return ret;
		}
	}

	return ret;
}

static void ndmar_h2t_ring_free(struct neuron_device *nd, int eng_id)
{
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;

	eng = ndmar_acquire_engine(nd, eng_id);
	BUG_ON(eng == NULL);

	queue = &eng->queues[MAX_DMA_RINGS - 1];
	ring = &queue->ring_info;

	if (ring->tx_mc)
		mc_free(&ring->tx_mc);

	if (ring->rx_mc)
		mc_free(&ring->rx_mc);

	if (ring->rxc_mc)
		mc_free(&ring->rxc_mc);

	ndmar_release_engine(eng);
}

void ndmar_close(struct neuron_device *nd)
{
	int nc_id;
	for (nc_id = 0; nc_id < V1_NC_PER_DEVICE; nc_id++) {
		ndmar_h2t_ring_free(nd, DMA_ENG_IDX_H2T + (nc_id * V1_DMA_ENG_PER_NC));
	}
	return;
}
