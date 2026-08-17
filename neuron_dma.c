// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/bitops.h>
#include <linux/hashtable.h>
#include <linux/kref.h>


#include "udma/udma.h"
#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"
#include "neuron_mmap.h"
#include "neuron_dhal.h"
#include "neuron_pci.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_dma_wait);
#endif

int zerocopy_trn1_override = 0;
module_param(zerocopy_trn1_override, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(zerocopy_trn1_override, "override zerocopy for trn1");

//#define NUNUSED	__attribute__ ((unused))

struct neuron_device;

/* data structures for explicit pin/unpin API */

/**
 * struct neuron_pinned_mem - Tracks a pre-pinned host memory region
 * @va: User virtual address that was pinned (lookup key)
 * @size: Size of the pinned region in bytes
 * @nr_pages: Number of pages pinned
 * @pages: Array of pinned page pointers
 * @rb_node: Red-black tree node for efficient VA lookup
 *
 * Process isolation is structural: each process has its own rbtree
 * in the global hash table, so no pid field is needed here.
 */
struct neuron_pinned_mem {
	u64 va;                    /* lookup key - user virtual address */
	u64 size;
	unsigned long nr_pages;
	struct page **pages;
	struct rb_node rb_node;    /* for VA-based lookup */
};

/* Per-process pinned memory state */
struct neuron_pinned_mem_process {
	pid_t pid;
	struct rb_root root;          /* rbtree of pinned regions for this process */
	struct mutex lock;            /* protects this process's rbtree */
	struct kref refcount;         /* lifetime management; freed when last ref drops */
	struct hlist_node hash_node;  /* for hash table lookup */
};


static void ndma_ack_completed_desc(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);
}

u32 ndma_mc_pair_to_nc(struct mem_chunk *src_mc, struct mem_chunk *dst_mc)
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
dma_addr_t ndma_mc_to_pa(struct mem_chunk *mc)
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
		pr_info_once("allocating descriptor for non-h2t\n");
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
int ndma_memcpy_add_completion_desc( struct ndma_eng *eng, struct ndma_ring *ring, void * completion_buffer, int barrier_type)
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
					DMA_COMPLETION_MARKER_SIZE, barrier_type, false);
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
	// Increase the wait time on virtual platforms
	if (narch_is_qemu() || narch_is_emu()) {
		wait = wait * 100 * 1000;
	}
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
		ret = -ETIMEDOUT;
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
		
		//TODO trace_dma_memcpy(nd, nc_id, src_offset, dst_offset, chunk_size, pending_transfers);
	}

	// write completion descriptor, kick off DMAs, record pending xfers and data outstanding and prefetch if requested
	//
	ret = ndma_memcpy_add_completion_desc( eng, ring, dma_ctx->completion_ptr, UDMA_M2M_BARRIER_NONE);
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
	const int qid = ndhal->ndhal_ndmar.ndmar_get_h2t_def_qid(nc_id);

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
	mutex_lock(&ring->h2t_ring_lock);

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

	if (dma_ctx->completion_ptr == NULL) {
		ret = -ENOMEM;
		goto fail;
	}

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
	
	mutex_unlock(&ring->h2t_ring_lock);
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

	// TODO: H2H memcpy's src and dst mc should have dedicated nc_id such as -1
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
	const int qid            = ndhal->ndhal_ndmar.ndmar_get_h2t_def_qid(nc_id);
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
		for (qid = 0; qid < ndhal->ndhal_udma.num_queues; qid++) {
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
 */

/* Context for tracking a single tensor batch operation on submit flow */
struct ndma_h2t_zcdma_op_context {
	void       *host_addr;
	dma_addr_t  dev_addr;
	u64         offset;
	u64         pin_size;
	u64         remaining;
};

/* DMA context state */
enum ndma_zcdma_state {
	NDMA_INVALID = 0,
	NDMA_UNPINNED,
	NDMA_PINNED_UNSUBMITTED,
	NDMA_SUBMITTED,
	NDMA_COMPLETED, // completed transfer waiting to be harvested from ctx queue to CQ
};

/* DMA context */
struct ndma_h2t_zcdma_context {
	struct ndma_eng  *eng;                // engine
	struct ndma_ring *ring;               // ring

	// Submission-related
	void                 *host_addr;          // host address
	dma_addr_t            dev_addr;           // device address
	u64                   size;               // size for this transfer
	bool                  direction;          // direction. true = to-device/write/copy-in
	bool                  last;               // last transfer for the entire request.
	u64                   start_time;         // start time for this transfer
	int                   nr_pages;           // number of pages for this transfer
	int                   nr_desc;            // number of descriptors which is equal to pending transfers -1
	struct page         **page_list;          // page structures tracking our pinned pages;
											  // managed by page_list_pool in ctx queue
	enum ndma_zcdma_state state;              // state of this transfer
	struct neuron_pinned_mem_process *prepin_proc;	// ref counted ptr to per process store of pinned memories
										      //  when set indicates that 1/ the context uses pre-pinned mem and it should not be unpinned
											  //  2/ prevents process exit cleanup from unpinning the memory while used by the context
	pid_t				pid;				  // PID of the process that initiated the copy

	// Completion-related
	void                 *completion_ptr;     // completion buffer pointer;
											  // host memory buffer which driver polls on for completions;
											  // managed by completion_pool in ctx queue
	u64                   sequence_num;       // async sequence number; 0 for sync transfers
	void                 *context;            // async completion context

	// Async-only
	struct mm_struct     *mm;                 // mm that owns the user buffers
	s64                   compl_ret;          // result for in-order CQ completion of completed work. Only valid for async.
											  // used only for async BAR4 writes and temporary fake-async D2D copy
};

static void ndma_pinned_mem_process_release(struct kref *kref);

static void ndma_zc_release_ctx(struct ndma_h2t_zcdma_context *ctx, u64 *nr_pinned_pages)
{
	// do not free or set completion_ptr null. it is managed by completion_pool in ctx queue
	// do not free or set page_list null. it is managed by page_list_pool in ctx queue

	if (ctx->state >= NDMA_PINNED_UNSUBMITTED && ctx->nr_pages > 0) {
		/* Only unpin if we pinned it ourselves (not pre-pinned memory) */
		if (!ctx->prepin_proc) {
			if (ctx->direction) {
				unpin_user_pages(ctx->page_list, ctx->nr_pages);
			} else {
				unpin_user_pages_dirty_lock(ctx->page_list, ctx->nr_pages, true);
			}
		} else {
			kref_put(&ctx->prepin_proc->refcount, ndma_pinned_mem_process_release);
		}

		*nr_pinned_pages -= ctx->nr_pages;
	}
	ctx->nr_pages = 0;
	ctx->prepin_proc = NULL;

	if (ctx->mm) {
		mmput(ctx->mm);
		ctx->mm = NULL;
	}

	ctx->state = NDMA_INVALID;
	ctx->sequence_num = 0;
	ctx->context = NULL;
	ctx->compl_ret = 0;
}

/* H2D DMA Completion Queue (CQ) */
#define NDMA_H2D_COMPL_QUEUE_CAPACITY 1024
int ndma_h2d_compl_queue_init(struct neuron_device *nd, struct ndma_h2d_compl_queue *compl_queue)
{
	int ret = 0;
	size_t queue_size = 0;
	struct mem_chunk *mc = NULL;
	neuron_h2d_dma_compl_queue_t *compl_queue_shared = NULL;

	queue_size = sizeof(neuron_h2d_dma_compl_queue_t) + (NDMA_H2D_COMPL_QUEUE_CAPACITY * sizeof(neuron_h2d_dma_compl_queue_entry_t));
	ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, queue_size, 0,
						 MEM_LOC_HOST, 0, 0,
						 NEURON_MEMALLOC_TYPE_NCDEV_HOST, &mc);
	if (ret) {
		pr_err("failed to allocate h2d dma completion queue mc: %d\n", ret);
		return ret;
	}
	ret = nmch_handle_alloc(nd, mc, &mc->mc_handle);
	if (ret) {
		pr_err("failed to allocate mc handle for h2d dma completion queue: %d\n", ret);
		mc_free(&mc);
		return ret;
	}
	memset(mc->va, 0, queue_size);

	compl_queue_shared = (neuron_h2d_dma_compl_queue_t *)mc->va;
	compl_queue_shared->capacity = NDMA_H2D_COMPL_QUEUE_CAPACITY;
	compl_queue_shared->head = 0;
	compl_queue_shared->tail = 0;

	compl_queue->mc = mc;
	compl_queue->compl_queue_shared = compl_queue_shared;
	compl_queue->capacity_mask = NDMA_H2D_COMPL_QUEUE_CAPACITY - 1;
	compl_queue->tail = 0;

	return 0;
}

