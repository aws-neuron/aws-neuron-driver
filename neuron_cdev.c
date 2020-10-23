// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
/** Exposes device node interface(/dev/neuron0) for each device.
 *  see neuron_ioctl.h for all the operations that can be done this node.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/pci.h>

#include "neuron_ioctl.h"
#include "neuron_device.h"
#include "neuron_core.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"
#include "neuron_trace.h"

#include "v1/address_map.h"
#include "v1/fw_io.h"

static dev_t neuron_dev;
static int major;
static struct class *neuron_dev_class;

/* one device node per device */
#define NEURON_MAX_DEV_NODES MAX_NEURON_DEVICE_COUNT

struct ncdev {
	int minor;
	int open_count; // number of times this node is opened.
	struct cdev *cdev;
	struct neuron_device *ndev; // neuron device associated with this device node.
};

/* char device nodes created for each device. */
static struct ncdev devnodes[NEURON_MAX_DEV_NODES];

static u64 ncdev_mem_chunk_to_mem_handle(struct mem_chunk *mc)
{
	return (u64)mc;
}

static struct mem_chunk *ncdev_mem_handle_to_mem_chunk(u64 mh)
{
	return (struct mem_chunk *)mh;
}

static int ncdev_dma_engine_init(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_eng_init arg;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_eng_init *)param, sizeof(arg));
	if (ret)
		return ret;

	return ndmar_eng_init(nd, arg.eng_id);
}

static int ncdev_dma_engine_set_state(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_eng_set_state arg;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_eng_set_state *)param, sizeof(arg));
	if (ret)
		return ret;
	return ndmar_eng_set_state(nd, arg.eng_id, arg.state);
}

static int ncdev_dma_engine_get_state(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_eng_get_state arg;
	struct neuron_dma_eng_state state;
	int ret;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_eng_get_state *)param, sizeof(arg));
	if (ret)
		return ret;
	ret = ndmar_eng_get_state(nd, arg.eng_id, &state);
	if (ret)
		return ret;
	return copy_to_user(arg.state, &state, sizeof(state));
}

static int ncdev_dma_queue_init(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_queue_init arg;
	struct mem_chunk *rx_mc;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rxc_mc;
	int ret;

	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_queue_init *)param, sizeof(arg));
	if (ret)
		return -EACCES;

	rx_mc = ncdev_mem_handle_to_mem_chunk(arg.rx_handle);
	tx_mc = ncdev_mem_handle_to_mem_chunk(arg.tx_handle);
	if (arg.rxc_handle)
		rxc_mc = ncdev_mem_handle_to_mem_chunk(arg.rxc_handle);
	else
		rxc_mc = NULL;
	ret = ndmar_queue_init(nd, arg.eng_id, arg.qid, arg.tx_desc_count, arg.rx_desc_count, tx_mc,
			       rx_mc, rxc_mc, arg.axi_port);
	return ret;
}

static int ncdev_dma_copy_descriptors(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_copy_descriptors arg;
	struct mem_chunk *src_mc;
	u32 offset = 0, copy_size = 0;
	int remaining, ret;

	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_copy_descriptors *)param, sizeof(arg));
	if (ret)
		return ret;

	struct mem_chunk *mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	if (!mc)
		return -EINVAL;
	// check access is within the range.
	if (arg.offset + (arg.num_descs * sizeof(union udma_desc)) > mc->size) {
		ret = -EINVAL;
		goto out;
	}

	remaining = arg.num_descs * sizeof(union udma_desc);
	ret = mc_alloc(&nd->mpset, &src_mc, MAX_DMA_DESC_SIZE, MEM_LOC_HOST, 0, 0, mc->nc_id);
	if (ret) {
		ret = -ENOMEM;
		goto out;
	}
	while (remaining) {
		copy_size = remaining < MAX_DMA_DESC_SIZE ? remaining : MAX_DMA_DESC_SIZE;
		ret = copy_from_user(src_mc->va, arg.buffer + offset, copy_size);
		if (ret) {
			break;
		}
		ret = ndma_memcpy_dma_copy_descriptors(nd, src_mc->va, 0, mc, arg.offset + offset,
						       copy_size, arg.queue_type);
		if (ret) {
			break;
		}
		remaining -= copy_size;
		offset += copy_size;
	}
out:
	mc_free(&src_mc);
	return ret;
}

static int ncdev_dma_copy_start(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_queue_copy_start arg;
	int ret;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_queue_copy_start *)param, sizeof(arg));
	if (ret)
		return ret;

	ret = ndmar_queue_copy_start(nd, arg.eng_id, arg.qid, arg.tx_desc_count, arg.rx_desc_count);
	return ret;
}

