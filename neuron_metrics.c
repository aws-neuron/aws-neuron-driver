// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/ctype.h>

#include "neuron_trace.h"
#include "neuron_metrics.h"
#include "neuron_device.h"
#include "neuron_dhal.h"
#include "neuron_power.h"

unsigned int nmetric_metric_post_delay = 150000; // milliseconds
unsigned int nmetric_metric_sample_delay = 50; // milliseconds.
unsigned int nmetric_log_posts = 1;

module_param(nmetric_metric_post_delay, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(nmetric_metric_post_delay, "Minimum time to wait (in milliseconds) before posting metrics again");

module_param(nmetric_log_posts, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(nmetric_log_posts, "1: send metrics to CW, 2: send metrics to trace, 3: send metrics to both");

static int nmetric_counters_buf_size = sizeof(u64) * NMETRIC_COUNTER_COUNT;
static int nmetric_versions_buf_size = sizeof(struct nmetric_versions) * NMETRIC_VERSION_COUNT;
static int nmetric_constants_buf_size = sizeof(char) * NMETRIC_CONSTANTS_COUNT * (NEURON_METRICS_VERSION_STRING_MAX_LEN + 1);

static char nmetric_constant_metrics[NMETRIC_CONSTANTS_COUNT][NEURON_METRICS_VERSION_STRING_MAX_LEN + 1];
static const char nmetric_instance_id_path[] = "/sys/devices/virtual/dmi/id/board_asset_tag";
extern const char driver_version[];

enum nmetric_cw_id {
	NMETRIC_CW_ID_UNUSED = 0,
	NMETRIC_CW_ID_FW_IO_ERROR_COUNT = 11, // internal driver fw_io error count. counted internally using a counter in fw_io_ctx struct
	NMETRIC_CW_ID_INSTANCE_ID = 12, // instance id
	NMETRIC_CW_ID_DRIVER_VERSION = 13, // driver version

    // Driver internal metics
	NMETRIC_CW_ID_MAX_DEVICE_RESET_TIME_MS = 50,
	NMETRIC_CW_ID_MAX_TPB_RESET_TIME_MS = 51,
	NMETRIC_CW_ID_AVG_DEVICE_RESET_TIME_MS = 52,
	NMETRIC_CW_ID_AVG_TPB_RESET_TIME_MS = 53,
	NMETRIC_CW_ID_DEVICE_RESET_FAILURE_COUNT = 54,
	NMETRIC_CW_ID_TPB_RESET_FAILURE_COUNT = 55,

	// Extra versions
	// extra space for reporting multiple versions of the same type in one post
	NMETRIC_CW_ID_RT_VERSION_BASE = 180, // base id for rt version
	NMETRIC_CW_ID_RT_VERSION_0 = NMETRIC_CW_ID_RT_VERSION_BASE,
	NMETRIC_CW_ID_RT_VERSION_1,
	NMETRIC_CW_ID_RT_VERSION_LAST = NMETRIC_CW_ID_RT_VERSION_1, // inclusive of last version

	NMETRIC_CW_ID_FW_VERSION_BASE = 190,
	NMETRIC_CW_ID_FW_VERSION_0 = NMETRIC_CW_ID_FW_VERSION_BASE,
	NMETRIC_CW_ID_FW_TYPE_0,
	NMETRIC_CW_ID_FW_VERSION_1,
	NMETRIC_CW_ID_FW_TYPE_1,
	NMETRIC_CW_ID_FW_VERSION_LAST = NMETRIC_CW_ID_FW_TYPE_1,

	NMETRIC_CW_ID_FAL_VERSION_BASE = 195,
	NMETRIC_CW_ID_FAL_VERSION_0 = NMETRIC_CW_ID_FAL_VERSION_BASE,
	NMETRIC_CW_ID_FAL_VERSION_1,
	NMETRIC_CW_ID_FAL_VERSION_LAST = NMETRIC_CW_ID_FAL_VERSION_1,

	// Return codes
	NMETRIC_CW_ID_NERR_OK = 200, // status ok
	NMETRIC_CW_ID_NERR_FAIL = 201, // status fail
	NMETRIC_CW_ID_NERR_INVALID = 202,
	NMETRIC_CW_ID_NERR_RESOURCE = 204,
	NMETRIC_CW_ID_NERR_TIMEOUT = 205,
	NMETRIC_CW_ID_NERR_HW_ERROR = 206,
	NMETRIC_CW_ID_NERR_QUEUE_FULL = 207,
	NMETRIC_CW_ID_NERR_RESOURCE_NC = 208,
	NMETRIC_CW_ID_NERR_UNSUPPORTED_VERSION = 209,
	NMETRIC_CW_ID_NERR_INFER_BAD_INPUT = 212,
	NMETRIC_CW_ID_NERR_INFER_COMPLETED_WITH_NUM_ERR = 213,
	NMETRIC_CW_ID_NERR_INFER_COMPLETED_WITH_ERR = 214,
	NMETRIC_CW_ID_NERR_NUMERICAL_ERR = 215,
	NMETRIC_CW_ID_NERR_MODEL_ERR = 216,
	NMETRIC_CW_ID_NERR_TRANSIENT_ERR = 217,
	NMETRIC_CW_ID_NERR_RT_ERR = 218,
	NMETRIC_CW_ID_NERR_GENERIC_TPB_ERR = 219, // generic notification error
	                                          // for reference look at "INFER_SUBTYPE_NONE" in
	                                          // Runtime repo "tdrv/infer_error_subtype_int.c"
	NMETRIC_CW_ID_NERR_OOB = 220,
	NMETRIC_CW_ID_NERR_HW_ERR_COLLECTIVES = 221,
	NMETRIC_CW_ID_NERR_HW_ERR_HBM_UE = 222,
	NMETRIC_CW_ID_NERR_HW_ERR_NC_UE = 223,
	NMETRIC_CW_ID_NERR_HW_ERR_DMA_ABORT = 224,
	NMETRIC_CW_ID_NERR_SW_SEMAPHORE_ERROR = 225,
	NMETRIC_CW_ID_NERR_SW_EVENT_ERROR = 226,
	NMETRIC_CW_ID_NERR_SW_PSUM_COLLISION = 227,
	NMETRIC_CW_ID_NERR_SW_SEQUENCER_FATAL = 228,
	NMETRIC_CW_ID_NERR_HW_ERR_REPAIRABLE_HBM_UE = 229,

	NMETRIC_CW_ID_FEATURE_BITMAP = 250,
	NMETRIC_CW_ID_SYSFS_METRIC_BITMAP = 251,
	NMETRIC_CW_ID_DEVICE_CLUSTER_ID = 252,
	NMETRIC_CW_ID_NERR_SW_NQ_OVERFLOW = 253,
};

static const nmetric_def_t nmetric_defs[] = {
	// constant metrics
	NMETRIC_CONSTANT_DEF(0, POST_TIME_ALWAYS, NMETRIC_CW_ID_INSTANCE_ID), // instance id
	NMETRIC_CONSTANT_DEF(1, POST_TIME_ALWAYS, NMETRIC_CW_ID_DRIVER_VERSION), // driver version

	// version metrics
	NMETRIC_VERSION_DEF(0, POST_TIME_ALWAYS, NMETRIC_CW_ID_RT_VERSION_BASE, NDS_ND_COUNTER_RUNTIME_VERSION, 0), // rt version
	NMETRIC_VERSION_DEF(1, POST_TIME_TICK_1, NMETRIC_CW_ID_FW_VERSION_BASE, NDS_ND_COUNTER_FRAMEWORK_VERSION, NMETRIC_FLAG_VERS_ALLOW_TYPE), // fw version
	NMETRIC_VERSION_DEF(2, POST_TIME_TICK_1, NMETRIC_CW_ID_FAL_VERSION_BASE, NDS_ND_COUNTER_FAL_VERSION, 0), // fal version

	// counter metrics
	NMETRIC_COUNTER_DEF(0, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_OK, NDS_NC_COUNTER_INFER_COMPLETED),
	NMETRIC_COUNTER_DEF(1, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_FAIL, NDS_NC_COUNTER_GENERIC_FAIL),
	NMETRIC_COUNTER_DEF(2, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_TIMEOUT, NDS_NC_COUNTER_INFER_TIMED_OUT),
	NMETRIC_COUNTER_DEF(3, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_INFER_BAD_INPUT, NDS_NC_COUNTER_INFER_INCORRECT_INPUT),
	NMETRIC_COUNTER_DEF(4, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_NUMERICAL_ERR, NDS_NC_COUNTER_ERR_NUMERICAL),
	NMETRIC_COUNTER_DEF(5, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_MODEL_ERR, NDS_NC_COUNTER_ERR_MODEL),
	NMETRIC_COUNTER_DEF(6, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_TRANSIENT_ERR, NDS_NC_COUNTER_ERR_TRANSIENT),
	NMETRIC_COUNTER_DEF(7, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_HW_ERROR, NDS_NC_COUNTER_ERR_HW),
	NMETRIC_COUNTER_DEF(8, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_RT_ERR, NDS_NC_COUNTER_ERR_RT),
	NMETRIC_COUNTER_DEF(9, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_INFER_COMPLETED_WITH_ERR, NDS_NC_COUNTER_INFER_COMPLETED_WITH_ERR),
	NMETRIC_COUNTER_DEF(10, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_INFER_COMPLETED_WITH_NUM_ERR, NDS_NC_COUNTER_INFER_COMPLETED_WITH_NUM_ERR),
	NMETRIC_COUNTER_DEF(11, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_GENERIC_TPB_ERR, NDS_NC_COUNTER_ERR_GENERIC),
	NMETRIC_COUNTER_DEF(12, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_RESOURCE, NDS_NC_COUNTER_ERR_RESOURCE),
	NMETRIC_COUNTER_DEF(13, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_RESOURCE_NC, NDS_NC_COUNTER_ERR_RESOURCE_NC),
	NMETRIC_COUNTER_DEF(14, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_QUEUE_FULL, NDS_NC_COUNTER_INFER_FAILED_TO_QUEUE),
	NMETRIC_COUNTER_DEF(15, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_INVALID, NDS_NC_COUNTER_ERR_INVALID),
	NMETRIC_COUNTER_DEF(16, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_UNSUPPORTED_VERSION, NDS_NC_COUNTER_ERR_UNSUPPORTED_NEFF_VERSION),

	// special counter metric case
	NMETRIC_DEF(17, NMETRIC_TYPE_FW_IO_ERR, 1, POST_TIME_TICK_0, NMETRIC_CW_ID_FW_IO_ERROR_COUNT, 0xFF, 0),

	// counter metrics continue
	NMETRIC_COUNTER_DEF(18, POST_TIME_TICK_0, NMETRIC_CW_ID_NERR_OOB, NDS_NC_COUNTER_OOB),

	NMETRIC_COUNTER_DEF(19, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_HW_ERR_COLLECTIVES, NDS_EXT_NC_COUNTER_HW_ERR_COLLECTIVES),
	NMETRIC_COUNTER_DEF(20, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_HW_ERR_HBM_UE, NDS_EXT_NC_COUNTER_HW_ERR_HBM_UE),
	NMETRIC_COUNTER_DEF(21, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_HW_ERR_NC_UE, NDS_EXT_NC_COUNTER_HW_ERR_NC_UE),
	NMETRIC_COUNTER_DEF(22, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_HW_ERR_DMA_ABORT, NDS_EXT_NC_COUNTER_HW_ERR_DMA_ABORT),

	NMETRIC_COUNTER_DEF(23, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_SW_NQ_OVERFLOW, NDS_EXT_NC_COUNTER_ERR_SW_NQ_OVERFLOW),

	NMETRIC_COUNTER_DEF(24, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_SW_SEMAPHORE_ERROR, NDS_EXT_NC_COUNTER_ERR_SW_SEMAPHORE_ERROR),
	NMETRIC_COUNTER_DEF(25, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_SW_EVENT_ERROR, NDS_EXT_NC_COUNTER_ERR_SW_EVENT_ERROR),
	NMETRIC_COUNTER_DEF(26, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_SW_PSUM_COLLISION, NDS_EXT_NC_COUNTER_ERR_SW_PSUM_COLLISION),
	NMETRIC_COUNTER_DEF(27, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_SW_SEQUENCER_FATAL, NDS_EXT_NC_COUNTER_ERR_SW_SEQUENCER_FATAL),
	NMETRIC_COUNTER_DEF(28, POST_TIME_TICK_1, NMETRIC_CW_ID_NERR_HW_ERR_REPAIRABLE_HBM_UE, NDS_EXT_NC_COUNTER_HW_ERR_REPAIRABLE_HBM_UE),

	// bitmap metrics
	NMETRIC_BITMAP_DEF(0, POST_TIME_TICK_1, NMETRIC_CW_ID_FEATURE_BITMAP, NDS_ND_COUNTER_FEATURE_BITMAP),
	NMETRIC_BITMAP_DEF(0, POST_TIME_TICK_1, NMETRIC_CW_ID_UNUSED, NDS_ND_COUNTER_DYNAMIC_SYSFS_METRIC_BITMAP),

	// const uint64 metrics
	NMETRIC_CONSTANT_U64(0, POST_TIME_TICK_1, NMETRIC_CW_ID_DEVICE_CLUSTER_ID, NDS_ND_COUNTER_DEVICE_CLUSTER_ID, NMETRIC_CONST_U64_FLAG_SKIP_ZERO),

	// driver metrics. not in datastore
	NMETRIC_DRIVER_DEF(NMETRIC_DRIVER_METRICS_IDX_MAX_DEVICE_RESET_TIME_MS, POST_TIME_TICK_1, NMETRIC_CW_ID_MAX_DEVICE_RESET_TIME_MS),
	NMETRIC_DRIVER_DEF(NMETRIC_DRIVER_METRICS_IDX_MAX_TPB_RESET_TIME_MS, POST_TIME_TICK_1, NMETRIC_CW_ID_MAX_TPB_RESET_TIME_MS),
	NMETRIC_DRIVER_DEF(NMETRIC_DRIVER_METRICS_IDX_AVG_DEVICE_RESET_TIME_MS, POST_TIME_TICK_1, NMETRIC_CW_ID_AVG_DEVICE_RESET_TIME_MS),
	NMETRIC_DRIVER_DEF(NMETRIC_DRIVER_METRICS_IDX_AVG_TPB_RESET_TIME_MS, POST_TIME_TICK_1, NMETRIC_CW_ID_AVG_TPB_RESET_TIME_MS),
	NMETRIC_DRIVER_DEF(NMETRIC_DRIVER_METRICS_IDX_DEVICE_RESET_FAILURE_COUNT, POST_TIME_TICK_1, NMETRIC_CW_ID_DEVICE_RESET_FAILURE_COUNT),
	NMETRIC_DRIVER_DEF(NMETRIC_DRIVER_METRICS_IDX_TPB_RESET_FAILURE_COUNT, POST_TIME_TICK_1, NMETRIC_CW_ID_TPB_RESET_FAILURE_COUNT),
};
static const int nmetric_count = sizeof(nmetric_defs) / sizeof(nmetric_def_t);

// IMPORTANT !!!
// If adding entries to nmetric_def_t, make sure the #defines below are still valid
// AND don't forget to increase the NMETRIC_..._COUNT in neuron_metrics.h
#define NMETRIC_INSTANCE_ID_IDX		0
#define NMETRIC_DRIVER_VERS_IDX 	1
#define NMETRIC_FW_IO_ERR_IDX		17

struct nmetric_cw_metric {
	u8 id;
	u8 len;
	u8 data[];
} __attribute__((__packed__));

/**
 * nmetric_init_constants_metrics() - Reads constants from their various sources
 *
 */
void nmetric_init_constants_metrics()
{
	int read_size;
	struct file *f;
	int driver_ver_str_len;
	int instance_id_idx = nmetric_defs[NMETRIC_INSTANCE_ID_IDX].index;
	int driver_vers_idx = nmetric_defs[NMETRIC_DRIVER_VERS_IDX].index;
	loff_t offset = 0;

	// initiate buffer to 0
	memset(nmetric_constant_metrics, 0, nmetric_constants_buf_size);
	// read instance id
	f = filp_open(nmetric_instance_id_path, O_RDONLY, 0);
	if (IS_ERR_OR_NULL(f) || (read_size = kernel_read(f, nmetric_constant_metrics[instance_id_idx], NEURON_METRICS_VERSION_STRING_MAX_LEN, &offset)) <= 0)
		memset(nmetric_constant_metrics[instance_id_idx], '0', sizeof(char)); // if instance id could not be read, default to 0
	else if (isspace(nmetric_constant_metrics[instance_id_idx][read_size - 1])) // remove trailing space if present
		nmetric_constant_metrics[instance_id_idx][read_size - 1] = '\0';

	if (!IS_ERR_OR_NULL(f))
		filp_close(f, NULL);

	// record driver version
	driver_ver_str_len = strlen(driver_version);
	BUG_ON(driver_ver_str_len > NEURON_METRICS_VERSION_STRING_MAX_LEN); // check for buffer overflow
	memcpy(nmetric_constant_metrics[driver_vers_idx], driver_version, min(driver_ver_str_len, (int)NEURON_METRICS_VERSION_STRING_MAX_LEN));
}

/**
 * nmetric_aggregate_version_metrics() - Gathers and stores version information of specified version metric in specified buffer for specified datastore entry
 *
 * @entry: valid initialized datastore entry to aggregate version info from
 * @ds_index: ds index of version metric to be recorded
 * @versions_buf: specified buffer where versions will be recorded
 *
 */
static void nmetric_aggregate_version_metrics(struct neuron_datastore_entry *entry, int ds_index, struct nmetric_versions *versions_buf)
{
	int i;
	void *ds_base_ptr = entry->mc->va;
	u64 version_info = NDS_ND_COUNTERS(ds_base_ptr)[ds_index]; // decode version information
	if (version_info == 0)
		return;
	u8 min_index = 0;
	u32 min_usage_count = ~0u;

	for (i = 0; i < NEURON_METRICS_VERSION_MAX_CAPACITY; i++) {
		if (version_info == versions_buf->version_metrics[i]) {
			versions_buf->version_usage_count[i]++;
			return;
		}
		if (versions_buf->version_usage_count[i] < min_usage_count) {
			min_index = i;
			min_usage_count = versions_buf->version_usage_count[i];
		}
	}
	versions_buf->version_metrics[min_index] = version_info;
	versions_buf->version_usage_count[min_index] = 1;
}

/**
 * nmetric_check_post_tick()
 *
 * Return if a metric needs to be posted based on its tick value and the global tick value
 *
 * @tick - current tick value
 * @metric_tick - metric tick value
 */
static inline bool nmetric_check_post_tick(u8 tick, const nmetric_def_t *metric)
{
	return tick == POST_TIME_ALWAYS || metric->tick == POST_TIME_ALWAYS || tick == metric->tick;
}

/**
 * nmetric_aggregate_nd_counter_entry()
 *
 * Aggregates all metrics in specified datastore entry to specified buffer. Counter metrics are added together.
 * Multiple version metrics are can be gathered per posting session up to a predefined limit. Any excess versions will be discarded
 *
 * @nd: neuron device
 * @entry: valid initialized datastore entry to aggregate metrics from
 * @dest_buf: destination buffer to recieve all aggregated data from datastore entry, must be large enough to accommodate all counters being tracked
 * @feature_bitmap: destination buffer to recieve feature_bitmap data from datastore entry
 * @tick: current tick value
 */
static void nmetric_aggregate_nd_counter_entry(struct neuron_device *nd, struct neuron_datastore_entry *entry, u64 *dest_buf,
                                               u64 *feature_bitmap, u64 *const_u64_metrics, u8 tick)
{
	int nc_id;
	int nmetric_index;
	const nmetric_def_t *curr_metric;
	void *ds_base_ptr = entry->mc->va;

	for (nmetric_index = 0; nmetric_index < nmetric_count; nmetric_index++) {
		curr_metric = &nmetric_defs[nmetric_index];
		if (!nmetric_check_post_tick(tick, curr_metric))
			continue;
		switch(curr_metric->type) {
		case NMETRIC_TYPE_VERSION:
			nmetric_aggregate_version_metrics(entry,
							  curr_metric->ds_id,
							  &nd->metrics.component_versions[curr_metric->index]);
		break;
		case NMETRIC_TYPE_COUNTER:
			for (nc_id = 0; nc_id < ndhal->ndhal_address_map.nc_per_device; nc_id++) {
				if (((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0) {
					continue;
				}
				dest_buf[curr_metric->index] += get_neuroncore_counter_value(entry, nc_id, curr_metric->ds_id);
			}
		break;
		case NMETRIC_TYPE_BITMAP:
			if (curr_metric->ds_id == NDS_ND_COUNTER_FEATURE_BITMAP) {
				*feature_bitmap |= NDS_ND_COUNTERS(ds_base_ptr)[curr_metric->ds_id];
			}
		break;
		case NMETRIC_TYPE_CONSTANT_U64:
			const_u64_metrics[curr_metric->index] = NDS_ND_COUNTERS(ds_base_ptr)[curr_metric->ds_id];
		break;
		}
	}
}

/**
 * nmetric_full_aggregation() - Aggregates all metrics in all datastore entries in device to specified buffer
 *
 * @nd: neuron device
 * @curr_metrics: destination buffer to recieve all aggregated data from datastore entry, must be large enough to accommodate all counters being tracked
 * @curr_feature_bitmap: destination buffer to recieve feature_bitmap from datastore entry
 * @tick: current tiint ds_id;ck value
 *
 */
static void nmetric_full_aggregate(struct neuron_device *nd, u64 *curr_metrics, u64 *curr_feature_bitmap, u64 *const_u64_metrics, u8 tick)
{
	// aggregate counter metrics in all cores of all entries of the datastore into current count array
	int i;
	const nmetric_def_t *nmetric_fw_io_def = &nmetric_defs[NMETRIC_FW_IO_ERR_IDX];

	for (i = 0; i < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; i++)
		if (neuron_ds_check_entry_in_use(&nd->datastore, i)) // ensure that datastore entry is in use and valid
			nmetric_aggregate_nd_counter_entry(nd, &nd->datastore.entries[i], curr_metrics, curr_feature_bitmap, const_u64_metrics, tick);

	// update metrics that do not have counters in nds
	if (nmetric_check_post_tick(tick, nmetric_fw_io_def))
		curr_metrics[nmetric_fw_io_def->index] = fw_io_get_err_count(nd->fw_io_ctx);
}

// Wrapper function for entry aggregate function
// The purpose of this function is to save out counters for processes that have stopped between
// data posts.
// Since NDS clears out after a process is terminated, we need to save out the counters on
// process termination to prevent us from losing metric data.
void nmetric_partial_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry)
{
	nmetric_aggregate_nd_counter_entry(nd, entry, nd->metrics.ds_freed_metrics_buf, &nd->metrics.ds_freed_feature_bitmap_buf,
	                                   nd->metrics.ds_freed_const_u64_buf, POST_TIME_ALWAYS);
}

/**
 * nmetric_mock_fw_io_post_metric() - Mock posting function used for internal testing
 *
 * @data: start of posting buffer
 * @size: size of posting buffer
 *
 */
static void nmetric_mock_fw_io_post_metric(u8 *data, u32 size)
{
	char temp_buf[NEURON_METRICS_VERSION_STRING_MAX_LEN + 1];
	struct nmetric_cw_metric *curr_metric;
	u8 *end_metric = data + size;
	while (data < end_metric) {
		curr_metric = (struct nmetric_cw_metric *)data;
		memcpy(temp_buf, curr_metric->data, curr_metric->len);
		temp_buf[curr_metric->len] = '\0'; // all metrics are saved as char arrays without trailing null char, so null char must be added
		trace_metrics_post(curr_metric->id, curr_metric->len, temp_buf);
		data += sizeof(struct nmetric_cw_metric) + curr_metric->len;
	}
}

/**
 * nmetric_post_version_with_max_usage()
 *
 * Writes the most used version (if it exists) to the post buffer, makes its usage 0, and returns bytes written
 *
 * @versions: versions to use
 * @metric: destination buffer
 * @available_size: available byte count
 * @cw_id: cloudwatch id
 * @add_fw_type: add framework type (appends version_info.reserved, which is the fw type, to the maj vers)
 * @return: bytes written
 *
 */
static int nmetric_post_version_with_max_usage(struct nmetric_versions *versions, struct nmetric_cw_metric *metric,
					       int available_size, int cw_id, bool add_fw_type)
{
	int idx;
	int found_idx;
	int version_len = 0; // length of the version string
	int metric_len = 0; // total length used in the metrics buffer by the current metric
	int written_len = 0; // total length used in the metrics buffer
	int fw_type = 0;
	int max_usage = 0;

	nmetric_version_t version_info;

	for (idx = 0; idx < NEURON_METRICS_VERSION_MAX_CAPACITY; idx++) {
		if (versions->version_usage_count[idx] > max_usage) {
			max_usage = versions->version_usage_count[idx];
			found_idx = idx;
		}
	}
	if (max_usage == 0)
		return 0;

	version_info.all = versions->version_metrics[found_idx];
	BUG_ON(version_info.all == 0);
	fw_type = (int)version_info.reserved % 10;
	if (fw_type == 0)
		add_fw_type = false;
	version_info.reserved = 0; // zero out .reserved to simplify the next comparison

	// Step 1: post version if not 0
	// In frameworkless mode the only non-zero value will be version_info.reserved (framework_type)
	// with a value of '1', and major_ver, minor_ver and build_num will all be 0, so don't post version,
	// only post framework_type - also make sure 0.0.0 is not posted in general when framework_type is not 0
	if (version_info.all != 0) {
		// check if there is enough space in buffer
		version_len = snprintf(NULL, 0, "%d.%d.%d", (int)version_info.major_ver,
				       (int)version_info.minor_ver, (int)version_info.build_num);

		metric_len = sizeof(struct nmetric_cw_metric) + version_len;

		if (metric_len <= available_size) {
			// save metrics to buffer
			metric->id = cw_id;
			metric->len = version_len; // null char will be replaced by next metric and should not be considered in the length
			snprintf(metric->data, version_len + 1, "%d.%d.%d", (int)version_info.major_ver, (int)version_info.minor_ver,
				 (int)version_info.build_num);

			written_len = metric_len;
		}
	}

	// Step 2: if required and not 0, also post the fw type
	if(add_fw_type) {
		metric_len = sizeof(struct nmetric_cw_metric) + 1;
		//save framework type to the next id
		if (written_len + metric_len <= available_size) {
			metric = (struct nmetric_cw_metric *)((void *)metric + written_len);
			metric->id = cw_id + 1;
			metric->len = 1;
			snprintf(metric->data, 2, "%d", fw_type);
			written_len += metric_len;
		}
	}

	versions->version_usage_count[found_idx] = 0;
	return written_len;
}

/* Functions for posting metric types (writing the metrics to the output buffer)
 */
static inline int nmetric_post_constant(const nmetric_def_t *metric, struct nmetric_cw_metric *dest, int available_size) {
	int const_len = strlen(nmetric_constant_metrics[metric->index]);
	int metric_size = sizeof(struct nmetric_cw_metric) + const_len;
	if (available_size < metric_size)
		return 0;
	// save metrics to buffer
	dest->id = metric->cw_id;
	dest->len = const_len;
	memcpy(dest->data, nmetric_constant_metrics[metric->index], const_len);
	return metric_size;
}

static inline int nmetric_post_version(struct nmetric_versions *versions, const nmetric_def_t *metric,
				       struct nmetric_cw_metric *dest, int available_size) {
	int idx;
	int size;
	int written_size = 0;
	int nmetric_cw_id_count = 1;
	bool add_fw_type = (metric->flags & NMETRIC_FLAG_VERS_ALLOW_TYPE) != 0;
	if (add_fw_type) {
		nmetric_cw_id_count = 2; // if type is added, then 2 cw ids will be used for every version post
	}
	for (idx = 0; idx < metric->count; idx++) {
		size = nmetric_post_version_with_max_usage(&versions[metric->index], dest,
							   available_size,
							   metric->cw_id + (idx * nmetric_cw_id_count),
							   add_fw_type);
		if (size == 0)
			continue;
		written_size += size;
		dest = (struct nmetric_cw_metric *)((void *)dest + size);
	}
	return written_size;
}

static inline int nmetric_post_counter(u64 *curr_metrics, u64 *prev_metrics,
				       u64 *freed_metrics, const nmetric_def_t *metric,
				       struct nmetric_cw_metric *dest, int available_size) {
	int metric_size;
	int expected_len;
	int metric_index = metric->index;
	u64 crt_metric_value = curr_metrics[metric_index] + freed_metrics[metric_index];
	u64 prev_metric_value = prev_metrics[metric_index];

	if (crt_metric_value <= prev_metric_value) { // on overflow or 0, we skip this one
		return 0;
	}

	crt_metric_value -= prev_metric_value;
	// check if there is enough space in buffer (if there's not, skip, maybe the next one fits)
	expected_len = snprintf(NULL, 0, "%llu", crt_metric_value);
	metric_size = sizeof(struct nmetric_cw_metric) + expected_len;
	if (available_size < metric_size)
		return 0;

	// save metrics to buffer
	dest->id = metric->cw_id;
	dest->len = expected_len;
	snprintf(dest->data, expected_len + 1, "%llu", crt_metric_value);

	return metric_size;
}

static inline int nmetric_post_feature_bitmap(const nmetric_def_t *metric, struct nmetric_cw_metric *dest,
											 u64 curr_feature_bitmap, u64 freed_feature_bitmap, int available_size)
	{
	u64 metric_value = curr_feature_bitmap | freed_feature_bitmap;

	// do not post the feature_bitmap if no feature is used
	if (metric_value == 0)
		return 0;

	// check if there is enough space in buffer
	int expected_len = snprintf(NULL, 0, "%llu", metric_value);
	int metric_size = sizeof(struct nmetric_cw_metric) + expected_len;
	if (available_size < metric_size)
		return 0;

	// save metrics to buffer
	dest->id = metric->cw_id;
	dest->len = expected_len;
	snprintf(dest->data, expected_len + 1, "%llu", metric_value); // post the feature_bitmap as decimal not hex, as cw reads it in decimal format

	return metric_size;
}

static int nmetric_post_u64(const nmetric_def_t *metric, u64 metric_value, struct nmetric_cw_metric *dest, int available_size)
{
	// check if there is enough space in buffer
	int expected_len = snprintf(NULL, 0, "%llu", metric_value);
	int metric_size = sizeof(struct nmetric_cw_metric) + expected_len;
	if (available_size < metric_size) {
		return 0;
	}

	// save metrics to buffer
	dest->id = metric->cw_id;
	dest->len = expected_len;
	snprintf(dest->data, expected_len + 1, "%llu", metric_value); // post the as decimal not hex, as cw reads it in decimal format

	return metric_size;
}

static inline int nmetric_post_constant_u64(const nmetric_def_t *metric, struct nmetric_cw_metric *dest, u64 *const_u64_metrics, u64 *freed_const_u64_metrics, int available_size)
{
	// we have a choice of taking the metric value from previous
	// NDS or current NDS.
	// For default flow, take current NDS value as preference.
	//
	// Change to backup NDS if there is NULL (0) data in the prefered NDS
	u64 *pref = const_u64_metrics;
	u64 *bak = freed_const_u64_metrics;
	if (metric->flags & NMETRIC_CONST_U64_FLAG_PREFER_FREED) {
		pref = freed_const_u64_metrics;
		bak = const_u64_metrics;
	}
	u64 metric_value = pref[metric->index];
	// do not post the constant if nothing is set
	if ((metric->flags & NMETRIC_CONST_U64_FLAG_SKIP_ZERO) != 0 && metric_value == 0) {
		metric_value = bak[metric->index];
		if (metric_value == 0)
			return 0;
	}

	return nmetric_post_u64(metric, metric_value, dest, available_size);
}

static inline int nmetric_post_driver_metrics(const nmetric_def_t *metric,
											  u64 *curr_metrics,
											  u64 *prev_metrics,
											  u64 *freed_metrics,
											  struct nmetric_cw_metric *dest,
											  u64 *driver_metrics,
											  int available_size)
{
	u64 metric_value = driver_metrics[metric->index];

	if (metric->index == NMETRIC_DRIVER_METRICS_IDX_MAX_DEVICE_RESET_TIME_MS
	  || metric->index == NMETRIC_DRIVER_METRICS_IDX_MAX_TPB_RESET_TIME_MS
	  || metric->index == NMETRIC_DRIVER_METRICS_IDX_AVG_DEVICE_RESET_TIME_MS
	  || metric->index == NMETRIC_DRIVER_METRICS_IDX_AVG_TPB_RESET_TIME_MS) {
		return nmetric_post_u64(metric, metric_value, dest, available_size);
	} else if (metric->index == NMETRIC_DRIVER_METRICS_IDX_DEVICE_RESET_FAILURE_COUNT
			|| metric->index == NMETRIC_DRIVER_METRICS_IDX_TPB_RESET_FAILURE_COUNT){
		return nmetric_post_counter(curr_metrics, prev_metrics, freed_metrics, metric, dest, available_size);		
	}

	return 0;
}

/**
 * nmetric_post_metrics()
 *
 * Sends a byte array of metrics in string form to fw. Differential counter metrics are sent (as compared to the last posting);
 * counter metrics with 0 difference from last posting are not posted. Extremely large counter metrics may be truncated and will log an error.
 * Multiple version metrics may be posted at once up to a predefined limit, versions beyond this limit will be discarded.
 *
 * @nd: neuron device
 * @curr_metrics: buffer containing metrics of the current session not yet posted to fw
 * @prev_metrics: buffer containing metrics of the previous session, last posted
 * @freed_metrics: buffer containing metrics that were freed before being posted in the current session and not captured in current metrics buf
 * @versions: buffer containing version metrics gathered from the current session
 * @constants_metrics: buffer containing metrics constant to the device
 * @curr_feature_bitmap: buffer containing feature_bitmap of the current session not yet posted to fw
 * @freed_feature_bitmap: buffer containing feature_bitmap that were freed before being posted in the current session and not captured in current feature_bitmap
 *
 */
static void nmetric_post_metrics(struct neuron_device *nd, u64 *curr_metrics, u64 *prev_metrics, u64 *freed_metrics,
				 struct nmetric_versions *versions, u64 curr_feature_bitmap, u64 freed_feature_bitmap, u64 *const_u64_metrics, u64 *freed_const_u64_metrics, u8 tick)
{
	int available_size;
	int nmetric_index;
	const nmetric_def_t *curr_metric;
	struct nmetric_cw_metric *dest;
	int data_size = 0;

	for (nmetric_index = 0; nmetric_index < nmetric_count; nmetric_index++) {
		curr_metric = &nmetric_defs[nmetric_index];
		if (!nmetric_check_post_tick(tick, curr_metric))
			continue;
		available_size = NEURON_METRICS_MAX_POSTING_BUF_SIZE - data_size;
		dest = (struct nmetric_cw_metric *)&nd->metrics.posting_buffer[data_size];
		switch(curr_metric->type) {
		case NMETRIC_TYPE_CONSTANT:
			data_size += nmetric_post_constant(curr_metric, dest, available_size);
		break;
		case NMETRIC_TYPE_VERSION:
			data_size += nmetric_post_version(versions, curr_metric, dest, available_size);
		break;
		case NMETRIC_TYPE_COUNTER:
		case NMETRIC_TYPE_FW_IO_ERR:
			data_size += nmetric_post_counter(curr_metrics, prev_metrics, freed_metrics,
							  curr_metric, dest, available_size);
		break;
		case NMETRIC_TYPE_BITMAP:
			data_size += nmetric_post_feature_bitmap(curr_metric, dest, curr_feature_bitmap, freed_feature_bitmap, available_size);
		break;
		case NMETRIC_TYPE_CONSTANT_U64:
			data_size += nmetric_post_constant_u64(curr_metric, dest, const_u64_metrics, freed_const_u64_metrics, available_size);
		break;
		case NMETRIC_TYPE_DRIVER:
			data_size += nmetric_post_driver_metrics(curr_metric, curr_metrics, prev_metrics, freed_metrics, dest, nd->metrics.driver_metrics, available_size);
		break;
		}
	}

	// post metrics if available
	//
	if (nmetric_log_posts & (1<<1)) {
		nmetric_mock_fw_io_post_metric(nd->metrics.posting_buffer, data_size);
	}
	if (data_size && (nmetric_log_posts & (1<<0))) {
		int ret = fw_io_post_metric(nd->fw_io_ctx, nd->metrics.posting_buffer, data_size);
		if (ret < 0)
			pr_err("Metric posting failed with error code: %d\n", ret);
	}
}

/**
 *
 * nmetric_cache_shared_bufs() - Caches neuron device buffer values to avoid needing extra locks
 *
 * @nd: neuron device
 * @freed_metrics[out]: will contain freed counter data copied from neuron device aggregation
 * @versions[out]: will contain version metrics data copied from neuron device aggregation
 * @freed_feature_bitmap: will contain freed feature_bitmap metrics data copied from neuron device aggregation
 * @tick: current tick value
 */
static void nmetric_cache_shared_bufs(struct neuron_device *nd, u64 *freed_metrics, struct nmetric_versions *versions, u64 *freed_feature_bitmap, u64 *freed_const_u64_metrics, u8 tick)
{
	int nmetric_index;
	const nmetric_def_t *curr_metric;

	// cache and reset freed metrics buf
	memcpy(freed_metrics, nd->metrics.ds_freed_metrics_buf, nmetric_counters_buf_size);
	// cache and reset version metrics buf
	memcpy(versions, nd->metrics.component_versions, nmetric_versions_buf_size);
	// cache and reset feature_bitmap metrics buf
	*freed_feature_bitmap = nd->metrics.ds_freed_feature_bitmap_buf;
	nd->metrics.ds_freed_feature_bitmap_buf = 0;

	// IMPORTANT MUST USE THIS LOOP TO RESET EVERYTHING.
	// IF NOT A DIFFERENT TICK WILL SAVE OFF THINGS AND RESET FOR YOU AND
	// YOU DO NOT POST
	//
	// TODO: Fix feature bitmap since that resets even if it is not posted
	// and to keep "versions" and the counters consistent, add them into the loop
	// as well.
	for (nmetric_index = 0; nmetric_index < nmetric_count; nmetric_index++) {
		curr_metric = &nmetric_defs[nmetric_index];
		if (!nmetric_check_post_tick(tick, curr_metric))
			continue;
		switch(curr_metric->type) {
		case NMETRIC_TYPE_VERSION:
			memset(&nd->metrics.component_versions[curr_metric->index], 0, sizeof(struct nmetric_versions));
		break;
		case NMETRIC_TYPE_COUNTER:
		case NMETRIC_TYPE_FW_IO_ERR:
			nd->metrics.ds_freed_metrics_buf[curr_metric->index] = 0;
		break;
		case NMETRIC_TYPE_CONSTANT_U64:
			freed_const_u64_metrics[curr_metric->index] = nd->metrics.ds_freed_const_u64_buf[curr_metric->index];
			nd->metrics.ds_freed_const_u64_buf[curr_metric->index] = 0;
		break;
		}
	}
}

/**
 * nmetric_start_new_session() - Copies metrics in the buffer of the current session to the reference buffer, resets all buffers containing metrics of the current session
 *
 * @curr_metrics: buffer containing metrics of the current session
 * @prev_metrics: reference buffer
 * @freed_metrics: cache of buffer containing metrics of freed datastore entries
 * @curr_feature_bitmap: buffer containing feature_bitmap of the current session
 * @freed_feature_bitmap: cache of buffer containing feature_bitmap from freed datastore
 * @tick: current tick value
 *
 */
static void nmetric_start_new_session(struct neuron_device *nd, u64 *curr_metrics, u64 *prev_metrics, u64 *freed_metrics, u64 *curr_feature_bitmap, u64 *freed_feature_bitmap, u64 *const_u64_metrics, u64 *freed_const_u64_metrics, u8 tick)
{
	int nmetric_index;
	const nmetric_def_t *curr_metric;

	// IMPORTANT MUST USE THIS LOOP TO START NEW SESSION.
	// IF NOT, YOU WILL MESS WITH DATA ON A DIFFERENT TICK
	//
	// TODO: Fix feature bitmap

	// save metrics to reference array
	for (nmetric_index = 0; nmetric_index < nmetric_count; nmetric_index++) {
		curr_metric = &nmetric_defs[nmetric_index];
		if (!nmetric_check_post_tick(tick, curr_metric))
			continue;
		switch(curr_metric->type) {
			case NMETRIC_TYPE_COUNTER:
				prev_metrics[curr_metric->index] = curr_metrics[curr_metric->index];
			break;
			case NMETRIC_TYPE_CONSTANT_U64:
				const_u64_metrics[curr_metric->index] = 0;
				freed_const_u64_metrics[curr_metric->index] = 0;
			break;
			case NMETRIC_TYPE_DRIVER:
				nd->metrics.driver_metrics[curr_metric->index] = 0;
			break;
		}
	}

	// reset all current metrics
	memset(curr_metrics, 0, nmetric_counters_buf_size);
	memset(freed_metrics, 0, nmetric_counters_buf_size);

	// reset feature_bitmap metrics
	*curr_feature_bitmap = 0;
	*freed_feature_bitmap = 0;
}

/**
 * nmetric_sample_high_freq() - sample metrics that operate at a higher frequency than most
 */
static void nmetric_sample_high_freq(struct neuron_device *nd)
{
	npower_sample_utilization(nd);
}

/**
 * nmetric_thread_fn() - periodically aggregates and posts metric at rate specified by module parameter
 *
 * @arg: expected to be a pointer to neuron device
 *
 */
static int nmetric_thread_fn(void *arg)
{
	struct neuron_device *nd = (struct neuron_device *)arg;
	struct nmetric_versions component_versions[NMETRIC_VERSION_COUNT];
	u64 curr_feature_bitmap;  // feature_bitmap for the current session
	u64 freed_feature_bitmap; // cache hold the feature_bitmap that was freed before the posting period was reached
	u64 const_u64_metrics[NMETRIC_CONSTANT_U64_COUNT];
	u64 freed_const_u64_metrics[NMETRIC_CONSTANT_U64_COUNT];
	u8 tick = 0;
	u64 sample_delay_in_jiffies;
	u64 post_delay_in_jiffies;
	u64 last_metric_post_time;
	u64 start_jiffies = jiffies;
	u64 last_logged_slow_tick = 0;
	u64 current_slow_tick;

	// initialize all aggregation buffers
	memset(nd->metrics.neuron_aggregation.prev, 0, nmetric_counters_buf_size);
	memset(nd->metrics.neuron_aggregation.curr, 0, nmetric_counters_buf_size);
	memset(nd->metrics.neuron_aggregation.freed, 0, nmetric_counters_buf_size);
	memset(component_versions, 0, nmetric_versions_buf_size);
	curr_feature_bitmap = 0;
	freed_feature_bitmap = 0;
	memset(const_u64_metrics, 0, NMETRIC_CONSTANT_U64_COUNT * sizeof(u64));
	memset(freed_const_u64_metrics, 0, NMETRIC_CONSTANT_U64_COUNT * sizeof(u64));

	sample_delay_in_jiffies = msecs_to_jiffies(nmetric_metric_sample_delay);
	post_delay_in_jiffies = msecs_to_jiffies(nmetric_metric_post_delay);
	last_metric_post_time = jiffies;

	pr_info("Starting metrics thread, sample_delay_in_jiffies is %llu, post delay in ms is %u, timer rate = %d, \n",
		sample_delay_in_jiffies, nmetric_metric_post_delay, HZ);

	// metrics are only sent once at rate specified by module param, new metric data may be saved without being immediately sent
	while (!kthread_should_stop() && nd->metrics.neuron_aggregation.running) {
	    long wait_return;
		wait_return = wait_event_interruptible_timeout(nd->metrics.neuron_aggregation.wait_queue, !nd->metrics.neuron_aggregation.running,sample_delay_in_jiffies);

		if (kthread_should_stop() || !nd->metrics.neuron_aggregation.running || (wait_return < 0)) {
			break;
		};

		// There are some metrics that we sample at a relatively higher frequency.  Do that here.
		nmetric_sample_high_freq(nd);

		// For the slower metrics, we want to log once every post_delay_in_jiffies jiffies.
		// We track this by keeping track of the number of intervals since this thread started
		// up so that we don't introduce drift due to the latency of other loop operations.
		current_slow_tick = (jiffies - start_jiffies)/post_delay_in_jiffies;
		if (current_slow_tick != last_logged_slow_tick) {
			last_logged_slow_tick = current_slow_tick;

			// aggregate and post metrics
			neuron_ds_acquire_lock(&nd->datastore);
			nmetric_full_aggregate(nd, nd->metrics.neuron_aggregation.curr,
					       &curr_feature_bitmap, const_u64_metrics, tick);
			nmetric_cache_shared_bufs(nd, nd->metrics.neuron_aggregation.freed,
						  component_versions, &freed_feature_bitmap,
						  freed_const_u64_metrics, tick);
			neuron_ds_release_lock(&nd->datastore);

			nmetric_post_metrics(nd, nd->metrics.neuron_aggregation.curr,
					     nd->metrics.neuron_aggregation.prev,
					     nd->metrics.neuron_aggregation.freed,
					     component_versions, curr_feature_bitmap,
					     freed_feature_bitmap, const_u64_metrics,
					     freed_const_u64_metrics, tick);
			nmetric_start_new_session(nd, nd->metrics.neuron_aggregation.curr,
						  nd->metrics.neuron_aggregation.prev,
						  nd->metrics.neuron_aggregation.freed,
						  &curr_feature_bitmap, &freed_feature_bitmap,
						  const_u64_metrics, freed_const_u64_metrics,
						  tick); // reset all current metrics for this tick
			tick = (tick + 1) % POST_TICK_COUNT;
		}
	}

	return 0;
}

static int nmetric_create_thread(struct neuron_device *nd)
{
	init_waitqueue_head(&nd->metrics.neuron_aggregation.wait_queue);
	nd->metrics.neuron_aggregation.running = true;
	nd->metrics.neuron_aggregation.thread = kthread_run(nmetric_thread_fn, nd, "nd%d metrics", nd->device_index);
	if (IS_ERR_OR_NULL(nd->metrics.neuron_aggregation.thread)) {
		pr_err("nd%d metrics aggregation thread creation failed\n", nd->device_index);
		return -1;
	}
	return 0;
}

void nmetric_stop_thread(struct neuron_device *nd)
{
	if (nd->metrics.neuron_aggregation.thread == NULL)
		return;
	nd->metrics.neuron_aggregation.running = false;
	wake_up(&nd->metrics.neuron_aggregation.wait_queue);
	kthread_stop(nd->metrics.neuron_aggregation.thread); //blocks till the thread exits
	nd->metrics.neuron_aggregation.thread = NULL;
}

void nmetric_init_driver_metrics(struct neuron_device *nd)
{
	memset(nd->metrics.driver_metrics, 0, NMETRIC_DRIVER_METRICS_COUNT * sizeof(u64));
}

int nmetric_init(struct neuron_device *nd)
{
	int ret;

	memset(nd->metrics.ds_freed_metrics_buf, 0, nmetric_counters_buf_size);
	memset(nd->metrics.ds_freed_const_u64_buf, 0, NMETRIC_CONSTANT_U64_COUNT * sizeof(u64));
	npower_init_stats(nd);

	// initiate metric aggregator thread
	ret = nmetric_create_thread(nd);

	return ret;
}

void nmetric_set_reset_time_metrics(struct neuron_device *nd, uint64_t cur_reset_time_ms, bool is_device_reset) {
	if (cur_reset_time_ms <= 0) {
		return;
	}

	if (is_device_reset) {
		if (nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_MAX_DEVICE_RESET_TIME_MS] < cur_reset_time_ms) {
			nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_MAX_DEVICE_RESET_TIME_MS] = cur_reset_time_ms;
		}
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_DEVICE_RESET_TIME_MS] += cur_reset_time_ms;
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_DEVICE_RESET_COUNT]++;
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_AVG_DEVICE_RESET_TIME_MS] = 
		  nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_DEVICE_RESET_TIME_MS] / 
		  nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_DEVICE_RESET_COUNT];
	} else {
		if (nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_MAX_TPB_RESET_TIME_MS] < cur_reset_time_ms) {
			nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_MAX_TPB_RESET_TIME_MS] = cur_reset_time_ms;
		}
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_TPB_RESET_TIME_MS] += cur_reset_time_ms;
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_TPB_RESET_COUNT]++;
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_AVG_TPB_RESET_TIME_MS] = 
		  nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_TPB_RESET_TIME_MS] / 
		  nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TOTAL_TPB_RESET_COUNT];
	}
}

void nmetric_increment_reset_failure_count(struct neuron_device *nd, bool is_device_reset)
{
	if (is_device_reset) {
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_DEVICE_RESET_FAILURE_COUNT]++;
	} else {
		nd->metrics.driver_metrics[NMETRIC_DRIVER_METRICS_IDX_TPB_RESET_FAILURE_COUNT]++;
	}
}
