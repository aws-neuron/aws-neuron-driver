// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#include "neuron_ds.h"

#ifndef _NEURON_METRICS_H
#define _NEURON_METRICS_H

#define NEURON_METRICS_VERSION_STRING_MAX_LEN 63
#define NEURON_METRICS_MAX_POSTING_BUF_SIZE 128
#define NEURON_METRICS_VERSION_CAPACITY 2	// version capacity (max distinct versions per device)
#define NEURON_METRICS_VERSION_MAX_CAPACITY 8	// number of versions to maintain in history

#define POST_TIME_ALWAYS	0xF
#define POST_TIME_TICK_0	0x0
#define POST_TIME_TICK_1	0x1
#define POST_TICK_COUNT		2

#define NMETRIC_TYPE_CONSTANT     0x0
#define NMETRIC_TYPE_VERSION      0x1
#define NMETRIC_TYPE_COUNTER      0x2
#define NMETRIC_TYPE_FW_IO_ERR    0x3
#define NMETRIC_TYPE_BITMAP       0x4
#define NMETRIC_TYPE_CONSTANT_U64 0x5
#define NMETRIC_TYPE_DRIVER       0x6

#define NMETRIC_FLAG_VERS_ALLOW_TYPE	(1)

#define NMETRIC_CONST_U64_FLAG_SKIP_ZERO (0x1ull << 0)
#define NMETRIC_CONST_U64_FLAG_PREFER_FREED (0x1ull << 1)

enum driver_metrics_idx {
	NMETRIC_DRIVER_METRICS_IDX_MAX_DEVICE_RESET_TIME_MS = 0,
	NMETRIC_DRIVER_METRICS_IDX_MAX_TPB_RESET_TIME_MS = 1,

	NMETRIC_DRIVER_METRICS_IDX_AVG_DEVICE_RESET_TIME_MS = 2,
	NMETRIC_DRIVER_METRICS_IDX_AVG_TPB_RESET_TIME_MS = 3,

	// Intermediate metrics. do not post to CW directly.
	NMETRIC_DRIVER_METRICS_IDX_TOTAL_DEVICE_RESET_TIME_MS = 4,
	NMETRIC_DRIVER_METRICS_IDX_TOTAL_TPB_RESET_TIME_MS = 5,
	NMETRIC_DRIVER_METRICS_IDX_TOTAL_DEVICE_RESET_COUNT = 6,
	NMETRIC_DRIVER_METRICS_IDX_TOTAL_TPB_RESET_COUNT = 7,

	NMETRIC_DRIVER_METRICS_IDX_DEVICE_RESET_FAILURE_COUNT = 8,
	NMETRIC_DRIVER_METRICS_IDX_TPB_RESET_FAILURE_COUNT = 9,

	NMETRIC_DRIVER_METRICS_IDX_COUNT = 10,
};

// Sadly, the 3 #defines below need to be updated when adding new metrics to nmetric_defs
// Number of metrics of type NMETRIC_TYPE_VERSION
#define NMETRIC_VERSION_COUNT	3

// Number of metrics of type NMETRIC_TYPE_CONSTANT
#define NMETRIC_CONSTANTS_COUNT	2

// Number of metrics of type NMETRIC_TYPE_COUNTER + the special case (type NMETRIC_TYPE_FW_IO_ERR)
#define NMETRIC_COUNTER_COUNT	29

// Number of metrics of type NMETRIC_TYPE_BITMAP
#define NMETRIC_BITMAP_COUNT 1

// Number of metrics of type NMETRIC_CONSTANT_U64
#define NMETRIC_CONSTANT_U64_COUNT 1

// Number of metrics of type NMETRIC_DRIVER
#define NMETRIC_DRIVER_METRICS_COUNT NMETRIC_DRIVER_METRICS_IDX_COUNT

typedef struct {
	u8 index;	// metric specific index
	u8 type;	// metric type
	u8 count;	// metric specific count
	u8 tick;	// tick index on which the metric is posted
	u8 cw_id;	// target cloudwatch id
	u8 ds_id;	// source datastore id
	u8 flags;	// metric specific flags
} nmetric_def_t;

// Create a metric id which is an unique combination of id, type and post time
#define NMETRIC_DEF(_index, _type, _count, _tick, _cw_id, _ds_id, _flags)	{ .index = _index, .type = _type, .count = _count, .tick = _tick, .cw_id = _cw_id, .ds_id = _ds_id, .flags = _flags }