void ndma_h2d_compl_queue_destroy(struct ndma_h2d_compl_queue *compl_queue)
{
	if (compl_queue->mc) {
		mc_free(&compl_queue->mc);
	}
	compl_queue->mc = NULL;
	compl_queue->compl_queue_shared = NULL;
	compl_queue->capacity_mask = 0;
	compl_queue->tail = 0;
}

/*
 * Emit the CQE for an accepted async io request. Caller must hold the ring
 * lock that serializes compl_queue->tail. Submission failures return an error
 * to userspace and must not call this function to emit a CQE.
 */
static void ndma_h2d_compl_queue_put(struct ndma_h2d_compl_queue *compl_queue,
							  u64 sequence_num,
							  s64 compl_ret,
							  void *context)
{
	u32 head = 0;
	u32 tail = 0;
	neuron_h2d_dma_compl_queue_t *compl_queue_shared = compl_queue->compl_queue_shared;
	neuron_h2d_dma_compl_queue_entry_t *entry = NULL;

	head = smp_load_acquire(&compl_queue_shared->head);
	tail = compl_queue->tail;

	while ((tail - head) >= (compl_queue->capacity_mask + 1)) {
		pr_warn_once("h2d dma completion queue full; blocking until space is available\n");
		msleep(1);
		head = smp_load_acquire(&compl_queue_shared->head);
		tail = compl_queue->tail;
	}

	entry = &compl_queue_shared->entries[tail & compl_queue->capacity_mask];

	/* Write completion result to tail */
	entry->compl_ret = compl_ret;
	entry->context = context;
	entry->sequence_num = sequence_num;

	/* Move tail */
	compl_queue->tail = tail + 1;
	smp_store_release(&compl_queue_shared->tail, compl_queue->tail);
}

#define NDMA_ZC_PAGES_PER_XFER  64        // number of pages in each zero copy dma transfer.  This is somewhat, but not
										  // totally arbitrary.  We don't want to pin a lot of pages. We just want to
										  // pin enough where (approximately):  
										  //       dma time > (pin time + setup time + completion update + initial poll wait)
										  // That's the simple explanation. It's a tad more complicated in trading off smaller
										  // transfers where even if that equation doesn't hold, the overlap can be beneficial.
										  // Right now the sweet spot looks to be ~ 64 pages.  More tuning is required.
#define NDMA_ZC_MIN_PAGES_PER_XFER 64

/* Hysteresis thresholds for descriptor wait checks in submission flow. */
#define NDMA_ZC_DESC_WAIT_THRESHOLD_LO (NDMA_ZC_PAGES_PER_XFER    + 1)
#define NDMA_ZC_DESC_WAIT_THRESHOLD_HI (NDMA_ZC_DESC_WAIT_THRESHOLD_LO * 8)

/* DMA ctx queue constants */
#define NDMA_CTX_QUEUE_DEFAULT_CAPACITY   1024
#define NDMA_CTX_QUEUE_MAX_PINNED_PAGES   524288

/* Skip tombstone ctxs */
static void ndma_ctx_queue_advance_to_valid(struct ndma_ctx_queue *queue, u32 *idx, u32 stop)
{
	while (*idx != stop && queue->entries[*idx].state == NDMA_INVALID) {
		*idx = (*idx + 1) & queue->capacity_mask;
	}
}

/* Check empty or full */
static bool ndma_ctx_queue_is_empty(const struct ndma_ctx_queue *queue)
{
	return queue->head == queue->tail;
}

static bool ndma_ctx_queue_is_full(const struct ndma_ctx_queue *queue)
{
	return queue->head == ((queue->tail + 1) & queue->capacity_mask);
}

static bool ndma_ctx_queue_submitted_empty(const struct ndma_ctx_queue *queue)
{
	return queue->head == queue->first_pinned_unsubmitted;
}

static bool ndma_ctx_queue_pinned_unsubmitted_empty(const struct ndma_ctx_queue *queue)
{
	return queue->first_pinned_unsubmitted == queue->first_unpinned;
}

static bool ndma_ctx_queue_unpinned_empty(const struct ndma_ctx_queue *queue)
{
	return queue->first_unpinned == queue->tail;
}

/* Increment to next index */
static void ndma_ctx_queue_inc_first_pinned_unsubmitted(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_pinned_unsubmitted_empty(queue)) {
		return;
	}
	queue->first_pinned_unsubmitted = (queue->first_pinned_unsubmitted + 1) & queue->capacity_mask;
	ndma_ctx_queue_advance_to_valid(queue, &queue->first_pinned_unsubmitted, queue->first_unpinned);
}

static void ndma_ctx_queue_inc_first_unpinned(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_unpinned_empty(queue)) {
		return;
	}
	queue->first_unpinned = (queue->first_unpinned + 1) & queue->capacity_mask;
	ndma_ctx_queue_advance_to_valid(queue, &queue->first_unpinned, queue->tail);
}

static void ndma_ctx_queue_inc_tail(struct ndma_ctx_queue *queue)
{
	u32 old_tail = queue->tail;
	u32 new_tail = (old_tail + 1) & queue->capacity_mask;

	// Assume the ctx at old tail is already filled by caller
	// Tail advance may also initialize/advance the pinned+unsubmitted and unpinned pointers
	struct ndma_h2t_zcdma_context *ctx = &queue->entries[old_tail];
	if (ctx->state == NDMA_PINNED_UNSUBMITTED || ctx->state == NDMA_COMPLETED) {
		if (ndma_ctx_queue_pinned_unsubmitted_empty(queue)) {
			// The first pinned+unsubmitted pointer appears at old_tail
			queue->first_pinned_unsubmitted = old_tail;
		}
		if (ndma_ctx_queue_unpinned_empty(queue)) {
			// No unpinned elements yet; start after the new tail
			queue->first_unpinned = new_tail;
		}
	} else if (ctx->state == NDMA_UNPINNED) {
		if (ndma_ctx_queue_unpinned_empty(queue)) {
			// The first unpinned pointer appears at old_tail
			queue->first_unpinned = old_tail;
		}
	}

	// Move tail forward after updating the two pointers
	queue->tail = new_tail;
}

/* Peek */
static struct ndma_h2t_zcdma_context *ndma_ctx_queue_peek_tail(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_is_full(queue)) {
		return NULL;
	}
	return &queue->entries[queue->tail];
}

static struct ndma_h2t_zcdma_context *ndma_ctx_queue_peek_pinned_unsubmitted(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_pinned_unsubmitted_empty(queue)) {
		return NULL;
	}
	return &queue->entries[queue->first_pinned_unsubmitted];
}

static struct ndma_h2t_zcdma_context *ndma_ctx_queue_peek_first_unpinned(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_unpinned_empty(queue)) {
		return NULL;
	}
	return &queue->entries[queue->first_unpinned];
}

/* Pop */
static struct ndma_h2t_zcdma_context *ndma_ctx_queue_pop_head(struct ndma_ctx_queue *queue)
{
	u32 old_head;
	struct ndma_h2t_zcdma_context *ctx = NULL;

