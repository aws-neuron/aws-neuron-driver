// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>
#include <linux/mm.h>

#include "udma/udma.h"
#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"
#include "neuron_dhal.h"
#include "neuron_pci.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_dma_wait);
#endif


//#define NUNUSED	__attribute__ ((unused))

struct neuron_device;

static void ndma_ack_completed_desc(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);
}

static inline u32 ndma_mc_pair_to_nc( struct mem_chunk *src_mc, struct mem_chunk *dst_mc)
{
	if (src_mc->mem_location != MEM_LOC_HOST)
		return src_mc->nc_id;
	else
		return dst_mc->nc_id;

	// Note: In the case where this is a host-to-host transfer we end up using the dst_mc's nc_id
}

/**
 * ndma_dma_ctx_get_next_handle()
 *
 *    Return the next dma context handle based on the prev handle.
 *    The previous handle is the handle we will be waiting on when the next transfer is started.
 *    
 *    For an Async transfer the transition progression is NONE->ASYNC1->ASYNC2->ASYNC1.... until we finish the transfer.
 *    Basically starting out with NONE then toggling between ASYNC1 and ASYNC2.
 *
 *    In the case of a synchronous transfer, the prev transfer handle is the SYNC transfer handle
 *    since we will be waiting on the transfer we just started. So the progression is 
 *    SYNC->SYNC->SYNC.... until we finish the transfer.
 *
 */
static inline int ndma_dma_ctx_get_next_handle( int pdma_ctx_handle, int * dma_ctx_handle)
{
	if (pdma_ctx_handle < NEURON_DMA_H2T_CTX_HANDLE_NONE || pdma_ctx_handle > NEURON_DMA_H2T_CTX_HANDLE_ASYNC2) {
		return -EINVAL;
	}

	switch (pdma_ctx_handle) {
		case NEURON_DMA_H2T_CTX_HANDLE_NONE:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_ASYNC1;
		   break;
		case  NEURON_DMA_H2T_CTX_HANDLE_SYNC:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_SYNC;
		   break;
		case  NEURON_DMA_H2T_CTX_HANDLE_ASYNC1:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_ASYNC2;
		   break;
		case  NEURON_DMA_H2T_CTX_HANDLE_ASYNC2:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_ASYNC1;
		   break;
	}
	return 0;
}

/**
 * memchunk to dma phy addr
 *
 */
static inline dma_addr_t ndma_mc_to_pa( struct mem_chunk *mc)
{
	if (mc->mem_location == MEM_LOC_HOST)
		return virt_to_phys(mc->va) | ndhal->ndhal_address_map.pci_host_base;   // why isn't this already set???
	else 
		return mc->pa;
}


/**
 * ndma_prefetch_user_pages()
 *
 *    Prefetch user buffer.
 *
 */
static int ndma_prefetch_user_pages( unsigned long start, int nr_pages)
{
	int nr_pinned;
	struct page **p = NULL;
	unsigned int gup_flags = FOLL_WRITE;

	// we technically check access here.

	p = kcalloc( nr_pages, sizeof(struct page *), GFP_KERNEL);

	if (!p) {
		pr_info("failed to allocate memory\n");
		return -ENOMEM;
	}

	nr_pinned = get_user_pages_fast( start, nr_pages, gup_flags, p);
	if (nr_pinned > 0) {
		int i;
		for (i = 0; i < nr_pinned; i++) {
			put_page(p[i]); // need to decide if we put page here or do it later.  If we do it later, need to grab context
		}
	} else {
		pr_info("prefetch failed\n");
	}

	kfree(p);

	return 0;
}


static inline int _ndma_prefetch_user_pages( unsigned long start, int len)
{
	const unsigned long offset = start & (PAGE_SIZE-1);
	int nr_pages = DIV_ROUND_UP(offset + len, PAGE_SIZE);

	return ndma_prefetch_user_pages( start & PAGE_MASK, nr_pages);
}


#define DMA_COMPLETION_MARKER_SIZE sizeof(u32)
#define DMA_COMPLETION_MARKER 0xabcdef01

/*
 * return descriptor for this dma_ctx_handle
 *
 *
 */
static inline void * ndma_memcpy_get_completion_buf( struct ndma_eng *eng, struct ndma_ring *ring, int dma_ctx_handle)
{
	if (eng->used_for_h2t)
		return ring->h2t_completion.ptr + dma_ctx_handle * 2 * DMA_COMPLETION_MARKER_SIZE;
	else
		return kmalloc(DMA_COMPLETION_MARKER_SIZE * 2, GFP_KERNEL);
}

static inline struct ndma_h2t_dma_context * ndma_get_dma_ctx( struct ndma_eng *eng, struct ndma_ring *ring, int dma_ctx_handle)
{
	if (dma_ctx_handle == -1) return NULL;

	if (eng->used_for_h2t)
    	return &ring->h2t_dma_ctx[dma_ctx_handle];
	else  {
		pr_info("allocating descriptor for non-h2t\n");   // FIXME remove at some point
    	return kmalloc( sizeof(struct ndma_h2t_dma_context), GFP_KERNEL);
	}
}

static inline void ndma_release_dma_ctx( struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_h2t_dma_context * dma_ctx)
{
	if (dma_ctx == NULL)
		return;
	if (eng->used_for_h2t) {
		dma_ctx->inuse = false;
	} else {
		if (dma_ctx->completion_ptr != NULL)
			kfree( dma_ctx->completion_ptr);
		kfree( dma_ctx);
	}
}


