// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/**
 * HAM Periodic Throttle Notification
 *
 * This module sets up a notification queue per NC to receive periodic
 * HAM throttle notifications. FW enables periodic mode on the HAM;
 * this module handles the Neuron-side queue setup and routing.
 *
 * The parsed per-NC state is exposed to userspace through the
 * neuron_sysfs_metrics framework (neuron_core{N}/stats/ham_throttle/throttle).
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/pci.h>

#include "neuron_ham_throttle.h"
#include "neuron_device.h"
#include "neuron_arch.h"
#include "neuron_dhal.h"
#include "neuron_nq.h"
#include "neuron_mempool.h"
#include "neuron_reg_access.h"
#include "v3/notific.h"

#define HAM_PERIODIC_NQ_SLOT 9

// Buffer size: 2KB, 128 entries at 16 bytes each. Periodic notifications
// arrive roughly every ~1s, and userspace polls and aggregates the most
// recent handful of entries, so a small ring is plenty.
#define HAM_PERIODIC_NQ_BUF_SIZE (2 * 1024)

#define TPB_NOTIFIC_SUBBLOCK_OFFSET       0x1000
#define TPB_SW_QUEUE_NUM4_OFFSET          0xa10
#define NOTIFIC_WR_BUF_ENABLE_LO_OFFSET   0x30
#define NOTIFIC_NQ_SW_OVERFLOW_OFFSET     0x18

#define NOTIFIC_WR_BUF_HAM_BIT            12

#define SW_QUEUE_NUM4_HAM_PERIODIC_MASK       0xF
#define SW_QUEUE_NUM4_HAM_PERIODIC_EN_BIT     (1 << 16)

#define NOTIF_ENTRY_SIZE 16
#define HAM_PERIODIC_NQ_NUM_ENTRIES (HAM_PERIODIC_NQ_BUF_SIZE / NOTIF_ENTRY_SIZE)

struct ham_throttle_entry {
	u64 ts;
	u8 mean;
	u8 max;
	u8 min;
};

/**
 * ham_throttle_decode_entry() - decode one 16B ring entry at index @idx.
 *
 * Returns true and fills @out if the slot holds a real notification, false if
 * the slot is empty (never written).
 *
 * Entry layout (16 bytes):
 *   word0 (d1): [7:0]=min, [15:8]=max, [23:16]=mean  (0=throttled, 255=unthrottled)
 *   word1 (d2): high nibble 0xF = periodic marker
 *   word2/word3: 64-bit timestamp
 */
static bool ham_throttle_decode_entry(const u32 *buf_words, u32 idx,
				      struct ham_throttle_entry *out)
{
	const u32 *words = &buf_words[idx * 4];
	u32 d1;

	// An untouched slot reads as all-zero. A live entry always carries a
	// non-zero timestamp, so treat a zero timestamp as "empty".
	if (words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == 0)
		return false;

	out->ts = ((u64)words[3] << 32) | words[2];
	if (out->ts == 0)
		return false;

	d1 = words[0];
	out->mean = (d1 >> 16) & 0xFF;
	out->max = (d1 >> 8) & 0xFF;
	out->min = d1 & 0xFF;
	return true;
}

/**
 * ham_throttle_select_next() - find the most-recent entry strictly older than
 *                              the previously selected one.
 *
 * We rank entries by the composite key (timestamp, slot index) so that even if
 * two entries carry identical timestamps we still walk them one at a time and
 * never pick the same slot twice. @prev_ts / @prev_idx describe the last entry
 * we returned; pass (U64_MAX, INT_MAX) to get the newest entry.
 *
 * Returns true and fills @out + @out_idx if a matching entry was found.
 */
static bool ham_throttle_select_next(const u32 *buf_words, u64 prev_ts, int prev_idx,
				     struct ham_throttle_entry *out, int *out_idx)
{
	struct ham_throttle_entry cand;
	u64 best_ts = 0;
	int best_idx = -1;
	u32 i;

	for (i = 0; i < HAM_PERIODIC_NQ_NUM_ENTRIES; i++) {
		if (!ham_throttle_decode_entry(buf_words, i, &cand))
			continue;

		if (cand.ts > prev_ts || (cand.ts == prev_ts && (int)i >= prev_idx))
			continue;

		if (best_idx < 0 || cand.ts > best_ts ||
		    (cand.ts == best_ts && (int)i > best_idx)) {
			best_ts = cand.ts;
			best_idx = i;
			*out = cand;
		}
	}

	if (best_idx < 0)
		return false;
	*out_idx = best_idx;
	return true;
}

