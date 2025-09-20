// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DEVICE_H
#define NEURON_DEVICE_H

// Maximum neuron devices supported on a system.
#define MAX_NEURON_DEVICE_COUNT 64
#define MAX_NC_PER_DEVICE 8
#define MAX_TS_PER_DEVICE 16

#include "neuron_arch.h"
#include "neuron_mempool.h"
#include "neuron_mc_handle.h"
#include "neuron_ring.h"
#include "neuron_core.h"
#include "neuron_reset.h"
#include "neuron_cinit.h"
#include "neuron_crwl.h"
#include "neuron_pid.h"
#include "neuron_ioctl.h"
#include "neuron_ds.h"
#include "neuron_metrics.h"
#include "neuron_sysfs_metrics.h"
#include "neuron_log.h"
#include "neuron_power.h"

#ifndef static_assert
#define static_assert(expr, ...)
#endif

/* Vendor / Device ID for all devices supported by the driver */
#define AMZN_VENDOR_ID  0x1D0F
#define INF1_DEVICE_ID0 0x7064
#define INF1_DEVICE_ID1 0x7065
#define INF1_DEVICE_ID2 0x7066
#define INF1_DEVICE_ID3 0x7067
#define INF2_DEVICE_ID0 0x7264
#define TRN1_DEVICE_ID0 0x7164
#define TRN2_DEVICE_ID0 0x7364

// Global host memory buf size used for memset the device memory
#define MEMSET_HOST_BUF_SIZE MAX_DMA_DESC_SIZE // guessed optimal DMA transfer and PCIe TLP size.

struct neuron_pci_device {
	phys_addr_t bar0_pa;
	void __iomem *bar0;
	u64 bar0_size;
	phys_addr_t bar2_pa;
	void __iomem *bar2;
	u64 bar2_size;
	phys_addr_t bar4_pa;
	void __iomem *bar4;
	u64 bar4_size;
};

enum neuron_device_state {
	NEURON_DEVICE_STATE_RESET = 0,
	NEURON_DEVICE_STATE_READY = 1,
	NEURON_DEVICE_STATE_INVALID = 2
};

struct neuron_hbm_scrub_ctx {
	struct mem_chunk *rx_mc[NUM_DMA_ENG_PER_DEVICE];
	struct mem_chunk *tx_mc[NUM_DMA_ENG_PER_DEVICE];
	struct mem_chunk *completion_marker_buf[MAX_DRAM_CHANNELS]; // one memchunk shared by all DMA engines for an HBM to reduce internal fragmentation
	struct mem_chunk *hostbuf_mc[NUM_DMA_ENG_PER_DEVICE];
};

struct neuron_device {
	struct pci_dev *pdev;
	int device_index;
	volatile enum neuron_device_state device_state; // current state of this device

	// all the processes that are opened this device
	struct neuron_attached_process attached_processes[NEURON_MAX_PROCESS_PER_DEVICE];

	void *ncdev; // chardev created for this devices

	struct neuron_pci_device npdev;

	bool dmar_init_done[MAX_NC_PER_DEVICE];
	struct ndma_eng ndma_engine[NUM_DMA_ENG_PER_DEVICE];
	struct mem_chunk *ndma_q_dummy_mc; // when any DMA queue is reset, it would set to point to this mc

	void *fw_io_ctx;

	struct mempool_set mpset;

	// memory chunk allocated for notification queue in each neuron core.
	struct mem_chunk *nq_mc[MAX_NC_PER_DEVICE][MAX_NQ_SUPPORTED];
	// memory chunk allocated for notification queue in each TOP_SP.
	struct mem_chunk *ts_nq_mc[MAX_TS_PER_DEVICE][MAX_NQ_SUPPORTED];

	// memory chunk for setting device mem
	struct mem_chunk *memset_mc;
	struct mutex memset_lock;

	// memory chunk handle map
	struct neuron_mc_handle_map nmch;

	struct neuron_datastore datastore; // neuron datastore

	struct neuron_metrics metrics; // aggregated metrics

	struct neuron_power power; // stats about device power utilization

	struct neuron_crwl crwl[MAX_NC_PER_DEVICE]; // cooperative rw lock per NC.

	struct neuron_reset nr; // neuron reset thread's data

	struct neuron_cinit nci[MAX_NC_PER_DEVICE]; // neuron

	u64 nc_model_started_count[MAX_NC_PER_DEVICE]; // number of times the NCs has started model

	struct nsysfsmetric_metrics sysfs_metrics;
	
	struct neuron_log_obj log_obj; // logging object

	struct neuron_hbm_scrub_ctx hbm_scrub_ctx;
};

#endif
