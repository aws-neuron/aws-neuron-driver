// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/list.h>
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
#include "neuron_dma_diag.h"
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


static void ndma_ack_completed_desc(struct ndma_eng *eng, struct ndma_ring *ring, u32 tx_count, u32 rx_count)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_RX, &rxq);

	if (rx_count)
		udma_cdesc_ack(rxq, rx_count);
	if (tx_count)
		udma_cdesc_ack(txq, tx_count);
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

int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 tx_count, u32 rx_count, void * ptr, bool async, bool is_intra_device_dma)
{
	int ret = 0;
	volatile u32 *dst;
	volatile u32 *src;
	u64 i;
	u64 first_wait_time, wait;
	/* wait time scales with descriptor count. Use the larger side so we don't underestimate. */
	u32 wait_count = (tx_count > rx_count) ? tx_count : rx_count;

	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time(wait_count, async, &first_wait_time, &wait);
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
			ndma_ack_completed_desc(eng, ring, tx_count, rx_count);
			break;
		}
		udelay(one_loop_sleep);
	}
	if (i > loop) {
		NDMA_DIAG_DUMP_ENGINE(eng, ring, wait_count, "dma_completion_timeout");
		pr_err("DMA completion timeout on nd%02d for %s q%d tx desc count %u rx desc count %u\n", eng->nd->device_index, eng->udma.name, ring->qid, tx_count, rx_count);
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


// Legacy path outstanding sync transfer constraints
//    Legacy sync/async IO path hold a lock for the duration of the transfer
//    limiting the amount of outstanding requests possible, which prevents the
//    ring from being overflowed. New contraints to coexist w/ zerocopy async flow.
//    - max of 256 * 64K = 16MB per sync transfer + 1 completion descriptor.
//    - max of 2 sync transfers outstanding + small pad (legacy async implementation) 
//
#define NDMA_SYNC_XFER_THRESHOLD 256
#define NDMA_SYNC_REQ_OUTSTANDING_MAX 2
#define NDMA_SYNC_XFER_OUTSTANDING_MAX ((NDMA_SYNC_XFER_THRESHOLD+1) * NDMA_SYNC_REQ_OUTSTANDING_MAX + UDMA_MAX_NUM_CDESC_PER_CACHE_LINE)

static_assert(NDMA_SYNC_XFER_OUTSTANDING_MAX < DMA_H2T_DESC_COUNT/4);

/**
 * ndma_pa_build_n_issue_descs()
 *
 *   Build and issue descriptors for a physical address to physical address
 *   transfer, append the completion descriptor and kick off the DMAs.
 *
 *   Shared by the legacy sync/async path (ndma_memcpy_chunks()) and the zerocopy
 *   async pipeline's PA memcpy request type, so barrier policy and chunking live
 *   in one place.
 *
 *   @src/@dst:         base addresses of the transfer
 *   @size:             bytes remaining to transfer
 *   @smove/@dmove:     apply @offset to src / dst (see ndma_memcpy_offset_move())
 *   @offset:           offset into the transfer to start from
 *   @max_data_descs:   cap on the number of data descriptors issued in this batch.
 *                      Fewer bytes than @size are issued when the cap is hit.
 *   @completion_ptr:   completion buffer the completion descriptor writes to
 *   @bytes_issued:     bytes covered by the descriptors issued
 *   @nr_data_descs:    number of data descriptors issued, excluding the completion descriptor
 *   @last:             last request for this transfere
 */
static int ndma_pa_build_n_issue_descs(struct ndma_eng *eng, struct ndma_ring *ring,
                                       dma_addr_t src, dma_addr_t dst, u64 size,
                                       bool smove, bool dmove, u64 offset,
                                       u32 max_data_descs, void *completion_ptr,
                                       u64 *bytes_issued, u32 *nr_data_descs, bool last)
{
	int        ret;
	u64        chunk_size        = MAX_DMA_DESC_SIZE;
	u64        remaining         = size;
	u32        pending_transfers = 0;
	bool       done              = false;
	int        barrier_type;

	*bytes_issued   = 0;
	*nr_data_descs  = 0;

	while (!done) {
		dma_addr_t src_offset;
		dma_addr_t dst_offset;

		if (remaining <= MAX_DMA_DESC_SIZE) {
			chunk_size = remaining;
		}

		if ((chunk_size == remaining) || (pending_transfers == max_data_descs - 1)) {
			done    = true;
		}

		src_offset = smove ? src + offset : src;
		dst_offset = dmove ? dst + offset : dst;

		ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, (u32)chunk_size, ndhal->ndhal_ndma.ndma_get_m2m_barrier_type(done && last));
		if (ret) {
			return ret;
		}

		offset    += chunk_size;
		remaining -= chunk_size;
		pending_transfers++;

		//TODO trace_dma_memcpy(nd, nc_id, src_offset, dst_offset, chunk_size, pending_transfers);
	}

	*bytes_issued  = size - remaining;
	*nr_data_descs = pending_transfers;

	// write completion descriptor and kick off DMAs
	// SOW isn't strictly necessary since if use use DMA on last, but it doesn't hurt anything and is needed if we
	// don't use DMB in the future when we have an overlap checker
	barrier_type = (last && (narch_get_arch() != NEURON_ARCH_V2)) ? UDMA_M2M_BARRIER_SOW : UDMA_M2M_BARRIER_NONE;
	ret = ndma_memcpy_add_completion_desc( eng, ring, completion_ptr, barrier_type);
	if (ret) {
		return ret;
	}

	pending_transfers++;

	ret = udma_m2m_copy_start(&eng->udma, ring->qid, pending_transfers, pending_transfers);
	if (ret) {
		pr_err("failed to start DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return 0;
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
	int ret;
	u64 bytes_issued;
	u32 nr_data_descs;

	ret = ndma_pa_build_n_issue_descs(eng, ring, dma_ctx->src, dma_ctx->dst, dma_ctx->remaining,
	                                  dma_ctx->smove, dma_ctx->dmove, dma_ctx->offset,
	                                  NDMA_SYNC_XFER_THRESHOLD, dma_ctx->completion_ptr,
	                                  &bytes_issued, &nr_data_descs, true);

	// record pending xfers and data outstanding (the caller prefetches if requested)
	/* ndma_memcpy_chunks emits paired m2s/s2m descriptors, so tx and rx counts match. */
	dma_ctx->pending_tx_transfers = nr_data_descs + 1;
	dma_ctx->pending_rx_transfers = nr_data_descs + 1;
	dma_ctx->outstanding          = bytes_issued;

	return ret;
}

static int _ndma_memcpy_wait_for_completion( struct neuron_device *nd, u32 nc_id, int qid, struct ndma_eng *eng, struct ndma_ring *ring, 
									  struct ndma_h2t_dma_context * dma_ctx, struct ndma_h2t_dma_context * ndma_ctx)
{
	int ret;
	bool async = (dma_ctx != ndma_ctx);

	while(true) {

		ret = ndma_memcpy_wait_for_completion(eng, ring, dma_ctx->pending_tx_transfers, dma_ctx->pending_rx_transfers, dma_ctx->completion_ptr, async, false);

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
	dma_ctx->pending_tx_transfers = 0;
	dma_ctx->pending_rx_transfers = 0;
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
	void                 *host_addr;          // host address (user VA; NULL for PA to PA transfers)
	dma_addr_t            src_pa;             // source address for PA to PA transfers (NDMA_H2T_REQ_PA_MEMCPY);
											  // the destination is dev_addr and host_addr/page_list are unused
	dma_addr_t            dev_addr;           // device address
	u64                   size;               // size for this transfer
	bool                  direction;          // direction. true = to-device/write/copy-in
	bool                  last;               // last transfer for the entire request.
	u64                   start_time;         // start time for this transfer
	int                   nr_pages;           // number of pages for this transfer
	int                   nr_tx_desc;         // number of tx (m2s) descriptors, excluding completion desc
	int                   nr_rx_desc;         // number of rx (s2m) descriptors, excluding completion desc
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

/*
 * H2D DMA Context Queue
 * ---------------------
 * A fixed-size circular buffer storing pointers to `ndma_h2t_zcdma_context`.
 * The queue tracks DMA contexts
 * and maintains four logical indices:
 *
 *   1. head
 *        - Index of the first *valid* (non-empty) entry in the queue.
 *        - The context at `head` may be in any ndma_zcdma_state except
 *          NDMA_COMPLETED.
 *
 *   2. tail
 *        - Index of the next free slot where a new context pointer will be
 *          inserted.
 *
 * Queue state conditions:
 *   - Empty: head == tail
 *   - Full:  head == ((tail + 1) & (capacity - 1))
 *   - Current size: (tail - head + capacity) & (capacity - 1)
 *
 * @entries: Array of DMA context entries in the queue
 * @completion_pool: Pre-allocated pool of completion_ptr buffer
 * @page_list_pool: Pre-allocated pool of page_list arrays
 * @capacity_mask: Maximum number of entries the queue can hold and minus one,
 *                 capacity = capacity_mask + 1
 * @head: Index pointing to the head of the queue
 * @tail: Index pointing to the tail of the queue
 * @nr_pinned_pages: Total count of pinned memory pages belonged to the queue
 */
struct ndma_ctx_queue {
	struct ndma_h2t_zcdma_context *entries;
	void *completion_pool;
	void *page_list_pool;
	u32 capacity_mask;
	u32 head;
	u32 tail;
	u64 nr_pinned_pages;
};

static void ndma_pinned_mem_process_release(struct kref *kref);

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

#define NDMA_ZC_PAGES_PER_XFER  64        // number of pages in each zero copy dma transfer.  This is somewhat, but not
										  // totally arbitrary.  We don't want to pin a lot of pages. We just want to
										  // pin enough where (approximately):  
										  //       dma time > (pin time + setup time + completion update + initial poll wait)
										  // That's the simple explanation. It's a tad more complicated in trading off smaller
										  // transfers where even if that equation doesn't hold, the overlap can be beneficial.
										  // Right now the sweet spot looks to be ~ 64 pages.  More tuning is required.
#define NDMA_ZC_MIN_PAGES_PER_XFER 64

#define NDMA_ZC_PA_DESCS_PER_XFER 64      // number of descriptors in each physical address to physical address
                                          // (NDMA_H2T_REQ_PA_MEMCPY) transfer step.  Nothing is pinned on this path and
                                          // both sides are contiguous, so the pipelining granularity is expressed in
                                          // descriptors instead of pages.  Same tradeoff as NDMA_ZC_PAGES_PER_XFER:
                                          // large enough to keep the engine busy, small enough to interleave with
                                          // other requests on the ring.
#define NDMA_ZC_PA_XFER_MAX ((u64)NDMA_ZC_PA_DESCS_PER_XFER * MAX_DMA_DESC_SIZE)

/* DMA ctx queue constants */
#define NDMA_CTX_QUEUE_DEFAULT_CAPACITY   1024
#define NDMA_CTX_QUEUE_MAX_PINNED_PAGES   524288

/* Check empty or full */
static bool ndma_ctx_queue_is_empty(const struct ndma_ctx_queue *queue)
{
	return queue->head == queue->tail;
}

static bool ndma_ctx_queue_is_full(const struct ndma_ctx_queue *queue)
{
	return queue->head == ((queue->tail + 1) & queue->capacity_mask);
}

static void ndma_ctx_queue_inc_tail(struct ndma_ctx_queue *queue)
{
	queue->tail = (queue->tail + 1) & queue->capacity_mask;
}

/* Peek */
static struct ndma_h2t_zcdma_context *ndma_ctx_queue_peek_tail(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_is_full(queue)) {
		return NULL;
	}
	return &queue->entries[queue->tail];
}

static struct ndma_h2t_zcdma_context *ndma_ctx_queue_peek_head(struct ndma_ctx_queue *queue)
{
	if (ndma_ctx_queue_is_empty(queue)) {
		return NULL;
	}
	return &queue->entries[queue->head];
}

/* Pop */
static struct ndma_h2t_zcdma_context *ndma_ctx_queue_pop_head(struct ndma_ctx_queue *queue)
{
	struct ndma_h2t_zcdma_context *ctx;
	if (ndma_ctx_queue_is_empty(queue)) {
		return NULL;
	}
	ctx = &queue->entries[queue->head];
	queue->head = (queue->head + 1) & queue->capacity_mask;
	return ctx;
}

/* Init and destroy queue */
static int ndma_ctx_queue_init(struct ndma_ctx_queue *queue)
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

static void ndma_ctx_queue_free(struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_ctx_queue *queue)
{
	int bit = ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(eng->eng_id, ring->qid);

	atomic64_andnot(BIT_ULL(bit), &eng->nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);

	if (!queue) {
		return;
	}
	if (queue->entries) {
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
			// To avoid data races, we use DMB for both reads and writes to protect against overlaps. 
			// The old code which had lock protection around data transfers was less agressive and could assume the read
			// path didn't need barriers because the completion cookie provided an implicit barrier.  Async pipelining defeats
			// that barrier.
			//     barrier_type = (remaining == chunk_size && dma_ctx->direction && dma_ctx->last) ? UDMA_M2M_BARRIER_DMB : UDMA_M2M_BARRIER_NONE;
			//
			// FWIW, We don't need the barrier to ensure it's safe to unpin.  We rely on s2m write ordering to ensure pinned
			// pages have no outstanding operations, which is why we use SOW on V3+ architectures for the completion descriptor.
			// For device to host it guarantees all host data is written, for host to device, it also implicitly guaratees 
			// all m2s data reads are complete so we are safe to unpin.
			barrier_type = (remaining == chunk_size && dma_ctx->last) ? UDMA_M2M_BARRIER_DMB : UDMA_M2M_BARRIER_NONE;

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

	/* zero-copy path emits paired m2s/s2m descriptors, so tx and rx counts match. */
	dma_ctx->nr_tx_desc = pending_transfers;
	dma_ctx->nr_rx_desc = pending_transfers;

	// completion descriptor needs a SOW (strong ordered writes) to push data out prior to completion landing
	// This guarantess both the request are 
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

/*
 * Return TX and RX descriptor availability for the paired m2m ring.
 *
 * Both counts are populated in a single pair of queue-handle lookups.
 * Callers can compare each side against its own required count, so this
 * supports asymmetric submissions where tx_count != rx_count.
 */
static void ndma_zc_descs_available(struct ndma_eng *eng, u32 qid, u32 *tx_avail_out, u32 *rx_avail_out)
{
	struct udma_q *txq;
	struct udma_q *rxq;

	udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, qid, UDMA_RX, &rxq);

	*tx_avail_out = udma_available_get(txq);
	*rx_avail_out = udma_available_get(rxq);
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
		/* pin_user_pages_remote() requires the caller to hold mmap_read_lock;
		 * unlike pin_user_pages()/pin_user_pages_fast() it does not take it
		 * internally. We run from the completion kthread (current->mm == NULL)
		 * against a stashed mm, so lock it explicitly here. */
		mmap_read_lock(dma_ctx->mm);
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
		mmap_read_unlock(dma_ctx->mm);
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

//forward
static int ndma_zca_process_ring_cmpltns(struct neuron_device *nd);

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
		ndma_zca_process_ring_cmpltns(nd);
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
	thread = kthread_run(ndma_h2d_cmpltn_thread_fn, nd, "nd%d dma cmpltn", nd->device_index);
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

/*
 * NEW CODE to handle queuing of h2t DMA requests (sync and async)
 *
 * ndma_h2t_dma_ioq   - contains h2t DMA related structures (except for completion queue atm)
 * request queue      - tracks user submitted requests
 * context queue      - tracks in flight data from requests.  These use the existing context queue entries.
 *                      need to be stripped down to minimal entries
 * spill record queue - when completion queue is full, we place completions in the spill record queue
 *
 * So flow is: 
 *  1. add to request queue.
 *  2. process requests into one or more context queue entries that track outstanding DMA operations.
 *     limited by credits as to how much we can process at a given go at it.
 *     set the service queue bit and kick the completion thread.
 *  3. completion thread
 *  3a. service rings (no-wait to see if there DMAs that have finished and process the 
 *      associated context queue entris.
 *  3b. service the spill record queue (to push held up async completions to the user)
 *  3c. service the request queue. lock held.  If idle, clear the service queue bit. unlock.
 *
 */

/* mimimized version of a context entry. not used yet
 *
 */
struct ndma_h2t_zcdma_context_minimal {
	dma_addr_t            dev_addr;           // device address (debug)
	void                 *completion_ptr;     // completion buffer pointer;
											  // host memory buffer which driver polls on for completions;
											  // managed by completion_pool in ctx queue
	u64                   sequence_num;       // async sequence number; 0 for sync transfers
	void                  *context;           // async completion context
	bool                  direction;          // direction. true = to-device/write/copy-in
	bool                  last;               // last transfer for the entire request.
	enum ndma_zcdma_state state;              // state of this transfer (shrink to u16)
	u16                   nr_pages;           // number of pages for this transfer (0 for bar4 writes)
	u16                   nr_tx_desc;         // number of tx (m2s) descriptors, excluding completion desc
	u16                   nr_rx_desc;         // number of rx (s2m) descriptors, excluding completion desc
	union {
		struct page       **page_list;        // page structures tracking our pinned pages;
		s64               compl_ret;          // result for in-order CQ completion of completed work. Only valid for async.
    };
};

/* what kind of transfer the queued request represents */
enum ndma_h2t_req_type {
	NDMA_H2T_REQ_DMA_MEMCPY = 0,	// standard zerocopy DMA (pin pages, build descs, submit)
	NDMA_H2T_REQ_BAR4_WR,			// BAR4 mmio write - already completed, only enqueued for CQE ordering
	NDMA_H2T_REQ_DESC_EXECUTE,		// pre-built tx/rx descriptors supplied by userspace
	NDMA_H2T_REQ_PA_MEMCPY,			// physical address to physical address copy (mem chunk to mem chunk).
									// no pages to pin, addresses are already DMA addresses
};

/**
 * struct to track a queued request. Common bookkeeping is at the top; per-type
 * fields live in the union below. `ops[]` is the trailing flexible array used
 * only by NDMA_H2T_REQ_DMA_MEMCPY (kzalloc'd with num_ops * sizeof(op) tail).
 */
struct ndma_h2t_queued_request {
	struct list_head queuelist;		// linked list of waiting requests
	enum ndma_h2t_req_type type;    // request kind - selects processing branch
	u64              sequence_num;  //
	void             *context;      //
	pid_t			 pid;			// PID of the process that initiated the copy
	union {
		struct {
			u32               num_ops;
			dma_addr_t        dev_base;
			bool              direction;
			struct mm_struct *mm;    // mm that owns the user buffers
			nrt_tensor_batch_op_t ops[];    // NDMA_H2T_REQ_DMA_MEMCPY only
		} memcpy;
		struct {
			s64               compl_ret; // pre-computed BAR4 write status echoed in CQE
		} bar4_wr;
		struct {
			dma_addr_t        src_base;  // source dma address
			dma_addr_t        dst_base;  // destination dma address
			u64               size;      // NDMA_H2T_REQ_PA_MEMCPY only
		} pa_memcpy;
		struct {
			union udma_desc  *tx_desc_buf; // pointer to `descs` for the tx portion of the descriptors
			union udma_desc  *rx_desc_buf; // pointer to `descs` for the rx portion of the descriptors
			u32               nr_tx_desc;
			u32               nr_rx_desc;
			union udma_desc   descs[]; // kernel-side copy of user tx and rx descriptors
		} desc_execute;
	};
};

/**
 * if the h2t dma completion queue used to communicate to user space is full (application hasn't been consuming),
 * we collect the overflow into a linked list of spill records that each have a smallish queue of completion
 * queue entries. The records are compact to fit into 4k of memory and have a little over 300 entries in each.
 *
 */
#define NDMA_H2T_COMPL_SPILL_REC_Q_CNT ((4096 - sizeof(struct ndma_h2t_compl_spill_q_rec_hdr)) / sizeof(neuron_h2d_dma_compl_queue_entry_t))

struct ndma_h2t_compl_spill_q_rec  {
	struct ndma_h2t_compl_spill_q_rec_hdr {
		struct list_head queuelist;  // linked list of spill queue recs
		u16              head;       // head in this spill q
		u16              tail;       // tail in this spill q
	} hdr;
	neuron_h2d_dma_compl_queue_entry_t queue[NDMA_H2T_COMPL_SPILL_REC_Q_CNT];
};

/**
 * h2t dma ioq structure
 *
 *   need to describe purpose
 */
struct ndma_h2t_dma_ioq {
	struct ndma_eng       *eng;
	struct ndma_ring      *ring;
	u32                   qid;
	struct list_head      queued_req_head;
	u64                   pin_retries;
	struct ndma_ctx_queue ctx_q;
	u64                   compl_timeout_deadline;
	bool                  cleanup;

	// execution context of current op for (head) request being processed
	// only relevant for requests that are managing a data transfer.  Other
	// requests are handled immediatly (CCE/Bar4 for example are handled immediately)
	struct ndma_h2t_queued_req_op_ctx {
		int        cur_op_index;   // index of current op (-1 == no current entry)
		bool       last_op;        // last op
		void       *host_addr;     // user VA of the host buffer (unused for PA to PA transfers)
		dma_addr_t src_pa;         // source dma address (PA to PA transfers only)
		dma_addr_t dev_addr;       //
		u64        offset;         //
		u64        xfer_size;      // size to transfer in the next step. pages pinned for that step
								   // for zerocopy requests, no pinning for PA to PA transfers or MC to MC transfers
		u64        remaining;      //
	} current_op;

	wait_queue_head_t  sync_wait_queue; // wait queue for synchronous requests.
	struct ndma_h2t_sync_wait_pool {
		atomic64_t map;
		u64 pool[64];
	} sync_wait_pool;

	struct list_head compl_spill_q_head;
}; 

/**
 * credit limits for servicing request queue
 *    submit limit is ~64MB (256 * 64 * 4096) aka (256 * max pinned pages per transfer * 4096)
 *    thread limit is 1/4 of that
 */
#define NDMA_ZC_SUBMIT_CREDIT_LIMIT 256
#define NDMA_ZC_THREAD_CREDIT_LIMIT 64

// FORWARDS
// 
static void ndma_zca_dma_ioq_initiate_cleanup(struct ndma_h2t_dma_ioq *dma_ioq);
static void ndma_zca_cleanup_queued_requests(struct ndma_h2t_dma_ioq *dma_ioq, bool discard_async);
static int ndma_zca_process_queued_requests(struct ndma_h2t_dma_ioq *dma_ioq, u32 credit_limit, bool remote);
static void ndma_zca_zerocopy_unpin_pages(struct ndma_h2t_zcdma_context *dma_ctx, u64 *nr_pinned_pages);

/** 
 * sync completion support
 *    
 *   sync zerocopy requests flow through the same request queue to context queue flow
 *   as async requests. sync requests can't use the completion queue to wait on (they 
 *   have no sequence number to correlate the completion with), so instead they
 *   wait on sync completion markers via a completion queue.  There's a small number (64)
 *   of these markers allocated via a bitmap.
 *    
 */

#define NDMA_ZC_SYNC_COMPL_MARKER (0xfeedbeef)
#define NDMA_ZC_SYNC_MAKE_COMPL_MARKER(r) ((u64)NDMA_ZC_SYNC_COMPL_MARKER | ((u64)(r) << 32))
#define NDMA_ZC_SYNC_CHK_COMPL_MARKER(p) ((*((u64*)(p)) & 0xffffffff) == NDMA_ZC_SYNC_COMPL_MARKER) 
#define NDMA_ZC_SYNC_COMPL_MARKER_RET(p) (((*((u64*)(p)) >> 32) & 0xffffffff))

/** 
 * ndma_zca_sync_compl_alloc()
 *
 *   Allocate a sync completion marker - used for waking
 *   syncrhonous zero copy requests and return status.  When a 
 *   syncrhonous zero copy request is recieved, it needs to be queued
 *   like async zero copy requests, so we need to allocate a marker/cookie
 *   to wait on.  Simple spin loop waiting for a free entry in a bitmap.
 *
 *   Called with the ring lock held for allocation.  Frees don't need locking.  Just atomic
 */
static u64 *ndma_zca_sync_compl_alloc(struct ndma_h2t_dma_ioq *dma_ioq)
{
	int pos;

	while (atomic64_read(&dma_ioq->sync_wait_pool.map) == ~0ULL) {
		mutex_unlock(&dma_ioq->ring->h2t_ring_lock);
		usleep_range(10, 10);
		mutex_lock(&dma_ioq->ring->h2t_ring_lock);
		if (fatal_signal_pending(current)) {
			return NULL;
		}
	}
	
	pos = ffz(atomic64_read(&dma_ioq->sync_wait_pool.map));
	atomic64_or(BIT_ULL(pos), &dma_ioq->sync_wait_pool.map);
	dma_ioq->sync_wait_pool.pool[pos] = 0ULL;
	return &dma_ioq->sync_wait_pool.pool[pos];
}

static void ndma_zca_sync_compl_free(struct ndma_h2t_dma_ioq *dma_ioq, u64 *compl_marker)
{
	u32 pos = (compl_marker - dma_ioq->sync_wait_pool.pool);
	atomic64_andnot(BIT_ULL(pos), &dma_ioq->sync_wait_pool.map);
}

/* ndma_zca_dma_ioq_idle()
 *
 *   check if a dma ioq is idle (no requests, no contexts, and completion spill queue list empty)
 */
static bool ndma_zca_dma_ioq_idle(struct ndma_h2t_dma_ioq *dma_ioq)
{
	return (list_empty(&dma_ioq->queued_req_head) && ndma_ctx_queue_is_empty(&dma_ioq->ctx_q) && list_empty(&dma_ioq->compl_spill_q_head));
}

/**
 * ndma_zca_dma_ioq_from_bit()
 *
 *   Fetch a dma_ioq structure from the active dma_ioq bitmap
 */
static inline struct ndma_h2t_dma_ioq * ndma_zca_dma_ioq_from_bit(struct neuron_device *nd, int bit)
{
	u32 eng_id;
	u32 qid;
	struct ndma_eng *eng;
	struct ndma_ring *ring;
	
	ndhal->ndhal_ndmar.ndmar_ctx_queue_from_bit(bit, &eng_id, &qid);
	eng     = &nd->ndma_engine[eng_id];
	ring    = &eng->queues[qid].ring_info;
	return ring->h2t_dma_ioq;
}

/**
 * ndma_zca_compl_queue_avail()
 *
 *    returns number of free entries in the completion queue.  The legacy
 *    implementation didn't wrap the head index on the runtime side (grr),
 *    so there's a wrap check/fix and warning.
 */
static u32 ndma_zca_compl_queue_avail(struct ndma_h2d_compl_queue *compl_queue)
{
	neuron_h2d_dma_compl_queue_t *compl_queue_shared = compl_queue->compl_queue_shared;
	u32 head = smp_load_acquire(&compl_queue_shared->head);
	u32 avail_cnt;

	if (head > compl_queue->capacity_mask) {
		pr_warn_once("warning legacy runtime using non-wrapping head/tail async IO completion queue implementation\n");
		head = head & compl_queue->capacity_mask;
	}
	avail_cnt = compl_queue->capacity_mask - ((compl_queue->tail - head) & compl_queue->capacity_mask);
	return avail_cnt;
}

/**
 * ndma_zca_compl_queue_add()
 *
 *   Add an entry to the tail of the completion queue (shared w/ user space) then
 *   update our local copy of tail along with the tail in the completion queue.
 */
static void ndma_zca_compl_queue_add(struct ndma_h2d_compl_queue *compl_queue,
							         u64 sequence_num, s64 compl_ret, void *context)
{
	neuron_h2d_dma_compl_queue_t *compl_queue_shared = compl_queue->compl_queue_shared;
	neuron_h2d_dma_compl_queue_entry_t *entry = &compl_queue_shared->entries[compl_queue->tail];
	entry->compl_ret = compl_ret;
	entry->context = context;
	entry->sequence_num = sequence_num;

	compl_queue->tail = (compl_queue->tail + 1) & compl_queue->capacity_mask;
	smp_store_release(&compl_queue_shared->tail, compl_queue->tail);
}

static bool ndma_zca_compl_spill_rec_q_full(struct ndma_h2t_compl_spill_q_rec *spill_rec)
{
	return (spill_rec->hdr.head == (spill_rec->hdr.tail +1) % NDMA_H2T_COMPL_SPILL_REC_Q_CNT);
}

static bool ndma_zca_compl_spill_rec_q_empty(struct ndma_h2t_compl_spill_q_rec *spill_rec)
{
	return (spill_rec->hdr.head == spill_rec->hdr.tail);
}

/**
 * ndma_zca_compl_spill_add()
 *
 *   There was no room in the completion queue so add the completion to 
 *   a spill record.  There's a list of Spill records, each with a tiny
 *   queue o entries, all kept in order that the completions are received.
 */
static int ndma_zca_compl_spill_add(struct ndma_h2t_dma_ioq *dma_ioq, u64 sequence_num, s64 compl_ret, void *context)
{
	struct ndma_h2t_compl_spill_q_rec *spill_rec;
	neuron_h2d_dma_compl_queue_entry_t *ent;
				                            
	if (!list_empty(&dma_ioq->compl_spill_q_head)) {
		spill_rec = list_last_entry(&dma_ioq->compl_spill_q_head, struct ndma_h2t_compl_spill_q_rec, hdr.queuelist);
		if (!ndma_zca_compl_spill_rec_q_full(spill_rec)) {
			goto add_entry;
		}
	}
	// allocate a new spill record
	//
	spill_rec = kzalloc(sizeof(*spill_rec), GFP_KERNEL);
	if (spill_rec == NULL)  {
		 pr_err("nd%02d: async io completion spill record allocation failure\n", dma_ioq->eng->nd->device_index);
		return -ENOMEM;
	}
	list_add_tail(&spill_rec->hdr.queuelist, &dma_ioq->compl_spill_q_head);

add_entry:
	ent = &spill_rec->queue[spill_rec->hdr.tail];
	ent->sequence_num = sequence_num;
	ent->compl_ret = compl_ret;
	ent->context = context;
	spill_rec->hdr.tail = (spill_rec->hdr.tail + 1) % NDMA_H2T_COMPL_SPILL_REC_Q_CNT;
	return 0;
}

/**
 * ndma_zca_compl_process_spill()
 *
 *   If there wasn't room in the completion queue (user hasn't consumed async completions),
 *   we have to create spill records to hold additional completions to prevent the 
 *   ctx queue (plus DMA ring) and request queue from all backing up. If spill is not empty,
 *   we try to drain it to the completion queue as space is avialable.
 */
static void ndma_zca_compl_process_spill(struct ndma_h2t_dma_ioq *dma_ioq)
{
	if (!list_empty(&dma_ioq->compl_spill_q_head)) {
		struct ndma_h2d_compl_queue *compl_queue = &dma_ioq->ring->dma_compl_queue;
		u32 compl_q_avail = ndma_zca_compl_queue_avail(compl_queue);

		while (!list_empty(&dma_ioq->compl_spill_q_head) && compl_q_avail) {
			struct ndma_h2t_compl_spill_q_rec *spill_rec = list_first_entry(&dma_ioq->compl_spill_q_head, 
					                                                        struct ndma_h2t_compl_spill_q_rec, hdr.queuelist);
			
			while (compl_q_avail) {
				neuron_h2d_dma_compl_queue_entry_t *ent;
				if (ndma_zca_compl_spill_rec_q_empty(spill_rec)) {
					list_del(&spill_rec->hdr.queuelist);
					kfree(spill_rec);
					break;
				}
				ent = &spill_rec->queue[spill_rec->hdr.head];
				ndma_zca_compl_queue_add(compl_queue, ent->sequence_num, ent->compl_ret, ent->context);
				spill_rec->hdr.head = (spill_rec->hdr.head + 1) % NDMA_H2T_COMPL_SPILL_REC_Q_CNT;
				compl_q_avail--;
			}
		}
	}
}

/**
 * ndma_zca_compl_cleanup_spill()
 *
 *   cleanup the spill record queue
 *   NOTE: only call from the completion thread
 */
static void ndma_zca_compl_cleanup_spill(struct ndma_h2t_dma_ioq *dma_ioq, bool discard_async)
{
	// first try to complete any requests
	//
	if (!discard_async) {
		ndma_zca_compl_process_spill(dma_ioq);
	}

	while (!list_empty(&dma_ioq->compl_spill_q_head)) {
		struct ndma_h2t_compl_spill_q_rec *spill_rec = list_first_entry(&dma_ioq->compl_spill_q_head, 
					                                                    struct ndma_h2t_compl_spill_q_rec, hdr.queuelist);
		list_del(&spill_rec->hdr.queuelist);
		kfree(spill_rec);
	}
}

/**
 * ndma_zca_release_ctx_resources()
 *   release resources allocated to the context and set the state to invalid.
 *   do not mess with the completion information or last flag, these are used after 
 *   the resources are released.
 */
static void ndma_zca_release_ctx_resources(struct ndma_h2t_zcdma_context *ctx, u64 *nr_pinned_pages)
{
	// do not free or set completion_ptr null. it is managed by completion_pool in ctx queue
	// do not free or set page_list null. it is managed by page_list_pool in ctx queue

	ndma_zca_zerocopy_unpin_pages(ctx, nr_pinned_pages);
	ctx->nr_pages = 0;
	ctx->prepin_proc = NULL;
	ctx->state = NDMA_INVALID;
}

/**
 * ndma_zca_complete_request()
 *
 *   for sync, wake the waiting process
 *   for async, put the completed request in the completion queue.  If there's not space in the
 *   completion queue or the spill queue isn't empty, add the completion to the spill list queue
 */
static int ndma_zca_complete_request(struct ndma_h2t_dma_ioq *dma_ioq, u64 sequence_num, s64 compl_ret, void *context, bool discard_async)
{
	int ret = 0;
	if (sequence_num == 0) {
		*((u64*)context) = NDMA_ZC_SYNC_MAKE_COMPL_MARKER(compl_ret);
		wake_up(&dma_ioq->sync_wait_queue);
	} else if (!discard_async) {
		struct ndma_h2d_compl_queue *compl_queue = &dma_ioq->ring->dma_compl_queue;
		if (list_empty(&dma_ioq->compl_spill_q_head) && ndma_zca_compl_queue_avail(compl_queue)) {
			ndma_zca_compl_queue_add(compl_queue,sequence_num, compl_ret, context);
		} else {
			ret = ndma_zca_compl_spill_add(dma_ioq, sequence_num, compl_ret, context);
		}
	}
	return ret;
}

/**
 * ndma_zca_dma_ioq_cleanup_ctx_queue()
 *
 *   cleanup the dma context queue (and descriptor ring).  Once
 *   we hit a timeout, we assume the dma ring is wedged and we can just clear out the
 *   remaining ctx.  Once dma_ioq->cleanup has been set, no more ctx queue entries will
 *   be created 
 */
static void ndma_zca_dma_ioq_cleanup_ctx_queue(struct ndma_h2t_dma_ioq *dma_ioq)
{
	int ret = 0;

	while (!ndma_ctx_queue_is_empty(&dma_ioq->ctx_q)) {
		struct ndma_h2t_zcdma_context * head_ctx = ndma_ctx_queue_peek_head(&dma_ioq->ctx_q);

		if (head_ctx->state == NDMA_COMPLETED) {
			u64 dummy;
			ndma_zca_release_ctx_resources(head_ctx, &dummy);
			if (head_ctx->last) { 
				ndma_zca_complete_request(dma_ioq, head_ctx->sequence_num, head_ctx->compl_ret, head_ctx->context, false);
			}
		} else if (head_ctx->state == NDMA_SUBMITTED) {
			if (ret != -ETIMEDOUT) {
				// consume completions as long as we don't see a timeout.
				ret = ndma_memcpy_wait_for_completion(dma_ioq->eng, dma_ioq->ring, head_ctx->nr_tx_desc + 1,
							                          head_ctx->nr_rx_desc + 1,
							                          head_ctx->completion_ptr, true, false);
			}
			head_ctx->compl_ret = ret;
			ndma_zca_release_ctx_resources(head_ctx, &dma_ioq->ctx_q.nr_pinned_pages);
			if (head_ctx->last) { 
				ndma_zca_complete_request(dma_ioq, head_ctx->sequence_num, head_ctx->compl_ret, head_ctx->context, false);
			}
		} else {
			pr_err("nd%02d: h2d dma completion queue entry has invalid state %d\n", dma_ioq->eng->nd->device_index, head_ctx->state);
		}
		ndma_ctx_queue_pop_head(&dma_ioq->ctx_q);
	}
}

/**
 * ndma_zca_dma_ioq_process_ctx_queue_no_wait()
 *
 *
 *   Note: This is currently called w/o locking.  Assumption is only a single thread owns both the completion queue
 *   (tail) and the and the head of the context queue
 */
static int ndma_zca_dma_ioq_process_ctx_queue_no_wait(struct ndma_h2t_dma_ioq *dma_ioq)
{
	int ret = 0;
	
	while (!ndma_ctx_queue_is_empty(&dma_ioq->ctx_q)) {
		struct ndma_h2t_zcdma_context *head_ctx = ndma_ctx_queue_peek_head(&dma_ioq->ctx_q);

		if (head_ctx->state == NDMA_COMPLETED) {
			u64 dummy;
			ndma_zca_release_ctx_resources(head_ctx, &dummy);
			if (head_ctx->last) { 
				ret = ndma_zca_complete_request(dma_ioq, head_ctx->sequence_num, head_ctx->compl_ret, head_ctx->context, false);
			}
			ndma_ctx_queue_pop_head(&dma_ioq->ctx_q);
		} else if (head_ctx->state == NDMA_SUBMITTED) {
			volatile u32 *dst = (volatile u32 *)(head_ctx->completion_ptr + DMA_COMPLETION_MARKER_SIZE);
			volatile u32 *src = (volatile u32 *)head_ctx->completion_ptr;
			u32 dst_val = READ_ONCE(*dst);

			if (dst_val == DMA_COMPLETION_MARKER) {
				WRITE_ONCE(*dst, 0);                         // this isn't strictly necessary but it will detect improper reuse issues
				WRITE_ONCE(*src, DMA_COMPLETION_MARKER);     // this isn't strictly necessary but it will detect improper reuse issues
				dma_ioq->compl_timeout_deadline = 0ull;      // reset completion timeout deadline
				ndma_ack_completed_desc(dma_ioq->eng, dma_ioq->ring, head_ctx->nr_tx_desc + 1, head_ctx->nr_rx_desc + 1);

				ndma_zca_release_ctx_resources(head_ctx, &dma_ioq->ctx_q.nr_pinned_pages);
				if (head_ctx->last) { 
					ret = ndma_zca_complete_request(dma_ioq, head_ctx->sequence_num, head_ctx->compl_ret, head_ctx->context, false);
				}
				ndma_ctx_queue_pop_head(&dma_ioq->ctx_q);
			} else {
				// if completion deadline isn't set, set it, otherwise check it
				if (dma_ioq->compl_timeout_deadline == 0ull) {
					u64 dummy, wait;
					ndhal->ndhal_ndma.ndma_get_wait_for_completion_time(head_ctx->nr_tx_desc, true, &dummy, &wait);
					dma_ioq->compl_timeout_deadline = get_jiffies_64() + usecs_to_jiffies(wait) + 1;
				} else if (get_jiffies_64() > dma_ioq->compl_timeout_deadline) {
					NDMA_DIAG_DUMP_ENGINE(dma_ioq->eng, dma_ioq->ring, head_ctx->nr_tx_desc + 1, "h2d_timeout");
					pr_err("nd%02d: h2d completion timeout on device addr: %llx size: %llx sequenc_num: %llx",
							dma_ioq->eng->nd->device_index, head_ctx->dev_addr, head_ctx->size, head_ctx->sequence_num);
					ret = -ETIMEDOUT;

					ndma_zca_release_ctx_resources(head_ctx, &dma_ioq->ctx_q.nr_pinned_pages);
					if (head_ctx->last) { 
						ret = ndma_zca_complete_request(dma_ioq, head_ctx->sequence_num, ret, head_ctx->context, false);
					}
					ndma_ctx_queue_pop_head(&dma_ioq->ctx_q);
				}
				break;
			}
		} else {
			pr_err("nd%02d: h2d dma completion queue entry has invalid state %d\n", dma_ioq->eng->nd->device_index, head_ctx->state);
			ret = -ENXIO; // invalid entry 
		}
	}
	return ret;
}

#define for_each_set_dma_ioq_bit(bit, val, start)                              \
	for (_tmp = (start) ? (((val) >> (start)) | ((val) << (64 - (start)))) \
		: (val);                                                               \
		_tmp && ((bit = (__ffs(_tmp) + (start)) & 63), 1);                     \
		_tmp &= _tmp - 1)

/**
 * ndma_zca_process_ring_cmpltns()
 *
 *    thread owns ring head and completion queue.  No locks (or separate lock)
 *
 *    Note: the thread can't touch a dma_ioq unless the bit for the queue is
 *          set in the bitmap.
 */
static int ndma_zca_process_ring_cmpltns(struct neuron_device *nd)
{
	int ret = 0; 
	u64 bitmap = atomic64_read(&nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);
	int start = nd->dma_cmpltn_thread.rr_start_bit;
	int bit;
	u64 _tmp;

	// process ring completions no wait and add to completion queue (no locking) I think this is doable
	// 
	for_each_set_dma_ioq_bit(bit, bitmap, start) {
		struct ndma_h2t_dma_ioq *dma_ioq = ndma_zca_dma_ioq_from_bit(nd, bit);
		ret = ndma_zca_dma_ioq_process_ctx_queue_no_wait(dma_ioq);
		if (ret) {
			dma_ioq->cleanup = true; // should set discard flag
		}
	}

	// process completion queue spill records
	//
	for_each_set_dma_ioq_bit(bit, bitmap, start) {
		struct ndma_h2t_dma_ioq *dma_ioq = ndma_zca_dma_ioq_from_bit(nd, bit);
		ndma_zca_compl_process_spill(dma_ioq);
	}

	// process any queued requests and check for idle and for external cleanup request
	//
	for_each_set_dma_ioq_bit(bit, bitmap, start) {
		struct ndma_h2t_dma_ioq *dma_ioq = ndma_zca_dma_ioq_from_bit(nd, bit);

		if (mutex_trylock(&dma_ioq->ring->h2t_ring_lock)) {
			ret = ndma_zca_process_queued_requests(dma_ioq, NDMA_ZC_THREAD_CREDIT_LIMIT, true);
			if (ret) {
				dma_ioq->cleanup = true;
			}
			// Don't clear the bitmap if cleanup is set.  A clear bitmap bit is our interlock with the code that
			// frees the dma_ioq
			if (ndma_zca_dma_ioq_idle(dma_ioq) && !dma_ioq->cleanup) {
				atomic64_andnot(BIT_ULL(bit), &nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);
			}
			mutex_unlock(&dma_ioq->ring->h2t_ring_lock);

		}
	}

	// if any queues have cleanup requests, perform cleanup.  Need to refresh
	// the bitmap because it could be stale and as mentioned above, it's our interlock w/ code the frees the dma_ioq
	bitmap = atomic64_read(&nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);

	for_each_set_dma_ioq_bit(bit, bitmap, start) {
		struct ndma_h2t_dma_ioq *dma_ioq = ndma_zca_dma_ioq_from_bit(nd, bit);
		if (dma_ioq->cleanup) {
			mutex_lock(&dma_ioq->ring->h2t_ring_lock);
			ndma_zca_dma_ioq_cleanup_ctx_queue(dma_ioq);
			ndma_zca_cleanup_queued_requests(dma_ioq, true);
			ndma_zca_compl_cleanup_spill(dma_ioq, false);
			if (!ndma_zca_dma_ioq_idle(dma_ioq)) {
				pr_err("unexpected: cleanup was unsuccessful");
			}
			mutex_unlock(&dma_ioq->ring->h2t_ring_lock);
			atomic64_andnot(BIT_ULL(bit), &nd->dma_cmpltn_thread.nonempty_ctxq_bitmap);
		}
	}

	// rotate start to next core (estimated)
	//
	nd->dma_cmpltn_thread.rr_start_bit = (start + 8) & 63;

	cpu_relax();
	if (need_resched())
		cond_resched();
	return ret;
}

/**
 * ndma_zca_dma_ioq_init()
 *
 */
int ndma_zca_dma_ioq_init(struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_h2t_dma_ioq **dma_ioq)
{
	int ret = 0;
	struct ndma_h2t_dma_ioq *p;

	if (*dma_ioq != NULL) {
		pr_err("trying to initialize dma ioq structure when previous not freed");
		return -EEXIST;
	}
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL) {
		return -ENOMEM;
	}
	p->eng  = eng;
	p->ring = ring;
	p->qid  = ring->qid;
	INIT_LIST_HEAD(&p->queued_req_head);
	p->pin_retries = 0ull;
	p->compl_timeout_deadline = 0ull;
	p->current_op.cur_op_index = -1;
	init_waitqueue_head(&p->sync_wait_queue);
	atomic64_set(&p->sync_wait_pool.map, 0);
	INIT_LIST_HEAD(&p->compl_spill_q_head);
	
	ret = ndma_ctx_queue_init(&p->ctx_q);
	if (ret) {
		kfree(p);
		return ret;
	}
	*dma_ioq = p;
	return 0;
}

/**
 * ndma_zca_dma_ioq_free()
 */
void ndma_zca_dma_ioq_free(struct ndma_h2t_dma_ioq **dma_ioq)
{
	struct ndma_h2t_dma_ioq *p = *dma_ioq;
	if (p == NULL) {
		return;
	}
	ndma_zca_dma_ioq_initiate_cleanup(p);
	ndma_ctx_queue_free(p->eng, p->ring, &p->ctx_q);
	kfree(p);
	*dma_ioq = NULL;
}

/**
 * Credit calculating functions for each of the three resources needed to submit a request
 */
static u32 ndma_zca_ctx_q_avail(struct ndma_ctx_queue *ctx_q)
{
	u32 avail_cnt = ctx_q->capacity_mask - ((ctx_q->tail - ctx_q->head) & ctx_q->capacity_mask);
	return avail_cnt;
}
		
static u32 ndma_zca_pinned_avail(struct ndma_h2t_dma_ioq *dma_ioq, u32 pin_size)
{
	return (NDMA_CTX_QUEUE_MAX_PINNED_PAGES - dma_ioq->ctx_q.nr_pinned_pages) / pin_size;
}

/**
 * ndma_zca_ring_avail
 *
 *    return the number of transfer steps that fit in the available descriptor
 *    entries, less what is reserved for legacy sync.  @nr_descs is the worst case
 *    data descriptor count for one step; the +1 covers its completion descriptor.
 */
static u32 ndma_zca_ring_avail(struct ndma_h2t_dma_ioq *dma_ioq, u32 nr_descs)
{
	u32 tx_avail, rx_avail, tmp;

	ndma_zc_descs_available(dma_ioq->eng, dma_ioq->qid, &tx_avail, &rx_avail);
	/* Paired-op capacity is bounded by whichever ring has less room. */
	tmp = (tx_avail < rx_avail) ? tx_avail : rx_avail;
	if (tmp < NDMA_SYNC_XFER_OUTSTANDING_MAX) {
		return 0;
	}
	return (tmp - NDMA_SYNC_XFER_OUTSTANDING_MAX) / (nr_descs + 1);
}

/**
 * ndma_zca_ring_has_credit
 *
 *    return true iff both tx and rx rings have room for the requested slot
 *    counts after honoring the legacy sync reservation. Uses the same
 *    threshold as ndma_zca_ring_avail but checks each side independently
 *    against its own need.
 */
static bool ndma_zca_ring_has_credit(struct ndma_h2t_dma_ioq *dma_ioq, u32 tx_need, u32 rx_need)
{
	u32 tx_avail, rx_avail;

	ndma_zc_descs_available(dma_ioq->eng, dma_ioq->qid, &tx_avail, &rx_avail);
	return tx_avail >= tx_need + NDMA_SYNC_XFER_OUTSTANDING_MAX &&
	       rx_avail >= rx_need + NDMA_SYNC_XFER_OUTSTANDING_MAX;
}

/**
 * ndma_zca_zerocopy_unpin_pages()
 *
 */
static void ndma_zca_zerocopy_unpin_pages(struct ndma_h2t_zcdma_context *dma_ctx, u64 *nr_pinned_pages)
{
	if (dma_ctx->state >= NDMA_PINNED_UNSUBMITTED && dma_ctx->nr_pages > 0) {
		/* Only unpin if we pinned it ourselves (not pre-pinned memory) */
		if (!dma_ctx->prepin_proc) {
			if (dma_ctx->direction) {
				unpin_user_pages(dma_ctx->page_list, dma_ctx->nr_pages);
			} else {
				unpin_user_pages_dirty_lock(dma_ctx->page_list, dma_ctx->nr_pages, true);
			}
		} else {
			kref_put(&dma_ctx->prepin_proc->refcount, ndma_pinned_mem_process_release);
		}
		*nr_pinned_pages -= dma_ctx->nr_pages;
	}
}

/**
 * ndma_zca_dma_ctx_init_common()
 *    Init for dma ctx common fields
 */
static void ndma_zca_dma_ctx_init_common(struct ndma_h2t_dma_ioq *dma_ioq, struct ndma_h2t_queued_request *request, struct ndma_h2t_zcdma_context *ctx)
{
	ctx->eng           = dma_ioq->eng;
	ctx->ring          = dma_ioq->ring;
	ctx->nr_tx_desc    = 0; // Set by ndma_build_n_issue_zc_descs()/ndma_zca_build_n_issue_pa_descs().
	ctx->nr_rx_desc    = 0;
	ctx->host_addr     = NULL;
	ctx->src_pa        = 0;
	ctx->mm            = NULL;
	ctx->pid		   = request->pid;
	ctx->sequence_num  = request->sequence_num;
	ctx->context       = request->context;
	ctx->compl_ret     = 0;
}

/** 
 * ndma_zca_dma_ioq_current_op_update()
 *   This function is used to update the dma_ioq's current op context.  
 *   current_op contains the context of the op of the current request we are processing for this
 *   h2t queue (batch requests can have multiple ops).  
 *   If done with this request, return true
 */
static bool ndma_zca_dma_ioq_current_op_update(struct ndma_h2t_queued_req_op_ctx *current_op, struct ndma_h2t_queued_request *request)
{
	nrt_tensor_batch_op_t *op;
	current_op->cur_op_index++;

	// PA to PA copies are a single op with both sides already resolved to dma addresses,
	// so there are no pages to pin and no host buffer to walk.
	if (request->type == NDMA_H2T_REQ_PA_MEMCPY) {
		if (current_op->cur_op_index == 1) {
			current_op->cur_op_index = -1; // reset index we're done w/ prev request and need a new one
			return true;
		}
		current_op->host_addr    = NULL;
		current_op->src_pa       = request->pa_memcpy.src_base;
		current_op->dev_addr     = request->pa_memcpy.dst_base;
		current_op->offset       = 0;
		current_op->remaining    = request->pa_memcpy.size;
		current_op->xfer_size    = min(current_op->remaining, NDMA_ZC_PA_XFER_MAX);
		current_op->last_op      = true;
		return false;
	}

	if (current_op->cur_op_index == request->memcpy.num_ops) {
		current_op->cur_op_index = -1; // reset index we're done w/ prev request and need a new one
		return true;
	}

	op = &request->memcpy.ops[current_op->cur_op_index];
	current_op->host_addr    = op->buffer;
	current_op->dev_addr     = request->memcpy.dev_base + op->offset;
	current_op->offset       = (unsigned long)current_op->host_addr & (PAGE_SIZE - 1);
	current_op->remaining    = op->size;
	current_op->xfer_size    = ndma_calc_zc_pin_size(current_op->remaining + current_op->offset);
	current_op->last_op      = current_op->cur_op_index == request->memcpy.num_ops - 1;
	return false;
}

/**
 * ndma_zca_dma_ioq_initiate_cleanup
 *
 *   notify the completion thread to cleanup any dma_ioq outstanding requests
 *   then wait for the thread to finish cleanup.  The cleanup flag also
 *   blocks any new requests/contexts from entering the sausage grinder
 *   which is why the cleanup flag is protected by the mutex.  It also prevents
 *   any further setting of the bitmap for this queue which serves as an interlock
 *   with the thread.
 */
static void ndma_zca_dma_ioq_initiate_cleanup(struct ndma_h2t_dma_ioq *dma_ioq)
{
	int bit = ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(dma_ioq->eng->eng_id, dma_ioq->qid);
	mutex_lock(&dma_ioq->ring->h2t_ring_lock);
	dma_ioq->cleanup = true;
	mutex_unlock(&dma_ioq->ring->h2t_ring_lock);

	while (atomic64_read(&dma_ioq->eng->nd->dma_cmpltn_thread.nonempty_ctxq_bitmap) & BIT_ULL(bit)) {
		msleep(10);
	}
}

/**
 * ndma_zca_discard_queued_request()
 *
 *   release a request and any per-type resources it holds. The request must not
 *   be on the queue (see ndma_zca_free_queued_request() for that case).
 */
static void ndma_zca_discard_queued_request(struct ndma_h2t_queued_request *request)
{
	switch (request->type) {
	case NDMA_H2T_REQ_DMA_MEMCPY:
		mmput(request->memcpy.mm);
		break;
	case NDMA_H2T_REQ_DESC_EXECUTE:
	case NDMA_H2T_REQ_BAR4_WR:
	case NDMA_H2T_REQ_PA_MEMCPY:
		break;
	}
	kvfree(request);
}

static void ndma_zca_free_queued_request(struct ndma_h2t_queued_request *request)
{
	list_del(&request->queuelist);
	ndma_zca_discard_queued_request(request);
}
/**
 * ndma_zca_cleanup_queued_requests()
 *
 *   cleanup any outstanding requests
 *   NOTE: only call from the completion thread
 */
static void ndma_zca_cleanup_queued_requests(struct ndma_h2t_dma_ioq *dma_ioq, bool discard_async)
{
	struct ndma_h2t_queued_request *request;

	if (current != dma_ioq->eng->nd->dma_cmpltn_thread.thread) {
		pr_err("ERROR: cleanup not called from completion thread");
	}

	while (!list_empty(&dma_ioq->queued_req_head)) {
		request = list_first_entry(&dma_ioq->queued_req_head, struct ndma_h2t_queued_request, queuelist);

		ndma_zca_complete_request(dma_ioq, request->sequence_num, -EIO, request->context, discard_async);
		ndma_zca_free_queued_request(request);
	}
}

/**
 * ndma_zca_push_user_descs()
 *
 *   Copy a user-supplied descriptor block into the next slots of one side
 *   (tx or rx) of the ring, rewriting the ring-id/phase-bit per descriptor.
 *   User-supplied ring-id bits are always overwritten because the target
 *   slot's phase-bit is a function of the ring's current position, not
 *   whatever the caller happened to put there.
 */
static int ndma_zca_push_user_descs(struct ndma_eng *eng, u32 qid,
                                    enum udma_type type,
                                    const union udma_desc *src_buf,
                                    u32 nr_desc)
{
	struct udma_q *q;
	u32 i;
	int ret;

	ret = udma_q_handle_get(&eng->udma, qid, type, &q);
	if (ret) {
		return ret;
	}

	for (i = 0; i < nr_desc; i++) {
		union udma_desc *slot = udma_desc_get(q);

		if (slot == NULL) {
			pr_err("nd%02d: q%d %s: no descriptor slot available at %u/%u\n",
			       eng->nd->device_index, qid,
			       (type == UDMA_TX) ? "tx" : "rx", i, nr_desc);
			return -ENOSPC;
		}
		*slot = src_buf[i];
		udma_ring_id_set(q, slot);
	}
	return 0;
}

/**
 * ndma_zca_build_n_issue_pa_descs()
 *
 *   Build and issue the descriptors for one step of a PA to PA transfer
 *   (NDMA_H2T_REQ_PA_MEMCPY).  Counterpart of ndma_build_n_issue_zc_descs() for
 *   contiguous transfers where both sides are already dma addresses: nothing is
 *   pinned and the descriptors come from the same chunking helper the legacy
 *   sync/async path uses.
 */
static int ndma_zca_build_n_issue_pa_descs(struct ndma_h2t_zcdma_context *dma_ctx)
{
	u64 bytes_issued;
	u32 nr_data_descs;
	int ret;

	ret = ndma_pa_build_n_issue_descs(dma_ctx->eng, dma_ctx->ring, dma_ctx->src_pa, dma_ctx->dev_addr,
	                                  dma_ctx->size, true, true, 0ull, NDMA_ZC_PA_DESCS_PER_XFER,
	                                  dma_ctx->completion_ptr, &bytes_issued, &nr_data_descs, dma_ctx->last);

	/* paired m2s/s2m descriptors, so tx and rx counts match. */
	dma_ctx->nr_tx_desc = nr_data_descs;
	dma_ctx->nr_rx_desc = nr_data_descs;
	if (ret) {
		return ret;
	}

	// the step size is capped at NDMA_ZC_PA_XFER_MAX so the descriptor budget always covers it
	if (bytes_issued != dma_ctx->size) {
		pr_err("nd%02d: PA transfer issued %llu of %llu bytes\n",
		       dma_ctx->eng->nd->device_index, bytes_issued, dma_ctx->size);
		return -EIO;
	}

	dma_ctx->state = NDMA_SUBMITTED;
	return 0;
}

/**
 * ndma_zca_process_desc_execute_request()
 *
 *   Push a caller-built pair of tx/rx descriptor blocks onto the ring,
 *   append our own completion desc pair, and register a ctx queue entry
 *   so the shared completion thread will retire the transfer through the
 *   sync / async completion paths.
 *
 *   Called with the ring lock held. Returns 0 on success (request has been
 *   consumed and freed), -EAGAIN when credits are insufficient (caller
 *   should retry later), or a negative errno on fatal error (request has
 *   been failed and freed).
 */
static int ndma_zca_process_desc_execute_request(struct ndma_h2t_dma_ioq *dma_ioq,
                                                 struct ndma_h2t_queued_request *request)
{
	struct ndma_h2t_zcdma_context *tail_ctx;
	u32 tx_need = request->desc_execute.nr_tx_desc + 1; // +1 for completion desc
	u32 rx_need = request->desc_execute.nr_rx_desc + 1;
	int ret;

	// 1 ctx slot to route completion.
	if (ndma_zca_ctx_q_avail(&dma_ioq->ctx_q) == 0) {
		return -EAGAIN;
	}

	if (!ndma_zca_ring_has_credit(dma_ioq, tx_need, rx_need)) {
		return -EAGAIN;
	}

	tail_ctx = ndma_ctx_queue_peek_tail(&dma_ioq->ctx_q);
	ndma_zca_dma_ctx_init_common(dma_ioq, request, tail_ctx);

	// Push user tx descs then rx descs into the ring.
	ret = ndma_zca_push_user_descs(dma_ioq->eng, dma_ioq->qid, UDMA_TX,
	                               request->desc_execute.tx_desc_buf,
	                               request->desc_execute.nr_tx_desc);
	if (ret) {
		goto fail;
	}
	ret = ndma_zca_push_user_descs(dma_ioq->eng, dma_ioq->qid, UDMA_RX,
	                               request->desc_execute.rx_desc_buf,
	                               request->desc_execute.nr_rx_desc);
	if (ret) {
		goto fail;
	}

	// Append our completion desc pair on top of the user's descs. In the future, caller
	// will pass the barrier request to the driver
	ret = ndma_memcpy_add_completion_desc(dma_ioq->eng, dma_ioq->ring, tail_ctx->completion_ptr, UDMA_M2M_BARRIER_SOW);
	if (ret) {
		goto fail;
	}

	// Kick the transfer (nr_*_desc + 1 accounts for our completion pair).
	ret = udma_m2m_copy_start(&dma_ioq->eng->udma, dma_ioq->qid, tx_need, rx_need);
	if (ret) {
		goto fail;
	}

	tail_ctx->direction  = true;
	tail_ctx->last       = true;
	tail_ctx->nr_pages   = 0;
	tail_ctx->nr_tx_desc = request->desc_execute.nr_tx_desc;
	tail_ctx->nr_rx_desc = request->desc_execute.nr_rx_desc;
	tail_ctx->state      = NDMA_SUBMITTED;
	ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);
	ndma_zca_free_queued_request(request);
	return 0;

fail:
	pr_err("nd%02d: desc_execute submission failed: %d\n",
	       dma_ioq->eng->nd->device_index, ret);
	tail_ctx->state     = NDMA_COMPLETED;
	tail_ctx->direction = true;
	tail_ctx->last      = true;
	tail_ctx->nr_pages  = 0;
	tail_ctx->compl_ret = ret;
	ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);
	ndma_zca_free_queued_request(request);
	return ret;
}

/** ndma_zca_retire_zero_len_op()
 *
 *   Retire a zero length op without building a transfer for it.
 *
 *   Such an op spends no credits and never advances, so leaving it to the submit loop
 *   would spin on it forever, and its nr_pages_max of 0 divides by zero in the credit
 *   math.  It is retired through the context queue rather than completed inline so
 *   that ordering, the last op flag and completion behave as they do for a real
 *   transfer.
 *
 *   Returns false if the context queue is full and the caller should retry later,
 *   true once the op is retired and the current op has been advanced.
 */
static bool ndma_zca_retire_zero_len_op(struct ndma_h2t_dma_ioq *dma_ioq,
                                        struct ndma_h2t_queued_request *request,
                                        struct ndma_h2t_queued_req_op_ctx *current_op)
{
	struct ndma_h2t_zcdma_context *tail_ctx;

	if (ndma_zca_ctx_q_avail(&dma_ioq->ctx_q) == 0) {
		return false;
	}

	tail_ctx = ndma_ctx_queue_peek_tail(&dma_ioq->ctx_q);
	ndma_zca_dma_ctx_init_common(dma_ioq, request, tail_ctx);
	tail_ctx->dev_addr  = current_op->dev_addr;
	tail_ctx->size      = 0;
	tail_ctx->direction = (request->type == NDMA_H2T_REQ_PA_MEMCPY) ? true : request->memcpy.direction;
	tail_ctx->last      = current_op->last_op;
	tail_ctx->nr_pages  = 0;
	tail_ctx->state     = NDMA_COMPLETED;
	tail_ctx->compl_ret = 0;
	ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);

	if (ndma_zca_dma_ioq_current_op_update(current_op, request)) {
		ndma_zca_free_queued_request(request);
	}
	return true;
}

/* ndma_zca_process_queued_requests()
 *
 *  - entered with mutex held
 *
 */
static int ndma_zca_process_queued_requests(struct ndma_h2t_dma_ioq *dma_ioq, u32 credit_limit, bool remote)
{
	int ret = 0;
	struct ndma_h2t_queued_request *request = NULL;
	struct ndma_h2t_queued_req_op_ctx *current_op = &dma_ioq->current_op;
	u32 credits;

	// if cleanup has been initiated, don't generate any more contexts
	if (dma_ioq->cleanup) {
		return 0;
	}

	// requests remain in the queue until their context queue entries are built and submitted
	// 
	while (!list_empty(&dma_ioq->queued_req_head)) {
		u32 ctx_credits;
		u32 pin_credits;
		u32 rng_credits;
		u32 nr_pages_max;
		u32 nr_descs_max;
		bool pa_copy;
		request = list_first_entry(&dma_ioq->queued_req_head, struct ndma_h2t_queued_request, queuelist);

		// check if we're processing a new request
		if (current_op->cur_op_index == -1) {
			// Bar4 writes simply get a completion queue entry marked completed
			//
			if (request->type == NDMA_H2T_REQ_BAR4_WR) {  // TODO: In theory individual OPs could be bar4
				struct ndma_h2t_zcdma_context *tail_ctx;

				if (ndma_zca_ctx_q_avail(&dma_ioq->ctx_q) == 0) {
					goto done;
				}
				tail_ctx = ndma_ctx_queue_peek_tail(&dma_ioq->ctx_q);
				ndma_zca_dma_ctx_init_common(dma_ioq, request, tail_ctx);
				tail_ctx->direction    = true;
				tail_ctx->last         = true;
				tail_ctx->nr_pages     = 0;
				tail_ctx->state        = NDMA_COMPLETED;
				tail_ctx->compl_ret    = request->bar4_wr.compl_ret;
				ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);
				ndma_zca_free_queued_request(request);
				continue;
			}
			if (request->type == NDMA_H2T_REQ_DESC_EXECUTE) {
				ret = ndma_zca_process_desc_execute_request(dma_ioq, request);
				if (ret == -EAGAIN) {
					// transient back-pressure: request stays on the
					// queue and the completion thread will retry it
					// once credits free up. Do not propagate to the
					// submitter (would trigger a spurious cleanup).
					ret = 0;
					goto done;
				}
				if (ret) {
					goto done; // fatal error; ctx entry already marked failed
				}
				continue;
			}
			// update the current op context for the new request
			ndma_zca_dma_ioq_current_op_update(current_op, request);
		}

		// a zero length op has no transfer to build, so retire it separately
		//
		if (current_op->remaining == 0) {
			if (!ndma_zca_retire_zero_len_op(dma_ioq, request, current_op)) {
				goto done;
			}
			continue;
		}

		// compute how many contexts we can create/process for this op (min of all available credits we need so we know when to stop)
		// it's possible we use less pin and ring credits so we could recompute in the loop
		//
		pa_copy = (request->type == NDMA_H2T_REQ_PA_MEMCPY);
		if (pa_copy) {
			// PA transfers pin nothing and are contiguous, so the descriptor count per
			// step is driven by the transfer size, not the page count.
			nr_descs_max = DIV_ROUND_UP(current_op->xfer_size, MAX_DMA_DESC_SIZE);
			pin_credits  = U32_MAX;
		} else {
			nr_pages_max = DIV_ROUND_UP(current_op->xfer_size, PAGE_SIZE);
			nr_descs_max = nr_pages_max;  // worst case: no two pinned pages are contiguous
			pin_credits  = ndma_zca_pinned_avail(dma_ioq, nr_pages_max);
		}
		ctx_credits  = ndma_zca_ctx_q_avail(&dma_ioq->ctx_q);
		rng_credits  = ndma_zca_ring_avail(dma_ioq, nr_descs_max);
		credits      = min(min(min(ctx_credits, pin_credits), rng_credits), credit_limit);

		if (credits == 0) {
			pr_info_once("no credits ctx: %d pin: %d rng %d limit %d\n", ctx_credits, pin_credits, rng_credits, credit_limit);
			break;
		}

		while (current_op->remaining && credits) {
			struct ndma_h2t_zcdma_context *tail_ctx;
			int nr_pages = pa_copy ? 0 : DIV_ROUND_UP(current_op->xfer_size, PAGE_SIZE);

			tail_ctx = ndma_ctx_queue_peek_tail(&dma_ioq->ctx_q);
			ndma_zca_dma_ctx_init_common(dma_ioq, request, tail_ctx);
			tail_ctx->host_addr     = current_op->host_addr;
			tail_ctx->src_pa        = current_op->src_pa;
			tail_ctx->dev_addr      = current_op->dev_addr;
			// First chunk may be unaligned; later chunks are page-aligned with offset=0.
			tail_ctx->size          = current_op->xfer_size - current_op->offset;
			tail_ctx->direction     = pa_copy ? true : request->memcpy.direction;
			tail_ctx->last          = (tail_ctx->size == current_op->remaining) && current_op->last_op;
			tail_ctx->nr_pages      = nr_pages;
			tail_ctx->state         = NDMA_UNPINNED; // TODO - could remove
			if (remote && !pa_copy) {
				tail_ctx->mm = request->memcpy.mm;
			}

		   	ret = pa_copy ? 0 : ndma_zerocopy_pin_pages(dma_ioq->eng->nd->device_index, dma_ioq->ring->h2t_nc_id, &dma_ioq->ctx_q, tail_ctx, remote); // TODO nc_id unused?
			if (ret) {
				// if memory resources are tight, drop out of the loop and retry later
				if (ret == -ENOMEM) {
					pr_warn_once("nd%02d: pinning retry encountered", dma_ioq->eng->nd->device_index);
					dma_ioq->pin_retries++;  // TODO need to have a way to report these
					ret = 0; // non-fatal - we will retry
				} else {
					pr_err("failed to pin pages for zero copy dma on nd %d\n", dma_ioq->eng->nd->device_index);  // TODO cache nd
					tail_ctx->state = NDMA_COMPLETED;
					tail_ctx->nr_pages = 0; // zero out page count since we never pinned
					tail_ctx->compl_ret = ret;
					ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);
					ndma_zca_free_queued_request(request);
				}
				goto done;
			}
			
			// process requests
			current_op->remaining -= tail_ctx->size;
			if (pa_copy) {
				current_op->src_pa    += tail_ctx->size;
			} else {
				current_op->host_addr += tail_ctx->size;
			}
			current_op->dev_addr  += tail_ctx->size;
			current_op->xfer_size  = (current_op->remaining < current_op->xfer_size) ? current_op->remaining : current_op->xfer_size;
			current_op->offset     = 0;

			ret = pa_copy ? ndma_zca_build_n_issue_pa_descs(tail_ctx) : ndma_build_n_issue_zc_descs(tail_ctx);
			if (ret) {
				ndma_zca_zerocopy_unpin_pages(tail_ctx, &dma_ioq->ctx_q.nr_pinned_pages);
				pr_err("failed to build and issue descs\n");
				tail_ctx->state = NDMA_COMPLETED;
				tail_ctx->nr_pages = 0; // zero out page count since there are no pinned pages
				tail_ctx->compl_ret = ret;
				ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);
				ndma_zca_free_queued_request(request);
				goto done;
			}

			tail_ctx->state = NDMA_SUBMITTED;
			ndma_ctx_queue_inc_tail(&dma_ioq->ctx_q);

			credits--;  // TODO we could recompute here   
			credit_limit--;	

			// if we are done w/ current op for this request, update to the next op. If there's no more ops
			// remove the current request and exit the loop.
			//
			if (current_op->remaining == 0) {
				if (ndma_zca_dma_ioq_current_op_update(current_op, request)) {
					ndma_zca_free_queued_request(request);
					break;
				}
			}
		}
	}

