// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "udma/udma.h"
#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"
#include "neuron_dhal.h"
#include "neuron_ring.h"

int nc_per_dev_param = 1;
module_param(nc_per_dev_param, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(nc_per_dev_param, "Number of neuron cores");

int dev_nc_map = 1;
module_param(dev_nc_map, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dev_nc_map, "Map of active neuron cores");

int dma_teardown_on_exit = 1;
module_param(dma_teardown_on_exit, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dma_teardown_on_exit, "Reset the DMA state on user process exit");

// forward
static void ndmar_h2t_ring_free(struct ndma_eng *eng, struct ndma_ring *ring);

struct ndma_eng *ndmar_acquire_engine(struct neuron_device *nd, u32 eng_id)
{
	if (eng_id >= NUM_DMA_ENG_PER_DEVICE)
		return NULL;
	mutex_lock(&nd->ndma_engine[eng_id].lock);
	return &nd->ndma_engine[eng_id];
}

// acquire dma engine w/o locking.  Use for scanning.
static struct ndma_eng *ndmar_acquire_engine_nl(struct neuron_device *nd, u32 eng_id)
{
	if (eng_id >= NUM_DMA_ENG_PER_DEVICE)
		return NULL;
	return &nd->ndma_engine[eng_id];
}

void ndmar_release_engine(struct ndma_eng *eng)
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
 * @queue_type: type of the queue(rx, tx, or completion)
 */
static void ndmar_ring_set_mem_chunk(struct ndma_eng *eng, u32 qid, struct mem_chunk *mc, enum neuron_dma_queue_type queue_type)
{
	struct ndma_queue *queue = ndmar_get_queue(eng, qid);
	struct ndma_ring *ring = ndmar_get_ring(queue);

	switch (queue_type) {
	case NEURON_DMA_QUEUE_TYPE_TX:
		ring->tx_mc = mc;
		ring->tx.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->tx.addr = virt_to_phys(ring->tx.ptr) | ndhal->ndhal_address_map.pci_host_base;
		} else {
			ring->tx.addr = mc->pa;
		}
		break;
	case NEURON_DMA_QUEUE_TYPE_RX:
		ring->rx_mc = mc;
		ring->rx.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->rx.addr = virt_to_phys(ring->rx.ptr) | ndhal->ndhal_address_map.pci_host_base;
		} else {
			ring->rx.addr = mc->pa;
		}
		break;
	case NEURON_DMA_QUEUE_TYPE_COMPLETION:
		ring->has_compl = true;
		ring->rxc_mc = mc;
		ring->rxc.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->rxc.addr = virt_to_phys(ring->rxc.ptr) | ndhal->ndhal_address_map.pci_host_base;
		} else {
			ring->rxc.addr = mc->pa;
		}
		break;
	default:
		break;
	}
}

int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, bool allocatable)
{
	int ret = -1;
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	if (qid >= ndhal->ndhal_udma.num_queues) {
		ret = -EINVAL;
		goto done;
	}
	
	queue = ndmar_get_queue(eng, qid);
	ring = ndmar_get_ring(queue);

	// check if the ring has been allocated to h2t and fail
	if (ndmar_h2t_ring_is_h2t(ring) && (tx_mc != nd->ndma_q_dummy_mc)) { 
		pr_err("nd%02d: DMA ring allocation collision with h2t dma on eng: %d, queue: %d", nd->device_index, eng_id, qid);
		ret = -EALREADY;
		goto done;
	}

	queue->eng_id = eng_id;
	queue->qid = qid;
	queue->owner = task_tgid_nr(current);
	ring->qid = qid;
	ring->h2t_completion_mc = NULL;

	trace_dma_queue_init(nd, eng_id, qid, tx_desc_count, rx_desc_count, tx_mc, rx_mc, rxc_mc);

	if (tx_mc) {
		/*
		if ((u64)tx_desc_count * sizeof(union udma_desc) > tx_mc->size) {
			ret = -EINVAL;
			goto done;
		}*/
		ndmar_ring_set_mem_chunk(eng, qid, tx_mc, NEURON_DMA_QUEUE_TYPE_TX);
	}

	if (rx_mc) {
		/*
		if ((u64)rx_desc_count * sizeof(union udma_desc) > rx_mc->size) {
			ret = -EINVAL;
			goto done;
		}*/
		ndmar_ring_set_mem_chunk(eng, qid, rx_mc, NEURON_DMA_QUEUE_TYPE_RX);
	}

	if (rxc_mc) {
		if ((u64)rx_desc_count * sizeof(union udma_desc) > rxc_mc->size) {
			ret = -EINVAL;
			goto done;
		}
		ndmar_ring_set_mem_chunk(eng, qid, rxc_mc, NEURON_DMA_QUEUE_TYPE_COMPLETION);
	}

	ret = udma_m2m_init_queue(&eng->udma, qid, eng_id, tx_desc_count, rx_desc_count, allocatable, tx_mc != NULL ? &ring->tx : NULL,
				  rx_mc != NULL ? &ring->rx : NULL, rxc_mc != NULL ? &ring->rxc : NULL);

done:
	ndmar_release_engine(eng);
	return ret;
}

