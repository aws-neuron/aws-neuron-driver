// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2025, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/delay.h>
#include <linux/pci.h>

#include "address_map.h"
#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_arch.h"
#include "../neuron_cdev.h"
#include "../neuron_dma.h"
#include "../neuron_pci.h"
#include "../v3/neuron_pelect.h"
#include "neuron_fw_metrics.h"


// TOP SP addresses are sparse on chip adjust to accommodate the table macro
//
#define V4_TOP_SP_GRP1_BASE V4_TOP_SP_0_BASE
#define V4_TOP_SP_GRP2_BASE (V4_TOP_SP_10_BASE - 8 * V4_TOP_SP_DIST)

#define V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET V4_PCIE_BAR0_TOP_SP_0_OFFSET
#define V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET (V4_PCIE_BAR0_TOP_SP_10_OFFSET - 8 * V4_TOP_SP_SIZE)

struct neuron_dm_special_mmap_ent dm_mmap_special_v4[] = {
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   4, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   5, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   6, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   7, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   4, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   5, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   6, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   7, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 6, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 7, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  8, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  9, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 10, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 11, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 12, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 13, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 14, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 15, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 6, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 7, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  8, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  9, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 10, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 11, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 12, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 13, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 14, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 15, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),

	{.block = NEURON_DM_BLOCK_HBM, .block_id = 0, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_0_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 0, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 1, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_1_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 1, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 2, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_2_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 2, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 3, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_3_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 3, .bar_num = 4},

	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

struct ncdev_mem_region ncdev_mem_regions_v4[] = {
	{ V4_MMAP_TPB_0_BASE, V4_MMAP_NC_SIZE },    // FIXME this is inefficient this may need a routine to slice and range check
	{ V4_MMAP_TPB_1_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_2_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_3_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_4_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_5_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_6_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_7_BASE, V4_MMAP_NC_SIZE },
	{ V4_TOP_SP_0_BASE, V4_TOP_SP_SIZE },       // could flatten TOP_SP
	{ V4_TOP_SP_1_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_2_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_3_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_4_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_5_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_6_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_7_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_8_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_9_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_10_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_11_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_12_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_13_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_14_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_15_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_16_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_17_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_18_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_19_BASE, V4_TOP_SP_SIZE },
	{ V4_HBM_0_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_HBM_1_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_HBM_2_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_HBM_3_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_PREPROC_0_BASE, V4_PREPROC_SIZE},
	{ V4_PREPROC_1_BASE, V4_PREPROC_SIZE},
	{ V4_PREPROC_2_BASE, V4_PREPROC_SIZE},
	{ V4_PREPROC_3_BASE, V4_PREPROC_SIZE},
	{ NCDEV_MEM_REGION_INVALID, 0 },
};


u32 npe_neighbor_eng_ids_v4[2][2] =
{
    {40, 72},  // Left
    {8, 104}   // Right
};

static int ndhal_register_funcs_trn3(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for trn3.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v4";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v4";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Trn3";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Trainium3";
	return 0;
}

static const uint32_t v4_nnq_h2d_id_to_eng_id[] = {
	V4_D2H_0_IDX, V4_H2D_0_IDX, V4_D2H_1_IDX, V4_H2D_1_IDX
};

static int nnq_get_nq_h2d_id_v4(uint32_t h2d_eng_id)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(v4_nnq_h2d_id_to_eng_id); i++) {
		if (v4_nnq_h2d_id_to_eng_id[i] == h2d_eng_id)
			return i;
	}
	return -1;
}

static uint32_t nnq_get_h2d_id_v4(int nq_h2d_id)
{
	return v4_nnq_h2d_id_to_eng_id[nq_h2d_id];
}

/**
 * * ndmar_ctx_queue_bit_v4() - map a V4 H2D queue to a dense bitmap index
 * @h2d_eng_id: DMA engine id in the V4 H2D/D2H engine range (128 to 131)
 * @qid: DMA queue id within the engine
 * Return bitmap bit index for the queue.
 */
static int ndmar_ctx_queue_bit_v4(uint32_t h2d_eng_id, uint32_t qid)
{
	return (h2d_eng_id - V4_D2H_0_IDX) * DMA_MAX_Q_V4 + qid;
}

/**
 * ndmar_ctx_queue_from_bit_v4() - map a dense bitmap index back to a V4 H2D queue
 * @bit: bitmap bit index
 * @h2d_eng_id: returned DMA engine id
 * @qid: returned DMA queue id
 */
