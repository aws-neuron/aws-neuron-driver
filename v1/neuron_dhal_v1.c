// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/
#include <linux/pci.h>
#include <linux/delay.h>

#include "../neuron_reset.h"
#include "../neuron_dhal.h"
#include "../neuron_topsp.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "../neuron_fw_io.h"
#include "../neuron_pci.h"
#include "../neuron_trace.h"
#include "../neuron_cdev.h"
#include "fw_io.h"
#include "putils.h"
#include "tdma.h"

#define V1_NR_RESET_INIT_MAX_TOTAL_WAIT_TIME_MS         (1000 * 120)
#define V1_NR_RESET_POLL_INTERVAL                       500
#define V1_NR_DEVICE_RESET_INITIAL_POLL_DELAY           7000
#define V1_NR_TPB_RESET_INITIAL_POLL_DELAY              3000


struct neuron_dm_special_mmap_ent dm_mmap_special_v1[] = {
	{NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

struct ncdev_mem_region ncdev_mem_regions_v1[] = {
	{ V1_MMAP_TPB_OFFSET, V1_MMAP_NC_SIZE * V1_NC_PER_DEVICE },
	{ NCDEV_MEM_REGION_INVALID, 0 },
};

u64 ncdev_bar0_write_blocked_addrs_v1[] = {
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET,
	MMAP_BAR0_APB_MISC_RAM_INVALID,
};

static int ndhal_register_funcs_inf1(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for inf1.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v1";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v1";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Inf1";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Inferentia";
	return 0;
}

/* Device Reset Functions */
/**
 * nr_initiate_reset() - initialize a reset
 * 
 * @param nd - Neuron device which will be reset by the thread.
 */
static int nr_initiate_reset_v1(struct neuron_device *nd, uint32_t nc_map)
{
	if (no_reset)
		return 0;

	int ret = nr_initiate_reset_via_fw(nd, nc_map, 0);
	if (ret) {
		return ret;
	}

	return 0;
}

static int nr_initiate_reset_v1_qemu(struct neuron_device *nd, uint32_t nc_map)
{
	return 0;
}

static int nr_initiate_reset_v1_emu(struct neuron_device *nd, uint32_t nc_map)
{
	return nr_initiate_reset_v1(nd, nc_map);
}

/**
 * nr_wait_for_reset_completion() - wait for a reset to be completed
 * 
 * @param nd - Neuron device which will be reset by the thread.
 */
static int nr_wait_for_reset_completion_v1(struct neuron_device *nd)
{
	if (no_reset)
		return 0;
	
	int i;
	for (i = 0; i < ndhal->ndhal_reset.retry_count; i++) {
		if (fw_io_is_device_ready_v1(nd->npdev.bar0))
			break;
		if (nd->nr.stop)
			return -1;
	}
	if (i == ndhal->ndhal_reset.retry_count) {
		return -1;
	}

	// V1 reset seems to wipe out the device ID, so write the device ID at the end of reset
	fw_io_device_id_write(nd->npdev.bar0, nd->device_index);

	return 0;
}

static int nr_wait_for_reset_completion_v1_qemu(struct neuron_device *nd)
{
	return nr_wait_for_reset_completion_v1(nd);
}

static int nr_wait_for_reset_completion_v1_emu(struct neuron_device *nd)
{
	return nr_wait_for_reset_completion_v1(nd);
}

/**
 * nr_post_reset_config() - perform and post reset configuration needed
 * 
 * @param nd - Neuron device which will be reset by the thread.
 */
static int nr_post_reset_config_v1(struct neuron_device *nd, bool reset_successful)
{
	return 0;
}

/* TOPSP Functions */
/**
 * ts_nq_get_nqid() - get the notification queue index
 *
 * @nd: neuron device
 * @index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @return u8: notification queue index
 */
static u8 ts_nq_get_nqid_v1(struct neuron_device *nd, u8 index, u32 nq_type)
{
	pr_err("topsp is not supported in v1\n");
	BUG_ON(true);
	return 0;
}

/**
 * ts_nq_init() - Initialize notification queue for TopSp.
 *
 * @nd: neuron device
 * @ts_id: TopSp Id
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @size: size of queue in bytes
 * @on_host_memory: if true, NQ is created in host memory
 * @dram_channel: If NQ is created on device memory which DRAM channel to use.
 * @dram_region: If NQ is created on device memory which DRAM region to use.
 * @force_alloc_mem: If true, force allocate new memory (and delete already allocated memory, if any)
 * @nq_mc[out]: memchunk used by the NQ will be written here
 * @mc_ptr[out]: Pointer to memchunk backing this NQ
 *
 * Return: 0 on if initialization succeeds, a negative error code otherwise.
 */
static int ts_nq_init_v1(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
					u32 on_host_memory, u32 dram_channel, u32 dram_region, bool force_alloc_mem,
					struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	pr_err("topsp is not supported in v1\n");
	return -ENOSYS;
}

/**
 * ts_nq_destroy_one() - Disable notification in the device
 *
 * @nd: neuron device
 * @ts_id: topsp id
 *
 */
static void ts_nq_destroy_one_v1(struct neuron_device *nd, u8 ts_id)
{
	pr_err("topsp is not supported in v1\n");
	BUG_ON(true);
}

/**
 * ts_nq_set_hwaddr() - set physical address of the notification queue
 * 
 * @nd: neuron device
 * @ts_id: TopSp Id
 * @index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @size: size of queue in bytes
 * @queue_pa: physical address of the notification queue
 */
static void ts_nq_set_hwaddr_v1(struct neuron_device *nd, u8 ts_id, u8 index, u32 nq_type, u32 size,
			     u64 queue_pa)
{
	pr_err("topsp is not supported in v1\n");
	BUG_ON(true);
}


/* Neuron Core Functions */
/**
 * nc_get_axi_offset() - Returns the axi offset
 * 
 * @param nd: neuron device
 * @param nc_index: neuron core index
 * @return u64: the axi offset
 */
static u64 nc_get_axi_offset(struct neuron_device *nd, int nc_index)
{
	return ndhal->ndhal_address_map.mmap_p_offset + (nc_index * ndhal->ndhal_address_map.mmap_nc_size);
}

/**
 * nc_get_semaphore_base() - get semaphore base address
 * 
 * @param nd - neuron device
 * @param nc_id - neuron core index
 * @return void* - semaphore base address
 */
static void *nc_get_semaphore_base_v1(struct neuron_device *nd, u8 nc_id)
{
	return nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id);
}

/**
 * nc_get_event_addr() - get event address
 * 
 * @param nd - neuron device
 * @param nc_id - neuron core index
 * @param event_index - event index
 * @return void* - event address
 */
static void *nc_get_event_addr_v1(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void *base = nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id) + ndhal->ndhal_address_map.mmap_nc_event_offset;
	return (base + (event_index * NC_EVENT_SIZE));
}


