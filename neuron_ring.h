// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_RING_H
#define NEURON_RING_H

#include <linux/atomic.h>

#include "udma/udma.h"
#include "share/neuron_driver_shared.h"

#define DMA_H2T_DESC_COUNT 4096
#define NUM_DMA_ENG_PER_DEVICE 132 // for v2 2 nc with each 16,
// Max H2D DMA engines per device
#define NUM_H2D_DMA_PER_DEVICE 8

#define NDMA_QUEUE_DUMMY_RING_DESC_COUNT 64
#define NDMA_QUEUE_DUMMY_RING_SIZE (NDMA_QUEUE_DUMMY_RING_DESC_COUNT * sizeof(union udma_desc))

extern int nc_per_dev_param;

struct neuron_device;
struct neuron_dma_eng_state;
struct neuron_dma_queue_state;
struct ndma_eng;
struct ndma_ring;

/*
 * H2D DMA Completion Thread
 * -------------------------
 * one thread per ND.
 * It is shared across rings for completion, remote pinning, and submission work.
 *
 * Async IO only.
 *
 * @thread: kthread handle
 * @wait_queue: wait queue used to sleep/wake the thread
 * @nonempty_ctxq_bitmap: bitmap of H2D ctx queues with pending work
 * @stop: set to request thread exit
 */
struct ndma_h2d_dma_cmpltn_thread {
	struct task_struct *thread;
	wait_queue_head_t wait_queue;
	atomic64_t nonempty_ctxq_bitmap;
	u32 rr_start_bit;
	volatile bool stop;
};

int ndma_h2d_create_cmpltn_thread(struct neuron_device *nd);
void ndma_h2d_stop_cmpltn_thread(struct neuron_device *nd);

/*
 * H2D DMA Completion Queue (CQ)
 * -----------------------------
 * A fixed-size circular buffer shared between driver and runtime, consisting of
 * NDMA_H2D_COMPL_QUEUE_CAPACITY Completion Queue Entries (CQEs).
 *
 * Driver Lib mmaps both the CQ and its metadata (head/tail and capacity) to 
 * runtime. The driver writes a completion result at tail CQE, while the runtime 
 * consumes from head CQE and clears it. Each CQE contains a sequence number,
 * a completion result, and an opaque context pointer.
 *
 * Correctness: CQ is SPMC (one driver kthread writes per ND; multiple runtime
 * threads read). Runtime must lock; driver is lock-free. Both sides require
 * smp_wmb()/smp_rmb() barriers.
 *
 * Async IO only.
 *
 * Also see neuron_h2d_dma_compl_queue_t and
 * neuron_h2d_dma_compl_queue_entry_t in neuron_driver_shared.h.
 *
 */
struct ndma_h2d_compl_queue {
	uint32_t capacity_mask;	// Capacity mask of the CQ (capacity - 1).
	uint32_t tail;			// Free-running index of the next free CQE to be written by driver. 
							// Internally maintained by driver.
	struct mem_chunk *mc;	// Memchunk for the CQ.
	neuron_h2d_dma_compl_queue_t *compl_queue_shared;// the CQ structure mmapped to and shared with user space.
};

int ndma_h2d_compl_queue_init(struct neuron_device *nd, struct ndma_h2d_compl_queue *compl_queue);
void ndma_h2d_compl_queue_destroy(struct ndma_h2d_compl_queue *compl_queue);


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
	int               pending_tx_transfers; // pending tx (m2s) transfers for this context. Used to update ring ptrs
	int               pending_rx_transfers; // pending rx (s2m) transfers for this context (may differ from tx)
	void             *completion_ptr;     // completion buffer pointer (host memory buffer we poll on for completions)
};

struct ndma_h2t_dma_ioq;

struct ndma_ring {
	// TODO combine all the h2t stuff in a sub structure
	struct mutex h2t_ring_lock;
	struct udma_ring_ptr h2t_completion;  // TODO why are we using udma_ring_ptr...
	struct mem_chunk *h2t_completion_mc;
	struct ndma_h2t_dma_context h2t_dma_ctx[NEURON_DMA_H2T_CTX_HANDLE_CNT];
	//TODO - these are blowing up ring size move them to h2t_dma_ioq
	struct ndma_h2d_compl_queue dma_compl_queue;
	struct ndma_h2t_dma_ioq *h2t_dma_ioq;
	u32 h2t_nc_id;
	bool h2t_allocated; // ring can be allocated for standard use or h2t 
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
	pid_t owner; // process which initialized this queue.
};

struct ndma_eng {
	struct mutex lock;
	struct neuron_device *nd;
	u32 eng_id;
	struct ndma_queue queues[DMA_MAX_Q_MAX];
	struct udma udma;
	bool used_for_h2t;
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
 * @allocatable: whether new descriptors can be added post queue init
 *
 * Return: 0 if queue init succeeds, a negative error code otherwise.
 */
int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, bool allocatable);

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

/** 
 * ndmar_h2t_ring_request() - request a h2t ring
 *
 * @nd: Neuron device which contains the DMA engine
 * @nc_id: neuron core id
 * @h2t: initialize ring to be used for h2t traffic
 * @rqid: returned id of the queue
 *
 */
int ndmar_h2t_ring_request(struct neuron_device *nd, int nc_id, bool h2t, int *rqid);

/** 
 * ndmar_h2t_ring_release()
 *
 * @nd: Neuron device which contains the DMA engine
 * @nc_id: neuron core id
 * @qid: id the h2t queue to release
 *
 */
int ndmar_h2t_ring_release(struct neuron_device *nd, int nc_id, int qid);

/**
 * ndmar_h2t_ring_is_h2t() - return true if this is an h2t ring
 */
static inline bool ndmar_h2t_ring_is_h2t(const struct ndma_ring *ring)
{
	return (ring->h2t_completion_mc != NULL);
}

/**
 * ndmar_h2t_ring_is_owner - return true if this h2t ring is owned by nc_id
 *
 */
static inline bool ndmar_h2t_ring_is_owner(struct ndma_ring *ring, int nc_id)
{
	return (nc_id == ring->h2t_nc_id) && ndmar_h2t_ring_is_h2t(ring);
}
		
static inline bool ndmar_h2t_ring_is_allocated(struct ndma_ring *ring)
{
	return ring->h2t_allocated;
}

static inline void ndmar_h2t_ring_state_clr(struct ndma_ring *ring)
{
	ring->h2t_nc_id = -1;
	ring->h2t_allocated = false;
}

/**
 * ndmar_qid_valid() - return true if a queue is valid
 */
static inline bool ndmar_qid_valid(int qid)
{
	return ((qid >= 0) && (qid < DMA_MAX_Q_MAX));
}
#endif