static void ndmar_ctx_queue_from_bit_v4(int bit, uint32_t *h2d_eng_id, uint32_t *qid)
{
	*h2d_eng_id = V4_D2H_0_IDX + (bit / DMA_MAX_Q_V4);
	*qid = bit % DMA_MAX_Q_V4;
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
static void mpset_set_dram_and_mpset_info_v4(struct neuron_mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	mpset->num_hbms = V4_NUM_HBMS;
	device_dram_addr[0] = V4_HBM_0_BASE;
	device_dram_addr[1] = V4_HBM_1_BASE;
	device_dram_addr[2] = V4_HBM_2_BASE;
	device_dram_addr[3] = V4_HBM_3_BASE;

	if (narch_is_qemu()) {
		// Allow qemu setups to dynamically allocate their HBM sizes
		const u64 msize = ndhal->ndhal_pci.dram_bar_size / 4;
		device_dram_size[0] = msize;
		device_dram_size[1] = msize;
		device_dram_size[2] = msize;
		device_dram_size[3] = msize;

		u32 mem_regions = sizeof(dm_mmap_special_v4) / sizeof(dm_mmap_special_v4[0]);
		int i = 0;
		for (; i < mem_regions; ++i) {
			if ((dm_mmap_special_v4[i].offset == V4_HBM_0_BASE) ||
				(dm_mmap_special_v4[i].offset == V4_HBM_1_BASE) ||
				(dm_mmap_special_v4[i].offset == V4_HBM_2_BASE) ||
				(dm_mmap_special_v4[i].offset == V4_HBM_3_BASE)) {
				dm_mmap_special_v4[i].size = msize;
			}
		}
		pr_info("overriding hbm size to %llu bytes", msize);
	} else {
		device_dram_size[0] = V4_HBM_ACTIVE_SIZE;
		device_dram_size[1] = V4_HBM_ACTIVE_SIZE;
		device_dram_size[2] = V4_HBM_ACTIVE_SIZE;
		device_dram_size[3] = V4_HBM_ACTIVE_SIZE;
	}
	int i;
	for (i = 0; i < mpset->num_hbms; i++) {
		ndhal->ndhal_mpset.device_dram_end_addr[i] = device_dram_addr[i] + device_dram_size[i];
	}
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
static int mmap_get_bar4_offset_v4(u64 start_addr, u64 size, u64 *offset)
{
	u64 hbm_dist = narch_is_qemu() ? (ndhal->ndhal_pci.dram_bar_size / 4) : V4_HBM_SIZE;

	if (start_addr >= V4_HBM_0_BASE && start_addr + size <= V4_HBM_0_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr;
	else if (start_addr >= V4_HBM_1_BASE && start_addr + size <= V4_HBM_1_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr - V4_HBM_1_BASE + hbm_dist;
	else if (start_addr >= V4_HBM_2_BASE && start_addr + size <= V4_HBM_2_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr - V4_HBM_2_BASE + hbm_dist * 2;
	else if (start_addr >= V4_HBM_3_BASE && start_addr + size <= V4_HBM_3_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr - V4_HBM_3_BASE + hbm_dist * 3;
	else
		return -EINVAL;
	return 0;
}


/* PCI Functions */

// for V4 rename Neuron devices for better customer experience.
// see internal documentation: TRN2-Discovery
// map routing id to user id:
static const u32 v4_torus_routing_id_to_user_id[] = {
	0,	3,	4,	7,
	12,	15,	8,	11,
	1,	2,	5,	6,
	13,	14,	9,	10 };

#define V4_ROUTING_ID_TBL_SZ  (sizeof(v4_torus_routing_id_to_user_id) / sizeof(v4_torus_routing_id_to_user_id[0]))

static const struct neuron_platform_desc v4_platform_std = {
	.type = NEURON_PLATFORM_TYPE_STD,
	.family = NEURON_PLATFORM_FAMILY_NONE,
	.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_TABLE, .table = { v4_torus_routing_id_to_user_id, V4_ROUTING_ID_TBL_SZ } },
	.device_count = 16,
	.default_node_cnt = 0,
	.pod_type = NEURON_POD_TYPE_NONE,
	.pod_discovery = NEURON_POD_DISCOVERY_TYPE_NONE,
	.max_bars = 0,
	.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_NONE } },
	.nc_efa_bdf_tbl = { .bdfs = trn3pd_nc_efa_bdf_tbl, .count = sizeof(trn3pd_nc_efa_bdf_tbl) / sizeof(trn3pd_nc_efa_bdf_tbl[0]) },
	.num_sleds_per_server = 8,
	.num_chips_per_sled = 2,
};

static const char *const v4_ultraserver_instance_names[] = {
	"trn3p.48xlarge",
	NULL,
};

static const char *const v4_pds_v2_switch_instance_names[] = {
	"trn3n.48xlarge",
	NULL,
};

static const char *const v4_pds_v1_switch_instance_names[] = {
	"trn3.48xlarge",
	"trn3-dev0.48xlarge",
	"trn3-dev1.48xlarge",
	"trn3-es.48xlarge",
	"trn3s-es.48xlarge",
	NULL,
};

static const char *const v4_max_v2_switch_instance_names[] = {
	"trn3e.24xlarge",
	"trn3en.24xlarge",
	NULL,
};

static const char *const v4_max_v1_switch_instance_names[] = {
	"trn3e-dev0.24xlarge",
	"trn3e-es.24xlarge",
	NULL,
};

// Simulation-only instance name for simulated (QEMU/inkling) MAX compute
// sleds.  Deliberately not a real instance name: platform capabilities are
// consumed from the driver's platform descriptor (sysfs), not derived from
// DMI, so a simulation does not need to impersonate the instance it models.
static const char *const v4_qemu_max_instance_names[] = {
	"qemu-trn3max",
	NULL,
};

static const char *const v4_3xl_instance_names[] = {
	"trn3pd98.3xlarge",
	"trn3s-es.3xlarge",
	"trn3.3xlarge",
	"trn3-es.3xlarge",
	"trn3n.3xlarge",
	NULL,
};

static const struct neuron_platform_desc v4_platform_table[] = {
	{
		.instance_names = v4_ultraserver_instance_names,
		.type = NEURON_PLATFORM_TYPE_ULTRASERVER,
		.family = NEURON_PLATFORM_FAMILY_TRN2_ULTRASERVER, /* trn3p.48xlarge is a dev platform on the deprecation path; use TRN2_ULTRASERVER as it is the publicly documented family for the trn2u topology */
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_TABLE, .table = { v4_torus_routing_id_to_user_id, V4_ROUTING_ID_TBL_SZ } },
		.device_count = 16,
		.default_node_cnt = 4,
		.pod_type = NEURON_POD_TYPE_P2P,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_ELECTION,
		.max_bars = 0,
		.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_TORUS }, .inter_rack = { .topology = NEURON_INTER_RACK_TOPOLOGY_P2P } },
		.nc_efa_bdf_tbl = { .bdfs = trn3pd_nc_efa_bdf_tbl, .count = sizeof(trn3pd_nc_efa_bdf_tbl) / sizeof(trn3pd_nc_efa_bdf_tbl[0]) },
		.num_sleds_per_server = 8,
		.num_chips_per_sled = 2,
	},
	{
		.instance_names = v4_pds_v2_switch_instance_names,
		.type = NEURON_PLATFORM_TYPE_PDS,
		.family = NEURON_PLATFORM_FAMILY_TRN3_GEN1_ULTRASERVER,
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL },
		.device_count = 16,
		.default_node_cnt = 4,
		.pod_type = NEURON_POD_TYPE_SWITCH,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_FW_IO,
		.max_bars = 1,
		.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_SWITCH, .switch_type = NEURON_SWITCH_TYPE_V2 }, .inter_rack = { .topology = NEURON_INTER_RACK_TOPOLOGY_P2P } },
		.nc_efa_bdf_tbl = { .bdfs = trn3pds_nc_efa_bdf_tbl, .count = sizeof(trn3pds_nc_efa_bdf_tbl) / sizeof(trn3pds_nc_efa_bdf_tbl[0]) },
		.num_sleds_per_server = 8,
		.num_chips_per_sled = 2,
	},
	{
		.instance_names = v4_pds_v1_switch_instance_names,
		.type = NEURON_PLATFORM_TYPE_PDS,
		.family = NEURON_PLATFORM_FAMILY_TRN3_GEN1_ULTRASERVER,
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL },
		.device_count = 16,
		.default_node_cnt = 4,
		.pod_type = NEURON_POD_TYPE_SWITCH,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_FW_IO,
		.max_bars = 1,
		.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_SWITCH, .switch_type = NEURON_SWITCH_TYPE_V1 }, .inter_rack = { .topology = NEURON_INTER_RACK_TOPOLOGY_P2P } },
		.nc_efa_bdf_tbl = { .bdfs = trn3pds_nc_efa_bdf_tbl, .count = sizeof(trn3pds_nc_efa_bdf_tbl) / sizeof(trn3pds_nc_efa_bdf_tbl[0]) },
		.num_sleds_per_server = 8,
		.num_chips_per_sled = 2,
	},
	{
		.instance_names = v4_max_v2_switch_instance_names,
		.type = NEURON_PLATFORM_TYPE_MAX,
		.family = NEURON_PLATFORM_FAMILY_TRN3_GEN2_ULTRASERVER,
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL },
		.device_count = 4,
		.default_node_cnt = 1,
		.pod_type = NEURON_POD_TYPE_SWITCH,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_FW_IO,
		.max_bars = 2,
		.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_SWITCH, .switch_type = NEURON_SWITCH_TYPE_V2 }, .inter_rack = { .topology = NEURON_INTER_RACK_TOPOLOGY_P2P } },
		.num_sleds_per_server = 1,
		.num_chips_per_sled = 4,
	},
	{
		.instance_names = v4_max_v1_switch_instance_names,
		.type = NEURON_PLATFORM_TYPE_MAX,
		.family = NEURON_PLATFORM_FAMILY_TRN3_GEN2_ULTRASERVER,
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL },
		.device_count = 4,
		.default_node_cnt = 1,
		.pod_type = NEURON_POD_TYPE_SWITCH,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_FW_IO,
		.max_bars = 2,
		.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_SWITCH, .switch_type = NEURON_SWITCH_TYPE_V1 }, .inter_rack = { .topology = NEURON_INTER_RACK_TOPOLOGY_P2P } },
		.num_sleds_per_server = 1,
		.num_chips_per_sled = 4,
	},
	{
		.instance_names = v4_3xl_instance_names,
		.type = NEURON_PLATFORM_TYPE_STD,
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL },
		.device_count = 1,
		.default_node_cnt = 0,
		.pod_type = NEURON_POD_TYPE_NONE,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_NONE,
		.max_bars = 0,
		.num_sleds_per_server = 1,
		.num_chips_per_sled = 1,
	},
	{
		// Simulated (QEMU/inkling) MAX compute sled.  A simulated compute
		// sled is one QEMU guest standing in for the sled's integrated host
		// CPU, so a guest presents like the physical single-sled MAX
		// instances above: same SEQUENTIAL routing over the sled's four
		// MLAs.  This entry differs from the physical MAX descriptors only
		// where the device model falls short of real FW today:
		//   - pod_discovery NONE: the model implements no FW_IO pod
		//     registers (serial number, partition size, reservation id) yet.
		//   - max_bars 0: no switch BAR-info emulation yet.
		// Both are staged, not permanent: they follow as soon as the device
		// model grows the corresponding FW_IO surface.  The simulation-only
		// instance name cannot appear on real hardware, and the entry is
		// only selectable from a simulated instance (see
		// ndhal_select_platform_v4).
		.instance_names = v4_qemu_max_instance_names,
		.type = NEURON_PLATFORM_TYPE_MAX,
		.family = NEURON_PLATFORM_FAMILY_TRN3_GEN2_ULTRASERVER,
		.routing_config = { .type = NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL },
		.device_count = 4,
		.default_node_cnt = 1,
		.pod_type = NEURON_POD_TYPE_SWITCH,
		.pod_discovery = NEURON_POD_DISCOVERY_TYPE_NONE,
		.max_bars = 0,
		.interconnect = { .intra_rack = { .topology = NEURON_INTRA_RACK_TOPOLOGY_SWITCH, .switch_type = NEURON_SWITCH_TYPE_V1 }, .inter_rack = { .topology = NEURON_INTER_RACK_TOPOLOGY_P2P } },
		.num_sleds_per_server = 1,
		.num_chips_per_sled = 4,
	},
	{ }
};