/* Notification Queue Functions */
/**
 * nnq_get_nqid() - get notification queue id
 * 
 * @param nd: neuron device
 * @param nc_id: core index in the device
 * @param index: notification engine index in the core
 * @param nq_type: type of the notification queue
 * @return u8: notification queue id
 */
static u8 nnq_get_nqid_v1(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type)
{
	return (nq_type * MAX_NQ_TYPE) + index;
}

/**
 * nnq_set_hwaddr() - set the physical address of the queue
 * 
 * @param nd: neuron device
 * @param nc_id: core index in the device
 * @param index: notification engine index in the core
 * @param nq_type: type of the notification queue
 * @param size: size of queue in bytes
 * @param queue_pa: physical address of the queue
 */
static void nnq_set_hwaddr_v1(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa)
{
	void *apb_base = nd->npdev.bar0 + pu_get_relative_offset(nc_id);
	u32 low = (u32)(queue_pa & 0xffffffff);
	u32 high = (u32)(queue_pa >> 32U);

	switch (nq_type) {
	case NQ_TYPE_ERROR:
		pu_write_error_notification_cfg_0(apb_base, low);
		pu_write_error_notification_cfg_1(apb_base, high);
		pu_write_error_notification_cfg_2(apb_base, size);
		break;
	case NQ_TYPE_EVENT:
		pu_write_event_notification_cfg_0(apb_base, low);
		pu_write_event_notification_cfg_1(apb_base, high);
		pu_write_event_notification_cfg_2(apb_base, size);
		break;
	case NQ_TYPE_NOTIFY:
		pu_write_expl_notification_cfg_0(apb_base, index, 0, low);
		pu_write_expl_notification_cfg_1(apb_base, index, 0, high);
		pu_write_expl_notification_cfg_2(apb_base, index, 0, size);
		break;
	case NQ_TYPE_TRACE:
		pu_write_impl_notification_cfg_0(apb_base, index, 0, low);
		pu_write_impl_notification_cfg_1(apb_base, index, 0, high);
		pu_write_impl_notification_cfg_2(apb_base, index, 0, size);
		break;
	default:
		BUG();
	}
}


/* Memory Pool Functions */
/**
 * mpset_set_dram_and_mpset_info() 
 *              - set the address and size of device dram
 *              - set mpset's num_channels and number of regions in the device pool
 * 
 * @param mpset: pointer to mpset
 * @param device_dram_addr: DRAM Channel 0 and 1's addresses
 * @param device_dram_size: DRAM Channel 0 and 1's sizes
 */
static void mpset_set_dram_and_mpset_info_v1(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	mpset->num_channels = V1_MAX_DRAM_CHANNELS;
	mpset->mp_device_num_regions = 4;
	device_dram_addr[0] = P_0_DRAM_0_BASE;
	device_dram_addr[1] = P_0_DRAM_1_BASE;
	device_dram_size[0] = P_0_DRAM_0_SIZE;
	device_dram_size[1] = P_0_DRAM_1_SIZE;
	ndhal->ndhal_mpset.device_dram_effective_base_addr[0] = device_dram_addr[0];
	ndhal->ndhal_mpset.device_dram_effective_base_addr[1] = device_dram_addr[1];
	ndhal->ndhal_mpset.device_dram_end_addr[0] = device_dram_addr[0] + device_dram_size[0];
	ndhal->ndhal_mpset.device_dram_end_addr[1] = device_dram_addr[1] + device_dram_size[1];
}

/**
 * mpset_block_carveout_regions() 
 *          - in v2, block carve out regions: Upper 16 MB is used internally by firmware
 *          - in v1, do nothing and just return 0 
 * 
 * @param nd: neuron device
 * @param mpset: pointer to mpset
 * @param device_dram_addr: DRAM Channel 0's and 1's addresses
 * @param device_dram_size: DRAM Channel 0's and 1's sizes
 * @param region_sz: region size
 * @return int: 0 on success, o/w on failure
 */
static int mpset_block_carveout_regions_v1(struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	// V1 doesn't have carve out regions
	return 0;
}


/* DMA Ring Functions */
/** 
 * ndmar_get_h2t_eng_id() 
 *          - get the host-to-device or device-to-host DMA engine ID
 *          - DMA engine 33 (H2D) and 32 (D2H) are top level DMA engines that allow moving data from/to HBM.
 *
 * @param nd: Neuron device which contains the DMA engine
 * @param nc_id: Neuron core corresponding to H2T engine
 * Return DMA engine id
 */
static uint32_t ndmar_get_h2t_eng_id_v1(struct neuron_device *nd, uint32_t nc_id)
{
	return (nc_id * V1_DMA_ENG_PER_NC) + (V1_DMA_ENG_PER_NC - 1);
}

/** 
 * ndmar_get_h2t_qid()  - return the H2T engine's queue id for this core 
 *
 * @param nc_id: Neuron core corresponding to H2T engine
 * Return DMA queue id
 */
static int ndmar_get_h2t_qid_v1(uint32_t nc_id)
{
	return V1_MAX_DMA_RINGS - 1;
}

/** 
 * ndmar_is_h2t_q() - return true 
 *
 * @param nd: Neuron device which contains the DMA engine
 * @param eng_id: engine id
 * @param q_id:  queue id
 * Return true if this is an h2t queue
 */
static bool ndmar_is_h2t_q_v1(struct neuron_device *nd, uint32_t eng_id, uint32_t q_id)
{
	return (nd->ndma_engine[eng_id].used_for_h2t && (q_id == V1_MAX_DMA_RINGS - 1));
}

/**
 * nr_init_h2t_eng() - return true if the h2t dma should be initialized
 * 
 * @param nd_idx - index of the core that owns the h2t 
 * @param nc_map - map of all cores being reset
 */
static bool nr_init_h2t_eng_v1( int nc_idx, uint32_t nc_map)
{
	return true;
}

/**
 * ndmar_is_nx_ring() - is the DMA ring reserved for NX cores
 * 
 * @param eng_id: the DMA engine id
 * @param q_id: the DMA queue id
 */
static bool ndmar_is_nx_ring_v1(uint32_t eng_id, uint32_t q_id)
{
	// V1 doesn't have NX sequencer
	return false;
}

/**
 * ndmar_quiesce_queues() - Quiesce DMA queues.
 *
 * @param nd: Neuron device which contains the DMA engines
 * @param nc_id: NC id that owns the queues
 * @param engine_count: the number of elements in the queue_mask array - currently not used, always pass 0
 * @param queue_mask:   per engine queues to reset - currently not used and ignored.
 *
 * Return: 0 if queue release succeeds, a negative error code otherwise.
 */
