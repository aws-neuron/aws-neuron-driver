// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include "neuron_arch.h"
#include "neuron_device.h"
#include "neuron_dhal.h"
#include "neuron_pci.h"
#include "neuron_platform.h"

static const char *const family_names[] = {
	[NEURON_PLATFORM_FAMILY_NONE]                  = "none",
	[NEURON_PLATFORM_FAMILY_INF2]                  = "inf2",
	[NEURON_PLATFORM_FAMILY_TRN1]                  = "trn1",
	[NEURON_PLATFORM_FAMILY_TRN1N]                 = "trn1n",
	[NEURON_PLATFORM_FAMILY_TRN2]                  = "trn2",
	[NEURON_PLATFORM_FAMILY_TRN2_ULTRASERVER]      = "trn2_ultraserver",
	[NEURON_PLATFORM_FAMILY_TRN3_GEN1_ULTRASERVER] = "trn3_gen1_ultraserver",
	[NEURON_PLATFORM_FAMILY_TRN3_GEN2_ULTRASERVER] = "trn3_gen2_ultraserver",
};
static_assert(ARRAY_SIZE(family_names) == NEURON_PLATFORM_FAMILY_TRN3_GEN2_ULTRASERVER + 1,
	      "family_names out of sync with enum neuron_platform_family");

static const char *const intra_topology_names[] = {
	[NEURON_INTRA_RACK_TOPOLOGY_NONE]   = "none",
	[NEURON_INTRA_RACK_TOPOLOGY_RING]   = "ring",
	[NEURON_INTRA_RACK_TOPOLOGY_TORUS]  = "torus",
	[NEURON_INTRA_RACK_TOPOLOGY_SWITCH] = "switch",
};
static_assert(ARRAY_SIZE(intra_topology_names) == NEURON_INTRA_RACK_TOPOLOGY_SWITCH + 1,
	      "intra_topology_names out of sync with enum neuron_intra_rack_topology");

static const char *const inter_topology_names[] = {
	[NEURON_INTER_RACK_TOPOLOGY_NONE] = "none",
	[NEURON_INTER_RACK_TOPOLOGY_P2P]  = "p2p",
};
static_assert(ARRAY_SIZE(inter_topology_names) == NEURON_INTER_RACK_TOPOLOGY_P2P + 1,
	      "inter_topology_names out of sync with enum neuron_inter_rack_topology");

static const char *const switch_type_names[] = {
	[NEURON_SWITCH_TYPE_NONE] = "none",
	[NEURON_SWITCH_TYPE_V1]   = "v1",
	[NEURON_SWITCH_TYPE_V2]   = "v2",
};
static_assert(ARRAY_SIZE(switch_type_names) == NEURON_SWITCH_TYPE_V2 + 1,
	      "switch_type_names out of sync with enum neuron_switch_type");

static const char *lookup_name(const char *const *tbl, size_t tbl_sz, unsigned int idx)
{
	if (idx >= tbl_sz || !tbl[idx])
		return "none";
	return tbl[idx];
}

#if (!defined(RHEL_RELEASE_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))) || (defined(RHEL_RELEASE_CODE) && (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 5)))
#define NPLATFORM_CLASS_SHOW_ARGS const struct class *cls, const struct class_attribute *attr
#else
#define NPLATFORM_CLASS_SHOW_ARGS struct class *cls, struct class_attribute *attr
#endif

static ssize_t soc_version_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	if (!ndhal)
		return 0;
	return dhal_sysfs_emit(buf, "%d\n", ndhal->ndhal_arch.arch);
}

static ssize_t sub_version_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	unsigned int sub_version = NEURON_SOC_SUB_VERSION_A0;

	if (!ndhal)
		return 0;
	if (ndhal->ndhal_arch.arch == NEURON_ARCH_V4 && narch_get_revision() >= 0x02)
		sub_version = NEURON_SOC_SUB_VERSION_B1;
	return dhal_sysfs_emit(buf, "%u\n", sub_version);
}
static CLASS_ATTR_RO(sub_version);

static ssize_t hbm_capacity_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	if (!ndhal)
		return 0;
	return dhal_sysfs_emit(buf, "%llu\n",
			       ndhal->ndhal_address_map.hbm_active_size >> 30);
}
static CLASS_ATTR_RO(hbm_capacity);

/*
 * neuron_soc/version and efa/version share the same attribute name in
 * different groups; declared with __ATTR to keep unique C symbols.
 */
static struct class_attribute class_attr_soc_version = __ATTR(version, S_IRUGO, soc_version_show, NULL);

static struct attribute *neuron_soc_attrs[] = {
	&class_attr_soc_version.attr,
	&class_attr_sub_version.attr,
	&class_attr_hbm_capacity.attr,
	NULL,
};
const struct attribute_group neuron_soc_group = {
	.name  = "neuron_soc",
	.attrs = neuron_soc_attrs,
};

static ssize_t efa_version_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	return dhal_sysfs_emit(buf, "%d\n", neuron_pci_get_efa_version());
}

static struct class_attribute class_attr_efa_version = __ATTR(version, S_IRUGO, efa_version_show, NULL);

static struct attribute *efa_attrs[] = {
	&class_attr_efa_version.attr,
	NULL,
};
const struct attribute_group efa_group = {
	.name  = "efa",
	.attrs = efa_attrs,
};

