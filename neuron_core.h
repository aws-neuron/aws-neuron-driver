// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_NOTIFICATION_H
#define NEURON_NOTIFICATION_H

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

#define MAX_NQ_TYPE 4
#define MAX_NQ_ENGINE 4
#define NQ_TYPE_PER_ENGINE 4

#define MAX_NQ_SUPPORTED (MAX_NQ_TYPE * MAX_NQ_ENGINE)

/**
 * nc_get_nq_mmap_offset() - Get notification queue's mmap offset for given neuron core.
 *
 * @nc_id: neuron core index.
 * @engine_index: engine index in the neuron core.
 * @nq_type: notification type.
 * @offset: mmap offset for the notification queue is stored here.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_get_nq_mmap_offset(int nc_id, int engine_index, int nq_type, u64 *offset);

/**
 * nc_get_nq_from_mmap_offset() - Get notification queue's index from given mmap offset.
 *
 * @offset: mmap offset.
 * @nc_id: neuron core index which is mapped by this mmap offset is updated here.
 * @engine_index: engine index in the neuron core which mapped by this mmap offset is updated here.
 * @nq_type: notification type for the engine is updated here.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_get_nq_from_mmap_offset(u64 offset, int *nc_id, int *engine_index, int *nq_type);

/**
 * nc_nq_init() - Initialize notification queue.
 *
 * @nd: neuron device
 * @nc_id: core index in the device
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @size: size of queue
 *
 * Return: 0 on if initialization succeeds, a negative error code otherwise.
 */
int nc_nq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size);

/**
 * nc_nq_destroy() - Cleanup and free notification queue.
 *
 * @nd: neuron device
 * @nc_id: core index in the device
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_nq_destroy(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type);

/**
 * nc_nq_destroy_all() - Disable notification in the device
 *
 * @nd: neuron device
 *
 */
void nc_nq_destroy_all(struct neuron_device *nd);

/**
 * nc_nq_mmap() - mmap the notification queue into process address space.
 *
 * @nd: neuron device
 * @nc_id: core index in the device
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @vma: mmap area.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_nq_mmap(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type,
	       struct vm_area_struct *vma);

#endif