done:	
	// notify the thread that there's work to do. (right now even for sync (sequence_num == 0)
	if (!remote) {
		int bit = ndhal->ndhal_ndmar.ndmar_ctx_queue_bit(dma_ioq->eng->eng_id, dma_ioq->qid);
		atomic64_or(BIT_ULL(bit), &dma_ioq->eng->nd->dma_cmpltn_thread.nonempty_ctxq_bitmap); // set the bit for this queue
		 wake_up(&dma_ioq->eng->nd->dma_cmpltn_thread.wait_queue);
	}

	return ret;
}

/**
 * ndma_zca_request_submit()
 *
 *   Common tail end of the request submission paths.  Validates ring ownership,
 *   allocates a sync completion marker for sync submissions, hands the request
 *   over to the queue and drives queue processing.  For sync submissions
 *   (request->sequence_num == 0) it blocks until the transfer retires and
 *   returns its status; async submissions return as soon as the request is
 *   queued and report completion through the shared completion queue.
 *
 *   On entry the caller owns @request.  On return ownership has transferred to
 *   the queue, or the request has been released on the failure paths.
 */
static int ndma_zca_request_submit(struct neuron_device *nd, u32 nc_id, int qid,
                                   struct ndma_ring *ring, struct ndma_h2t_queued_request *request)
{
	struct ndma_h2t_dma_ioq *dma_ioq = ring->h2t_dma_ioq;
	const u64 sequence_num = request->sequence_num;
	u64 *sync_compl_marker = NULL;
	int ret;

