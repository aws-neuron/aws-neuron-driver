// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Memory pool for allocating device memory and host memory.
 *
 *  1. mem_chunk/mc         - Is a chunk of memory in device/host DRAM.
 *  2. mempool/mp           - Is a pool of memory backed either device DRAM or host DRAM.
 *                            For device memory it uses gen_pool allocator to allocate memory.
 *                            For host memory it directly uses kmalloc().
 *  3. mempool_set/mpset    - Is collection for mp for given neuron device.
 */

#ifndef NEURON_MEMPOOL_H
#define NEURON_MEMPOOL_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>

#include "share/neuron_driver_shared.h"
#include "neuron_ioctl.h"
#include "neuron_mc_handle.h"

struct neuron_device;

extern int mempool_min_alloc_size;

enum mem_location {
	MEM_LOC_INVALID = 0, // Invalid type
	MEM_LOC_HOST = 1, // Memory chunk is from Host DRAM
	MEM_LOC_DEVICE = 2, // Memory chunk is from Device DRAM
	MEM_LOC_COUNT = MEM_LOC_DEVICE
};

/** Memory pool to manage Device memory.
 *
 * Device is memory is split in to chunks and allocated.
 * Uses genpool allocator in the backend.
 */
struct mempool {
	char name[32]; // friendly name
	bool initialized; // True if initialized.

	struct mempool_set *mpset; // parent mpset

	enum mem_location mem_location; // location of the memory
	u32 dram_channel; // DRAM channel valid only if location is device
	u32 dram_region; // DRAM region valid only if location is device

	struct gen_pool *gen_pool; // main gen pool
	struct gen_pool *gen_pool_small; // small gen pool for small allocations to avoid fragmentation, may be NULL
	u64 main_pool_end_addr; // also the start addr of small genpool when enabled
	size_t small_pool_size;

	size_t region_size; // size of the initial region
	size_t allocated_size; // total allocated memory size in bytes

	u32 page_size; // size of the host page backing this pool
	u32 page_requested_count; // number pages requested during pool creation
	u32 page_count; // number pages allocated successfully
	void **page_va_array; // array of allocated page's kva
	dma_addr_t *page_pa_array; // array of allocated page's pa

	u64 scratchpad_size; // only used for allocations of type NEURON_MEMALLOC_TYPE_CONTIGUOUS_SCRATCHPAD_DEVICE
};

// DRAM region is split into multiple regions.
#define MAX_DDR_REGIONS 4
#define MAX_DRAM_CHANNELS 4

// start page size for host MP
#define MP_HOST_PAGE_SIZE_MIN (256UL * 1024)
// Number for MPs for host allocation
#define MP_HOST_RESERVE_MEMORY_POOL_COUNT 4

struct mempool_set {
	struct mutex lock;

	struct neuron_device *nd; // backponter to neuron_device

	u32 mp_device_num_regions; // number of regions in the device pool
	u32 num_channels; // number of regions in the device pool
	struct mempool mp_device[MAX_DRAM_CHANNELS][MAX_DDR_REGIONS]; // device memory pools

	struct mempool mp_hrm[MP_HOST_RESERVE_MEMORY_POOL_COUNT]; // host reserve memory pools

	// linked list head to store mem_chunk of different lifespan
	struct list_head mc_lifespan_local_head;
	struct list_head mc_lifespan_cur_process_head[NEURON_MAX_PROCESS_PER_DEVICE];
	struct list_head mc_lifespan_all_process_head;
	struct list_head mc_lifespan_device_head;

	// for stats and debugging
	u64 host_mem_size; // host memory used
	u64 device_mem_size; // device memory used

	void *pdev; // pci_dev->dev pointer
	struct rb_root root; //rbtree that has all mem chunks allocated
	rwlock_t rblock; //protect the rbtree access

	struct rb_root mmap_root[NEURON_MAX_PROCESS_PER_DEVICE]; //rbtree that tracks all mmap'd device mem va
	rwlock_t rbmmaplock; //protect the dmm root tree access
};

enum mc_lifespan {
	MC_LIFESPAN_LOCAL = 1,  	// MC is freed when current IOCTL/syscall ends
	MC_LIFESPAN_CUR_PROCESS,	// MC is freed when the current process exits
	MC_LIFESPAN_ALL_PROCESS,	// MC is freed when all the processes associated with ND exits
	MC_LIFESPAN_DEVICE,		// MC is freed when the device is detached
};