static int ndmar_quiesce_queues_v1(struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask)
{
	if (engine_count > DMA_QUIESCE_MAX_ENG)
		return -EINVAL;

	// For V1, reset queues on all 3 engines that belong to NC;
	// Skip h2t because it is shared between models, processes
	u32 start_eng = nc_id * V1_DMA_ENG_PER_NC;
	u32 eng_id;
	for (eng_id = 0; eng_id < V1_DMA_ENG_PER_NC; eng_id++) {
		int qid;
		struct ndma_eng *eng  = ndmar_acquire_engine(nd, start_eng + eng_id);
		if (eng == NULL)
			return -EINVAL;
		for (qid = 0; qid < V1_MAX_DMA_RINGS; qid++) {
			u32 mask = 0x1 << qid;
			// skip h2t because it is shared
			if ((start_eng + eng_id) == ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id) && qid == ndmar_get_h2t_qid_v1(nc_id)) {
				continue;
			}
			// check if only the specific queues were requested
			if (engine_count > 0) {
				// if was not specified for this engine or was not requested for this queue
				if (eng_id >= engine_count || (queue_mask[eng_id] & mask) == 0) {
					continue;
				}
			}
			udma_q_pause(&eng->udma.udma_q_m2s[qid]);
			udma_q_pause(&eng->udma.udma_q_s2m[qid]);
		}
		ndmar_release_engine(eng);
	}
	// sleep a bit, ideally we would wait for prefetch and scheduling
	// to get disabled but that requires reads that we don't want to do
	udelay(4);
	return 0;
}

/** ndmar_set_model_started()
 *
 * Checks to see if the pa belongs to PE IRAM FIFO offset. If so, then these
 * descs are used to load the iram. The mem chunk is going to have all the descriptors
 * to load the instructions in iram. So go through all the dma queues and check if this mem chunk is
 * in that queue. Once we have the queue we set that queue to have descs
 * for iram. The actual copy start of the queue would come when model is started and at that time
 * set the state of model start for this nc.
 *
 * @nd: Neuron device which contains the DMA engine
 * @pa: pa to check
 * @mc: mem chunk that has descs
 *
 * Return: None
 */
static void ndmar_set_model_started_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc)
{
	// For v1, the first model started state needs to be set. Determine the nc
	// that has the pr iram instr descriptor and when the copy start comes
	// for that queue it would imply that the model is started

	int nc_id;
	u64 tpb_addr = pa & ~P_1_BASE; //for v1 axi port is used

	for (nc_id = 0; nc_id < V1_NC_PER_DEVICE; nc_id++) {
		u64 iram_offset = V1_MMAP_TPB_OFFSET + (nc_id * V1_MMAP_NC_SIZE) +
				  V1_MMAP_PE_IRAM_FIFO_OFFSET;
		if ((tpb_addr >= iram_offset) &&
		    (tpb_addr < (iram_offset + V1_MMAP_PE_IRAM_SIZE))) {
			mc->model_start_tracker.has_pe_iram_inst = true;
			mc->model_start_tracker.nc_id = nc_id;
			break;
		}
	}
	return;
}


/* FWIO Functions */
/**
 * fw_io_topology() - Discovers devices connected to the given device.
 *
 * @ctx: FWIO context of the device for which topology
 * @pdev_index: the pci device id
 * @device_id: The index of the neuron device
 * @connected_device_ids:  Connected device IDs are stored here.
 * @count: Number of devices connected to the given device.
 *
 * @return int: 0 on success. -1 on failure
 *
 */
static int fw_io_topology_v1(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count)
{
	int ret = 0, i;
	u64 addr = P_0_APB_MISC_RAM_BASE + V1_FW_IO_REG_FW_STATUS_OFFSET;
	u32 reg_val;
	bool is_ready;
	int found = 0;

	*count = 0;
	is_ready = fw_io_wait_for_device_ready_v1(ctx, &reg_val);
	if (!is_ready)
		return 1;

	// assume no device is connected.
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++)
		connected_device_ids[i] = -1;

	// if east link is up, read the link's device's address
	if (reg_val & V1_FW_IO_REG_FW_STATUS_EAST_LINK_MASK) {
		addr = PCIEX4_0_BASE | (P_0_APB_MISC_RAM_BASE + FW_IO_REG_DEVICE_ID_OFFSET);
		ret = fw_io_read(ctx, &addr, &connected_device_ids[found], 1);
		if (ret) {
			pr_err("failed to read east device id\n");
			return 1;
		}
		found++;
	}
	// if west link is up, read the link's device's address
	if (reg_val & V1_FW_IO_REG_FW_STATUS_WEST_LINK_MASK) {
		addr = PCIEX4_1_BASE | (P_0_APB_MISC_RAM_BASE + FW_IO_REG_DEVICE_ID_OFFSET);
		ret = fw_io_read(ctx, &addr, &connected_device_ids[found], 1);
		if (ret) {
			pr_err("failed to read west device id\n");
			return 1;
		}
		found++;
	}
	*count = found;

	return 0;
}

/**
 * fw_io_register_readless_read_region() - Register readless read BAR regions
 * 
 * @param ctx: FWIO context
 * 
 * @return int: 0 on success. -1 on failure
 */
static int fw_io_register_readless_read_region_v1(struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size)
{
	if (fw_io_register_read_region(ctx, bar0, bar0_size, P_0_APB_BASE)) {
		pr_err("failed to register readless read BAR0 region\n");
		return -1;
	}
	if (fw_io_register_read_region(ctx, bar2, bar2_size, V1_MMAP_TPB_OFFSET)) {
		pr_err("failed to register readless read BAR2 region\n");
		return -1;
	}
	return 0;
}

/**
 * fw_io_read_csr_array() - Read the CSR array
 * 
 * @param addrs: array of CSR addresses to be read
 * @param values: output array
 * @param num_csrs; number of CSR addresses
 * @param operational: true if the read expects the device to be in operational state;
 *                     it's used to distinguish between when the driver first discovers the device (possibly unknown state) and when it's successfully been reset "operational"
 * 
 * @return int: 0 on success, -1 on failure
 */
static int fw_io_read_csr_array_v1(void **ptrs, u32 *values, u32 num_csrs, bool operational)
{
	if (num_csrs > FW_IO_MAX_READLESS_READ_REGISTER_COUNT)
		return -EINVAL;

	int ret = fw_io_read_csr_array_readless(ptrs, values, num_csrs);
	return ret;
}