static const struct neuron_platform_desc *ndhal_select_platform_v4(void)
{
	const struct neuron_platform_desc *platform;

	platform = nplatform_get_platform_desc(v4_platform_table);

	// QEMU/emulator: a simulated system opts in to a platform descriptor by
	// publishing the simulation-only instance name.  A simulated system
	// publishing anything else -- in particular the real instance names
	// every pre-existing simulated system publishes -- presents as STD
	// exactly as before, rather than matching a physical descriptor whose
	// probe requirements the simulation may not satisfy.
	if (narch_is_qemu() || narch_is_emu()) {
		if (platform && platform->instance_names == v4_qemu_max_instance_names)
			return platform;
		return &v4_platform_std;
	}

	return platform ? platform : &v4_platform_std;
}

static u32 neuron_pci_routing_id_to_user_id(u32 routing_id)
{
	u32 index = routing_id % ndhal->ndhal_arch.platform->device_count;

	if (ndhal->ndhal_arch.platform->routing_config.type == NEURON_PLATFORM_ROUTING_TYPE_TABLE) {
		return ndhal->ndhal_arch.platform->routing_config.table.map[index];
	}
	return index;
}

static bool ndhal_instance_type_3xl(void)
{
	char buf[128];
	int i;

	if (narch_get_instance_type_name(buf, sizeof(buf))) {
		return false;
	}
	for (i = 0; v4_3xl_instance_names[i]; i++) {
		if (strncmp(buf, v4_3xl_instance_names[i], strlen(v4_3xl_instance_names[i])) == 0)
			return true;
	}
	return false;
}

