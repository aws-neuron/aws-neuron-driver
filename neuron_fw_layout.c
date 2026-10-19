// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * Reader for the firmware general memory layout (see neuron_fw_layout.h for
 * the contract); the platform-specific parts come from ndhal_fw_metrics
 * (vN/neuron_fw_metrics.c).  All state transitions happen on the metrics
 * thread via fw_layout_poll(); other threads only set renegotiate_requested
 * or copy cached payloads out through fw_layout_get_entry().
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>

#include "neuron_fw_layout.h"
#include "neuron_device.h"
#include "neuron_dhal.h"
#include "neuron_fw_io.h"
#include "neuron_arch.h"
#include "neuron_mempool.h"

static_assert(sizeof(struct fw_layout_header) == FW_LAYOUT_HEADER_SIZE, "fw layout header must be 64 bytes");
static_assert(sizeof(struct fw_layout_tlv) == FW_LAYOUT_TLV_SIZE, "fw layout descriptor must be 16 bytes");

static int fw_layout_enable = 1;
module_param(fw_layout_enable, int, S_IRUSR | S_IRGRP);
MODULE_PARM_DESC(fw_layout_enable, "enable the firmware HBM layout reader");

/*
 * Copy region bytes [offset, offset + size) into a normal kernel buffer.  HBM
 * is read in whole 8-byte words; the head and tail are clipped here so callers
 * need no alignment of their own.
 */
static void fw_layout_read(struct neuron_fw_layout *fwl, void *dst, u32 offset, u32 size)
{
	// TODO: we technically may have races with the head tail guard where:
	//  1. we read old head
	//  2. firmware writes new head and new body
	//  3. we read new body and old tail
	// Need more atomic reads to get around this
	u32 end = offset + size;
	u32 pos = round_down(offset, sizeof(u64)); // widest plain access: every HBM read is a PCIe round trip
	u8 *d = dst;

	for (; pos < end; pos += sizeof(u64)) {
		u64 word = readq(fwl->region + pos);
		u32 from = max(pos, offset);
		u32 to = min(pos + (u32)sizeof(u64), end);

		memcpy(d + (from - offset), (u8 *)&word + (from - pos), to - from);
	}
}

/* Free the parsed directory and the payload cache. */
static void fw_layout_drop_layout(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	struct fw_layout_entry *entries;
	u8 *cache, *staging;

	mutex_lock(&fwl->lock);
	entries = fwl->entries;
	cache = fwl->cache;
	staging = fwl->staging;
	fwl->entries = NULL;
	fwl->num_entries = 0;
	fwl->cache = NULL;
	fwl->staging = NULL;
	fwl->cache_offset = 0;
	fwl->cache_size = 0;
	mutex_unlock(&fwl->lock);

	kfree(entries);
	kvfree(cache);
	kvfree(staging);
}

/* Start a negotiation, invalidating any parsed layout (it belongs to the
 * previous incarnation of the region).
 */
static void fw_layout_enter_negotiate(struct neuron_device *nd)
{
	fw_layout_drop_layout(nd);
	nd->fw_layout.state = FW_LAYOUT_STATE_NEGOTIATE;
}

/* SET_MAX_VERSION carries the platform's misc-ram version; firmware picks the
 * layout version from it, so the header's ver_major says what we actually got.
 */
static void fw_layout_negotiate(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	u32 version = ndhal->ndhal_fw_metrics.max_misc_ram_version;
	int ret;

	ret = fw_io_set_max_version(nd->fw_io_ctx, version);
	if (ret == 0) {
		fwl->state = FW_LAYOUT_STATE_WAIT_HEADER;
		return;
	}
	if (ret == -ENOTSUPP) {
		// the running firmware predates SET_MAX_VERSION: there is no
		// layout to read
		pr_info("nd%d: firmware does not support the HBM layout, falling back to legacy behavior\n",
			nd->device_index);
	} else {
		// FAIL response or timeout.  No retry policy yet: give up on this
		// incarnation of the layout, the next device reset negotiates again
		pr_warn("nd%d: HBM layout negotiation (misc-ram version 0x%x) failed (%d), not reading the layout\n",
			nd->device_index, version, ret);
	}
	fwl->state = FW_LAYOUT_STATE_IDLE;
}

