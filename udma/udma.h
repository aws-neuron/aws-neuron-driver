// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __UDMA_H__
#define __UDMA_H__

#include "../neuron_reg_access.h"
#include "udma_regs.h"

#define DMA_MAX_Q_V4 16
#define DMA_MAX_Q_MAX DMA_MAX_Q_V4

/* Maximum number of completion descriptors per cache line */
#define UDMA_MAX_NUM_CDESC_PER_CACHE_LINE 16

/* helper #define, when used during DMA initialization all DMA
 * queues will be initialized
 */
#define UDMA_NUM_QUEUES_MAX 0xff

#define UDMA_QUEUE_ADDR_BYTE_ALIGNMENT 256 // start and end addr for ring must be 256 byte aligned for prefetching

/* DMA queue size range */
#define UDMA_MIN_Q_SIZE 32
#define UDMA_MAX_Q_SIZE ((1 << 24) - 16) // udma start and end addr must be 256 byte aligned. Since a desc is 16 bytes, Max queue size in descs is (2^24 - (256 / 16))

/* V1 hardware is DMA rev 4, other revisions are not supported */
#define UDMA_REV_ID_4 4

/* Initial Ring ID(phase bit) expected by hardware */
#define UDMA_INITIAL_RING_ID 1
/* Number of bits used to represent Ring ID */
#define DMA_RING_ID_MASK 0x3

/** UDMA submission descriptor */
union udma_desc {
	/* TX */
	struct {
		u32 len_ctrl;
		u32 meta_ctrl;
		u64 buf_ptr;
	} tx;
	/* TX Meta, used by upper layer */
	struct {
		u32 len_ctrl;
		u32 meta_ctrl;
		u32 meta1;
		u32 meta2;
	} tx_meta;
	/* RX */
	struct {
		u32 len_ctrl;
		u32 buf2_ptr_lo;
		u64 buf1_ptr;
	} rx;
} __attribute__((packed, aligned(16)));

#define M2S_DESC_DMB BIT(30) /* Data Memory Barrier */
#define M2S_DESC_INT_EN BIT(28) /* Enable Interrupt on completion */
#define M2S_DESC_LAST BIT(27) /* Last descriptor in the packet */
#define M2S_DESC_FIRST BIT(26) /* First descriptor in the packet */
#define M2S_DESC_RING_ID_SHIFT 24
#define M2S_DESC_RING_ID_MASK (0x3 << M2S_DESC_RING_ID_SHIFT) /* Ring ID bits in m2s */
#define M2S_DESC_LEN_SHIFT 0
#define M2S_DESC_LEN_MASK (0xffff << M2S_DESC_LEN_SHIFT) /* Data length */

#define S2M_DESC_INT_EN BIT(28) /* Enable Interrupt on completion */
#define S2M_DESC_RING_ID_SHIFT 24
#define S2M_DESC_RING_ID_MASK (0x3 << S2M_DESC_RING_ID_SHIFT) /* Ring ID bits in s2m */
#define S2M_DESC_RING_SHIFT UDMA_S2M_Q_RDRBP_LOW_ADDR_SHIFT

#define IS_POWER_OF_TWO(base) (((base) & ((base) - 1)) == 0) /* helper to check for powers of 2 */
#define HAS_ALIGNMENT(base, alignment) (((base) & ((alignment) - 1)) == 0) /* helper to check alignments */

/** UDMA completion descriptor */
union udma_cdesc {
	/* TX completion */
	struct {
		u32 ctrl_meta;
	} desc_comp_tx;
	/* RX completion */
	struct {
		/* TBD */
		u32 ctrl_meta;
	} desc_comp_rx;
} __attribute__((packed, aligned(4)));

/* TX/RX common completion desc ctrl_meta feilds */
#define UDMA_CDESC_LAST BIT(27)
#define UDMA_CDESC_FIRST BIT(26)

/** UDMA queue type */
enum udma_type {
	UDMA_TX,
	UDMA_RX
};