	// Add the request to the request queue head then kick off queue processing
	//
	mutex_lock(&ring->h2t_ring_lock);

	if (!ndmar_h2t_ring_is_owner(ring, nc_id)) {
		pr_err("nd%02d: attempting to use qid %d that was not assigned to nc %d\n", nd->device_index, qid, nc_id);
		ret = -ENOENT;
		goto err_unlock;
	}

	if (sequence_num == 0) {
		sync_compl_marker = ndma_zca_sync_compl_alloc(dma_ioq);
		if (sync_compl_marker == NULL) {
			ret = -EINTR;
			goto err_unlock;
		}
		request->context = sync_compl_marker;
	}

	// if we are cleaning up, don't keep stuffing the sausage grinder do this after the sequence_num
	// marker acquire because it does a lock/unlock spin
	//
	if (dma_ioq->cleanup) {
		if (sync_compl_marker != NULL) {
			ndma_zca_sync_compl_free(dma_ioq, sync_compl_marker);
		}
		ret = -EIO;
		goto err_unlock;
	}

	// once added to the list the processing code owns and is responsible for the request
	//
	list_add_tail(&request->queuelist, &dma_ioq->queued_req_head);  // TODO COULD HAVE A FLAG FOR SYNC REQ TO INSERT AT FRONT+1