/*
 * ndma_memcpy_add_completion_desc()
 *
 *    add a completion entry to the ring 
 *
 */
int ndma_memcpy_add_completion_desc( struct ndma_eng *eng, struct ndma_ring *ring, void * completion_buffer)
{
	int ret = 0;
	struct udma_ring_ptr completion;
	volatile u32 *dst;
	volatile u32 *src;

	completion.ptr = completion_buffer;

	dst = (volatile u32 *)(completion.ptr + DMA_COMPLETION_MARKER_SIZE);
	src = (volatile u32 *)completion.ptr;

	// set the src value to the marker
	WRITE_ONCE(*src, DMA_COMPLETION_MARKER);
	WRITE_ONCE(*dst, 0);

	completion.addr = virt_to_phys(completion.ptr) | ndhal->ndhal_address_map.pci_host_base;
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, completion.addr,
					completion.addr + DMA_COMPLETION_MARKER_SIZE,
					DMA_COMPLETION_MARKER_SIZE, UDMA_M2M_BARRIER_NONE, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor on nd%02d for %s q%d\n", eng->nd->device_index, eng->udma.name, ring->qid);
		ret = -1;
		goto error;
	}

error:
	return ret;
}

int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count, void * ptr, bool async, bool is_intra_device_dma)
{
	int ret = 0;
	volatile u32 *dst;
	volatile u32 *src;
	u64 i;
	u64 first_wait_time, wait;

	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time(count, async, &first_wait_time, &wait);
	if (is_intra_device_dma && !async) {
		first_wait_time = 10; // device-to-device DMA is much faster, just choose a small value independent of number of descriptors
		wait = wait/200; // can probably be set even lower if required
	}

	unsigned long one_loop_sleep = 1; // poll every 1 usecs
	u64 loop = wait / one_loop_sleep + 1;

	dst = (volatile u32 *)(ptr + DMA_COMPLETION_MARKER_SIZE);
	src = (volatile u32 *)ptr;


#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_dma_wait, 1)) {
		ret = -ETIMEDOUT;
		goto error;
	}
#endif

	udelay(first_wait_time);
	for (i = 0; i <= loop; i++) {
		u32 dst_val = READ_ONCE(*dst);
		// this descriptor is executed, meaning all other have completed
		if (dst_val == DMA_COMPLETION_MARKER) {
			// reset in case we are going to use this ring again
			WRITE_ONCE(*dst, 0);                                                        // this isn't strictly necessary but it will detect improper reuse issues
			WRITE_ONCE(*src, DMA_COMPLETION_MARKER);                                    // this isn't strictly necessary but it will detect improper reuse issues
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
		pr_err("DMA completion timeout on nd%02d for %s q%d desc count %u\n", eng->nd->device_index, eng->udma.name, ring->qid, count);
		ret = -1;
		goto error;
	}

error:
	return ret;
}

int ndma_memcpy64k(struct ndma_eng *eng, struct ndma_ring *ring, dma_addr_t src,
			  dma_addr_t dst, u32 size, int barrier_type)
{
	int ret = -1;

	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, src, dst, size, barrier_type, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return ret;
}

/**
 * ndma_memcpy_chunks()
 *
 *
 *   caveats/notes:
 *     need to figure out inuse & cleanup
 *
 */