#define NMETRIC_CONSTANT_DEF(idx, tick, cw_id)               NMETRIC_DEF(idx, NMETRIC_TYPE_CONSTANT, 1, tick, cw_id, 0xFF, 0)
#define NMETRIC_VERSION_DEF(idx, tick, cw_id, ds_id, flags)  NMETRIC_DEF(idx, NMETRIC_TYPE_VERSION, NEURON_METRICS_VERSION_CAPACITY, tick, cw_id, ds_id, flags)
#define NMETRIC_COUNTER_DEF(idx, tick, cw_id, ds_id)         NMETRIC_DEF(idx, NMETRIC_TYPE_COUNTER, 1, tick, cw_id, ds_id, 0)
#define NMETRIC_BITMAP_DEF(idx, tick, cw_id, ds_id)          NMETRIC_DEF(idx, NMETRIC_TYPE_BITMAP, 1, tick, cw_id, ds_id, 0)
#define NMETRIC_CONSTANT_U64(idx, tick, cw_id, ds_id, flags) NMETRIC_DEF(idx, NMETRIC_TYPE_CONSTANT_U64, 1, tick, cw_id, ds_id, flags)
#define NMETRIC_DRIVER_DEF(idx, tick, cw_id)                 NMETRIC_DEF(idx, NMETRIC_TYPE_DRIVER, 1, tick, cw_id, 0xFF, 0)

struct nmetric_versions {
	u32 version_usage_count[NEURON_METRICS_VERSION_MAX_CAPACITY];
	u64 version_metrics[NEURON_METRICS_VERSION_MAX_CAPACITY];
};

struct nmetric_aggregation_thread {
	struct task_struct *thread; // aggregation thread that sends metrics every ~5 minutes
	wait_queue_head_t wait_queue;
	volatile bool running; // if cleared, thread would exit the loop
	u64 curr[NMETRIC_COUNTER_COUNT]; // metrics for the current session so far
	u64 prev[NMETRIC_COUNTER_COUNT]; // recorded metrics from the last post
	u64 freed[NMETRIC_COUNTER_COUNT]; // cache holding metrics that were freed before the posting period was reached
};

struct neuron_metrics {
	struct nmetric_versions component_versions[NMETRIC_VERSION_COUNT];
	u64 ds_freed_feature_bitmap_buf;						// stores unsent feature bitmap metrics about to be freed from datastore
	u64 ds_freed_metrics_buf[NMETRIC_COUNTER_COUNT];		// stores unsent metrics about to be freed from datastore
	u64 ds_freed_const_u64_buf[NMETRIC_CONSTANT_U64_COUNT];	// stores unsent constant u64 values about to be freed from datastore
	struct nmetric_aggregation_thread neuron_aggregation;	// aggregation thread that periodically aggregates and posts metrics
	u8 posting_buffer[NEURON_METRICS_MAX_POSTING_BUF_SIZE + 1];
    u64 driver_metrics[NMETRIC_DRIVER_METRICS_COUNT];		// stores driver internal metrics that is not in datastore
};

/**
 * nmetric_init_constants_metrics() - Gathers and stores device constant information for metric posting.
 * 
 * @note Should be called before metric posting is initialized
 * 
 */
void nmetric_init_constants_metrics(void);

/**
 * nmetric_partial_aggregate() - Aggregates all metrics in specified datastore entry. Expects datastore lock to have been acquired
 * 
 * @nd: neuron device
 * @entry: valid initialized datastore entry to aggregate metrics from
 * 
 */
void nmetric_partial_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry);

/**
 * neuron_stop_thread() - Stop aggregation thread of specified neuron device
 *
 * @nd: neuron device 
 *
 */
void nmetric_stop_thread(struct neuron_device *nd);

/**
 * nmetric_init_driver_metrics() - Initializes the driver metrics to 0
 * 
 * @param nd - the neuron device
 */
void nmetric_init_driver_metrics(struct neuron_device *nd);

/** nmetric_init() - Initializes neuron metric aggregation for the driver
 *
 * @nd: neuron device 
 *
 * Return: 0 on success, < 0 error code on failure
 */
int nmetric_init(struct neuron_device *nd);

/**
 * nmetric_set_reset_time_metrics() - Set the max TPB or device reset time seen so far.
 * 
 * @param cur_reset_time_ms: the current TPB or device reset time in milliseconds.
 * @param is_device_reset: whether it is TPB or device reset.
 */
void nmetric_set_reset_time_metrics(struct neuron_device *nd, uint64_t cur_reset_time_ms, bool is_device_reset);

/**
 * nmetric_increment_reset_failure_count() - Increment the reset failure count by 1 for a device or TPB reset failure.
 * 
 * @param nd: neuron device
 * @param is_device_reset: whether it is TPB or device reset.
 */
void nmetric_increment_reset_failure_count(struct neuron_device *nd, bool is_device_reset);

#endif
