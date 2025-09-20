// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#include "neuron_dmabuf.h"
#include "neuron_device.h"
#include "neuron_mmap.h"
#include "neuron_pci.h"

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
 *      - ndmabuf_detach
 *      - ndmabuf_release
 *
 * See internal documentation: Moving-to-dma-buf-for-EFA-Memory-Registration for more information.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
#endif

/* Private context attached to a dmabuf object */
struct ndmabuf_private_data {
	/* Virtual address in userspace */
	void *va;

	/* size of the buffer */
	u64 size;

	/* Set to 1 when an external driver is attached to dmabuf object */
	bool is_attached;

	/* File-descriptor associated with the dmabuf object */
	int fd;

	/* Neuron device index associated with the dmabuf object */
	int device_idx;
};

/* Invoked when an external driver is being attached to a dmabuf object */
static int ndmabuf_attach(
		struct dma_buf * dmabuf,
		struct dma_buf_attachment *attachment) {
	struct ndmabuf_private_data *private_data;

	private_data = dmabuf->priv;
	if (private_data == NULL) {
		pr_err("ndmabuf_attach: Neuron context (private data) in dmabuf was freed prematurely!");
		return -EINVAL;
	}

	if (!__sync_bool_compare_and_swap(&private_data->is_attached, 0, 1)) {
		/* An external driver has to detach before calling attach again */
		pr_err("ndmabuf_attach: Only one device is allowed to be attached to a Neuron dmabuf object\n");
		return -EPERM;
	}

	return 0;
}

/* Invoked when an external driver is being detached from a dmabuf object */
static void ndmabuf_detach(
		struct dma_buf * dmabuf,
		struct dma_buf_attachment *attachment) {
	struct ndmabuf_private_data *private_data;
	struct neuron_device *nd;

	private_data = dmabuf->priv;
	if (private_data == NULL) {
		pr_err("ndmabuf_detach: Neuron context (private data) in dmabuf was freed prematurely!");
		return;
	}

	if (!__sync_bool_compare_and_swap(&private_data->is_attached, 1, 0)) {
		pr_err("ndmabuf_detach: dmabuf object is already detached, "
			"multiple detach calls are not allowed for the same dmabuf object\n");
		return;
	}

	nd = neuron_pci_get_device(private_data->device_idx);
	if (nd == NULL) {
		pr_err("ndmabuf_detach: Failed to retrieve nd%d, is the device closed?\n",
				private_data->device_idx);
		BUG_ON(true); /* Very bad news - no point in moving further */
	}

	/* By design, dma-buf doesn't release its FD until the
	 * process exits. That might create a problem (run out of FDs)
	 * for Neuron applications that load/unload multiple NEFFs
	 * within a single process. So, forcefuly mark the FD as
	 * unused to recycle it. Also, put back the associated file.
	 */
	/* Special : Make sure the process is still attached to the device.
	 * This is to cover the case where a process dies and cleans up
	 * FD before detach is called in the dma-buf flow.
	 * For example, when an application dies between memory registration and
	 * de-registration, FD is already uninstalled by the process before
	 * de-registration is called in the kernel */
	if (npid_is_attached(nd)) {
		put_unused_fd(private_data->fd);
		fput(dmabuf->file);
	}

	return;
}

/* Invoked when an external driver wants to retrieve pages
 * (physical addresses) of a Neuron device buffer */