/*
 * Validate the header in the prescribed order: magic -> INITIALIZED -> crc32
 * -> ver_major.  Returns 0 with *hdr filled on success, -EAGAIN if the region
 * is not (yet) valid or looks torn, -EINVAL on a major version we must not
 * parse.
 */
static int fw_layout_validate_header(struct neuron_device *nd, struct fw_layout_header *hdr)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	u32 crc;

	fw_layout_read(fwl, hdr, 0, sizeof(*hdr));

	if (hdr->magic != FW_LAYOUT_MAGIC)
		return -EAGAIN;
	if (!(hdr->flags & FW_LAYOUT_HDR_INITIALIZED))
		return -EAGAIN; // firmware is mid-(re)write
	crc = fw_io_calc_crc32((const u8 *)hdr, FW_LAYOUT_HDR_CRC_LEN);
	if (crc != hdr->header_crc32)
		return -EAGAIN; // torn/invalid header
	if (hdr->ver_major != FW_LAYOUT_VER_MAJOR) {
		pr_warn("nd%d: fw layout ver %u.%u unsupported (driver supports major %u), not parsing\n",
			nd->device_index, hdr->ver_major, hdr->ver_minor, FW_LAYOUT_VER_MAJOR);
		return -EINVAL;
	}
	return 0;
}

/*
 * Walk the directory once, cache every descriptor and size the payload cache.
 * Entries with a geometry we cannot safely read (empty, or payload outside the
 * region) are kept but marked unusable so instance enumeration still matches
 * directory order.
 */
static int fw_layout_parse_directory(struct neuron_device *nd, const struct fw_layout_header *hdr)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	struct fw_layout_entry *entries;
	u32 count = hdr->record_count;
	// payloads are densely packed after the full directory - a payload offset
	// inside the header or the directory (even past the entries we cap at)
	// can only be garbage
	u64 dir_end = FW_LAYOUT_DIR_OFFSET + (u64)hdr->record_count * FW_LAYOUT_TLV_SIZE;
	u32 dir_size;
	u8 *dir;
	u8 *cache = NULL, *staging = NULL;
	u32 lo = U32_MAX, hi = 0;
	u32 i;

	if (dir_end > FW_LAYOUT_REGION_SIZE) {
		// the directory alone overflows the region: no layout version looks
		// like this (a newer minor with more entries is capped below)
		pr_err("nd%d: fw layout header claims %u entries, more than the region holds - firmware bug, not parsing\n",
			nd->device_index, hdr->record_count);
		return -EINVAL;
	}
	if (count > FW_LAYOUT_MAX_ENTRIES) {
		pr_warn_once("nd%d: fw layout has %u entries, only reading the first %u\n",
			     nd->device_index, count, FW_LAYOUT_MAX_ENTRIES);
		count = FW_LAYOUT_MAX_ENTRIES;
	}

	dir_size = count * FW_LAYOUT_TLV_SIZE;
	dir = kmalloc(dir_size, GFP_KERNEL);
	entries = kcalloc(count, sizeof(*entries), GFP_KERNEL);
	if (!dir || !entries) {
		kfree(dir);
		kfree(entries);
		return -ENOMEM;
	}
	fw_layout_read(fwl, dir, FW_LAYOUT_DIR_OFFSET, dir_size);

	for (i = 0; i < count; i++) {
		struct fw_layout_tlv tlv;
		struct fw_layout_entry *e = &entries[i];

		memcpy(&tlv, dir + i * FW_LAYOUT_TLV_SIZE, sizeof(tlv));
		e->type_id = tlv.type_id;
		e->length = tlv.length;
		e->offset = tlv.offset;
		e->usable = tlv.length >= FW_LAYOUT_MIN_PAYLOAD_SIZE &&
			    tlv.offset >= dir_end &&
			    (u64)tlv.offset + tlv.length <= FW_LAYOUT_REGION_SIZE;
		if (!e->usable) {
			pr_warn_once("nd%d: fw layout entry %u (type 0x%04x) has unusable geometry (offset 0x%x len %u), skipping\n",
				     nd->device_index, i, tlv.type_id, tlv.offset, tlv.length);
			continue;
		}
		lo = min(lo, tlv.offset);
		hi = max(hi, tlv.offset + tlv.length);
	}
	kfree(dir);

	// the cache mirrors the one span that covers every usable payload.
	// payloads are densely packed, so the span is about the sum of their
	// sizes - and it is what every poll reads from HBM
	if (hi <= lo) {
		lo = 0; // no usable payloads: nothing to cache
		hi = 0;
	} else {
		cache = kvzalloc(hi - lo, GFP_KERNEL);
		staging = kvzalloc(hi - lo, GFP_KERNEL);
		if (!cache || !staging) {
			kvfree(cache);
			kvfree(staging);
			kfree(entries);
			return -ENOMEM;
		}
	}

	// negotiation already dropped the previous incarnation; never leak one
	fw_layout_drop_layout(nd);
	mutex_lock(&fwl->lock);
	fwl->entries = entries;
	fwl->num_entries = count;
	fwl->cache = cache;
	fwl->staging = staging;
	fwl->cache_offset = lo;
	fwl->cache_size = hi - lo;
	mutex_unlock(&fwl->lock);
	return 0;
}

