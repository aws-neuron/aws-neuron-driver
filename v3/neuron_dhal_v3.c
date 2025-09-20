// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/kernel_read_file.h>
#endif

#include "sdma.h"
#include "notific.h"
#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_topsp.h"
#include "../neuron_mmap.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "../neuron_fw_io.h"
#include "../neuron_pci.h"
#include "../neuron_trace.h"
#include "../neuron_cdev.h"
#include "../neuron_sysfs_metrics.h"
#include "../neuron_ring.h"
#include "../neuron_mempool.h"
#include "neuron_pelect.h"

extern int dev_nc_map;

#define NR_RESET_RETRY_SLEEP_MS                     100
#define V3_NR_RESET_INIT_MAX_TOTAL_WAIT_TIME_MS     (1000 * 480)
#define V3_NR_RESET_POLL_INTERVAL                   100

int force_userver = 0;
module_param(force_userver , int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(force_userver, "Force Neuron UltraServer");

// For v3/testing
//
int force_die_flip = 0;
module_param(force_die_flip, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(force_die_flip, "Force Neuron Core Mapping APIs to give back DIE flip mappings");

// TOP SP addresses are sparse on chip adjust to accommodate the table macro
//
#define V3_TOP_SP_GRP1_BASE V3_TOP_SP_0_BASE
#define V3_TOP_SP_GRP2_BASE (V3_TOP_SP_10_BASE - 8 * V3_TOP_SP_DIST)

#define V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET V3_PCIE_BAR0_TOP_SP_0_OFFSET
#define V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET (V3_PCIE_BAR0_TOP_SP_10_OFFSET - 8 * V3_TOP_SP_SIZE)

struct neuron_dm_special_mmap_ent dm_mmap_special_v3[] = {
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   4, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   5, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   6, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   7, NEURON_DM_RESOURCE_SEMAPHORE, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, V3_MMAP_NC_EVENT_OFFSET, V3_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   4, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   5, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   6, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   7, NEURON_DM_RESOURCE_SBUF, V3_MMAP_TPB_0_BASE, V3_PCIE_BAR0_TPB_0_OFFSET, V3_PCIE_BAR0_TPB_DIST, V3_PCIE_BAR0_TPB_SIZE, 0, V3_PCIE_BAR0_TPB_SBUF_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 6, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 7, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  8, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  9, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 10, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 11, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 12, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 13, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 14, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 15, NEURON_DM_RESOURCE_SEMAPHORE, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST,  V3_TOP_SP_SIZE, 0, V3_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 6, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 7, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP1_BASE, V3_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  8, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  9, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 10, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 11, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 12, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 13, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 14, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 15, NEURON_DM_RESOURCE_ALL, V3_TOP_SP_GRP2_BASE, V3_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V3_TOP_SP_DIST, V3_TOP_SP_SIZE, 0, V3_TOP_SP_SIZE, 0),

	{.block = NEURON_DM_BLOCK_HBM, .block_id = 0, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V3_HBM_0_BASE, .size = V3_HBM_ACTIVE_SIZE, .bar_offset = V3_HBM_SIZE * 0, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 1, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V3_HBM_1_BASE, .size = V3_HBM_ACTIVE_SIZE, .bar_offset = V3_HBM_SIZE * 1, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 2, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V3_HBM_2_BASE, .size = V3_HBM_ACTIVE_SIZE, .bar_offset = V3_HBM_SIZE * 2, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 3, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V3_HBM_3_BASE, .size = V3_HBM_ACTIVE_SIZE, .bar_offset = V3_HBM_SIZE * 3, .bar_num = 4},

	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

struct ncdev_mem_region ncdev_mem_regions_v3[] = {
	{ V3_MMAP_TPB_0_BASE, V3_MMAP_NC_SIZE },    // FIXME this is inefficient this may need a routine to slice and range check
	{ V3_MMAP_TPB_1_BASE, V3_MMAP_NC_SIZE },
	{ V3_MMAP_TPB_2_BASE, V3_MMAP_NC_SIZE },
	{ V3_MMAP_TPB_3_BASE, V3_MMAP_NC_SIZE },
	{ V3_MMAP_TPB_4_BASE, V3_MMAP_NC_SIZE },
	{ V3_MMAP_TPB_5_BASE, V3_MMAP_NC_SIZE },
	{ V3_MMAP_TPB_6_BASE, V3_MMAP_NC_SIZE },
	{ V3_MMAP_TPB_7_BASE, V3_MMAP_NC_SIZE },
	{ V3_TOP_SP_0_BASE, V3_TOP_SP_SIZE },       // could flatten TOP_SP
	{ V3_TOP_SP_1_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_2_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_3_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_4_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_5_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_6_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_7_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_8_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_9_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_10_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_11_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_12_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_13_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_14_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_15_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_16_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_17_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_18_BASE, V3_TOP_SP_SIZE },
	{ V3_TOP_SP_19_BASE, V3_TOP_SP_SIZE },
	{ V3_HBM_0_BASE, V3_HBM_ACTIVE_SIZE },
	{ V3_HBM_1_BASE, V3_HBM_ACTIVE_SIZE },
	{ V3_HBM_2_BASE, V3_HBM_ACTIVE_SIZE },
	{ V3_HBM_3_BASE, V3_HBM_ACTIVE_SIZE },
	{ V3_PREPROC_0_BASE, V3_PREPROC_SIZE},
	{ V3_PREPROC_1_BASE, V3_PREPROC_SIZE},
	{ V3_PREPROC_2_BASE, V3_PREPROC_SIZE},
	{ V3_PREPROC_3_BASE, V3_PREPROC_SIZE},
	{ NCDEV_MEM_REGION_INVALID, 0 },
};

u64 ncdev_bar0_write_blocked_addrs_v3[] = {
	V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET,
	V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET,
	V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET,
	V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET,
	V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET,
	MMAP_BAR0_APB_MISC_RAM_INVALID,
};

#define V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(base, apb_io_base, pcie_bar0_apb_offset) (base - apb_io_base + pcie_bar0_apb_offset + V3_TPB_ARR_SEQ_QUEUE_PERF_BASE_OFFSET)
#define V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(base, xbus_id, offset) (base + (xbus_id * V3_TPB_ARR_SEQ_QUEUE_PERF_SIZE) + offset)

u64 ntpb_pe_mm_cntr_offsets_v3[V3_NC_PER_DEVICE] =
{
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_MATMUL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
};

u64 ntpb_pe_wl_cntr_offsets_v3[V3_NC_PER_DEVICE] =
{
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 8, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
};

u64 ntpb_pe_fast_wl_cntr_offsets_v3[V3_NC_PER_DEVICE] =
{
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 0, V3_TPB_ARR_SEQ_QUEUE_PERF_WL_ACTIVE_CYCLE_CNT_LSB_OFFSET),
};

u64 ntpb_pe_idle_cntr_offsets_v3[V3_NC_PER_DEVICE] =
{
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_0_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_0_BASE, V3_PCIE_BAR0_APB_IO_0_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_0_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_0_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
	V3_TPB_PE_ACTIVITY_COUNTER_OFFSET(V3_TPB_PE_SEQ_QUEUE_PERF_OFFSET(V3_APB_IO_1_USER_SE_1_TPB_1_PE_SEQ_CLUSTER_HOST_VISIBLE_BASE, V3_APB_IO_1_BASE, V3_PCIE_BAR0_APB_IO_1_OFFSET), 4, V3_TPB_ARR_SEQ_QUEUE_PERF_IDLE_CYCLE_CNT_LSB_OFFSET),
};

static int ndhal_register_funcs_trn2(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for trn2.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v3";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v3";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Trn2";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Trainium2";
	return 0;
}

static bool ndhal_instance_type_pod(void)
{
	static bool instance_type_is_pod = false;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#define NEURON_TRN2P_INSTANCE_NAME "trn2p.48xlarge"
#define NEURON_TRN2EU_INSTANCE_NAME "trn2eu.48xlarge"
#define NEURON_TRN2U_INSTANCE_NAME "trn2u.48xlarge"
	static bool initialized = false;
	ssize_t len;
	ssize_t file_size;
	void *buf = NULL;

	if (initialized) {
		return instance_type_is_pod;
	}

	initialized = true;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);

	if (buf == NULL) {
		pr_err("failed to allocate buffer to read instance type");
		goto done;
	}

	len = kernel_read_file_from_path("/sys/class/dmi/id/product_name",
									 0, &buf, 64, &file_size, READING_UNKNOWN);

	if (!len) {
		pr_err("read instance type failed");
		goto done;
	}

	if ((strncmp(buf, NEURON_TRN2P_INSTANCE_NAME, sizeof(NEURON_TRN2P_INSTANCE_NAME)-1) == 0) ||
	    (strncmp(buf, NEURON_TRN2EU_INSTANCE_NAME, sizeof(NEURON_TRN2EU_INSTANCE_NAME)-1) == 0) ||
	    (strncmp(buf, NEURON_TRN2U_INSTANCE_NAME, sizeof(NEURON_TRN2U_INSTANCE_NAME)-1) == 0)) {
		instance_type_is_pod = true;
	}

done:
	if (buf) {
		kfree(buf);
	}
#endif
	if (force_userver) {
		instance_type_is_pod = true;
	}

	return instance_type_is_pod;
}



/* Device Reset Functions */
/**
 * nr_get_tpb_reset_map() - generates a the reset map of all resources associated with resetting a particular TPB
 *
 *   Map is as follows:
 *       8x TPBs            bits 0-7
 *       8x SDMA groups     bits 8-15  (16 per group)
 *       8x TOPSP groups    bits 16-23 (2 per group)
 *       8x CC TOP groups   bits 24-31 (2 per group)
 *
 * / Break down mappings from bv: 8x TPBs, 8x SDMA groups (16 per group)
    // 8x TOP_SP groups (2 per group), 8x CC_TOP Q7's (2 Q7's per group)
    uint8_t tpb_bv = reset_unit_index_bv & 0xFFU;
    uint8_t sdma_bv = (reset_unit_index_bv >> 8) & 0xFFU;
    uint8_t top_sp_bv = (reset_unit_index_bv >> 16) & 0xFFU;
    uint8_t cc_top_bv = (reset_unit_index_bv >> 24) & 0xFU; // Note: 4b here instead of 8b
 *
 */
static void nr_get_tpb_reset_map(uint32_t nc_map, uint32_t *tpb_reset_map)
{
	int i;

	// Build the tpb reset map if we are not performing a device reset
	if (nc_map != NEURON_NC_MAP_DEVICE) {
		for (i = 0; i < MAX_NC_PER_DEVICE; i++) {
			if ((1 << i) & nc_map) {
				// Add this tpb to the reset map
				*tpb_reset_map |= (1 << i);
				*tpb_reset_map |= (1 << (i+8));   // SDMA group for this core
				*tpb_reset_map |= (1 << (i+16));  // TOP SP group for this core
			}
		}
	}
}

