// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __PUTILS_H__

#define __PUTILS_H__

/** Neuron device can generate 4 type of notifications during program execution
 * 1. Errors - Any accelerator generated error such as infinity, NaN
 * 2. Events - Accelerator set or cleared an event.
 * 3. Explicit - Program had an instruction which explicitly generated notification.
 * 4. Implicit - If configured all instructions would generate notification.
 *
 * Each NeuronCore has one error and event notification queue and multiple
 * implicit and explicit notification queues.
 *
 * The notifications are stored in a circular buffer.
 * The following functions enables setting up the circular buffer.
 */

#include "address_map.h"
#include "../neuron_reg_access.h"

/** Returns PUTILS relative offset for given the NC.
 */
static inline u64 pu_get_relative_offset(int nc_idx)
{
	// NC blocks are the same distance apart, use the first 2
	u64 bs = P_0_APB_NC_1_PUTILS_RELBASE - P_0_APB_NC_0_PUTILS_RELBASE;
	return P_0_APB_NC_0_PUTILS_RELBASE + (nc_idx * bs);
}

/** Set base address(low 32bit) of error notification.
 */
static inline void pu_write_error_notification_cfg_0(void __iomem *base, u32 value)
{
	reg_write32(base + 0x1074, value);
}

/** Set base address(high 32bit) of error notification.
 */
static inline void pu_write_error_notification_cfg_1(void __iomem *base, u32 value)
{
	reg_write32(base + 0x1078, value);
}

/** Set error notification queue size.
 */
static inline void pu_write_error_notification_cfg_2(void __iomem *base, u32 value)
{
	reg_write32(base + 0x107c, value);
}

/** Set base address(low 32bit) of event notification.
 */
static inline void pu_write_event_notification_cfg_0(void __iomem *base, u32 value)
{
	reg_write32(base + 0x1094, value);
}

/** Set base address(high 32bit) of event notification.
 */
static inline void pu_write_event_notification_cfg_1(void __iomem *base, u32 value)
{
	reg_write32(base + 0x1098, value);
}

/** Set event notification queue size.
 */
static inline void pu_write_event_notification_cfg_2(void __iomem *base, u32 value)
{
	reg_write32(base + 0x109c, value);
}

#define PU_INSTANCE_SIZE 0x104
#define PU_EXPL_NOTIFICATION_QUEUE_SIZE 0x20
#define PU_EXPL_NOTIFICATION_OFFSET(instance_num, queue)                                           \
	(((instance_num)*PU_INSTANCE_SIZE) + ((queue)*PU_EXPL_NOTIFICATION_QUEUE_SIZE))

/** Set base address(low 32bit) of explicit notification queue for given instance and queue.
 */
static inline void pu_write_expl_notification_cfg_0(void __iomem *base, u8 instance, u8 queue,
						    u32 value)
{
	u32 offset = 0xbfc + PU_EXPL_NOTIFICATION_OFFSET(instance, queue);
	reg_write32(base + offset, value);
}

/** Set base address(high 32bit) of explicit notification queue for given instance and queue.
 */
static inline void pu_write_expl_notification_cfg_1(void __iomem *base, u8 instance, u8 queue,
						    u32 value)
{
	u32 offset = 0xc00 + PU_EXPL_NOTIFICATION_OFFSET(instance, queue);
	reg_write32(base + offset, value);
}

/** Set explicit notification queue size.
 */
static inline void pu_write_expl_notification_cfg_2(void __iomem *base, u8 instance, u8 queue,
						    u32 value)
{
	const size_t offset = 0xc04 + PU_EXPL_NOTIFICATION_OFFSET(instance, queue);
	reg_write32(base + offset, value);
}

#define PU_IMPL_NOTIFICATION_QUEUE_SIZE 0x20
#define PU_IMPL_NOTIFICATION_OFFSET(instance_num, queue)                                           \
	(((instance_num)*PU_INSTANCE_SIZE) + ((queue)*PU_IMPL_NOTIFICATION_QUEUE_SIZE))

/** Set base address(low 32bit) of implicit notification queue for given instance and queue.
*/
static inline void pu_write_impl_notification_cfg_0(void __iomem *base, u8 instance, u8 queue,
						    u32 value)
{
	const size_t offset = 0xc7c + PU_IMPL_NOTIFICATION_OFFSET(instance, queue);
	reg_write32(base + offset, value);
}

/** Set base address(high 32bit) of implicit notification queue for given instance and queue.
*/
static inline void pu_write_impl_notification_cfg_1(void __iomem *base, u8 instance, u8 queue,
						    u32 value)
{
	const size_t offset = 0xc80 + PU_IMPL_NOTIFICATION_OFFSET(instance, queue);
	reg_write32(base + offset, value);
}

/** Set implicit notification queue size.
 */
static inline void pu_write_impl_notification_cfg_2(void __iomem *base, u8 instance, u8 queue,
						    u32 value)
{
	const size_t offset = 0xc84 + PU_IMPL_NOTIFICATION_OFFSET(instance, queue);
	reg_write32(base + offset, value);
}

#endif
