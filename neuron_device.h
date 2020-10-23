// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DEVICE_H
#define NEURON_DEVICE_H

#include "neuron_mempool.h"
#include "neuron_ring.h"
#include "neuron_core.h"

#include "neuron_ioctl.h"

// Maximum neuron devices supported on a system.
#define MAX_NEURON_DEVICE_COUNT 16

enum neuron_device_arch {
	NEURON_ARCH_INVALID,
	NEURON_ARCH_INFERENTIA = 1,
	NEURON_ARCH_NUM = 3
};

struct neuron_pci_device {
	phys_addr_t bar0_pa;
	void __iomem *bar0;
	u64 bar0_size;
	phys_addr_t bar2_pa;
	void __iomem *bar2;
	u64 bar2_size;
};

struct neuron_device {
	struct pci_dev *pdev;
	int device_index;
	u8 revision;
	pid_t current_pid;
	int current_pid_open_count;
	u8 architecture;

	void *cdev; // chardev created for this devices

	struct neuron_pci_device npdev;

	struct ndma_eng ndma_engine[NUM_DMA_ENG_PER_DEVICE];

	void *fw_io_ctx;

	struct mempool_set mpset;

	// memory chunk allocated for notification queue in each neuron core.
	struct mem_chunk *nq_mc[V1_NC_PER_DEVICE][MAX_NQ_SUPPORTED];

	int connected_device_count; // number of devices connected to this device
	u32 connected_devices[MAX_NEURON_DEVICE_COUNT]; // device ids of the connected devices
};

/**
 * neuron_pci_get_device() - Returns devices associated with given index.
 *
 * @device_index: device index
 *
 * Return: NULL if device does not exists, neuron_device otherwise.
 */
struct neuron_device *neuron_pci_get_device(u8 device_index);

#endif
