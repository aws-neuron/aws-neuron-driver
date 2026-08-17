// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DMA_H
#define NEURON_DMA_H

#include <linux/mm_types.h>

#include "udma/udma.h"

#include "neuron_mempool.h"
#include "neuron_ring.h"

struct neuron_device;

#define DMA_COMPLETION_MARKER_SIZE sizeof(u32)

/**
 * ndma_memcpy_mc() - Copy data from a memory to another memory chunk.
 *
 * @nd: neuron device which should be used for dma
 * @src_mc: source memory chunk from which data should be copied
 * @dst_mc: destination memory chunk to which data should be copied
 * @src_offset: offset in the source from where copy should start
 * @dst_offset: offset in the destination
 * @size: copy size
 *
 * Return: 0 if copy succeeds, a negative error code otherwise.
 */
int ndma_memcpy_mc(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u64 src_offset, u64 dst_offset, u64 size);

/**
 * ndma_memcpy_mc_async() - Copy data from a memory to another memory chunk.
 *
 * @nd: neuron device which should be used for dma
 * @src_mc: source memory chunk from which data should be copied
 * @dst_mc: destination memory chunk to which data should be copied
 * @src_offset: offset in the source from where copy should start
 * @dst_offset: offset in the destination
 * @size: copy size
 * @prefetch_addr: address to prefetch for device to host copies
 * @pdma_ctx_handle: context handle for the previous dma
 * @dma_ctx_handle: context handle for this dma
 *
 * Return: 0 
 */
int ndma_memcpy_mc_async(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u64 src_offset, u64 dst_offset, u64 size, u64 prefetch_addr, int pdma_ctx_handle, int *dma_ctx_handle);


/**
 * ndma_memcpy_buf_to_mc() - Copyin data from given buffer to a memory chunk.
 *
 * @nd: neuron device which should be used for dma
 * @buffer: source buffer from which data should be copied
 * @dst_mc: destination memory chunk to which data should be copied
 * @src_offset: offset in the source from where copy should start
 * @dst_offset: offset in the destination
 * @size: copy size
 *
 * Return: 0 if copy succeeds, a negative error code otherwise.
 */
int ndma_memcpy_buf_to_mc(struct neuron_device *nd, void *buffer, u64 src_offset,
			  struct mem_chunk *dst_mc, u64 dst_offset, u64 size);

/**
 * ndma_memcpy_buf_from_mc() - Copyout data from given buffer to a memory chunk.
 *
 * @nd: neuron device which should be used for dma
 * @src_mc: source memory chunk from which data should be copied
 * @buffer: destination buffer
 * @src_offset: offset in the source from where copy should start
 * @dst_offset: offset in the destination
 * @size: copy size.
 *
 * Return: 0 if copy succeeds, a negative error code otherwise.
 */
int ndma_memcpy_buf_from_mc(struct neuron_device *nd, void *buffer, u64 dst_offset,
			    struct mem_chunk *src_mc, u64 src_offset, u64 size);

/**
 * ndma_memcpy_dma_copy_descriptors() - Copy dma descriptors to mc which is backing a dma queue.
 *
 * @nd: neuron device which should be used for dma
 * @buffer: source buffer which contains the dma descriptors
 * @queue_type: dma queue type(tx or rx)
 * @dst_mc: mc which backs the dma queue
 * @offset: offset in the queue.
 * @size: copy size.
 *
 * Return: 0 if copy succeeds, a negative error code otherwise.
 */
int ndma_memcpy_dma_copy_descriptors(struct neuron_device *nd, void *buffer, u64 src_offset,
				     struct mem_chunk *dst_mc, u64 dst_offset, u64 size,
				     u32 queue_type);

/**
 * ndma_memset() - fills the size bytes at offset of the memory area
 * pointed to by mc with the constant byte value
 *
 * @nd: neuron device which should be used for dma
 * @mc: memory chunk that needs to be set with the value
 * @offset: start offset in the chunk
 * @value: byte value to set to
 * @size: number of bytes to set to
 *
 * Return: 0 if copy succeeds, a negative error code otherwise.
 */
