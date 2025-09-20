// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#if !defined(NEURON_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define NEURON_TRACE_H

#include <linux/tracepoint.h>

#include "neuron_device.h"

#undef TRACE_SYSTEM
#define TRACE_SYSTEM neuron

TRACE_EVENT(dma_engine_init,
	TP_PROTO(struct neuron_device *nd, u32 eng_id),
	TP_ARGS(nd, eng_id),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(u32,	        eng_id)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->eng_id = eng_id;
	),
	TP_printk("nd%d eng%d",
		__entry->device_index,
		__entry->eng_id
	));


TRACE_EVENT(dma_queue_init,
	TP_PROTO(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		 u32 rx_desc_count, struct mem_chunk *tx_mc,
		 struct mem_chunk *rx_mc, struct mem_chunk *rxc_mc, u32 port),
	TP_ARGS(nd, eng_id, qid, tx_desc_count, rx_desc_count, tx_mc, rx_mc, rxc_mc, port),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(u32,	        eng_id)
		__field(u32,	        qid)
		__field(u32,	        tx_desc_count)
		__field(u32,	        rx_desc_count)
		__field(struct mem_chunk *,	        tx_mc)
		__field(struct mem_chunk *,	        rx_mc)
		__field(struct mem_chunk *,	        rxc_mc)
		__field(u32,	        port)
		),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->eng_id = eng_id;
		__entry->qid = qid;
		__entry->tx_desc_count = tx_desc_count;
		__entry->rx_desc_count = rx_desc_count;
		__entry->tx_mc = tx_mc;
		__entry->rx_mc = rx_mc;
		__entry->rxc_mc = rxc_mc;
		__entry->port = port;
	),
	TP_printk("nd%d eng%d q%d tx_count %d rx_count %d tx %llx rx %llx rxc %llx port %d",
		__entry->device_index,
		__entry->eng_id,
		__entry->qid,
		__entry->tx_desc_count,
		__entry->rx_desc_count,
		__entry->rx_mc->pa,
		__entry->tx_mc->pa,
		__entry->rxc_mc == NULL ? 0 : __entry->rxc_mc->pa,
		__entry->port
	));

TRACE_EVENT(dma_queue_release,
	TP_PROTO(struct neuron_device *nd, u32 eng_id, u32 qid),
	TP_ARGS(nd, eng_id, qid),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(u32,	        eng_id)
		__field(u32,	        qid)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->eng_id = eng_id;
		__entry->qid = qid;
	),
	TP_printk("nd%d eng%d q%d",
		__entry->device_index,
		__entry->eng_id,
		__entry->qid
	));

TRACE_EVENT(dma_desc_copy,
	TP_PROTO(struct neuron_device *nd, u32 eng_id, u32 qid, enum neuron_dma_queue_type desc_type,
		 void *buffer, u32 src_offset, u32 dst_offset, u32 size),
	TP_ARGS(nd, eng_id, qid, desc_type, buffer, src_offset, dst_offset, size),
	TP_STRUCT__entry(
		__field(u32, device_index)
		__field(u32, eng_id)
		__field(u32, qid)
		__field(enum neuron_dma_queue_type,	desc_type)
		__field(void *,	buffer)
		__field(u32, src_offset)
		__field(u32, dst_offset)
		__field(u64, size)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->eng_id = eng_id;
		__entry->qid = qid;
		__entry->desc_type = desc_type;
		__entry->buffer = buffer;
		__entry->src_offset = src_offset;
		__entry->dst_offset = dst_offset;
		__entry->size = size;
	),
	TP_printk("nd%d eng%d q%d type %d src %p src_offset %x dst_offset %x size %llx",
		__entry->device_index,
		__entry->eng_id,
		__entry->qid,
		__entry->desc_type,
		__entry->buffer,
		__entry->src_offset,
		__entry->dst_offset,
		__entry->size
	));