static int ncdev_dma_ack_completed(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_ack_completed arg;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_ack_completed *)param, sizeof(arg));
	if (ret)
		return ret;

	return ndmar_ack_completed(nd, arg.eng_id, arg.qid, arg.count);
}

static int ncdev_dma_queue_get_state(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_queue_get_state arg;
	struct neuron_dma_queue_state tx, rx;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_queue_get_state *)param, sizeof(arg));
	if (ret)
		return ret;
	ret = ndmar_queue_get_state(nd, arg.eng_id, arg.qid, &tx, &rx);
	if (ret)
		return ret;
	ret = copy_to_user(arg.tx, &tx, sizeof(tx));
	if (ret)
		return ret;
	return copy_to_user(arg.rx, &rx, sizeof(rx));
}

static int ncdev_dma_queue_release(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_queue_release arg;
	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_queue_release *)param, sizeof(arg));
	if (ret)
		return ret;
	return ndmar_queue_release(nd, arg.eng_id, arg.qid);
}

static int ncdev_dma_descriptor_copyout(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_descriptor_copyout arg;
	struct mem_chunk *tx = NULL, *rx = NULL, *mc = NULL;
	u32 tx_size = 0, rx_size = 0;
	void *addr = NULL;
	u32 desc_size = sizeof(union udma_desc), total_size, offset;
	int ret;

	ret = copy_from_user(&arg, (struct neuron_ioctl_dma_descriptor_copyout *)param,
			     sizeof(arg));
	if (ret)
		return ret;
	if (arg.count == 0)
		return -EINVAL;

	total_size = arg.count * desc_size;
	offset = arg.start_index * desc_size;

	ret = ndmar_queue_get_descriptor_mc(nd, arg.eng_id, arg.qid, &tx, &rx, &tx_size, &rx_size);
	if (ret) {
		pr_err("get DMA queue desc failed %d\n", ret);
		return -EINVAL;
	}

	if (arg.type == NEURON_DMA_QUEUE_TYPE_TX) {
		if (arg.count > tx_size) {
			pr_err("tx size is less than count %d tx %d\n", arg.count, tx_size);
			return -EFBIG;
		}
		mc = tx;
	} else if (arg.type == NEURON_DMA_QUEUE_TYPE_RX) {
		if (arg.count > rx_size) {
			pr_err("rx size is less than count %d rx %d\n", arg.count, rx_size);
			return -EFBIG;
		}
		mc = rx;
	}
	if (mc == NULL)
		return -EINVAL;
	if (mc->mem_location == MEM_LOC_DEVICE) {
		addr = kmalloc(total_size, GFP_KERNEL);
		if (addr == NULL) {
			return -ENOMEM;
		}
		ret = ndma_memcpy_buf_from_mc(nd, addr, 0, mc, offset, total_size);
		if (ret) {
			kfree(addr);
			return ret;
		}
	} else {
		addr = mc->va + offset;
	}

	ret = copy_to_user(arg.buffer, addr, total_size);
	if (mc->mem_location == MEM_LOC_DEVICE)
		kfree(addr);

	return ret;
}

static int ncdev_mem_alloc(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_alloc mem_alloc_arg;
	enum mem_location location;
	u64 mh;
	struct mem_chunk *mc;
	int ret;

	ret = copy_from_user(&mem_alloc_arg, (struct neuron_ioctl_mem_alloc *)param,
			     sizeof(mem_alloc_arg));
	if (ret)
		return -EACCES;
	if (mem_alloc_arg.host_memory)
		location = MEM_LOC_HOST;
	else
		location = MEM_LOC_DEVICE;
	ret = mc_alloc(&nd->mpset, &mc, mem_alloc_arg.size, location, mem_alloc_arg.dram_channel,
		       mem_alloc_arg.dram_region, mem_alloc_arg.nc_id);
	if (ret)
		return ret;

	trace_ioctl_mem_alloc(nd, mc);

	mh = ncdev_mem_chunk_to_mem_handle(mc);
	ret = copy_to_user(mem_alloc_arg.mem_handle, &mh, sizeof(mc));
	if (ret) {
		mc_free(&mc);
		return ret;
	}
	return 0;
}