ssize_t neuron_ham_throttle_format(struct neuron_device *nd, int nc_id, char *buf, size_t len)
{
	struct mem_chunk *mc;
	const u32 *buf_words;
	struct ham_throttle_entry e;
	u32 n_agg, count = 0;
	u64 prev_ts = U64_MAX;
	int prev_idx = INT_MAX;
	u64 first_ts = 0, last_ts = 0;
	u32 mean_sum = 0;
	u8 min_of_min = 255, max_of_max = 0, mean_of_mean;

	if (nc_id < 0 || nc_id >= MAX_NC_PER_DEVICE)
		return scnprintf(buf, len, "invalid nc\n");

	mc = nd->ham_throttle.periodic_mc[nc_id];
	if (!mc || !mc->va)
		return scnprintf(buf, len, "not configured\n");

	buf_words = (const u32 *)mc->va;

	n_agg = nd->ham_throttle.agg_count[nc_id];
	if (n_agg == 0)
		n_agg = 1;
	if (n_agg > HAM_PERIODIC_NQ_NUM_ENTRIES)
		n_agg = HAM_PERIODIC_NQ_NUM_ENTRIES;

	while (count < n_agg &&
	       ham_throttle_select_next(buf_words, prev_ts, prev_idx, &e, &prev_idx)) {
		prev_ts = e.ts;

		if (count == 0)
			last_ts = e.ts;	// newest entry seen
		first_ts = e.ts;	// oldest so far; final value after loop

		mean_sum += e.mean;
		if (e.min < min_of_min)
			min_of_min = e.min;
		if (e.max > max_of_max)
			max_of_max = e.max;
		count++;
	}

	if (count == 0)
		return scnprintf(buf, len, "no entries\n");

	mean_of_mean = mean_sum / count;

	return scnprintf(buf, len,
			 "entries=%u mean=%u/255(%d%%) max=%u/255(%d%%) min=%u/255(%d%%) first_ts=%llu last_ts=%llu\n",
			 count,
			 mean_of_mean, (mean_of_mean * 100) / 255,
			 max_of_max, (max_of_max * 100) / 255,
			 min_of_min, (min_of_min * 100) / 255,
			 first_ts, last_ts);
}

ssize_t neuron_ham_throttle_set_aggregate_count(struct neuron_device *nd, int nc_id,
						const char *buf, size_t size)
{
	u32 val;
	int ret;

	if (nc_id < 0 || nc_id >= MAX_NC_PER_DEVICE)
		return -EINVAL;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	if (val == 0 || val > HAM_PERIODIC_NQ_NUM_ENTRIES) {
		pr_err("nd%d nc%d: aggregate count %u out of range [1, %u]\n",
		       nd->device_index, nc_id, val, HAM_PERIODIC_NQ_NUM_ENTRIES);
		return -EINVAL;
	}

	nd->ham_throttle.agg_count[nc_id] = val;
	return size;
}

/**
 * ham_throttle_program_nc() - Program the NOTIFIC block for one NC to route
 *                             periodic HAM notifications into slot 9.
 *
 * @nd: neuron device
 * @nc_id: neuron core / TPB index
 * @return: 0 on success, negative errno if a register read fails
 */
static int ham_throttle_program_nc(struct neuron_device *nd, u8 nc_id)
{
	u64 notific_offset = notific_get_relative_offset_v3(nc_id);
	void *notific_base = nd->npdev.bar0 + notific_offset;
	void *tpb_base = notific_base - TPB_NOTIFIC_SUBBLOCK_OFFSET;
	void *sq4_addr = tpb_base + TPB_SW_QUEUE_NUM4_OFFSET;
	u32 nq_en, val, after, wob, ovf;
	int ret;

	// Enable only the HAM write-buffer input (bit 12), read-modify-write, so we
	// don't disturb other notification sources the runtime depends on
	ret = reg_read32(notific_base + NOTIFIC_WR_BUF_ENABLE_LO_OFFSET, &wob);
	if (ret) {
		pr_err("nd%d nc%d: read wr_buf_enable_lo failed: %d\n",
		       nd->device_index, nc_id, ret);
		return ret;
	}
	wob |= (1 << NOTIFIC_WR_BUF_HAM_BIT);
	reg_write32(notific_base + NOTIFIC_WR_BUF_ENABLE_LO_OFFSET, wob);

	// Enable the NQ slot for HAM periodic notifications
	ret = reg_read32(notific_base + NOTIFIC_NQ_ENABLE_OFFSET, &nq_en);
	if (ret) {
		pr_err("nd%d nc%d: read nq_enable failed: %d\n",
		       nd->device_index, nc_id, ret);
		return ret;
	}
	nq_en |= (1 << HAM_PERIODIC_NQ_SLOT);
	reg_write32(notific_base + NOTIFIC_NQ_ENABLE_OFFSET, nq_en);

	// We don't continuously consume periodic HAM notifications. Let the queue
	// wrap around and overwrite old entries. We are simply interested in the most
	// recent entries
	ret = reg_read32(notific_base + NOTIFIC_NQ_SW_OVERFLOW_OFFSET, &ovf);
	if (ret) {
		pr_err("nd%d nc%d: read nq_sw_overflow failed: %d\n",
		       nd->device_index, nc_id, ret);
		return ret;
	}
	ovf |= (1 << HAM_PERIODIC_NQ_SLOT);
	reg_write32(notific_base + NOTIFIC_NQ_SW_OVERFLOW_OFFSET, ovf);

	// Route periodic notifications to slot 9 and enable
	ret = reg_read32(sq4_addr, &val);
	if (ret) {
		pr_err("nd%d nc%d: read sw_queue_num4 failed: %d\n",
		       nd->device_index, nc_id, ret);
		return ret;
	}
	val = (val & ~SW_QUEUE_NUM4_HAM_PERIODIC_MASK) | HAM_PERIODIC_NQ_SLOT;
	val |= SW_QUEUE_NUM4_HAM_PERIODIC_EN_BIT;
	reg_write32(sq4_addr, val);
	mb();
	reg_read32(sq4_addr, &after);

	pr_debug("nd%d nc%d sw_queue_num4 wrote=0x%08x after=0x%08x\n",
		 nd->device_index, nc_id, val, after);
	return 0;
}