/** UDMA state */
enum udma_state {
	UDMA_DISABLE = 0,
	UDMA_IDLE,
	UDMA_NORMAL,
	UDMA_ABORT,
	UDMA_RESET
};

extern const char *const udma_states_name[];

/** UDMA Q specific parameters from upper layer */
struct udma_q_params {
	u32 size; // Number of descriptors, submission and completion rings must have same size
	u32 allocatable; // If true, new descriptors can be added otherwise no
	union udma_desc *desc_base; // cpu address for submission ring descriptors
	dma_addr_t desc_phy_base; /// device physical base address for submission ring descriptors
	u8 *cdesc_base; // completion descriptors pointer, NULL means no completion update
	dma_addr_t cdesc_phy_base; // completion descriptors ring physical base address
	u8 adapter_rev_id; // PCI adapter revision ID
	enum udma_type type; // m2s (TX) or s2m (RX)
	u32 eng_id;
};

#define UDMA_INSTANCE_NAME_LEN 25
#define UDMA_ENG_CFG_FLAGS_DISABLE_ERROR BIT(0)
#define UDMA_ENG_CFG_FLAGS_DISABLE_ABORT BIT(1)
/** UDMA parameters from upper layer */
struct udma_params {
	void __iomem *udma_regs_base; // udma base address
	u32 cdesc_size; // size of the completion descriptor
	u8 num_of_queues; // number of queues used by the UDMA
	u32 flags;
	const char *name; /**< caller should not free name*/
	bool reserve_max_read_axi_id; // a V2 configuration workaround
};

/* Fordward declaration */
struct udma;

/** SW status of a queue */
enum udma_queue_status { QUEUE_DISABLED = 1, QUEUE_ENABLED, QUEUE_ABORTED };

/** UDMA Queue private data structure */
struct __attribute__((__aligned__(64))) udma_q {
	u32 size_mask; // mask used for pointers wrap around equals to size - 1
	union udma_q_regs __iomem *q_regs; // pointer to the per queue UDMA registers
	union udma_desc *desc_base_ptr; // base address submission ring descriptors
	u32 is_allocatable; // True if new descriptors can be allocated from this queue
	u32 next_desc_idx; // index to the next available submission descriptor
	u32 desc_ring_id; // current submission ring id
	u8 *cdesc_base_ptr; // completion descriptors pointer, NULL means no completion
	u32 cdesc_size; // size (in bytes) of the udma completion ring descriptor
	u32 next_cdesc_idx; // index in descriptors for next completing ring descriptor
	u8 *end_cdesc_ptr; // used for wrap around detection
	u32 comp_head_idx; // completion ring head pointer register shadow
	volatile union udma_cdesc *comp_head_ptr;
	u32 pkt_crnt_descs; // holds the number of processed descriptors of the current packet
	u32 comp_ring_id; // current completion Ring Id
	dma_addr_t desc_phy_base; // submission desc. physical base
	dma_addr_t cdesc_phy_base; // completion desc. physical base
	u32 flags; //flags used for completion modes
	u32 size; // ring size in descriptors
	enum udma_queue_status status;
	struct udma *udma; // pointer to parent UDMA
	u32 qid; // the index number of the queue
	u32 eng_id; // engine_id that this queue belongs to
	enum udma_type type; /* Tx or Rx */
	u8 adapter_rev_id; // PCI adapter revision ID
	u32 cfg; // default value of CFG CSR. Cache here to avoid reads during queue enable/disable
	u32 rlimit_mask; // default value of rlimit_mask (m2s only)
};

