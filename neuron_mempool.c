// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <asm/io.h>
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/fault-inject.h>

#include "neuron_mempool.h"
#include "neuron_device.h"

int mempool_min_alloc_size = 256;

module_param(mempool_min_alloc_size, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(mempool_min_alloc_size, "Minimum size for device memory allocation");

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_mc_alloc);
#endif

// Limit for using kmalloc
#define MEMPOOL_KMALLOC_MAX_SIZE (256 * 1024)

/**
 * mc_insert_node() - Insert a mem chunk to the tree
 *
 * @root: binary tree root
 * @mc: memory chunk that needs to be inserted
 */
static void mc_insert_node(struct rb_root *root, struct mem_chunk *mc)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;
	phys_addr_t pa = mc->pa;

	/* Go to the bottom of the tree */
	while (*link) {
		parent = *link;
		struct mem_chunk *mc = rb_entry(parent, struct mem_chunk, node);

		if (mc->pa > pa) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
		}
	}

	/* Put the new node there */
	rb_link_node(&mc->node, parent, link);
	rb_insert_color(&mc->node, root);
}

/**
 * mc_remove_node() - Remove a mem chunk from the tree
 *
 * @root: binary tree root
 * @mc: memory chunk that needs to be removed
 */
void mc_remove_node(struct rb_root *root, struct mem_chunk *mc)
{
	rb_erase(&mc->node, root);
}

/**
 * mp_init() Initialize the mempool structure with given values.
 * Creates a backing gen_pool if the mem_location is device DRAM.
 *
 * @mp: pointer to mempool that needs to be initialized
 * @start_addr: starting address of the pool
 * @pool_size: size of the pool.
 * @mem_location: location of the backing memory.
 * @dram_channel: device dram channel backing this pool(applicable only if mem_location is device).
 * @dram_region: device dram region backing this pool(applicable only if mem_location is device).
 *
 * Return: 0 if pool is created, a negative error code otherwise.
 */
static int mp_init(struct mempool *mp, u64 start_addr, size_t pool_size,
		   enum mem_location mem_location, u32 dram_channel, u32 dram_region)
{
	int ret;

	memset(mp, 0, sizeof(*mp));

	mp->mem_location = mem_location;
	mp->dram_channel = dram_channel;
	mp->dram_region = dram_region;
	INIT_LIST_HEAD(&mp->device_allocated_head);
	mp->gen_pool = gen_pool_create(ilog2(mempool_min_alloc_size), -1);
	if (mp->gen_pool == NULL)
		return -ENOMEM;

	// 0 is special since we cant differentiate failure(NULL) in gen_pool_alloc().
	// so avoid starting at 0 by sacrificing first chunk.
	if (start_addr == 0) {
		start_addr = mempool_min_alloc_size;
		pool_size -= mempool_min_alloc_size;
	}
	ret = gen_pool_add_virt(mp->gen_pool, start_addr, start_addr, pool_size, -1);
	if (ret) {
		gen_pool_destroy(mp->gen_pool);
		return ret;
	}

	snprintf(mp->name, sizeof(mp->name), "device mempool [%d:%d]", dram_channel, dram_region);
	mp->region_size = pool_size;
	mp->initialized = 1;

	return 0;
}

/**
 * Frees all the chunks associated with the mempool.
 */
static void mp_free_device_mem(struct mempool *mp)
{
	BUG_ON(mp == NULL);
	if (!mp->initialized)
		return;

	if (mp->gen_pool != NULL) {
		// Free all entries
		struct list_head *this, *next;

		list_for_each_safe (this, next, &mp->device_allocated_head) {
			struct mem_chunk *mc =
				list_entry(this, struct mem_chunk, device_allocated_list);
			if (mc->va) {
				gen_pool_free(mp->gen_pool, (unsigned long)mc->va, mc->size);
				mc->va = NULL;
			}
			list_del(&mc->device_allocated_list);
			kfree(mc);
		}
		mp->allocated_size = 0;
	}
}

/**
 * Frees all the chunks associated with the mempool and releases the mempool.
 */