	ret = ndma_zca_process_queued_requests(dma_ioq, NDMA_ZC_SUBMIT_CREDIT_LIMIT, false);

	mutex_unlock(&ring->h2t_ring_lock);

	if (ret) {
		pr_err("nd%02d: error queuing request. %d", nd->device_index, ret);
		ndma_zca_dma_ioq_initiate_cleanup(dma_ioq);
	}

	if (sequence_num == 0) {
		/* wait_event_killable only unblocks on fatal signals (SIGKILL or a
		 * thread-group already flagged for exit).  Benign signals — SIGINT,
		 * SIGCHLD, SIGALRM, SIGWINCH, etc. — are left to be delivered on the
		 * next return to userspace and do NOT tear down the shared queue.
		 * On a real termination we still must drain: the completion thread
		 * would otherwise write into sync_compl_marker's pool slot after we
		 * free it, corrupting whichever sync waiter reallocates the slot. */
		ret = wait_event_killable(dma_ioq->sync_wait_queue, NDMA_ZC_SYNC_CHK_COMPL_MARKER(sync_compl_marker));
		if (ret) {
			pr_err("nd%02d: sync io request aborted by fatal signal. Initiating cleanup", nd->device_index);
			ndma_zca_dma_ioq_initiate_cleanup(dma_ioq);
		}
		ret = NDMA_ZC_SYNC_COMPL_MARKER_RET(sync_compl_marker);
		ndma_zca_sync_compl_free(dma_ioq, sync_compl_marker);
	}

