// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_PID_H
#define NEURON_PID_H

#include <linux/sched.h>

#include "neuron_arch.h"
#include "neuron_mempool.h"
#include "neuron_ring.h"
#include "neuron_core.h"

#include "neuron_ioctl.h"

struct neuron_attached_process {
	pid_t pid; // pid which attached to this process
	int open_count; // how many time this process opened this device.
	size_t memory_used[MEM_LOC_COUNT]; // currently allocated memory size
	void * task; // pointer to task
};

/**
 * npid_print_usage() - print all attached process info.
 *
 * @nd: Neuron device
 */
void npid_print_usage(struct neuron_device *nd);

/**
 * npid_is_attached_task() - checks whether current task is opened to the given device.
 *
 * @nd: Neuron device
 *
 * @return 0 if process is not attached, open_count by the processs otherwise.
 */
int npid_is_attached_task(struct neuron_device *nd);

/**
 * npid_is_attached() - checks whether current pid is opened to the given device.
 *
 * @nd: Neuron device
 *
 * @return 0 if process is not attached, open_count by the processs otherwise.
 */
int npid_is_attached(struct neuron_device *nd);

/** npid_attached_process_count() - Returns number of processes attached to the given device.
 *
 * @nd: Neuron device
 *
 * @return Number of processes attached to the device.
 */
int npid_attached_process_count(struct neuron_device *nd);

/**
 * npid_attach() - Attach current process to neuron device.
 *
 * @nd: Neuron device
 *
 * @return true if the current process attached successfully, false otherwise.
 */
bool npid_attach(struct neuron_device *nd);

/**
 * npid_detach() - Detach current process from neuron device.
 *
 * @nd: Neuron device
 *
 * @return number of open reference count for the current process on the device.
 */
int npid_detach(struct neuron_device *nd);

/**
 * npid_find_process_slot() - Find current process's slot index in the attached process array.
 *
 * @param nd - Neuron device
 *
 * @return -1 on failure, slot index on success
 */
int npid_find_process_slot(struct neuron_device *nd);

/**
 * npid_add_allocated_memory() - Adds allocated memory for the current pid
 *
 * @nd: Neuron device
 * @location: Memory location
 * @amount: Amount to increase with
 *
 * @return 0 if successful, -1 if current PID not found
 */
int npid_add_allocated_memory(struct neuron_device *nd, enum mem_location location, size_t amount);

/**
 * npid_dec_allocated_memory() - Decreases allocated memory for the current pid
 *
 * @nd: Neuron device
 * @location: Memory location
 * @amount: Amount to decrease with
 *
 * @return 0 if successful, -1 if current PID not found
 */
int npid_dec_allocated_memory(struct neuron_device *nd, enum mem_location location, size_t amount);

/**
 * npid_get_allocated_memory() - Gets allocated memory for the given pid
 *
 * @nd: Neuron device
 * @pid: PID for which to retrieve memory usage
 * @host_memory[out]: Amount of host memory in use
 * @device_memory[out]: Amount of device memory in use
 *
 */
int npid_get_allocated_memory(struct neuron_device *nd, pid_t pid, size_t *host_memory, size_t *device_memory);

#endif