/**
 * nr_initiate_reset() - initialize a reset
 *
 * @param nd - Neuron device which will be reset by the thread.
 */
static int nr_initiate_reset_v3(struct neuron_device *nd, uint32_t nc_map)
{
	if (no_reset)
		return 0;

	uint32_t tpb_reset_map = 0;
	nr_get_tpb_reset_map(nc_map, &tpb_reset_map);

	int ret = nr_initiate_reset_via_fw(nd, nc_map, tpb_reset_map);
	if (ret) {
		return ret;
	}

	return 0;
}

static int nr_initiate_reset_v3_qemu(struct neuron_device *nd, uint32_t nc_map)
{
	if (no_reset)
		return 0;

    uint32_t tpb_reset_map = 0;
    nr_get_tpb_reset_map(nc_map, &tpb_reset_map);
	volatile void *addr = nd->npdev.bar0 + V3_PCIE_BAR0_APB_IO_0_OFFSET + V3_APB_IO_0_USER_SE_0_RESERVED2_RELBASE + 0x10;
	writel(tpb_reset_map, (volatile uint32_t *)addr);

	return 0;
}

static int nr_initiate_reset_v3_emu(struct neuron_device *nd, uint32_t nc_map)
{
	return nr_initiate_reset_v3(nd, nc_map);
}

/**
 * nr_wait_for_reset_completion() - wait for a reset to be completed
 *
 * @param nd - Neuron device which will be reset by the thread.
 */
static int nr_wait_for_reset_completion_v3(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	int i;
	void *addr = nd->npdev.bar0 + V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + V2_FW_IO_REG_FW_STATUS_OFFSET; // FIXME in reset.h abstract and move to somewhere in v3

	for (i = 0; i < ndhal->ndhal_reset.retry_count; i++) {
		bool reset_in_progress = true;
		u32 status;

		if (ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, &status, 1, false) == 0)
			reset_in_progress = status & V2_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK; // FIXME in reset.h abstract and move to somewhere in v3
		if (!reset_in_progress)
			return 0;
		if (nr_msleep_stoppable(nd, NR_RESET_RETRY_SLEEP_MS * i))
			return -1;
	}
	return -1;
}

static int nr_wait_for_reset_completion_v3_qemu(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	int i;
	void *addr = nd->npdev.bar0 + V3_PCIE_BAR0_APB_IO_0_OFFSET + V3_APB_IO_0_USER_SE_0_RESERVED2_RELBASE + 0x10;

	for (i = 0; i < ndhal->ndhal_reset.retry_count; i++) {
		bool reset_in_progress = true;

		reset_in_progress = readl((volatile uint32_t *)addr);
		msleep(2 * 1000);

		if (!reset_in_progress)
			return 0;
		if (nr_msleep_stoppable(nd, NR_RESET_RETRY_SLEEP_MS * i))
			return -1;
	}
	return -1;
}

static int nr_wait_for_reset_completion_v3_emu(struct neuron_device *nd)
{
	return nr_wait_for_reset_completion_v3(nd);
}

/**
 * nr_post_reset_config() - perform and post reset configuration needed
 *
 * @param nd - Neuron device which will be reset by the thread.
 * @param reset_successful - device reset was successful
 */