/**
 * neuron_pci_get_device_id() - get device id and set nd->device_index
 *
 * @param dev: PCI device
 * @param nd: neuron device
 * @return int: 0 on success, otherwise on failure
 */
static int neuron_pci_get_device_id_v4(struct neuron_device *nd, struct pci_dev *dev)
{
	int ret = 0;
	int i;
	u32 routing_id = (u32)-1;
	u32 routing_id_max = MAX_NEURON_DEVICE_COUNT;

	if (ndhal_instance_type_3xl()) {
		// Temporarily auto-assign routing_id to 0 for 3xl instances, since they
		// only have 1 device anyways
		routing_id = 0;
	} else {
		// Poll the device id until the device is ready
		for (i = 0; i < 20; i++) {
			ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
			if (!ret && routing_id != 0xdeadbeef) {
				break;
			}
			msleep(1000);
		}
	}

	if (ret) {
		pr_err("Could not retrieve device index (read timeout)");
		return -ENODEV;
	}

	if (ndhal->ndhal_arch.platform->interconnect.intra_rack.topology == NEURON_INTRA_RACK_TOPOLOGY_SWITCH) {
		int server_id;

		ret = fw_io_server_info_read(nd->npdev.bar0, &server_id, NULL);
		if (ret) {
			return -ENODEV;
		}

		if (server_id == -1) {
			pr_err("Could not retrieve valid server id, ret = %d\n", ret);
			return -ENODEV;
		}
		ndhal->ndhal_arch.server_id = server_id;
		routing_id_max = MAX_NEURON_DEVICE_COUNT * 2;
	}

	// TODO - this should be a "valid routing_id check for TRN3
	if (routing_id < 0 || routing_id >= routing_id_max) {
		pr_err("Invalid device index %u", routing_id);
		return -ENODEV;
	}

	nd->device_index = neuron_pci_routing_id_to_user_id(routing_id);

	pr_err("** BDF: %2.2x:%2.2x.%x => nd[%d] (routing id: %u)\n", dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn), nd->device_index, routing_id);

	// protection against duplicate IDs - doesn't provide 100% protection in multi-threaded device discovery
	if (neuron_devices[nd->device_index] != NULL) {
		pr_err("duplicate routing id %u found\n", routing_id);
		neuron_pci_handle_dup_routing_id();
		return -ENODEV;
	}

	return 0;
}

