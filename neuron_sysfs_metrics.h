/*
 * Copyright 2022, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef NEURON_SYSFS_METRICS_H
#define NEURON_SYSFS_METRICS_H

#include <linux/device.h>

#define MAX_CHILD_NODES_NUM	    32
#define NON_NDS_COUNTER_COUNT   64
#define MAX_METRIC_ID           (NDS_ND_COUNTER_COUNT + NDS_EXT_NC_COUNTER_LAST + NON_NDS_COUNTER_COUNT)
#define MAX_COUNTER_ATTR_TYPE_COUNT	3

#define NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(nds_id) (nds_id)
#define NDS_ND_COUNTER_ID_TO_SYSFS_METRIC_ID(nds_id) (nds_id + NDS_EXT_NC_COUNTER_LAST)
#define NON_NDS_ID_TO_SYSFS_METRIC_ID(non_nds_id)    (non_nds_id + NDS_ND_COUNTER_COUNT + NDS_EXT_NC_COUNTER_LAST)

#define ATTR_INFO(_attr_name, _metric_id, _attr_type) { \
    .attr_name = _attr_name,                            \
    .metric_id = _metric_id,                            \
    .attr_type = _attr_type                             \
}

enum nsysfsmetric_attr_type {
    TOTAL,     // counter value accumulated
    PRESENT,   // counter value at the current window
    PEAK,      // max counter value
    OTHER,     // all other types besides TOTAL, PRESENT, and PEAK
};

enum nsysfsmetric_metric_id_category {
    NDS_NC_METRIC,
    NDS_ND_METRIC,
    NON_NDS_METRIC,
};

enum nsysfsmetric_non_nds_ids { // The metrics needed by sysfs metrics but not stored in datastore
	NON_NDS_COUNTER_HOST_MEM,
	NON_NDS_COUNTER_DEVICE_MEM,

	NON_NDS_ND_COUNTER_MEM_USAGE_UNCATEGORIZED_HOST, // for old runtimes only
	NON_NDS_ND_COUNTER_MEM_USAGE_CODE_HOST,
	NON_NDS_ND_COUNTER_MEM_USAGE_TENSORS_HOST,
	NON_NDS_ND_COUNTER_MEM_USAGE_CONSTANTS_HOST,
	NON_NDS_ND_COUNTER_MEM_USAGE_MISC_HOST,
	NON_NDS_ND_COUNTER_MEM_USAGE_NCDEV_HOST,
	NON_NDS_ND_COUNTER_MEM_USAGE_NOTIFICATION_HOST,
	NON_NDS_ND_COUNTER_MEM_USAGE_DMA_RINGS_HOST,

	NON_NDS_NC_COUNTER_MEM_USAGE_UNCATEGORIZED_DEVICE, // for old runtimes only
	NON_NDS_NC_COUNTER_MEM_USAGE_CODE_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_TENSORS_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_CONSTANTS_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_SCRATCHPAD_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_MISC_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_NCDEV_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_COLLECTIVES_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_SCRATCHPAD_NONSHARED_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_NOTIFICATION_DEVICE,
	NON_NDS_NC_COUNTER_MEM_USAGE_DMA_RINGS_DEVICE,

	NON_NDS_COUNTER_RESET_REQ_COUNT,
	NON_NDS_COUNTER_RESET_FAIL_COUNT,
	NON_NDS_COUNTER_MODEL_LOAD_COUNT,
	NON_NDS_COUNTER_INFERENCE_COUNT,
	NON_NDS_COUNTER_ECC_SRAM_UNCORRECTED,
	NON_NDS_COUNTER_ECC_HBM_UNCORRECTED,
	NON_NDS_COUNTER_ECC_REPAIRABLE_HBM_UNCORRECTED,
	NON_NDS_COUNTER_PE_ARRAY_ACTIVITY,
	NON_NDS_OTHER_NEURON_ARCH_TYPE,
	NON_NDS_OTHER_NEURON_INSTANCE_TYPE,
	NON_NDS_OTHER_NEURON_DEVICE_NAME,
	NON_NDS_OTHER_NOTIFY_DELAY,
	NON_NDS_OTHER_SERIAL_NUMBER,
	NON_NDS_OTHER_POWER_UTILIZATION,
};

struct neuron_device;

struct sysfs_mem_thread {
	struct task_struct *thread; // aggregation thread that sends metrics every 1 second
	wait_queue_head_t wait_queue;
	volatile bool stop; // if cleared, thread would exit the loop
};

struct nsysfsmetric_counter {
    struct nsysfsmetric_node *node; // used for sysfs_notify
    u64 total;
    u64 present;
    u64 peak;
};

struct nsysfsmetric_node { // represent a subdirectory in sysfs
    struct kobject kobj;
    struct mutex lock;
    bool is_root;
    int child_node_num;
    struct nsysfsmetric_node *child_nodes[MAX_CHILD_NODES_NUM];
    struct attribute_group *attr_group;
};

struct nsysfsmetric_metrics { // per neuron_device
    struct nsysfsmetric_node root; // represent the neuron device
    struct nsysfsmetric_node *dynamic_metrics_dirs[MAX_NC_PER_DEVICE];
    struct nsysfsmetric_counter nrt_metrics[MAX_METRIC_ID][MAX_NC_PER_DEVICE]; // runtime metrics per NC, indiced by metric_id and nc_id
    struct nsysfsmetric_counter nrt_nd_metrics[MAX_METRIC_ID]; // runtime metrics for whole ND, indiced by metric_id
    // nc_id should be -1 to use nrt_nd_metrics, and should be a valid neuron core ID to use nrt_metrics
    struct nsysfsmetric_counter dev_metrics[MAX_METRIC_ID]; // TODO: the device metrics
    uint64_t bitmap; // store the dynamic metrics to be added
};

typedef struct nsysfsmetric_attr_info {
    char *attr_name;
    int metric_id;
    int attr_type;
} nsysfsmetric_attr_info_t;

typedef struct nsysfsmetric_counter_node_info {
    char *node_name;
    int metric_id;
    int attr_cnt;
    nsysfsmetric_attr_info_t attr_info_tbl[MAX_COUNTER_ATTR_TYPE_COUNT]; // present, total, and/or peak
} nsysfsmetric_counter_node_info_t;

/**
 * nsysfsmetric_register() - Perform various sysfs inits such as kobj init and attribute group creation per neuron device
 *
 * @nd: The pointer to the device which is the parent of the kobj to be initialized 
 * @nd_kobj: The pointer to the parent kobject of nd->sysfs_metrics.metric_kobj
 */