static int ncdev_mem_get_pa(void *param)
{
	struct neuron_ioctl_mem_get_pa mem_get_pa_arg;
	struct mem_chunk *mc;
	u64 pa;
	int ret;

	ret = copy_from_user(&mem_get_pa_arg, (struct neuron_ioctl_mem_get_pa *)param,
			     sizeof(mem_get_pa_arg));
	if (ret)
		return ret;

	mc = ncdev_mem_handle_to_mem_chunk(mem_get_pa_arg.mem_handle);
	if (mc->mem_location == MEM_LOC_HOST)
		pa = mc->pa | PCIEX8_0_BASE;
	else
		pa = mc->pa;
	return copy_to_user(mem_get_pa_arg.pa, &pa, sizeof(u64));
}

static int ncdev_mem_free(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_free mem_free_arg;
	struct mem_chunk *mc;
	int ret;

	ret = copy_from_user(&mem_free_arg, (struct neuron_ioctl_mem_free *)param,
			     sizeof(mem_free_arg));
	if (ret)
		return ret;
	mc = ncdev_mem_handle_to_mem_chunk(mem_free_arg.mem_handle);
	trace_ioctl_mem_alloc(nd, mc);
	mc_free(&mc);
	return 0;
}

static int ncdev_mem_copy(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_copy arg;
	struct mem_chunk *src_mc;
	struct mem_chunk *dst_mc;
	int ret;

	ret = copy_from_user(&arg, (struct neuron_ioctl_mem_copy *)param, sizeof(arg));
	if (ret)
		return ret;
	src_mc = ncdev_mem_handle_to_mem_chunk(arg.src_mem_handle);
	dst_mc = ncdev_mem_handle_to_mem_chunk(arg.dst_mem_handle);
	// check access is within the range.
	if (arg.src_offset + arg.size > src_mc->size) {
		pr_err("src offset+size is too large for mem handle\n");
		return -EINVAL;
	}
	// check access is within the range.
	if (arg.dst_offset + arg.size > dst_mc->size) {
		pr_err("src offset+size is too large for mem handle\n");
		return -EINVAL;
	}
	ret = ndma_memcpy_mc(nd, src_mc, dst_mc, arg.src_offset, arg.dst_offset, arg.size);
	if (ret) {
		pr_err("dma memcpy failed\n");
		return ret;
	}
	trace_ioctl_mem_copy(nd, src_mc, dst_mc);
	return 0;
}

int ncdev_mem_buf_copy(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_buf_copy arg;
	struct mem_chunk *mc;
	int ret;

	ret = copy_from_user(&arg, (struct neuron_ioctl_mem_buf_copy *)param, sizeof(arg));
	if (ret)
		return ret;
	mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	// check access is within the range.
	if (arg.offset + arg.size > mc->size) {
		pr_err("offset+size is too large for mem handle\n");
		return -EINVAL;
	}

	if (arg.copy_to_mem_handle)
		trace_ioctl_mem_copyin(nd, mc, arg.buffer, arg.offset, arg.size);
	else
		trace_ioctl_mem_copyout(nd, mc, arg.buffer, arg.offset, arg.size);

	if (mc->mem_location == MEM_LOC_HOST) {
		if (arg.copy_to_mem_handle) {
			ret = copy_from_user(mc->va + arg.offset, arg.buffer, arg.size);
		} else {
			ret = copy_to_user(arg.buffer, mc->va + arg.offset, arg.size);
		}
		return ret;
	} else {
		// TODO - this has to be converted to mmap
		struct mem_chunk *src_mc;
		u32 offset = 0;
		int remaining = arg.size;
		u32 copy_size = 0;
		ret = mc_alloc(&nd->mpset, &src_mc, MAX_DMA_DESC_SIZE, MEM_LOC_HOST, 0, 0,
			       mc->nc_id);
		if (ret) {
			ret = -ENOMEM;
			return ret;
		}
		while (remaining) {
			copy_size = remaining < MAX_DMA_DESC_SIZE ? remaining : MAX_DMA_DESC_SIZE;
			if (arg.copy_to_mem_handle) {
				ret = copy_from_user(src_mc->va, arg.buffer + offset, copy_size);
				if (ret) {
					break;
				}
				ret = ndma_memcpy_buf_to_mc(nd, src_mc->va, 0, mc,
							    arg.offset + offset, copy_size);
				if (ret) {
					break;
				}
			} else {
				ret = ndma_memcpy_buf_from_mc(nd, src_mc->va, 0, mc,
							      arg.offset + offset, copy_size);
				if (ret) {
					break;
				}
				ret = copy_to_user(arg.buffer + offset, src_mc->va, copy_size);
				if (ret) {
					break;
				}
			}
			remaining -= copy_size;
			offset += copy_size;
		}
		mc_free(&src_mc);
		return ret;
	}
}

