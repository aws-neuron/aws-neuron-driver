// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * Reader for the firmware general memory layout.
 *
 * Firmware publishes a self-describing, versioned region in HBM (inside the
 * 16MB firmware carveout at the base of HBM0) instead of fixed misc-ram
 * registers.  The region contains a fixed 64-byte header, a directory of
 * fixed-size TLV descriptors and densely packed payloads located by the
 * directory offsets.
 *
 * This module implements only the layout protocol - it does not interpret a
 * single payload byte.  The per-type knowledge lives in
 * neuron_fw_metrics_common.{c,h} and vN/neuron_fw_metrics.c.
 *
 * The driver's contract (reading side):
 *   1. Negotiate: after every completed device reset (the initial one at load
 *      included) send SET_MAX_VERSION with the platform's misc-ram version
 *      (vN/neuron_fw_metrics.h).  UNKNOWN_COMMAND means the running firmware
 *      predates the feature - fall back to legacy behavior.
 *   2. Read and validate the header once per negotiation:
 *      magic -> INITIALIZED -> crc32 -> ver_major.
 *   3. Walk the directory once and cache the offsets.
 *   4. Poll from the metrics thread: refresh the payload cache - a mirror of
 *      the span of the region that holds every payload - with one bulk read.
 *      The cache is raw bytes; whether a payload is internally consistent
 *      (e.g. carries matching generation guards) is a property of its type,
 *      checked when a metric is parsed out of the cache.
 *
 * Not handled yet: a layout rebuilt or withdrawn under a running driver
 * (firmware live update / rollback) and firmware that stopped publishing
 * (staleness).
 */

#ifndef NEURON_FW_LAYOUT_H
#define NEURON_FW_LAYOUT_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

struct neuron_device;

/* Layout major this driver parses (header ver_major check).  Not the negotiated
 * value - that is the platform's misc-ram version, ndhal_fw_metrics.
 */
#define FW_LAYOUT_VER_MAJOR 1

/* Region geometry: the layout occupies [0x100000, 0x200000) of HBM0, inside
 * the 16MB firmware carveout the driver already reserves at the base of every
 * HBM (MEMPOOL_CARVEOUT_SIZE).
 */
#define FW_LAYOUT_REGION_HBM_OFFSET 0x100000ull
#define FW_LAYOUT_REGION_SIZE 0x100000ull

#define FW_LAYOUT_MAGIC 0x5043 /* "PC" */
#define FW_LAYOUT_HDR_INITIALIZED (1u << 0) /* header flags: frame fully written */

#define FW_LAYOUT_HEADER_SIZE 64
#define FW_LAYOUT_TLV_SIZE 16
#define FW_LAYOUT_DIR_OFFSET FW_LAYOUT_HEADER_SIZE /* directory starts right after the header */
#define FW_LAYOUT_HDR_CRC_LEN 12 /* header crc32 covers bytes [0x00..0x0B] */

/* Sanity caps on what we accept from the directory */
#define FW_LAYOUT_MAX_ENTRIES 256
#define FW_LAYOUT_MIN_PAYLOAD_SIZE 1

/* Fixed 64-byte header at the base of the region.  All fields little-endian. */
struct fw_layout_header {
	u16 magic; /* 0x00 FW_LAYOUT_MAGIC */
	u16 flags; /* 0x02 bit 0 = INITIALIZED */
	u16 ver_major; /* 0x04 breaking change - do not parse on mismatch */
	u16 ver_minor; /* 0x06 additive - higher minor is safe to read */
	u32 record_count; /* 0x08 number of directory descriptors */
	u32 header_crc32; /* 0x0C crc32 over bytes [0x00..0x0B] */
	u8 reserved[48]; /* 0x10 */
} __packed;

/* Fixed 16-byte directory descriptor, record i at FW_LAYOUT_DIR_OFFSET + i * 16. */
struct fw_layout_tlv {
	u16 type_id; /* +0x00 payload schema id; unknown ids are skipped */
	u16 flags; /* +0x02 no flags defined yet */
	u32 length; /* +0x04 payload length in bytes */
	u32 offset; /* +0x08 payload offset from region base */
	u32 reserved; /* +0x0C */
} __packed;