static int ndma_memcpy_chunks( struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_h2t_dma_context * dma_ctx)
{
	int        ret;
	dma_addr_t src;
   	dma_addr_t dst;
	u64        chunk_size;
	u64        remaining;
	u64        offset;
	int        pending_transfers;
	bool       done;
	const u32  sync_threshold = DMA_H2T_DESC_COUNT/2 - UDMA_MAX_NUM_CDESC_PER_CACHE_LINE - 1;

	src               = dma_ctx->src;
	dst               = dma_ctx->dst;         
	remaining         = dma_ctx->remaining;
	offset            = dma_ctx->offset;
	done              = false;
   	pending_transfers = 0; 
	chunk_size        = MAX_DMA_DESC_SIZE; 

	while (!done) {
		dma_addr_t src_offset;
		dma_addr_t dst_offset;

		if (remaining <= MAX_DMA_DESC_SIZE) {
			chunk_size = remaining;
		} 

		if ((chunk_size == remaining) || (pending_transfers == sync_threshold)) {
			done    = true;
		}

		src_offset = dma_ctx->smove ? src + offset : src;
		dst_offset = dma_ctx->dmove ? dst + offset : dst;

		ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, (u32)chunk_size, ndhal->ndhal_ndma.ndma_get_m2m_barrier_type(done));
		if (ret) { 
			return ret;
		}

		offset    += chunk_size; 
		remaining -= chunk_size;
		pending_transfers++;
		
		//FIXME trace_dma_memcpy(nd, nc_id, src_offset, dst_offset, chunk_size, pending_transfers);
	}

	// write completion descriptor, kick off DMAs, record pending xfers and data outstanding and prefetch if requested
	//
	ret = ndma_memcpy_add_completion_desc( eng, ring, dma_ctx->completion_ptr);
	if (ret) {
		return ret; 
	}

	pending_transfers++;
	dma_ctx->pending_transfers = pending_transfers;
	dma_ctx->outstanding       = dma_ctx->remaining - remaining;
			
	ret = udma_m2m_copy_start(&eng->udma, ring->qid, pending_transfers, pending_transfers);
	if (ret) {
		pr_err("failed to start DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return 0;
}

static int _ndma_memcpy_wait_for_completion( struct neuron_device *nd, u32 nc_id, int qid, struct ndma_eng *eng, struct ndma_ring *ring, 
									  struct ndma_h2t_dma_context * dma_ctx, struct ndma_h2t_dma_context * ndma_ctx)
{
	int ret;
	bool async = (dma_ctx != ndma_ctx);

	while(true) {

		ret = ndma_memcpy_wait_for_completion(eng, ring, dma_ctx->pending_transfers, dma_ctx->completion_ptr, async, false);

		if (ret == 0) 
			return ret;

		// if the memcpy starts within a NeuronCore reset window, 
		// the timeout is possible due to DMA hanging caused by V2 hardware issue.
		// if so, restart DMA and retry the memcpy
		if (!ndhal->ndhal_ndma.ndma_retry_memcpy) {
			break;
		}

		if (!nr_op_in_reset_wnd(dma_ctx->start_time, nd)) {
			break;
		}
		
		pr_info("Failed to copy memory during a NeuronCore reset: nd %d, src %#llx, dst %#llx, size %llu. Retrying the copy.\n", 
				nd->device_index, dma_ctx->src, dma_ctx->dst, dma_ctx->size);

		dma_ctx->start_time = get_jiffies_64();

		ret = ndmar_h2t_ring_init(eng, qid);

		if (ret) {
			pr_err("H2T ring init failed on nd %d: ret %d\n", nd->device_index, ret);
			break;
		}
	
		// restart dmas
		// 
		ret = ndma_memcpy_chunks( eng, ring, dma_ctx);
		if (ret)
			break;
		
		if (dma_ctx != ndma_ctx) {
			ret = ndma_memcpy_chunks( eng, ring, ndma_ctx);
			if (ret)
				break;
		}

		async = false;
	}	
	return ret;
}

/** 
 *   
 * Common function for dma content from src to dst
 * if smove is set then the source offset will keep changing after every max desc size is copied
 * if dmove is set then the dest offset will keep changing after every max desc size is copied
 *
 */
static int ndma_memcpy_offset_move(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u64 size, bool smove, bool dmove, 
		                           u64 prefetch_addr, int pwait_handle, int wait_handle)
{
	int ret = 0;

	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	// for v2 the last one is reserved for collectives
	const int qid = ndhal->ndhal_ndmar.ndmar_get_h2t_qid(nc_id);

	struct ndma_eng   *eng   = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring  *ring  = &queue->ring_info;

	struct ndma_h2t_dma_context * dma_ctx  = ndma_get_dma_ctx( eng, ring, wait_handle);
	struct ndma_h2t_dma_context * pdma_ctx = (eng->used_for_h2t) ? ndma_get_dma_ctx( eng, ring, pwait_handle) : dma_ctx;


	// The h2t_ring_lock two things
	//   1. access to the ring itself
	//   2. usage of the SYNC dma context (basically even though we specify we are using the SYNC ctxt handle outside this routine
	//      the SYNC dma context itself is only used within this routine.
	//
	mutex_lock(&eng->h2t_ring_lock);

    // initialize the DMA context
	dma_ctx->inuse             = true;
	dma_ctx->eng               = eng;
	dma_ctx->ring              = ring;
	dma_ctx->src               = src;
	dma_ctx->dst               = dst;
	dma_ctx->offset            = 0ull;
	dma_ctx->remaining         = size;
	dma_ctx->pending_transfers = 0ull;
	dma_ctx->size              = size;
	dma_ctx->smove             = smove;
	dma_ctx->dmove             = dmove;
    dma_ctx->completion_ptr    = ndma_memcpy_get_completion_buf( eng, ring, wait_handle);

	// Sanity check 
	if ((pdma_ctx != NULL) && (!pdma_ctx->inuse)) {
		pr_err("Async dma previous request on nd %d nc %d has invalid state. src %#llx, dst %#llx, size %llu.\n", 
				nd->device_index, nc_id, pdma_ctx->src, pdma_ctx->dst, pdma_ctx->size);
		ret = -EINVAL;
		goto fail;
	}

	dma_ctx->start_time = get_jiffies_64();

	while (true) {

		ret = ndma_memcpy_chunks( eng, ring, dma_ctx);

		if (ret) {
			goto fail;
		}

		if (prefetch_addr  && dma_ctx->offset == 0) { 
			_ndma_prefetch_user_pages( prefetch_addr, dma_ctx->size); 
		}

		if (pdma_ctx != NULL) {

			ret = _ndma_memcpy_wait_for_completion( nd, nc_id, qid, eng, ring, pdma_ctx, dma_ctx);

			if (ret) {
				goto fail;
			} else {

				if (dma_ctx->outstanding == dma_ctx->remaining)  {
					break;
				}

				if (dma_ctx != pdma_ctx) {
					pr_err("Async dma request on nd %d nc %d is too large. src %#llx, dst %#llx, size %llu.\n", 
							nd->device_index, nc_id, dma_ctx->src, dma_ctx->dst, dma_ctx->size);
					ret = -EINVAL;
					goto fail;
				}	

				dma_ctx->start_time         = get_jiffies_64();
				dma_ctx->remaining         -= dma_ctx->outstanding;
				dma_ctx->offset            += dma_ctx->outstanding;
			}
			
		} else {
			if (dma_ctx->outstanding == dma_ctx->remaining)
				break;
			pr_err("Async dma request on nd %d nc %d is too large\n", nd->device_index, nc_id);
			ret = -EINVAL;
			break;
		}
	}

fail:
	// release the dma_ctx in the event of a failure
	if (ret  && (dma_ctx != pdma_ctx))
		ndma_release_dma_ctx( eng, ring, dma_ctx);

	ndma_release_dma_ctx( eng, ring, pdma_ctx);
	
	mutex_unlock(&eng->h2t_ring_lock);
	return ret;
}

int ndma_memset(struct neuron_device *nd, struct mem_chunk *mc, u64 offset, u32 value, u64 size)
{
	u64 transfer_size, remaining_size;
	struct mem_chunk *memset_mc = nd->memset_mc;
	int ret = 0;

	mutex_lock(&nd->memset_lock);

	// memset the preallocated host memory with the value passed
	transfer_size = size > MEMSET_HOST_BUF_SIZE ? MEMSET_HOST_BUF_SIZE : size;
	memset(memset_mc->va, value, transfer_size);

	// transfer the contents to the memory
	ret = ndma_memcpy_mc(nd, memset_mc, mc, 0, offset, transfer_size);
	if (ret) {
		pr_err("memset memory failed for size:%llu\n", transfer_size);
		goto error;
	}
	remaining_size = size - transfer_size;
	if (remaining_size) {
		// copy rest of memroy with zers from the src
		ret = ndma_memcpy_offset_move(nd, mc->nc_id, mc->pa + offset, mc->pa + offset + transfer_size, remaining_size, false, true, 0, 
										NEURON_DMA_H2T_CTX_HANDLE_SYNC, NEURON_DMA_H2T_CTX_HANDLE_SYNC);
		if (ret) {
			pr_err("memset device to device failed for size:%llu\n", remaining_size);
			goto error;
		}
	}

error:
	mutex_unlock(&nd->memset_lock);
	return ret;
}

int ndma_memcpy(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u64 size)
{
	return ndma_memcpy_offset_move(nd, nc_id, src, dst, size, true, true, 0, NEURON_DMA_H2T_CTX_HANDLE_SYNC, NEURON_DMA_H2T_CTX_HANDLE_SYNC);
}

int ndma_memcpy_mc_async(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u64 src_offset, u64 dst_offset, u64 size, u64 prefetch_addr, int pdma_ctx_handle, int *dma_ctx_handle)
{
	dma_addr_t src_pa, dst_pa;
	u32 nc_id = 0;
	int ret;

	ret = ndma_dma_ctx_get_next_handle( pdma_ctx_handle, dma_ctx_handle);

	if (ret) {
		return ret;
	}

	nc_id  = ndma_mc_pair_to_nc( src_mc, dst_mc);
	src_pa = ndma_mc_to_pa( src_mc) + src_offset;
	dst_pa = ndma_mc_to_pa( dst_mc) + dst_offset;

	return ndma_memcpy_offset_move(nd, nc_id, src_pa, dst_pa, size, true, true, prefetch_addr, pdma_ctx_handle, *dma_ctx_handle);
}

int ndma_memcpy_mc(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u64 src_offset, u64 dst_offset, u64 size)
{
	dma_addr_t src_pa, dst_pa;
	u32 nc_id = 0; //default use NC 0

	if (src_mc->mem_location == MEM_LOC_HOST)
		src_pa = virt_to_phys(src_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	else {
		src_pa = src_mc->pa;
		nc_id = src_mc->nc_id;
	}
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	} else {
		dst_pa = dst_mc->pa;
		nc_id = dst_mc->nc_id;
	}
	dst_pa += dst_offset;

	// FIXME: H2H memcpy's src and dst mc should have dedicated nc_id such as -1
	if (src_mc->mem_location == MEM_LOC_HOST && dst_mc->mem_location == MEM_LOC_HOST) {
		nc_id = dst_mc->nc_id;
	}

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

/**
 * ndma_memcpy_mc_wait()
 *
 *    This is ugly, but gets the job done.  We have to get nc_id from the MCs, then from there we get engine id, queue id, ring id 
 *    in a bunch of separate calls.  Once we have the ring, we can extract the dma context to wait on...
 *
 *
 */
int ndma_memcpy_mc_wait( struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc, int dma_ctx_handle)
{
	int ret;
	const u32  nc_id         = ndma_mc_pair_to_nc( src_mc, dst_mc);
	const int eng_id         = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	const int qid            = ndhal->ndhal_ndmar.ndmar_get_h2t_qid(nc_id);
	struct ndma_eng *eng     = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring   = &queue->ring_info;
	struct ndma_h2t_dma_context * dma_ctx;

	// non-h2t we do sync under the covers
	if (!eng->used_for_h2t) {
		return 0;
	}

	dma_ctx  = ndma_get_dma_ctx( eng, ring, dma_ctx_handle);

	if (dma_ctx == NULL) {
		return -EINVAL;
	}

	if (!dma_ctx->inuse) {
		pr_err("trying to wait on async DMA context that is not in use on nd %d nc %d handle %d\n", nd->device_index, nc_id, dma_ctx_handle);
		return -EINVAL;
	}

    ret = _ndma_memcpy_wait_for_completion( nd, nc_id, qid, eng, ring, dma_ctx, dma_ctx);

	ndma_release_dma_ctx( eng, ring, dma_ctx);

	return ret;	
}


int ndma_memcpy_buf_to_mc(struct neuron_device *nd, void *buffer, u64 src_offset,
			  struct mem_chunk *dst_mc, u64 dst_offset, u64 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	src_pa = virt_to_phys(buffer) | ndhal->ndhal_address_map.pci_host_base;
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	} else {
		dst_pa = dst_mc->pa;
		nc_id = dst_mc->nc_id;
	}
	dst_pa += dst_offset;

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

int ndma_memcpy_buf_from_mc(struct neuron_device *nd, void *buffer, u64 dst_offset,
				struct mem_chunk *src_mc, u64 src_offset, u64 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	dst_pa = virt_to_phys(buffer) | ndhal->ndhal_address_map.pci_host_base;
	dst_pa += dst_offset;

	if (src_mc->mem_location == MEM_LOC_HOST) {
		src_pa = virt_to_phys(src_mc->va) | ndhal->ndhal_address_map.pci_host_base;
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
static bool ndma_is_valid_host_mem_from_nd(u8 nd_index, phys_addr_t pa)
{
	struct neuron_device *nd;
	bool found = false;

	if (nd_index >= MAX_NEURON_DEVICE_COUNT)
		return false;
	nd = neuron_pci_get_device(nd_index);
	if (nd == NULL)
		return false;
	if (!npid_is_attached(nd))
		return false;

	read_lock(&nd->mpset.rblock);
	found = mpset_search_mc(&nd->mpset, pa) != NULL;
	read_unlock(&nd->mpset.rblock);

	return found;
}

bool ndma_is_valid_host_mem(struct neuron_device *nd, phys_addr_t pa)
{
	bool found = false;
	int i;

	// common case - check whether the PA is allocated from the current ND
	found = ndma_is_valid_host_mem_from_nd(nd->device_index, pa);
	if (found)
		goto done;
	// chaining - check neighbor NDs
	found = ndma_is_valid_host_mem_from_nd(nd->device_index - 1, pa);
	if (found)
		goto done;
	found = ndma_is_valid_host_mem_from_nd(nd->device_index + 1, pa);
	if (found)
		goto done;
	// check all devices
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
		// skip already checked devices
		if (i >= nd->device_index - 1 && i <= nd->device_index + 1)
			continue;
		found = ndma_is_valid_host_mem_from_nd(i, pa);
		if (found)
			goto done;
	}

done:
	if (!found)
		pr_err("nd%d:invalid host memory(%#llx) in DMA descriptor\n", nd->device_index, pa);
	return found;
}

int ndma_memcpy_dma_copy_descriptors(struct neuron_device *nd, void *buffer, u64 src_offset,
					 struct mem_chunk *dst_mc, u64 dst_offset, u64 size,
					 u32 desc_type)
{
	u64 curr_size = size;
	union udma_desc *desc = (union udma_desc *)buffer;
	phys_addr_t pa;

	// Check the validity of the desc physical addresses
	while (curr_size > 0) {
		if (desc_type == NEURON_DMA_QUEUE_TYPE_TX)
			pa = desc->tx.buf_ptr;
		else if (desc_type == NEURON_DMA_QUEUE_TYPE_RX)
			pa = desc->rx.buf1_ptr;
		else
			return -1;

		int ret = ndhal->ndhal_ndma.ndma_validate_pa(nd, pa, dst_mc, desc_type);
		if (ret) {
			return ret;
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

static const u64 udma_blocked[] = { offsetof(struct udma_rings_regs, drbp_low), offsetof(struct udma_rings_regs, drbp_high),
									offsetof(struct udma_rings_regs, crbp_low), offsetof(struct udma_rings_regs, crbp_high),
									offsetof(struct udma_rings_regs, drtp_inc) };
int ndma_bar0_blocked_one_engine(u64 base, u64 off)
{
	int qid, dir;
	// check m2s and s2m
	for (dir = 0; dir < 2; dir++) {
		u64 q_start;
		u64 q_size = sizeof(union udma_q_regs);
		if (dir == 0) { // m2s
			q_start = base + offsetof(struct unit_regs_v4, m2s); // start of m2s block
			q_start += offsetof(struct udma_m2s_regs_v4, m2s_q); // start of q registers
		} else { // s2m
			q_start = base + offsetof(struct unit_regs_v4, s2m); // start of s2m block
			q_start += offsetof(struct udma_s2m_regs_v4, s2m_q); // start of q registers
		}
		for (qid = 0; qid < DMA_MAX_Q_V4; qid++) {
			u64 q_off = q_start + q_size * qid;
			int i;
			for (i = 0; i < sizeof(udma_blocked) / sizeof(udma_blocked[0]); i++) {
				if (off == q_off + udma_blocked[i]) {
					return -1;
				}
			}
		}
	}
	return 0;
}

/*
 * Zero copy impementation.
 *
 *
 *
 */

struct ndma_h2t_zcdma_context {
	struct ndma_eng  *eng;                // engine 
	struct ndma_ring *ring;               //
	void             *host_addr;          // host address
	dma_addr_t        dev_addr;           // device address
	u64               size;               // size for this transfer
	bool              direction;          // direction. true = to device
	u64               start_time;         // start time for this transfer
	int               nr_pages;           // number of pages for this transfer
	int               nr_desc;            // number of descriptors which is equal to pending transfers -1
	void             *completion_ptr;     // completion buffer pointer (host memory buffer we poll on for completions)
	struct page     **page_list;          // page structures tracking our pinned pages
};


#define NDMA_ZC_PAGES_PER_XFER  64        // number of pages in each zero copy dma transfer.  This is somewhat, but not 
										  // totally arbitrary.  We don't want to pin a lot of pages. We just want to
										  // pin enough where (approximately):  
										  //       dma time > (pin time + setup time + completion update + initial poll wait)
										  // That's the simple explanation. It's a tad more complicated in trading off smaller
										  // transfers where even if that equation doesn't hold, the overlap can be beneficial.
										  // Right now the sweet spot looks to be ~ 32 pages

/**
 * ndma_build_n_issue_zc_descs() 
 *
 *   build descriptors for a zero copy operation consisting of non-continuous
 *   physical host memory pages.  
 *
 *   explain how alignment is handled.
 *
 *   Todo:
 *     go i=0 to nr_pages
 *     Think about using some permanent location in HBM as source for completion descriptor update.  Like
 *     why are we reading across the PCIe bus to fetch completion data.
 */
static int ndma_build_n_issue_zc_descs( struct ndma_h2t_zcdma_context * dma_ctx)
{
	int            ret;
	unsigned long  offset        = (unsigned long)(dma_ctx->host_addr) & (PAGE_SIZE-1);
	dma_addr_t     dev_addr      = dma_ctx->dev_addr;
	dma_addr_t     pci_host_base = ndhal->ndhal_address_map.pci_host_base;
	u64            remaining     = dma_ctx->size;
	int            i = 0;
	u64            chunk_size;
	int            pending_transfers = 0;

	while (i < dma_ctx->nr_pages) {
		dma_addr_t src_addr;
		dma_addr_t dst_addr;

		// find the largest contiguous set of pages
		u64 contig_size = PAGE_SIZE - offset; // we might be starting from the middle of the first page
		dma_addr_t contig_start = page_to_phys(dma_ctx->page_list[i++]);
		dma_addr_t tmp = contig_start;
		for (; i < dma_ctx->nr_pages; i++) {
			dma_addr_t next = page_to_phys(dma_ctx->page_list[i]);
			if ((tmp + PAGE_SIZE) != next) { // done, not contiguous
				break;
			}
			contig_size += PAGE_SIZE;
		}

		if (dma_ctx->direction) {
			src_addr = (contig_start + offset) | pci_host_base;
			dst_addr = dev_addr;
		} else {
			src_addr = dev_addr;
			dst_addr = (contig_start + offset) | pci_host_base;
		}
		// after the first page the offset is always 0
		offset     = 0;

		// contiguous memory can be larger than a descriptor size, loop until we are done with this chunk of contiguous memory
		while ((contig_size > 0) && (remaining > 0)) { // if the end is less then a full page contig_size could be > remaining
			chunk_size = (remaining < contig_size) ? remaining : contig_size;
			if (chunk_size > MAX_DMA_DESC_SIZE)
				chunk_size = MAX_DMA_DESC_SIZE;

			ret = udma_m2m_copy_prepare_one(&dma_ctx->eng->udma, dma_ctx->ring->qid, src_addr, dst_addr, chunk_size, remaining == chunk_size, false); // set the barrier if the last descriptor
			if (ret) {
				pr_err("failed to prepare DMA descriptor for %s q%d\n", dma_ctx->eng->udma.name, dma_ctx->ring->qid);
				goto error;
			}
			dev_addr  += chunk_size;
			src_addr  += chunk_size;
			dst_addr  += chunk_size;
			remaining -= chunk_size;
			contig_size -= chunk_size;
			pending_transfers++;
		}
	}
	
	dma_ctx->nr_desc = pending_transfers;

	ret = ndma_memcpy_add_completion_desc( dma_ctx->eng, dma_ctx->ring, dma_ctx->completion_ptr);
	if (ret) {
		goto error;
	}

	pending_transfers++;

	ret = udma_m2m_copy_start(&dma_ctx->eng->udma, dma_ctx->ring->qid, pending_transfers, pending_transfers);

	if (ret) {
		pr_info("copy start failed %d\n", ret);
	}

error:
	return ret;
}

/**
 * ndma_zero_copy_wait_for_completion()
 *
 *
 *
 */
static int ndma_zero_copy_wait_for_completion( struct neuron_device *nd, u32 nc_id, struct ndma_eng   *eng, struct ndma_ring  *ring,
											   struct ndma_h2t_zcdma_context * dma_ctx, struct ndma_h2t_zcdma_context * ndma_ctx)
{
	int  ret;
	bool async = true;

	while(true) {
		ret = ndma_memcpy_wait_for_completion(eng, ring, dma_ctx->nr_desc+1, dma_ctx->completion_ptr, async, false);  // FIXM we shouldn't even be waiting 1usec here

		if (ret == 0) {
			if (dma_ctx->direction)
				unpin_user_pages( dma_ctx->page_list, dma_ctx->nr_pages);
			else
				unpin_user_pages_dirty_lock( dma_ctx->page_list, dma_ctx->nr_pages, true);
			return ret;
		}

		// if the memcpy starts within a NeuronCore reset window,
		// the timeout is possible due to DMA hanging caused by hardware issue.
		// if so, restart DMA and retry the memcpy
		if (narch_get_arch() != NEURON_ARCH_V2) {    // FIXME - this should be if (!ndhal.tpb_reset_dma_retry) or part of dma_ctx
			break;
		}

		if (!nr_op_in_reset_wnd(dma_ctx->start_time, nd)) {
			break;
		}

		pr_info( "Failed to copy memory during a NeuronCore reset: nd %d, host %#llx, dev  %#llx, size %llu. Retrying the copy.\n",
				nd->device_index, (dma_addr_t)dma_ctx->host_addr, dma_ctx->dev_addr, dma_ctx->size);

		dma_ctx->start_time = get_jiffies_64();
		if (ndma_ctx != NULL)
			ndma_ctx->start_time = get_jiffies_64();

		ret = ndmar_h2t_ring_init(eng, ring->qid);

		if (ret) {
			pr_err("H2T ring init failed on nd %d: ret %d\n", nd->device_index, ret);
			break;
		}

		// restart dmas
		//
		ret = ndma_build_n_issue_zc_descs( dma_ctx);
		if (ret)
			break;

		if (ndma_ctx != NULL) {
			ret = ndma_build_n_issue_zc_descs( ndma_ctx);
			if (ret) {
				ndma_memcpy_wait_for_completion(eng, ring, dma_ctx->nr_desc+1, dma_ctx->completion_ptr, false, false);
				break;
			}
		}

		async = false;
	}

	// If we are exiting here, we've failed so unpin pages associated with the DMA.  If the next DMA
	// context is valid, do an obligatory wait for the DMA operation so we don't splat data on someone 
	// else's memory just in case the physical pages are reassigned after unpinning.
	//
	unpin_user_pages( dma_ctx->page_list, dma_ctx->nr_pages);

	// blindly wait - 
	if (ndma_ctx != NULL) {
		ndma_memcpy_wait_for_completion(eng, ring, ndma_ctx->nr_desc+1, ndma_ctx->completion_ptr, false, false);
		unpin_user_pages( ndma_ctx->page_list, ndma_ctx->nr_pages);
	}

	return ret;
}

/**
 * ndma_memcpy_zero_copy()
 *
 *   dma data between a user space virtual address range and a contiguous location in device memory.
 *   In order to do this, we need to know the physical pages are associated with
 *   the user virtual address range and we need to make sure those physical pages stay
 *   associated with the user virtual address range while the DMA is happening.
 *   
 *   How do we do this?  By asking the kernel to pin the physical pages in memory until we are 
 *   done with them.  But our transaction could be large, the physical pages won't be contiguous,
 *   and pinning takes CPU cycles, so we break the dma transfer up into a series of smaller transfers
 *   where we pipeline the pinning of physical pages with dma transfers.
 *
 *   We use pin_user_pages_fast() to reduce pinning overhead because we know the process can't go 
 *   away while we are down here doing our thing in the kernel within a single IOCTL call. 
 *   
 *   We ping pong back and forth between two dma contexts. So while dma for context A is in progress, 
 *   we are pinning pages and starting dmas for context B. 
 *
 *   Algorithm goes like this:
 *      initial a pair of dma contexts 
 *      prev dma ctx = null
 *      lock()
 *      while still more data remaining
 *         current dma ctx = next available context
 *         init current dma context
 *         calc size of the transfer for this dma context.  We want to transfer up to page boundaries
 *         calc number of pages that need to be pinned for this dma
 *         pin host pages in memory
 *         generate descriptors for 
 *         if prev dma ctx != NULL, wait for the prev dma to complete
 *         update host address, device address and ammount remaining
 *      wait for the last dma ctx to complete
 *      unlock()
 *      free resources
 *
 *  Notes:
 *    unpinning responsibilities. Up until a dma is successfully launched, this routine is responsible for unpinning
 *    host memory.  After that ndma_zero_copy_wait_for_completion() owns responsibility for unpinning pages.
 *
 *    We don't do this here, but pinning user pages across system (IOCTL) calls has a number of additional requirements.
 *    We would have to cleanup any pinned pages when the process goes away, so any pinned pages have to get tracked in 
 *    process context.
 *    
 * direction == true means write from host to device
 *
 */

static int ndma_memcpy_zero_copy(struct neuron_device *nd, u32 nc_id, void * host_addr, dma_addr_t dev_addr, u64 size, bool direction)
{
	int ret = 0;

	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	const int qid = ndhal->ndhal_ndmar.ndmar_get_h2t_qid(nc_id);   // TODO - this needs direction or transfer type to select qid
	struct ndma_eng   *eng   = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring  *ring  = &queue->ring_info;
	struct ndma_h2t_zcdma_context   dma_ctx_tbl[2] = {0};
	struct ndma_h2t_zcdma_context * dma_ctx;
	struct ndma_h2t_zcdma_context * pdma_ctx = NULL;
	int    next_dma_idx = 0;
	int    i;
	u64    remaining  = size;
	u64    cpy_size = (NDMA_ZC_PAGES_PER_XFER*PAGE_SIZE < size) ? NDMA_ZC_PAGES_PER_XFER*PAGE_SIZE : size;
	int nr_pinned;

	// initialize the static fields in the dma contexts that are the same for every operation
	//
	for (i=0;i< 2;i++) {
		dma_ctx_tbl[i].eng            = eng;
		dma_ctx_tbl[i].ring           = ring;
		dma_ctx_tbl[i].direction      = direction;
		dma_ctx_tbl[i].page_list      = kcalloc( NDMA_ZC_PAGES_PER_XFER, sizeof(struct page *), GFP_KERNEL);
		dma_ctx_tbl[i].completion_ptr = kmalloc(DMA_COMPLETION_MARKER_SIZE * 2, GFP_KERNEL);

		if ((dma_ctx_tbl[i].page_list == NULL) || (dma_ctx_tbl[i].completion_ptr == NULL)) {
			pr_err("could not allocate memory for dma contexts on nd %d\n", nd->device_index);
			goto fail;
		}
	}
	pdma_ctx = NULL;

	mutex_lock(&eng->h2t_ring_lock);

	while (remaining) {
		unsigned long offset = (unsigned long)(host_addr) & (PAGE_SIZE-1);
		dma_ctx = &dma_ctx_tbl[next_dma_idx];
		dma_ctx->start_time = get_jiffies_64();
		dma_ctx->host_addr  = host_addr;
		dma_ctx->dev_addr   = dev_addr;
		dma_ctx->size       = (cpy_size == remaining) ? cpy_size : cpy_size - offset; // slightly non-obvious, we are setting up xfer size
		                                                                              // that only the first xfer has its starting address
																					  // not aligned to the page boundary.  First time around
																					  // offset >= 0 and cpy_size <= xfer size.  Other times
																					  // host_addr is aligned, offset = 0 and cpy_size = xfer_size
		dma_ctx->nr_pages   = DIV_ROUND_UP(offset + dma_ctx->size, PAGE_SIZE);

		//__GFP_SKIP_ZERO
		nr_pinned = pin_user_pages_fast((unsigned long)dma_ctx->host_addr & PAGE_MASK, dma_ctx->nr_pages, direction ? 0 : FOLL_WRITE, dma_ctx->page_list);
		if (nr_pinned != dma_ctx->nr_pages) {
			// if failed pin_fast because of page fault, do the regular pinning
			if (nr_pinned > 0)
				unpin_user_pages( dma_ctx->page_list, nr_pinned);

#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)))
			nr_pinned = pin_user_pages((unsigned long)dma_ctx->host_addr & PAGE_MASK, dma_ctx->nr_pages, direction ? 0 : FOLL_WRITE, dma_ctx->page_list);
#else
			nr_pinned = pin_user_pages((unsigned long)dma_ctx->host_addr & PAGE_MASK, dma_ctx->nr_pages, direction ? 0 : FOLL_WRITE, dma_ctx->page_list, NULL);
#endif
			if (nr_pinned != dma_ctx->nr_pages) {
				ret = -ENOMEM; // could use -EBUSY instead
				pr_err("could not pin host pages for zero copy dma on nd %d: nr_pinned %d\n", nd->device_index, nr_pinned);

				if (nr_pinned > 0)
					unpin_user_pages( dma_ctx->page_list, nr_pinned);
				// cleanup: wait for prev dma to complete (which also unpins pages)
				if (pdma_ctx != NULL)
					ndma_zero_copy_wait_for_completion( nd, nc_id, eng, ring, pdma_ctx, NULL);
				goto fail;
			}
		}

		// TODO need to have this for other architectures
		// for (i=0; i < dma_ctx->nr_pages; i++) {
		//     struct device
		// 	    dma_ctx->addr[i] = dma_map_page( nd->pdev->dev, dma_ctx_page_list[i], 0, PAGE_SIZE, DMA_TO_DEVICE/DMA_FROM_DEVICE);
		// 	    ret = dma_mapping_error(dev->dev, dma_ctx->addr[i]);
		//		if (ret) { }
		// }
		// flush_cache_range(vma,  
		//
		// TODO - may need a callback here to check descriptors

		ret = ndma_build_n_issue_zc_descs( dma_ctx);
		if (ret) {
			unpin_user_pages( dma_ctx->page_list, dma_ctx->nr_pages);
			// cleanup: wait for prev dma to complete (which also unpins pages)
			if (pdma_ctx != NULL) ndma_zero_copy_wait_for_completion( nd, nc_id, eng, ring, pdma_ctx, NULL);
			goto fail;
		}

		if (pdma_ctx != NULL) {
			ret = ndma_zero_copy_wait_for_completion( nd, nc_id, eng, ring, pdma_ctx, dma_ctx); 
			if (ret)
				goto fail;
		}
		pdma_ctx     = dma_ctx;
		next_dma_idx = (next_dma_idx+1) % 2;

		remaining -= dma_ctx->size;
		host_addr += dma_ctx->size;
		dev_addr  += dma_ctx->size;
		cpy_size   = (remaining < cpy_size) ? remaining : cpy_size;
	}

	ret = ndma_zero_copy_wait_for_completion( nd, nc_id, eng, ring, pdma_ctx, NULL);

fail:

	// release resources
	//
	for (i=0;i< 2;i++) {
		if (dma_ctx_tbl[i].page_list != NULL)
			kfree(dma_ctx_tbl[i].page_list);
		if (dma_ctx_tbl[i].completion_ptr != NULL) {
			kfree(dma_ctx_tbl[i].completion_ptr);
		}
	}
	mutex_unlock(&eng->h2t_ring_lock);

	return ret;
}

/**
 * ndma_memcpy_zero_copy_mc()
 *
 *   Wrapper around ndma_memcpy_zero_copy() that pulls nc_id and device phyical address from
 *   the mem chunk.
 *
 *   Todo:
 *     Range check the device address here.  
 *
 *   Assumptions:
 *     caller has done access_ok() check on the host address 
 *     if (!access_ok(blah) return -EFAULT;
 *     or check_copy_size()
 */
int ndma_memcpy_zero_copy_mc( struct neuron_device *nd,  void * host_addr, struct mem_chunk *dev_mc, u64 dev_offset, u64 size, bool direction)
{
	dma_addr_t dev_addr;
	u32 nc_id;

	nc_id    = ndma_mc_pair_to_nc( dev_mc, dev_mc);
	dev_addr = ndma_mc_to_pa( dev_mc) + dev_offset;   // range has been checked by the caller

	return ndma_memcpy_zero_copy(nd, nc_id, host_addr, dev_addr, size, direction);
}
