
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 */
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/module.h>

#include "neuron_log.h"
#include "neuron_device.h"

#define NEURON_LOG_NUM_ENTRIES 1024

static const char * neuron_log_rec_type_to_str( enum neuron_log_type type)
{
	switch(type) {
		case NEURON_LOG_TYPE_INVALID:
			return "INVALID,";
		case NEURON_LOG_TYPE_FILE_OPEN:
			return "FOPEN,  ";
		case NEURON_LOG_TYPE_FILE_FLUSH:
			return "FFLUSH, ";
		case NEURON_LOG_TYPE_FILE_IOCTL:
			return "IOCTL,  ";
		case NEURON_LOG_TYPE_FILE_MMAP:
			return "MMAP,   ";
		default:
			break;
	}

	return "UNKNOWN,";
}

int neuron_log_init(struct neuron_device *nd)
{
	atomic_set(&nd->log_obj.tail, 0);
	nd->log_obj.log  = kzalloc(sizeof(struct neuron_log_rec) * NEURON_LOG_NUM_ENTRIES, GFP_KERNEL);

	if (nd->log_obj.log == NULL) {
		pr_info("neuron_log_init: failed to allocate memory for neuron log buffer\n");
		return -ENOMEM;
	}
	return 0;
}

void neuron_log_destroy(struct neuron_device *nd)
{
	if (nd->log_obj.log != NULL) {
		kfree(nd->log_obj.log);
	}
}

void neuron_log_rec_add(struct neuron_device *nd, enum neuron_log_type type, uint64_t data)
{
	uint32_t i;
	struct neuron_log_rec * log_rec;

	if (nd->log_obj.log == NULL) {
		return;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	i  = atomic_fetch_add(1, &nd->log_obj.tail);
#else
	i = atomic_add_return(1, &nd->log_obj.tail) - 1;
#endif 
	log_rec = &nd->log_obj.log[i % NEURON_LOG_NUM_ENTRIES];

	log_rec->type    = NEURON_LOG_TYPE_INVALID;
	log_rec->pid     = task_tgid_nr(current);
	log_rec->data    = data;
	log_rec->jiffies = get_jiffies_64();
	log_rec->type    = type;
}

#define NEURON_LOG_DUMP_LIMIT 128

int neuron_log_dump(struct neuron_device *nd, pid_t pid, uint32_t log_dump_limit)
{
	struct neuron_log_rec * log_snapshot = NULL;
	uint32_t tail_index;
	uint32_t i,j;
	uint64_t jiffies_now;
	struct timespec64 tv_now;
	struct timespec64 tv;
	struct tm tm;

	if (nd->log_obj.log == NULL) {
		return -ENOMEM;
	}

	log_snapshot = kzalloc(sizeof(struct neuron_log_rec) * NEURON_LOG_NUM_ENTRIES, GFP_KERNEL);
	if (log_snapshot == NULL) {
		pr_info("neuron_log_dump: failed to allocate memory for snapshot buffer\n");
		return -ENOMEM;
	}

	if (log_dump_limit == 0) {
		log_dump_limit = NEURON_LOG_DUMP_LIMIT;
	}

	// grab a copy of the log
	tail_index = (atomic_read(&nd->log_obj.tail) - 1) % NEURON_LOG_NUM_ENTRIES;
	memcpy(log_snapshot, nd->log_obj.log, sizeof(struct neuron_log_rec) * NEURON_LOG_NUM_ENTRIES);

	// scan backwards
	//
	for (i=1, j=0; i < NEURON_LOG_NUM_ENTRIES-1; i++) {
		struct neuron_log_rec * log_rec = &log_snapshot[(tail_index-i) % NEURON_LOG_NUM_ENTRIES];

		if ((log_rec->type == NEURON_LOG_TYPE_INVALID) || (pid && log_rec->pid != pid)) {
			continue;
		}
	 	if (j++ > log_dump_limit) {
			break;
		}
	}

	// get current jiffies and tv
	//
	jiffies_now = get_jiffies_64();
	ktime_get_real_ts64(&tv_now);

	// print forwards
	//
	i = (tail_index-i) % NEURON_LOG_NUM_ENTRIES;
	while (i != tail_index)  {
		struct neuron_log_rec * log_rec = &log_snapshot[i];

		i = (i+1) % NEURON_LOG_NUM_ENTRIES;
		if ((log_rec->type == NEURON_LOG_TYPE_INVALID) || (pid && log_rec->pid != pid)) {
			continue;
		}

		// convert jiffies into a timestamp
		//
		jiffies_to_timespec64(jiffies_now - log_rec->jiffies, &tv);
		tv = timespec64_sub(tv_now, tv);
		time64_to_tm(tv.tv_sec, 0, &tm);

		pr_info("neuron_log_dump: nd%02d: type: %s pid: %8u, data: %16llx, %4ld-%02d-%02d %02d:%02d:%02d.%06d\n",
				nd->device_index, neuron_log_rec_type_to_str(log_rec->type), log_rec->pid, log_rec->data,
				tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_nsec/1000));
	}

	if (log_snapshot != NULL) {
		kfree(log_snapshot);
	}
	return 0;
}