int ndma_memset(struct neuron_device *nd, struct mem_chunk *mc, u64 offset, u32 value, u64 size);

/**
 * ndma_memcpy() - Copy data from one physical address to another physical address.
 *
 * @nd: neuron device which should be used for dma
 * @nc_id: neuron core index(determines which dma engine to use for the transfer)
 * @src: source address in the neuron core
 * @dst: destination address in the neuron core
 * @size: copy size.
 *
 * Return: 0 if copy succeeds, a negative error code otherwise.
 */
int ndma_memcpy(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u64 size);


/**
 * ndma_memcpy_mc_wait() - wait for an asynchronous memcpy to complete
 *
 * @nd: neuron device which was used for this dma
 * @src_mc: source mem check for dma we are waiting on
 * @dst_mc: destination mem chunk for dma we are waiting on. 
 * @dma_ctx_handle: handle to the dma context we want to wait on
 *
 */
int ndma_memcpy_mc_wait( struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc, int dma_ctx_handle);

/** 
 * ndma_is_valid_host_mem() - check whether given PA is valid host memory.
 *                          - a PA is valid only if it is allocated by the current process.
 *
 * @return: True if PA is valid, false otherwise.
 */
bool ndma_is_valid_host_mem(struct neuron_device *nd, phys_addr_t pa);

/**
 * ndma_bar0_blocked_one_engine()
 * 
 * @param base 
 * @param off 
 * @return int 
 */
int ndma_bar0_blocked_one_engine(u64 base, u64 off);

/**
 * ndma_memcpy64k - create one descriptor (size must be <= 64K)
 * 
 */

int ndma_memcpy64k(struct ndma_eng *eng, struct ndma_ring *ring, dma_addr_t src,
			  dma_addr_t dst, u32 size, int barrier_type);

/*
 * ndma_memcpy_add_completion_desc()
 *
 *    add a completion entry to the ring 
 *
 */
int ndma_memcpy_add_completion_desc( struct ndma_eng *eng, struct ndma_ring *ring, void * completion_buffer, int barrier_type);

/**
 * Wait for completion by start transfer of a DMA between two host memory locations and polling
 * on the host memory for the data to be written.
 */
int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count, void * ptr, bool async, bool is_d2d);

/**
 * ndma_mc_pair_to_nc - Resolve the neuron core id for two memory chunks.
 * @src_mc: Source memory chunk participating in the transfer.
 * @dst_mc: Destination memory chunk participating in the transfer.
 *
 * Returns the NC identifier that owns the DMA engine, favoring the device-side
 * chunk when one side resides in host memory.
 */
u32 ndma_mc_pair_to_nc(struct mem_chunk *src_mc, struct mem_chunk *dst_mc);

/**
 * ndma_mc_to_pa - Translate a memory chunk into a DMA-usable physical address.
 * @mc: Memory chunk to translate.
 *
 * Host chunks map through the PCI host BAR, while device chunks already carry
 * their physical base address.
 */
dma_addr_t ndma_mc_to_pa(struct mem_chunk *mc);

/**
 * ndma_zerocopy_supported - Check whether zero-copy DMA is permitted.
 *
 * Architectures that require DMA retry disable the zero-copy pipeline.
 */
bool ndma_zerocopy_supported(void);

