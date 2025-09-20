// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Multiple user application can access neuron device.
 * All processes accessing a device needs to kept track so that clean up(memory free, reset)
 * can be done after all processes are done. For this array(attached_processes) is maintained in
 * the neuron_device. When a application open /dev/neuronX, this array is updated with process id.
 * An application can open /dev/neuronX multiple time. Each subsequent access would result in
 * incrementing use_count. When the use_count becomes zero, the neuron_device can be cleanedup.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "neuron_device.h"
#include "neuron_pid.h"

int npid_find_process_slot(struct neuron_device *nd)
{
	int i;
	for (i = 0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		if (nd->attached_processes[i].pid == task_tgid_nr(current))
			return i;
	}
	return -1;
}

static int npid_find_process_slot_by_task(struct neuron_device *nd)
{
	int i;
	for (i = 0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		if (nd->attached_processes[i].task == current)
			return i;
	}
	return -1;
}

int npid_is_attached_task(struct neuron_device *nd)
{
	int slot;
	slot = npid_find_process_slot_by_task(nd);
	if (slot == -1)
		return 0;

	pr_info("npid_is_attached_task: neuron: nd%d found task with new pid %u old pid %u:\n", nd->device_index, task_tgid_nr(current), nd->attached_processes[slot].pid);
	return nd->attached_processes[slot].open_count;
}

int npid_attached_process_count(struct neuron_device *nd)
{
	int i, count = 0;
	for (i = 0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		if (nd->attached_processes[i].pid != 0)
			count++;
	}
	return count;
}

int npid_is_attached(struct neuron_device *nd)
{
	int slot;
	slot = npid_find_process_slot(nd);
	if (slot == -1)
		return 0;
	return nd->attached_processes[slot].open_count;
}

void npid_print_usage(struct neuron_device *nd)
{
	int i;
	pr_info("neuron: nd%d usage:\n", nd->device_index);
	pr_info("current pid: %u\n", task_tgid_nr(current));
	for (i=0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		if (nd->attached_processes[i].pid > 0)
			pr_info("pid %d open count %d\n",
				nd->attached_processes[i].pid, nd->attached_processes[i].open_count);
	}
}

bool npid_attach(struct neuron_device *nd)
{
	int i;
	int slot;

	// if already attached just increment the use count
	slot = npid_find_process_slot(nd);
	if (slot != -1) {
		BUG_ON(nd->attached_processes[slot].open_count <= 0);
		nd->attached_processes[slot].open_count++;
		return true;
	}

	// find a free slot
	for (i=0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		if (nd->attached_processes[i].pid == 0) {
			nd->attached_processes[i].pid = task_tgid_nr(current);
			nd->attached_processes[i].task = current;
			nd->attached_processes[i].open_count = 1; //since the ioctl done after open set to 1
			pr_info("neuron:npid_attach: pid=%u, slot=%u\n", task_tgid_nr(current), i);
			return true;
		}
	}

	return false;
}

#define NPID_GET_SLOT() 			\
	int slot;				\
	slot = npid_find_process_slot(nd);	\
	if (slot == -1) {			\
		return -1;			\
	}

int npid_detach(struct neuron_device *nd)
{
	NPID_GET_SLOT();
	BUG_ON(nd->attached_processes[slot].open_count == 0);
	nd->attached_processes[slot].open_count--;
	// release the process if refcount becomes 0
	if (nd->attached_processes[slot].open_count == 0) {
		pr_info("neuron:npid_detach: pid=%u, slot=%u\n", nd->attached_processes[slot].pid, slot);
		nd->attached_processes[slot].pid = 0;
		nd->attached_processes[slot].task = NULL;
	}
	return nd->attached_processes[slot].open_count;
}

int npid_add_allocated_memory(struct neuron_device *nd, enum mem_location location, size_t amount) {
	NPID_GET_SLOT();
	nd->attached_processes[slot].memory_used[(int)location - 1] += amount;
	return 0;
}

int npid_dec_allocated_memory(struct neuron_device *nd, enum mem_location location, size_t amount) {
	size_t *dest;
	NPID_GET_SLOT();
	dest = &nd->attached_processes[slot].memory_used[(int)location - 1];
	*dest = *dest > amount ? *dest - amount : 0;
	return 0;
}

int npid_get_allocated_memory(struct neuron_device *nd, pid_t pid, size_t *host_memory, size_t *device_memory) {
	*host_memory = 0;
	*device_memory = 0;
	NPID_GET_SLOT();
	*host_memory = nd->attached_processes[slot].memory_used[MEM_LOC_HOST - 1];
	*device_memory = nd->attached_processes[slot].memory_used[MEM_LOC_DEVICE - 1];
	return 0;
}
