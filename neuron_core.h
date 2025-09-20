// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_CORE_H
#define NEURON_CORE_H

#define NC_SEMAPHORE_SIZE 4
#define NC_EVENT_SIZE 4

/**
 * nc_semaphore_read() - Read current semaphore value
 *
 * @nd: neuron device from which semaphore needs to be read
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @result: location to store the result
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
int nc_semaphore_read(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 *result);

/**
 * nc_semaphore_write() - Write given value on a semaphore
 *
 * @nd: neuron device on which semaphore operation needs to be performed
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @value: value to set
 *
 * Return: 0 if write succeeds, a negative error code otherwise.
 */
int nc_semaphore_write(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value);

/**
 * nc_semaphore_increment() - Increment a semaphore by given value
 *
 * @nd: neuron device on which semaphore operation needs to be performed
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @value: value to increment
 *
 * Return: 0 if increment succeeds, a negative error code otherwise.
 */
int nc_semaphore_increment(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value);

/**
 * nc_semaphore_decrement() - Decrement a semaphore by given value
 *
 * @nd: neuron device on which semaphore operation needs to be performed
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @value: value to decrement
 *
 * Return: 0 if decrement succeeds, a negative error code otherwise.
 */
int nc_semaphore_decrement(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value);

/**
 * nc_event_get() - Get current value of given event
 *
 * @nd: neuron device on which event operation needs to be performed
 * @nc_id: core which has the event
 * @event_index: index of the event
 * @value: result is stored here(0 or 1)
 *
 * Return: 0 if event read succeeds, a negative error code otherwise.
 */
int nc_event_get(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 *result);

/**
 * nc_event_set() - Set or clear given event
 *
 * @nd: neuron device on which event operation needs to be performed
 * @nc_id: core which has the event
 * @event_index: index of the event
 * @value: value to set(0 or 1)
 *
 * Return: 0 if event set succeeds, a negative error code otherwise.
 */
int nc_event_set(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 value);

// followin defines have the max between versions of chip
// please check the chip's address_map.h to find the values
#define MAX_NQ_TYPE 6  //for v1 4 and v2 6
#define MAX_NQ_ENGINE 16 // for v1 4 engines for v2 16 queues

#define MAX_NQ_SUPPORTED (MAX_NQ_TYPE * MAX_NQ_ENGINE)

/**
 * nc_get_nq_mem_handle() - Get notification queue's mem handle for given neuron core.
 *
 * @nd: neuron device
 * @nc_id: neuron core index.
 * @engine_index: engine index in the neuron core.
 * @nq_type: notification type.
 * @handle: handle for the notification queue is stored here.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_get_nq_mem_handle(struct neuron_device *nd, int nc_id, int engine_index, int nq_type, u64 *handle);

/**
 * nc_get_nq_mem_handle() - Get notification queue's mem handle for given neuron core.
 *
 * @nd: neuron device
 * @nc_id: neuron core index.
 * @engine_index: engine index in the neuron core.
 * @nq_type: notification type.
 * @handle: handle for the notification queue is stored here.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_get_nq_mem_handle(struct neuron_device *nd, int nc_id, int engine_index, int nq_type, u64 *handle);

/**
 * nc_nq_device_init() - Initialize the mc's in device
 *
 * @nd: neuron device
 *
 */
void nc_nq_device_init(struct neuron_device *nd);


#endif