#define NC_MAPPING_MAX_CORE_COUNT_V4 128
static const struct neuron_ioctl_nc_map_entry nc_mapping_v0_seng_swap_pds[] = {
	{ .device_id = 0,  .device_nc_idx = 4 }, { .device_id = 0,  .device_nc_idx = 5 }, { .device_id = 0,  .device_nc_idx = 6 }, { .device_id = 0,  .device_nc_idx = 7 }, { .device_id = 0,  .device_nc_idx = 2 }, { .device_id = 0,  .device_nc_idx = 3  }, { .device_id = 0,  .device_nc_idx = 0 }, { .device_id = 0,  .device_nc_idx = 1 }, // ND0
	{ .device_id = 1,  .device_nc_idx = 6 }, { .device_id = 1,  .device_nc_idx = 7 }, { .device_id = 1,  .device_nc_idx = 4 }, { .device_id = 1,  .device_nc_idx = 5 }, { .device_id = 1,  .device_nc_idx = 0 }, { .device_id = 1,  .device_nc_idx = 1  }, { .device_id = 1,  .device_nc_idx = 2 }, { .device_id = 1,  .device_nc_idx = 3 }, // ND1
	{ .device_id = 2,  .device_nc_idx = 4 }, { .device_id = 2,  .device_nc_idx = 5 }, { .device_id = 2,  .device_nc_idx = 6 }, { .device_id = 2,  .device_nc_idx = 7 }, { .device_id = 2,  .device_nc_idx = 2 }, { .device_id = 2,  .device_nc_idx = 3  }, { .device_id = 2,  .device_nc_idx = 0 }, { .device_id = 2,  .device_nc_idx = 1 }, // ND2
	{ .device_id = 3,  .device_nc_idx = 6 }, { .device_id = 3,  .device_nc_idx = 7 }, { .device_id = 3,  .device_nc_idx = 4 }, { .device_id = 3,  .device_nc_idx = 5 }, { .device_id = 3,  .device_nc_idx = 0 }, { .device_id = 3,  .device_nc_idx = 1  }, { .device_id = 3,  .device_nc_idx = 2 }, { .device_id = 3,  .device_nc_idx = 3 }, // ND3
	{ .device_id = 4,  .device_nc_idx = 4 }, { .device_id = 4,  .device_nc_idx = 5 }, { .device_id = 4,  .device_nc_idx = 6 }, { .device_id = 4,  .device_nc_idx = 7 }, { .device_id = 4,  .device_nc_idx = 2 }, { .device_id = 4,  .device_nc_idx = 3  }, { .device_id = 4,  .device_nc_idx = 0 }, { .device_id = 4,  .device_nc_idx = 1 }, // ND4
	{ .device_id = 5,  .device_nc_idx = 6 }, { .device_id = 5,  .device_nc_idx = 7 }, { .device_id = 5,  .device_nc_idx = 4 }, { .device_id = 5,  .device_nc_idx = 5 }, { .device_id = 5,  .device_nc_idx = 0 }, { .device_id = 5,  .device_nc_idx = 1  }, { .device_id = 5,  .device_nc_idx = 2 }, { .device_id = 5,  .device_nc_idx = 3 }, // ND5
	{ .device_id = 6,  .device_nc_idx = 4 }, { .device_id = 6,  .device_nc_idx = 5 }, { .device_id = 6,  .device_nc_idx = 6 }, { .device_id = 6,  .device_nc_idx = 7 }, { .device_id = 6,  .device_nc_idx = 2 }, { .device_id = 6,  .device_nc_idx = 3  }, { .device_id = 6,  .device_nc_idx = 0 }, { .device_id = 6,  .device_nc_idx = 1 }, // ND6
	{ .device_id = 7,  .device_nc_idx = 6 }, { .device_id = 7,  .device_nc_idx = 7 }, { .device_id = 7,  .device_nc_idx = 4 }, { .device_id = 7,  .device_nc_idx = 5 }, { .device_id = 7,  .device_nc_idx = 0 }, { .device_id = 7,  .device_nc_idx = 1  }, { .device_id = 7,  .device_nc_idx = 2 }, { .device_id = 7,  .device_nc_idx = 3 }, // ND7
	{ .device_id = 8,  .device_nc_idx = 4 }, { .device_id = 8,  .device_nc_idx = 5 }, { .device_id = 8,  .device_nc_idx = 6 }, { .device_id = 8,  .device_nc_idx = 7 }, { .device_id = 8,  .device_nc_idx = 2 }, { .device_id = 8,  .device_nc_idx = 3  }, { .device_id = 8,  .device_nc_idx = 0 }, { .device_id = 8,  .device_nc_idx = 1 }, // ND8
	{ .device_id = 9,  .device_nc_idx = 6 }, { .device_id = 9,  .device_nc_idx = 7 }, { .device_id = 9,  .device_nc_idx = 4 }, { .device_id = 9,  .device_nc_idx = 5 }, { .device_id = 9,  .device_nc_idx = 0 }, { .device_id = 9,  .device_nc_idx = 1  }, { .device_id = 9,  .device_nc_idx = 2 }, { .device_id = 9,  .device_nc_idx = 3 }, // ND9
	{ .device_id = 10, .device_nc_idx = 4 }, { .device_id = 10, .device_nc_idx = 5 }, { .device_id = 10, .device_nc_idx = 6 }, { .device_id = 10, .device_nc_idx = 7 }, { .device_id = 10, .device_nc_idx = 2 }, { .device_id = 10, .device_nc_idx = 3  }, { .device_id = 10, .device_nc_idx = 0 }, { .device_id = 10, .device_nc_idx = 1 }, // ND10
	{ .device_id = 11, .device_nc_idx = 6 }, { .device_id = 11, .device_nc_idx = 7 }, { .device_id = 11, .device_nc_idx = 4 }, { .device_id = 11, .device_nc_idx = 5 }, { .device_id = 11, .device_nc_idx = 0 }, { .device_id = 11, .device_nc_idx = 1  }, { .device_id = 11, .device_nc_idx = 2 }, { .device_id = 11, .device_nc_idx = 3 }, // ND11
	{ .device_id = 12, .device_nc_idx = 4 }, { .device_id = 12, .device_nc_idx = 5 }, { .device_id = 12, .device_nc_idx = 6 }, { .device_id = 12, .device_nc_idx = 7 }, { .device_id = 12, .device_nc_idx = 2 }, { .device_id = 12, .device_nc_idx = 3  }, { .device_id = 12, .device_nc_idx = 0 }, { .device_id = 12, .device_nc_idx = 1 }, // ND12
	{ .device_id = 13, .device_nc_idx = 6 }, { .device_id = 13, .device_nc_idx = 7 }, { .device_id = 13, .device_nc_idx = 4 }, { .device_id = 13, .device_nc_idx = 5 }, { .device_id = 13, .device_nc_idx = 0 }, { .device_id = 13, .device_nc_idx = 1  }, { .device_id = 13, .device_nc_idx = 2 }, { .device_id = 13, .device_nc_idx = 3 }, // ND13
	{ .device_id = 14, .device_nc_idx = 4 }, { .device_id = 14, .device_nc_idx = 5 }, { .device_id = 14, .device_nc_idx = 6 }, { .device_id = 14, .device_nc_idx = 7 }, { .device_id = 14, .device_nc_idx = 2 }, { .device_id = 14, .device_nc_idx = 3  }, { .device_id = 14, .device_nc_idx = 0 }, { .device_id = 14, .device_nc_idx = 1 }, // ND14
	{ .device_id = 15, .device_nc_idx = 6 }, { .device_id = 15, .device_nc_idx = 7 }, { .device_id = 15, .device_nc_idx = 4 }, { .device_id = 15, .device_nc_idx = 5 }, { .device_id = 15, .device_nc_idx = 0 }, { .device_id = 15, .device_nc_idx = 1  }, { .device_id = 15, .device_nc_idx = 2 }, { .device_id = 15, .device_nc_idx = 3 }, // ND15
};