	if (ndma_ctx_queue_is_empty(queue)) {
		return NULL;
	}

	old_head = queue->head;
	ctx = &queue->entries[old_head];
	queue->head = (queue->head + 1) & queue->capacity_mask;
	ndma_ctx_queue_advance_to_valid(queue, &queue->head, queue->tail);

	if (ndma_ctx_queue_is_empty(queue)) {
		queue->first_pinned_unsubmitted = queue->tail;
		queue->first_unpinned = queue->tail;
	} else {
		if (old_head == queue->first_pinned_unsubmitted) {
			ndma_ctx_queue_inc_first_pinned_unsubmitted(queue);
		}
		if (old_head == queue->first_unpinned) {
			ndma_ctx_queue_inc_first_unpinned(queue);
		}
	}

	return ctx;
}

static struct ndma_h2t_zcdma_context *ndma_ctx_queue_pop_submitted(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_submitted_empty(queue)) {
		return NULL;
	}

	return ndma_ctx_queue_pop_head(queue);
}

/* Failure-path helper.
 * Given a sequence number of a async request, wait for any matching submitted ctxs, then reset all matching ctxs.
 * This prevents further remote pinning and submitting on a failed async request.
 * Once a wait times out, skip waiting on subsequent contexts (engine is stuck).
 */
static void ndma_ctx_queue_drain_sequence(struct ndma_ctx_queue *queue, u64 sequence_num)
{
	u32 idx;
	bool timed_out = false;

	for (idx = queue->head; idx != queue->tail; idx = (idx + 1) & queue->capacity_mask) {
		struct ndma_h2t_zcdma_context *ctx = &queue->entries[idx];

		if (ctx->sequence_num == sequence_num) {
			// wait for already submitted DMAs to complete, unless engine already timed out.
			if (ctx->state == NDMA_SUBMITTED && !timed_out) {
				if (ndma_memcpy_wait_for_completion(ctx->eng, ctx->ring, ctx->nr_desc + 1, ctx->completion_ptr, false, false))
					timed_out = true;
			}

			// release pinned pages and mm, and set state to invalid (tombstone).
			ndma_zc_release_ctx(ctx, &queue->nr_pinned_pages);
		}
	}

	// After draining, advance the pointers to skip the invalidated ctxs.
	ndma_ctx_queue_advance_to_valid(queue, &queue->first_unpinned, queue->tail);
	ndma_ctx_queue_advance_to_valid(queue, &queue->first_pinned_unsubmitted, queue->first_unpinned);
	ndma_ctx_queue_advance_to_valid(queue, &queue->head, queue->tail);
}

/* Failure-path helper.
 * Wait for submitted contexts from head up to (but not including) first_pinned_unsubmitted.
 * Unpin from head up to (but not including) first_unpinned.
 * Once a wait times out, skip waiting on subsequent contexts (engine is stuck).
 */
static void ndma_ctx_queue_drain(struct ndma_eng *eng,
								 struct ndma_ring *ring,
								 struct ndma_ctx_queue *queue)
{
	bool timed_out = false;

	while (!ndma_ctx_queue_is_empty(queue)) {
		struct ndma_h2t_zcdma_context *ctx = ndma_ctx_queue_pop_head(queue);

		if (ctx->state == NDMA_SUBMITTED && !timed_out) {
			if (ndma_memcpy_wait_for_completion(eng, ring, ctx->nr_desc + 1, ctx->completion_ptr, false, false))
				timed_out = true;
		}

		ndma_zc_release_ctx(ctx, &queue->nr_pinned_pages);
	}
}

/* Init and destroy queue */
int ndma_ctx_queue_init(struct ndma_ctx_queue *queue)
{
	int i;

	if (!queue) {
		pr_err("ctx queue pointer cannot be NULL\n");
		return -EINVAL;
	}

	memset(queue, 0, sizeof(*queue));

	u32 capacity = NDMA_CTX_QUEUE_DEFAULT_CAPACITY;
	if (!is_power_of_2(capacity)) {
		pr_err("ctx queue capacity must be power of two\n");
		return -EINVAL;
	}
	queue->capacity_mask = capacity - 1;
	queue->head = 0;
	queue->tail = 0;
	queue->first_pinned_unsubmitted = 0;
	queue->first_unpinned = 0;

	queue->entries = kvcalloc(capacity, sizeof(*queue->entries), GFP_KERNEL);
	if (!queue->entries) {
		pr_err("failed to allocate ctx queue entries\n");
		return -ENOMEM;
	}

	// allocate completion ptrs in one contiguous array at once,
	// and let queue->entries[i].completion_ptr point to each completion buffer
	queue->completion_pool = kcalloc(capacity, DMA_COMPLETION_MARKER_SIZE * 2, GFP_KERNEL);
	if (!queue->completion_pool) {
		pr_err("failed to allocate ctx queue completion pool\n");
		goto err;
	}

	// allocate page_list arrays in one contiguous pool, and let each entry point to its slice
	queue->page_list_pool = kvcalloc(capacity * NDMA_ZC_PAGES_PER_XFER, sizeof(struct page *), GFP_KERNEL);
	if (!queue->page_list_pool) {
		pr_err("failed to allocate ctx queue page_list pool\n");
		goto err;
	}

	for (i = 0; i < capacity; i++) {
		queue->entries[i].completion_ptr =
			(u8 *)queue->completion_pool + i * DMA_COMPLETION_MARKER_SIZE * 2;
		queue->entries[i].page_list =
			(struct page **)queue->page_list_pool + i * NDMA_ZC_PAGES_PER_XFER;
	}

	return 0;

err:
	if (queue->completion_pool) {
		kfree(queue->completion_pool);
		queue->completion_pool = NULL;
	}
	if (queue->page_list_pool) {
		kvfree(queue->page_list_pool);
		queue->page_list_pool = NULL;
	}
	if (queue->entries) {
		kvfree(queue->entries);
		queue->entries = NULL;
	}
	return -ENOMEM;
}

void ndma_ctx_queue_free(struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_ctx_queue *queue)
{
	int bit = ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(eng->eng_id, ring->qid);

	atomic64_andnot(BIT_ULL(bit), &eng->nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);

	if (!queue) {
		return;
	}
	if (queue->entries) {
		ndma_ctx_queue_drain(eng, ring, queue);
		kvfree(queue->entries);
		queue->entries = NULL;
	}
	if (queue->completion_pool) {
		kfree(queue->completion_pool);
		queue->completion_pool = NULL;
	}
	if (queue->page_list_pool) {
		kvfree(queue->page_list_pool);
		queue->page_list_pool = NULL;
	}
	memset(queue, 0, sizeof(*queue));
}

/** ndma_calc_zc_pin_size()
 *
 *   determine how many pages to pin per step for zercopy dma pipelining.
 */
static size_t ndma_calc_zc_pin_size(size_t size)
{
	if (size > NDMA_ZC_PAGES_PER_XFER * PAGE_SIZE * 2) {
		return NDMA_ZC_PAGES_PER_XFER * PAGE_SIZE;
	} else if (size <= NDMA_ZC_MIN_PAGES_PER_XFER * PAGE_SIZE) {
		return  size;
	}
	return (size/2 + PAGE_SIZE-1) & ~(PAGE_SIZE-1);
}

/**
 * ndma_zerocopy_supported()
 *
 *   zero copy is not support for platforms that require retry
 *   
 */
bool ndma_zerocopy_supported(void)
{
	return !ndhal->ndhal_ndma.ndma_retry_memcpy || zerocopy_trn1_override;
}

/**
 * ndma_build_n_issue_zc_descs() 
 *
 *   build descriptors for a zero copy operation consisting of non-continuous
 *   physical host memory pages.  
 *
 *   explain how alignment is handled.
 *
 *   TODO:
 *     go i=0 to nr_pages
 *     Think about using some permanent location in HBM as source for completion descriptor update.  Like
 *     why are we reading across the PCIe bus to fetch completion data.
 */
