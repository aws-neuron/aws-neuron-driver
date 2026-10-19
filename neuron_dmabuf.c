// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#include "neuron_dmabuf.h"
#include "neuron_device.h"
#include "neuron_mmap.h"
#include "neuron_pci.h"

#include <linux/overflow.h>

/* Below is a short description of how dma-buf works (with libfabric and EFA driver)
 * - Memory Registration
 *   - Libfabric (user-space) requests a dmabuf file-descriptor (FD)
 *      - Neuron driver creates a new dmabuf object and installs a new FD
 *   - Libfabric passes the FD to ibcore
 *   - Then these APIs get invoked in Neuron driver:
 *      - ndmabuf_attach
 *      - ndmabuf_map
 * - Memory Deregistration
 *   - These APIs get invoked in Neuron driver:
 *      - ndmabuf_unmap
 *      - ndmabuf_release
 *
 * See internal documentation: Moving-to-dma-buf-for-EFA-Memory-Registration for more information.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

#define SG_MAX_LEN ALIGN_DOWN((u64)U32_MAX, PAGE_SIZE)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)) || \
    (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(10, 0)))
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
#endif

/* Private context attached to a dmabuf object */
struct ndmabuf_private_data {
	/* Virtual address in userspace */
	void *va;

	/* Page-aligned virtual address */
	void *aligned_va;

	/* Neuron device index associated with the dmabuf object */
	int device_idx;
};

/* Invoked when an external driver is being attached to a dmabuf object */
static int ndmabuf_attach(struct dma_buf * dmabuf, struct dma_buf_attachment *attachment)
{
	return 0;
}

/* Invoked when an external driver wants to retrieve pages
 * (physical addresses) of a Neuron device buffer */
static struct sg_table *ndmabuf_map(struct dma_buf_attachment *attachment, enum dma_data_direction dir)
{
	struct ndmabuf_private_data *private_data;
	struct neuron_device *nd;
	struct nmmap_node *mmap;
	struct scatterlist *sg;
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	u64 pa, remaining;
	int sg_entries;
	int sg_idx;
	int ret;

	dmabuf = attachment->dmabuf;
	private_data = dmabuf->priv;
	if (private_data == NULL) {
		pr_err("ndmabuf_map: Neuron context (private data) in dmabuf was freed prematurely!");
		return ERR_PTR(-EINVAL);
	}

	/* Find the matching mmap node in the device.
	 * Populate sg_table using the information in mmap.
	 * Also, store some context to the private data inside dmabuf object. */
	nd = neuron_pci_get_device(private_data->device_idx);
	if (nd == NULL) {
		pr_err("ndmabuf_map: Failed to retrieve nd%d, is the device closed?\n",
				private_data->device_idx);
		return ERR_PTR(-EINVAL);
	}

	/* Device memory is physically contiguous within an mmap region, so use
	 * max-sized SG entries (capped at U32_MAX-PAGE_SIZE due to the 32-bit
	 * scatterlist length field). Page size optimization will be handled by
	 * EFA driver via IB subsystem's ib_umem_find_best_pgsz() */
	sg_entries = DIV_ROUND_UP_ULL(dmabuf->size, SG_MAX_LEN);
	
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, sg_entries, GFP_KERNEL);
	if (ret) {
		goto err_free_sgt;
	}

	write_lock(&nd->mpset.rbmmaplock);

	mmap = nmmap_search_va(nd, private_data->va);
	if (mmap == NULL) {
		pr_err("ndmabuf_map: mmap node (nd:%d va:0x%llx) was freed prematurely!\n",
				private_data->device_idx, (u64)private_data->va);
		ret = -EINVAL;
		goto err_unlock;
	}

	pa = mmap->pa + (private_data->aligned_va - mmap->va);

	remaining = dmabuf->size;
	for_each_sgtable_dma_sg(sgt, sg, sg_idx) {
		u32 len = min_t(u64, remaining, SG_MAX_LEN);
		sg_dma_address(sg) = pa;
		sg_dma_len(sg) = len;
		pa += len;
		remaining -= len;
	}

	WARN_ON(remaining != 0);

	/* Increment the usage count */
	mmap->dmabuf_ref_cnt++;

	write_unlock(&nd->mpset.rbmmaplock);

	return sgt;

err_unlock:
	write_unlock(&nd->mpset.rbmmaplock);

err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);

	return ERR_PTR(ret);
}

