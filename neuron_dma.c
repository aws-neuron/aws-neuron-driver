// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "udma/udma.h"
#include "v1/address_map.h"

#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_dma_wait);
#endif

struct neuron_device;

void ndma_ack_completed_desc(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);
}

#define DMA_COMPLETION_MARKER_SIZE sizeof(u32)
#define DMA_COMPLETION_MARKER 0xabcdef01

/**
 * Wait for completion by start transfer of a DMA between two host memory locations and polling
 * on the host memory for the data to be written.
 */
int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_ring_ptr rxc;
	int ret = 0;
	volatile u32 *dst;
	volatile u32 *src;
	u64 i;

	// One descriptor takes ~4 usec to transfer (64K at 16G/sec) -  wait 100x longer
	u64 wait = 4 * count * 100;
	unsigned long one_loop_sleep = 1; // poll every 10 usecs
	u64 loop = wait / one_loop_sleep + 1;

	rxc.ptr = kmalloc(DMA_COMPLETION_MARKER_SIZE * 2, GFP_ATOMIC);
	if (!rxc.ptr) {
		pr_err("can't allocate memory for completion\n");
		return -1;
	}
	dst = (volatile u32 *)(rxc.ptr + DMA_COMPLETION_MARKER_SIZE);
	src = (volatile u32 *)rxc.ptr;

	// set the src value to the marker
	WRITE_ONCE(*src, DMA_COMPLETION_MARKER);
	WRITE_ONCE(*dst, 0);

	rxc.addr = virt_to_phys(rxc.ptr) | PCIEX8_0_BASE;
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, rxc.addr,
					rxc.addr + DMA_COMPLETION_MARKER_SIZE,
					DMA_COMPLETION_MARKER_SIZE, false, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		ret = -1;
		goto error;
	}

	count++; // for host to host(completion) descriptor.

	ret = udma_m2m_copy_start(&eng->udma, ring->qid, 1, 1);
	if (ret) {
		pr_err("failed to start DMA copy for %s q%d\n", eng->udma.name, ring->qid);
		goto error;
	}

#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_dma_wait, 1)) {
		ret = -ETIMEDOUT;
		goto error;
	}
#endif
	for (i = 0; i <= loop; i++) {
		u32 dst_val = READ_ONCE(*dst);
		// this descriptor is executed, meaning all other have completed
		if (dst_val == DMA_COMPLETION_MARKER) {
			// reset in case we are going to use this ring again
			WRITE_ONCE(*dst, 0);
			WRITE_ONCE(*src, DMA_COMPLETION_MARKER);
			// while we don't have completion ring, udma uses completion counter
			// for keeping track of which descriptors are free and can be allocated
			// Call ack in order to advance the counter, otherwise we eventually
			// run out of the descriptors to allocate on this ring
			ndma_ack_completed_desc(eng, ring, count);
			break;
		}
		udelay(one_loop_sleep);
	}
	if (i > loop) {
		pr_err("DMA completion timeout for %s q%d\n", eng->udma.name, ring->qid);
		ret = -1;
		goto error;
	}

error:
	if (rxc.ptr)
		kfree(rxc.ptr);

	return ret;
}

static int ndma_memcpy64k(struct ndma_eng *eng, struct ndma_ring *ring, dma_addr_t src,
			  dma_addr_t dst, u32 size, bool set_dmb)
{
	int ret = -1;
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, src, dst, size, set_dmb, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}
	// Start the DMA
	ret = udma_m2m_copy_start(&eng->udma, ring->qid, 1, 1);
	if (ret) {
		pr_err("failed to start DMA copy for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return ret;
}

int ndma_memcpy(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u32 size)
{
	u32 chunk_size, remaining;
	int pending_transfers = 0;
	// max number of usable descriptors - we never allocate the last 16 (max_num_... ) and need to
	// keep one free for checking completion
	const u32 sync_threshold = DMA_H2T_DESC_COUNT - UDMA_MAX_NUM_CDESC_PER_CACHE_LINE - 1;
	u32 offset;
	int ret = 0;
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	int eng_id = DMA_ENG_IDX_H2T + (nc_id * V1_DMA_ENG_PER_NC);

	eng = &(nd->ndma_engine[eng_id]);
	queue = &eng->queues[MAX_DMA_RINGS - 1];
	ring = &queue->ring_info;

	chunk_size = size < MAX_DMA_DESC_SIZE ? size : MAX_DMA_DESC_SIZE;
	remaining = size;
	mutex_lock(&eng->h2t_ring_lock);

	for (offset = 0; remaining; offset += chunk_size, remaining -= chunk_size) {
		if (remaining < MAX_DMA_DESC_SIZE)
			chunk_size = remaining;

		dma_addr_t src_offset, dst_offset;
		src_offset = src + offset;
		dst_offset = dst + offset;
		if (++pending_transfers == sync_threshold || chunk_size == remaining) {
			// no more room, transfer what's been queued so far OR last chunk
			ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, chunk_size, true);
			if (ret)
				goto fail;
			ret = ndma_memcpy_wait_for_completion(eng, ring, pending_transfers);
			if (ret)
				goto fail;
			pending_transfers = 0;
		} else {
			ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, chunk_size, false);
			if (ret)
				goto fail;
		}
		trace_dma_memcpy(nd, nc_id, src_offset, dst_offset, chunk_size, pending_transfers);
	}