	return ret;

err_unlock:
	mutex_unlock(&ring->h2t_ring_lock);
	ndma_zca_discard_queued_request(request);
	return ret;
}

/**
 * ndma_zca_zerocopy_submit()
 *
 */
int ndma_zca_zerocopy_submit(struct neuron_device *nd,
						u32 nc_id,
						const nrt_tensor_batch_op_t *ops,
						u32 num_ops,
						dma_addr_t dev_base,
						int qid,
						bool direction,
						bool bar4_wr,
						u64 sequence_num,
						void *context)
{
	int ret = 0;
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_eng *eng = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring = &queue->ring_info;
	struct ndma_h2t_queued_request *request = NULL;

	ret = ndma_h2d_create_cmpltn_thread(nd);
	if (ret) {
		return ret;
	}

	// TODO for batch, need ncdev to allocate because it needs to does op vetting
	// allocate request and copy request info to it.
	request = kvzalloc(sizeof(*request) + num_ops * sizeof(*ops), GFP_KERNEL);
	if (request == NULL) {
		return -ENOMEM;
	}

	request->type         = bar4_wr ? NDMA_H2T_REQ_BAR4_WR : NDMA_H2T_REQ_DMA_MEMCPY;
	request->sequence_num = sequence_num;
	request->context      = (request->sequence_num == 0) ? NULL : context;
	request->pid          = task_tgid_nr(current);

	if (!bar4_wr) {
		struct mm_struct *mm = current->mm;

		/* The completion kthread pins pages against this mm long after this
		 * ioctl returns, so pin the mm's lifetime here. Use mmget_not_zero:
		 * if the owning process is already exiting, fail the submit rather
		 * than queue work against a dying mm. */
		if (!mm || !mmget_not_zero(mm)) {
			kvfree(request);
			return -ESRCH;
		}
		memcpy(request->memcpy.ops, ops, num_ops * sizeof(*ops));
		request->memcpy.num_ops   = num_ops;
		request->memcpy.dev_base  = dev_base;
		request->memcpy.direction = direction;
		request->memcpy.mm        = mm;
	}

	return ndma_zca_request_submit(nd, nc_id, qid, ring, request);
}