/* Register Access (read and write) Functions */
/**
 * reg_read32_array() - read an array of 32bit registers.
 * 
 * @addr: register address.
 * @value: read value would be stored here.
 * @num_values: num values to read
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
inline int reg_read32_array_v1(void **addr, u32 *value, u32 num_values)
{
	int ret;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(addr, value, num_values, true);
	if (ret != 0) {
		pr_err("register read failure while reading %p\n", addr[0]);
		dump_stack();
	}

	return ret;
}


/* Memory Map Functions */
/**
 * mmap_get_bar4_offset() - calculate the offset of BAR4
 * 
 * @param start_addr: start address
 * @param size: size of memory
 * @param offset: offset of BAR4
 * @return int: 0 on success; negative on failure
 */
static int mmap_get_bar4_offset_v1(u64 start_addr, u64 size, u64 *offset)
{
	// Note: 
	// 1) we mapped the address to get VA but R/W access to the BAR
	// from the instance might still be blocked.
	// 2) in the new future Neuron software will not request the mapping when running on INF
	if (start_addr >= P_0_DRAM_0_BASE && start_addr + size < P_0_DRAM_0_BASE + P_0_DRAM_0_SIZE)
		*offset = start_addr;
	else if (start_addr >= P_0_DRAM_1_BASE && start_addr + size < P_0_DRAM_1_BASE + P_0_DRAM_1_SIZE)
		// The BAR is squashed, 4GB+4GB are mapped consecutively but they are apart
		// in the actual address space
		*offset = start_addr - P_0_DRAM_1_BASE + P_0_DRAM_0_SIZE;
	else
		return -EINVAL;
	return 0;
}


/* Sysfs Metrics Functions */
// sysfs root node's attrs and its child nodes' attrs
static nsysfsmetric_attr_info_t root_info_node_attrs_info_tbl_v1[] = {
    ATTR_INFO("notify_delay", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY), OTHER),
};
static int root_info_node_attrs_info_tbl_cnt_v1 = sizeof(root_info_node_attrs_info_tbl_v1) / sizeof(nsysfsmetric_attr_info_t);

/**
 * nsysfsmetric_add_ecc_nodes() - add neuron{0, 1, ...}/stats/hardware/{sram_ecc_uncorrected, mem_ecc_uncorrected} to sysfs directory
 * 
 * @param metrics: the sysfs metrics structure
 * @param stats_node: the sysfs node structure of the stats directory
 * @param ecc_attrs_info_tbl_cnt: number of the ecc attributes
 * @param attr_info_tbl: the ecc attributes as an array
 * @return int 0 on success; otherwise on failure
 * 
 * Note: ecc errors are only supported by sysfs for V2. TODO: V1 support will be added 
 */
static int nsysfsmetric_add_ecc_nodes_v1(struct nsysfsmetric_metrics *metrics, 
                               struct nsysfsmetric_node *stats_node,
                               int ecc_attrs_info_tbl_cnt,
                               const nsysfsmetric_attr_info_t *attr_info_tbl)
{
	// ecc errors are only supported by sysfs for V2. V1 support will be added later.
	return 0;
}

/**
 * nsysfsmetric_get_hbm_error_count_v1() - check hbm repairable/unrepairable error count
 *
 * @param nd - neuron device
 * @param repairable - indicates checking for repairable/unrepairable error counts
 * @param err_count - error count returns
 */
static void nsysfsmetric_get_hbm_error_count_v1(struct neuron_device *nd,
                                                 bool repairable,
                                                 uint32_t *err_count)
{
    *err_count = 0;
}

/**
 * nsysfsmetric_add_tensor_engine_node() - add neuron{0, 1, ...}/neuron_core{0,1, ...}/stats/tensor_engine node to sysfs directory
 *
 * @param metrics: the sysfs metrics structure
 * @param stats_node: the sysfs node structure of the nc stats directory
 * @param ecc_attrs_info_tbl_cnt: number of the teng attributes
 * @param attr_info_tbl: the teng attributes as an array
 * @return int 0 on success; otherwise on failure
 */
static int nsysfsmetric_add_tensor_engine_node_v1(struct nsysfsmetric_metrics *metrics,
				struct nsysfsmetric_node *stats_node,
				int nc_id,
				int tensor_engine_attrs_info_tbl_cnt,
				const nsysfsmetric_attr_info_t *tensor_engine_attrs_info_tbl)
{
	// teng pe array stats are not supported by sysfs for V1.
	return 0;
}

/* PCI Functions */
/**
 * neuron_pci_release_bar() - Release a PCI BAR
 * 
 * @param dev: PCI device whose resources were previously reserved by pci_request_region()
 * @param bar: BAR to be reserved
 * 
 * for V2, this function is dummy
 */
static int neuron_pci_release_bar_v1(struct pci_dev *dev, int bar)
{
	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}
	pci_release_region(dev, bar);
	return 0;
}

/**
 * neuron_pci_reserve_bar() - Mark the PCI region associated with PCI BAR as being reserved
 * 
 * @param dev: PCI device whose resources are to be reserved
 * @param bar: BAR to be reserved
 * @param res_name: Name to be associated with resource.
 * @return int: Returns 0 on success, otherwise failure
 */
static int neuron_pci_reserve_bar_v1(struct pci_dev *dev, int bar, const char *res_name)
{
	int ret;

	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}

	ret = pci_request_region(dev, bar, res_name);
	if (ret) {
		pci_info(dev, "BAR %d: can't reserve %s\n", bar, res_name);
		return -ENODEV;
	}

	return 0;
}

 /**
 * neuron_pci_set_npdev() - set BAR's physical addr, io addr, and size of neuron_pci_device
 * 
 * @param dev: PCI device that owns the BAR
 * @param bar: BAR number
 * @param res_name: Name associated with resource
 * @param bar_pa: start physical address of BAR
 * @param bar_ioaddr: __iomem address to device BAR
 * @param bar_size: size of BAR
 * @return int: Returns 0 on success, otherwise failure
 */
static int neuron_pci_set_npdev_v1(struct pci_dev *dev,
                            int bar,
                            const char *res_name,
                            phys_addr_t *bar_pa,
                            void __iomem **bar_ioaddr,
                            u64 *bar_size)
{
	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}

	if (pci_resource_len(dev, bar) == 0) {
		pci_info(dev, "BAR%d len is 0\n", bar);
		return -ENODEV;
	}
	
	*bar_pa = pci_resource_start(dev, bar);
	if (!(*bar_pa)) {
		pci_info(dev, "Can't get start address of BAR%d %s\n", bar, res_name);
		return -ENODEV;
	}
	*bar_size = pci_resource_len(dev, bar);
	
	if (bar == ndhal->ndhal_pci.dram_bar) {
		ndhal->ndhal_pci.dram_bar_size = *bar_size;
	}

	if (bar == ndhal->ndhal_pci.dram_bar && wc_enable)
		*bar_ioaddr = pci_iomap_wc(dev, bar, pci_resource_len(dev, bar));
	else
		*bar_ioaddr = pci_iomap(dev, bar, pci_resource_len(dev, bar));
	
	return 0;
}

