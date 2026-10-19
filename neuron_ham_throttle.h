// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef _NEURON_HAM_THROTTLE_H
#define _NEURON_HAM_THROTTLE_H

#include <linux/types.h>

struct neuron_device;

/**
 * neuron_ham_throttle_init() - Set up periodic throttle notification queues
 *
 * @nd: neuron device
 * @return: 0 on success, negative on failure
 */
int neuron_ham_throttle_init(struct neuron_device *nd);

/**
 * neuron_ham_throttle_format() - format the aggregated periodic throttle state
 *                                for one NC into @buf.
 *
 * @nd: neuron device
 * @nc_id: neuron core / TPB index
 * @buf: sysfs output buffer
 * @len: size of @buf
 * @return: number of bytes written
 */
ssize_t neuron_ham_throttle_format(struct neuron_device *nd, int nc_id, char *buf, size_t len);

/**
 * neuron_ham_throttle_set_aggregate_count() - set how many recent entries a
 *                                             subsequent read aggregates for one NC.
 *
 * @nd: neuron device
 * @nc_id: neuron core / TPB index
 * @buf: sysfs input buffer (decimal or 0x-prefixed count)
 * @size: size of @buf
 * @return: @size on success, negative errno on parse error
 */
ssize_t neuron_ham_throttle_set_aggregate_count(struct neuron_device *nd, int nc_id,
						const char *buf, size_t size);

/**
 * neuron_ham_throttle_reprogram_nc() - Re-apply NOTIFIC routing for one NC.
 *
 * @nd: neuron device
 * @nc_id: neuron core / TPB index
 */
void neuron_ham_throttle_reprogram_nc(struct neuron_device *nd, u8 nc_id);

/**
 * neuron_ham_throttle_destroy() - Tear down periodic throttle notification queues
 *
 * @nd: neuron device
 */
void neuron_ham_throttle_destroy(struct neuron_device *nd);

#endif /* _NEURON_HAM_THROTTLE_H */

