// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>

#include "neuron_device.h"
#include "neuron_dhal.h"

// Sleep range when waiting for lock state to be changed
#define NEURON_CRWL_SLEEP_MIN 10
#define NEURON_CRWL_SLEEP_MAX 20

// Number of times to retry (each retry takes between(10us to 20us)).
#define NEURON_CRWL_READER_MAX_RETRY (50 * 1000) // ~500ms
#define NEURON_CRWL_WRITER_MAX_RETRY (200 * 1000) // ~2sec

static inline int ncrwl_validate_uuid(struct neuron_device *nd, u32 nc_index,
				      struct neuron_uuid uuid)
{
	struct neuron_crwl *crwl;
	BUG_ON(nc_index >= MAX_NC_PER_DEVICE);

	crwl = &nd->crwl[nc_index];
	if (memcmp(&uuid, &crwl->uuid, sizeof(uuid)))
		return -ENOENT;

	if (crwl->writer_pid != task_tgid_nr(current)) {
		pr_err("nd%dnc%d: pid:%d Invalid pid - writer:%d\n",
		       nd->device_index, nc_index, task_tgid_nr(current), crwl->writer_pid);
		return -ENOENT;
	}
	return 0;
}

int ncrwl_reader_enter(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid)
{
	int ret = 0;
	struct neuron_crwl *crwl;
	u64 retry_count = 0;

	if (nc_index >= MAX_NC_PER_DEVICE)
		return -EINVAL;

	crwl = &nd->crwl[nc_index];

	mutex_lock(&crwl->lock);

	// if writer lock is taken then wait for writer to finish updating the model.
	while (crwl->writer_acquired) {
		mutex_unlock(&crwl->lock);
		if (retry_count > NEURON_CRWL_READER_MAX_RETRY) {
			pr_err("nd%dnc%d: pid:%d - reader starved. writer:%d\n",
			       nd->device_index, nc_index, task_tgid_nr(current), crwl->writer_pid);
			return -EBUSY;
		}
		retry_count++;
		usleep_range(NEURON_CRWL_SLEEP_MIN, NEURON_CRWL_SLEEP_MAX);
		mutex_lock(&crwl->lock);
	}

	ret = ncrwl_validate_uuid(nd, nc_index, uuid);
	if (ret)
		goto done;

	crwl->reader_count++;

done:
	mutex_unlock(&crwl->lock);
	return ret;
}

int ncrwl_reader_exit(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid)
{
	int ret = 0;
	struct neuron_crwl *crwl;

	if (nc_index >= MAX_NC_PER_DEVICE)
		return -EINVAL;

	crwl = &nd->crwl[nc_index];
	mutex_lock(&crwl->lock);
	ret = ncrwl_validate_uuid(nd, nc_index, uuid);
	if (ret)
		goto done;

	if (crwl->reader_count == 0) {
		pr_err("nd%dnc%d: pid:%d - reader count is already 0\n", nd->device_index, nc_index,
		       task_tgid_nr(current));
		ret = -EINVAL;
		goto done;
	}
	crwl->reader_count--;

done:
	mutex_unlock(&crwl->lock);
	return ret;
}

int ncrwl_writer_enter(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid)
{
	int ret = 0;
	struct neuron_crwl *crwl;
	u64 retry_count = 0;

	if (nc_index >= MAX_NC_PER_DEVICE)
		return -EINVAL;

	crwl = &nd->crwl[nc_index];
	mutex_lock(&crwl->lock);

	while (crwl->writer_acquired || // only one writer is allowed, so wait for other to release
	       crwl->reader_count) { // wait for old model's readers to finish
		mutex_unlock(&crwl->lock);
		if (retry_count > NEURON_CRWL_WRITER_MAX_RETRY) {
			pr_err("nd%dnc%d: pid:%d - writer starved. readers:%lld writer:%d\n",
			       nd->device_index, nc_index, task_tgid_nr(current),
			       crwl->reader_count, crwl->writer_acquired);
			return -EBUSY;
		}
		retry_count++;
		if (retry_count % 20000 == 0) {
			pr_info("nd%dnc%d: pid:%d - writer retrying(%lld/%d) readers:%lld writer:%d\n",
				nd->device_index, nc_index, task_tgid_nr(current),
				retry_count, NEURON_CRWL_WRITER_MAX_RETRY,
				crwl->reader_count, crwl->writer_pid);
		}
		usleep_range(NEURON_CRWL_SLEEP_MIN, NEURON_CRWL_SLEEP_MAX);
		mutex_lock(&crwl->lock);
	}

	crwl->writer_acquired = true;

	/* some other thread same process already switched to required model then no need redo it. */
	if (crwl->writer_pid == task_tgid_nr(current) &&
	    memcmp(&uuid, &crwl->uuid, sizeof(uuid)) == 0) {
		ret = -EALREADY;
		goto done;
	}

	crwl->writer_pid = task_tgid_nr(current);
	memcpy(&crwl->uuid, &uuid, sizeof(uuid));

done:
	mutex_unlock(&crwl->lock);
	return ret;
}