void ndmar_handle_process_exit(struct neuron_device *nd, pid_t pid)
{
	int ret, eng_id, qid;

	// Skip resetting the DMAs on user process exit if module param is disabled
	if (dma_teardown_on_exit == 0) {
		return;
	}

	struct mem_chunk *mc = nd->ndma_q_dummy_mc;
	const int desc_count = NDMA_QUEUE_DUMMY_RING_DESC_COUNT;
	for (eng_id = 0; eng_id < ndhal->ndhal_address_map.seng_dma_eng_per_nd; eng_id++) {
		for (qid = 0; qid < ndhal->ndhal_udma.num_queues; qid++) {
			struct ndma_eng *eng = ndmar_acquire_engine_nl(nd, eng_id);
			struct ndma_queue *queue;
			struct ndma_ring *ring;

			if (eng == NULL) {
				// 
				continue;
			}
			queue = ndmar_get_queue(eng, qid);
			ring = ndmar_get_ring(queue);

			if (queue->owner != pid) {
				continue;
			}

			// default h2t rings are maintained by the driver so dont reset.
			// there cant be any outstanding DMA transaction in h2t since it is a
			// synchronous system call(which will block till finished when a process crashes).
			// TODO: async h2t will need to get cleaned up here.
			
			if (ndhal->ndhal_ndmar.ndmar_is_h2t_def_q(nd, eng_id, qid)) {
				pr_err_once("nd%02d: unexpected pid associated with default h2t ring", nd->device_index);
				continue;
			}
		
			// h2t rings owned by driver are freed up on different path
			if (ndmar_h2t_ring_is_h2t(ring)) { 
				pr_err_once("h2t ring should not be bound to process");
				continue;
			}

			ndmar_h2t_ring_state_clr(ring);
			queue->owner = 0;
                                                         
			// rings owned by the nx should not be reset by us
			// ok since they should never be interacting with host mem
			if (ndhal->ndhal_ndmar.ndmar_is_nx_ring(eng_id, qid)) {
				continue;
			}

			ret = ndmar_queue_init(nd, eng_id, qid, desc_count, desc_count, mc, mc, NULL, false);
			// ignore the error and continue to reset other queues.
			if (ret)
				pr_err("nd%d:dma%d:q%d failed to reset (%d)", nd->device_index, eng_id, qid, ret);
		}
	}
}

int ndmar_ack_completed(struct neuron_device *nd, u32 eng_id, u32 qid, u32 count)
{
	int ret;
	struct ndma_eng *eng;
	struct udma_q *rxq, *txq;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_ack_completed(nd, eng_id, qid, count);

	ret = udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);
	if (ret || !txq) {
		pr_err("invalid tx qid %d for eng %d\n", qid, eng_id);
		goto done;
	}
	ret = udma_q_handle_get(&eng->udma, qid, UDMA_RX, &rxq);
	if (ret || !rxq) {
		pr_err("invalid rx qid %d for eng %d\n", qid, eng_id);
		goto done;
	}

	ret = udma_cdesc_ack(rxq, count);
	if (ret) {
		goto done;
	}
	ret = udma_cdesc_ack(txq, count);
	if (ret) {
		goto done;
	}