static int ndma_build_n_issue_zc_descs(struct ndma_h2t_zcdma_context * dma_ctx)
{
	int            ret;
	unsigned long  offset        = (unsigned long)(dma_ctx->host_addr) & (PAGE_SIZE-1);
	dma_addr_t     dev_addr      = dma_ctx->dev_addr;
	dma_addr_t     pci_host_base = ndhal->ndhal_address_map.pci_host_base;
	u64            remaining     = dma_ctx->size;
	int            i = 0;
	u64            chunk_size;
	int            pending_transfers = 0;
	int            barrier_type;

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
			tmp = next;
		}

		if (dma_ctx->direction) { // write to device
			src_addr = (contig_start + offset) | pci_host_base;
			dst_addr = dev_addr;
		} else {                 // read from device
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
			// on the read path completion write follows data writes in order, that means when the completion write finishes
			// it's guaranteed that all the data has been written, no need for a barrier

			// on the write path we only need the barrier for the last transfer (the last set of pinned pages), why?
			// HBM writes (data) and host write (completion) take different path through data fabric.  That means w/o a barrier
			// it's possible for the completion to be written before the data.

			// We don't need the barrier to ensure it's safe to unpin.  
			// s2m descriptors are executed in order, that means when s2m completion write is executed all s2m data writes 
			// have been executed as well, that means all m2s data reads have been executed, that means it's safe to unpin

			// use WRITE_BARRIER on V2 (set on the last data descriptor)
			// use SOW on V3+ (set on completion descriptor below)
			if (narch_get_arch() == NEURON_ARCH_V2)
				barrier_type = (remaining == chunk_size && dma_ctx->direction && dma_ctx->last) ? UDMA_M2M_BARRIER_WRITE_BARRIER : UDMA_M2M_BARRIER_NONE;
			else
				barrier_type = UDMA_M2M_BARRIER_NONE;

			ret = udma_m2m_copy_prepare_one(&dma_ctx->eng->udma, dma_ctx->ring->qid, src_addr, dst_addr, chunk_size,  barrier_type, false);
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

	if (narch_get_arch() != NEURON_ARCH_V2)
		barrier_type = (dma_ctx->direction && dma_ctx->last) ? UDMA_M2M_BARRIER_SOW: UDMA_M2M_BARRIER_NONE;
	else
		barrier_type = UDMA_M2M_BARRIER_NONE;
	ret = ndma_memcpy_add_completion_desc( dma_ctx->eng, dma_ctx->ring, dma_ctx->completion_ptr, barrier_type);
	if (ret) {
		goto error;
	}

	pending_transfers++;

	ret = udma_m2m_copy_start(&dma_ctx->eng->udma, dma_ctx->ring->qid, pending_transfers, pending_transfers);
	if (ret) {
		pr_info("copy start failed %d\n", ret);
	}
	dma_ctx->state = NDMA_SUBMITTED;

error:
	return ret;
}

/* Return the number of descriptors available (TX-only; TX/RX counts match) */
static u32 ndma_zc_descs_available(struct ndma_eng *eng, u32 qid)
{
	struct udma_q *txq;
	u32 tx_desc_available;

	udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);

	tx_desc_available = udma_available_get(txq);

	/* TX/RX descriptor availability is kept in lock-step. */
	return tx_desc_available;
}

/* Estimate if a zero-copy DMA context fits in the available descriptors. */
static bool _ndma_zc_descs_available(struct ndma_eng *eng, u32 qid, u32 threshold)
{
	u32 max_descs_required = threshold + 1; /* +1 for completion descriptor */

	return ndma_zc_descs_available(eng, qid) >= max_descs_required;
}

/* Whether we should wait for some completions before submitting more in the next iteration */
static bool ndma_zc_should_wait(struct ndma_eng *eng,
								struct ndma_ring *ring,
								struct ndma_ctx_queue *ctx_queue,
								u32 *desc_threshold)
{
	bool pinned_at_max;
	bool desc_ring_full;
	bool ctx_queue_full;

	pinned_at_max = ctx_queue->nr_pinned_pages >= NDMA_CTX_QUEUE_MAX_PINNED_PAGES;
	ctx_queue_full = ndma_ctx_queue_is_full(ctx_queue);
	desc_ring_full = !_ndma_zc_descs_available(eng, ring->qid, *desc_threshold);

	if (pinned_at_max || desc_ring_full || ctx_queue_full) {
		*desc_threshold = NDMA_ZC_DESC_WAIT_THRESHOLD_HI;
		return true;
	}

	return false;
}

static bool ndma_pinned_mem_try_populate(pid_t pid, u64 va, u64 size, struct page **page_list, int nr_pages, struct neuron_pinned_mem_process **prepin_proc);

static int ndma_zerocopy_pin_pages(int nd_id,
								   u32 nc_id,
								   struct ndma_ctx_queue *ctx_queue,
								   struct ndma_h2t_zcdma_context *dma_ctx,
								   bool use_remote_pin)
{
	int nr_pinned = 0;
	struct neuron_pinned_mem_process *prepin_proc = NULL;

	/* Check if this VA range is in pre-pinned memory */
	if (ndma_pinned_mem_try_populate(dma_ctx->pid, (u64)dma_ctx->host_addr, dma_ctx->size,
					 dma_ctx->page_list, dma_ctx->nr_pages, &prepin_proc)) {
		dma_ctx->prepin_proc = prepin_proc;
		ctx_queue->nr_pinned_pages += dma_ctx->nr_pages;
		dma_ctx->state = NDMA_PINNED_UNSUBMITTED;
		return 0;
	}

	if (use_remote_pin) {
		if (!dma_ctx->mm) {
			pr_err("remote pin requested without mm context\n");
			return -EINVAL;
		}
#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)))
		nr_pinned = pin_user_pages_remote(dma_ctx->mm,
										(unsigned long)dma_ctx->host_addr & PAGE_MASK,
										dma_ctx->nr_pages,
										dma_ctx->direction ? 0 : FOLL_WRITE,
										dma_ctx->page_list,
										NULL);
#else
		nr_pinned = pin_user_pages_remote(dma_ctx->mm,
										(unsigned long)dma_ctx->host_addr & PAGE_MASK,
										dma_ctx->nr_pages,
										dma_ctx->direction ? 0 : FOLL_WRITE,
										dma_ctx->page_list,
										NULL,
										NULL);
#endif
		mmput(dma_ctx->mm);
		dma_ctx->mm = NULL;
	} else {
		nr_pinned = pin_user_pages_fast((unsigned long)dma_ctx->host_addr & PAGE_MASK, dma_ctx->nr_pages,
							dma_ctx->direction ? 0 : FOLL_WRITE, dma_ctx->page_list);

		if (nr_pinned != dma_ctx->nr_pages) {
			// if failed pin_fast because of page fault, do the regular pinning
			if (nr_pinned > 0) {
				unpin_user_pages(dma_ctx->page_list, nr_pinned);
			}

#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)))
			nr_pinned = pin_user_pages((unsigned long)dma_ctx->host_addr & PAGE_MASK, dma_ctx->nr_pages, dma_ctx->direction ? 0 : FOLL_WRITE, dma_ctx->page_list);
#else
			nr_pinned = pin_user_pages((unsigned long)dma_ctx->host_addr & PAGE_MASK, dma_ctx->nr_pages, dma_ctx->direction ? 0 : FOLL_WRITE, dma_ctx->page_list, NULL);
#endif
		}
	}

	if (nr_pinned != dma_ctx->nr_pages) {
		int err = (nr_pinned < 0) ? nr_pinned : -ENOMEM;

		pr_err("could not pin host pages for zero copy dma on nd %d: nr_pinned %d\n", nd_id, nr_pinned);

		if (nr_pinned > 0) {
			unpin_user_pages(dma_ctx->page_list, nr_pinned);
		}

		return err;
	}

	ctx_queue->nr_pinned_pages += dma_ctx->nr_pages;
	dma_ctx->state = NDMA_PINNED_UNSUBMITTED;

	return 0;
}

