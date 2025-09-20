// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_RESET_H
#define NEURON_RESET_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/module.h>

struct neuron_device;

#define NR_RESET_RETRY_COUNT 5
#define NEURON_RESET_REQUEST_ALL 0xffffffff // special reset request id for internal driver resets

extern int no_reset;

enum {
	V2_FW_IO_REG_FW_TRIGGER_OFFSET = 0x800,
	V2_FW_IO_REG_FW_STATUS_OFFSET = 0x808,
	V2_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK = 0x8
};

enum neuron_reset_state {
	NEURON_RESET_STATE_STARTED = 1, // Reset is initiated
	NEURON_RESET_STATE_COMPLETED, // Reset is completed successfully
	NEURON_RESET_STATE_FAILED // Reset failed
};

struct neuron_reset_request {
	uint32_t request_id;
	uint32_t nc_map;
	volatile enum neuron_reset_state ret;
	volatile struct neuron_reset_request *next;
	volatile struct neuron_reset_request *prev;
};

struct neuron_reset {
	struct task_struct *thread; // reset thread
	wait_queue_head_t wait_queue;
	volatile bool stop; // if set, reset thread would exit the loop
	// request pending queue ptrs. always processed in order, singly linked list
	volatile struct neuron_reset_request *req_pending_head;
	volatile struct neuron_reset_request *req_pending_tail;
	// request completed queue ptrs. procs can come in and wait in any order, doubly linked list
	volatile struct neuron_reset_request *req_cmpl_head;
	volatile struct neuron_reset_request *req_cmpl_tail;
	struct mutex nr_lock;
	uint64_t reset_start_time; // the latest reset start time
	uint64_t reset_end_time;   // the latest reset end time
};

/**
 * nr_create_thread() - Create a thread to reset a neuron device.
 *
 * @nd: Neuron device which will be reset by the thread.
 *
 * Return: 0 on success, -1 on failure
 */
int nr_create_thread(struct neuron_device *nd);

/**
 * nr_stop_thread() - Stop reset thread.
 *
 * @nd: Neuron device
 */
void nr_stop_thread(struct neuron_device *nd);

/**
 * nr_start() - Initiate reset operation on the given device
 *
 * @nd: Neuron device to reset
 */
void nr_start(struct neuron_device *nd);

/**
 * nr_start_ncs() - Initiate reset operation on the given neuron core
 *
 * @nd: Neuron device to reset
 * @nc_map: Neuron core to reset (NEURON_NC_MAP_DEVICE to reset all cores)
 * @request_id: ID of this reset request
 *
 * Return: 0 if reset was successfully queued, 1 otherwise.
 */
int nr_start_ncs(struct neuron_device *nd, uint32_t nc_map, uint32_t request_id);

/**
 * nr_wait() - Waits for reset to complete
 *
 * @nd: Neuron device
 * @request_id: The reset request id to wait for
 * @check: If true, return success if request_id is not in the queue.
 *
 * Return: 0 if reset was successfully completed, 1 otherwise.
 */
int nr_wait(struct neuron_device *nd, uint32_t request_id, bool check);

/**
 * nr_op_in_reset_wnd() - Check if an operation is possibly within a reset window
 * 
 * @op_start_time: The start time of the operation
 * @nd: Neuron device
 * 
 */
bool nr_op_in_reset_wnd(uint64_t op_start_time, struct neuron_device *nd);

/**
 * nr_initiate_reset_via_fw() - Initiate a reset request to the device and retry until the device respond
 * 
 * @nd: Neuron device structure
 * @nc_map: Neural Core map that specifies reset scope (device vs TPB level)
 * @tpb_reset_map: Bitmap of TPBs to reset
 * 
 * @return: 0 on success, -1 on failure or interruption
 * 
 */
int nr_initiate_reset_via_fw(struct neuron_device *nd, uint32_t nc_map, uint32_t tpb_reset_map);

/**
 * nr_msleep_stoppable() - Sleep until msec or reset thread is stopped
 * 
 */
int nr_msleep_stoppable(struct neuron_device *nd, uint32_t msec);

#endif
