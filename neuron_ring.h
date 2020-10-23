// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_RING_H
#define NEURON_RING_H

#include "udma/udma.h"
#include "v1/tdma.h"
#include "v1/address_map.h"

#define DMA_ENG_IDX_H2T 2
#define DMA_H2T_DESC_COUNT 4096
#define MAX_DMA_RINGS 16

#define NUM_DMA_ENG_PER_DEVICE (V1_NC_PER_DEVICE * V1_DMA_ENG_PER_NC)

struct neuron_device;
struct neuron_dma_eng_state;
struct neuron_dma_queue_state;

struct ndma_ring {
	u32 qid;
	u32 size; //total size - num desc * desc size
	bool has_compl;
	struct udma_ring_ptr tx;
	struct udma_ring_ptr rx;
	struct udma_ring_ptr rxc;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rx_mc;
	struct mem_chunk *rxc_mc;
	u32 dram_channel;
};

struct ndma_queue {
	struct ndma_ring ring_info;
	u32 eng_id;
	u32 qid;
	bool in_use;
};

struct ndma_eng {
	struct mutex lock;
	struct neuron_device *nd;
	u32 eng_id;
	struct ndma_queue queues[DMA_MAX_Q_MAX];
	struct udma udma;
	bool used_for_h2t;
	struct mutex h2t_ring_lock;
};

/**
 * ndmar_init() - Initialize DMA structures for given neuron device
 *
 * @nd: Neuron device to initialize
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int ndmar_init(struct neuron_device *nd);

/**
 * ndmar_close() - Close and cleanup DMA for given neuron device
 *
 * @nd: Neuron device to cleanup
 *
 * Return: 0 if cleanup succeeds, a negative error code otherwise.
 */
void ndmar_close(struct neuron_device *nd);

/**
 * ndmar_eng_init() - Initialize a DMA engine
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index to initialize
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int ndmar_eng_init(struct neuron_device *nd, int eng_id);

/**
 * ndmar_eng_set_state() - Change DMA engine's state
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index which state needs to be changed
 * @state: New state to set
 *
 * Return: 0 if state is successfully changed, a negative error code otherwise.
 */
int ndmar_eng_set_state(struct neuron_device *nd, int eng_id, u32 state);

/**
 * ndmar_queue_init() - Initialize a DMA queue.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index which needs to be initialized
 * @tx_desc_count: Total TX descriptors to allocate
 * @rx_desc_count: Total RX descriptors to allocate
 * @tx_mc: Memory chunk backing TX queue
 * @rx_mc: Memory chunk backing RX queue
 * @rxc_mc: Memory chunk backing RX completion queue
 * @port: AXI port.
 *
 * Return: 0 if queue init succeeds, a negative error code otherwise.
 */
int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, u32 port);

/**
 * ndmar_queue_release() - Release a DMA queue.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index which needs to be released
 *
 * Return: 0 if queue release succeeds, a negative error code otherwise.
 */
int ndmar_queue_release(struct neuron_device *nd, u32 eng_id, u32 qid);

/**
 * ndmar_queue_copy_start() - Start DMA transfer.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index which needs to be released
 * @tx_desc_count: Number of Tx descriptors to transfer
 * @rx_desc_count: Number of Rx descriptors to transfer
 *
 * Return: 0 if DMA copy succeeds, a negative error code otherwise.
 */
int ndmar_queue_copy_start(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
			   u32 rx_desc_count);

/**
 * ndmar_ack_completed() - Ack completed descriptor.
 *
 * After doing DMA transfer, the number of descriptors completed needs to acknowledged so that
 * the descriptors can be reused.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index where the DMA transfers were done
 * @count: Number descriptors to ack.
 *
 * Return: 0 if queue release succeeds, a negative error code otherwise.
 */
int ndmar_ack_completed(struct neuron_device *nd, u32 eng_id, u32 qid, u32 count);

/**
 * ndmar_queue_get_descriptor_mc() - Get backing memory chunk info.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index
 * @tx: Buffer to store TX mc
 * @rx: Buffer to store RX mc
 * @tx_size: Buffer to store tx descriptor count
 * @rx_size: Buffer to store rx descriptor count
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int ndmar_queue_get_descriptor_mc(struct neuron_device *nd, u8 eng_id, u8 qid,
				  struct mem_chunk **tx, struct mem_chunk **rx, u32 *tx_size,
				  u32 *rx_size);

/**
 * ndmar_eng_get_state() - Get DMA engine's current state.
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index
 * @state: Current hardware state will be updated here
 *
 * Return: 0 on success, a negative error code otherwise
 */
int ndmar_eng_get_state(struct neuron_device *nd, int eng_id, struct neuron_dma_eng_state *state);

/**
 * ndmar_queue_get_state() - Get current state of the Tx and Rx queue.
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index
 * @qid: DMA queue index
 * @tx: TxQueue state will be set here.
 * @rx: RxQueue state will be set here.
 *
 * Return: 0 on success, a negative error code otherwise
 */
int ndmar_queue_get_state(struct neuron_device *nd, int eng_id, int qid,
			  struct neuron_dma_queue_state *tx, struct neuron_dma_queue_state *rx);

#endif