int ndma_zerocopy_submit(struct neuron_device *nd,
			 u32 nc_id,
			 const nrt_tensor_batch_op_t *ops,
			 u32 num_ops,
			 dma_addr_t dev_base,
			 int qid,
			 bool direction,
			 u64 sequence_num,
			 void *context)
{
	int ret = 0;
	int i = 0;
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_eng *eng = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring = &queue->ring_info;
	struct ndma_ctx_queue *ctx_queue = &ring->dma_ctx_queue;
	struct ndma_h2t_zcdma_context *cur_ctx = NULL;
	struct ndma_h2t_zcdma_op_context op_ctx;
	bool async = (sequence_num != 0);

	/* Verify ring ownership. */
	if (!ndmar_h2t_ring_is_owner(ring, nc_id)) {
		pr_err("nd%02d: attempting to use qid %d that was not assigned to nc %d\n",
		       nd->device_index, qid, nc_id);
		return -ENOENT;
	}

	if (async) {
		ret = ndma_h2d_create_cmpltn_thread(nd);
		if (ret) {
			return ret;
		}
	}

	mutex_lock(&ring->h2t_ring_lock);

	for (i = 0; i < num_ops; i++) {
		const nrt_tensor_batch_op_t *op = &ops[i];
		op_ctx.host_addr = op->buffer;
		op_ctx.dev_addr = dev_base + op->offset;
		op_ctx.offset = (unsigned long)op_ctx.host_addr & (PAGE_SIZE - 1);
		op_ctx.remaining = op->size;
		/* pin_size is in page units; include the page offset. */
		op_ctx.pin_size = ndma_calc_zc_pin_size(op_ctx.remaining + op_ctx.offset);

		while (op_ctx.remaining) {
			int nr_pages;
			bool can_pin;
			bool ctx_queue_full;

			/* Step 1: submit any pinned contexts that have available descriptors. */
			while (true) {
				struct ndma_h2t_zcdma_context *unsubmitted_ctx = ndma_ctx_queue_peek_pinned_unsubmitted(ctx_queue);

				if (!unsubmitted_ctx || !_ndma_zc_descs_available(eng, ring->qid, unsubmitted_ctx->nr_pages)) {
					break;
				}

				ret = ndma_build_n_issue_zc_descs(unsubmitted_ctx);
				if (ret) {
					pr_err("failed to build and issue zero-copy descs\n");
					goto done;
				}

				ndma_ctx_queue_inc_first_pinned_unsubmitted(ctx_queue);
			}

			/* Step 2: set up the current ctx if there is room and pinned-page budget. */
			nr_pages = DIV_ROUND_UP(op_ctx.pin_size, PAGE_SIZE);
			can_pin = (ctx_queue->nr_pinned_pages + nr_pages <= NDMA_CTX_QUEUE_MAX_PINNED_PAGES);
			ctx_queue_full = ndma_ctx_queue_is_full(ctx_queue);

			if (async && ctx_queue_full) {
				pr_err("ctx queue full. failed to submit async ctx\n");
				ret = -EBUSY;
				goto done;
			}

			if ((can_pin || async) && !ctx_queue_full) {
				cur_ctx                 = ndma_ctx_queue_peek_tail(ctx_queue);
				cur_ctx->eng            = eng;
				cur_ctx->ring           = ring;
				cur_ctx->host_addr      = op_ctx.host_addr;
				cur_ctx->dev_addr       = op_ctx.dev_addr;
				// First chunk may be unaligned; later chunks are page-aligned with offset=0.
				cur_ctx->size           = op_ctx.pin_size - op_ctx.offset;
				cur_ctx->direction      = direction;
				cur_ctx->last           = (cur_ctx->size == op_ctx.remaining && i == num_ops - 1);
				cur_ctx->nr_pages       = nr_pages;
				cur_ctx->state          = NDMA_UNPINNED;
				cur_ctx->nr_desc        = 0; // Set by ndma_build_n_issue_zc_descs().
				cur_ctx->mm             = NULL;
				cur_ctx->pid		    = task_tgid_nr(current);
				cur_ctx->sequence_num   = sequence_num;
				cur_ctx->context        = context;

				/* Pin now if possible; otherwise capture mm for remote pinning (async only). */
				if (can_pin) {
					ret = ndma_zerocopy_pin_pages(nd->device_index, nc_id, ctx_queue, cur_ctx, false);
					if (ret) {
						pr_err("failed to pin pages for zero copy dma on nd %d\n", nd->device_index);
						goto done;
					}
				} else if (async) {
					struct mm_struct *mm = current->mm;
					mmget(mm);
					cur_ctx->mm = mm;
				}

				/* Advance the queue tail.
				 * May also initialize/advance the pinned+unsubmitted and unpinned pointers.
				 */
				ndma_ctx_queue_inc_tail(ctx_queue);

				/* Update loop variables for the next chunk. */
				op_ctx.remaining -= cur_ctx->size;
				op_ctx.host_addr += cur_ctx->size;
				op_ctx.dev_addr += cur_ctx->size;
				op_ctx.pin_size = (op_ctx.remaining < op_ctx.pin_size) ? op_ctx.remaining : op_ctx.pin_size;
				op_ctx.offset = 0;
				cur_ctx = NULL;
			}

			/* Step 3 (sync): wait for submitted transfers to complete from the head. */
			if (!async) {
				u32 desc_threshold = NDMA_ZC_DESC_WAIT_THRESHOLD_LO;

				while (ndma_zc_should_wait(eng, ring, ctx_queue, &desc_threshold)) {
					struct ndma_h2t_zcdma_context *submitted_ctx = ndma_ctx_queue_pop_submitted(ctx_queue);

					ret = ndma_memcpy_wait_for_completion(eng, ring, submitted_ctx->nr_desc + 1,
									      submitted_ctx->completion_ptr,
									      true, false);
					ndma_zc_release_ctx(submitted_ctx, &ctx_queue->nr_pinned_pages);
					if (ret) {
						pr_err("failed to wait for completion of zero copy dma\n");
						goto done;
					}
				}
			}
		}
	}

	if (!async) {
		/* Step 4 (sync): submit remaining pinned ctxs, then drain all submitted ctxs. */
		while (true) {
			struct ndma_h2t_zcdma_context *ctx_to_submit = ndma_ctx_queue_peek_pinned_unsubmitted(ctx_queue);

			if (ctx_to_submit && _ndma_zc_descs_available(eng, ring->qid, ctx_to_submit->nr_pages)) {
				ret = ndma_build_n_issue_zc_descs(ctx_to_submit);
				if (ret) {
					pr_err("failed to build and issue zero-copy descs\n");
					goto done;
				}
				ndma_ctx_queue_inc_first_pinned_unsubmitted(ctx_queue);
			}

			struct ndma_h2t_zcdma_context *ctx_to_wait = ndma_ctx_queue_pop_submitted(ctx_queue);
			if (ctx_to_wait) {
				ret = ndma_memcpy_wait_for_completion(eng, ring, ctx_to_wait->nr_desc + 1,
								      ctx_to_wait->completion_ptr,
								      true, false);
				ndma_zc_release_ctx(ctx_to_wait, &ctx_queue->nr_pinned_pages);
				if (ret) {
					pr_err("failed to wait for completion of zero copy dma\n");
					goto done;
				}
			}

			if (!ctx_to_submit && !ctx_to_wait) {
				break;
			}
		}
	}

done:
	if (ret) {
		ndma_ctx_queue_drain(eng, ring, ctx_queue);
	}

	mutex_unlock(&ring->h2t_ring_lock);

	if (!ret && async) {
		int bit = ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(eng_id, qid);
		atomic64_or(BIT_ULL(bit), &nd->dma_cmpltn_thread.nonempty_ctxq_bitmap); // set the bit for this queue
		wake_up(&nd->dma_cmpltn_thread.wait_queue);
	}

	return ret;
}

