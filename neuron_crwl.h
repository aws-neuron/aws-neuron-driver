// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_CRWL_H
#define NEURON_CRWL_H

#include <linux/kernel.h>
#include <linux/types.h>

#include "neuron_ioctl.h"

struct neuron_crwl {
	struct mutex lock;
	struct neuron_uuid uuid; // UUID of currently running model

	volatile bool writer_acquired; // true if exclusive access is taken by a process
	pid_t writer_pid; // last pid which updated the model

	volatile u64 reader_count; // total number of outstanding readers.
};

/**
 * ncrwl_nc_range_mark() - Find free NCs of given count, acquire and return as bitmap.
 *
 * @param nc_count: Number of free NC need to acquire.
 * @param start_nc_index: Starting NC index from where search should start.
 * @param end_nc_index: Last NC index where search should end.
 * @param max_range_available: Maximum number of NC cores available. (valid only on failure)
 * @param result: Resulting acquired NC bitmap will be stored here.  Caller is expected to zero prior to call
 *
 * @return 0 on success, negative error code on failure.
 */
int ncrwl_nc_range_mark(u32 nc_count, u32 start_nc_index, u32 end_nc_index,
			u32 *max_range_available, volatile long unsigned int *result);

/**
 * ncrwl_nc_range_mark() - Release one or more NC acquired.
 *
 * @param free: Map of NCs to release.
 *
 */
void ncrwl_nc_range_unmark(volatile long unsigned int *bitmap);

/**
 * ncrwl_nc_range_pid_get( uint32_t nc_index, pid_t *pid)
 *
 * @param nc_index: core index which we want to return the owner pid
 * @param *pid: return pid value. 0 = no owner.
 *
 */
int ncrwl_nc_range_pid_get(uint32_t nc_index, pid_t *pid);

/**
 * ncrwl_range_mark_cnt_get() - return the count of cores that have been marked
 *
 */
int ncrwl_range_mark_cnt_get(void);

/**
 * ncrwl_reader_enter() - Takes reader lock of given neuron core.
 *
 * @nd: Neuron device.
 * @nc_index: Neuron Core Index
 * @uuid: Unique Identifier used by writer.
 *
 * @return 0 on success, negative error code on failure to take the lock.
 */
int ncrwl_reader_enter(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid);

/**
 * ncrwl_reader_exit() - Releases reader lock of given neuron core.
 *
 * @nd: Neuron device.
 * @nc_index: Neuron Core Index
 * @uuid: Unique Identifier used by writer.
 *
 * @return 0 on success, negative error code on failure to release the lock.
 */
int ncrwl_reader_exit(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid);

/**
 * ncrwl_writer_enter() - Takes writer lock of given neuron core.
 *
 * @nd: Neuron device.
 * @nc_index: Neuron Core Index
 * @uuid: Unique Identifier to identify the model.
 *
 * @return 0 on success, negative error code on failure to take the lock.
 */
int ncrwl_writer_enter(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid);

/**
 * ncrwl_writer_downgrade() - Downgrades given NC's write lock to read lock.
 *
 * @nd: Neuron device.
 * @nc_index: Neuron Core Index
 * @uuid: Unique Identifier used during ncrwl_writer_enter().
 *
 * @return 0 on success, negative error code on failure to release the lock.
 */
int ncrwl_writer_downgrade(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid);

/**
 * ncrwl_release_current_process() - Cleanup crwl for all cores owned by the current process.
 *
 * @nd: Neuron device.
 */
void ncrwl_release_current_process(struct neuron_device *nd);

#endif
