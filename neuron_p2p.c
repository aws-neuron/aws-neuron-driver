// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
/** Exposes API to be used for p2p
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/log2.h>

#include "neuron_device.h"
#include "neuron_mmap.h"
#include "neuron_p2p.h"
#include "neuron_pci.h"

#define NEURON_P2P_HUGE_PAGE_SZ 0x200000
#define NEURON_P2P_HUGE_PAGE_SZ_USAGE_THRESHOLD 0x10000000

/*
 * Registers the VA with the callback and also returns the PA
 */
static u64 neuron_p2p_register_and_get_pa(void *va, u64 size, void (*free_callback)(void *data),
                                          void *data, int *device_index)
{
        int i;
        struct neuron_device *nd;

        for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
                nd = neuron_pci_get_device(i);
                if (!nd) {
                        continue;
                }
                write_lock(&nd->mpset.rbmmaplock);
                struct nmmap_node *mmap = nmmap_search_va(nd, va);
                if (mmap != NULL) {
                        // Validate that:
                        // 1. The starting address is within bounds
                        // 2. The size doesn't exceed the mapped region
                        u64 offset = va - mmap->va;
                        if (offset + size > mmap->size) {
                                write_unlock(&nd->mpset.rbmmaplock);
                                pr_err("Invalid size: request size %llu exceeds mapped region size\n",
                                       size);
                                return -EINVAL;
                        }
                        mmap->free_callback = free_callback;
                        mmap->data = data;
                        write_unlock(&nd->mpset.rbmmaplock);
                        *device_index = i;
                        return mmap->pa + (va - mmap->va);
                } else {
                        write_unlock(&nd->mpset.rbmmaplock);
                }
        }
        return 0;
}

int neuron_p2p_register_va(u64 virtual_address, u64 length, struct neuron_p2p_va_info **va_info,
                           void (*free_callback)(void *data), void *data)
{
        int device_index;
        struct neuron_p2p_va_info *vainfo;
        u64 pa = 0;
        u32 entries = 1; // multiple entries support is for future expansion
        u32 page_size;

        if (!va_info) {
                pr_debug("va_info is NULL");
                return -EINVAL;
        }

        if (!virtual_address) {
                pr_debug("virtual_address is NULL");
                return -EINVAL;
        }

        pa = neuron_p2p_register_and_get_pa((void *)virtual_address, length, free_callback, data,
                                            &device_index);
        if (!pa) {
                // this could be a legitimate case, EFA might try registering PA with different
                // drivers, so some PAs might not be ours
                pr_debug("Could not find the physical address va:0x%llx, pid:%d", virtual_address,
                         task_tgid_nr(current));
                return -EINVAL;
        }

        // Memory allocated & mapped using driver are always PAGE_SIZE aligned. Make sure the pa we get
        // is page size aligned
        if (pa % PAGE_SIZE != 0) {
                pr_err("physical address is not %ld aligned for pid:%d", PAGE_SIZE,
                       task_tgid_nr(current));
                return -EINVAL;
        }

        //in the current implementation the device memory is always contiguous and is mapped to a single entry, mutiple entries are supported and could be used in the future
        vainfo = kzalloc(sizeof(struct neuron_p2p_va_info) +
                                 (sizeof(struct neuron_p2p_page_info) * entries),
                         GFP_KERNEL);
        if (!vainfo) {
                pr_err("Could not allocate memory for va info for va:0x%llx, pid:%d",
                       virtual_address, task_tgid_nr(current));
                return -ENOMEM;
        }

        // TODO: First step is to use just page size as default. In the subsequent commit will try to optimize.
        // Page size should be chosen such that we can have the largest page size possible and the
        // smallest page count
        if ((length >= NEURON_P2P_HUGE_PAGE_SZ_USAGE_THRESHOLD) &&
            (length % NEURON_P2P_HUGE_PAGE_SZ == 0) &&
            (virtual_address % NEURON_P2P_HUGE_PAGE_SZ == 0) &&
            (pa % NEURON_P2P_HUGE_PAGE_SZ == 0)) {
                page_size = NEURON_P2P_HUGE_PAGE_SZ;
        } else {
                page_size = PAGE_SIZE;
        }

        vainfo->size = length;
        vainfo->page_info[0].physical_address =
                pa; //just set to pa, since pa is already page size aligned.
        vainfo->page_info[0].page_count = length / page_size;
        vainfo->device_index = device_index;
        vainfo->entries = 1;
        vainfo->virtual_address = (void *)virtual_address;
        vainfo->shift_page_size = fls(page_size) - 1;
        *va_info = vainfo;

        return 0;
}
EXPORT_SYMBOL_GPL(neuron_p2p_register_va);

int neuron_p2p_unregister_va(struct neuron_p2p_va_info *vainfo)
{
        struct neuron_device *nd;

        if (!vainfo) {
                return -1;
        }

        // pci call will catch out of bounds index, but we still want to check for null nd because not all systems have same number of neuron devices
        nd = neuron_pci_get_device(vainfo->device_index);
        if (nd == NULL) {
                pr_debug("Invalid vainfo struct nd: %d , vaddr: %p size: 0x%llx",
                         vainfo->device_index, vainfo->virtual_address, vainfo->size);
                return -1;
        }

        write_lock(&nd->mpset.rbmmaplock);

        if (vainfo->device_index >= MAX_NEURON_DEVICE_COUNT) {
                write_unlock(&nd->mpset.rbmmaplock);
                pr_err("Invalid device index: %d", vainfo->device_index);
                return -EINVAL;
        }

        struct nmmap_node *mmap = nmmap_search_va(nd, vainfo->virtual_address);
        if (mmap != NULL) {
                // Security check: Verify that the device index in vainfo matches the one in the original allocation
                if (mmap->device_index != vainfo->device_index) {
                        write_unlock(&nd->mpset.rbmmaplock);
                        pr_debug("Device index mismatch during unregister");
                        return -EPERM;
                }

                mmap->free_callback = NULL;
                mmap->data = NULL;
        } else {
                write_unlock(&nd->mpset.rbmmaplock);
                return -EINVAL;
        }
        vainfo->device_index = -1;
        write_unlock(&nd->mpset.rbmmaplock);
        kfree(vainfo);
        return 0;
}
EXPORT_SYMBOL_GPL(neuron_p2p_unregister_va);