int ndma_zerocopy_submit_completed(struct neuron_device *nd, u32 nc_id, int qid,
				   u64 sequence_num, s64 compl_ret, void *context)
{
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_eng *eng = &nd->ndma_engine[eng_id];
	struct ndma_ring *ring = &eng->queues[qid].ring_info;
	struct ndma_ctx_queue *ctx_queue = &ring->dma_ctx_queue;
	struct ndma_h2t_zcdma_context *ctx;
	int ret;

	if (!ndmar_h2t_ring_is_owner(ring, nc_id)) {
		pr_err("nd%02d: attempting to use qid %d that was not assigned to nc %d\n",
		       nd->device_index, qid, nc_id);
		return -ENOENT;
	}

	ret = ndma_h2d_create_cmpltn_thread(nd);
	if (ret) {
		return ret;
	}

	mutex_lock(&ring->h2t_ring_lock);
	if (ndma_ctx_queue_is_full(ctx_queue)) {
		mutex_unlock(&ring->h2t_ring_lock);
		return -EBUSY;
	}

	ctx = ndma_ctx_queue_peek_tail(ctx_queue);
	ctx->eng = eng;
	ctx->ring = ring;
	ctx->direction = true;
	ctx->last = true;
	ctx->nr_pages = 0;
	ctx->nr_desc = 0;
	ctx->state = NDMA_COMPLETED;
	ctx->sequence_num = sequence_num;
	ctx->context = context;
	ctx->compl_ret = compl_ret;
	ndma_ctx_queue_inc_tail(ctx_queue);
	mutex_unlock(&ring->h2t_ring_lock);

	atomic64_or(BIT_ULL(ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(eng_id, qid)),
		    &nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);
	wake_up(&nd->dma_cmpltn_thread.wait_queue);
	return 0;
}

/* The completion flow for completion, remote pinning, and submission. Async IO only */
static int ndma_zerocopy_complete(struct neuron_device *nd,
								  struct ndma_eng *eng,
								  struct ndma_ring *ring,
								  u64 *nonempty_ctxq_bitmap_copy)
{
	int ret = 0;
	int err = 0;
	bool did_work = false;
	struct ndma_ctx_queue *ctx_queue = NULL;
	u32 desc_threshold = NDMA_ZC_DESC_WAIT_THRESHOLD_LO;

	if (!ring) {
		return -EINVAL;
	}

	ctx_queue = &ring->dma_ctx_queue;

	mutex_lock(&ring->h2t_ring_lock);

	/* 1) Wait for at least one submitted context to complete */
	while (true) {
		if (ndma_ctx_queue_submitted_empty(ctx_queue)) {
			break;
		}
		/*
		 * Async completion must always retire at least one submitted context.
		 * Only fall back to the wait-throttling heuristic after we have made
		 * some forward progress in this pass.
		 */
		if (did_work && !ndma_zc_should_wait(eng, ring, ctx_queue, &desc_threshold)) {
			break;
		}
		struct ndma_h2t_zcdma_context *submitted_ctx = ndma_ctx_queue_pop_submitted(ctx_queue);

		if (submitted_ctx->state == NDMA_COMPLETED) {
			// Dummy context: BAR4/fake-async copy completed before enqueue.
			ret = submitted_ctx->compl_ret;
		} else {
			ret = ndma_memcpy_wait_for_completion(eng, ring, submitted_ctx->nr_desc + 1,
							      submitted_ctx->completion_ptr, true, false);
		}
		if (ret) {
			err = ret;
			pr_err("async h2d dma completion failed for seq num %llu: %d\n", submitted_ctx->sequence_num, ret);
			ndma_h2d_compl_queue_put(&ring->dma_compl_queue, submitted_ctx->sequence_num, ret, submitted_ctx->context);
			ndma_ctx_queue_drain_sequence(ctx_queue, submitted_ctx->sequence_num);
		} else if (submitted_ctx->last) {
			ndma_h2d_compl_queue_put(&ring->dma_compl_queue, submitted_ctx->sequence_num, 0, submitted_ctx->context);
		}

		ndma_zc_release_ctx(submitted_ctx, &ctx_queue->nr_pinned_pages);
		did_work = true;
	}

	/* 2) Submit pinned but unsubmitted contexts */
	while (true) {
		struct ndma_h2t_zcdma_context *pinned_unsubmitted_ctx = ndma_ctx_queue_peek_pinned_unsubmitted(ctx_queue);

		if (!pinned_unsubmitted_ctx) {
			break;
		}
		if (pinned_unsubmitted_ctx->state == NDMA_COMPLETED) {
			// Dummy context has no DMA descriptors; preserve CQ order only.
			ndma_ctx_queue_inc_first_pinned_unsubmitted(ctx_queue);
			did_work = true;
			continue;
		}
		if (!_ndma_zc_descs_available(eng, ring->qid, pinned_unsubmitted_ctx->nr_pages)) {
			break;
		}

		ret = ndma_build_n_issue_zc_descs(pinned_unsubmitted_ctx);
		if (ret) {
			err = ret;
			pr_err("async h2d dma submission failed for seq num %llu: %d\n", pinned_unsubmitted_ctx->sequence_num, ret);
			ndma_h2d_compl_queue_put(&ring->dma_compl_queue, pinned_unsubmitted_ctx->sequence_num, ret, pinned_unsubmitted_ctx->context);
			ndma_ctx_queue_drain_sequence(ctx_queue, pinned_unsubmitted_ctx->sequence_num);
		} else {
			ndma_ctx_queue_inc_first_pinned_unsubmitted(ctx_queue);
		}
		did_work = true;
	}

	/* 3) Remote pin unpinned contexts */
	while (true) {
		struct ndma_h2t_zcdma_context *unpinned_ctx = ndma_ctx_queue_peek_first_unpinned(ctx_queue);

		if (!unpinned_ctx || ctx_queue->nr_pinned_pages + unpinned_ctx->nr_pages > NDMA_CTX_QUEUE_MAX_PINNED_PAGES) {
			break;
		}

		ret = ndma_zerocopy_pin_pages(nd->device_index, ring->h2t_nc_id, ctx_queue, unpinned_ctx, true);
		if (ret) {
			err = ret;
			pr_err("async h2d dma remote pinning failed for seq num %llu: %d\n", unpinned_ctx->sequence_num, ret);
			ndma_h2d_compl_queue_put(&ring->dma_compl_queue, unpinned_ctx->sequence_num, ret, unpinned_ctx->context);
			ndma_ctx_queue_drain_sequence(ctx_queue, unpinned_ctx->sequence_num);
		} else {
			ndma_ctx_queue_inc_first_unpinned(ctx_queue);
		}
		did_work = true;
	}

	mutex_unlock(&ring->h2t_ring_lock);

	if (ndma_ctx_queue_is_empty(ctx_queue)) {
		int bit = ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(eng->eng_id, ring->qid);
		*nonempty_ctxq_bitmap_copy &= ~BIT_ULL(bit);
	}

	return err;
}

