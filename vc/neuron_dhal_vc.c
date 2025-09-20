// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2025, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#include "neuron_dhal.h"

/**
 * ntpb_pe_read_counter() - read 64-bit counter value given counter lsb offset on given device
 *
 * @param bar0 - BAR0 address
 * @param lsb_offset - counter csr lsb offset
 * @param val - counter value
 * @return int: 0 on success, -EIO on failure
 *
 */
static int ntpb_pe_get_counter_val_vc(void __iomem *bar0, u64 lsb_offset, uint64_t *val)
{
    uint32_t lsb = 0;
    uint32_t msb = 0;
    u64 msb_offset = lsb_offset + 4;

    const void __iomem *msb_base_addr = (void __iomem *)bar0 + msb_offset;
    if(reg_read32(msb_base_addr, &msb)){
        return -EIO;
    }
    const void __iomem *lsb_base_addr = (void __iomem *)bar0 + lsb_offset;
    if(reg_read32(lsb_base_addr, &lsb)) {
        return -EIO;
    }
    *val = ((uint64_t)msb << 32) | lsb;

    return 0;
}

/**
 * ntpb_pe_get_fast_wl_cycle_cnt() - aggregate fast-wl cycle counts across col_grps for given row_grp
 *
 * @param nd - neuron device
 * @param nc_id - neuron core id
 * @param row_grp_id - row group id
 * @param val - aggregated fast-wl cycle count
 * @return int: 0 on success, -EIO on failure
 *
 */
static int ntpb_pe_get_fast_wl_cycle_cnt_vc(struct neuron_device *nd, int nc_id, int row_grp_id, u64 *val)
{
	int ret = 0;
	int i = 0;
	u64 cntr_val = 0;
	u64 fast_wl_row_grp_cntr_offset;
	u64 fast_wl_col_grp_cntr_offset;
	*val = 0;
	fast_wl_row_grp_cntr_offset = ndhal->ndhal_tpb.pe_get_row_grp_activity_counter_offset(ndhal->ndhal_tpb.pe_fast_wl_cntr_offsets[nc_id], row_grp_id);

	for (i = 0; i < ndhal->ndhal_tpb.pe_col_grp_count; i++) {
		fast_wl_col_grp_cntr_offset = (i * ndhal->ndhal_tpb.pe_perf_reg_grp_size) + fast_wl_row_grp_cntr_offset;
		ret = ndhal->ndhal_tpb.pe_get_counter_val(nd->npdev.bar0, fast_wl_col_grp_cntr_offset, &cntr_val);
		if(ret) {
			pr_err("failed to read pe activity counter for nd%dnc%d", nd->device_index, nc_id);
			return ret;
		}
		*val += cntr_val;
	}

	return ret;
}

/**
 * ntpb_pe_get_row_grp_activity_counter_offset() - return pe activity counter offset for given row group
 *
 * @param base - relative offset of activity counter
 * @param row_grp_id - row group id
 * @return u64 - activity counter lsb offset
 *
 */
static u64 ntpb_pe_get_row_grp_activity_counter_offset_vc(u64 base, int row_grp_id)
{
    return base + (row_grp_id * ndhal->ndhal_tpb.pe_xbus_count * ndhal->ndhal_tpb.pe_perf_reg_grp_size);
}

/**
 * ntpb_pe_format_activity_stats() - return pe array activity stats for given nc on given neuron device
 *
 * @param nd - neuron device
 * @param nc_id - neuron core id
 * @param buffer - sysfs buffer
 * @param bufflen - buffer length
 * @return int - return 0 on success
 *
 */
static int ntpb_pe_format_activity_stats_vc(struct neuron_device *nd, int nc_id, char buffer[], unsigned int bufflen)
{
    int offset = 0;
    int ret = 0;
    u64 mm_cycle_cnt = 0;
    u64 wl_cycle_cnt = 0;
    u64 idle_cycle_cnt = 0;

    offset += snprintf(buffer + offset, bufflen - offset, "xrow_grp,mm_active,wl_active,idle\n");
    if (offset >= bufflen) {
        goto done;
    }

    int i = 0;
    for (i = 0; i < ndhal->ndhal_tpb.pe_row_grp_count; i++) {
        ret = ndhal->ndhal_tpb.pe_get_counter_val(nd->npdev.bar0, ndhal->ndhal_tpb.pe_get_row_grp_activity_counter_offset(ndhal->ndhal_tpb.pe_mm_cntr_offsets[nc_id], i), &mm_cycle_cnt);
        if(ret) {
			pr_err("failed to read pe matmul activity counter for nd%dnc%d", nd->device_index, nc_id);
			return ret;
		}

        ret = ndhal->ndhal_tpb.pe_get_aggregated_wl_cycle_cnt(nd, nc_id, i, &wl_cycle_cnt);
        if(ret) {
			pr_err("failed to read pe weight-load activity counter for nd%dnc%d", nd->device_index, nc_id);
			return ret;
		}

        ret = ndhal->ndhal_tpb.pe_get_counter_val(nd->npdev.bar0, ndhal->ndhal_tpb.pe_get_row_grp_activity_counter_offset(ndhal->ndhal_tpb.pe_idle_cntr_offsets[nc_id], i), &idle_cycle_cnt);
        if(ret) {
			pr_err("failed to read pe idle cycle counter for nd%dnc%d", nd->device_index, nc_id);
			return ret;
		}

        offset += snprintf(buffer + offset, bufflen - offset, "%d,%llu,%llu,%llu\n", i, mm_cycle_cnt, wl_cycle_cnt, idle_cycle_cnt);
        if (offset >= bufflen) {
            goto done;
        }
    }

done:
    return ((offset > bufflen) || ((offset == bufflen) && buffer[bufflen - 1] != '\0'));
}

/**
 * ndhal_register_funcs_vc() - initialize the common dhal for all chips
 *
 */
int ndhal_register_funcs_vc(void) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for VC.");
		return -EINVAL;
	}

    ndhal->ndhal_tpb.pe_format_activity_stats = ntpb_pe_format_activity_stats_vc;
    ndhal->ndhal_tpb.pe_get_counter_val = ntpb_pe_get_counter_val_vc;
    ndhal->ndhal_tpb.pe_get_row_grp_activity_counter_offset = ntpb_pe_get_row_grp_activity_counter_offset_vc;
    ndhal->ndhal_tpb.pe_get_fast_wl_cycle_cnt = ntpb_pe_get_fast_wl_cycle_cnt_vc;
    return ret;
}