TRACE_EVENT(dma_queue_copy_start,
	TP_PROTO(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		 u32 rx_desc_count),
	TP_ARGS(nd, eng_id, qid, tx_desc_count, rx_desc_count),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(u32,	        eng_id)
		__field(u32,	        qid)
		__field(u32,	        tx_desc_count)
		__field(u32,	        rx_desc_count)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->eng_id = eng_id;
		__entry->qid = qid;
		__entry->tx_desc_count = tx_desc_count;
		__entry->rx_desc_count = rx_desc_count;
	),
	TP_printk("nd%d eng%d q%d tx_count %d rx_count %d",
		__entry->device_index,
		__entry->eng_id,
		__entry->qid,
		__entry->tx_desc_count,
		__entry->rx_desc_count
	));

TRACE_EVENT(dma_ack_completed,
	TP_PROTO(struct neuron_device *nd, u32 eng_id, u32 qid, u32 count),
	TP_ARGS(nd, eng_id, qid, count),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(u32,	        eng_id)
		__field(u32,	        qid)
		__field(u32,	        count)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->eng_id = eng_id;
		__entry->qid = qid;
		__entry->count = count;
	),
	TP_printk("nd%d eng%d qid%d count %d",
		__entry->device_index,
		__entry->eng_id,
		__entry->qid,
		__entry->count
	));


TRACE_EVENT(ioctl_mem_alloc,
	TP_PROTO(struct neuron_device *nd, struct mem_chunk *mc),
	TP_ARGS(nd, mc),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(struct mem_chunk*,	        mc)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->mc = mc;
	),
	TP_printk("nd%d nc%d %s:%llx (%lld bytes) channel %d region %d mc %p",
		__entry->device_index,
		__entry->mc->nc_id,
		__entry->mc->mem_location == MEM_LOC_HOST ? "HOST": "DEVICE",
		__entry->mc->pa,
		__entry->mc->size,
		__entry->mc->dram_channel,
		__entry->mc->dram_region,
		__entry->mc
	));

TRACE_EVENT(ioctl_mem_free,
	TP_PROTO(struct neuron_device *nd, struct mem_chunk *mc),
	TP_ARGS(nd, mc),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(struct mem_chunk*,	        mc)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->mc = mc;
	),
	TP_printk("nd%d nc%d %s:%llx (%lld bytes) mc %p",
		__entry->device_index,
		__entry->mc->nc_id,
		__entry->mc->mem_location == MEM_LOC_HOST ? "HOST": "DEVICE",
		__entry->mc->pa,
		__entry->mc->size,
		__entry->mc
));

TRACE_EVENT(ioctl_mem_copy,
	TP_PROTO(struct neuron_device *nd, struct mem_chunk *src, struct mem_chunk *dst),
	TP_ARGS(nd, src, dst),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(struct mem_chunk*,	        src)
		__field(struct mem_chunk*,	        dst)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->src = src;
		__entry->dst = dst;
	),
	TP_printk("nd%d nc%d %s:%#llx (%lld bytes) => nc%d %s:%#llx",
		__entry->device_index,
		__entry->src->nc_id,
		__entry->src->mem_location == MEM_LOC_HOST ? "HOST": "DEVICE",
		__entry->src->pa,
		__entry->src->size,
		__entry->dst->nc_id,
		__entry->dst->mem_location == MEM_LOC_HOST ? "HOST": "DEVICE",
		__entry->dst->pa
	));

TRACE_EVENT(ioctl_mem_copyin,
	TP_PROTO(struct neuron_device *nd, struct mem_chunk *mc, void *buffer, u32 offset, u32 size),
	TP_ARGS(nd, mc, buffer, offset, size),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(struct mem_chunk*,	        mc)
		__field(void*,	                buffer)
		__field(u32,	        offset)
		__field(u32,	        size)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->mc = mc;
		__entry->buffer = buffer;
		__entry->size = size;
	),
	TP_printk("nd%d %p (%d bytes) => nc%d %s:%#llx+%#x",
		__entry->device_index,
		__entry->buffer,
		__entry->size,
		__entry->mc->nc_id,
		__entry->mc->mem_location == MEM_LOC_HOST ? "HOST": "DEVICE",
		__entry->mc->pa,
		__entry->offset
	));