fail:
	mutex_unlock(&eng->h2t_ring_lock);
	return ret;
}

int ndma_memcpy_mc(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u32 src_offset, u32 dst_offset, u32 size)
{
	dma_addr_t src_pa, dst_pa;
	u32 nc_id = 0; //default use NC 0

	if (src_mc->mem_location == MEM_LOC_HOST) {
		src_pa = virt_to_phys(src_mc->va) | PCIEX8_0_BASE;
	} else {
		src_pa = src_mc->pa;
		nc_id = src_mc->nc_id;
	}
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | PCIEX8_0_BASE;
	} else {
		dst_pa = dst_mc->pa;
		nc_id = dst_mc->nc_id;
	}
	dst_pa += dst_offset;

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

int ndma_memcpy_buf_to_mc(struct neuron_device *nd, void *buffer, u32 src_offset,
			  struct mem_chunk *dst_mc, u32 dst_offset, u32 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	src_pa = virt_to_phys(buffer) | PCIEX8_0_BASE;
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | PCIEX8_0_BASE;
	} else {
		dst_pa = dst_mc->pa;
		nc_id = dst_mc->nc_id;
	}
	dst_pa += dst_offset;

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

int ndma_memcpy_buf_from_mc(struct neuron_device *nd, void *buffer, u32 dst_offset,
			    struct mem_chunk *src_mc, u32 src_offset, u32 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	dst_pa = virt_to_phys(buffer) | PCIEX8_0_BASE;
	dst_pa += dst_offset;

	if (src_mc->mem_location == MEM_LOC_HOST) {
		src_pa = virt_to_phys(src_mc->va) | PCIEX8_0_BASE;
	} else {
		src_pa = src_mc->pa;
		nc_id = src_mc->nc_id;
	}
	src_pa += src_offset;

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

/**
 * Check whether given address is allocated in host memory by given pid and in given ND.
 */
static bool ndma_is_valid_host_mem_from_nd(pid_t pid, u8 nd_index, phys_addr_t pa)
{
	struct neuron_device *nd;
	if (nd_index >= MAX_NEURON_DEVICE_COUNT)
		return false;
	nd = neuron_pci_get_device(nd_index);
	if (nd == NULL)
		return false;
	if (pid != nd->current_pid)
		return false;

	return mpset_search_mc(&nd->mpset, pa) != NULL;
}

/**
 * Check whether given PA is valid host memory allocation.
 */
static bool ndma_is_valid_host_mem(struct neuron_device *nd, phys_addr_t pa)
{
	bool found = false;
	int i;

	read_lock(&nd->mpset.rblock);
	// common case - check whether the PA is allocated from the current ND
	found = ndma_is_valid_host_mem_from_nd(nd->current_pid, nd->device_index, pa);
	if (found)
		goto done;
	// chaining - check neighbor NDs
	found = ndma_is_valid_host_mem_from_nd(nd->current_pid, nd->device_index - 1, pa);
	if (found)
		goto done;
	found = ndma_is_valid_host_mem_from_nd(nd->current_pid, nd->device_index + 1, pa);
	if (found)
		goto done;
	// check all devices
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
		// skip already checked devices
		if (i >= nd->device_index - 1 && i <= nd->device_index + 1)
			continue;
		found = ndma_is_valid_host_mem_from_nd(nd->current_pid, i, pa);
		if (found)
			goto done;
	}

done:
	read_unlock(&nd->mpset.rblock);
	if (!found)
		pr_err("nd%d:invalid host memory(%#llx) in DMA descriptor\n", nd->device_index, pa);
	return found;
}