/**
 * neuron_pci_get_device_id() - get device id from the device and set nd->device_index
 * 
 * @param dev: PCI device
 * @param nd: neuron device
 * @return int: 0 on success, otherwise on failure
 * 
 * for V1, this function is dummy
 */
static int neuron_pci_get_device_id_v1(struct neuron_device *nd, struct pci_dev *dev)
{
	// dummy for V1
	return 0;
}

/**
 * neuron_pci_device_id_to_rid_map() - return device id to routing id map.  Used by nccl for topology discovery
 * 
 * @param count: total number of neuron devices on the host instance
 * @param did_to_rid_map: map of device ids to routing ids
 * @return int: Returns 0 on success, otherwise failure
 */
static int neuron_pci_device_id_to_rid_map_v1(uint32_t * count, uint32_t * did_to_rid_map)
{
	int i;

	for (i = 0; i < total_neuron_devices; i++) {
		did_to_rid_map[i] = i;
	}
	*count = total_neuron_devices;
	return 0;
}


/* Char Device (cdev) Functions */
/* 
 * IMPORTANT:
 *           - These variables track the range of "compatible" versions of the RT
 *             i.e. the range of RT versions that is compatible with this version of the driver.
 *           - This value is independent from the "release" version because
 *             "release" number is controlled by PM, marketing, etc. considerations.
 * 
 *           - MAX should be incremented when the driver API/behavior
 *             changes in a way that is meaningful to the RT.  In that case
 *             both the MAX here and the version expected by the RT should be
 *             incremented to prevent the new RT from starting on an old driver
 *           - MIN should be incremented when we make changes in the driver
 *             that are not compatible with old RT.  When MIN is incremented
 *             it will prevent old RT from starting up.
 *
 *           - Version 3 of runtime requires 1) aligned memory allocation support  2) SPROT
 *           - Version 4 of the runtime requires support for DMA queue init w/o already allocated rings. (2.7)
 *           - Version 5 of the runtime requires V2 device renumbering (don't care for V1)
 *           - Version 6 of the runtime requires ham notification support
 *              + new V2 reset api for single-tpb reset + new notification init API with force mem realloc/resize
 *           - Version 7 of the runtime requires udma queue size support for non power of 2 rings + dmabuf support
 */
#define V1_RT_MIN_COMPATIBLE_VERSION 2
#define V1_RT_MAX_COMPATIBLE_VERSION 7
/**
 * ncdev_compatible_version() - fill in the compatible version of the RT with the current driver version
 * 
 * @param arg: min and max compatible versions to be filled in
 */
static void ncdev_compatible_version_v1(struct neuron_ioctl_compatible_version *arg)
{
	arg->min = V1_RT_MIN_COMPATIBLE_VERSION;
	arg->max = V1_RT_MAX_COMPATIBLE_VERSION;
}

/**
 * ncdev_quiesce_exec_on_proc_exit() - for V1, before resetting DMA, allow current NeuronCore execution to finish and settle
 * 
 * Note:
 *      When a process is killed, the driver resets DMA but there is no
 *      way to soft reset neuron cores. This causes problem if the
 *      process was executing serial TPB or switching activation tables,
 *      which result in abrubtly stopping DMA engines hence engines are
 *      are blocked on semaphores. This results in next model
 *      load failure or inference timeout.
 * 
 *      Proper way is clearing out semaphore, events after resetting
 *      DMA engines. However, it is a lot of code change, hence
 *      adding a sleep for 1 second when process exits, which allows
 *      the NeuronCore to continue to execute for a second. Since
 *      no new inference can be submitted during this time, NeuronCore
 *      state would be cleared out.
 * 
 */
static void ncdev_quiesce_exec_on_proc_exit_v1(void)
{
	msleep(1000);  // TODO: should directly clear semaphore and events instead of 1 sec sleep
}

/**
 * ncdev_bar_write_data() - write data to bar
 * 
 * @param nd: neuron device
 * @param bar: the BAR to write to
 * @param reg_addresses
 * @param data: the data to be written into the bar
 * @param data_count: the number of data to be written
 * @return 0 on success, otherwise failure
 * 
 * V1:
 *    For BAR0 the addresses are passed as array(random access).
 *    For BAR2 a single address is provided and driver does sequential writes.
 * V2:
 *    Only BAR0 is used right now. TODO: change runtime ioctl
*/
static int ncdev_bar_write_data_v1(struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count)
{
	if (bar == 0) {
		int i;
		for (i = 0; i < data_count; i++) {
			u64 off = reg_addresses[i] - (u64)nd->npdev.bar0;
			if (off > nd->npdev.bar0_size) {
				return -EINVAL;
			}
			if (ndhal->ndhal_ndma.ndma_is_bar0_write_blocked(off)) {
				return -EINVAL;
			}
			writel(data[i], nd->npdev.bar0 + off);
			trace_bar_write(nd, bar, off, data[i]);
		}
	} else if (bar == 2) {
		int i;
		u64 off = reg_addresses[0] - (u64)nd->npdev.bar2;
		for (i = 0; i < data_count; i++, off += sizeof(u32)) {
			if (off > nd->npdev.bar2_size) {
				return -EINVAL;
			}
			writel(data[i], nd->npdev.bar2 + off);
			trace_bar_write(nd, bar, off, data[i]);
		}
	} else {
		pr_err("direct BAR%d write is not supported.\n", bar);
		return -EINVAL;
	}

	return 0;
}

static void ncdev_get_default_tpbs_for_hbm_v1(u32 hbm_index, u32 tpbs[MAX_NC_PER_DEVICE], u32 *tpb_count)
{
	// Unimplemented for v1 - can be implemented if required
	*tpb_count = 0;
}

/* UDMA Functions */
/**
 * udma_m2s_data_rd_cfg_boundaries_set(): set data_rd_cfg to break at 256B boundaries
 * 
 * @param udma: the UDMA structure
 * 
 * for V1, this function is dummy
 */
static void udma_m2s_data_rd_cfg_boundaries_set_v1(struct udma *udma)
{
    return;
}

/**
 * udma_q_config() - set misc queue configurations
 *
 * @param udma_q udma_q: the queue data structure
 *
 * for V1, this function is dummy
 */
static void udma_q_config_v1(struct udma_q *udma_q)
{
    return;
}


/* NDMA Functions */
/**
 * ndma_get_wait_for_completion_time() - calculate the first and the following wait times for a DMA tranfer completion
 * 
 *      One full descriptor takes ~4 usec to transfer (64K at 16G/sec) on V2  and ~16 usec to transfer on V1.
 *      The last descriptor may be partial, so wait 1/4 64K transfer time for that descriptor.
 *      Also, count includes the completion descriptor so don't include that in the count.
 * 
 * @param first_wait_time: the wait time for the first sleep
 * @param wait_time: the wait time for the following sleeps
 */
