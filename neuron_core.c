// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Each neuron device has N number of neuron cores.
 *
 * Engines:
 * -------
 * Neuron Core has multiple engines(Tensor, Scalar, GpSimd, Vector, and/or Sync) which can do different types of computations.
 * Each engine's instruction stream is feed through DMA.
 *
 * Semaphores and events:
 * ---------------------
 * For synchronization between hardware blocks and software, NC provides two type synchronization
 * hardware primitives, semaphores and events. Events can be considered simple bitmap which hold
 * either 1 or 0. Semaphores hold any value in signed 32 bit range. Engines can be programmed
 * with instructions which can wait for semaphore to reach a certain value or a particular event
 * is set. Applications can use this to manipulate execution of the program.
 *
 * mmap:
 * ----
 * The following can be mmap() into application's address space.
 *  1. Host DMAable memory
 *  2. Neuron Core's Notification queue
 * Host DMAable memory can be mmapped so that applications can do initialization/compute before
 * it is DMAed to device memory. This avoids a memcpy() from user space to kernel. A total of
 * 16GB per device is allowed to be mmaped.
 *
 * mmap() of notification queue allows application to do polling on the host memory instead of waiting
 * for an interrupt.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "neuron_mempool.h"
#include "neuron_device.h"
#include "neuron_dhal.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_nc_mmap);
#endif

int nc_semaphore_read(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 *result)
{
	int ret = 0;
	void *addr;

	if (semaphore_index >= ndhal->ndhal_address_map.semaphore_count)
		return -EINVAL;

	ret = ndhal->ndhal_nc.nc_get_semaphore_base(nd, nc_id, &addr);
	if (ret) {
		pr_err("failed to retrieve semaphore base");
		return ret;
	}

	addr += ndhal->ndhal_address_map.mmap_nc_sema_read_offset + (semaphore_index * NC_SEMAPHORE_SIZE);
	return ndhal->ndhal_fw_io.fw_io_read_csr_array((void **)&addr, result, 1, true);
}

int nc_semaphore_write(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	int ret = 0;
	void *addr;

	if (semaphore_index >= ndhal->ndhal_address_map.semaphore_count)
		return -EINVAL;

	ret = ndhal->ndhal_nc.nc_get_semaphore_base(nd, nc_id, &addr);
	if (ret) {
		pr_err("failed to retrieve semaphore base");
		return ret;
	}

	addr += ndhal->ndhal_address_map.mmap_nc_sema_set_offset + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_semaphore_increment(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	int ret = 0;
	void *addr;

	if (semaphore_index >= ndhal->ndhal_address_map.semaphore_count)
		return -EINVAL;

	ret = ndhal->ndhal_nc.nc_get_semaphore_base(nd, nc_id, &addr);
	if (ret) {
		pr_err("failed to retrieve semaphore base");
		return ret;
	}

	addr += ndhal->ndhal_address_map.mmap_nc_sema_incr_offset + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_semaphore_decrement(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	int ret = 0;
	void *addr;

	if (semaphore_index >= ndhal->ndhal_address_map.semaphore_count)
		return -EINVAL;

	ret = ndhal->ndhal_nc.nc_get_semaphore_base(nd, nc_id, &addr);
	if (ret) {
		pr_err("failed to retrieve semaphore base");
		return ret;
	}

	addr += ndhal->ndhal_address_map.mmap_nc_sema_decr_offset + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_event_get(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 *result)
{
	int ret = 0;
	void *addr;

	if (event_index > ndhal->ndhal_address_map.event_count)
		return -EINVAL;

	ret = ndhal->ndhal_nc.nc_get_event_addr(nd, nc_id, event_index, &addr);
	if (ret) {
		pr_err("failed to retrieve event %u addr", event_index);
		return ret;
	}

	return ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, result, 1, true);
}

int nc_event_set(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 value)
{
	int ret = 0;
	void *addr;

	if (event_index > ndhal->ndhal_address_map.event_count)
		return -EINVAL;

	ret = ndhal->ndhal_nc.nc_get_event_addr(nd, nc_id, event_index, &addr);
	if (ret) {
		pr_err("failed to retrieve event %u addr", event_index);
		return ret;
	}

	writel(value, addr);
	return 0;
}