/* UDMA */
struct udma {
	char name[UDMA_INSTANCE_NAME_LEN];
	enum udma_state state_m2s;
	enum udma_state state_s2m;
	u8 num_of_queues_max; // max number of queues supported by the UDMA
	u8 num_of_queues; // number of queues used by the UDMA
	void __iomem *unit_regs_base; // udma unit (RX & TX) regs base
	// bi-directional engine, keep both m2s and s2m registers
	struct udma_m2s_regs_v4 __iomem *udma_regs_m2s;
	struct udma_s2m_regs_v4 __iomem *udma_regs_s2m;
	void __iomem *gen_regs; // pointer to the Gen registers
	struct udma_gen_axi __iomem *gen_axi_regs; // pointer to the gen axi regs
	struct udma_iofic_regs __iomem *gen_int_regs; // pointer to the gen iofic regs
	void __iomem *gen_ex_regs; // pointer to the Gen ex registers
	struct udma_q udma_q_m2s[DMA_MAX_Q_MAX]; // Array of UDMA Qs pointers
	struct udma_q udma_q_s2m[DMA_MAX_Q_MAX]; // Array of UDMA Qs pointers
	unsigned int rev_id; // UDMA revision ID
	u32 cdesc_size;
	bool reserve_max_read_axi_id; // a V2 configuration workaround
};

enum {
	UDMA_M2M_BARRIER_NONE = 0,
	UDMA_M2M_BARRIER_DMB = 1,
	UDMA_M2M_BARRIER_WRITE_BARRIER = 2
};

/**
 * udma_init() - Initialize udma engine.
 *
 * Initializes DMA engine(hardware) and udma structure.
 *
 * @udma: udma structure needs to be initialized
 * @udma_params	- udma init parameters
 *
 * Return: 0 if UDMA is initialized successfully, a negative error code otherwise.
 */
int udma_init(struct udma *udma, struct udma_params *udma_params);

/**
 * udma_q_init() - Initialize the udma queue.
 *
 * Initializes DMA queue(hardware) and udma structure.
 *
 * @udma: udma structure
 * @qid: Hardware queue id
 * @q_params: Init parameters
 *
 * Return: 0 if UDMA queue is initialized successfully,
 *	       -EINVAL if the qid is out of range,
 *	       -EIO if queue was already initialized.
 */
int udma_q_init(struct udma *udma, u32 qid, struct udma_q_params *q_params);

/**
 * udma_q_pause() - Pauses a udma queue
 *
 * @udma_q:	udma queue data structure
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int udma_q_pause(struct udma_q *udma_q);

/**
 * udma_q_handle_get() -  Return a pointer to a queue date structure.
 *
 * @udma: udma data structure
 * @qid: queue index
 * @q_handle: pointer to the location where the queue structure pointer written to
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int udma_q_handle_get(struct udma *udma, u32 qid, enum udma_type type, struct udma_q **q_handle);

/**
 * udma_iofic_m2s_error_ints_unmask() - Unmask all error interrupts for m2s queue.
 *
 * @udma: udma data structure
 */
void udma_iofic_m2s_error_ints_unmask(struct udma *udma);

/**
 * udma_iofic_s2m_error_ints_unmask() - Unmask all error interrupts for s2m queue.
 *
 * @udma: udma data structure
 */
void udma_iofic_s2m_error_ints_unmask(struct udma *udma);

/**
 * udma_iofic_error_ints_unmask_one() - Unmask error interrupts for the given iofic_ctrl.
 *
 * @iofic_ctrl: iofic data structure
 * @mask: mask of interrupts to unmask
 */
void udma_iofic_error_ints_unmask_one(struct iofic_grp_ctrl *iofic_ctrl, uint32_t mask);

/**
 * udma_m2m_set_axi_error_abort() - Program axi error detection.
 * Needed to trigger udma abort for v2: https://tiny.amazon.com/tbm9ad1i
 *
 * @udma: udma data structure
 */
void udma_m2m_set_axi_error_abort(struct udma *udma);

/**
 * udma_m2m_mask_ring_id_error() - Mask out error detection on invalid ring ids
 *
 * @udma: udma data structure
 * @intc_base: interrupt controller base
 */
void udma_m2m_mask_ring_id_error(struct udma *udma, void __iomem *intc_base);

/**
 * udma_state_set() - Change the UDMA's state
 *
 * @udma: udma data structure
 * @state: new state to set
 *
* Return: 0 on success, a negative error code otherwise.
 */
int udma_state_set(struct udma *udma, enum udma_state state);