static void ndma_get_wait_for_completion_time_v1(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	u64 est_wait_time = 16 * (count - 1);
	*first_wait_time = async ? 1 : (est_wait_time - 12);
	*following_wait_time = (est_wait_time * 100) - *first_wait_time;

	// for some reason getting a timeout tools pipeline, so bumping wait by 10x 
	// https://tiny.amazon.com/9xmp72cm
	*following_wait_time *= 10;
}

static void ndma_get_wait_for_completion_time_v1_qemu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v1(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 10 * 1000;
}

static void ndma_get_wait_for_completion_time_v1_emu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v1(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 100 * 1000;
}

/**
 * ndma_validate_pa() - check the validity of the desc physical addresses
 *      V1:
 *         west side: PCIEX4_1_BASE: 0x00c00000000000 host: PCIEX8_0_BASE: 0x00400000000000
 *         If west side is set then even host bit is set. When mc_alloc is called we set only the host bit
 *         and insert into tree.. If some one sets the west side on that PA, then there is no way to check that,
 *         since there could be a tdram address that could have the west side set
 *         (that will look as though host is also set)
 *      V2:
 *         similar idea.  Just check for valid address allocated in host memory
 *
 * @param nd: the neuron device
 * @param pa: the desc physical addresses
 * @param dst_mc: the mc that backs the dma queue
 * @return int: return 0 if the pa is valid; otherwise return negative
 */
static int ndma_validate_pa_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type)
{
	if (((pa & PCIEX8_0_BASE) == ndhal->ndhal_address_map.pci_host_base) && ((pa & PCIEX4_1_BASE) != PCIEX4_1_BASE)) {
		if (!ndma_is_valid_host_mem(nd, pa)) {
			return -EINVAL;
		}
	} else if (desc_type == NEURON_DMA_QUEUE_TYPE_RX) {
		// For V1 need to set the first model start state. If the desc has pa for PE instr fifo, then
		// whichever dma engine queue that has this mc is set to have the pe instr.
		ndmar_set_model_started_v1(nd, pa, dst_mc);
	}
	return 0;
}

static const u64 teng_udma_base[] = {
	P_0_APB_TENG_0_UDMA_0_RELBASE,
	P_0_APB_TENG_1_UDMA_0_RELBASE,
	P_0_APB_TENG_2_UDMA_0_RELBASE,
	P_0_APB_TENG_3_UDMA_0_RELBASE };
static const u64 teng_tdma_base[] = {
	P_0_APB_TENG_0_TDMA_0_RELBASE,
	P_0_APB_TENG_1_TDMA_0_RELBASE,
	P_0_APB_TENG_2_TDMA_0_RELBASE,
	P_0_APB_TENG_3_TDMA_0_RELBASE };
/**
 * ndma_init() - Initialize a DMA engine
 * 
 * @param bar0: BAR0
 * @param udma: UDMA structure
 * @param eng_id: DMA engine index to initialize
 * @return int: 0 on success, otherwise on failure
 */
static int ndma_init_v1(void __iomem *bar0, struct udma *udma, int eng_id)
{
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	void __iomem *udma_base;
	void __iomem *tdma_base;
	int nc_id = eng_id / V1_DMA_ENG_PER_NC;
	int eid = eng_id % V1_DMA_ENG_PER_NC;
	udma_base = (void __iomem *)bar0 + teng_udma_base[nc_id] + (eid * P_0_APB_TENG_0_UDMA_0_SIZE);
	tdma_base = (void __iomem *)bar0 + teng_tdma_base[nc_id] + (eid * P_0_APB_TENG_0_TDMA_0_SIZE);

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(udma, udma_base, DMA_MAX_Q_MAX, udma_name, 0, V1_ALLOWED_DESC_PER_PACKET, false);
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = tdma_init_engine(tdma_base);
	if (ret) {
		pr_err("TDMA ENG:%d init failed\n", eng_id);
		goto done;
	}

done:
	return ret;
}

/**
 * ndma_is_bar0_write_blocked() - is BAR0 blocked for write access
 *      1. Block write access from user space to some APB MISC RAMs
 *      2. Block write access from user space to DMA queues
 *    Both of these accesses are only allowed through the driver
 *
 * @param offset: offset to be checked as blocked or not
 * @return int: return -1 if the access should be blocked, otherwise return 0.
 */
static int ndma_is_bar0_write_blocked_v1(u64 off)
{
	int nc_id, eid;
	// if not writing to udma space - quick exit; note this also ignores writes to some tdma
	// space but we don't care since we will not be checking it later anyway
	if (off < P_0_APB_TENG_0_UDMA_0_RELBASE || off >= (P_0_APB_TENG_3_UDMA_0_RELBASE + V1_DMA_ENG_PER_NC * P_0_APB_TENG_0_UDMA_0_SIZE)) {
		return 0;
	}
	for (nc_id = 0; nc_id < sizeof(teng_udma_base) / sizeof(teng_udma_base[0]); nc_id++) {
		for (eid = 0; eid < V1_DMA_ENG_PER_NC; eid++) {
			u64 udma_off = teng_udma_base[nc_id] + (eid * P_0_APB_TENG_0_UDMA_0_SIZE);
			if (ndma_bar0_blocked_one_engine(udma_off, off)) {
				return -1;
			}
		} 
	}

	int i = 0;
	while (ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs[i] != MMAP_BAR0_APB_MISC_RAM_INVALID) {
		if (off == ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs[i]) {
			pr_err("** blocking %llx\n", off);
			return -1;
		}
		i++;
	}

	return 0;
}

/**
 * ndma_get_m2m_barrier_type() - get the m2m barrier type
 * 
 * @param set_dmb 
 * @return int 
 */
static int ndma_get_m2m_barrier_type_v1(bool set_dmb)
{
	if (set_dmb)
		return UDMA_M2M_BARRIER_DMB;
	else
		return UDMA_M2M_BARRIER_NONE;
}

/**
 * ndma_get_engines_with_host_connectivity - get DMA engines for a particular HBM index which have host connectivity
 */
static void ndma_get_engines_with_host_connectivity_v1(u32 hbm_index, u32 engines[NUM_DMA_ENG_PER_DEVICE], u32 *num_engines)
{
	// Unimplemented for v1, can implement if required.
	*num_engines = 0;
}


/* POD Functions */
/**
 * npe_notify_mark() - api for crwl to notify range marking (core claiming) activities
 *
 * @param mark_cnt - marked core count (for mark, count before, for unmark, count after)
 * @param mark     - true if calling operation was a mark vs unmark
 *
 */