static int ndma_h2d_cmpltn_thread_fn(void *arg)
{
	struct neuron_device *nd = (struct neuron_device *)arg;
	int ret = 0;

	while (!kthread_should_stop() && !nd->dma_cmpltn_thread.stop) {
		wait_event_interruptible(nd->dma_cmpltn_thread.wait_queue,
								 nd->dma_cmpltn_thread.stop || atomic64_read(&nd->dma_cmpltn_thread.nonempty_ctxq_bitmap) != 0);
		if (kthread_should_stop() || nd->dma_cmpltn_thread.stop) {
			break;
		}
		u64 bitmap = atomic64_xchg(&nd->dma_cmpltn_thread.nonempty_ctxq_bitmap, 0);

		while (bitmap) {
			int bit = __ffs64(bitmap);
			u32 eng_id;
			u32 qid;
			struct ndma_eng *eng;
			struct ndma_ring *ring;

			ndhal->ndhal_ndmar.ndmar_ctx_queue_from_bit(bit, &eng_id, &qid);

			eng = &nd->ndma_engine[eng_id];
			ring = &eng->queues[qid].ring_info;
			ret = ndma_zerocopy_complete(nd, eng, ring, &bitmap);
			if (ret) {
				pr_err("dma completion thread failed to process ctx queue for eng %d q %d: %d\n", eng_id, qid, ret);
			}
		}
	}

	return ret;
}

int ndma_h2d_create_cmpltn_thread(struct neuron_device *nd)
{
	int ret = 0;
	struct task_struct *thread;

	if (READ_ONCE(nd->dma_cmpltn_thread.thread)) {
		return 0;
	}

	mutex_lock(&nd->lock);

	if (nd->dma_cmpltn_thread.thread) {
		/* thread already created */
		goto out;
	}

	nd->dma_cmpltn_thread.stop = false;
	init_waitqueue_head(&nd->dma_cmpltn_thread.wait_queue);
	atomic64_set(&nd->dma_cmpltn_thread.nonempty_ctxq_bitmap, 0);
	thread = kthread_run(ndma_h2d_cmpltn_thread_fn, nd, "neuron dma cmpltn");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("h2d dma completion thread creation failed\n");
		goto out;
	}
	WRITE_ONCE(nd->dma_cmpltn_thread.thread, thread);

out:
	mutex_unlock(&nd->lock);
	return ret;
}

void ndma_h2d_stop_cmpltn_thread(struct neuron_device *nd)
{
	if (!nd->dma_cmpltn_thread.thread) {
		return;
	}
	if (IS_ERR(nd->dma_cmpltn_thread.thread)) {
		nd->dma_cmpltn_thread.thread = NULL;
		return;
	}

	nd->dma_cmpltn_thread.stop = true;
	wake_up(&nd->dma_cmpltn_thread.wait_queue);
	kthread_stop(nd->dma_cmpltn_thread.thread);
	nd->dma_cmpltn_thread.thread = NULL;
}

/*
 * Pre-pinned host memory implementation
 * Uses a global hash table keyed by PID, with each process having its own
 * rbtree of pinned memory regions keyed by VA.
 * Host memory is not device-specific — a process can pin via any device
 * and the zerocopy path on any device will find the pre-pinned region.
 */

/* 256 buckets: up to 16 devices × 16 processes per device */
static DEFINE_HASHTABLE(pinned_mem_htable, 8);
static DEFINE_MUTEX(pinned_mem_htable_lock); /* protects hash table add/remove/lookup only */

/*
 * Find or create per-process state and take a reference.
 * Caller must hold pinned_mem_htable_lock; caller owns the returned ref.
 */
static struct neuron_pinned_mem_process *ndma_pinned_mem_get_process_locked(pid_t pid)
{
	struct neuron_pinned_mem_process *proc;

	hash_for_each_possible(pinned_mem_htable, proc, hash_node, pid) {
		if (proc->pid == pid) {
			kref_get(&proc->refcount);
			return proc;
		}
	}

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);
	if (!proc)
		return NULL;
	proc->pid = pid;
	proc->root = RB_ROOT;
	mutex_init(&proc->lock);
	kref_init(&proc->refcount); /* hash table holds initial ref */
	hash_add(pinned_mem_htable, &proc->hash_node, pid);
	kref_get(&proc->refcount);  /* caller's operational ref */
	return proc;
}

/*
 * Find per-process state and take a reference.
 * Caller must hold pinned_mem_htable_lock; caller owns the returned ref.
 * Returns NULL if not found (no ref taken).
 */
static struct neuron_pinned_mem_process *ndma_pinned_mem_find_process_locked(pid_t pid)
{
	struct neuron_pinned_mem_process *proc;

	hash_for_each_possible(pinned_mem_htable, proc, hash_node, pid) {
		if (proc->pid == pid) {
			kref_get(&proc->refcount);
			return proc;
		}
	}
	return NULL;
}

static void ndma_pinned_mem_free_entry(struct neuron_pinned_mem *entry)
{
	if (entry->pages) {
		unpin_user_pages(entry->pages, entry->nr_pages);
		kvfree(entry->pages);
	}
	kfree(entry);
}

static void ndma_pinned_mem_destroy_tree(struct rb_root *root)
{
	struct rb_node *node;

	while ((node = rb_first(root)) != NULL) {
		struct neuron_pinned_mem *entry = rb_entry(node, struct neuron_pinned_mem, rb_node);
		rb_erase(node, root);
		ndma_pinned_mem_free_entry(entry);
	}
}

void ndma_pinned_mem_destroy(void)
{
	struct neuron_pinned_mem_process *proc;
	struct hlist_node *tmp;
	int bkt;

	mutex_lock(&pinned_mem_htable_lock);
	hash_for_each_safe(pinned_mem_htable, bkt, tmp, proc, hash_node) {
		hash_del(&proc->hash_node);
		ndma_pinned_mem_destroy_tree(&proc->root);
		kfree(proc);
	}
	mutex_unlock(&pinned_mem_htable_lock);
}

static void ndma_pinned_mem_process_release(struct kref *kref)
{
	struct neuron_pinned_mem_process *proc =
		container_of(kref, struct neuron_pinned_mem_process, refcount);
	mutex_lock(&proc->lock); // this is likely unnecessary because when we get here proc has been removed from the hash table
	                         // on process exit and nobody can find this entry anymore 
	ndma_pinned_mem_destroy_tree(&proc->root);
	mutex_unlock(&proc->lock);
	kfree(proc);
}

/* Find by exact VA match (for unpin) - caller must hold lock */
static struct neuron_pinned_mem *ndma_pinned_mem_find_exact_locked(struct rb_root *root, u64 va)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct neuron_pinned_mem *entry = rb_entry(node, struct neuron_pinned_mem, rb_node);

		if (va < entry->va)
			node = node->rb_left;
		else if (va > entry->va)
			node = node->rb_right;
		else
			return entry; /* exact match */
	}
	return NULL;
}

/* Find region containing VA range (for zerocopy) - caller must hold lock */
static struct neuron_pinned_mem *ndma_pinned_mem_find_containing_locked(struct rb_root *root, u64 va, u64 size)
{
	struct rb_node *node = root->rb_node;
	u64 va_end = va + size;

	while (node) {
		struct neuron_pinned_mem *entry = rb_entry(node, struct neuron_pinned_mem, rb_node);
		u64 entry_end = entry->va + entry->size;

		if (va_end <= entry->va) {
			/* Range is entirely before this entry */
			node = node->rb_left;
		} else if (va >= entry_end) {
			/* Range is entirely after this entry */
			node = node->rb_right;
		} else if (va >= entry->va && va_end <= entry_end) {
			/* Range is fully contained within this entry */
			return entry;
		} else {
			/* Partial overlap - not supported, return NULL */
			return NULL;
		}
	}
	return NULL;
}

/* Insert into rbtree - caller must hold lock */
static int ndma_pinned_mem_insert_locked(struct rb_root *root, struct neuron_pinned_mem *new)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	u64 new_end = new->va + new->size;

	while (*link) {
		struct neuron_pinned_mem *entry = rb_entry(*link, struct neuron_pinned_mem, rb_node);
		u64 entry_end = entry->va + entry->size;

		parent = *link;
		if (new->va < entry->va) {
			/* Check for overlap */
			if (new_end > entry->va)
				return -EEXIST; /* overlaps */
			link = &(*link)->rb_left;
		} else if (new->va > entry->va) {
			/* Check for overlap */
			if (new->va < entry_end)
				return -EEXIST; /* overlaps */
			link = &(*link)->rb_right;
		} else {
			return -EEXIST; /* exact duplicate */
		}
	}

	rb_link_node(&new->rb_node, parent, link);
	rb_insert_color(&new->rb_node, root);
	return 0;
}