static struct sg_table *ndmabuf_map(
		struct dma_buf_attachment *attachment,
		enum dma_data_direction dir) {
	struct dma_buf *dmabuf;
	struct ndmabuf_private_data *private_data;
	struct scatterlist *sg;
	struct sg_table *sgt;
	int sg_entries, sg_idx, ret;
	u64 pa, size_aligned;
	u32 page_size;

	dmabuf = attachment->dmabuf;
	private_data = dmabuf->priv;
	if (private_data == NULL) {
		pr_err("ndmabuf_map: Neuron context (private data) in dmabuf was freed prematurely!");
		return ERR_PTR(-EINVAL);
	}

	/* NOTE: Theoritically, __sync builtin is not required here but
	 * just being paranoid in case the order of dmabuf function calls
	 * gets messed up outside of Neuron driver. */
	if (!__sync_bool_compare_and_swap(&private_data->is_attached, 1, 1)) {
		pr_err("ndmabuf_map: Must attach() before map()\n");
		return ERR_PTR(-EPERM);
	}

	/* Single entry in scatterlist */
	page_size = PAGE_SIZE;

	/* Find the matching mmap node in the device.
	 * Populate sg_table using the information in mmap.
	 * Also, store some context to the private data inside dmabuf object. */

	struct neuron_device *nd = neuron_pci_get_device(private_data->device_idx);
	if (nd == NULL) {
		pr_err("ndmabuf_detach: Failed to retrieve nd%d, is the device closed?\n",
				private_data->device_idx);
		return ERR_PTR(-EINVAL);
	}

	write_lock(&nd->mpset.rbmmaplock);

	struct nmmap_node *mmap = nmmap_search_va(nd, private_data->va);
	if (mmap == NULL) {
		pr_err("ndmabuf_map: mmap node (nd:%d va:0x%llx) was freed prematurely!\n",
				private_data->device_idx, (u64)private_data->va);
		ret = -EINVAL;
		goto err_unlock;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	size_aligned = ALIGN(dmabuf->size, page_size);
	sg_entries = size_aligned/page_size;
	ret = sg_alloc_table(sgt, sg_entries, GFP_KERNEL | __GFP_ZERO);
	if (ret) {
		goto err_sgt_free;
	}

	pa = mmap->pa + (private_data->va - mmap->va);

	/* Memory allocated & mapped using driver are always PAGE_SIZE aligned.
	 * Make sure the pa we get is page size aligned */
	if (pa % PAGE_SIZE != 0) {
		pr_err("physical address is not %ld aligned for pid:%d", PAGE_SIZE, task_tgid_nr(current));
		ret = -EINVAL;
		goto err_sgt_free;
	}

	/* Populate the sg_table */
	for_each_sgtable_dma_sg(sgt, sg, sg_idx) {
		sg_set_page(sg, NULL, page_size, 0);
		sg_dma_address(sg) = pa;
		sg_dma_len(sg) = page_size;
		pa += page_size;
	}

	/* Increment the usage count */
	mmap->dmabuf_ref_cnt++;

	write_unlock(&nd->mpset.rbmmaplock);

	return sgt;

err_sgt_free:
	kfree(sgt);

err_unlock:
	write_unlock(&nd->mpset.rbmmaplock);
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

	/* NOTE: Theoritically, __sync builtin is not required here but
	 * just being paranoid in case the order of dmabuf function calls
	 * gets messed up outside of Neuron driver. */
	if (!__sync_bool_compare_and_swap(&private_data->is_attached, 1, 1)) {
		pr_err("ndmabuf_unmap: Must attach() before unmap()\n");
		BUG_ON(true); /* Very bad news - no point in moving further */
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
	.detach = ndmabuf_detach,
	.map_dma_buf = ndmabuf_map,
	.unmap_dma_buf = ndmabuf_unmap,
	.release = ndmabuf_release,
};

/* Create a new dmabuf object and retrieve its fd */
int ndmabuf_get_fd(u64 va, u64 size, int *dmabuf_fd)
{
	struct dma_buf *dmabuf;
	struct ndmabuf_private_data *private_data;
	int device_idx;
	bool mmap_found;
	int fd, ret;

	private_data = kzalloc(sizeof(struct ndmabuf_private_data), GFP_KERNEL);
	if (private_data == NULL) {
		return -ENOMEM;
	}
	private_data->is_attached = 0;
	private_data->va = (void *)va;
	private_data->size = size;

	/* Detect invalid VA/size by iterating over all available neuron devices to
	 * find the matching mmap node */
	mmap_found = 0;
	for (device_idx = 0; device_idx < MAX_NEURON_DEVICE_COUNT; device_idx++) {
		struct neuron_device *nd = neuron_pci_get_device(device_idx);
		if (!nd)
			continue;

		write_lock(&nd->mpset.rbmmaplock);
		struct nmmap_node *mmap = nmmap_search_va(nd, private_data->va);
		if (mmap != NULL) {
			write_unlock(&nd->mpset.rbmmaplock);
			mmap_found = 1;
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

	private_data->device_idx = device_idx;

	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	exp_info.ops = &ndmabuf_ops;
	exp_info.size = size;
	exp_info.flags = O_CLOEXEC;
	exp_info.priv = private_data;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		pr_err("error %d while exporting dma-buf\n", ret);
		goto err_free_private_data;
	}

	fd = dma_buf_fd(dmabuf, exp_info.flags);
	if (fd < 0) {
		pr_err("error %d while installing a file descriptor for dma-buf\n", ret);
		ret = -EINVAL;
		goto err_dma_buf_put;
	}

	private_data->fd = fd;
	*dmabuf_fd = fd;

	return 0;

err_dma_buf_put:
	dma_buf_put(dmabuf);

err_free_private_data:
	kfree(private_data);
	return ret;
}

#else // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

int ndmabuf_get_fd(u64 va, u64 size, int *dmabuf_fd)
{
	return -EPROTONOSUPPORT;
}

#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