static void mp_destroy(struct mempool *mp)
{
	BUG_ON(mp == NULL);
	if (!mp->initialized)
		return;

	if (mp->gen_pool != NULL) {
		// Free all entries
		mp_free_device_mem(mp);
		gen_pool_destroy(mp->gen_pool);
	}
}

int mpset_host_init(struct mempool_set *mpset)
{
	mutex_init(&mpset->lock);
	INIT_LIST_HEAD(&mpset->host_allocated_head);
	mpset->root = RB_ROOT;
	return 0;
}

int mpset_device_init(struct mempool_set *mpset, int num_channels, int num_regions,
		      const phys_addr_t device_dram_addr[], const u64 device_dram_size[])
{
	int ret;
	u32 channel, region;
	u64 region_sz;

	if (num_regions <= 0 || num_regions > 4)
		num_regions = 1;
	mpset->num_regions = num_regions;

	for (channel = 0; channel < num_channels; channel++) {
		region_sz = device_dram_size[channel] / mpset->num_regions;
		for (region = 0; region < mpset->num_regions; region++) {
			dma_addr_t addr = device_dram_addr[channel] + (region * region_sz);
			ret = mp_init(&mpset->mp_device[channel][region], addr, region_sz,
				      MEM_LOC_DEVICE, channel, region);
			if (ret) {
				pr_err("neuron: mpset device init failed %d\n", ret);
				goto fail;
			}
		}
	}

	return 0;

fail:
	for (; channel >= 0; channel--) {
		for (; region >= 0; region--) {
			mp_destroy(&mpset->mp_device[channel][region]);
		}
	}
	memset(mpset, 0, sizeof(struct mempool_set));

	return ret;
}

static void mpset_free_host_memory(struct mempool_set *mpset)
{
	struct list_head *this, *next;
	list_for_each_safe (this, next, &mpset->host_allocated_head) {
		struct mem_chunk *mc = list_entry(this, struct mem_chunk, host_allocated_list);
		if (mc->va) {
			write_lock(&mpset->rblock);
			mc_remove_node(&mpset->root, mc);
			write_unlock(&mpset->rblock);
			if (mc->size > MEMPOOL_KMALLOC_MAX_SIZE) {
				dma_free_coherent(mpset->pdev, mc->size, mc->va, mc->pa);
			} else {
				kfree(mc->va);
			}
			mc->va = NULL;
		}
		list_del(&mc->host_allocated_list);
		kfree(mc);
	}
	mpset->host_mem_size = 0;
}

void mpset_free_all(struct mempool_set *mpset)
{
	u32 channel, region;

	mutex_lock(&mpset->lock);
	for (channel = 0; channel < V1_MAX_DRAM_CHANNELS; channel++) {
		for (region = 0; region < mpset->num_regions; region++) {
			mp_free_device_mem(&mpset->mp_device[channel][region]);
		}
	}
	mpset_free_host_memory(mpset);
	mutex_unlock(&mpset->lock);
}

void mpset_destroy(struct mempool_set *mpset)
{
	u32 channel, region;

	mutex_lock(&mpset->lock);
	for (channel = 0; channel < V1_MAX_DRAM_CHANNELS; channel++) {
		for (region = 0; region < mpset->num_regions; region++) {
			mp_destroy(&mpset->mp_device[channel][region]);
		}
	}
	mpset_free_host_memory(mpset);
	mutex_unlock(&mpset->lock);
	memset(mpset, 0, sizeof(struct mempool_set));
}

struct mem_chunk *mpset_search_mc(struct mempool_set *mp, phys_addr_t pa)
{
	struct rb_node *node = mp->root.rb_node; /* top of the tree */