/**
 * udma_state_get() - Return the current UDMA hardware state
 *
 * @udma: udma handle
 *
 * Return: the UDMA state as reported by the hardware.
 */
enum udma_state udma_state_get(struct udma *udma, enum udma_type type);

/**
 * udma_available_get() - Get number of descriptors that can be submitted to the udma.
 *
 * @udma_q: queue handle
 *
 * Return: number of free descriptors.
 */
static inline u32 udma_available_get(struct udma_q *udma_q)
{
	// This function will only run on dma queues that use the
	// wraparound feature (h2t only at the moment).
	// Due to the wraparound logic using bitwise and as mod,
	// we need to check size is power of 2.
	BUG_ON(IS_POWER_OF_TWO(udma_q->size) == false);
	u32 tmp = udma_q->next_cdesc_idx -
		  (udma_q->next_desc_idx + UDMA_MAX_NUM_CDESC_PER_CACHE_LINE);
	tmp &= udma_q->size_mask;

	return tmp;
}

/**
 * udma_desc_get() - Get next available descriptor
 *
 * @udma_q: queue handle
 *
 * Return: pointer to the next available descriptor
 */
static inline union udma_desc *udma_desc_get(struct udma_q *udma_q)
{
	union udma_desc *desc;
	u32 next_desc_idx;

	BUG_ON(udma_q == NULL);
	/* when setting up the queue caller might pass NULL for
	   the queue base address.  That means the caller is responsible for
	   attaching the ring and managing it somehow.  Should not be calling
	   this function in that case
	*/
	if (udma_q->desc_base_ptr == NULL)
		return NULL;
	if (!udma_q->is_allocatable)
		return NULL;

	next_desc_idx = udma_q->next_desc_idx;
	desc = udma_q->desc_base_ptr + next_desc_idx;

	next_desc_idx++;

	// This function will only run on dma queues that use the
	// wraparound feature (h2t only at the moment).
	// Due to the wraparound logic using bitwise and as mod,
	// we need to check size is power of 2.
	BUG_ON(IS_POWER_OF_TWO(udma_q->size) == false);
	/* if reached end of queue, wrap around */
	udma_q->next_desc_idx = next_desc_idx & udma_q->size_mask;

	return desc;
}

/**
 * udma_ring_id_get() - Get ring id(phase bit) for the last allocated descriptor
 *
 * @udma_q: udma queue data structure
 *
 * Return: ring id for the last allocated descriptor
 *
 * @note This function must be called each time a new descriptor is allocated
 *       by the udma_desc_get(), unless ring id is ignored.
 */
static inline u32 udma_ring_id_get(struct udma_q *udma_q)
{
	u32 ring_id;

	BUG_ON(udma_q == NULL);

	ring_id = udma_q->desc_ring_id;

	/* calculate the ring id of the next desc */
	/* if next_desc points to first desc, then queue wrapped around */
	if (unlikely(udma_q->next_desc_idx) == 0)
		udma_q->desc_ring_id = (udma_q->desc_ring_id + 1) & DMA_RING_ID_MASK;
	return ring_id;
}

/**
 * udma_desc_action_add() - Add number descriptors to the submission queue.
 *
 * @udma_q: queue handle
 * @num: number of descriptors to add to the queues ring
 */
void udma_desc_action_add(struct udma_q *udma_q, u32 num);

/**
 * udma_cdesc_ack() - Acknowledge processing completion descriptors
 *
 * @udma_q: udma queue handle
 * @num: number of descriptors to acknowledge
 */
static inline void udma_cdesc_ack(struct udma_q *udma_q, u32 num)
{
	BUG_ON(udma_q == NULL);
	// This function will only run on dma queues that use the
	// wraparound feature (h2t only at the moment).
	// Due to the wraparound logic using bitwise and as mod,
	// we need to check size is power of 2.
	BUG_ON(IS_POWER_OF_TWO(udma_q->size) == false);

	u32 cdesc_idx = udma_q->next_cdesc_idx;
	u32 next_cdesc_idx = (cdesc_idx + num) & udma_q->size_mask;
	while (!__sync_bool_compare_and_swap(&udma_q->next_cdesc_idx, cdesc_idx, next_cdesc_idx)) {
		cpu_relax();
		cdesc_idx = udma_q->next_cdesc_idx;
		next_cdesc_idx = (cdesc_idx + num) & udma_q->size_mask;
	}
}