int ndma_memcpy_dma_copy_descriptors(struct neuron_device *nd, void *buffer, u32 src_offset,
				     struct mem_chunk *dst_mc, u32 dst_offset, u32 size,
				     u32 desc_type)
{
	u32 curr_size = size;
	union udma_desc *desc = (union udma_desc *)buffer;
	phys_addr_t pa;

	// Check the validity of the desc physical addresses
	while (curr_size > 0) {
		if (desc_type == NEURON_DMA_QUEUE_TYPE_TX) {
			pa = desc->tx.buf_ptr;
		} else if (desc_type == NEURON_DMA_QUEUE_TYPE_RX) {
			pa = desc->rx.buf1_ptr;
		} else {
			return -1;
		}

		// west side: PCIEX4_1_BASE: 0x00c00000000000 host: PCIEX8_0_BASE: 0x00400000000000
		// If west side is set then even host bit is set. When mc_alloc is called we set only the host bit
		// and insert into tree.. If some one sets the west side on that PA, then there is no way to check that,
		// since there could be a tdram address that could have the west side set
		// (that will look as though host is also set)
		if (((pa & PCIEX8_0_BASE) == PCIEX8_0_BASE) &&
		    ((pa & PCIEX4_1_BASE) != PCIEX4_1_BASE)) {
			if (!ndma_is_valid_host_mem(nd, pa & ~PCIEX8_0_BASE))
				return -EINVAL;
		}
		curr_size = curr_size - sizeof(union udma_desc);
		desc++;
	}

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		memcpy(dst_mc->va + dst_offset, buffer + src_offset, size);
		return 0;
	} else {
		return ndma_memcpy_buf_to_mc(nd, buffer, src_offset, dst_mc, dst_offset, size);
	}
}

static int ndmar_queue_read_state(struct udma_q *hw_q, struct neuron_dma_queue_state *result)
{
	u32 low, high;

	result->sw_status = hw_q->status;
	if (reg_read32(&hw_q->q_regs->rings.status, &result->hw_status))
		return -EIO;

	if (reg_read32(&hw_q->q_regs->rings.drl, &low))
		return -EIO;
	result->length = low & UDMA_M2S_Q_TDRL_OFFSET_MASK;

	if (reg_read32(&hw_q->q_regs->rings.drbp_high, (u32 *)&result->base_addr))
		return -EIO;
	result->base_addr <<= 32;
	if (reg_read32(&hw_q->q_regs->rings.drbp_low, &low))
		return -EIO;
	result->base_addr |= (low & UDMA_M2S_Q_TDRBP_LOW_ADDR_MASK);

	if (reg_read32(&hw_q->q_regs->rings.crbp_high, &high))
		return -EIO;
	if (reg_read32(&hw_q->q_regs->rings.crbp_low, &low))
		return -EIO;
	result->completion_base_addr = ((u64)high << 32) | low;

	if (reg_read32(&hw_q->q_regs->rings.drhp, &low))
		return -EIO;

	result->head_pointer = low & UDMA_M2S_Q_TDRHP_OFFSET_MASK;

	if (reg_read32(&hw_q->q_regs->rings.drtp, &low))
		return -EIO;
	result->tail_pointer = low & UDMA_M2S_Q_TDRTP_OFFSET_MASK;

	if (reg_read32(&hw_q->q_regs->rings.crhp, &low))
		return -EIO;
	result->completion_head = low & UDMA_M2S_Q_TDRHP_OFFSET_MASK;

	return 0;
}

int ndmar_queue_get_state(struct neuron_device *nd, int eng_id, int qid,
			  struct neuron_dma_queue_state *tx, struct neuron_dma_queue_state *rx)
{
	int ret;
	struct ndma_eng *eng;
	struct udma_q *m2s_queue, *s2m_queue;

	eng = &(nd->ndma_engine[eng_id]);
	m2s_queue = &eng->udma.udma_q_m2s[qid];
	s2m_queue = &eng->udma.udma_q_s2m[qid];
	ret = ndmar_queue_read_state(m2s_queue, tx);
	if (ret)
		return ret;
	ret = ndmar_queue_read_state(s2m_queue, rx);

	return ret;
}