#define MEMCHUNK_MAGIC 0xE1C2D3F4

struct model_start_tracker {
	bool has_pe_iram_inst; // whether this memchunk is used for copying PE instructions. used to detect/record model start
	u32 nc_id; // the NC on which the model is started
};

struct mem_chunk {
	u32 magic; // magic pattern to validate the structure is actually mem_chunk.

	struct rb_node node; // valid when this chunk is added to the rbtree
	phys_addr_t pa; // physical address of the chunk
	void *va; // virtual address of the chunk

	u64 size; // chunk size

	struct mempool *mp; // backpointer to mp
	struct mempool_set *mpset; // back pointer to mpset
	struct gen_pool *gen_pool; // pointer to genpool

	u32 dram_channel; // DRAM channel
	u32 dram_region; // TDRAM region
	u32 nc_id; //neuron core index
	neuron_mc_handle_t mc_handle; // memchunk handle
	mem_alloc_category_t alloc_type; // memory allocation category

	enum mem_location mem_location; // location of memory - Host or Device

	pid_t pid; // process which allocated the memory

	int ref_count; // reference count

	enum mc_lifespan lifespan; // how long this mc should live.
	struct list_head lifespan_list; // link for the lifespan list

	void *caller_pc; // the function allocated this MC.

	struct model_start_tracker model_start_tracker;
};

/**
 * mpset_constructor() - Construct mpset for given device.
 *
 * @mpset: Pointer to mpset which need to be initialized
 * @pdev: Pointer to device structure.
 * @nd: Neuron device to initialize
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int mpset_constructor(struct mempool_set *mpset, void *pdev, struct neuron_device *nd);

/**
 * mpset_destructor() - Free all mp in the set.
 *
 * @mpset: Pointer to mpset which need to be destroyed.
 */
void mpset_destructor(struct mempool_set *mpset);

/** mpset_search_mc() - Find memory chunk which maps given physical address
 *
 * @mpset: Pointer to mpset
 * @pa: physical address to search
 *
 * Return: mem chunk that has pa on success, NULL on failure
 */
struct mem_chunk *mpset_search_mc(struct mempool_set *mp, phys_addr_t pa);

/**
 * mc_alloc_align() - Allocate a memory chunk of size from given mpset, with alignment
 *
 * @nd: neuron_device to which the mc should be associated
 * @lifespan: When the MC needs to be automatically freed(if not freed already).
 * @size: Allocation size
 * @align: alignment requirement
 * @location: Backing DRAM location(host/device)
 * @channel: Backing DRAM channel
 * @region: Region in the backing DRAM
 * @mem_type: category of memory allocation (used for sysfs memory counters)
 * @result: Buffer to store the allocated memory chunk pointer
 *
 * Return: 0 if allocation succeeds, a negative error code otherwise.
 */
int mc_alloc_align(struct neuron_device *nd, enum mc_lifespan lifespan, u64 size, u64 align,
		   enum mem_location location, u32 channel, u32 region, u32 nc_id, mem_alloc_category_t mem_type,
		   struct mem_chunk **result);

/**
 * mc_free() - Free memory chunk and associated backing memory.
 *
 * @mc: Pointer to memory chunk to be freed(this would be set to NULL on success)
 */
void mc_free(struct mem_chunk **mcp);

/**
 * mpset_free_expired_mc() - Frees all MCs with given lifespan.
 *
 * @mpset: Pointer to mpset
 * @lifespan: Lifespan list to use
 */
void mpset_free_expired_mc(struct mempool_set *mpset, enum mc_lifespan lifespan);

/**
 * mc_inc_refcount() - Increases reference count of the given mc.
 *
 * @mc: Pointer to memory chunk
 */
void mc_inc_refcount(struct mem_chunk *mc);

//int mc_dump_all_chunks(struct neuron_device *nd, u32 channel);
int mc_dump_all_chunks(struct neuron_device *nd, u32 channel, u32 num_entries_in, struct neuron_ioctl_mem_chunk_info *data, u32 *num_entries_out);

static inline bool mc_access_is_within_bounds(const struct mem_chunk *mc, u64 access_offset, u64 access_size)
{
	if (mc->alloc_type == NEURON_MEMALLOC_TYPE_CONTIGUOUS_SCRATCHPAD_DEVICE) {
		return (mc->pa + access_offset + access_size <= mc->mp->main_pool_end_addr);
	}
	return access_offset + access_size <= mc->size;
}

#endif