/**
 * ndma_check_pages_contiguous() - Check if pinned pages are physically contiguous
 * @pages: Array of pinned pages
 * @nr_pages: Number of pages
 * @offset: Byte offset within the first page
 *
 * Return: Physical address of the start of the region if all pages are
 *         contiguous, or ~0ULL if they are not.
 */
static u64 ndma_check_pages_contiguous(struct page **pages, unsigned long nr_pages, unsigned long offset)
{
	unsigned long i;

	for (i = 1; i < nr_pages; i++) {
		if (page_to_phys(pages[i]) != page_to_phys(pages[i - 1]) + PAGE_SIZE)
			return ~0ULL;
	}
	return (page_to_phys(pages[0]) + offset) | ndhal->ndhal_address_map.pci_host_base;
}

int ndma_pin_host_memory(u64 va, u64 size, u64 *pa_out)
{
	struct neuron_pinned_mem *entry;
	struct neuron_pinned_mem_process *proc;
	unsigned long offset = va & (PAGE_SIZE - 1);
	unsigned long nr_pages = DIV_ROUND_UP(offset + size, PAGE_SIZE);
	int ret;
	long pinned;

	if (va == 0 || size == 0)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->pages = kvmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!entry->pages) {
		ret = -ENOMEM;
		goto err_free_entry;
	}

	/* Try fast path first - doesn't require mmap_lock */
	pinned = pin_user_pages_fast(va & PAGE_MASK, nr_pages, FOLL_WRITE | FOLL_LONGTERM, entry->pages);
	if (pinned < 0 || pinned < nr_pages) {
		/* Fast path failed or incomplete - fall back to slow path */
		if (pinned > 0)
			unpin_user_pages(entry->pages, pinned);

		/* Slow path with mmap_lock */
		mmap_read_lock(current->mm);
#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)))
		pinned = pin_user_pages(va & PAGE_MASK, nr_pages, FOLL_WRITE | FOLL_LONGTERM, entry->pages);
#else
		pinned = pin_user_pages(va & PAGE_MASK, nr_pages, FOLL_WRITE | FOLL_LONGTERM, entry->pages, NULL);
#endif
		mmap_read_unlock(current->mm);

		if (pinned < 0) {
			pr_err("failed to pin pages: %ld\n", pinned);
			ret = pinned;
			goto err_free_pages;
		}
		if (pinned < nr_pages) {
			pr_err("could not pin all pages: %ld/%lu\n", pinned, nr_pages);
			unpin_user_pages(entry->pages, pinned);
			ret = -EFAULT;
			goto err_free_pages;
		}
	}

	entry->va = va;
	entry->size = size;
	entry->nr_pages = nr_pages;
	RB_CLEAR_NODE(&entry->rb_node);

	mutex_lock(&pinned_mem_htable_lock);
	proc = ndma_pinned_mem_get_process_locked(task_tgid_nr(current));
	mutex_unlock(&pinned_mem_htable_lock);
	if (!proc) {
		ret = -ENOMEM;
		goto err_unpin;
	}
	// here and elsewhere, slightly non-obvious.
	// we ref counting proc to make sure it's not deleted in the 
	// unlikely case the process is detached while we are here. Not 
	// possible to happen in this function because it's called from IOCTL
	// but a general pattern is to 1/ lock the hashtable 2/ return ref counted
	// proc entry, 3/ operate on the entry and 4/ decrement the count
	// this is specifically relevant for async zerocopy case getting pinned pages
	// from proc because it's running as an independent thread.
	mutex_lock(&proc->lock);
	ret = ndma_pinned_mem_insert_locked(&proc->root, entry);
	mutex_unlock(&proc->lock);
	kref_put(&proc->refcount, ndma_pinned_mem_process_release);
	if (ret) {
		pr_err("Failed to register, likely due to app failure to unpin previous mmap()\n");
		goto err_unpin;
	}

	/* Report contiguous PA if all pinned pages are physically adjacent. */
	if (pa_out)
		*pa_out = ndma_check_pages_contiguous(entry->pages, nr_pages, offset);

	return 0;

err_unpin:
	unpin_user_pages(entry->pages, nr_pages);
err_free_pages:
	kvfree(entry->pages);
err_free_entry:
	kfree(entry);
	return ret;
}

int ndma_unpin_host_memory(u64 va)
{
	struct neuron_pinned_mem *entry;
	struct neuron_pinned_mem_process *proc;

	mutex_lock(&pinned_mem_htable_lock);
	proc = ndma_pinned_mem_find_process_locked(task_tgid_nr(current));
	mutex_unlock(&pinned_mem_htable_lock);
	if (!proc)
		return -ENOENT;

	mutex_lock(&proc->lock);
	entry = ndma_pinned_mem_find_exact_locked(&proc->root, va);
	if (!entry) {
		mutex_unlock(&proc->lock);
		kref_put(&proc->refcount, ndma_pinned_mem_process_release);
		return -ENOENT;
	}

	rb_erase(&entry->rb_node, &proc->root);
	mutex_unlock(&proc->lock);
	kref_put(&proc->refcount, ndma_pinned_mem_process_release);

	ndma_pinned_mem_free_entry(entry);
	return 0;
}

/* Used by zero-copy API to use pinned pages instead on pinning on demand
 * the copy can run either as part of IOCTL or in async thread, it takes PID
 * of the process that pinned the pages.
 */
static bool ndma_pinned_mem_try_populate(pid_t pid, u64 va, u64 size, struct page **page_list, int nr_pages, struct neuron_pinned_mem_process **prepin_proc)
{
	struct neuron_pinned_mem_process *proc;
	struct neuron_pinned_mem *entry;
	bool found = false;

	*prepin_proc = NULL;

	mutex_lock(&pinned_mem_htable_lock);
	proc = ndma_pinned_mem_find_process_locked(pid);
	mutex_unlock(&pinned_mem_htable_lock);

	if (proc) {
		mutex_lock(&proc->lock);
		entry = ndma_pinned_mem_find_containing_locked(&proc->root, va, size);
		if (entry) {
			unsigned long va_start = va & PAGE_MASK;
			unsigned long pinned_va_start = entry->va & PAGE_MASK;
			unsigned long page_offset = (va_start - pinned_va_start) >> PAGE_SHIFT;
			int i;

			for (i = 0; i < nr_pages; i++)
				page_list[i] = entry->pages[page_offset + i];
			found = true;
		}
		mutex_unlock(&proc->lock);
		if (found) { 
			*prepin_proc = proc;
		} else { // we are holding a ref count for proc, but we did not find/copy any pages
			     // so we don't need to hold on to the proc
			kref_put(&proc->refcount, ndma_pinned_mem_process_release);
		}
	}

	return found;
}

void ndma_pinned_mem_cleanup_process(pid_t pid)
{
	struct neuron_pinned_mem_process *proc;

	mutex_lock(&pinned_mem_htable_lock);
	proc = ndma_pinned_mem_find_process_locked(pid);
	if (proc)
		hash_del(&proc->hash_node); /* prevent new lookups */
	mutex_unlock(&pinned_mem_htable_lock);

	if (proc) {
		/* Drop the find ref; the hash_del above means no new refs can be taken */
		kref_put(&proc->refcount, ndma_pinned_mem_process_release);
		/* Drop the hash table's initial ref — frees proc when last user is done, when ref count is 0 rb tree is deleted and everything is unpinned */
		kref_put(&proc->refcount, ndma_pinned_mem_process_release);
	}
}
