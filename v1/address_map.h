// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __V1_ADDR_MAP_H__

// Base addresses(device side) and sizes of hardware block.

// Port 0 base
#define P_0_BASE 0x00000000000000ull
// Port 1 base
#define P_1_BASE 0x00100000000000ull

// NC registers
#define V1_MMAP_TPB_OFFSET 0x000fffc0000000ull

// APB registers
#define P_0_APB_BASE 0x000ffff0000000ull
// Misc RAM
#define P_0_APB_MISC_RAM_BASE 0x000ffff078e000ull

// DRAM Channel 0
#define P_0_DRAM_0_BASE 0x00000000000000ull
#define P_0_DRAM_0_SIZE 0x00000100000000ull
// DRAM Channel 1
#define P_0_DRAM_1_BASE 0x00001000000000ull
#define P_0_DRAM_1_SIZE 0x00000100000000ull

// NC0 putils block
#define P_0_APB_NC_0_PUTILS_RELBASE 0x00000000404000ull
// NC1 putils block
#define P_0_APB_NC_1_PUTILS_RELBASE 0x00000000414000ull

// UDMA engine offsets
#define P_0_APB_TENG_0_UDMA_0_RELBASE 0x00000000040000ull
#define P_0_APB_TENG_1_UDMA_0_RELBASE 0x00000000140000ull
#define P_0_APB_TENG_2_UDMA_0_RELBASE 0x00000000240000ull
#define P_0_APB_TENG_3_UDMA_0_RELBASE 0x00000000340000ull

#define P_0_APB_TENG_0_UDMA_0_SIZE 0x00000000040000ull

// TDMA engine offsets
#define P_0_APB_TENG_0_TDMA_0_RELBASE 0x00000000000000ull
#define P_0_APB_TENG_1_TDMA_0_RELBASE 0x00000000100000ull
#define P_0_APB_TENG_2_TDMA_0_RELBASE 0x00000000200000ull
#define P_0_APB_TENG_3_TDMA_0_RELBASE 0x00000000300000ull

#define P_0_APB_TENG_0_TDMA_0_SIZE 0x00000000001000ull

// Host memory access
#define PCIEX8_0_BASE 0x00400000000000ull
// East side device
#define PCIEX4_0_BASE 0x00800000000000ull
// West side device access
#define PCIEX4_1_BASE 0x00c00000000000ull

// Relative offsets within V1_MMAP_TPB_OFFSET
#define V1_MMAP_P_OFFSET 0x00000000000000ull
#define V1_MMAP_NC_EVENT_OFFSET 0x00000002700000ull
#define V1_MMAP_NC_SEMA_READ_OFFSET 0x00000002701000ull
#define V1_MMAP_NC_SEMA_SET_OFFSET 0x00000002701100ull
#define V1_MMAP_NC_SEMA_INCR_OFFSET 0x00000002701200ull
#define V1_MMAP_NC_SEMA_DECR_OFFSET 0x00000002701300ull
#define V1_MMAP_PE_IRAM_FIFO_OFFSET 0x0000000265c000ull
#define V1_MMAP_PE_IRAM_SIZE 0x00000000004000ull

// relative to bar0
#define V1_MMAP_BAR0_APB_MISC_RAM_OFFSET 0x0000000078e000ull

#define V1_MMAP_NC_SIZE 0x00000004000000ull

// Number of dice per chip
#define V1_NUM_DIE_PER_DEVICE 1

// Number of Neuron Core per device
#define V1_NC_PER_DEVICE 4
// Number of DMA engines per NC
#define V1_DMA_ENG_PER_NC 3
// Number of DMA queues in each engine
#define V1_DMA_QUEUE_PER_ENG 16

#define V1_MAX_DMA_RINGS 16
#define V1_NUM_DMA_ENG_PER_DEVICE (V1_NC_PER_DEVICE * V1_DMA_ENG_PER_NC)


// max channels supported by v1 device
#define V1_MAX_DRAM_CHANNELS 2
#define V1_MAX_DDR_REGIONS V1_NC_PER_DEVICE

#define V1_SEMAPHORE_COUNT 32
#define V1_EVENTS_COUNT 256

#define V1_ALLOWED_DESC_PER_PACKET 128

#define V1_MAX_NQ_TYPE 4
#define V1_MAX_NQ_ENGINE 4
#define V1_MAX_NQ_SUPPORTED (V1_MAX_NQ_TYPE * V1_MAX_NQ_ENGINE)

#endif