done:
	ndmar_release_engine(eng);
	return ret;
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

	// For V1 we need to see if first model was start. The copy start is used to copy
	// pe iram instructions and if that is done, then the state is set to indicate that
	// model has been started
    struct mem_chunk *rx_mc = eng->queues[qid].ring_info.rx_mc;
	if (!ret && rx_mc->model_start_tracker.has_pe_iram_inst) {
		nd->nc_model_started_count[rx_mc->model_start_tracker.nc_id]++;
	}

	ndmar_release_engine(eng);

	return ret;
}

int ndmar_queue_release(struct neuron_device *nd, u32 eng_id, u32 qid)
{
	trace_dma_queue_release(nd, eng_id, qid);
	return 0;
}

static int ndmar_h2t_ring_alloc(struct neuron_device *nd, int nc_id, int qid)
{
	int ret = 0;
	struct mem_chunk *rx_mc = NULL, *tx_mc = NULL, *h2t_completion_mc = NULL;
	struct ndma_queue *queue;
	struct ndma_ring *ring;

	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	const int ndesc = DMA_H2T_DESC_COUNT;
	const u32 ring_size = ndmar_ring_get_desc_count(ndesc) * sizeof(union udma_desc);

	struct ndma_eng *eng  = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	queue = ndmar_get_queue(eng, qid);
	ring = ndmar_get_ring(queue);

	eng->used_for_h2t = true;
	queue->eng_id = eng_id;
	queue->qid = qid;
	queue->owner = 0;
	ring->qid = qid;
	ring->h2t_nc_id = nc_id;
	ring->size = ring_size;
	ring->has_compl = false;

	ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, ring_size, 0, MEM_LOC_HOST, 0, nc_id, NEURON_MEMALLOC_TYPE_NCDEV_HOST, &rx_mc);
	if (ret) {
		pr_err("can't allocate rx queue for H2T - size %d\n", ring_size);
		goto error;
	}

	ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, ring_size, 0, MEM_LOC_HOST, 0, nc_id, NEURON_MEMALLOC_TYPE_NCDEV_HOST, &tx_mc);
	if (ret) {
		pr_err("can't allocate tx queue for H2T - size %d\n", ring_size);
		goto error;
	}

	ndmar_ring_set_mem_chunk(eng, qid, tx_mc, NEURON_DMA_QUEUE_TYPE_TX);
	ndmar_ring_set_mem_chunk(eng, qid, rx_mc, NEURON_DMA_QUEUE_TYPE_RX);

	ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, sizeof(u32) * 2 * NEURON_DMA_H2T_CTX_HANDLE_CNT, 0, MEM_LOC_HOST, 0, nc_id, NEURON_MEMALLOC_TYPE_NCDEV_HOST, &h2t_completion_mc);
	if (ret) {
		pr_err("can't allocate h2t_completion_mc memory for H2T\n");
		goto error;
	}

	ring->h2t_completion_mc = h2t_completion_mc;
	ring->h2t_completion.ptr = h2t_completion_mc->va;
	ring->h2t_completion.addr = virt_to_phys(ring->h2t_completion.ptr) | ndhal->ndhal_address_map.pci_host_base;

	ret = ndma_h2d_compl_queue_init(nd, &ring->dma_compl_queue);
	if (ret) {
		pr_err("can't initialize h2d dma completion queue\n");
		goto error;
	}

	ret = ndma_ctx_queue_init(&ring->dma_ctx_queue);
	if (ret) {
		pr_err("can't initialize dma context queue\n");
		goto error_ctx_queue;
	}

	mutex_init(&ring->h2t_ring_lock);

	ndmar_release_engine(eng);

	return 0;

error_ctx_queue:
	ndma_h2d_compl_queue_destroy(&ring->dma_compl_queue);
error:
	ring->h2t_nc_id = -1;
	ring->tx_mc = NULL;
	ring->rx_mc = NULL;
	ring->h2t_completion_mc = NULL;

	ndmar_release_engine(eng);

	if (rx_mc)
		mc_free(&rx_mc);
	if (tx_mc)
		mc_free(&tx_mc);
	if (h2t_completion_mc)
		mc_free(&h2t_completion_mc);
	
	return ret;
}