static void npe_notify_mark_v1(int mark_cnt, bool mark)
{
}

/**
 * npe_pod_info() - return information about the pod the instance belongs to
 *
 * @param pod_type 			- type of pod the instance belongs to or NONE if not part of a pod
 * @param pod_id   			- unique id of the pod
 * @param pod_sz   			- size of the pod.  0 if not a pod
 * @param mode     			- current operating mode
 * @param modes_supported	- supported operating modes
 *
 */
static int npe_pod_info_v1(u8 *pod_type, u8 *pod_id, u8 *pod_sz, enum neuron_ultraserver_mode *mode, u32 *modes_supported)
{
	*pod_type = NEURON_POD_TYPE_NONE;
	*pod_sz = 0;
	*mode = NEURON_ULTRASERVER_MODE_UNSET; 
	*modes_supported = 0;
	return 0;
}

/**
 * npe_pod_status() - return status information about the pod the instance belongs to
 *
 * @param pod_state - state/outcome of the pod's election process
 * @param node_id   - node id within the pod
 *
 */ 
static int npe_pod_status_v1(u32 *pod_state, s8 *node_id)
{
	*pod_state = NEURON_POD_E_STATE_SINGLE_NODE;
	*node_id = -1;
	return 0;
}

/**
 * npe_pod_ctrl() - control the state of the pode
 *
 * @nd:    neuron device
 * @param pod_ctrl  - control operation to perform
 * @param mode - requested operating mode for mode control
 * @param timeout - timeout for the control operation
 * @param pod_state - state/outcome of the pod's election process
 *
 */
static int npe_pod_ctrl_v1(struct neuron_device *nd, u32 pod_ctrl, enum neuron_ultraserver_mode mode, u32 timeout, u32 *pod_state)
{
	*pod_state = NEURON_POD_E_STATE_SINGLE_NODE;
	return 0;
}

/**
 * npe_class_node_id_show_data() - return sysfs class node_id
 *
 * @buf - sysfs buffer
 * @sz - size of ultraserver config to show data for 
 *
 */
static ssize_t npe_class_node_id_show_data_v1(char *buf, u32 sz)
{
   return dhal_sysfs_emit(buf, "-1\n");
}

/**
 * npe_class_server_id_show_data() - return sysfs class node_id
 *
 * @buf - sysfs buffer
 * @sz - size of ultraserver config to show data for 
 *
 */
static ssize_t npe_class_server_id_show_data_v1(char *buf, u32 sz)
{
    return dhal_sysfs_emit(buf, "0000000000000000\n");
}

/**
 * npe_class_ultraserver_mode_show_data() - return sysfs class ultraserver_mode
 *
 * @buf - sysfs buffer
 *
 */
static ssize_t npe_class_ultraserver_mode_show_data_v1(char *buf)
{
    return dhal_sysfs_emit(buf, "\n");
}

/**
 * ndhal_ext_cleanup_v1() - cleanup any extended resources`
 *
 */
static void ndhal_ext_cleanup_v1(void)
{
	return;
}


/**
 * static asserts to valid static const sizes work across versions
 *
 */
static_assert( MAX_DRAM_CHANNELS >= V1_MAX_DRAM_CHANNELS, "Max dram channel count too small");
static_assert( MAX_NC_PER_DEVICE >= V1_NC_PER_DEVICE, "Max nc per device count too small");
static_assert( MAX_NQ_TYPE >= V1_MAX_NQ_TYPE, "Max nq type count too small");
static_assert( MAX_NQ_ENGINE >= V1_MAX_NQ_ENGINE, "Max nq per engine count too small");
static_assert( NUM_DMA_ENG_PER_DEVICE >= V1_NUM_DMA_ENG_PER_DEVICE, "Max dma engine per device count too small");

/**
 * ndhal_register_funcs_v1() - initialize the dhal for v1 chips
 *  
 */