/* Invoked when an external driver is done with the pages */
static void ndmabuf_unmap(
		struct dma_buf_attachment *attachment,
		struct sg_table *sgt,
		enum dma_data_direction dir) {
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ndmabuf_private_data *private_data = dmabuf->priv;

	if (private_data == NULL) {
		pr_err("ndmabuf_unmap: Neuron context (private data) in dmabuf was freed prematurely!");
		return;
	}

	struct neuron_device *nd = neuron_pci_get_device(private_data->device_idx);
	if (nd == NULL) {
		pr_err("ndmabuf_unmap: Failed to retrieve nd%d, is the device closed?\n",
				private_data->device_idx);
		BUG_ON(true); /* Very bad news - no point in moving further */
	}

	write_lock(&nd->mpset.rbmmaplock);
	struct nmmap_node *mmap = nmmap_search_va(nd, private_data->va);
	/* It is okay for the above search to come up empty. When an application
	 * is terminated between memory registration and de-registration,
	 * mmap node may get freed/released before this function is called. */
	if (mmap != NULL) {
		/* Decrement the usage count */
		if (mmap->dmabuf_ref_cnt == 0) {
			pr_err("ndmabuf_unmap: dmabuf reference count for va:0x%llx is already zero!\n",
					(u64)private_data->va);
			BUG_ON(true); /* Very bad news - no point in moving further */
		}
		mmap->dmabuf_ref_cnt--;
	}
	write_unlock(&nd->mpset.rbmmaplock);

	sg_free_table(sgt);
	kfree(sgt);
}

/* Invoked when the dmabuf object is being freed */
static void ndmabuf_release(struct dma_buf *dmabuf)
{
	struct ndmabuf_private_data *private_data = dmabuf->priv;
	if (private_data == NULL) {
		pr_err("ndmabuf_release: Neuron context (private data) in dmabuf was freed prematurely!");
		return;
	}
	kfree(private_data);
}

static const struct dma_buf_ops ndmabuf_ops = {
	.attach = ndmabuf_attach,
	.map_dma_buf = ndmabuf_map,
	.unmap_dma_buf = ndmabuf_unmap,
	.release = ndmabuf_release,
};

/* Create a new dmabuf object and retrieve its fd */
int ndmabuf_get_fd(u64 va, u64 size, int *dmabuf_fd, u64 *offset)
{
	struct dma_buf *dmabuf;
	struct ndmabuf_private_data *private_data;
	struct nmmap_node *mmap;
	int device_idx;
	bool mmap_found;
	int fd, ret;
	u64 va_end, aligned_va, aligned_size, page_offset;

	private_data = kzalloc(sizeof(struct ndmabuf_private_data), GFP_KERNEL);
	if (private_data == NULL) {
		return -ENOMEM;
	}
	private_data->va = (void *)va;

	aligned_va = ALIGN_DOWN(va, PAGE_SIZE);
	page_offset = va - aligned_va;
	if (check_add_overflow(page_offset, size, &aligned_size)) {
		ret = -EINVAL;
		goto err_free_private_data;
	}

	private_data->aligned_va = (void *)aligned_va;

	/* Detect invalid VA/size by iterating over all available neuron devices to
	 * find the matching mmap node */
	mmap_found = 0;
	for (device_idx = 0; device_idx < MAX_NEURON_DEVICE_COUNT; device_idx++) {
		struct neuron_device *nd = neuron_pci_get_device(device_idx);
		if (!nd)
			continue;

		write_lock(&nd->mpset.rbmmaplock);
		mmap = nmmap_search_va(nd, private_data->va);
		if (mmap != NULL) {
			if (check_add_overflow(va, size, &va_end) || ((va_end - (u64)mmap->va) > mmap->size)) {
				write_unlock(&nd->mpset.rbmmaplock);
				pr_err("ndmabuf_get_fd: va + size exceeds mmap region\n");
				ret = -EINVAL;
				goto err_free_private_data;
			}
			private_data->device_idx = device_idx;
			mmap_found = true;
			write_unlock(&nd->mpset.rbmmaplock);
			break;
		}
		write_unlock(&nd->mpset.rbmmaplock);
	}

	if (!mmap_found) {
		/* No mmap was found after iterating over all available devices */
		pr_err("No matching memory was found with va=0x%llx after searching all neuron devices\n", va);
		ret = -EINVAL;
		goto err_free_private_data;
	}

	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	exp_info.ops = &ndmabuf_ops;
	exp_info.size = aligned_size;
	exp_info.flags = O_CLOEXEC;
	exp_info.priv = private_data;

	/* On success, dma_buf_export assigns ownership of private_data to the
	 * dmabuf object via exp_info.priv; it will be freed in ndmabuf_release. */
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		pr_err("error %d while exporting dma-buf\n", ret);
		goto err_free_private_data;
	}

	fd = dma_buf_fd(dmabuf, exp_info.flags);
	if (fd < 0) {
		if (fd == -EMFILE) {
			pr_err("dma_buf_fd failed: too many open files\n");
		} else {
			pr_err("error %d while installing a file descriptor for dma-buf\n", fd);
		}
		ret = fd;
		goto err_dma_buf_put;
	}

	*dmabuf_fd = fd;
	*offset = page_offset;

	return 0;

err_dma_buf_put:
	/* dma_buf_put drops the last file ref, triggering dma_buf_release ->
	 * ndmabuf_release -> kfree(private_data). Must not fall through to
	 * err_free_private_data or private_data will be freed twice. */
	dma_buf_put(dmabuf);
	return ret;

err_free_private_data:
	kfree(private_data);
	return ret;
}

#else // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

int ndmabuf_get_fd(u64 va, u64 size, int *dmabuf_fd, u64 *offset)
{
	return -EPROTONOSUPPORT;
}

#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