/**
 * neuron_ham_throttle_reprogram_nc() - Re-apply NOTIFIC routing for one NC.
 *
 * A TPB reset clears sw_queue_num4 and the NOTIFIC enables back to 0, losing
 * the routing set up at init. The slot-9 buffer survives, so we only re-program
 * the registers no re-allocation.
 *
 * @nd: neuron device
 * @nc_id: neuron core / TPB index
 */
void neuron_ham_throttle_reprogram_nc(struct neuron_device *nd, u8 nc_id)
{
	if (narch_get_arch() != NEURON_ARCH_V3 &&
	    narch_get_arch() != NEURON_ARCH_V4)
		return;
	if (nc_id >= MAX_NC_PER_DEVICE)
		return;
	// Only reprogram NCs whose buffer was already allocated at init
	if (!nd->ham_throttle.periodic_mc[nc_id])
		return;
	if (nd->ham_throttle.periodic_mc[nc_id]->va)
		memset(nd->ham_throttle.periodic_mc[nc_id]->va, 0, HAM_PERIODIC_NQ_BUF_SIZE);
	// The buffer survives reset, but nnq_set_hwaddr programs the NOTIFIC
	// base-address/size registers, which the reset also clears
	ndhal->ndhal_nq.nnq_set_hwaddr(nd, nc_id, HAM_PERIODIC_NQ_SLOT,
					NQ_TYPE_THROTTLE, HAM_PERIODIC_NQ_BUF_SIZE,
					nd->ham_throttle.periodic_mc[nc_id]->pa);
	if (ham_throttle_program_nc(nd, nc_id))
		pr_warn("nd%d nc%d: failed to set periodic throttle routing\n",
			nd->device_index, nc_id);
}

int neuron_ham_throttle_init(struct neuron_device *nd)
{
	int ret;
	u8 nc_id;
	struct mem_chunk *mc = NULL;

	if (narch_get_arch() != NEURON_ARCH_V3 &&
	    narch_get_arch() != NEURON_ARCH_V4)
		return 0;

	for (nc_id = 0; nc_id < ndhal->ndhal_address_map.nc_per_device; nc_id++) {
		if (((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0)
			continue;

		// Reuse the existing buffer; only allocate the first time. This
		// runs on each device-wide reset, so the one-time buffer alloc
		// must be guarded, or we leak buffers.
		mc = nd->ham_throttle.periodic_mc[nc_id];
		if (!mc) {
			ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, HAM_PERIODIC_NQ_BUF_SIZE,
					     0, MEM_LOC_HOST, 0, nc_id,
					     NEURON_MEMALLOC_TYPE_NOTIFICATION_HOST, &mc);
			if (ret) {
				pr_err("Failed to allocate periodic throttle NQ buffer for NC %d: %d\n",
				       nc_id, ret);
				neuron_ham_throttle_destroy(nd);
				return ret;
			}
			nd->ham_throttle.periodic_mc[nc_id] = mc;
		}
		if (mc->va)
			memset(mc->va, 0, HAM_PERIODIC_NQ_BUF_SIZE);
		ndhal->ndhal_nq.nnq_set_hwaddr(nd, nc_id, HAM_PERIODIC_NQ_SLOT,
						NQ_TYPE_THROTTLE, HAM_PERIODIC_NQ_BUF_SIZE,
						mc->pa);
		if (ham_throttle_program_nc(nd, nc_id))
			pr_warn("nd%d nc%d: failed to set periodic throttle routing\n",
				nd->device_index, nc_id);
	}

	return 0;
}

void neuron_ham_throttle_destroy(struct neuron_device *nd)
{
	u8 nc_id;

	for (nc_id = 0; nc_id < ndhal->ndhal_address_map.nc_per_device; nc_id++) {
		if (((1 << nc_id) & ndhal->ndhal_address_map.dev_nc_map) == 0)
			continue;

		if (nd->ham_throttle.periodic_mc[nc_id]) {
			ndhal->ndhal_nq.nnq_set_hwaddr(nd, nc_id, HAM_PERIODIC_NQ_SLOT,
							NQ_TYPE_THROTTLE, 0, 0);
			mc_free(&nd->ham_throttle.periodic_mc[nc_id]);
		}
	}
}

