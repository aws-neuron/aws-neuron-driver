/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DMA_DIAG_H
#define NEURON_DMA_DIAG_H

#include <linux/dynamic_debug.h>
#include <linux/types.h>

struct ndma_eng;
struct ndma_ring;

/**
 * ndma_diag_dump_engine() - Dump DMA engine diagnostic state on error.
 *
 * Emits a concise diagnostic snapshot of the DMA engine via pr_err,
 * covering engine state, failing queue, IOFIC interrupts, and
 * prefetch/AXI configuration. Performs register reads and formatted
 * prints only; it does not sleep, allocate, or take locks, so it is
 * safe to call from error/timeout context.
 *
 * Prefer NDMA_DIAG_DUMP_ENGINE() (below) over calling this directly.
 *
 * @eng:        Pointer to the failing DMA engine
 * @ring:       Pointer to the failing ring (provides qid)
 * @desc_count: Descriptor count that timed out / failed
 * @reason:     Caller-provided string identifying the error context
 *              (e.g. "dma_completion_timeout", "h2d_timeout"). Name the
 *              code path, not one of its callers -- a shared path such as
 *              ndma_memcpy_wait_for_completion() is reached from scrub,
 *              memcpy and memset, so a caller-specific tag would mislabel
 *              most of its dumps.
 */
void ndma_diag_dump_engine(struct ndma_eng *eng, struct ndma_ring *ring,
			   u32 desc_count, const char *reason);

/**
 * NDMA_DIAG_DUMP_ENGINE() - dynamic-debug-gated DMA diagnostic dump.
 *
 * Wraps ndma_diag_dump_engine() in a dynamic debug callsite. When the
 * callsite is disabled (the default) the static-key branch is not taken,
 * the dump function is never entered, and none of its register reads
 * execute -- so the fast path is unaffected. Enable at runtime, or before
 * the driver loads, via the kernel dynamic debug facility, e.g.:
 *
 *   # enable both diag callsites by their shared format prefix
 *   echo 'format "NEURON_DMA_DIAG dump" +p' \
 *       > /sys/kernel/debug/dynamic_debug/control
 *
 *   # or enable just one failure path by function
 *   echo 'func ndma_memcpy_wait_for_completion +p' \
 *       > /sys/kernel/debug/dynamic_debug/control
 *
 *   # pre-arm before the driver loads (modprobe.d or kernel cmdline)
 *   options neuron dyndbg="format 'NEURON_DMA_DIAG dump' +p"
 *   neuron.dyndbg="format 'NEURON_DMA_DIAG dump' +p"
 *
 * @reason must be a string literal -- it is folded into the callsite's
 * dynamic debug format string.
 */
#define NDMA_DIAG_DUMP_ENGINE(eng, ring, desc_count, reason)			\
do {										\
	DEFINE_DYNAMIC_DEBUG_METADATA(_ddm, "NEURON_DMA_DIAG dump: " reason);	\
	if (DYNAMIC_DEBUG_BRANCH(_ddm))						\
		ndma_diag_dump_engine((eng), (ring), (desc_count), (reason));	\
} while (0)

#endif /* NEURON_DMA_DIAG_H */