int ndmar_h2t_ring_init(struct ndma_eng *eng, int qid)
{
	int ret = -1;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	int ndesc = DMA_H2T_DESC_COUNT;
	u32 alloced_desc = ndmar_ring_get_desc_count(ndesc);

	queue = ndmar_get_queue(eng, qid);
	ring = ndmar_get_ring(queue);
	ret = udma_m2m_init_queue(&eng->udma, qid, eng->eng_id, alloced_desc, alloced_desc, true, &ring->tx,
				  &ring->rx, NULL);
	return ret;
}

static bool ndmar_h2t_ring_claim(struct neuron_device *nd, u32 eng_id, struct ndma_ring *ring)
{
	bool claimed = false;
	struct ndma_eng *eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return false;
	if (!ring->h2t_allocated) {
		ring->h2t_nc_id = -1;
		ring->h2t_allocated = true;
		claimed = true;
	}
	ndmar_release_engine(eng);
	return claimed;
}

/* ndmar_h2t_ring_request()
 *
 *    Ask the driver to dynamically allocate an h2t ring.
 *
 *   caveats:
 *    - Other runtime allocated rings are not tracked by the driver, so h2t rings have to be requested
 *      prior to any rings allocated for a model.  So basically we only track h2t ring allocations in the
 *      driver.
 */
int ndmar_h2t_ring_request(struct neuron_device *nd, int nc_id, bool h2t, int *rqid)
{
	int ret = -EBUSY; // default if no free queue found
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	int qid;
	
	eng = ndmar_acquire_engine_nl(nd, eng_id);
	if (eng == NULL)
		return -ENOENT;

	for (qid = 0; qid < ndhal->ndhal_udma.num_queues; qid++) {
		if (ndhal->ndhal_ndmar.ndmar_is_h2t_def_q(nd, eng_id, qid))
			continue;
		queue = ndmar_get_queue(eng, qid);
		ring = ndmar_get_ring(queue);

		// ring is unallocated, we can use it
		if (ndmar_h2t_ring_claim(nd, eng_id, ring)) {
			// For historical reasons, there are two ways we keep track of and manage queue ownership.  DMAs that
			// are managed by the driver on behalf of a process are tagged by nc_id that is used by the process,
			// DMAs managed elsewhere (such as DMAs used NX refill) are tagged with PID.  The tagging determines
			// where the DMA resources are cleaned up.
			if (h2t) {
				// queue is used for driver memcopy
				ret = ndmar_h2t_ring_alloc(nd, nc_id, qid);
				if (ret) {
					pr_err("nd%d:nc%d H2T ring allocation for qid:%d failed - %d\n", nd->device_index, nc_id, qid, ret);
					ring->h2t_allocated = false;
					goto done;
				}
				ret = ndmar_h2t_ring_init(eng, qid);
				if (ret) {
					ndmar_h2t_ring_free(eng, ring);
					pr_err("nd%d:nc%d H2T ring init for qid:%d failed - %d\n", nd->device_index, nc_id, qid, ret);
					ring->h2t_allocated = false;
					goto done;
				}
			} else {
				// queue is use by HW or the RT directly
				queue->owner = task_tgid_nr(current);
				ring->h2t_nc_id = nc_id;
				ret = 0;
			}
			*rqid = qid;
			break;
		}
	}

done:
	return ret;
}

int ndmar_h2t_ring_release(struct neuron_device *nd, int nc_id, int qid)
{
	int ret = 0;
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	
	if (qid >= ndhal->ndhal_udma.num_queues) {
		return -EINVAL;
	}

	if (ndhal->ndhal_ndmar.ndmar_is_h2t_def_q(nd, eng_id, qid)) {
		return 0;
	}

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL) {
		return -EINVAL;
	}

	queue = ndmar_get_queue(eng, qid);
	ring = ndmar_get_ring(queue);

	if (!ndmar_h2t_ring_is_allocated(ring) || (ring->h2t_nc_id != nc_id)) {
		pr_err("nd%02d: attempting to release ring %d on nc: %d that is not allocated as h2t ring", nd->device_index, qid, nc_id);
		ret = -ENXIO;
		goto done;
	}

	if (ndmar_h2t_ring_is_h2t(ring)) {
		ndmar_h2t_ring_free(eng, ring);
	} else {
		ndmar_h2t_ring_state_clr(ring);
		queue->owner = 0;
	}

done:
	ndmar_release_engine(eng);
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

