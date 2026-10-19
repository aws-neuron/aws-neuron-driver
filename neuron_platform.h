// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_PLATFORM_H
#define NEURON_PLATFORM_H

#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/types.h>

#include "share/neuron_driver_shared.h"

struct neuron_device;

enum neuron_platform_type {
	NEURON_PLATFORM_TYPE_STD = 0,
	NEURON_PLATFORM_TYPE_ULTRASERVER = 1,
	NEURON_PLATFORM_TYPE_PDS = 2,
	NEURON_PLATFORM_TYPE_MAX = 3,
	NEURON_PLATFORM_TYPE_INVALID,
};

enum neuron_platform_operation_type {
	NEURON_PLATFORM_OP_TYPE_DEVOPEN = 0,
	NEURON_PLATFORM_OP_TYPE_EXEC = 1,
};

enum neuron_platform_routing_type {
	NEURON_PLATFORM_ROUTING_TYPE_INVALID = 0,
	NEURON_PLATFORM_ROUTING_TYPE_SEQUENTIAL,
	NEURON_PLATFORM_ROUTING_TYPE_TABLE,
};

enum neuron_pod_discovery_type {
	NEURON_POD_DISCOVERY_TYPE_NONE = 0,
	NEURON_POD_DISCOVERY_TYPE_ELECTION,
	NEURON_POD_DISCOVERY_TYPE_FW_IO,
};

enum neuron_platform_family {
	NEURON_PLATFORM_FAMILY_NONE = 0,
	NEURON_PLATFORM_FAMILY_INF2,
	NEURON_PLATFORM_FAMILY_TRN1,
	NEURON_PLATFORM_FAMILY_TRN1N,
	NEURON_PLATFORM_FAMILY_TRN2,
	NEURON_PLATFORM_FAMILY_TRN2_ULTRASERVER,
	NEURON_PLATFORM_FAMILY_TRN3_GEN1_ULTRASERVER,
	NEURON_PLATFORM_FAMILY_TRN3_GEN2_ULTRASERVER,
};

enum neuron_soc_sub_version {
	NEURON_SOC_SUB_VERSION_A0 = 0,
	NEURON_SOC_SUB_VERSION_B1 = 1,
};

enum neuron_intra_rack_topology {
	NEURON_INTRA_RACK_TOPOLOGY_NONE = 0,
	NEURON_INTRA_RACK_TOPOLOGY_RING,
	NEURON_INTRA_RACK_TOPOLOGY_TORUS,
	NEURON_INTRA_RACK_TOPOLOGY_SWITCH,
};

enum neuron_inter_rack_topology {
	NEURON_INTER_RACK_TOPOLOGY_NONE = 0,
	NEURON_INTER_RACK_TOPOLOGY_P2P,
};

enum neuron_switch_type {
	NEURON_SWITCH_TYPE_NONE = 0,
	NEURON_SWITCH_TYPE_V1,
	NEURON_SWITCH_TYPE_V2,
};

struct neuron_intra_rack_interconnect {
	enum neuron_intra_rack_topology topology;
	enum neuron_switch_type switch_type;
};

struct neuron_inter_rack_interconnect {
	enum neuron_inter_rack_topology topology;
	enum neuron_switch_type switch_type;
};

struct neuron_interconnect_info {
	struct neuron_intra_rack_interconnect intra_rack;
	struct neuron_inter_rack_interconnect inter_rack;
};

struct neuron_efa_bdf_map {
	const char *const *bdfs;
	u16 count;
};

struct neuron_platform_routing_config {
	enum neuron_platform_routing_type type;
	union {
		struct {
			const u32 *map;
			u32 size;
		} table;
	};
};

struct neuron_platform_desc {
	const char *const *instance_names;
	enum neuron_platform_type type;
	enum neuron_platform_family family;

	struct neuron_platform_routing_config routing_config;
	u8 device_count;
	u8 default_node_cnt;
	enum neuron_pod_type pod_type;
	enum neuron_pod_discovery_type pod_discovery;
	u8 max_bars;
	struct neuron_interconnect_info interconnect;
	struct neuron_efa_bdf_map nc_efa_bdf_tbl;
	u8 num_sleds_per_server;
	u8 num_chips_per_sled;
};

extern const char *const trn1_nc_efa_bdf_tbl[32];
extern const char *const trn1n_nc_efa_bdf_tbl[32];
extern const char *const trn2_nc_efa_bdf_tbl[128];
extern const char *const trn2n_nc_efa_bdf_tbl[128];
extern const char *const trn3pd_nc_efa_bdf_tbl[128];
extern const char *const trn3pds_nc_efa_bdf_tbl[128];

/**
 * nplatform_get_platform_desc() - Look up the platform descriptor for this instance.
 *
 * @table: Per-arch table of platform descriptors
 *
 * Return: Matching platform descriptor
 */
const struct neuron_platform_desc *nplatform_get_platform_desc(const struct neuron_platform_desc *table);

extern const struct attribute_group neuron_soc_group;
extern const struct attribute_group efa_group;
extern const struct attribute_group platform_group;

/**
 * nplatform_sysfs_add_efa_symlink() - Create neuron_coreN/info/efa_device symlink to the paired EFA PCI device.
 *
 * @nd:      Neuron device the neuron-core belongs to.
 * @nc_kobj: Kobject of the neuron_coreN/info directory to attach the symlink to.
 * @nc_id:   Neuron-core index within the device.
 *
 * Return: 0 on success or when the platform doesn't have efa support
 * Negative errno on failure:
 *   -ENOENT: missing BDF entry
 *   -EINVAL: malformed BDF string
 *   -ENODEV: EFA PCI device not enumerated
 *   or the return value from sysfs_create_link().
 */
int nplatform_sysfs_add_efa_symlink(struct neuron_device *nd, struct kobject *nc_kobj, int nc_id);

#endif // NEURON_PLATFORM_H