int nsysfsmetric_register(struct neuron_device *nd, struct kobject *nd_kobj);

/**
 * nsysfsmetric_destroy() - Clean up memory allocated for kobj and remove attribute group
 *
 * @param nd: The pointer to the neuron_device
 */
void nsysfsmetric_destroy(struct neuron_device *nd);

/**
 * nsysfsmetric_init_and_add_one_node() - add one node to sysfs
 * 
 * @return struct nsysfsmetric_node* the newly created not
 */
struct nsysfsmetric_node *nsysfsmetric_init_and_add_one_node(struct nsysfsmetric_metrics *sysfs_metrics,
                                                            struct nsysfsmetric_node *parent_node,
                                                            const char *node_name,
                                                            bool is_root,
                                                            int nc_id,
                                                            int attr_info_tbl_cnt,
                                                            const nsysfsmetric_attr_info_t *attr_info_tbl);

/**
 * nsysfsmetric_init_and_add_dynamic_counter_nodes() - add all new dynamic metrics requested by runtime under each neuron device directory
 * 
 * @param nd: The pointer to the neuron_device
 * @param ds_val: the value from datastore to be aggregated with the current bitmap
 */
int nsysfsmetric_init_and_add_dynamic_counter_nodes(struct neuron_device *nd, uint64_t ds_val);

/**
 * nsysfsmetric_nds_aggregate() - Aggregate sysfs metrics from datastore when a process exits
 * 
 * @param nd: The pointer to the neuron_device
 * @param entry: : The pointer to the datastore entry
 */
void nsysfsmetric_nds_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry);

/**
 * nsysfsmetric_inc_counter() - Increment the counter with metric_id for neuron_device nd and neuron core nc_id by delta
 * 
 * @param nd: The pointer to the neuron_device
 * @param metric_id_category: one of the three metric categories (NDS_NC_METRIC, NON_NDS_METRIC, NON_NDS_METRIC)
 * @param id: the index that represents the counter. It can be a ds id or non ds id
 * @param nc_id: the neuron core id (or -1 if it is a counter for whole ND)
 * @param delta: the amount to be incremented
 * @param acquire_lock: whether to acquire lock while incrementing counter. It must be true except as an
 * optimization when all calls to nsysfsmetric_[inc/dec/set]_counter for a counter are already protected by a lock.
 */
void nsysfsmetric_inc_counter(struct neuron_device *nd, int metric_id_category, int id, int nc_id, u64 delta, bool acquire_lock);

/**
 * nsysfsmetric_dec_counter() - Decrement the counter with metric_id for neuron_device nd and neuron core nc_id by delta
 * 
 * @param nd: The pointer to the neuron_device
 * @param metric_id_category: one of the three metric categories (NDS_NC_METRIC, NON_NDS_METRIC, NON_NDS_METRIC)
 * @param id: the index that represents the counter. It can be a ds id or non ds id
 * @param nc_id: the neuron core id (or -1 if it is a counter for whole ND)
 * @param delta: the amount to be decremented
 * @param acquire_lock: whether to acquire lock while decrementing counter. It must be true except as an
 * optimization when all calls to nsysfsmetric_[inc/dec/set]_counter for a counter are already protected by a lock.
 */
void nsysfsmetric_dec_counter(struct neuron_device *nd, int metric_id_category, int id, int nc_id, u64 delta, bool acquire_lock);

/**
 * nsysfsmetric_inc_reset_req_count() - Increment the RESET_COUNT metrics
 * 
 * @param nd: The pointer to the neuron_device 
 * @param nc_id: the neuron core id (or -1 if it is a counter for whole ND)
 */
void nsysfsmetric_inc_reset_req_count(struct neuron_device *nd, int nc_id);

/**
 * nsysfsmetric_set_counter() - Set the counter with metric_id for neuron_device nd and neuron core nc_id to val
 *
 * @param nd: The pointer to the neuron_device
 * @param metric_id_category: one of the three metric categories (NDS_NC_METRIC, NON_NDS_METRIC, NON_NDS_METRIC)
 * @param id: the index that represents the counter. It can be a ds id or non ds id
 * @param nc_id: the neuron core id (or -1 if it is a counter for whole ND)
 * @param val: the value the counter should be set to
 * @param acquire_lock: whether to acquire lock while setting counter. It must be true except as an
 * optimization when all calls to nsysfsmetric_[inc/dec/set]_counter for a counter are already protected by a lock.
 */
void nsysfsmetric_set_counter(struct neuron_device *nd, int metric_id_category, int metric_id, int nc_id, u64 val, bool acquire_lock);

/**
 * nsysfsmetric_inc_reset_fail_count() - Increment the NON_NDS_COUNTER_RESET_FAIL_COUNT metrics
 * 
 * @param nd: The pointer to the neuron_device
 */
void nsysfsmetric_inc_reset_fail_count(struct neuron_device *nd);


#endif