static long ncdev_semaphore_ioctl(struct neuron_device *nd, unsigned int cmd, void *param)
{
	int ret;
	struct neuron_ioctl_semaphore arg;

	ret = copy_from_user(&arg, (struct neuron_ioctl_semaphore *)param, sizeof(arg));
	if (ret)
		return ret;
	if (cmd == NEURON_IOCTL_SEMAPHORE_READ) {
		ret = nc_semaphore_read(nd, arg.nc_id, arg.semaphore_index, &arg.value);
		if (ret)
			return ret;
		return copy_to_user((struct neuron_ioctl_semaphore *)param, &arg, sizeof(arg));
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_WRITE) {
		return nc_semaphore_write(nd, arg.nc_id, arg.semaphore_index, arg.value);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_INCREMENT) {
		return nc_semaphore_increment(nd, arg.nc_id, arg.semaphore_index, arg.value);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_DECREMENT) {
		return nc_semaphore_decrement(nd, arg.nc_id, arg.semaphore_index, arg.value);
	}
	return -1;
}

static long ncdev_events_ioctl(struct neuron_device *nd, unsigned int cmd, void *param)
{
	int ret;
	struct neuron_ioctl_event arg;

	ret = copy_from_user(&arg, (struct neuron_ioctl_event *)param,
			     sizeof(struct neuron_ioctl_event));
	if (ret)
		return ret;

	if (cmd == NEURON_IOCTL_EVENT_GET) {
		ret = nc_event_get(nd, arg.nc_id, arg.event_index, &arg.value);
		if (ret) {
			return ret;
		}
		return copy_to_user((struct neuron_ioctl_event *)param, &arg, sizeof(arg));
	} else if (cmd == NEURON_IOCTL_EVENT_SET) {
		return nc_event_set(nd, arg.nc_id, arg.event_index, arg.value);
	}
	return -1;
}

static long ncdev_bar_read(struct neuron_device *nd, u8 bar, u64 *reg_addresses, void *user_va,
			   u32 data_count)
{
	int ret;
	u64 data_size = data_count * sizeof(u32);
	if (bar == 0) {
		u32 *data = NULL;
		data = kmalloc(data_size, GFP_KERNEL);
		if (data == NULL)
			return -ENOMEM;
		ret = fw_io_read_csr_array((void **)reg_addresses, data, data_count);
		if (ret) {
			kfree(data);
			return ret;
		}
		ret = copy_to_user(user_va, data, data_size);
		kfree(data);
	} else {
		struct mem_chunk *mc;
		u32 nc_id = 0;
		dma_addr_t src_addr = reg_addresses[0];

		ret = mc_alloc(&nd->mpset, &mc, data_size, MEM_LOC_HOST, 0, 0, nc_id);
		if (ret)
			return -ENOMEM;

		ret = ndma_memcpy(nd, mc->nc_id, src_addr, mc->pa | PCIEX8_0_BASE, data_size);
		if (ret) {
			mc_free(&mc);
			return ret;
		}
		ret = copy_to_user(user_va, mc->va, data_size);
		mc_free(&mc);
	}
	return ret;
}