/**
 * ndmar_queue_get_descriptor_info()
 *
 *    return the device side physical addresses and lengths
 *    from registers for the tx/rx descriptor rings for a given ring.
 *    also return the MCs for vetting
 */
int ndmar_queue_get_descriptor_info(struct neuron_device *nd, u8 eng_id, u8 qid,
				  struct mem_chunk **tx_mc, struct mem_chunk **rx_mc, u64 *tx_pa, u64 *rx_pa, 
				  u32 *tx_size, u32 *rx_size)
{
	int ret = 0;
	struct ndma_eng *eng;
	struct udma *udma;
	struct udma_q *hw_q;
	u32 low, high;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	udma = &eng->udma;

	if (udma_q_handle_get( udma, qid, UDMA_TX, &hw_q) != 0) {
			ret = -EINVAL;
			goto done;
	}
	if (reg_read32(&hw_q->q_regs->rings.drbp_high, &high)) {
		return -EIO;
		goto done;
	}
	if (reg_read32(&hw_q->q_regs->rings.drbp_low, &low)) {
		return -EIO;
		goto done;
	}
	if (reg_read32(&hw_q->q_regs->rings.drl, tx_size)) {
		return -EIO;
		goto done;
	}
	*tx_pa = ((u64)high<<32) | low;

	if (udma_q_handle_get( udma, qid, UDMA_RX, &hw_q) != 0) {
		ret = -EINVAL;
		goto done;
	}
	if (reg_read32(&hw_q->q_regs->rings.drbp_high, &high)) {
		return -EIO;
		goto done;
	}
	if (reg_read32(&hw_q->q_regs->rings.drbp_low, &low)) {
		return -EIO;
		goto done;
	}
	if (reg_read32(&hw_q->q_regs->rings.drl, rx_size)) {
		return -EIO;
		goto done;
	}
	*rx_pa = ((u64)high<<32) | low;

	// get MCs so caller can validate the physical address lives inside memchunks
	//
	*tx_mc = nmmap_get_mc_from_pa(nd, *tx_pa);
	*rx_mc = nmmap_get_mc_from_pa(nd, *rx_pa);

done:
	ndmar_release_engine(eng);
	return ret;
}

int ndmar_eng_init(struct neuron_device *nd, int eng_id)
{
	int ret = 0;
	struct ndma_eng *eng  = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_engine_init(nd, eng_id);

	ret = ndhal->ndhal_ndma.ndma_init(nd->npdev.bar0, &eng->udma, eng_id);
	if (ret)
		goto done;

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
		memset((void *)eng, 0, sizeof(struct ndma_eng));
		eng->nd = nd;
		eng->eng_id = i;
		mutex_init(&eng->lock);
	}
}

static int ndmar_init_nc(struct neuron_device *nd, int nc_idx, bool init_h2t_eng)
{
	int ret = 0;

	if (nd->dmar_init_done[nc_idx]) {
		return 0;
	}

	// init all seng DMA engines in the NC
	int eng_id;
	int start_eng = nc_idx * ndhal->ndhal_address_map.dma_eng_per_nc;
	int end_eng = (nc_idx + 1) * ndhal->ndhal_address_map.dma_eng_per_nc - 1;
	for (eng_id = start_eng; eng_id <= end_eng; eng_id++) {
		ret = ndmar_eng_init(nd, eng_id);
		if (ret) {
			pr_err("nd%d: DMA eng%d init failed - %d\n", nd->device_index, eng_id, ret);
			return ret;
		}
	}

	// allocate H2T engines and rings
	eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_idx);
	if ((eng_id < start_eng || eng_id > end_eng) && init_h2t_eng) {
		ret = ndmar_eng_init(nd, eng_id);
		if (ret) {
			pr_err("nd%d: DMA eng%d init failed - %d\n", nd->device_index, eng_id, ret);
			return ret;
		}
	}

	const int qid = ndhal->ndhal_ndmar.ndmar_get_h2t_def_qid(nc_idx);

	ret = ndmar_h2t_ring_alloc(nd, nc_idx, qid);
	if (ret) {
		pr_err("nd%d:nc%d H2T ring allocation failed - %d\n", nd->device_index, nc_idx, ret);
		return ret;
	}

	struct ndma_eng *eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	ret = ndmar_h2t_ring_init(eng, qid);
	ndmar_release_engine(eng);
	if (ret) {
		pr_err("H2T ring init failed - %d\n", ret);
		return ret;
	}

	nd->dmar_init_done[nc_idx] = true;

	return ret;
}

