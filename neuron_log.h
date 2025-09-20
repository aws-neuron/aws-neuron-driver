
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 */

#ifndef NEURON_LOG_H
#define NEURON_LOG_H

#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/atomic.h>

struct neuron_device;

enum neuron_log_type {
	NEURON_LOG_TYPE_INVALID    =  0, //
	NEURON_LOG_TYPE_FILE_OPEN  =  1, // log entry for a file open { data = file ptr }
	NEURON_LOG_TYPE_FILE_FLUSH =  2, // log entry for a file close { data = file ptr }
	NEURON_LOG_TYPE_FILE_IOCTL =  3, // log entry for a ioctl { data = ioctl }
	NEURON_LOG_TYPE_FILE_MMAP  =  4  // log entry for a mmap  { data = ioctl }
};

struct neuron_log_rec {
	enum neuron_log_type type;    // type of log entry
	pid_t                pid;     // process we are logging for
	uint64_t             data;    // type specific log data
	uint64_t             jiffies; // jiffies stamp to give us an idea when the entry was logged.
};

struct neuron_log_obj {
	atomic_t tail;
	struct neuron_log_rec * log;
};


/**
 * neuron_log_init() - initialize logging for this neuron device
 *
 * @nd: Neuron device which we are initializing logging for
 *
 * Return: 0 on success, -1 on failure
 */
int neuron_log_init(struct neuron_device *nd);

/**
 * neuron_log_destroy() - cleanup logging for this neuron device
 *
 * @nd: Neuron device which we are cleaningup logging for
 */
void neuron_log_destroy(struct neuron_device *nd);

/**
 * neuron_log_rec_add() - add a record to the log
 *
 * @nd: Neuron device which we are cleaningup logging for
 * @type:  type of log entry
 * @data:  type specific data 
 */
void neuron_log_rec_add(struct neuron_device *nd, enum neuron_log_type type, uint64_t data);

/**
 * neuron_log_dump() - dump the log for this neuron device
 *
 * @nd:   Neuron device which we are dumping the log for
 * @pid:  optional id of the process to filter on for the log dump 
 *
 * Return: 0 on success, -1 on failure
 */
int neuron_log_dump(struct neuron_device *nd, pid_t pid, uint32_t log_dump_limit);

#endif