#define NC_MAPPING_V0_SENG_SWAP_SIZE (sizeof(nc_mapping_v0_seng_swap_pds) / sizeof(nc_mapping_v0_seng_swap_pds[0]))
static_assert((NC_MAPPING_V0_SENG_SWAP_SIZE == NC_MAPPING_MAX_CORE_COUNT_V4) && (NC_MAPPING_V0_SENG_SWAP_SIZE <= NEURON_NC_MAP_MAX_ENTRIES));

static int ncdev_logical_to_physical_nc_map_v4(struct neuron_ioctl_nc_map *map, uint32_t max_num_entries, enum neuron_ioctl_nc_mapping_type version)
{
	uint32_t entry_idx;
	uint32_t entries_to_copy = (max_num_entries < NC_MAPPING_MAX_CORE_COUNT_V4) ? max_num_entries : NC_MAPPING_MAX_CORE_COUNT_V4;
	const struct neuron_ioctl_nc_map_entry *mapping;

	if (version != NEURON_IOCTL_NC_MAPPING_TYPE_V0) {
		pr_err("Unsupported Neuron Core Mapping verion %u for v4 arch", version);
		return -EINVAL;
	}
	mapping = nc_mapping_v0_seng_swap_pds;

	for (entry_idx = 0; entry_idx < entries_to_copy; entry_idx++) {
		uint32_t core_idx = entry_idx;
		WARN_ONCE(core_idx >= NC_MAPPING_MAX_CORE_COUNT_V4, "core_idx %d > max core count %d", core_idx, NC_MAPPING_MAX_CORE_COUNT_V4);
		map->mappings[entry_idx] = mapping[core_idx];
	}
	map->num_entries = entries_to_copy;

	return 0;
}