int ndmar_init_ncs(struct neuron_device *nd, uint32_t nc_map) {
	int ret = 0;
	int nc_idx;

	for (nc_idx = 0; nc_idx < ndhal->ndhal_address_map.nc_per_device; nc_idx++) {
		if (((1 << nc_idx) & ndhal->ndhal_address_map.dev_nc_map) == 0) {
			continue;
		}
		if (nc_map == NEURON_NC_MAP_DEVICE || ((1 << nc_idx) & nc_map)) {
			bool init_h2t_eng = ndhal->ndhal_ndmar.nr_init_h2t_eng( nc_idx, nc_map);
			ret = ndmar_init_nc(nd, nc_idx, init_h2t_eng);
			if (ret) {
				pr_err("nd%d: DMA init failed on nc %d", nd->device_index, nc_idx);
				return ret;
			}
		}
	}
	return ret;
}

int ndmar_init(struct neuron_device *nd)
{
	return ndmar_init_ncs(nd, -1);
}

static void ndmar_h2t_ring_free(struct ndma_eng *eng, struct ndma_ring *ring)
{
	if (ring->tx_mc) {
		mc_free(&ring->tx_mc);
		ring->tx_mc = NULL;
	}

	if (ring->rx_mc) {
		mc_free(&ring->rx_mc);
		ring->rx_mc = NULL;
	}

	if (ring->rxc_mc) {
		mc_free(&ring->rxc_mc);
		ring->rxc_mc = NULL;
	}
	
	if (ring->h2t_completion_mc) {
		mc_free(&ring->h2t_completion_mc);
		ring->h2t_completion_mc = NULL;
	}

	ndma_ctx_queue_free(eng, ring, &ring->dma_ctx_queue);
	ndma_h2d_compl_queue_destroy(&ring->dma_compl_queue);

	ndmar_h2t_ring_state_clr(ring);
}

/* ndmar_h2t_ring_free_all()
 *
 */
static void ndmar_h2t_ring_free_all(struct neuron_device *nd, int nc_idx)
{
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_idx);
	int qid;
	
	eng  = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL) {
		pr_err("nd%02d: fatal error unable to acquire engine %d", nd->device_index, eng_id);
		return;
	}

	for (qid = 0; qid < ndhal->ndhal_udma.num_queues; qid++) {
		queue = ndmar_get_queue(eng, qid);
		ring = ndmar_get_ring(queue);

		// The default queues do not go through the ndmar_h2t_ring_request path
		// So they should be handled specially at cleanup too
		if (qid == ndhal->ndhal_ndmar.ndmar_get_h2t_def_qid(nc_idx)) {
			ndmar_h2t_ring_free(eng, ring);
			continue;
		}

		if (ndmar_h2t_ring_is_allocated(ring) && ring->h2t_nc_id == nc_idx) {
			if (ndmar_h2t_ring_is_h2t(ring)) {
				// h2t queue free all resources
				ndmar_h2t_ring_free(eng, ring);
			} else {
				// service queue only clear state
				ndmar_h2t_ring_state_clr(ring);
			}
		}
	}

	ndmar_release_engine(eng);
}

static void ndmar_close_nc(struct neuron_device *nd, int nc_idx)
{
	if (!nd->dmar_init_done[nc_idx]) {
		return;
	}
	ndmar_h2t_ring_free_all(nd, nc_idx);
	nd->dmar_init_done[nc_idx] = false;
}

void ndmar_close_ncs(struct neuron_device *nd, uint32_t nc_map)
{
	int nc_idx;
	for (nc_idx = 0; nc_idx < ndhal->ndhal_address_map.nc_per_device; nc_idx++) {
		if (((1 << nc_idx) & ndhal->ndhal_address_map.dev_nc_map) == 0) {
			continue;
		}
		if (nc_map == NEURON_NC_MAP_DEVICE || ((1 << nc_idx) & nc_map)) {
			ndmar_close_nc(nd, nc_idx);
		}
	}
}

void ndmar_close(struct neuron_device *nd)
{
	ndmar_close_ncs(nd, NEURON_NC_MAP_DEVICE);
}
