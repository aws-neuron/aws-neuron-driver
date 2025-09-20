// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_RING_H
#define NEURON_RING_H

#include "udma/udma.h"

#define DMA_H2T_DESC_COUNT 4096
#define NUM_DMA_ENG_PER_DEVICE 132 // for v2 2 nc with each 16,

#define NDMA_QUEUE_DUMMY_RING_DESC_COUNT 64
#define NDMA_QUEUE_DUMMY_RING_SIZE (NDMA_QUEUE_DUMMY_RING_DESC_COUNT * sizeof(union udma_desc))

extern int nc_per_dev_param;

struct neuron_device;
struct neuron_dma_eng_state;
struct neuron_dma_queue_state;

/*
 * dma context for both sync and async DMA operations
 *    the context keeps transient and 
 *
 * this needs to be encapsulated in the DMA module
 */
struct ndma_h2t_dma_context {
	bool              inuse;              //
	struct ndma_eng  *eng;                //
	struct ndma_ring *ring;               //
	dma_addr_t        src;                // original src
	dma_addr_t        dst;                // original dst
	u64               size;               // original size 
	bool              smove;              //
	bool              dmove;              //
	u64               start_time;         // start time for this transfer
	u64               offset;             // initial offset for this transfer
	u64               remaining;          // initial remaining for this transfer
	u64               outstanding;        // outstanding data for this transfer - used to compute wait time. (vs pending)
	int               pending_transfers;  // pending transfers for this context.  Used to update ring ptrs
	void             *completion_ptr;     // completion buffer pointer (host memory buffer we poll on for completions)
};

struct ndma_ring {
	u32 qid;
	u32 size; //total size - num desc * desc size
	bool has_compl;
	struct udma_ring_ptr tx;
	struct udma_ring_ptr rx;
	struct udma_ring_ptr rxc;
	struct udma_ring_ptr h2t_completion;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rx_mc;
	struct mem_chunk *rxc_mc;
	struct mem_chunk *h2t_completion_mc;
	struct ndma_h2t_dma_context h2t_dma_ctx[NEURON_DMA_H2T_CTX_HANDLE_CNT];
	u32 dram_channel;
};

struct ndma_queue {
	struct ndma_ring ring_info;
	u32 eng_id;
	u32 qid;
	pid_t owner; // process which initialized this queue.
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
 * ndmar_acquire_engine() - acquire the DMA engine
 * 
 * @param nd: neuron device
 * @param eng_id: DMA engine ID
 * @return struct ndma_eng*: the DMA engine to be acquired
 */
struct ndma_eng *ndmar_acquire_engine(struct neuron_device *nd, u32 eng_id);

/**
 * ndmar_release_engine() - release the DMA engine
 * 
 * @param eng: the DMA engine to be released
 */
void ndmar_release_engine(struct ndma_eng *eng);

/**
 * ndmar_init() - Initialize DMA structures for given neuron device
 *
 * @nd: Neuron device to initialize
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int ndmar_init(struct neuron_device *nd);

/**
 * ndmar_init_ncs() - Initialize DMA structures for given neuron core
 *
 * @nd:     Neuron device to initialize
 * @nc_map: Neuron core to initialize (NEURON_NC_MAP_DEVICE for entire device)
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int ndmar_init_ncs(struct neuron_device *nd, uint32_t nc_map);

/**
 * ndmar_close() - Close and cleanup DMA for given neuron device
 *
 * @nd: Neuron device to cleanup
 *
 * Return: 0 if cleanup succeeds, a negative error code otherwise.
 */
void ndmar_close(struct neuron_device *nd);

/**
 * ndmar_close_ncs() - Close and cleanup DMA for given neuron core
 *
 * @nd:     Neuron device to cleanup
 * @nc_map: Neuron core to cleanup (NEURON_NC_MAP_DEVICE for entire device)
 *
 * Return: 0 if cleanup succeeds, a negative error code otherwise.
 */
void ndmar_close_ncs(struct neuron_device *nd, uint32_t nc_map);

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
 * ndmar_preinit() - Initialize all DMA engines for neuron device
 *
 * @param nd: Neuron device which contains the DMA engine
 */
void ndmar_preinit(struct neuron_device *nd);

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
 * @allocatable: whether new descriptors can be added post queue init
 *
 * Return: 0 if queue init succeeds, a negative error code otherwise.
 */
int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, u32 port, bool allocatable);

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
 * ndmar_handle_process_exit() - Stops all the queues used by the given process.
 *
 * This function should be called when a process exits(before the MCs are freed),
 * so that the DMA engines used can be reset and any ongoing DMA transaction can be
 * stopped.
 *
 * @nd: Neuron device
 * @pid: Process id.
 */
void ndmar_handle_process_exit(struct neuron_device *nd, pid_t pid);

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
 * ndmar_queue_get_descriptor_info() - Get backing physical address info, len, mcs
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index
 * @tx_mc: Buffer to store TX mc
 * @rx_mc: Buffer to store RX mc
 * @tx_pa: Buffer to store TX device side physical address
 * @rx_pa: Buffer to store RX device side physical address
 * @tx_size: Buffer to store tx descriptor count
 * @rx_size: Buffer to store rx descriptor count
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int ndmar_queue_get_descriptor_info(struct neuron_device *nd, u8 eng_id, u8 qid,
				  struct mem_chunk **tx_mc, struct mem_chunk **rx_mc, u64 *tx_pa, u64 *rx_pa, 
				  u32 *tx_size, u32 *rx_size);

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

/** ndmar_h2t_ring_init() - initialize a DMA ring 
 * 
 * @eng_id: DMA engine index
 * @qid: DMA queue index
 * 
 * Return: 0 on success, a negative error code otherwise
 */
int ndmar_h2t_ring_init(struct ndma_eng *eng, int qid);

u32 ndmar_ring_get_desc_count(u32 v);

#endif
