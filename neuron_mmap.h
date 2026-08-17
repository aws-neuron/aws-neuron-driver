
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Provides memory map related functions */

#ifndef NEURON_DMM_H
#define NEURON_DMM_H

#include <linux/version.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include "neuron_mempool.h"

#ifndef RHEL_RELEASE_VERSION
#define RHEL_RELEASE_VERSION(a,b) 1
#endif

/*
 * mm_get_unmapped_area() signature history:
 *   < 6.10: no standalone function; call current->mm->get_unmapped_area()
 *           directly (5 args).
 *   6.10:   introduced as
 *           mm_get_unmapped_area(struct mm_struct *mm, struct file *filp,
 *                                addr, len, pgoff, flags) -- 6 args.
 *   6.19:   the leading mm parameter was dropped, reverting to
 *           mm_get_unmapped_area(struct file *filp, addr, len, pgoff, flags)
 *           -- 5 args. (Present in v6.18, gone in v6.19.)
 */
#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)))
#define nmmap_kern_get_unmapped_area(filep, addr, len, pgoff, flags) \
	mm_get_unmapped_area(filep, addr, len, pgoff, flags)
#elif (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 5)))
#define nmmap_kern_get_unmapped_area(filep, addr, len, pgoff, flags) \
	mm_get_unmapped_area(current->mm, filep, addr, len, pgoff, flags)
#else
#define nmmap_kern_get_unmapped_area(filep, addr, len, pgoff, flags) \
	current->mm->get_unmapped_area(filep, addr, len, pgoff, flags)
#endif

#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 5)))
static inline void vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)
{
    vma->vm_flags |= flags;
}

static inline void vm_flags_clear(struct vm_area_struct *vma, vm_flags_t flags)
{
    vma->vm_flags &= ~flags;
}
#endif

struct nmmap_node {
	struct rb_node node; //rbtree node
	u64 pa; //physical address of the device memory
	void *va; //mapped virtual address of the device memory
	pid_t pid; //pid that map'd this device memory
	u32 device_index; //device index to which the memory belongs to
	u64 size; //size of the device memory
	u64 neuron_pa; // physical address in Neuron address space (only set when mmap'ed host allocated Neuron HBM)

	//call back routine that will be called when this device memory is freed
	//this will be used by efa or other utilities that are using this memory
	//and if they have this function registered then it will be called so that
	//it can clean up their state
	void (*free_callback)(void *data);
	void *data;

	/* dmabuf usage count */
	u32 dmabuf_ref_cnt;
};

/**
 * mapping table of resources that can be mapped into user space.
 * Used by EFA.
 */
struct neuron_dm_special_mmap_ent {
	enum neuron_dm_block_type block;
	int  block_id;
	enum neuron_dm_resource_type resource;
	u64  offset;
	u64  size;
	u64  bar_offset;
	int  bar_num;
};

#define DM_SPECIAL_MM_ENT(blk, blk_id, res, blk_mmoff, blk_baroff, blk_sz, res_off, res_sz, bar_num)  \
						{blk, blk_id, res, (blk_mmoff) + (blk_sz)*(blk_id) + (res_off), res_sz, (blk_baroff) + (blk_sz)*(blk_id) + res_off, bar_num}

#define DM_SPECIAL_MM_ENT_(blk, blk_id, res, blk_mmoff, blk_baroff, blk_mmsz, blk_sz, res_off, res_sz, bar_num)  \
						{blk, blk_id, res, (blk_mmoff) + (blk_mmsz)*(blk_id) + (res_off), res_sz, (blk_baroff) + (blk_sz)*(blk_id) + res_off, bar_num}

/**
 * nmmap_create_node - Creates a memory map node that can be used by external drivers
 * like EFA
 *
 * @nd: neuron device
 * @va: mapped virtual address
 * @pid_t: pid that has this virtual address
 * @size: size of the mapped memory
 * @pa: physical address of the device memory in host address space
 * @neuron_pa: physical address in Neuron address space
 */
void nmmap_create_node(struct neuron_device *nd, void *va, pid_t pid, u64 size, u64 pa, u64 neuron_pa);

/**
 * nmmap_delete_node - Deletes all mmap nodes for the current PID. If external drivers have any free function regsistered
 * then this will call the routine
 *
 * @nd: neuron device
 */
void nmmap_delete_all_nodes(struct neuron_device *nd);
/**
 * nmmap_offset() - Return mmap offset for given memory chunk.
 *
 * @mc: memory chunk which needs to be mapped
 *
 * Return: offset to be used to mmap in the /dev/ndX file.
 */
u64 nmmap_offset(struct mem_chunk *mc);

/**
 * nc_mmap() - mmap a range into userspace
 * @nd: neuron device
 * @vma: mmap area.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nmmap_mem(struct neuron_device *nd, struct vm_area_struct *vma);

/**
 * nmap_dm_special_resource_get() - return the mmap offset and size for a special resource
 * in general used to map bar0 resources for EFA
 *
 * @block:    block type containing the resource
 * @id:       id of the block if is more than one block .
 * @resource: resource the caller wants to mmap
 *
 */
int nmap_dm_special_resource_get( enum neuron_dm_block_type block, u32 id,  enum neuron_dm_resource_type resource, u64 *offset, u64 *size);

/**
 * nmmap_search_va - Searches for va (mmap'd) in the rbtree
 *
 * @nd: neuron device
 * @va: mapped virtual address
 *
 * Return: mmap node
 */
struct nmmap_node *nmmap_search_va(struct neuron_device *nd, void *va);

/**
 * nmmap_get_mc_from_pa() - return the mc associated with a pa
 * @nd: neuron device
 * @pa: physical address with the mc
 *
 */
struct mem_chunk *nmmap_get_mc_from_pa(struct neuron_device *nd, phys_addr_t pa);


/**
 * nmmap_get_va_placement() - returns Neuron device and HBM index that VA
 *  was allocated from
 *
 * @va:				virtual address of Neuron memory
 * @device_index: 	Neuron device the memory was allocated from
 * @hbm_index:		the index of the HBM the memory was allocated from
 */

int nmmap_get_va_placement(void *va, int *device_index, int *hbm_index);

/**
 * nmmap_get_unmapped_area() - Return a huge page aligned VA for device mmaps
 *  whose offset and size are both huge page aligned. This enables EFA P2P MR
 *  registration to use 2MB pages instead of 4KB pages.
 *
 * @filep:	file pointer
 * @addr:	address hint from userspace
 * @len:	mapping length
 * @pgoff:	page offset (device BAR offset)
 * @flags:	mmap flags
 *
 * Return: unmapped area address, or error value
 */
unsigned long nmmap_get_unmapped_area(struct file *filep, unsigned long addr,
				      unsigned long len, unsigned long pgoff,
				      unsigned long flags);
#endif