/**
 * ndma_zca_mc_copy_available()
 *
 */
bool ndma_zca_mc_copy_available(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc)
{
	const u32 nc_id  = ndma_mc_pair_to_nc(src_mc, dst_mc);
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	const int qid    = ndhal->ndhal_ndmar.ndmar_get_h2t_def_qid(nc_id);
	struct ndma_ring *ring = &nd->ndma_engine[eng_id].queues[qid].ring_info;

	// the pipeline has no equivalent of the legacy path's reset window DMA retry, so
	// architectures that need the retry keep using the legacy path.
	if (!ndma_zerocopy_supported()) {
		return false;
	}

	// requests are queued on the h2t ring of the nc that owns it. If this nc doesn't own
	// the ring there's no queue to submit to, so the caller falls back to the legacy path.
	return ndmar_h2t_ring_is_owner(ring, nc_id) && (ring->h2t_dma_ioq != NULL);
}

/**
 * ndma_zca_mc_copy_submit()
 *
 */
int ndma_zca_mc_copy_submit(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
                            u64 src_offset, u64 dst_offset, u64 size, int qid, u64 sequence_num, void *context)
{
	const u32 nc_id  = ndma_mc_pair_to_nc(src_mc, dst_mc);
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_ring *ring = &nd->ndma_engine[eng_id].queues[qid].ring_info;
	struct ndma_h2t_queued_request *request = NULL;
	int ret;

	// a zero sized transfer has no op to build a context from, so it would sit on the
	// request queue forever. Nothing to do, so complete it here.
	if (size == 0) {
		return 0;
	}

	ret = ndma_h2d_create_cmpltn_thread(nd);
	if (ret) {
		return ret;
	}

	request = kvzalloc(sizeof(*request), GFP_KERNEL);
	if (request == NULL) {
		return -ENOMEM;
	}

	request->type                = NDMA_H2T_REQ_PA_MEMCPY;
	request->sequence_num        = sequence_num;
	request->context             = (sequence_num == 0) ? NULL : context;
	request->pid                 = task_tgid_nr(current);
	request->pa_memcpy.src_base  = ndma_mc_to_pa(src_mc) + src_offset;
	request->pa_memcpy.dst_base  = ndma_mc_to_pa(dst_mc) + dst_offset;
	request->pa_memcpy.size      = size;

	return ndma_zca_request_submit(nd, nc_id, qid, ring, request);
}