/* Driver-side view of one directory entry.  The payload bytes themselves live
 * in the cache, at offset - cache_offset.
 */
struct fw_layout_entry {
	u16 type_id;
	u32 length; /* payload length in bytes */
	u32 offset; /* payload offset from region base */
	bool usable; /* geometry validated; payload lies within the cache */
};

enum fw_layout_state {
	FW_LAYOUT_STATE_DISABLED = 0, /* this driver instance cannot run the reader: feature off / no BAR4 */
	FW_LAYOUT_STATE_IDLE, /* nothing to read until the next device reset: negotiation failed, bad header or no memory */
	FW_LAYOUT_STATE_NEGOTIATE, /* send SET_MAX_VERSION */
	FW_LAYOUT_STATE_WAIT_HEADER, /* negotiated; waiting for a valid INITIALIZED header */
	FW_LAYOUT_STATE_ACTIVE, /* directory parsed; refreshing the payload cache */
};

struct neuron_fw_layout {
	/* the state machine is owned by the metrics thread; lock protects the
	 * entry table and the cache for cross-thread readers
	 */
	struct mutex lock;
	enum fw_layout_state state;
	atomic_t renegotiate_requested; /* set by the reset thread after a completed device reset */

	void __iomem *region; /* layout region base within the BAR4 mapping */

	struct fw_layout_entry *entries; /* one per directory descriptor */
	u32 num_entries;

	/* payload cache: a mirror of region bytes [cache_offset, cache_offset +
	 * cache_size) - the span covering every usable payload -
	 * refreshed with one bulk read per poll
	 */
	u8 *cache;
	u32 cache_offset;
	u32 cache_size;
	u8 *staging; /* HBM read lands here off-lock, then memcpy into cache */
};

/**
 * fw_layout_init() - initialize the firmware layout reader for a device.
 *
 * Never fails: on unsupported setups (feature off, no BAR4, qemu/emulator) the
 * reader is left in FW_LAYOUT_STATE_DISABLED.
 *
 * @nd: neuron device
 */
void fw_layout_init(struct neuron_device *nd);

/**
 * fw_layout_destroy() - free all reader resources.
 *
 * Must be called after the metrics thread is stopped.
 *
 * @nd: neuron device
 */
void fw_layout_destroy(struct neuron_device *nd);

/**
 * fw_layout_poll() - drive the layout reader state machine.
 *
 * Called periodically from the metrics thread while the device is READY.
 * Negotiates with firmware, parses the layout and refreshes the payload cache.
 *
 * @nd: neuron device
 */
void fw_layout_poll(struct neuron_device *nd);

/**
 * fw_layout_request_renegotiate() - request a negotiation on the next poll.
 *
 * Called from the post-reset configuration after a completed device-level
 * reset, the initial reset at driver load included: the HBM carveout is
 * scrubbed and re-written, so a cached layout is only valid for one
 * incarnation of the region.  Only called on v3 onwards; elsewhere the reader
 * stays IDLE.  Safe to call from any context.
 *
 * @nd: neuron device
 */
void fw_layout_request_renegotiate(struct neuron_device *nd);

/**
 * fw_layout_get_entry() - copy the cached payload of an entry.
 *
 * Instances of a duplicated type_id are enumerated in directory order; per the
 * layout contract, callers must identify a specific instance by the index
 * field carried inside the payload (e.g. tpb_index), not by this position.
 *
 * The copy is a raw snapshot of the cache, taken under the lock so it cannot
 * be torn by a refresh.  Whether it is internally consistent (the head/tail
 * generation guards of a gen-guarded type) is for the type's parser to check,
 * see neuron_fw_metrics_common.h.
 *
 * @nd: neuron device
 * @type_id: payload type to look up
 * @instance: Nth descriptor with this type_id (0 based)
 * @buf: destination buffer
 * @buf_size: destination buffer size; the copy is truncated to this
 *
 * Return: number of bytes copied on success, -ENODEV if the layout is not
 * active, -ENOENT if no such entry, -ENODATA if the entry's geometry is
 * unusable and it is not cached.
 */
int fw_layout_get_entry(struct neuron_device *nd, u16 type_id, u32 instance, void *buf, u32 buf_size);

#endif