int ndhal_register_funcs_v1(void) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for V1.");
		return -EINVAL;
	}

	ndhal->ndhal_address_map.pci_host_base = PCIEX8_0_BASE;
	ndhal->ndhal_address_map.mmap_p_offset = V1_MMAP_P_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_event_offset = V1_MMAP_NC_EVENT_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_read_offset = V1_MMAP_NC_SEMA_READ_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_set_offset = V1_MMAP_NC_SEMA_SET_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_incr_offset = V1_MMAP_NC_SEMA_INCR_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_decr_offset = V1_MMAP_NC_SEMA_DECR_OFFSET;
	ndhal->ndhal_address_map.bar0_misc_ram_offset = V1_MMAP_BAR0_APB_MISC_RAM_OFFSET;
	ndhal->ndhal_address_map.port_1_base = P_1_BASE;
	ndhal->ndhal_address_map.mmap_nc_size = V1_MMAP_NC_SIZE;
	ndhal->ndhal_address_map.nc_per_device = V1_NC_PER_DEVICE;
	ndhal->ndhal_address_map.dev_nc_map = (1 << V1_NC_PER_DEVICE) - 1;
	ndhal->ndhal_address_map.dice_per_device = V1_NUM_DIE_PER_DEVICE;
	ndhal->ndhal_address_map.semaphore_count = V1_SEMAPHORE_COUNT;
	ndhal->ndhal_address_map.event_count = V1_EVENTS_COUNT;
	ndhal->ndhal_address_map.ts_per_device = 0;
	ndhal->ndhal_address_map.dma_eng_per_nd = V1_NUM_DMA_ENG_PER_DEVICE;
	ndhal->ndhal_address_map.dma_eng_per_nc = V1_DMA_ENG_PER_NC;
	ndhal->ndhal_address_map.dram_channels = V1_MAX_DRAM_CHANNELS;
	ndhal->ndhal_reset.reset_poll_interval = V1_NR_RESET_POLL_INTERVAL;
	ndhal->ndhal_reset.reset_device_initial_poll_delay = V1_NR_DEVICE_RESET_INITIAL_POLL_DELAY;
	ndhal->ndhal_reset.reset_tpb_initial_poll_delay = V1_NR_TPB_RESET_INITIAL_POLL_DELAY;
	ndhal->ndhal_reset.initiate_max_wait_time = V1_NR_RESET_INIT_MAX_TOTAL_WAIT_TIME_MS;
	ndhal->ndhal_reset.retry_count = NR_RESET_RETRY_COUNT;
	ndhal->ndhal_reset.nr_post_reset_config = nr_post_reset_config_v1;
	ndhal->ndhal_topsp.ts_nq_init = ts_nq_init_v1;
	ndhal->ndhal_topsp.ts_nq_destroy_one = ts_nq_destroy_one_v1;
	ndhal->ndhal_topsp.ts_nq_get_nqid = ts_nq_get_nqid_v1;
	ndhal->ndhal_topsp.ts_nq_set_hwaddr = ts_nq_set_hwaddr_v1;
	ndhal->ndhal_nc.nc_get_semaphore_base = nc_get_semaphore_base_v1;
	ndhal->ndhal_nc.nc_get_event_addr = nc_get_event_addr_v1;
	ndhal->ndhal_nq.nnq_get_nqid = nnq_get_nqid_v1;
	ndhal->ndhal_nq.nnq_set_hwaddr = nnq_set_hwaddr_v1;
	ndhal->ndhal_mpset.mp_min_alloc_size = mempool_min_alloc_size;
	ndhal->ndhal_mpset.small_pool_supported = false; // doesn't make sense to have separate small allocations genpool since memory on v1 is less
	ndhal->ndhal_mpset.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v1;
	ndhal->ndhal_mpset.mpset_block_carveout_regions = mpset_block_carveout_regions_v1;
	ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id = ndmar_get_h2t_eng_id_v1;
    ndhal->ndhal_ndmar.ndmar_get_h2t_qid = ndmar_get_h2t_qid_v1;
    ndhal->ndhal_ndmar.ndmar_is_h2t_q = ndmar_is_h2t_q_v1;
	ndhal->ndhal_ndmar.nr_init_h2t_eng = nr_init_h2t_eng_v1;
	ndhal->ndhal_ndmar.ndmar_is_nx_ring = ndmar_is_nx_ring_v1;
	ndhal->ndhal_ndmar.ndmar_quiesce_queues = ndmar_quiesce_queues_v1;
	ndhal->ndhal_ndmar.ndmar_set_model_started = ndmar_set_model_started_v1;
	ndhal->ndhal_fw_io.fw_io_topology = fw_io_topology_v1;
	ndhal->ndhal_fw_io.fw_io_register_readless_read_region = fw_io_register_readless_read_region_v1;
	ndhal->ndhal_fw_io.fw_io_read_csr_array = fw_io_read_csr_array_v1;
	ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v1;
	ndhal->ndhal_mmap.dm_mmap_special = dm_mmap_special_v1;
	ndhal->ndhal_mmap.mmap_get_bar4_offset = mmap_get_bar4_offset_v1;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl_cnt = root_info_node_attrs_info_tbl_cnt_v1;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl = root_info_node_attrs_info_tbl_v1;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_add_ecc_nodes = nsysfsmetric_add_ecc_nodes_v1;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_get_hbm_error_count = nsysfsmetric_get_hbm_error_count_v1;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_add_tensor_engine_node = nsysfsmetric_add_tensor_engine_node_v1;
	ndhal->ndhal_pci.apb_bar = 0;
	ndhal->ndhal_pci.axi_bar = 2;
	ndhal->ndhal_pci.dram_bar = 4;
	ndhal->ndhal_pci.neuron_pci_release_bar = neuron_pci_release_bar_v1;
	ndhal->ndhal_pci.neuron_pci_reserve_bar = neuron_pci_reserve_bar_v1;
	ndhal->ndhal_pci.neuron_pci_set_npdev = neuron_pci_set_npdev_v1;
	ndhal->ndhal_pci.neuron_pci_get_device_id = neuron_pci_get_device_id_v1;
	ndhal->ndhal_pci.neuron_pci_device_id_to_rid_map = neuron_pci_device_id_to_rid_map_v1; 
	ndhal->ndhal_cdev.ncdev_mem_regions = ncdev_mem_regions_v1;
	ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs = ncdev_bar0_write_blocked_addrs_v1;
	ndhal->ndhal_cdev.ncdev_compatible_version = ncdev_compatible_version_v1;
	ndhal->ndhal_cdev.ncdev_quiesce_exec_on_proc_exit = ncdev_quiesce_exec_on_proc_exit_v1;
	ndhal->ndhal_cdev.ncdev_bar_write_data = ncdev_bar_write_data_v1;
	ndhal->ndhal_cdev.ncdev_logical_to_physical_nc_map = NULL;
	ndhal->ndhal_cdev.ncdev_get_default_tpbs_for_hbm = ncdev_get_default_tpbs_for_hbm_v1;
	ndhal->ndhal_udma.udma_m2s_data_rd_cfg_boundaries_set = udma_m2s_data_rd_cfg_boundaries_set_v1;
	ndhal->ndhal_udma.num_beats = 1024; // >= UDMA_REV_ID_4
	ndhal->ndhal_udma.udma_q_config = udma_q_config_v1;
	ndhal->ndhal_ndma.ndma_retry_memcpy = false;
	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v1;
	ndhal->ndhal_ndma.ndma_validate_pa = ndma_validate_pa_v1;
	ndhal->ndhal_ndma.ndma_init = ndma_init_v1;
	ndhal->ndhal_ndma.ndma_is_bar0_write_blocked = ndma_is_bar0_write_blocked_v1;
	ndhal->ndhal_ndma.ndma_get_m2m_barrier_type = ndma_get_m2m_barrier_type_v1;
	ndhal->ndhal_ndma.ndma_get_engines_with_host_connectivity = ndma_get_engines_with_host_connectivity_v1;
	ndhal->ndhal_npe.npe_notify_mark = npe_notify_mark_v1;
	ndhal->ndhal_npe.npe_pod_info = npe_pod_info_v1;
	ndhal->ndhal_npe.npe_pod_status = npe_pod_status_v1;
	ndhal->ndhal_npe.npe_pod_ctrl = npe_pod_ctrl_v1;
	ndhal->ndhal_npe.npe_class_node_id_show_data = npe_class_node_id_show_data_v1;
	ndhal->ndhal_npe.npe_class_server_id_show_data = npe_class_server_id_show_data_v1;
	ndhal->ndhal_npe.npe_class_ultraserver_mode_show_data = npe_class_ultraserver_mode_show_data_v1;
	ndhal->ndhal_ext_cleanup = ndhal_ext_cleanup_v1;

	if (narch_is_qemu()) {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v1_qemu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1_qemu;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v1_qemu;
	} else if (narch_is_emu()) {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v1_emu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1_emu;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v1_emu;
	} else {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v1;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1;
	}

	switch (ndhal->pci_device_id) {
		case INF1_DEVICE_ID0:
		case INF1_DEVICE_ID1:
		case INF1_DEVICE_ID2:
		case INF1_DEVICE_ID3:
			ret = ndhal_register_funcs_inf1();
			if (ret) {
				pr_err("failed to register ndhal funcs on inf1.\n");
				return ret;
			}
			break;
		default:
			pr_err("Unknown HW architecture. Can't init neuron_dhal.\n");
			return -EINVAL;
	}
	return ret;
}