/**
 * ndma_zca_desc_execute_submit()
 *
 *   Queue a pair of pre-built H2D DMA descriptor blocks copied in from
 *   userspace and drive them through the shared queued-request pipeline.
 *   Behaves like ndma_zca_zerocopy_submit() but skips page pinning and
 *   descriptor construction: the descriptors are supplied by the caller.
 *   sequence_num == 0 selects the sync flow (blocks and returns status);
 *   non-zero selects the async flow (returns immediately, completion is
 *   posted to the shared CQ with sequence_num/context echoed back).
 */
int ndma_zca_desc_execute_submit(struct neuron_device *nd,
                                 u32 nc_id,
                                 int qid,
                                 void __user *tx_descs_user,
                                 u64 tx_descs_bytes,
                                 void __user *rx_descs_user,
                                 u64 rx_descs_bytes,
                                 u64 sequence_num,
                                 void *context)
{
	int ret = 0;
	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	struct ndma_eng *eng = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring = &queue->ring_info;
	struct ndma_h2t_queued_request *request = NULL;
	u32 nr_tx, nr_rx;
	const u32 max_side = DMA_H2T_DESC_COUNT - NDMA_SYNC_XFER_OUTSTANDING_MAX - 1;

	// Validate sizes: non-zero, aligned to descriptor size, and small enough
	// that a single request + our completion pair fits after the sync reservation.
	if (tx_descs_bytes == 0 || rx_descs_bytes == 0 ||
	    !IS_ALIGNED(tx_descs_bytes, sizeof(union udma_desc)) ||
	    !IS_ALIGNED(rx_descs_bytes, sizeof(union udma_desc))) {
		return -EINVAL;
	}
	nr_tx = tx_descs_bytes / sizeof(union udma_desc);
	nr_rx = rx_descs_bytes / sizeof(union udma_desc);
	if (nr_tx > max_side || nr_rx > max_side) {
		return -E2BIG;
	}

	ret = ndma_h2d_create_cmpltn_thread(nd);
	if (ret) {
		return ret;
	}

	request = kvzalloc(sizeof(*request) + (nr_tx + nr_rx) * sizeof(union udma_desc), GFP_KERNEL);
	if (!request) {
		return -ENOMEM;
	}
	request->type                       = NDMA_H2T_REQ_DESC_EXECUTE;
	request->sequence_num               = sequence_num;
	request->context                    = (sequence_num == 0) ? NULL : context;
	request->pid                        = task_tgid_nr(current);
	request->desc_execute.tx_desc_buf   = &request->desc_execute.descs[0];
	request->desc_execute.rx_desc_buf   = &request->desc_execute.descs[nr_tx];
	request->desc_execute.nr_tx_desc    = nr_tx;
	request->desc_execute.nr_rx_desc    = nr_rx;

	if (copy_from_user(request->desc_execute.tx_desc_buf, tx_descs_user, tx_descs_bytes) ||
	    copy_from_user(request->desc_execute.rx_desc_buf, rx_descs_user, rx_descs_bytes)) {
		ret = -EFAULT;
		goto err;
	}

	// Ownership of the request (and its desc buffers) transfers to the queue.
	return ndma_zca_request_submit(nd, nc_id, qid, ring, request);

err:
	kvfree(request);
	return ret;
}