int ncrwl_writer_downgrade(struct neuron_device *nd, u32 nc_index, struct neuron_uuid uuid)
{
	int ret = 0;
	struct neuron_crwl *crwl;

	if (nc_index >= MAX_NC_PER_DEVICE)
		return -EINVAL;

	crwl = &nd->crwl[nc_index];
	mutex_lock(&crwl->lock);
	if (!crwl->writer_acquired) {
		pr_err("nd%dnc%d: pid:%d - writer downgrade called without enter\n",
		       nd->device_index, nc_index, task_tgid_nr(current));
		ret = -EINVAL;
		goto done;
	}
	ret = ncrwl_validate_uuid(nd, nc_index, uuid);
	if (ret)
		goto done;
	BUG_ON(crwl->reader_count != 0);

	crwl->writer_acquired = false;
	crwl->reader_count = 1;

done:
	mutex_unlock(&crwl->lock);
	return ret;
}


// Each entry points to NC and value points to PID acquired that NC.
static pid_t ncrwl_range_pids[MAX_NEURON_DEVICE_COUNT * MAX_NC_PER_DEVICE] = {0};
static int ncrwl_range_mark_cnt = 0;
DEFINE_MUTEX(ncrwl_range_lock); // lock to protect ncrwl_range_pids
int ncrwl_nc_range_mark(u32 nc_count, u32 start_nc_index, u32 end_nc_index,
			u32 *max_range, volatile long unsigned int *result_map)
{
	int i, j;
	*max_range = 0;
	if (start_nc_index > end_nc_index ||
		    start_nc_index >= MAX_NEURON_DEVICE_COUNT * MAX_NC_PER_DEVICE ||
		    end_nc_index >= MAX_NEURON_DEVICE_COUNT * MAX_NC_PER_DEVICE)
		return -EINVAL;
	mutex_lock(&ncrwl_range_lock);
	for (i = start_nc_index; i <= end_nc_index; i++) {
		int range_len = 1;
		// skip already marked ones
		if (ncrwl_range_pids[i] != 0)
			continue;

		// find how many consecutive neuron cores are free.
		for (j = i; j <= end_nc_index && j < (i + nc_count); j++) {
			if (ncrwl_range_pids[j] != 0)
				break;
			range_len = j - i + 1;
		}

		// if required number of NCs are free, mark them as acquired and update the result map
		if (range_len >= nc_count) {
			// notify election code of mark and provide prev range mark count
			ndhal->ndhal_npe.npe_notify_mark(ncrwl_range_mark_cnt, true);

			for (j = i; j <= end_nc_index && j < (i + nc_count); j++) {
				ncrwl_range_pids[j] = task_tgid_nr(current);
				set_bit(j, result_map);
				ncrwl_range_mark_cnt++;
			}
			mutex_unlock(&ncrwl_range_lock);
			return 0;
		}
		if (*max_range < range_len)
			*max_range = range_len;
		i = j + 1;
	}
	mutex_unlock(&ncrwl_range_lock);
	return -EBUSY;
}

void ncrwl_nc_range_unmark(volatile long unsigned int *free_map)
{
	int i;
	mutex_lock(&ncrwl_range_lock);
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT * MAX_NC_PER_DEVICE; i++) {
		if (test_bit(i, free_map) && ncrwl_range_pids[i] == task_tgid_nr(current)) {
			ncrwl_range_pids[i] = 0;
			ncrwl_range_mark_cnt--;
		}
		// notify election code of  decremented range mark count
		ndhal->ndhal_npe.npe_notify_mark(ncrwl_range_mark_cnt, false);
	}
	mutex_unlock(&ncrwl_range_lock);
}

int ncrwl_nc_range_pid_get( uint32_t nc_index, pid_t *pid)
{
	if (nc_index >= MAX_NEURON_DEVICE_COUNT * MAX_NC_PER_DEVICE) {
		return -ENOENT;
	}
	mutex_lock(&ncrwl_range_lock);
	*pid = ncrwl_range_pids[nc_index];
	mutex_unlock(&ncrwl_range_lock);
	return 0;
}

int ncrwl_range_mark_cnt_get(void)
{
	int cnt = 0;
	mutex_lock(&ncrwl_range_lock);
	cnt = ncrwl_range_mark_cnt;
	mutex_unlock(&ncrwl_range_lock);
	return cnt;
}

void ncrwl_release_current_process(struct neuron_device *nd)
{
	struct neuron_crwl *crwl;
	int nc_index;

	for (nc_index = 0; nc_index < MAX_NC_PER_DEVICE; nc_index++) {
		crwl = &nd->crwl[nc_index];
		mutex_lock(&crwl->lock);
		if (crwl->writer_pid == task_tgid_nr(current)) {
			crwl->reader_count = 0;
			crwl->writer_pid = 0;
			crwl->writer_acquired = false;
			memset(&crwl->uuid, 0, sizeof(struct neuron_uuid));
		}
		mutex_unlock(&crwl->lock);
	}
}