/**
 * ndma_zerocopy_submit() - Perform a pipelined zero-copy DMA transfer.
 * @nd: Neuron device whose DMA engine is used.
 * @nc_id: Neuron core identifier owning the queue.
 * @ops: Array of host buffer descriptors.
 * @num_ops: Number of descriptors in @ops.
 * @dev_base: Base device physical address for the transfer.
 * @qid: Queue identifier to submit descriptors on.
 * @direction: true for host-to-device, false for device-to-host.
 * @sequence_num: sequence number under async submission; 0 for sync.
 * @context: opaque context pointer returned in the completion queue for async submissions.
 *
 *   DMA data between a user space virtual address range and a contiguous location in device memory.
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
 *   ## For sync mode ##
 *   We enqueue DMA contexts into a fixed-size queue and drive submission from that queue.
 *   The loop:
 *      lock()
 *      while data remains
 *         submit any pinned-but-unsubmitted ctxs when descriptors are available
 *         create a new ctx when queue space and pin budget allow
 *         pin pages immediately for the ctx
 *         advance queue tail and update host/device pointers
 *         wait on submitted ctxs from the head as needed to relieve pressure
 *      submit remaining pinned ctxs
 *      drain submitted ctxs from the head
 *      unlock()
 *
 *   ## For async mode ##
 *   We keep submitting dma contexts until we hit a threshold of pinned pages. 
 *   Once we hit the threshold, we stop pinning pages and set the mm_struct for remote pinning later.
 *   TODO: liulily to add more detail once the complete async path is implemented.
 * 
 *  Notes:
 *    unpinning responsibilities. Up until a dma is successfully launched, this routine is responsible for unpinning
 *    host memory.  After that ndma_zerocopy_wait_for_completion() owns responsibility for unpinning pages.
 *
 *    We don't do this here, but pinning user pages across system (IOCTL) calls has a number of additional requirements.
 *    We would have to cleanup any pinned pages when the process goes away, so any pinned pages have to get tracked in 
 *    process context.
 *
 */
int ndma_zerocopy_submit(struct neuron_device *nd,
						u32 nc_id,
						const nrt_tensor_batch_op_t *ops,
						u32 num_ops,
						dma_addr_t dev_base,
						int qid,
						bool direction,
						u64 sequence_num,
						void *context);

/**
 * ndma_zerocopy_submit_completed() - Enqueue completed work for ordered async CQE.
 *
 * Used only for async BAR4 writes and temporary fake-async D2D copy, where the work
 * has already completed before we enqueue a dummy context.
 *
 * @nd: Neuron device.
 * @nc_id: NeuronCore id that owns the H2D queue.
 * @qid: H2D queue id.
 * @sequence_num: Async sequence number returned to userspace.
 * @compl_ret: Completion result to report in the CQE.
 * @context: Userspace completion context to copy into the CQE.
 *
 * Return: 0 if queued, negative errno otherwise.
 */
int ndma_zerocopy_submit_completed(struct neuron_device *nd, u32 nc_id, int qid,
				   u64 sequence_num, s64 compl_ret, void *context);

/**
 * Pre-pinned host memory support
 *
 * Allows userspace to pin host memory once and reuse it for multiple
 * DMA transfers without the overhead of pinning/unpinning on each transfer.
 * Uses VA as the lookup key - zerocopy operations auto-detect pinned memory.
 */

/**
 * ndma_pinned_mem_destroy() - Cleanup pinned memory tracking subsystem
 */
void ndma_pinned_mem_destroy(void);

/**
 * ndma_pin_host_memory() - Pin host memory for accelerated DMA operations
 * @va: User virtual address to pin
 * @size: Size of memory to pin
 *
 * Pins host memory so zerocopy operations auto-detect pinned regions
 * and skip per-transfer pinning. Uses fast path (pin_user_pages_fast)
 * first, then falls back to slow path (pin_user_pages with mmap_lock)
 * if needed.
 *
 * Return: 0 on success, -EEXIST if already pinned, negative errno on failure
 */
int ndma_pin_host_memory(u64 va, u64 size, u64 *pa_out);

/**
 * ndma_unpin_host_memory() - Unpin previously pinned host memory
 * @va: VA that was used in ndma_pin_host_memory (exact match required)
 *
 * Return: 0 on success, -ENOENT if not found, -EPERM if not owner
 */
int ndma_unpin_host_memory(u64 va);

/**
 * ndma_pinned_mem_cleanup_process() - Cleanup all pinned memory for a process
 * @pid: Process ID to cleanup
 */
void ndma_pinned_mem_cleanup_process(pid_t pid);

#endif