/* Refresh the payload cache with one bulk read of its span.  The HBM read
 * lands in the staging buffer so the lock is only held for the memcpy.
 */
static void fw_layout_refresh_cache(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;

	fw_layout_read(fwl, fwl->staging, fwl->cache_offset, fwl->cache_size);
	mutex_lock(&fwl->lock);
	memcpy(fwl->cache, fwl->staging, fwl->cache_size);
	mutex_unlock(&fwl->lock);
}

/* WAIT_HEADER state: once the header validates, parse the directory and go
 * ACTIVE.  This is the only time the header is read.
 */
static void fw_layout_wait_header(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	struct fw_layout_header hdr __aligned(sizeof(u64));
	int ret;

	ret = fw_layout_validate_header(nd, &hdr);
	if (ret == 0)
		ret = fw_layout_parse_directory(nd, &hdr);

	switch (ret) {
	case 0:
		pr_info("nd%d: fw layout ver %u.%u active, %u entries, caching %u bytes\n",
			nd->device_index, hdr.ver_major, hdr.ver_minor, fwl->num_entries,
			fwl->cache_size);
		// fill the cache before serving it, so ACTIVE means real data
		fw_layout_refresh_cache(nd);
		fwl->state = FW_LAYOUT_STATE_ACTIVE;
		break;
	case -EINVAL:
	case -ENOMEM:
		// bad data from firmware, or no memory: idle until the next device reset
		fwl->state = FW_LAYOUT_STATE_IDLE;
		break;
	default:
		// -EAGAIN: not initialized yet or torn - keep polling the header
		break;
	}
}

void fw_layout_poll(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;

	if (fwl->state == FW_LAYOUT_STATE_DISABLED)
		return;

	// a completed device reset (the initial one at load included) kicks a
	// negotiation: the carveout was scrubbed and re-written, so a parsed
	// layout belongs to the previous incarnation of the region
	if (atomic_xchg(&fwl->renegotiate_requested, 0))
		fw_layout_enter_negotiate(nd);

	// firmware halts the layout writers for any reset (TPB included) and
	// scrubs the carveout on a device reset: nothing to read until the reset
	// queue drains.  a completed device reset kicks a negotiation (above)
	if (nd->nr.req_pending_head)
		return;

	switch (fwl->state) {
	case FW_LAYOUT_STATE_NEGOTIATE:
		fw_layout_negotiate(nd);
		break;
	case FW_LAYOUT_STATE_WAIT_HEADER:
		fw_layout_wait_header(nd);
		break;
	case FW_LAYOUT_STATE_ACTIVE:
		fw_layout_refresh_cache(nd);
		break;
	default:
		// IDLE: nothing to read until the next device reset
		break;
	}
}