#define NC_MAPPING_MAX_CORE_COUNT_MAX_V4 32
static const struct neuron_ioctl_nc_map_entry nc_mapping_v0_seng_swap_max[] = {
	{ .device_id = 0,  .device_nc_idx = 2 }, { .device_id = 0,  .device_nc_idx = 3 }, { .device_id = 0,  .device_nc_idx = 0 }, { .device_id = 0,  .device_nc_idx = 1 }, { .device_id = 0,  .device_nc_idx = 4 }, { .device_id = 0,  .device_nc_idx = 5  }, { .device_id = 0,  .device_nc_idx = 6 }, { .device_id = 0,  .device_nc_idx = 7 }, // ND0
	{ .device_id = 1,  .device_nc_idx = 2 }, { .device_id = 1,  .device_nc_idx = 3 }, { .device_id = 1,  .device_nc_idx = 0 }, { .device_id = 1,  .device_nc_idx = 1 }, { .device_id = 1,  .device_nc_idx = 4 }, { .device_id = 1,  .device_nc_idx = 5  }, { .device_id = 1,  .device_nc_idx = 6 }, { .device_id = 1,  .device_nc_idx = 7 }, // ND1
	{ .device_id = 2,  .device_nc_idx = 4 }, { .device_id = 2,  .device_nc_idx = 5 }, { .device_id = 2,  .device_nc_idx = 6 }, { .device_id = 2,  .device_nc_idx = 7 }, { .device_id = 2,  .device_nc_idx = 2 }, { .device_id = 2,  .device_nc_idx = 3  }, { .device_id = 2,  .device_nc_idx = 0 }, { .device_id = 2,  .device_nc_idx = 1 }, // ND2
	{ .device_id = 3,  .device_nc_idx = 4 }, { .device_id = 3,  .device_nc_idx = 5 }, { .device_id = 3,  .device_nc_idx = 6 }, { .device_id = 3,  .device_nc_idx = 7 }, { .device_id = 3,  .device_nc_idx = 2 }, { .device_id = 3,  .device_nc_idx = 3  }, { .device_id = 3,  .device_nc_idx = 0 }, { .device_id = 3,  .device_nc_idx = 1 }, // ND3
};

#define NC_MAPPING_V0_SENG_SWAP_MAX_SIZE (sizeof(nc_mapping_v0_seng_swap_max) / sizeof(nc_mapping_v0_seng_swap_max[0]))
static_assert((NC_MAPPING_V0_SENG_SWAP_MAX_SIZE == NC_MAPPING_MAX_CORE_COUNT_MAX_V4) && (NC_MAPPING_V0_SENG_SWAP_MAX_SIZE <= NEURON_NC_MAP_MAX_ENTRIES));
static int ncdev_logical_to_physical_nc_map_max_v4(struct neuron_ioctl_nc_map *map, uint32_t max_num_entries, enum neuron_ioctl_nc_mapping_type version)
{
	uint32_t entry_idx;
	uint32_t entries_to_copy = (max_num_entries < NC_MAPPING_MAX_CORE_COUNT_MAX_V4) ? max_num_entries : NC_MAPPING_MAX_CORE_COUNT_MAX_V4;
	const struct neuron_ioctl_nc_map_entry *mapping;

	if (version != NEURON_IOCTL_NC_MAPPING_TYPE_V0) {
		pr_err("Unsupported Neuron Core Mapping verion %u for v4 arch", version);
		return -EINVAL;
	}
	mapping = nc_mapping_v0_seng_swap_max;

	for (entry_idx = 0; entry_idx < entries_to_copy; entry_idx++) {
		uint32_t core_idx = entry_idx;
		WARN_ONCE(core_idx >= NC_MAPPING_MAX_CORE_COUNT_MAX_V4, "core_idx %d > max core count %d", core_idx, NC_MAPPING_MAX_CORE_COUNT_MAX_V4);
		map->mappings[entry_idx] = mapping[core_idx];
	}
	map->num_entries = entries_to_copy;

	return 0;
}

static void perf_update_hbm_7200_supported_v4(struct neuron_device *nd) {
	nd->supports_hbm_7200 = 0;
	return;
}