static ssize_t family_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	enum neuron_platform_family fam;

	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	fam = ndhal->ndhal_arch.platform->family;
	return dhal_sysfs_emit(buf, "%s\n",
			       lookup_name(family_names, ARRAY_SIZE(family_names), fam));
}
static CLASS_ATTR_RO(family);

static ssize_t num_chips_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	return dhal_sysfs_emit(buf, "%u\n", ndhal->ndhal_arch.platform->device_count);
}
static CLASS_ATTR_RO(num_chips);

static ssize_t num_sleds_per_server_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	return dhal_sysfs_emit(buf, "%u\n",
			       ndhal->ndhal_arch.platform->num_sleds_per_server);
}
static CLASS_ATTR_RO(num_sleds_per_server);

static ssize_t num_chips_per_sled_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	return dhal_sysfs_emit(buf, "%u\n",
			       ndhal->ndhal_arch.platform->num_chips_per_sled);
}
static CLASS_ATTR_RO(num_chips_per_sled);

static ssize_t num_cores_per_chip_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	if (!ndhal)
		return 0;
	return dhal_sysfs_emit(buf, "%d\n", ndhal->ndhal_address_map.nc_per_device);
}
static CLASS_ATTR_RO(num_cores_per_chip);

static ssize_t supported_lnc_degrees_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	unsigned long mask;
	int bit;
	ssize_t n = 0;

	if (!ndhal)
		return 0;
	mask = ndhal->ndhal_address_map.supported_lnc_degrees;
	for_each_set_bit(bit, &mask, BITS_PER_LONG)
		n += scnprintf(buf + n, PAGE_SIZE - n, "%s%d", n ? " " : "", bit);
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	return n;
}
static CLASS_ATTR_RO(supported_lnc_degrees);

static ssize_t intra_rack_topology_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	enum neuron_intra_rack_topology t;

	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	t = ndhal->ndhal_arch.platform->interconnect.intra_rack.topology;
	return dhal_sysfs_emit(buf, "%s\n",
			       lookup_name(intra_topology_names,
					   ARRAY_SIZE(intra_topology_names), t));
}
static CLASS_ATTR_RO(intra_rack_topology);

static ssize_t intra_rack_switch_type_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	enum neuron_switch_type s;

	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	s = ndhal->ndhal_arch.platform->interconnect.intra_rack.switch_type;
	return dhal_sysfs_emit(buf, "%s\n",
			       lookup_name(switch_type_names,
					   ARRAY_SIZE(switch_type_names), s));
}
static CLASS_ATTR_RO(intra_rack_switch_type);

static ssize_t inter_rack_topology_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	enum neuron_inter_rack_topology t;

	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	t = ndhal->ndhal_arch.platform->interconnect.inter_rack.topology;
	return dhal_sysfs_emit(buf, "%s\n",
			       lookup_name(inter_topology_names,
					   ARRAY_SIZE(inter_topology_names), t));
}
static CLASS_ATTR_RO(inter_rack_topology);

static ssize_t inter_rack_switch_type_show(NPLATFORM_CLASS_SHOW_ARGS, char *buf)
{
	enum neuron_switch_type s;

	if (!ndhal || !ndhal->ndhal_arch.platform)
		return 0;
	s = ndhal->ndhal_arch.platform->interconnect.inter_rack.switch_type;
	return dhal_sysfs_emit(buf, "%s\n",
			       lookup_name(switch_type_names,
					   ARRAY_SIZE(switch_type_names), s));
}
static CLASS_ATTR_RO(inter_rack_switch_type);

static struct attribute *platform_attrs[] = {
	&class_attr_family.attr,
	&class_attr_num_chips.attr,
	&class_attr_num_sleds_per_server.attr,
	&class_attr_num_chips_per_sled.attr,
	&class_attr_num_cores_per_chip.attr,
	&class_attr_supported_lnc_degrees.attr,
	&class_attr_intra_rack_topology.attr,
	&class_attr_intra_rack_switch_type.attr,
	&class_attr_inter_rack_topology.attr,
	&class_attr_inter_rack_switch_type.attr,
	NULL,
};

const struct attribute_group platform_group = {
	.name  = "platform",
	.attrs = platform_attrs,
};

int nplatform_sysfs_add_efa_symlink(struct neuron_device *nd, struct kobject *nc_kobj, int nc_id)
{
	const struct neuron_efa_bdf_map *m;
	const char *bdf;
	struct pci_dev *pdev;
	unsigned int dom, bus, slot, func;
	int global_nc_id;
	int ret;

	if (!ndhal || !ndhal->ndhal_arch.platform) {
		return 0;
	}

	m = &ndhal->ndhal_arch.platform->nc_efa_bdf_tbl;
	if (m->count == 0) {
		return 0;
	}

	global_nc_id = nd->device_index * ndhal->ndhal_address_map.nc_per_device + nc_id;
	if (global_nc_id >= m->count) {
		return -ENOENT;
	}

	bdf = m->bdfs[global_nc_id];
	if (!bdf) {
		return -ENOENT;
	}

	if (sscanf(bdf, "%x:%x:%x.%x", &dom, &bus, &slot, &func) != 4) {
		return -EINVAL;
	}

	pdev = pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, func));
	if (!pdev) {
		return -ENODEV;
	}

	ret = sysfs_create_link(nc_kobj, &pdev->dev.kobj, "efa_device");
	pci_dev_put(pdev);
	return ret;
}