#define UDMA_M2S_MAX_ALLOWED_DESCS_PER_PACKET_V4 128

// size of completion descriptor
#define UDMA_CDESC_SIZE 16

// Hardware transfer limit is 64K per descriptor
#define MAX_DMA_DESC_SIZE 65536

struct udma_ring_ptr {
	dma_addr_t addr; // physical address of the queue
	void *ptr; // virtual address of the queue
};

/**
 * udma_m2m_init_engine() - Initialize the M2M Engine.
 *
 * @udma: DMA structure to initialize
 * @regs_base: Base address of the UDMA engine
 * @num_queues: Number of queues
 * @eng_name: Human readable name(for debugging)
 * @disable_phase_bit: If true disables checking phase bit
 * @reserve_max_read_axi_id: a V2 configuration workaround
 * Return: 0 if initialization is successful, a negative error code otherwise.
 */
int udma_m2m_init_engine(struct udma *udma, void __iomem *regs_base, int num_queues, char *eng_name,
			 int disable_phase_bit, int allowed_desc_per_pkt, bool reserve_max_read_axi_id);

/**
 * udma_m2m_init_queue() - Initialize a queue in the M2M engine.
 *
 * @udma: UDMA structure.
 * @qid: Queue index.
 * @eng_id: Engine index
 * @m2s_ring_size: Number of descriptors in the m2s queue.
 * @s2m_ring_size: Number of descriptors in the s2m queue.
 * @desc_allocatable: If false it means, descriptors cant be allocated from this queue.
 * @m2s_ring: Address/pointer to m2s queue.
 * @s2m_ring: Address/pointer to s2m queue.
 * @s2m_compl_ring: Address/pointer to completion queue.
 *
 * Return: 0 if initialization is successful, a negative error code otherwise.
 */
int udma_m2m_init_queue(struct udma *udma, int qid, u32 eng_id, u32 m2s_ring_size, u32 s2m_ring_size,
			bool desc_allocatable, struct udma_ring_ptr *m2s_ring,
			struct udma_ring_ptr *s2m_ring, struct udma_ring_ptr *s2m_compl_ring);

/**
 * udma_m2m_copy_prepare_one() - Prepare next descriptor in the queue for copying memory to memory.
 *
 * @udma: DMA structure.
 * @qid: Queue index.
 * @s_addr: Source physical address.
 * @d_addr: Destination physical address.
 * @size: - Size of the transfer(max 64K).
 * @barrier_type:
 *     0. no barrier is used
 *     1. set memory barrier bit (DMB): this would make sure all previous transfers are done before starting this.
 *     2. use write barrier: if set dmb is set for trn chip, this will use the write barrier
 * @param en_int[in] - Enable interrupt bit.
 *
 * Return: 0 if successful, a negative error code otherwise.
 */
int udma_m2m_copy_prepare_one(struct udma *udma, u32 qid, dma_addr_t s_addr, dma_addr_t d_addr,
			      u32 size, int barrier_type, bool en_int);

/**
 * udma_m2m_copy_start() - Start DMA transfer or prefetch s2m descriptors
 *
 * udma_m2m_copy_prepare_one() should be used create descriptor and then use this function
 * to start the transfer.
 *
 * @udma: DMA structure.
 * @qid: Queue index.
 * @src_count: Number of source descriptors. Can be 0 when called to prefetch s2m ahead of time
 * @dst_count: Number of destination descriptors.
 *
 * Return: 0 if successful, a negative error code otherwise.
 */
int udma_m2m_copy_start(struct udma *udma, u32 qid, u32 m2s_count, u32 s2m_count);

#endif