static int nr_post_reset_config_v3(struct neuron_device *nd, bool reset_successful)
{
	if (!ndhal_instance_type_pod()) {
		return 0;
	}

	npe_election_exec_on_rst(nd, reset_successful);
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
static u8 ts_nq_get_nqid_v3(struct neuron_device *nd, u8 index, u32 nq_type)
{
	u8 nq_id = 0;
	nq_id = (nq_type * V3_MAX_NQ_QUEUES) + index;
	return nq_id;
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
static void ts_nq_set_hwaddr_v3(struct neuron_device *nd, u8 ts_id, u8 index, u32 nq_type, u32 size,
			     u64 queue_pa)
{
	void *apb_base;
	u32 low, high;

	apb_base = nd->npdev.bar0 + notific_get_relative_offset_topsp_v3(ts_id);

	low = (u32)(queue_pa & 0xffffffff);
	high = (u32)(queue_pa >> 32U);

	notific_write_nq_base_addr_hi(apb_base, index, high);
	notific_write_nq_base_addr_lo(apb_base, index, low);
	notific_write_nq_f_size(apb_base, index, size);
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
static int ts_nq_init_v3(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
				u32 on_host_memory, u32 dram_channel, u32 dram_region,
				bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	// Check that size is power of 2
	if (size & (size - 1)) {
		pr_err("notification ring size must be power of 2");
		return -EINVAL;
	}

	if (nd == NULL || ts_id >= ndhal->ndhal_address_map.ts_per_device)
		return -EINVAL;

	u8 nq_id = ts_nq_get_nqid_v3(nd, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	struct mem_chunk *mc = nd->ts_nq_mc[ts_id][nq_id];
	if (mc == NULL || force_alloc_mem) {
		struct mem_chunk *_mc = NULL;
		u32 nc_id = ts_id / V3_TS_PER_NC;

		int ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, size, (on_host_memory) ? 0 : size, on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
				   dram_channel, dram_region, nc_id, on_host_memory ? NEURON_MEMALLOC_TYPE_NOTIFICATION_HOST : NEURON_MEMALLOC_TYPE_NOTIFICATION_DEVICE, &_mc);
		if (ret)
			return ret;
		ts_nq_set_hwaddr_v3(nd, ts_id, eng_index, nq_type, size, _mc->pa);
		nd->ts_nq_mc[ts_id][nq_id] = _mc;
		if (mc) {
			mc_free(&mc);
		}
		mc = _mc;
	}
	if (mc->mem_location == MEM_LOC_HOST)
		*mmap_offset = nmmap_offset(mc);
	else
		*mmap_offset = -1;
	*nq_mc = mc;
	return 0;
}

/**
 * ts_nq_destroy_one() - Disable notification in the device
 *
 * @nd: neuron device
 * @ts_id: topsp id
 *
 */
static void ts_nq_destroy_one_v3(struct neuron_device *nd, u8 ts_id)
{
	u8 eng_index;
	u8 nq_type;
	for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			ts_nq_destroy(nd, ts_id, eng_index, nq_type);
		}
	}
}


/* Neuron Core Functions */
/**
 * nc_get_semaphore_base() - get semaphore base address
 *
 * @param nd - neuron device
 * @param nc_id - neuron core index
 * @return void* - semaphore base address
 */
static void *nc_get_semaphore_base_v3(struct neuron_device *nd, u8 nc_id)
{
	return nd->npdev.bar0 + V3_PCIE_BAR0_TPB_0_OFFSET + (V3_PCIE_BAR0_TPB_DIST * nc_id);
}

/**
 * nc_get_event_addr() - get event address
 *
 * @param nd - neuron device
 * @param nc_id - neuron core index
 * @param event_index - event index
 * @return void* - event address
 */
static void *nc_get_event_addr_v3(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void * base = nd->npdev.bar0 + V3_PCIE_BAR0_TPB_0_OFFSET + (V3_PCIE_BAR0_TPB_DIST * nc_id) + ndhal->ndhal_address_map.mmap_nc_event_offset;
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
static u8 nnq_get_nqid_v3(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type)
{
	return (nq_type * V3_MAX_NQ_QUEUES) + index;
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
static void nnq_set_hwaddr_v3(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa)
{
	void *apb_base;
	if (nq_type == NQ_TYPE_TRACE_DMA) {
		apb_base = nd->npdev.bar0 + notific_get_relative_offset_sdma_v3(nc_id, index);
		index = 0; //in the block write it on queue 0 as the base is different
	} else {
		apb_base = nd->npdev.bar0 + notific_get_relative_offset_v3(nc_id);
	}

	u32 low = (u32)(queue_pa & 0xffffffff);
	u32 high = (u32)(queue_pa >> 32U);

	notific_write_nq_base_addr_hi(apb_base, index, high);
	notific_write_nq_base_addr_lo(apb_base, index, low);
	notific_write_nq_f_size(apb_base, index, size);
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
static void mpset_set_dram_and_mpset_info_v3(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	mpset->num_channels = V3_MAX_DRAM_CHANNELS;
	mpset->mp_device_num_regions = 1;
	device_dram_addr[0] = V3_HBM_0_BASE;
	device_dram_addr[1] = V3_HBM_1_BASE;
	device_dram_addr[2] = V3_HBM_2_BASE;
	device_dram_addr[3] = V3_HBM_3_BASE;

	if (narch_is_qemu()) {
		// Allow qemu setups to dynamically allocate their HBM sizes
		const u64 msize = ndhal->ndhal_pci.dram_bar_size / 4;
		device_dram_size[0] = msize;
		device_dram_size[1] = msize;
		device_dram_size[2] = msize;
		device_dram_size[3] = msize;

		u32 mem_regions = sizeof(dm_mmap_special_v3) / sizeof(dm_mmap_special_v3[0]);
		int i = 0;
		for (; i < mem_regions; ++i) {
			if ((dm_mmap_special_v3[i].offset == V3_HBM_0_BASE) ||
				(dm_mmap_special_v3[i].offset == V3_HBM_1_BASE) ||
				(dm_mmap_special_v3[i].offset == V3_HBM_2_BASE) ||
				(dm_mmap_special_v3[i].offset == V3_HBM_3_BASE)) {
				dm_mmap_special_v3[i].size = msize;
			}
		}
		pr_info("overriding hbm size to %llu bytes", msize);
	} else {
		device_dram_size[0] = V3_HBM_ACTIVE_SIZE;
		device_dram_size[1] = V3_HBM_ACTIVE_SIZE;
		device_dram_size[2] = V3_HBM_ACTIVE_SIZE;
		device_dram_size[3] = V3_HBM_ACTIVE_SIZE;
	}
	int i;
	for (i = 0; i < mpset->num_channels; i++) {
		ndhal->ndhal_mpset.device_dram_end_addr[i] = device_dram_addr[i] + device_dram_size[i];
	}
}

// Upper 16MB is used internally by the firmware, don't use it in the allocation pool
#define MEMPOOL_CARVEOUT_SIZE 0x1000000 // 16MB
/**
 * mpset_block_carveout_regions()
 *          - in v3, block carve out regions: Upper 16 MB is used internally by firmware
 *
 * @param nd: neuron device
 * @param mpset: pointer to mpset
 * @param device_dram_addr: DRAM Channel 0's and 1's addresses
 * @param device_dram_size: DRAM Channel 0's and 1's sizes
 * @param region_sz: region size
 * @return int: 0 on success, o/w on failure
 */
static int mpset_block_carveout_regions_v3(struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	int ret;
	u64 region_sz;
	int channel = 0, region = 0;

	/*
	*  Block carve out regions: Upper 16 MB is used internally by firmware for trainuim2
	*
	*  Ideally we would carve out by simply changing the start address of the chunk;
	*  however, that breaks aligned allocation in 4.x kernel versions (fixed in 5.x).
	*  Fix here:
	*     commit 52fbf1134d479234d7e64ba9dcbaea23405f229e
	*     Author: Alexey Skidanov <alexey.skidanov@intel.com>
	*     Date:   Thu Jan 3 15:26:44 2019 -0800
	*
	*     lib/genalloc.c: fix allocation of aligned buffer from non-aligned chunk
	*/
	for (channel = 0; channel < mpset->num_channels; channel++) {
		region_sz = device_dram_size[channel] / mpset->mp_device_num_regions;
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			const dma_addr_t start_addr = device_dram_addr[channel] + (region * region_sz);
			struct mem_chunk *mc = NULL;
			u32 nc_id = channel;
			ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, MEMPOOL_CARVEOUT_SIZE, 0, MEM_LOC_DEVICE, channel, region, nc_id, NEURON_MEMALLOC_TYPE_NCDEV_DEVICE, &mc);
			if (ret) {
				pr_err("failed to allocate hbm carveout region: ret=%d\n", ret);
				return -ENOMEM;
			}
			if (mc->pa != start_addr) {
				pr_err("carve out mc not offset 0!");
				mc_free(&mc);
				return -EINVAL;
			}
		}
		ndhal->ndhal_mpset.device_dram_effective_base_addr[channel] = device_dram_addr[channel] + MEMPOOL_CARVEOUT_SIZE;
	}

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
static uint32_t ndmar_get_h2t_eng_id_v3(struct neuron_device *nd, uint32_t nc_id)
{
	const uint32_t h2d_dma_eng_id[V3_NUM_H2D_DMA_PER_DEVICE] = {
		V3_D2H_0_IDX,
		V3_H2D_0_IDX,
		V3_D2H_1_IDX,
		V3_H2D_1_IDX
		};
	const uint32_t seng_id = nc_id / V3_NC_PER_SENG;
	return h2d_dma_eng_id[seng_id];
}

/**
 * ndmar_get_h2t_qid()  - return the H2T engine's queue id for this core
 *
 * @param nc_id: Neuron core corresponding to H2T engine
 * Return DMA queue id
 */
static int ndmar_get_h2t_qid_v3(uint32_t nc_id)
{
 	// on V3 on h2t engines are shared between 2 cores so we give the even core queue 0 and the odd core queue 1
	return nc_id % V3_NC_PER_SENG;
}

/**
 * ndmar_is_h2t_q() - return true
 *
 * @param nd: Neuron device which contains the DMA engine
 * @param eng_id: engine id
 * @param q_id:  queue id
 * Return true if this is an h2t queue
 */
static bool ndmar_is_h2t_q_v3(struct neuron_device *nd, uint32_t eng_id, uint32_t q_id)
{
	return (nd->ndma_engine[eng_id].used_for_h2t && ((q_id == 0) || (q_id == 1)));
}

/**
 * nr_init_h2t_eng() - return true if the h2t dma should be initialized
 *     h2t dma is shared by adjacent cores so only have the even nc init the engine
 *
 * @param nd_idx - index of the core that owns the h2t
 * @param nc_map - map of all cores being reset
 */
static bool nr_init_h2t_eng_v3( int nc_idx, uint32_t nc_map)
{
	// h2t initialization should be owned by the first tpb per seng
	if (nc_idx % V3_NC_PER_SENG != 0) {
		return false;
	}
	// h2t initialization should always be done after device reset
	if (nc_map == NEURON_NC_MAP_DEVICE) {
		return true;
	}

	// when resetting a subset of the device, (re)initialize the h2t engines when all cores in an
	// seng are reset
	const uint32_t seng_id = nc_idx / V3_NC_PER_SENG;
	const uint32_t seng_map = ((1 << V3_NC_PER_SENG) - 1) << (V3_NC_PER_SENG * seng_id);
	return ((nc_map & seng_map) == seng_map);
}

/**
 * ndmar_is_nx_ring() - is the DMA ring reserved for NX cores
 *
 * @param eng_id: the DMA engine id
 * @param q_id: the DMA queue id
 */
static bool ndmar_is_nx_ring_v3(uint32_t eng_id, uint32_t q_id)
{
	// for v3 the last queue is reserved for collectives, and the second
	// to last queue in engs 0-4 and 7-8 are reserved for NX cores
	return (q_id == (V3_DMA_QUEUE_PER_ENG - 2)) &&
		(((eng_id % V3_DMA_ENG_PER_NC) < V3_TPB_ENG_PER_NC) ||
		((eng_id % V3_DMA_ENG_PER_NC) == 7 ||
		(eng_id % V3_DMA_ENG_PER_NC) == 8 ));
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
static int ndmar_quiesce_queues_v3(struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask)
{
	if (engine_count > DMA_QUIESCE_MAX_ENG)
		return -EINVAL;

	u32 start_eng = nc_id * V3_DMA_ENG_PER_NC;
	u32 eng_id;
	for (eng_id = 0; eng_id < V3_DMA_ENG_PER_NC; eng_id++) {
		int qid;
		struct ndma_eng *eng  = ndmar_acquire_engine(nd, start_eng + eng_id);
		if (eng == NULL)
			return -EINVAL;
		for (qid = 0; qid < V3_MAX_DMA_RINGS; qid++) {
			u32 mask = 0x1 << qid;
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
static void ndmar_set_model_started_v3(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc)
{
	return;
}


/* FWIO Functions */


/**
 * neighbor id mapping
 *    column is indexed by seng
 *    row is indexed by device id
 *    neighbor is reported from table by device id.
 *    e.g. device 0, seng 0 neighbor is device 12
 */
const int trn2_32xl_neigbor_ids[16][4] = {
	{12, 1, 3, 4},   // neuron device 0
	{0, 13, 5, 2},   // neuron device 1
	{14, 3, 1, 6},   // neuron device 2
	{2, 15, 7, 0},   // neuron device 3
	{8, 5, 7, 0},    // neuron device 4
	{4, 9, 1, 6},    // neuron device 5
	{10, 7, 5, 2},   // neuron device 6
	{6, 11, 3, 4},   // neuron device 7
	{4, 9, 11, 12},  // neuron device 8
	{8, 5, 13, 10},  // neuron device 9
	{6, 11, 9, 14},  // neuron device 10
	{10, 7, 15, 8},  // neuron device 11
	{0, 13, 15, 8},  // neuron device 12
	{12, 1, 9, 14},  // neuron device 13
	{2, 15, 13, 10}, // neuron device 14
	{14, 3, 11, 12}  // neuron device 15
};

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
static int fw_io_topology_v3(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count)
{
	// V3 does not have Pacific support to detect east/west/south/north neighbors like V1,
	// so its topology is hardcoded based on instance type.
	*count = 0;

	if (total_neuron_devices == 0)
		return 0;

	switch (pdev_index) {
		case TRN2_DEVICE_ID0: // Trn2
			if (total_neuron_devices % 16 == 0) {
				int i;
				*count = 4;
				memcpy(connected_device_ids, trn2_32xl_neigbor_ids[device_id % 16], (*count) * sizeof(int));
				for ( i=0; i < *count; i++) {
					connected_device_ids[i] |= (0x10 & device_id);
				}
			}
			break;
		default:
			break;
	}
	return 0;
}

/**
 * fw_io_register_readless_read_region() - Register readless read BAR regions
 *
 * @param ctx: FWIO context
 *
 * @return int: 0 on success. -1 on failure
 */
static int fw_io_register_readless_read_region_v3(struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size)
{
	// NOTE: V3 bar0->device physical mapping is non contiguous so restrict the RR window.
	//
	if (fw_io_register_read_region(ctx, bar0, V3_PCIE_BAR0_APB_IO_0_SIZE, V3_APB_IO_0_BASE)) {
		pr_err("failed to register readless read BAR0 region\n");
		return -1;
	}
	return 0;
}

/**
 * fw_io_read_csr_array() - Read the CSR array
 *
 * @param addrs: array of CSR addresses to be read
 * @param values: output array
 * @param num_csrs: number of CSR addresses
 * @param operational: true if the read expects the device to be in operational state;
 *                     it's used to distinguish between when the driver first discovers the device (possibly unknown state) and when it's successfully been reset "operational"
 *
 * @return int: 0 on success, -1 on failure
 */
static int fw_io_read_csr_array_v3(void **ptrs, u32 *values, u32 num_csrs, bool operational)
{
	if (num_csrs > FW_IO_MAX_READLESS_READ_REGISTER_COUNT)
		return -EINVAL;

	return fw_io_read_csr_array_direct(ptrs, values, num_csrs, operational);
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
inline int reg_read32_array_v3(void **addr, u32 *value, u32 num_values)
{
	int ret;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(addr, value, num_values, true);
	if (ret != 0) {
		pr_err("register read failure while reading %p\n", addr[0]);
		dump_stack();
	}
	return ret;
}

inline int reg_read32_array_v3_qemu_emu(void **addr, u32 *value, u32 num_values)
{
	int i;
	for (i = 0; i < num_values; i++) {
		value[i] = readl(addr[i]);
	}
	return 0;
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
static int mmap_get_bar4_offset_v3(u64 start_addr, u64 size, u64 *offset)
{
	u64 hbm_dist = narch_is_qemu() ? (ndhal->ndhal_pci.dram_bar_size / 4) : V3_HBM_SIZE;

	if (start_addr >= V3_HBM_0_BASE && start_addr + size < V3_HBM_0_BASE + V3_HBM_ACTIVE_SIZE)
		*offset = start_addr;
	else if (start_addr >= V3_HBM_1_BASE && start_addr + size < V3_HBM_1_BASE + V3_HBM_ACTIVE_SIZE)
		*offset = start_addr - V3_HBM_1_BASE + hbm_dist;
	else if (start_addr >= V3_HBM_2_BASE && start_addr + size < V3_HBM_2_BASE + V3_HBM_ACTIVE_SIZE)
		*offset = start_addr - V3_HBM_2_BASE + hbm_dist * 2;
	else if (start_addr >= V3_HBM_3_BASE && start_addr + size < V3_HBM_3_BASE + V3_HBM_ACTIVE_SIZE)
		*offset = start_addr - V3_HBM_3_BASE + hbm_dist * 3;
	else
		return -EINVAL;
	return 0;
}


/* Sysfs Metrics Functions */
// sysfs root node's attrs and its child nodes' attrs
static nsysfsmetric_attr_info_t root_info_node_attrs_info_tbl_v3[] = {
    ATTR_INFO("notify_delay", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY), OTHER),
    ATTR_INFO("serial_number", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_SERIAL_NUMBER), OTHER),
};
static int root_info_node_attrs_info_tbl_cnt_v3 = sizeof(root_info_node_attrs_info_tbl_v3) / sizeof(nsysfsmetric_attr_info_t);

/**
 * nsysfsmetric_add_ecc_nodes() - add neuron{0, 1, ...}/stats/hardware/{sram_ecc_uncorrected, mem_ecc_uncorrected} to sysfs directory
 *
 * @param metrics: the sysfs metrics structure
 * @param stats_node: the sysfs node structure of the stats directory
 * @param ecc_attrs_info_tbl_cnt: number of the ecc attributes
 * @param attr_info_tbl: the ecc attributes as an array
 * @return int 0 on success; otherwise on failure
 *
 * Note: ecc errors are only supported by sysfs for V2/3. TODO: V1 support will be added
 */
static int nsysfsmetric_add_ecc_nodes_v3(struct nsysfsmetric_metrics *metrics,
                                  struct nsysfsmetric_node *stats_node,
                                  int ecc_attrs_info_tbl_cnt,
                                  const nsysfsmetric_attr_info_t *ecc_attrs_info_tbl)
{
	struct nsysfsmetric_node *hardware_node = nsysfsmetric_init_and_add_one_node(metrics, stats_node, "hardware", false, -1, ecc_attrs_info_tbl_cnt, ecc_attrs_info_tbl);
	if (!hardware_node) {
		pr_err("failed to add hardware node its attributes under stats\n");
		return -1;
	}

	return 0;
}

/**
 * nsysfsmetric_get_hbm_error_count_v3() - check hbm repairable/unrepairable error count
 *
 * @param nd - neuron device
 * @param repairable - indicates checking for repairable/unrepairable error counts
 * @param err_count - error count returns
 */
static void nsysfsmetric_get_hbm_error_count_v3(struct neuron_device *nd,
                                                 bool repairable,
                                                 uint32_t *err_count)
{
	int ret;
	uint32_t total_uncorrected_ecc_err_count;
	uint32_t ecc_repair_state;

	*err_count = 0;

	ret = fw_io_hbm_uecc_repair_state_read(nd->npdev.bar0, &ecc_repair_state);
	if (ret) {
		pr_err("sysfs failed to read HBM ECC repair state from FWIO\n");
		return;
	}
	total_uncorrected_ecc_err_count = fw_io_get_total_uecc_err_count(nd->npdev.bar0);

	/*
	*  HBM Repair State Bitfield notes:
	*      2 bits to represent the state of hbm repair
	*      0x0 means no pending repair
	*      0x1 means pending repair
	*      0x2 means repair failure
	*/
	if (total_uncorrected_ecc_err_count == 0 && ecc_repair_state != 0) {
		// For legacy Pacific firmware, there might be the case that (err count > 0 && repair state == 0), so allow this case
		// When err count = 0, repair state must be 0x0
		pr_warn_once("[ND %d] Total Uncorrected ecc err count is %d, but repair state is %d which is invalid. Please contact Neuron for support.\n", nd->device_index, total_uncorrected_ecc_err_count, ecc_repair_state);
		return;
	}
	if (repairable) {
		*err_count = (ecc_repair_state == 0x1) ? total_uncorrected_ecc_err_count : 0;
	} else {
		*err_count = (ecc_repair_state == 0x2) ? total_uncorrected_ecc_err_count : 0;
		if (ecc_repair_state == 0x0) {
			// legacy FW hack - TODO remove at some point
			*err_count = total_uncorrected_ecc_err_count;
		}
	}
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
static int nsysfsmetric_add_tensor_engine_node_v3(struct nsysfsmetric_metrics *metrics,
				struct nsysfsmetric_node *stats_node,
				int nc_id,
				int tensor_engine_attrs_info_tbl_cnt,
				const nsysfsmetric_attr_info_t *tensor_engine_attrs_info_tbl)
{
	struct nsysfsmetric_node *tensor_engine_node = nsysfsmetric_init_and_add_one_node(metrics, stats_node, "tensor_engine", false, nc_id, tensor_engine_attrs_info_tbl_cnt, tensor_engine_attrs_info_tbl);
	if (!tensor_engine_node) {
		pr_err("failed to add the tensor_engine node under stats for nc%d\n", nc_id);
		return -1;
	}
	return 0;
}


/* PCI Functions */
/**
 * neuron_pci_release_bar() - Release a PCI BAR
 *
 * @param dev: PCI device whose resources were previously reserved by pci_request_region()
 * @param bar: BAR to be reserved
 *
 * for V3, this function is dummy
 */
static int neuron_pci_release_bar_v3(struct pci_dev *dev, int bar)
{
	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}
	if (bar == BAR_UNUSED) {
		return 0;
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
static int neuron_pci_reserve_bar_v3(struct pci_dev *dev, int bar, const char *res_name)
{
	int ret;

	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		goto err;
	}
	if (bar == BAR_UNUSED) {
		return 0;
	}

	ret = pci_request_region(dev, bar, res_name);
	if (ret) {
		pci_info(dev, "BAR %d: can't reserve %s\n", bar, res_name);
		goto err;
	}

	return 0;

err:
	//return -ENODEV;  Until we can map BAR4 on cmdk
	return (bar == 4)? 0:-ENODEV;

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
static int neuron_pci_set_npdev_v3(struct pci_dev *dev,
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
	if (bar == BAR_UNUSED) {
		return 0;
	}

	if (pci_resource_len(dev, bar) == 0) {
		pci_info(dev, "BAR%d len is 0\n", bar);
		goto err;
	}

	*bar_pa = pci_resource_start(dev, bar);
	if (!(*bar_pa)) {
		pci_info(dev, "Can't get start address of BAR%d %s\n", bar, res_name);
		goto err;
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

err:
	//return -ENODEV;  Until we can map BAR4 on cmdk
	*bar_pa = 0;
	*bar_size = 0;
	*bar_ioaddr = NULL;
	return 0;
}

extern int dup_helper_enable;
static atomic_t dup_rid_cnt = ATOMIC_INIT(0); // count of duplicate routing IDs encountered
static int neuron_pci_handle_dup_routing_id(void)
{
	int  ret = -ENODEV;
	int  dup_cnt;
	char cmd[256];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	dup_cnt = atomic_fetch_add(1, &dup_rid_cnt);
#else
	dup_cnt = atomic_add_return(1, &dup_rid_cnt) - 1;
#endif

	// If this is the first dup encounted, unload the driver
	if ((dup_cnt == 0) && dup_helper_enable) {
		pr_err("scheduling unload of %s due to duplicate routing id\n", module_name(THIS_MODULE));

		int n = snprintf(cmd, sizeof(cmd), "sleep 10;/sbin/modprobe -r %s", module_name(THIS_MODULE));
		if (n > sizeof(cmd)) {
			pr_err("unable to schedule driver unload cmd buffer len exceeded\n");
			return -EINVAL;
		}
		char *argv[] = 		  { "/bin/sh",
								"-c",
								cmd,
								NULL};
		static char *envp[] = { "HOME=/",
								"TERM=linux",
								"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
								NULL};

		ret = call_usermodehelper( argv[0], argv, envp, UMH_WAIT_EXEC);
		if (ret)
			pr_err("unable to schedule driver unload. Error: %d\n", ret);
	}

	return ret;
}

// for V3 rename Neuron devices for better customer experience.
// see internal documentation: TRN2-Discovery
// map routing id to user id:
static const u32 v3_routing_id_to_user_id[] = {
	0,	3,	4,	7,
	12,	15,	8,	11,
	1,	2,	5,	6,
	13,	14,	9,	10 };

#define V3_ROUTING_ID_TBL_SZ  (sizeof(v3_routing_id_to_user_id) / sizeof(v3_routing_id_to_user_id[0]))

static u32 neuron_pci_routing_id_to_user_id(u32 routing_id)
{
	u32 user_id_base = v3_routing_id_to_user_id[ routing_id % V3_ROUTING_ID_TBL_SZ];
	return user_id_base + (routing_id / V3_ROUTING_ID_TBL_SZ) * V3_ROUTING_ID_TBL_SZ;
}

/**
 * neuron_pci_get_device_id() - get device id from pacific and set nd->device_index
 *
 * @param dev: PCI device
 * @param nd: neuron device
 * @return int: 0 on success, otherwise on failure
 *
 * for V1, this function is dummy
 */
static int neuron_pci_get_device_id_v3(struct neuron_device *nd, struct pci_dev *dev)
{
	int ret = 0;
	int i;
	u32 routing_id = (u32)-1;

	// Poll the device id until the device is ready
	for (i = 0; i < 20; i++) {
		ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
		if (!ret && routing_id != 0xdeadbeef) {
			break;
		}
		msleep(1000);
	}

	if (ret) {
		pr_err("Could not retrieve device index (read timeout)");
		return -ENODEV;
	}

	// TODO - this should be a "valid routing_id check for TRN2
	if (routing_id < 0 || routing_id >= MAX_NEURON_DEVICE_COUNT) {
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

/**
 * neuron_pci_device_id_to_rid_map() - return device id to routing id map.  Used by nccl for topology discovery
 *
 * @param count: total number of neuron devices on the host instance
 * @param did_to_rid_map: map of device ids to routing ids
 * @return int: Returns 0 on success, otherwise failure
 */
static int
neuron_pci_device_id_to_rid_map_v3(uint32_t * count, uint32_t * did_to_rid_map)
{
	int i;

	switch (ndhal->pci_device_id) {
		case TRN2_DEVICE_ID0:
			for (i = 0; i < total_neuron_devices; i++) {
				did_to_rid_map[neuron_pci_routing_id_to_user_id(i)] = i;
			}
			break;

		default:
			for (i = 0; i < total_neuron_devices; i++) {
				did_to_rid_map[i] = i;
			}
	}

	*count = total_neuron_devices;
	return 0;
}


/* Char Device (cdev) Functions */
/*
 * IMPORTANT:
 *           - These variables track the range of "compatible" versions of the RT
 *             i.e. the range of RT versions that is compatible with this version of the driver.
 *           - This value is independent from the "release" version, because the
 *             "release" number is controlled by PM, marketing, etc. considerations.
 *
 *           - MAX should be incremented when the driver API/behavior
 *             changes in a way that is meaningful to the RT.  In that case
 *             both the MAX here and the version expected by the RT should be
 *             incremented to prevent the new RT from starting on an old driver.
 *           - MIN should be incremented when we make changes in the driver
 *             that are not compatible with old RT.  When MIN is incremented,
 *             it will prevent old RT from starting up.
 *
 *           - Version 3 of runtime requires 1) aligned memory allocation support  2) SPROT.
 *           - Version 4 of the runtime requires support for DMA queue init w/o already allocated rings (2.7).
 *           - Version 5 of the runtime requires V2 device renumbering (don't care for V1).
 *           - Version 6 of the runtime requires ham notification support,
 *              + new V2 reset api for single-tpb reset + new notification init API with force mem realloc/resize.
 *           - Version 7 of the runtime requires udma queue size support for non power of 2 rings + dmabuf support.
 *           - Version 8 of the runtime needs Neuron Core remapping support.
 *           - Version 9 of the runtime requres seng swap support and dieflip.
 *           - Version 10 of the runtime requres split miscram region for model stop data
 *           - Version 11 - fix for hwdge caused DMA abort
 */
#define V3_RT_MIN_COMPATIBLE_VERSION 10
#define V3_RT_MAX_COMPATIBLE_VERSION 11
/**
 * ncdev_compatible_version() - fill in the compatible version of the RT with the current driver version
 *
 * @param arg: min and max compatible versions to be filled in
 */
static void ncdev_compatible_version_v3(struct neuron_ioctl_compatible_version *arg)
{
	arg->min = V3_RT_MIN_COMPATIBLE_VERSION;
	arg->max = V3_RT_MAX_COMPATIBLE_VERSION;
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
static void ncdev_quiesce_exec_on_proc_exit_v3(void)
{
	// for V3, the 1 second DMA queisce delay in flush was eliminated to improve nrt_init performance
	return;
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
 * V2/3:
 *    Only BAR0 is used right now. TODO: change runtime ioctl
*/
static int ncdev_bar_write_data_v3(struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count)
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
	} else if (bar == 4) {
		// TODO: we don't have any use case for r/w memory over the BAR right now.  Disabling.
		//
		// We'd like to use DMA for r/w of BAR4 because we might expect access to large amounts of data.
		// Access via DMA requires an application to own a TPB because it determines which of the h2t DMAs
		// are safe to use, otherwise a TPB along with its DMA could be reset while that DMA is used here.
		// Don't want/need to solve it now.
		return -EINVAL;

		/*
		dma_addr_t dst_addr = reg_addresses[0] - (u64)nd->npdev.bar0;

		ret = ndma_memcpy(nd, 0, virt_to_phys(data) | ndhal->ndhal_address_map.pci_host_base, dst_addr, data_size);
		if (ret)
			return ret;
		*/
	} else {
		pr_err("direct BAR%d write is not supported.\n", bar);
		return -EINVAL;
	}

	return 0;
}

static void ncdev_get_default_tpbs_for_hbm_v3(u32 hbm_index, u32 tpbs[MAX_NC_PER_DEVICE], u32 *tpb_count)
{
	tpbs[0] = hbm_index * 2;
	tpbs[1] = (hbm_index * 2) + 1;
	*tpb_count = 2;
}

/* UDMA Functions */
#define UDMA_AXI_M2S_DATA_RD_CFG_ALWAYS_BREAK_ON_MAX_BOUDRY (1 << 16)
/**
 * udma_m2s_data_rd_cfg_boundaries_set(): set data_rd_cfg to break at 256B boundaries
 *
 * @param udma: the UDMA structure
 *
 * for V1, this function is dummy
 */
static void udma_m2s_data_rd_cfg_boundaries_set_v3(struct udma *udma)
{
	reg_write32(&udma->udma_regs_m2s->axi_m2s.data_rd_cfg,
	  UDMA_AXI_M2S_DATA_RD_CFG_ALWAYS_BREAK_ON_MAX_BOUDRY | 0x8);
}

#define UDMA_M2S_Q_RATE_LIMIT_MASK_INTERNAL_PAUSE_DMB (1 << 2)
/**
 * udma_q_config() - set misc queue configurations
 *
 * @param udma_q udma_q: the queue data structure
 *
 * for V1, this function is dummy
 */
static void udma_q_config_v3(struct udma_q *udma_q)
{
	if (udma_q->type != UDMA_TX) {
		return;
	}

	uint32_t *reg_addr = &udma_q->q_regs->m2s_q.rlimit.mask;
	uint32_t val = udma_q->rlimit_mask;

	// enable DMB
	val &= ~UDMA_M2S_Q_RATE_LIMIT_MASK_INTERNAL_PAUSE_DMB;
	reg_write32(reg_addr, val);
}


/* NDMA Functions */
/**
 * ndma_get_wait_for_completion_time() - calculate the first and the following wait times for a DMA tranfer completion
 *
 *      One full descriptor takes ~2 usec to transfer (64K at 32G/sec) on V3  and ~4 usec to transfer on V2.
 *      The last descriptor may be partial, so wait 1/4 64K transfer time for that descriptor.
 *      Also, count includes the completion descriptor so don't include that in the count.
 *
 * @param first_wait_time: the wait time for the first sleep
 * @param wait_time: the wait time for the following sleeps
 */
static void ndma_get_wait_for_completion_time_v3(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	u64 est_wait_time = 2 * (count -1);
	*first_wait_time = async ? 1 : (est_wait_time - 1);  // FIXME - need to adjust for zerocopy
	*following_wait_time = (est_wait_time * 100) - *first_wait_time;

	// for some reason getting a timeout when staging some of BERT training graphs.
	// https://tiny.amazon.com/8jw7wl18
	// In the meantime make the timeout 100x the original
	*following_wait_time *= 100;
}

static void ndma_get_wait_for_completion_time_v3_qemu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v3(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 10 * 1000;
}

static void ndma_get_wait_for_completion_time_v3_emu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v3(count, async, first_wait_time, following_wait_time);
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
 *      V3:
 *         similar idea.  Just check for valid address allocated in host memory
 *
 * @param nd: the neuron device
 * @param pa: the desc physical addresses
 * @param dst_mc: the mc that backs the dma queue
 * @return int: return 0 if the pa is valid; otherwise return negative
 */
static int ndma_validate_pa_v3(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type)
{
	if ((pa & V3_PCIE_A0_BASE) == ndhal->ndhal_address_map.pci_host_base) {
		if (!ndma_is_valid_host_mem(nd, pa)) {
			return -EINVAL;
		}
	}
	return 0;
}

/**
 * ndma_init() - Initialize a DMA engine
 *
 * @param bar0: BAR0
 * @param udma: UDMA structure
 * @param eng_id: DMA engine index to initialize
 * @return int: 0 on success, otherwise on failure
 */
static int ndma_init_v3(void __iomem *bar0, struct udma *udma, int eng_id)
{
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	const bool d2h_0 = eng_id == V3_D2H_0_IDX;
	const bool h2d_0 = eng_id == V3_H2D_0_IDX;
	const bool d2h_1 = eng_id == V3_D2H_1_IDX;
	const bool h2d_1 = eng_id == V3_H2D_1_IDX;

	uint64_t seng_udma_relbase;
	uint64_t se_user_fis_relbase = 0;
	if (h2d_0 || d2h_0) {
		seng_udma_relbase = (h2d_0 ? V3_APB_IO_0_H2D_UDMA_BASE : V3_APB_IO_0_D2H_UDMA_BASE);
		seng_udma_relbase = seng_udma_relbase - V3_APB_IO_0_BASE + V3_PCIE_BAR0_APB_IO_0_OFFSET;
	} else if (h2d_1 || d2h_1) {
		seng_udma_relbase = (h2d_1 ? V3_APB_IO_1_H2D_UDMA_BASE : V3_APB_IO_1_D2H_UDMA_BASE);
		seng_udma_relbase = seng_udma_relbase - V3_APB_IO_1_BASE + V3_PCIE_BAR0_APB_IO_1_OFFSET;
	} else {
		int seng_id = eng_id / V3_NUM_DMA_ENG_PER_SENG;
		int eng_id_within_seng = eng_id % V3_NUM_DMA_ENG_PER_SENG;
		if (seng_id == 0) {
			seng_udma_relbase = V3_APB_SE_0_SDMA_0_BASE + eng_id_within_seng * V3_APB_SDMA_DIST;
			seng_udma_relbase = seng_udma_relbase - V3_APB_SE_0_BASE + V3_PCIE_BAR0_APB_SE_0_OFFSET;
			se_user_fis_relbase = V3_PCIE_BAR0_APB_SE_0_OFFSET +
				V3_APB_SE_0_USER_FIS_SDMA_0_OFFSET +
				V3_APB_SE_USER_FIS_SDMA_0_FIS_0_USER_ERRTRIG_OFFSET +
				(eng_id_within_seng * V3_APB_SE_USER_FIS_SDMA_0_SIZE);
		} else if (seng_id == 1) {
			seng_udma_relbase = V3_APB_SE_1_SDMA_0_BASE + eng_id_within_seng * V3_APB_SDMA_DIST;
			seng_udma_relbase = seng_udma_relbase - V3_APB_SE_1_BASE + V3_PCIE_BAR0_APB_SE_1_OFFSET;
			se_user_fis_relbase = V3_PCIE_BAR0_APB_SE_1_OFFSET +
				V3_APB_SE_1_USER_FIS_SDMA_0_OFFSET +
				V3_APB_SE_USER_FIS_SDMA_0_FIS_0_USER_ERRTRIG_OFFSET +
				(eng_id_within_seng * V3_APB_SE_USER_FIS_SDMA_0_SIZE);
		} else if (seng_id == 2) {
			seng_udma_relbase = V3_APB_SE_2_SDMA_0_BASE + eng_id_within_seng * V3_APB_SDMA_DIST;
			seng_udma_relbase = seng_udma_relbase - V3_APB_SE_2_BASE + V3_PCIE_BAR0_APB_SE_2_OFFSET;
			se_user_fis_relbase = V3_PCIE_BAR0_APB_SE_2_OFFSET +
				V3_APB_SE_2_USER_FIS_SDMA_0_OFFSET +
				V3_APB_SE_USER_FIS_SDMA_0_FIS_0_USER_ERRTRIG_OFFSET +
				(eng_id_within_seng * V3_APB_SE_USER_FIS_SDMA_0_SIZE);
		} else {
			seng_udma_relbase = V3_APB_SE_3_SDMA_0_BASE + eng_id_within_seng * V3_APB_SDMA_DIST;
			seng_udma_relbase = seng_udma_relbase - V3_APB_SE_3_BASE + V3_PCIE_BAR0_APB_SE_3_OFFSET;
			se_user_fis_relbase = V3_PCIE_BAR0_APB_SE_3_OFFSET +
				V3_APB_SE_3_USER_FIS_SDMA_0_OFFSET +
				V3_APB_SE_USER_FIS_SDMA_0_FIS_0_USER_ERRTRIG_OFFSET +
				(eng_id_within_seng * V3_APB_SE_USER_FIS_SDMA_0_SIZE);
		}
	}

 	void __iomem *udma_base = (void __iomem *)bar0 + seng_udma_relbase;
	void __iomem *sdma_base = udma_base + V3_APB_SDMA_MISC_OFFSET;

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(udma, udma_base, DMA_MAX_Q_MAX, udma_name, 0,
							   V3_ALLOWED_DESC_PER_PACKET + 1, false); // we add one to allow for MD descriptor
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = sdma_init_engine(sdma_base);
	if (ret) {
		pr_err("SDMA ENG:%d init failed\n", eng_id);
		goto done;
	}

	if (eng_id < V3_NUM_SENG_DMA_PER_DEVICE) {
		void __iomem *user_fis_base = (void __iomem *)bar0 + se_user_fis_relbase;
		udma_m2m_mask_ring_id_error(udma, user_fis_base);
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
static int ndma_is_bar0_write_blocked_v3(u64 off)
{
	int eid;
	// check seng 0
	u64 start_off = V3_APB_SE_0_SDMA_0_BASE - V3_APB_SE_0_BASE + V3_PCIE_BAR0_APB_SE_0_OFFSET;
	u64 end_off = start_off + V3_NUM_DMA_ENG_PER_SENG * V3_APB_SDMA_DIST;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V3_NUM_DMA_ENG_PER_SENG; eid++) {
			if (ndma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V3_APB_SDMA_DIST;
		}
	}
	// check seng 1
	start_off = V3_APB_SE_1_SDMA_0_BASE - V3_APB_SE_1_BASE + V3_PCIE_BAR0_APB_SE_1_OFFSET;
	end_off = start_off + V3_NUM_DMA_ENG_PER_SENG * V3_APB_SDMA_DIST;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V3_NUM_DMA_ENG_PER_SENG; eid++) {
			if (ndma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V3_APB_SDMA_DIST;
		}
	}
	// check seng 2
	start_off = V3_APB_SE_2_SDMA_0_BASE - V3_APB_SE_2_BASE + V3_PCIE_BAR0_APB_SE_2_OFFSET;
	end_off = start_off + V3_NUM_DMA_ENG_PER_SENG * V3_APB_SDMA_DIST;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V3_NUM_DMA_ENG_PER_SENG; eid++) {
			if (ndma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V3_APB_SDMA_DIST;
		}
	}
	// check seng 3
	start_off = V3_APB_SE_3_SDMA_0_BASE - V3_APB_SE_3_BASE + V3_PCIE_BAR0_APB_SE_3_OFFSET;
	end_off = start_off + V3_NUM_DMA_ENG_PER_SENG * V3_APB_SDMA_DIST;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V3_NUM_DMA_ENG_PER_SENG; eid++) {
			if (ndma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V3_APB_SDMA_DIST;
		}
	}
	// check D2H
	start_off = V3_APB_IO_0_D2H_UDMA_BASE - V3_APB_IO_0_BASE + V3_PCIE_BAR0_APB_IO_0_OFFSET;
	end_off = start_off + V3_APB_SDMA_DIST;
	if (ndma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}
	start_off = V3_APB_IO_1_D2H_UDMA_BASE - V3_APB_IO_1_BASE + V3_PCIE_BAR0_APB_IO_1_OFFSET;
	end_off = start_off + V3_APB_SDMA_DIST;
	if (ndma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}
	// check H2D
	start_off = V3_APB_IO_0_H2D_UDMA_BASE - V3_APB_IO_0_BASE + V3_PCIE_BAR0_APB_IO_0_OFFSET;
	end_off = start_off + V3_APB_SDMA_DIST;
	if (ndma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}
	start_off = V3_APB_IO_1_H2D_UDMA_BASE - V3_APB_IO_1_BASE + V3_PCIE_BAR0_APB_IO_1_OFFSET;
	end_off = start_off + V3_APB_SDMA_DIST;
	if (ndma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
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
static int ndma_get_m2m_barrier_type_v3(bool set_dmb)
{
	if (set_dmb)
		return UDMA_M2M_BARRIER_WRITE_BARRIER;
	else
		return UDMA_M2M_BARRIER_NONE;
}

#define NC_MAPPING_MAX_CORE_COUNT_V3 128
static const struct neuron_ioctl_nc_map_entry nc_mapping_v0_seng_swap[] = {
	{ .device_id = 0,  .device_nc_idx = 4 }, { .device_id = 0,  .device_nc_idx = 5 }, { .device_id = 0,  .device_nc_idx = 6 }, { .device_id = 0,  .device_nc_idx = 7 }, { .device_id = 0,  .device_nc_idx = 2 }, { .device_id = 0,  .device_nc_idx = 3  }, { .device_id = 0,  .device_nc_idx = 0 }, { .device_id = 0,  .device_nc_idx = 1 }, // ND0
	{ .device_id = 1,  .device_nc_idx = 2 }, { .device_id = 1,  .device_nc_idx = 3 }, { .device_id = 1,  .device_nc_idx = 0 }, { .device_id = 1,  .device_nc_idx = 1 }, { .device_id = 1,  .device_nc_idx = 4 }, { .device_id = 1,  .device_nc_idx = 5  }, { .device_id = 1,  .device_nc_idx = 6 }, { .device_id = 1,  .device_nc_idx = 7 }, // ND1
	{ .device_id = 2,  .device_nc_idx = 4 }, { .device_id = 2,  .device_nc_idx = 5 }, { .device_id = 2,  .device_nc_idx = 6 }, { .device_id = 2,  .device_nc_idx = 7 }, { .device_id = 2,  .device_nc_idx = 2 }, { .device_id = 2,  .device_nc_idx = 3  }, { .device_id = 2,  .device_nc_idx = 0 }, { .device_id = 2,  .device_nc_idx = 1 }, // ND2
	{ .device_id = 3,  .device_nc_idx = 2 }, { .device_id = 3,  .device_nc_idx = 3 }, { .device_id = 3,  .device_nc_idx = 0 }, { .device_id = 3,  .device_nc_idx = 1 }, { .device_id = 3,  .device_nc_idx = 4 }, { .device_id = 3,  .device_nc_idx = 5  }, { .device_id = 3,  .device_nc_idx = 6 }, { .device_id = 3,  .device_nc_idx = 7 }, // ND3
	{ .device_id = 4,  .device_nc_idx = 0 }, { .device_id = 4,  .device_nc_idx = 1 }, { .device_id = 4,  .device_nc_idx = 2 }, { .device_id = 4,  .device_nc_idx = 3 }, { .device_id = 4,  .device_nc_idx = 6 }, { .device_id = 4,  .device_nc_idx = 7  }, { .device_id = 4,  .device_nc_idx = 4 }, { .device_id = 4,  .device_nc_idx = 5 }, // ND4
	{ .device_id = 5,  .device_nc_idx = 6 }, { .device_id = 5,  .device_nc_idx = 7 }, { .device_id = 5,  .device_nc_idx = 4 }, { .device_id = 5,  .device_nc_idx = 5 }, { .device_id = 5,  .device_nc_idx = 0 }, { .device_id = 5,  .device_nc_idx = 1  }, { .device_id = 5,  .device_nc_idx = 2 }, { .device_id = 5,  .device_nc_idx = 3 }, // ND5
	{ .device_id = 6,  .device_nc_idx = 0 }, { .device_id = 6,  .device_nc_idx = 1 }, { .device_id = 6,  .device_nc_idx = 2 }, { .device_id = 6,  .device_nc_idx = 3 }, { .device_id = 6,  .device_nc_idx = 6 }, { .device_id = 6,  .device_nc_idx = 7  }, { .device_id = 6,  .device_nc_idx = 4 }, { .device_id = 6,  .device_nc_idx = 5 }, // ND6
	{ .device_id = 7,  .device_nc_idx = 6 }, { .device_id = 7,  .device_nc_idx = 7 }, { .device_id = 7,  .device_nc_idx = 4 }, { .device_id = 7,  .device_nc_idx = 5 }, { .device_id = 7,  .device_nc_idx = 0 }, { .device_id = 7,  .device_nc_idx = 1  }, { .device_id = 7,  .device_nc_idx = 2 }, { .device_id = 7,  .device_nc_idx = 3 }, // ND7
	{ .device_id = 8,  .device_nc_idx = 4 }, { .device_id = 8,  .device_nc_idx = 5 }, { .device_id = 8,  .device_nc_idx = 6 }, { .device_id = 8,  .device_nc_idx = 7 }, { .device_id = 8,  .device_nc_idx = 2 }, { .device_id = 8,  .device_nc_idx = 3  }, { .device_id = 8,  .device_nc_idx = 0 }, { .device_id = 8,  .device_nc_idx = 1 }, // ND8
	{ .device_id = 9,  .device_nc_idx = 2 }, { .device_id = 9,  .device_nc_idx = 3 }, { .device_id = 9,  .device_nc_idx = 0 }, { .device_id = 9,  .device_nc_idx = 1 }, { .device_id = 9,  .device_nc_idx = 4 }, { .device_id = 9,  .device_nc_idx = 5  }, { .device_id = 9,  .device_nc_idx = 6 }, { .device_id = 9,  .device_nc_idx = 7 }, // ND9
	{ .device_id = 10, .device_nc_idx = 4 }, { .device_id = 10, .device_nc_idx = 5 }, { .device_id = 10, .device_nc_idx = 6 }, { .device_id = 10, .device_nc_idx = 7 }, { .device_id = 10, .device_nc_idx = 2 }, { .device_id = 10, .device_nc_idx = 3  }, { .device_id = 10, .device_nc_idx = 0 }, { .device_id = 10, .device_nc_idx = 1 }, // ND10
	{ .device_id = 11, .device_nc_idx = 2 }, { .device_id = 11, .device_nc_idx = 3 }, { .device_id = 11, .device_nc_idx = 0 }, { .device_id = 11, .device_nc_idx = 1 }, { .device_id = 11, .device_nc_idx = 4 }, { .device_id = 11, .device_nc_idx = 5  }, { .device_id = 11, .device_nc_idx = 6 }, { .device_id = 11, .device_nc_idx = 7 }, // ND11
	{ .device_id = 12, .device_nc_idx = 0 }, { .device_id = 12, .device_nc_idx = 1 }, { .device_id = 12, .device_nc_idx = 2 }, { .device_id = 12, .device_nc_idx = 3 }, { .device_id = 12, .device_nc_idx = 6 }, { .device_id = 12, .device_nc_idx = 7  }, { .device_id = 12, .device_nc_idx = 4 }, { .device_id = 12, .device_nc_idx = 5 }, // ND12
	{ .device_id = 13, .device_nc_idx = 6 }, { .device_id = 13, .device_nc_idx = 7 }, { .device_id = 13, .device_nc_idx = 4 }, { .device_id = 13, .device_nc_idx = 5 }, { .device_id = 13, .device_nc_idx = 0 }, { .device_id = 13, .device_nc_idx = 1  }, { .device_id = 13, .device_nc_idx = 2 }, { .device_id = 13, .device_nc_idx = 3 }, // ND13
	{ .device_id = 14, .device_nc_idx = 0 }, { .device_id = 14, .device_nc_idx = 1 }, { .device_id = 14, .device_nc_idx = 2 }, { .device_id = 14, .device_nc_idx = 3 }, { .device_id = 14, .device_nc_idx = 6 }, { .device_id = 14, .device_nc_idx = 7  }, { .device_id = 14, .device_nc_idx = 4 }, { .device_id = 14, .device_nc_idx = 5 }, // ND14
	{ .device_id = 15, .device_nc_idx = 6 }, { .device_id = 15, .device_nc_idx = 7 }, { .device_id = 15, .device_nc_idx = 4 }, { .device_id = 15, .device_nc_idx = 5 }, { .device_id = 15, .device_nc_idx = 0 }, { .device_id = 15, .device_nc_idx = 1  }, { .device_id = 15, .device_nc_idx = 2 }, { .device_id = 15, .device_nc_idx = 3 }, // ND15
};

#define NC_MAPPING_V0_SENG_SWAP_SIZE (sizeof(nc_mapping_v0_seng_swap) / sizeof(nc_mapping_v0_seng_swap[0]))
static_assert((NC_MAPPING_V0_SENG_SWAP_SIZE == NC_MAPPING_MAX_CORE_COUNT_V3) && (NC_MAPPING_V0_SENG_SWAP_SIZE <= NEURON_NC_MAP_MAX_ENTRIES));
static const uint32_t neuron_nc_map_die_flip_mask = 0x6;

static bool ndhal_die_flipped(void)
{
	u32 state;
	s8 node_id;

	if (force_die_flip) {
		return true;
	}
	if (!ndhal_instance_type_pod()) {
		return false;
	}

	npe_get_pod_status(&state, &node_id);
	if ((state == NEURON_POD_E_STATE_ULTRASERVER) && ((node_id == 1) || (node_id == 3))) {
		return true;
	}
	return false;
}

static int ncdev_logical_to_physical_nc_map_v3(struct neuron_ioctl_nc_map *map, uint32_t max_num_entries, enum neuron_ioctl_nc_mapping_type version)
{
	bool apply_dieflip = ndhal_die_flipped();
	uint32_t entry_idx;
	uint32_t entries_to_copy = (max_num_entries < NC_MAPPING_MAX_CORE_COUNT_V3) ? max_num_entries : NC_MAPPING_MAX_CORE_COUNT_V3;
	const struct neuron_ioctl_nc_map_entry *mapping;

	if (version != NEURON_IOCTL_NC_MAPPING_TYPE_V0) {
		pr_err("Unsupported Neuron Core Mapping verion %u for v3 arch", version);
		return -EINVAL;
	}
	mapping = nc_mapping_v0_seng_swap;

	for (entry_idx = 0; entry_idx < entries_to_copy; entry_idx++) {
		uint32_t core_idx = entry_idx;
		if (apply_dieflip) {
            core_idx ^= neuron_nc_map_die_flip_mask;
		}
		WARN_ONCE(core_idx >= NC_MAPPING_MAX_CORE_COUNT_V3, "core_idx %d > max core count %d", core_idx, NC_MAPPING_MAX_CORE_COUNT_V3);
		map->mappings[entry_idx] = mapping[core_idx];
	}
	map->num_entries = entries_to_copy;

	return 0;
}

/**
 * ndma_get_engines_with_host_connectivity - get DMA engines for a particular HBM index which have host connectivity
 * V3 - only particular DMA engines have host connectivity
 */
static void ndma_get_engines_with_host_connectivity_v3(u32 hbm_index, u32 engines[NUM_DMA_ENG_PER_DEVICE], u32 *num_engines)
{
	const int num_dma_engines_per_hbm = 32;
	const int offset = num_dma_engines_per_hbm * hbm_index;
	const int engines_with_connectivity[] = {4, 5, 6, 7,
						12, 13, 14, 15,
						20, 21, 22, 23,
						28, 29, 30, 31};

	int i = 0;
	for (i = 0; i<16; i++) {
		engines[i] = offset + engines_with_connectivity[i];
	}
	*num_engines = 16;
}


/* POD Functions */
/**
 * npe_notify_mark() - api for crwl to notify range marking (core claiming) activities
 *
 * @param mark_cnt - marked core count (for mark, count before, for unmark, count after)
 * @param mark     - true if calling operation was a mark vs unmark
 *
 */
static void npe_notify_mark_v3(int mark_cnt, bool mark)
{
	if (ndhal_instance_type_pod()) {
		npe_notify_mark(mark_cnt, mark);
	}
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
static int npe_pod_info_v3(u8 *pod_type, u8 *pod_id, u8 *pod_sz, enum neuron_ultraserver_mode *mode, u32 *modes_supported)
{
	if (!ndhal_instance_type_pod()) {
		*pod_type = NEURON_POD_TYPE_NONE;
		*pod_sz = 0;
		*mode = NEURON_ULTRASERVER_MODE_UNSET;
		*modes_supported = 0;
	} else {
		*pod_type = NEURON_POD_TYPE_P2P;
		npe_get_pod_sz(pod_sz);
		npe_get_pod_id(pod_id);
		npe_get_pod_mode(mode);
		npe_get_pod_modes_supported(modes_supported);
	}
	return 0;
}

/**
 * npe_pod_status() - return status information about the pod the instance belongs to
 *
 * @param pod_state - state/outcome of the pod's election process
 * @param node_id   - node id within the pod
 *
 */
static int npe_pod_status_v3(u32 *pod_state, s8 *node_id)
{
	if (!ndhal_instance_type_pod()) {
		*pod_state = NEURON_POD_E_STATE_SINGLE_NODE;
		*node_id = -1;
		return 0;
	}
	return npe_get_pod_status(pod_state, node_id);
}

/**
 * npe_pod_ctrl() - control the state of the pod
 *
 * @nd:    neuron device
 * @param pod_ctrl  - control operation to perform
 * @param mode - requested operating mode for mode control
 * @param timeout - timeout for the control operation
 * @param pod_state - state/outcome of the pod's election process
 *
 */
static int npe_pod_ctrl_v3(struct neuron_device *nd, u32 pod_ctrl, enum neuron_ultraserver_mode mode, u32 timeout, u32 *pod_state)
{
	if (!ndhal_instance_type_pod()) {
		return 0;
	}
	return npe_pod_ctrl(nd, pod_ctrl, mode, timeout, pod_state);
}

/**
 * npe_class_node_id_show_data() - return sysfs class node_id
 *
 * @buf - sysfs buffer
 * @sz - size of ultraserver config to show data for
 *
 */
static ssize_t npe_class_node_id_show_data_v3(char *buf, u32 sz)
{
	if (!ndhal_instance_type_pod()) {
    	return dhal_sysfs_emit(buf, "-1\n");
	}
	return npe_class_node_id_show_data(buf, sz);
}

/**
 * npe_class_server_id_show_data() - return sysfs class node_id
 *
 * @buf - sysfs buffer
 * @sz - size of ultraserver config to show data for
 *
 */
static 	ssize_t npe_class_server_id_show_data_v3(char *buf, u32 sz)
{
	if (!ndhal_instance_type_pod()) {
    	return dhal_sysfs_emit(buf, "0000000000000000\n");
	}
	return npe_class_server_id_show_data(buf, sz);
}

/**
 * npe_class_ultraserver_mode_show_data() - return sysfs class ultraserver_mode
 *
 * @buf - sysfs buffer
 *
 */
static ssize_t npe_class_ultraserver_mode_show_data_v3(char *buf)
{
	if (!ndhal_instance_type_pod()) {
    	return dhal_sysfs_emit(buf, "\n");
	}
    return npe_class_ultraserver_mode_show_data(buf);
}

/**
 * ntpb_pe_get_aggregated_wl_cycle_cnt() - return aggregated pe array weight-load activity cycle count
 *
 * @param nd - neuron device
 * @param nc_id - neuron core id
 * @param row_grp_id - row group id
 * @param val: aggregated weight load cycle count
 * @return int - 0 on success
 *
 */
static int ntpb_pe_get_aggregated_wl_cycle_cnt_v3(struct neuron_device *nd, int nc_id, int row_grp_id, u64 *val)
{
	int ret = 0;
	u64 wl_cycle_cnt = 0;
	u64 fast_wl_cycle_cnt = 0;

	ret = ndhal->ndhal_tpb.pe_get_counter_val(nd->npdev.bar0, ndhal->ndhal_tpb.pe_get_row_grp_activity_counter_offset(ndhal->ndhal_tpb.pe_wl_cntr_offsets[nc_id], row_grp_id), &wl_cycle_cnt);
	if (ret) {
		return ret;
	}
	ret = ndhal->ndhal_tpb.pe_get_fast_wl_cycle_cnt(nd, nc_id, row_grp_id, &fast_wl_cycle_cnt);
	if (ret) {
		return ret;
	}

	*val = wl_cycle_cnt + fast_wl_cycle_cnt;

	return ret;
}

/**
 * ndhal_ext_cleanup_v3() - cleanup any extended resources`
 *
 */
static void ndhal_ext_cleanup_v3(void)
{
	if (ndhal_instance_type_pod()) {
    	npe_cleanup();
	}
	return;
}

/**
 * static asserts to valid static const sizes work across versions
 *
 */
static_assert( MAX_DRAM_CHANNELS >= V3_MAX_DRAM_CHANNELS, "Max dram channel count too small");
static_assert( MAX_TS_PER_DEVICE >= V3_TS_PER_DEVICE, "Max ts per device count too small");
static_assert( MAX_NC_PER_DEVICE >= V3_NC_PER_DEVICE, "Max nc per device count too small");
static_assert( MAX_NQ_TYPE >= V3_MAX_NQ_TYPE, "Max nq type count too small");
static_assert( MAX_NQ_ENGINE >= V3_MAX_NQ_QUEUES, "Max nq per engine count too small");
static_assert( NUM_DMA_ENG_PER_DEVICE >= V3_NUM_DMA_ENG_PER_DEVICE, "Max dma engine per device count too small");


/**
 * ndhal_register_funcs_v3() - initialize the dhal for v3 chips
 *
 */
int ndhal_register_funcs_v3(void) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for V3.");
		return -EINVAL;
	}

	ndhal->ndhal_address_map.pci_host_base = V3_PCIE_A0_BASE;
	ndhal->ndhal_address_map.mmap_p_offset = V3_MMAP_P_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_event_offset = V3_MMAP_NC_EVENT_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_read_offset = V3_MMAP_NC_SEMA_READ_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_set_offset = V3_MMAP_NC_SEMA_SET_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_incr_offset = V3_MMAP_NC_SEMA_INCR_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_decr_offset = V3_MMAP_NC_SEMA_DECR_OFFSET;
	ndhal->ndhal_address_map.bar0_misc_ram_offset = V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET;
	ndhal->ndhal_address_map.port_1_base = 0ull;
	ndhal->ndhal_address_map.mmap_nc_size = V3_MMAP_NC_SIZE;
	ndhal->ndhal_address_map.nc_per_device = V3_NC_PER_DEVICE;
	ndhal->ndhal_address_map.dev_nc_map = (1 << V3_NC_PER_DEVICE) - 1;
	ndhal->ndhal_address_map.dice_per_device = V3_NUM_DIE_PER_DEVICE;
	ndhal->ndhal_address_map.semaphore_count = V3_SEMAPHORE_COUNT;
	ndhal->ndhal_address_map.event_count = V3_EVENTS_COUNT;
	ndhal->ndhal_address_map.ts_per_device = V3_TS_PER_DEVICE;
	ndhal->ndhal_address_map.dma_eng_per_nc = V3_DMA_ENG_PER_NC;
	ndhal->ndhal_address_map.dram_channels = V3_MAX_DRAM_CHANNELS;
	ndhal->ndhal_reset.reset_poll_interval = V3_NR_RESET_POLL_INTERVAL;
	ndhal->ndhal_reset.reset_device_initial_poll_delay = 0;
	ndhal->ndhal_reset.reset_tpb_initial_poll_delay = 0;
	ndhal->ndhal_reset.initiate_max_wait_time = V3_NR_RESET_INIT_MAX_TOTAL_WAIT_TIME_MS;
	ndhal->ndhal_reset.retry_count = NR_RESET_RETRY_COUNT;
	ndhal->ndhal_reset.nr_post_reset_config = nr_post_reset_config_v3;
	ndhal->ndhal_topsp.ts_nq_init = ts_nq_init_v3;
	ndhal->ndhal_topsp.ts_nq_destroy_one = ts_nq_destroy_one_v3;
	ndhal->ndhal_topsp.ts_nq_get_nqid = ts_nq_get_nqid_v3;
	ndhal->ndhal_topsp.ts_nq_set_hwaddr = ts_nq_set_hwaddr_v3;
	ndhal->ndhal_nc.nc_get_semaphore_base = nc_get_semaphore_base_v3;
	ndhal->ndhal_nc.nc_get_event_addr = nc_get_event_addr_v3;
	ndhal->ndhal_nq.nnq_get_nqid = nnq_get_nqid_v3;
	ndhal->ndhal_nq.nnq_set_hwaddr = nnq_set_hwaddr_v3;
	ndhal->ndhal_mpset.mp_min_alloc_size = (mempool_min_alloc_size < 1024) ? 1024 : mempool_min_alloc_size;
	ndhal->ndhal_mpset.small_pool_supported = true;
	ndhal->ndhal_mpset.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v3;
	ndhal->ndhal_mpset.mpset_block_carveout_regions = mpset_block_carveout_regions_v3;
	ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id = ndmar_get_h2t_eng_id_v3;
	ndhal->ndhal_ndmar.ndmar_get_h2t_qid = ndmar_get_h2t_qid_v3;
	ndhal->ndhal_ndmar.ndmar_is_h2t_q = ndmar_is_h2t_q_v3;
	ndhal->ndhal_ndmar.nr_init_h2t_eng = nr_init_h2t_eng_v3;
	ndhal->ndhal_ndmar.ndmar_is_nx_ring = ndmar_is_nx_ring_v3;
	ndhal->ndhal_ndmar.ndmar_quiesce_queues = ndmar_quiesce_queues_v3;
	ndhal->ndhal_ndmar.ndmar_set_model_started = ndmar_set_model_started_v3;
	ndhal->ndhal_fw_io.fw_io_topology = fw_io_topology_v3;
	ndhal->ndhal_fw_io.fw_io_register_readless_read_region = fw_io_register_readless_read_region_v3;
	ndhal->ndhal_fw_io.fw_io_read_csr_array = fw_io_read_csr_array_v3;
	ndhal->ndhal_mmap.dm_mmap_special = dm_mmap_special_v3;
	ndhal->ndhal_mmap.mmap_get_bar4_offset = mmap_get_bar4_offset_v3;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl_cnt = root_info_node_attrs_info_tbl_cnt_v3;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl = root_info_node_attrs_info_tbl_v3;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_add_ecc_nodes = nsysfsmetric_add_ecc_nodes_v3;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_get_hbm_error_count = nsysfsmetric_get_hbm_error_count_v3;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_add_tensor_engine_node = nsysfsmetric_add_tensor_engine_node_v3;
	ndhal->ndhal_pci.axi_bar = BAR_UNUSED;
	ndhal->ndhal_pci.dram_bar = 4;
	ndhal->ndhal_pci.neuron_pci_release_bar = neuron_pci_release_bar_v3;
	ndhal->ndhal_pci.neuron_pci_reserve_bar = neuron_pci_reserve_bar_v3;
	ndhal->ndhal_pci.neuron_pci_set_npdev = neuron_pci_set_npdev_v3;
	ndhal->ndhal_pci.neuron_pci_get_device_id = neuron_pci_get_device_id_v3;
	ndhal->ndhal_pci.neuron_pci_device_id_to_rid_map = neuron_pci_device_id_to_rid_map_v3;
	ndhal->ndhal_cdev.ncdev_mem_regions = ncdev_mem_regions_v3;
	ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs = ncdev_bar0_write_blocked_addrs_v3;
	ndhal->ndhal_cdev.ncdev_compatible_version = ncdev_compatible_version_v3;
	ndhal->ndhal_cdev.ncdev_quiesce_exec_on_proc_exit = ncdev_quiesce_exec_on_proc_exit_v3;
	ndhal->ndhal_cdev.ncdev_bar_write_data = ncdev_bar_write_data_v3;
	ndhal->ndhal_cdev.ncdev_logical_to_physical_nc_map = ncdev_logical_to_physical_nc_map_v3;
	ndhal->ndhal_cdev.ncdev_get_default_tpbs_for_hbm = ncdev_get_default_tpbs_for_hbm_v3;
	ndhal->ndhal_udma.num_beats = 2296;  // allow up to 288 outstanding writes
	ndhal->ndhal_udma.udma_m2s_data_rd_cfg_boundaries_set = udma_m2s_data_rd_cfg_boundaries_set_v3;
	ndhal->ndhal_udma.udma_q_config = udma_q_config_v3;
	ndhal->ndhal_ndma.ndma_retry_memcpy = false;
	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v3;
	ndhal->ndhal_ndma.ndma_validate_pa = ndma_validate_pa_v3;
	ndhal->ndhal_ndma.ndma_init = ndma_init_v3;
	ndhal->ndhal_ndma.ndma_is_bar0_write_blocked = ndma_is_bar0_write_blocked_v3;
	ndhal->ndhal_ndma.ndma_get_m2m_barrier_type = ndma_get_m2m_barrier_type_v3;
	ndhal->ndhal_ndma.ndma_get_engines_with_host_connectivity = ndma_get_engines_with_host_connectivity_v3;
	ndhal->ndhal_npe.npe_notify_mark = npe_notify_mark_v3;
	ndhal->ndhal_npe.npe_pod_info = npe_pod_info_v3;
	ndhal->ndhal_npe.npe_pod_status = npe_pod_status_v3;
	ndhal->ndhal_npe.npe_pod_ctrl = npe_pod_ctrl_v3;
	ndhal->ndhal_npe.npe_class_node_id_show_data = npe_class_node_id_show_data_v3;
	ndhal->ndhal_npe.npe_class_server_id_show_data = npe_class_server_id_show_data_v3;
	ndhal->ndhal_npe.npe_class_ultraserver_mode_show_data = npe_class_ultraserver_mode_show_data_v3;
	ndhal->ndhal_tpb.pe_xbus_count = 9;
	ndhal->ndhal_tpb.pe_row_grp_count = 4;
	ndhal->ndhal_tpb.pe_col_grp_count = 4;
	ndhal->ndhal_tpb.pe_perf_reg_grp_size = V3_TPB_ARR_SEQ_QUEUE_PERF_SIZE;
	ndhal->ndhal_tpb.pe_mm_cntr_offsets = ntpb_pe_mm_cntr_offsets_v3;
	ndhal->ndhal_tpb.pe_wl_cntr_offsets = ntpb_pe_wl_cntr_offsets_v3;
	ndhal->ndhal_tpb.pe_fast_wl_cntr_offsets = ntpb_pe_fast_wl_cntr_offsets_v3;
	ndhal->ndhal_tpb.pe_idle_cntr_offsets = ntpb_pe_idle_cntr_offsets_v3;
	ndhal->ndhal_tpb.pe_get_aggregated_wl_cycle_cnt = ntpb_pe_get_aggregated_wl_cycle_cnt_v3;
	ndhal->ndhal_ext_cleanup = ndhal_ext_cleanup_v3;

	extern unsigned int nmetric_log_posts;
	if (narch_is_qemu()) {
		ndhal->ndhal_reset.retry_count *= 1000; // wait longer on qemu
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v3_qemu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v3_qemu;
		ndhal->ndhal_address_map.dma_eng_per_nd = V3_NC_PER_DEVICE * V3_DMA_ENG_PER_NC;
		ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v3_qemu_emu;
		ndhal->ndhal_pci.apb_bar = 2;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v3_qemu;
		ndhal->ndhal_address_map.dice_per_device = 1;

		// Disable metrics on inkling
		nmetric_log_posts = 0;
	} else if (narch_is_emu()) {
		ndhal->ndhal_reset.retry_count *= 1000; // wait longer on the emulator
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v3_emu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v3_emu;
		ndhal->ndhal_address_map.dma_eng_per_nd = nc_per_dev_param * V3_DMA_ENG_PER_NC;
		ndhal->ndhal_address_map.nc_per_device = nc_per_dev_param;
		ndhal->ndhal_address_map.dev_nc_map = dev_nc_map;
		ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v3_qemu_emu;
		ndhal->ndhal_pci.apb_bar = 0;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v3_emu;
		ndhal->ndhal_address_map.dice_per_device = 1;

		// Disable metrics on emulation
		nmetric_log_posts = 0;

	} else {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v3;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v3;
		ndhal->ndhal_address_map.dma_eng_per_nd = V3_NC_PER_DEVICE * V3_DMA_ENG_PER_NC;
		ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v3;
		ndhal->ndhal_pci.apb_bar = 0;
	}

	if (ndhal_instance_type_pod()) {
		ret = npe_init();
	}

	switch (ndhal->pci_device_id) {
		case TRN2_DEVICE_ID0:
			ret = ndhal_register_funcs_trn2();
			if (ret) {
				pr_err("failed to register ndhal funcs on trn2.\n");
				return ret;
			}
			break;
		default:
			pr_err("Unknown HW architecture. Can't init neuron_dhal.\n");
			return -EINVAL;
	}

	if (ndhal->ndhal_address_map.dev_nc_map >= (1 << ndhal->ndhal_address_map.nc_per_device)) {
		pr_err("Invalid nc map for device");
		return -EINVAL;
	}

	return ret;
}