/**
 * ndma_get_engines_with_host_connectivity - get DMA engines for a particular HBM index which have host connectivity
 * V4 - unlike V3, all DMA engines for an HBM have host connectivity, so use all 32.
 */
static void ndma_get_engines_with_host_connectivity_v4(u32 hbm_index, u32 engines[NUM_DMA_ENG_PER_DEVICE], u32 *num_engines)
{
	const int num_dma_engines_per_hbm = 32;
	const int offset = num_dma_engines_per_hbm * hbm_index;

	int i = 0;
	for (i = 0; i < num_dma_engines_per_hbm; i++) {
		engines[i] = offset + i;
	}
	*num_engines = num_dma_engines_per_hbm;
}

static int neuron_pci_device_id_to_rid_map_v4(uint32_t *count, uint32_t *did_to_rid_map)
{
	int i;

	for (i = 0; i < total_neuron_devices; i++) {
		u32 routing_id;
		if (ndhal->ndhal_arch.platform->interconnect.intra_rack.topology == NEURON_INTRA_RACK_TOPOLOGY_SWITCH) {
			routing_id = i + ndhal->ndhal_arch.server_id * total_neuron_devices;
		} else {
			routing_id = i;
		}
		did_to_rid_map[neuron_pci_routing_id_to_user_id(routing_id)] = routing_id;
	}

	*count = total_neuron_devices;
	return 0;
}

/**
 * ndhal_register_funcs_v4() - initialize the dhal for v4 chips
 *
 * This function only overrides the functions and
 * constants that are different from v3 in v4.
 */
int ndhal_register_funcs_v4(void) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for V4.");
		return -EINVAL;
	}

	ndhal->ndhal_arch.platform = ndhal_select_platform_v4();
	ndhal->ndhal_fw_io.new_readless_read_min_api_version = 6;
	fw_metrics_init_reader_v4();
	ndhal->ndhal_pci.neuron_pci_get_device_id = neuron_pci_get_device_id_v4;
	ndhal->ndhal_pci.neuron_pci_device_id_to_rid_map = neuron_pci_device_id_to_rid_map_v4;
	ndhal->ndhal_npe.npe_neighbor_eng_ids = npe_neighbor_eng_ids_v4;
	ndhal->ndhal_mpset.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v4;
	ndhal->ndhal_mmap.dm_mmap_special = dm_mmap_special_v4;
	ndhal->ndhal_mmap.mmap_get_bar4_offset = mmap_get_bar4_offset_v4;
	ndhal->ndhal_ndma.ndma_get_engines_with_host_connectivity = ndma_get_engines_with_host_connectivity_v4;
	ndhal->ndhal_ndmar.ndmar_ctx_queue_bit = ndmar_ctx_queue_bit_v4;
	ndhal->ndhal_ndmar.ndmar_ctx_queue_from_bit = ndmar_ctx_queue_from_bit_v4;
	ndhal->ndhal_nq.nnq_get_nq_h2d_id = nnq_get_nq_h2d_id_v4;
	ndhal->ndhal_nq.nnq_get_h2d_id = nnq_get_h2d_id_v4;
	ndhal->ndhal_cdev.ncdev_mem_regions = ncdev_mem_regions_v4;
	ndhal->ndhal_perf.perf_update_hbm_7200_supported = perf_update_hbm_7200_supported_v4;
	ndhal->ndhal_sysfs_metrics.health_status_enabled = true;
	ndhal->ndhal_address_map.hbm_active_size = V4_HBM_ACTIVE_SIZE;
	ndhal->ndhal_address_map.supported_lnc_degrees = BIT(1) | BIT(2);

	if (narch_is_emu()) {
		// Temporarily disable resets on emulation until support is ready
		extern int no_reset;
		no_reset = 1;
	}

	// TODO initialization needs refactoring because V4 is piggybacking on V3
	// which risks double calling any hal init functions
	//
	if (ndhal->ndhal_arch.platform->pod_discovery == NEURON_POD_DISCOVERY_TYPE_ELECTION) {
		ret = npe_init();
		if (ret) {
			pr_err("failed to initialize pod election on V4\n");
			return ret;
		}
	} else if (ndhal->ndhal_arch.platform->type == NEURON_PLATFORM_TYPE_PDS) {
		//TODO PDS
	    ndhal->ndhal_cdev.ncdev_logical_to_physical_nc_map = ncdev_logical_to_physical_nc_map_v4;
	} else if (ndhal->ndhal_arch.platform->type == NEURON_PLATFORM_TYPE_MAX) {
	    ndhal->ndhal_cdev.ncdev_logical_to_physical_nc_map = ncdev_logical_to_physical_nc_map_max_v4;
	}

	switch (ndhal->pci_device_id) {
		case TRN3_DEVICE_ID0:
		case TRN3_DEVICE_ID1:
			ret = ndhal_register_funcs_trn3();
			if (ret) {
				pr_err("failed to register ndhal funcs for trn3.\n");
				return ret;
			}
			break;
		default:
			pr_err("Unknown HW architecture. Can't init neuron_dhal.\n");
			return -EINVAL;
	}

	return ret;
}