static long ncdev_bar_write(struct neuron_device *nd, u8 bar, u64 *reg_addresses, void *user_va,
			    u32 data_count)
{
	int ret = 0;
	u32 *data = NULL;
	u64 data_size = data_count * sizeof(u32);

	data = kmalloc(data_size, GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;
	ret = copy_from_user(data, user_va, data_size);
	if (ret)
		goto done;

	/*
	 * For BAR0 the addresses are passed as array(random access).
	 * For BAR2 a single address is provided and driver does sequential writes.
	 */
	if (bar == 0) {
		int i;
		for (i = 0; i < data_count; i++) {
			u64 off = reg_addresses[i] - (u64)nd->npdev.bar0;
			if (off > nd->npdev.bar0_size) {
				ret = -EINVAL;
				goto done;
			}
			writel(data[i], nd->npdev.bar0 + off);
		}
	} else {
		int i;
		u64 off = reg_addresses[0] - (u64)nd->npdev.bar2;
		for (i = 0; i < data_count; i++, off += sizeof(u32)) {
			if (off > nd->npdev.bar2_size) {
				ret = -EINVAL;
				goto done;
			}
			writel(data[i], nd->npdev.bar2 + off);
		}
	}
done:
	kfree(data);

	return ret;
}

static long ncdev_bar_rw(struct neuron_device *nd, void *param, bool read)
{
	int ret;
	struct neuron_ioctl_bar_rw arg;
	u64 *reg_addresses = NULL;
	u64 address_count;

	ret = copy_from_user(&arg, (struct neuron_ioctl_bar *)param, sizeof(arg));
	if (ret)
		return ret;

	/* BAR2 reads are always sequential and so addresses are autogenerated from base*/
	if (arg.bar == 0)
		address_count = arg.count;
	else
		address_count = 1;

	reg_addresses = kmalloc(address_count * sizeof(u64), GFP_KERNEL);
	if (reg_addresses == NULL)
		return -ENOMEM;

	ret = copy_from_user(reg_addresses, arg.address, address_count * sizeof(u64));
	if (ret != 0)
		goto done;

	if (read)
		ret = ncdev_bar_read(nd, arg.bar, reg_addresses, arg.data, arg.count);
	else
		ret = ncdev_bar_write(nd, arg.bar, reg_addresses, arg.data, arg.count);

done:
	kfree(reg_addresses);
	return ret;
}

static long ncdev_post_metric(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_post_metric arg;
	u32 *data = NULL;

	ret = copy_from_user(&arg, (struct neuron_ioctl_post_metric *)param, sizeof(arg));
	if (ret) {
		return ret;
	}

	data = kmalloc(arg.data_size, GFP_KERNEL);
	if (data == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	ret = copy_from_user(data, arg.data, arg.data_size);
	if (ret)
		goto done;
	ret = fw_io_post_metric(nd->fw_io_ctx, (u8 *)data, arg.data_size);
done:
	kfree(data);
	return ret;
}

static long ncdev_read_hw_counters(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_read_hw_counters arg;
	uint64_t *reg_addresses = NULL;
	uint32_t *data = NULL;

	ret = copy_from_user(&arg, (struct neuron_ioctl_read_hw_counters *)param, sizeof(arg));
	if (ret)
		return ret;

	reg_addresses = kmalloc(arg.count * sizeof(uint64_t), GFP_KERNEL);
	if (reg_addresses == NULL)
		return -ENOMEM;
	ret = copy_from_user(reg_addresses, arg.address, arg.count * sizeof(uint64_t));
	if (ret != 0)
		goto done;

	data = kmalloc(arg.count * sizeof(uint32_t), GFP_KERNEL);
	if (data == NULL)
		goto done;

	ret = fw_io_read_counters(nd->fw_io_ctx, reg_addresses, data, arg.count);
	if (ret)
		goto done;
	ret = copy_to_user(arg.data, data, arg.count * sizeof(uint32_t));
done:
	kfree(reg_addresses);
	kfree(data);
	return ret;
}

static long ncdev_device_reset(struct neuron_device *nd)
{
	fw_io_initiate_reset(nd->npdev.bar0);
	return 0;
}

static long ncdev_device_reset_status(struct neuron_device *nd, void *param)
{
	bool ret;
	u8 result = 0;
	ret = fw_io_is_reset_initiated(nd->npdev.bar0);
	if (ret) {
		result = 1;
	}
	return copy_to_user(param, &result, 1);
}

static long ncdev_device_ready(struct neuron_device *nd, void *param)
{
	u8 result;
	result = fw_io_is_device_ready(nd->npdev.bar0);
	return copy_to_user(param, &result, 1);
}

/* only one process can do discovery at a time */
static DEFINE_MUTEX(ncdev_discovery_lock);
static long ncdev_device_info(struct neuron_device *nd, void *param)
{
	int i, ret;
	struct neuron_ioctl_device_info result;

	result.architecture = nd->architecture;
	result.revision = nd->revision;

	mutex_lock(&ncdev_discovery_lock);

	/**
	 * UMD runtime directly access PCI config space to enable/disable device.
	 * When UMD runtime process is stopped it would remove master and memory enable bits from device.
	 * If system had driver and UMD runtime is installed then when installing KMD runtime,
	 * the UMD runtime is stopped(which will make Mem- and BusMaster- on device as cleanup) and
	 * KMD runtime process is started. The KMD runtime then would fail to access the device
	 * because of "Mem-".
	 *
	 * To avoid this always directly write to config space until old runtime goes away.
	 * We cant use pci_enable_device() since the device is already enabled during driver start.
	 */
	pci_write_config_word(nd->pdev, PCI_COMMAND, PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);

	// if topology discovery is not yet done, do it and cache the result
	if (nd->connected_device_count <= 0) {
		ret = fw_io_topology(nd->fw_io_ctx, nd->connected_devices,
				     &nd->connected_device_count);
		if (ret) {
			ret = -EFAULT;
			goto out;
		}
	}

	for (i = 0; i < nd->connected_device_count; i++) {
		result.connected_devices[i] = nd->connected_devices[i];
	}
	result.bar_address[0] = (u64)nd->npdev.bar0;
	result.bar_size[0] = nd->npdev.bar0_size;
	result.bar_address[1] = (u64)nd->npdev.bar2;
	result.bar_size[1] = nd->npdev.bar2_size;

	result.connected_device_count = nd->connected_device_count;
	ret = copy_to_user(param, &result, sizeof(result));

out:
	mutex_unlock(&ncdev_discovery_lock);
	return ret;
}

/* Only one process can take ownership of a device. */
static DEFINE_MUTEX(ncdev_device_lock);
static long ncdev_device_init(struct neuron_device *nd, void *param)
{
	int ret = 0;
	struct neuron_ioctl_device_init arg;
	u64 device_dram_addr[V1_MAX_DRAM_CHANNELS] = { P_0_DRAM_0_BASE, P_0_DRAM_1_BASE };
	u64 device_dram_size[V1_MAX_DRAM_CHANNELS] = { P_0_DRAM_0_SIZE, P_0_DRAM_1_SIZE };

	mutex_lock(&ncdev_device_lock);
	ret = copy_from_user(&arg, (struct neuron_ioctl_device_init *)param, sizeof(arg));
	if (ret) {
		return ret;
	}

	if (nd->current_pid == 0) {
		ret = mpset_device_init(&nd->mpset, V1_MAX_DRAM_CHANNELS, arg.mem_regions,
					device_dram_addr, device_dram_size);
		if (ret)
			goto done;
		nd->current_pid = task_tgid_nr(current);
		nd->current_pid_open_count = 1; //since the ioctl done after open set to 1
		ret = ndmar_init(nd);
	} else if (nd->current_pid != task_tgid_nr(current)) {
		pr_err("device inuse by pid:%d\n", nd->current_pid);
		ret = -EBUSY;
	}

done:
	mutex_unlock(&ncdev_device_lock);
	return ret;
}

static long ncdev_device_release(struct ncdev *dev, struct neuron_device *nd)
{
	int ret = 0;
	mutex_lock(&ncdev_device_lock);

	if ((nd->current_pid_open_count == 0) && ((nd->current_pid == task_tgid_nr(current)) || nd->current_pid == task_ppid_nr(current))) {
		nd->current_pid = 0;
	}

	if (dev->open_count == 0) {
		nc_nq_destroy_all(nd);
		ndmar_close(nd);
		mpset_free_all(&nd->mpset);
		nd->mpset.num_regions = 0;
	}

	mutex_unlock(&ncdev_device_lock);
	return ret;
}

static long ncdev_device_app_pid(struct neuron_device *nd, void *param)
{
	return copy_to_user(param, &nd->current_pid, sizeof(int));
}

static long ncdev_nc_nq_init(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_notifications_init arg;
	int ret;

	ret = copy_from_user(&arg, (struct neuron_ioctl_notifications_init *)param, sizeof(arg));
	if (ret) {
		return ret;
	}

	ret = nc_nq_init(nd, arg.nc_id, arg.engine_index, arg.nq_type, arg.size);
	if (ret) {
		return ret;
	}
	ret = nc_get_nq_mmap_offset(arg.nc_id, arg.engine_index, arg.nq_type, &arg.mmap_offset);
	if (ret) {
		return ret;
	}
	return copy_to_user(&((struct neuron_ioctl_notifications_init *)param)->mmap_offset,
			    &arg.mmap_offset, sizeof(arg.mmap_offset));
}

static long ncdev_nc_nq_destroy(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_notifications_destroy arg;
	int ret, nc_id, nq_type, eng_index;

	ret = copy_from_user(&arg, param, sizeof(arg));
	if (ret) {
		return ret;
	}

	ret = nc_get_nq_from_mmap_offset(arg.mmap_offset, &nc_id, &eng_index, &nq_type);
	if (ret) {
		return ret;
	}
	return nc_nq_destroy(nd, nc_id, eng_index, nq_type);
}

long ncdev_ioctl(struct file *filep, unsigned int cmd, unsigned long param)
{
	struct ncdev *ncd;
	struct neuron_device *nd;

	ncd = filep->private_data;
	if (ncd == NULL) {
		return -EINVAL;
	}
	nd = ncd->ndev;
	if (nd == NULL) {
		return -EINVAL;
	}
	// the following IOCTL allowed only for the process which did DEVICE_INIT
	if (cmd == NEURON_IOCTL_DEVICE_RESET || cmd == NEURON_IOCTL_DEVICE_RESET_STATUS ||
	    cmd == NEURON_IOCTL_DMA_ENG_INIT || cmd == NEURON_IOCTL_DMA_ENG_SET_STATE ||
	    cmd == NEURON_IOCTL_DMA_QUEUE_INIT || cmd == NEURON_IOCTL_DMA_ACK_COMPLETED ||
	    cmd == NEURON_IOCTL_DMA_QUEUE_RELEASE || cmd == NEURON_IOCTL_DMA_COPY_DESCRIPTORS ||
	    cmd == NEURON_IOCTL_MEM_ALLOC || cmd == NEURON_IOCTL_MEM_FREE ||
	    cmd == NEURON_IOCTL_MEM_COPY || cmd == NEURON_IOCTL_MEM_GET_PA ||
	    cmd == NEURON_IOCTL_BAR_WRITE || cmd == NEURON_IOCTL_POST_METRIC ||
	    cmd == NEURON_IOCTL_NOTIFICATIONS_INIT || cmd == NEURON_IOCTL_NOTIFICATIONS_DESTROY) {
		if (nd->current_pid != task_tgid_nr(current)) {
			return -EACCES;
		}
	}

	if (cmd == NEURON_IOCTL_DEVICE_RESET) {
		return ncdev_device_reset(nd);
	} else if (cmd == NEURON_IOCTL_DEVICE_RESET_STATUS) {
		return ncdev_device_reset_status(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_READY) {
		return ncdev_device_ready(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_INFO) {
		return ncdev_device_info(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_INIT) {
		return ncdev_device_init(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_RELEASE) {
		return ncdev_device_release(ncd, nd);
	} else if (cmd == NEURON_IOCTL_DEVICE_APP_PID) {
		return ncdev_device_app_pid(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_ENG_INIT) {
		return ncdev_dma_engine_init(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_ENG_SET_STATE) {
		return ncdev_dma_engine_set_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_INIT) {
		return ncdev_dma_queue_init(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_COPY_START) {
		return ncdev_dma_copy_start(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_ACK_COMPLETED) {
		return ncdev_dma_ack_completed(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_RELEASE) {
		return ncdev_dma_queue_release(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_COPY_DESCRIPTORS) {
		return ncdev_dma_copy_descriptors(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_ENG_GET_STATE) {
		return ncdev_dma_engine_get_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_GET_STATE) {
		return ncdev_dma_queue_get_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_DESCRIPTOR_COPYOUT) {
		return ncdev_dma_descriptor_copyout(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_ALLOC) {
		return ncdev_mem_alloc(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_GET_PA) {
		return ncdev_mem_get_pa((void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_FREE) {
		return ncdev_mem_free(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_COPY) {
		return ncdev_mem_copy(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_BUF_COPY) {
		return ncdev_mem_buf_copy(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_READ) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_WRITE) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_INCREMENT) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_DECREMENT) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_EVENT_GET) {
		return ncdev_events_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_EVENT_SET) {
		return ncdev_events_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_BAR_READ) {
		return ncdev_bar_rw(nd, (void *)param, true);
	} else if (cmd == NEURON_IOCTL_BAR_WRITE) {
		return ncdev_bar_rw(nd, (void *)param, false);
	} else if (cmd == NEURON_IOCTL_POST_METRIC) {
		return ncdev_post_metric(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_INIT) {
		return ncdev_nc_nq_init(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_DESTROY) {
		return ncdev_nc_nq_destroy(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_READ_HW_COUNTERS) {
		return ncdev_read_hw_counters(nd, (void *)param);
	} else {
		pr_err("invalid IOCTL %d\n", cmd);
		return -EINVAL;
	}
}

static int ncdev_open(struct inode *inode, struct file *filep)
{
	struct ncdev *dev;
	struct neuron_device *nd;
	dev = &devnodes[iminor(inode)];
	nd = dev->ndev;
	if (!dev) {
		pr_err("unable to lock device\n");
		return -ENODEV;
	}
	mutex_lock(&ncdev_device_lock);
	dev->open_count++;
	if (nd && (nd->current_pid == task_tgid_nr(current) || nd->current_pid == task_ppid_nr(current))) {
		nd->current_pid_open_count++;
	}
	mutex_unlock(&ncdev_device_lock);
	filep->private_data = dev;
	return 0;
}

static int ncdev_close(struct inode *inode, struct file *filep)
{
	struct ncdev *dev = (struct ncdev *)filep->private_data;
	struct neuron_device *nd = dev->ndev;
	mutex_lock(&ncdev_device_lock);
	dev->open_count--;
	if (nd && (nd->current_pid == task_tgid_nr(current) || nd->current_pid == task_ppid_nr(current))) {
		nd->current_pid_open_count--;
	}
	mutex_unlock(&ncdev_device_lock);

	return ncdev_device_release(dev, nd);
}

static int ncdev_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct ncdev *ncd;
	struct neuron_device *nd;
	int ret, nc_id, eng_index, nq_type;
	u64 offset;

	ncd = filep->private_data;
	if (ncd == NULL) {
		return -EINVAL;
	}
	nd = ncd->ndev;
	if (nd == NULL) {
		return -EINVAL;
	}
	offset = vma->vm_pgoff * PAGE_SIZE;
	ret = nc_get_nq_from_mmap_offset(offset, &nc_id, &eng_index, &nq_type);
	if (ret) {
		return ret;
	}

	return nc_nq_mmap(nd, nc_id, eng_index, nq_type, vma);
}

static struct file_operations ncdev_fops = {
	.owner = THIS_MODULE,
	.open = ncdev_open,
	.release = ncdev_close,
	.unlocked_ioctl = ncdev_ioctl,
	.mmap = ncdev_mmap,
};

#define NEURON_MAX_DEV_NAME 32
int ncdev_create_device_node(struct neuron_device *ndev)
{
	int ret, minor;
	struct cdev *cdev;
	dev_t devno;
	struct device *device = NULL;
	char dev_name[NEURON_MAX_DEV_NAME];

	snprintf(dev_name, sizeof(dev_name), "neuron%d", ndev->device_index);

	minor = ndev->device_index;
	devnodes[ndev->device_index].minor = minor;

	devno = MKDEV(major, minor);
	cdev = cdev_alloc();
	if (cdev == NULL) {
		return -1;
	}
	cdev_init(cdev, &ncdev_fops);
	cdev->owner = THIS_MODULE;

	/* register cdev */
	ret = cdev_add(cdev, devno, 1);
	if (ret < 0) {
		pr_err("failed to register character device %s\n", dev_name);
		cdev_del(cdev);
		return -1;
	}

	devnodes[minor].cdev = cdev;
	devnodes[minor].ndev = ndev;

	device = device_create(neuron_dev_class, NULL, /* no parent device */
			       devno, NULL, /* no additional data */
			       "%s", dev_name);

	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		pr_err("error %d while trying to create %s\n", ret, dev_name);
		device_destroy(neuron_dev_class, devno);
		cdev_del(cdev);
		return ret;
	}

	ndev->cdev = &devnodes[minor];
	return 0;
}

int ncdev_delete_device_node(struct neuron_device *ndev)
{
	int minor;
	dev_t devno;

	minor = devnodes[ndev->device_index].minor;
	devno = MKDEV(major, minor);
	device_destroy(neuron_dev_class, devno);
	cdev_del(devnodes[minor].cdev);
	memset(&devnodes[ndev->device_index], 0, sizeof(devnodes[0]));

	return 0;
}

static void ncdev_cleanup(void)
{
	int i;
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
		if (!devnodes[i].ndev)
			continue;
		ncdev_delete_device_node(devnodes[i].ndev);
	}

	if (neuron_dev_class) {
		class_destroy(neuron_dev_class);
	}

	unregister_chrdev_region(MKDEV(major, 0), NEURON_MAX_DEV_NODES);
}

int ncdev_module_init(void)
{
	int ret;

	memset(devnodes, 0, sizeof(devnodes));

	ret = alloc_chrdev_region(&neuron_dev, 0, NEURON_MAX_DEV_NODES, "neuron");
	if (ret < 0) {
		pr_err("can't get major\n");
		return ret;
	}

	major = MAJOR(neuron_dev);

	neuron_dev_class = class_create(THIS_MODULE, "neuron_device");
	if (IS_ERR(neuron_dev_class)) {
		ret = PTR_ERR(neuron_dev_class);
		goto fail;
	}

	return ret;

fail:
	ncdev_cleanup();
	return ret;
}

void ncdev_module_exit(void)
{
	ncdev_cleanup();
}