	while (node) {
		struct mem_chunk *mc = rb_entry(node, struct mem_chunk, node);

		if ((mc->pa <= pa) && ((mc->pa + mc->size) >= pa)) {
			return mc;
		} else if (mc->pa > pa) {
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return NULL;
}

int mc_alloc(struct mempool_set *mpset, struct mem_chunk **result, u32 size,
	     enum mem_location location, u32 channel, u32 region, u32 nc_id)
{
	struct mem_chunk *mc;
	int ret = 0;

	*result = NULL;

	if (channel >= V1_MAX_DRAM_CHANNELS)
		return -EINVAL;
#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_mc_alloc, 1))
		return -ENOMEM;
#endif
	if (mpset->num_regions == 1) // shared DRAM mode, always use region 0
		region = 0;

	mc = (struct mem_chunk *)kmalloc(sizeof(struct mem_chunk), GFP_KERNEL);
	if (mc == NULL)
		return -ENOMEM;

	*result = mc;
	memset(mc, 0, sizeof(struct mem_chunk));

	mutex_lock(&mpset->lock);
	if (location == MEM_LOC_HOST) {
		if (size > MEMPOOL_KMALLOC_MAX_SIZE) {
			dma_addr_t addr;
			mc->va = dma_alloc_coherent(mpset->pdev, size, &addr,
						    GFP_KERNEL | GFP_DMA32);
			mc->pa = (phys_addr_t)addr;
		} else {
			mc->va = (void *)kmalloc(size, GFP_KERNEL);
			if (mc->va) {
				memset(mc->va, 0, size);
				mc->pa = virt_to_phys(mc->va);
			}
		}
		if (mc->va) {
			INIT_LIST_HEAD(&mc->host_allocated_list);
			list_add(&mc->host_allocated_list, &mpset->host_allocated_head);
			write_lock(&mpset->rblock);
			mc_insert_node(&mpset->root, mc);
			write_unlock(&mpset->rblock);
		} else {
			pr_info("host mem occupied %lld\n", mpset->host_mem_size);
		}
	} else {
		struct mempool *mp = NULL;
		mp = &mpset->mp_device[channel][region];
		if (!mp->gen_pool) {
			pr_err("neuron: mempool not initialized\n");
			ret = -ENOMEM;
			goto exit;
		}

		mc->va = gen_pool_dma_alloc(mp->gen_pool, size, &mc->pa);
		if (mc->va) {
			INIT_LIST_HEAD(&mc->device_allocated_list);
			list_add(&mc->device_allocated_list, &mp->device_allocated_head);
		} else {
			pr_info("%s total %ld occupied %ld needed %d available %ld\n", mp->name,
				mp->region_size, mp->allocated_size, size,
				gen_pool_avail(mp->gen_pool));
			pr_info("device regions %d occupied %lld\n", mpset->num_regions,
				mpset->device_mem_size);
		}
		mp->allocated_size += size;
	}
	if (mc->va == NULL) {
		ret = -ENOMEM;
		goto exit;
	}

	mc->mpset = mpset;
	mc->size = size;
	mc->mem_location = location;
	mc->dram_channel = channel;
	mc->dram_region = region;
	mc->nc_id = nc_id;

	if (location == MEM_LOC_HOST)
		mpset->host_mem_size += size;
	else
		mpset->device_mem_size += size;

exit:
	mutex_unlock(&mpset->lock);
	if (ret) {
		kfree(mc);
		*result = NULL;
	}
	return ret;
}

void mc_free(struct mem_chunk **mcp)
{
	struct mempool_set *mpset;
	struct mem_chunk *mc = *mcp;

	if (mc == NULL)
		return;

	mpset = mc->mpset;
	mutex_lock(&mpset->lock);

	if (mc->mem_location == MEM_LOC_HOST) {
		list_del(&mc->host_allocated_list);
		write_lock(&mpset->rblock);
		mc_remove_node(&mpset->root, mc);
		write_unlock(&mpset->rblock);
		if (mc->size > MEMPOOL_KMALLOC_MAX_SIZE) {
			dma_free_coherent(mpset->pdev, mc->size, mc->va, mc->pa);
		} else {
			kfree(mc->va);
			mc->va = NULL;
		}
		mpset->host_mem_size -= mc->size;
	} else if (mc->mem_location == MEM_LOC_DEVICE) {
		struct mempool *mp;
		mp = &mpset->mp_device[mc->dram_channel][mc->dram_region];
		list_del(&mc->device_allocated_list);
		gen_pool_free(mp->gen_pool, (u64)mc->va, mc->size);
		mc->va = NULL;
		mp->allocated_size -= mc->size;
		mpset->device_mem_size -= mc->size;
	} else {
		BUG();
	}

	*mcp = NULL;
	mutex_unlock(&mpset->lock);

	kfree(mc);
}
