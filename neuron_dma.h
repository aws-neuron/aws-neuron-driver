// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DMA_H
#define NEURON_DMA_H

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
int ndma_memcpy_add_completion_desc( struct ndma_eng *eng, struct ndma_ring *ring, void * completion_buffer);

/**
 * Wait for completion by start transfer of a DMA between two host memory locations and polling
 * on the host memory for the data to be written.
 */
int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count, void * ptr, bool async, bool is_d2d);

/**
 * ndma_memcpy_zero_copy_mc()
 *
 *   Wrapper around ndma_memcpyzero_copy() that pulls nc_id and device phyical address from
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
int ndma_memcpy_zero_copy_mc( struct neuron_device *nd,  void * host_addr, struct mem_chunk *dev_mc, u64 dev_offset, u64 size, bool direction);
#endif