void fw_layout_request_renegotiate(struct neuron_device *nd)
{
	atomic_set(&nd->fw_layout.renegotiate_requested, 1);
}

int fw_layout_get_entry(struct neuron_device *nd, u16 type_id, u32 instance, void *buf, u32 buf_size)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	int ret = -ENOENT;
	u32 i;

	mutex_lock(&fwl->lock);
	if (fwl->state != FW_LAYOUT_STATE_ACTIVE) {
		ret = -ENODEV;
		goto done;
	}
	for (i = 0; i < fwl->num_entries; i++) {
		struct fw_layout_entry *e = &fwl->entries[i];

		if (e->type_id != type_id)
			continue;
		if (instance--)
			continue;
		if (!e->usable) {
			ret = -ENODATA;
			goto done;
		}
		ret = min(buf_size, e->length);
		memcpy(buf, fwl->cache + (e->offset - fwl->cache_offset), ret);
		goto done;
	}
done:
	mutex_unlock(&fwl->lock);
	return ret;
}

/*
 * Locate the layout region within the BAR4 mapping.  The region base is 1MB
 * into HBM0 - inside the 16MB firmware carveout already excluded from the
 * allocation pool.
 */
static int fw_layout_locate_region(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;
	u64 hbm0_base, bar_offset;
	int ret;

	if (!nd->npdev.bar4 || !nd->npdev.bar4_pa || !ndhal->ndhal_mmap.mmap_get_bar4_offset)
		return -ENODEV;

	hbm0_base = ndhal->ndhal_mpset.device_dram_effective_base_addr[0] - MEMPOOL_CARVEOUT_SIZE;
	ret = ndhal->ndhal_mmap.mmap_get_bar4_offset(hbm0_base + FW_LAYOUT_REGION_HBM_OFFSET,
						     FW_LAYOUT_REGION_SIZE, &bar_offset);
	if (ret)
		return ret;
	if (bar_offset + FW_LAYOUT_REGION_SIZE > nd->npdev.bar4_size)
		return -ENODEV;

	fwl->region = nd->npdev.bar4 + bar_offset;
	return 0;
}

void fw_layout_init(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;

	// nd is kvzalloc'ed, so all fw_layout fields start out zeroed
	// (state == FW_LAYOUT_STATE_DISABLED).  Do not memset here: the reset
	// thread is already running and the initial reset may already have
	// completed and set renegotiate_requested - a request that must not be
	// wiped.
	mutex_init(&fwl->lock);

	if (!fw_layout_enable)
		return;
	// simulation/emulation has no firmware behind the BARs
	if (narch_is_qemu() || narch_is_emu())
		return;
	if (fw_layout_locate_region(nd)) {
		pr_info("nd%d: no BAR4 window for the fw layout, reader disabled\n", nd->device_index);
		return;
	}

	// the post-reset config (v3 onwards - v2 never negotiates) kicks a
	// negotiation once the initial device reset completes.  SET_MAX_VERSION is
	// sent unconditionally - an old firmware answers UNKNOWN_COMMAND and the
	// reader falls back to legacy behavior
	fwl->state = FW_LAYOUT_STATE_IDLE;
}

void fw_layout_destroy(struct neuron_device *nd)
{
	struct neuron_fw_layout *fwl = &nd->fw_layout;

	fw_layout_drop_layout(nd);
	fwl->state = FW_LAYOUT_STATE_DISABLED;
	fwl->region = NULL;
	mutex_destroy(&fwl->lock);
}

