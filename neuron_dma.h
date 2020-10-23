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
		   u32 src_offset, u32 dst_offset, u32 size);

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
int ndma_memcpy_buf_to_mc(struct neuron_device *nd, void *buffer, u32 src_offset,
			  struct mem_chunk *dst_mc, u32 dst_offset, u32 size);

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
int ndma_memcpy_buf_from_mc(struct neuron_device *nd, void *buffer, u32 dst_offset,
			    struct mem_chunk *src_mc, u32 src_offset, u32 size);

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
int ndma_memcpy_dma_copy_descriptors(struct neuron_device *nd, void *buffer, u32 src_offset,
				     struct mem_chunk *dst_mc, u32 dst_offset, u32 size,
				     u32 queue_type);

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
int ndma_memcpy(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u32 size);

/**
 * ndma_memcpy_wait_for_completion() - Wait for already initiated DMA transfer to complete.
 *
 * @eng: dma engine
 * @ring: dma ring
 * @count: number of descriptors to wait for.
 *
 * Return: 0 if wait succeeds, a negative error code otherwise.
 */
int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count);

#endif