TRACE_EVENT(ioctl_mem_copyout,
	TP_PROTO(struct neuron_device *nd, struct mem_chunk *mc, void *buffer, u32 offset, u32 size),
	TP_ARGS(nd, mc, buffer, offset, size),
	TP_STRUCT__entry(
		__field(u32,	        device_index)
		__field(struct mem_chunk*,	        mc)
		__field(void*,	                buffer)
		__field(u32,	        offset)
		__field(u32,	        size)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->mc = mc;
		__entry->buffer = buffer;
		__entry->size = size;
	),
	TP_printk("nd%d nc%d %s:%#llx+%#x (%d bytes)=> %p",
		__entry->device_index,
		__entry->mc->nc_id,
		__entry->mc->mem_location == MEM_LOC_HOST ? "HOST": "DEVICE",
		__entry->mc->pa,
		__entry->offset,
		__entry->size,
		__entry->buffer
	));

TRACE_EVENT(dma_memcpy,
	TP_PROTO(struct neuron_device *nd, u32 nc_id, u64 src, u64 dst, u32 size,
		 u32 pending_transfers),
	TP_ARGS(nd, nc_id, src, dst, size, pending_transfers),
	TP_STRUCT__entry(
		__field(u32,              device_index)
		__field(u32,              nc_id)
		__field(u64,              src)
		__field(u64,              dst)
		__field(u32,              size)
		__field(u32,              pending_transfers)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->nc_id = nc_id;
		__entry->src = src;
		__entry->dst = dst;
		__entry->size = size;
		__entry->pending_transfers = pending_transfers;
	),
	TP_printk("nd%d nc%d src %llx dst %llx (%d bytes) pending %d",
		__entry->device_index,
		__entry->nc_id,
		__entry->src,
		__entry->dst,
		__entry->size,
		__entry->pending_transfers
	));

TRACE_EVENT(bar_write,
	TP_PROTO(struct neuron_device *nd, u32 bar, u64 offset, u32 data),
	TP_ARGS(nd, bar, offset, data),
	TP_STRUCT__entry(
		__field(u32,              device_index)
		__field(u32,              bar)
		__field(u64,              offset)
		__field(u32,              data)
		),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->bar = bar;
		__entry->offset = offset;
		__entry->data = data;
	),
	TP_printk("nd%d bar%d offset:%llx data:%x",
		__entry->device_index,
		__entry->bar,
		__entry->offset,
		__entry->data
	));

TRACE_EVENT(bar_read,
	TP_PROTO(struct neuron_device *nd, u32 bar, u64 offset, u32 data),
	TP_ARGS(nd, bar, offset, data),
	TP_STRUCT__entry(
		__field(u32,              device_index)
		__field(u32,              bar)
		__field(u64,              offset)
		__field(u32,              data)
	),
	TP_fast_assign(
		__entry->device_index = nd->device_index;
		__entry->bar = bar;
		__entry->offset = offset;
		__entry->data = data;
	),
	TP_printk("nd%d bar%d offset:%llx data:%x",
		__entry->device_index,
		__entry->bar,
		__entry->offset,
		__entry->data
	));

#define METRICS_MAX_VALUE_LENGTH	128

TRACE_EVENT(metrics_post,
	TP_PROTO(u32 id, u32 length, const char *value),
	TP_ARGS(id, length, value),
	TP_STRUCT__entry(
		__field(u32,              id)
		__field(u32,              length)
		__array(char, value, METRICS_MAX_VALUE_LENGTH)
	),
	TP_fast_assign(
		__entry->id = id;
		__entry->length = length;
		memcpy(__entry->value, value, length);
		__entry->value[length] = 0;
	),
	TP_printk("%u:%u:%s",
	__entry->id,
	__entry->length,
	__entry->value
));
#endif

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE neuron_trace
#include <trace/define_trace.h>
