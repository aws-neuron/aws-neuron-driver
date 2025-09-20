// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Creates a thread which resets the device.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/signal.h>

#include "neuron_ioctl.h"
#include "neuron_device.h"
#include "neuron_fw_io.h"
#include "neuron_dhal.h"
#include "neuron_nq.h"

int no_reset = 0;
module_param(no_reset, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(no_reset, "Dont reset device");

#define NR_DEVICE_RESET_RETRY_INTERVAL  30000   // millisecond
#define NR_TPB_RESET_RETRY_INTERVAL     10000   // millisecond


/**
 * ITER_COAL_REQS - iterate over coalesced reset requests
 *
 *     reset requests are coalesced to reduce latency
 *     caused by performing resets serially. We still need 
 *     to iterate over the coalesced requests for a variety
 *     of purposes, so use a macro to make it compact.
 *
 *     Iterates from first to last, both inclusive in range
 */
#define ITER_COAL_REQS(iter, first, last, statements)   \
    for (iter = first; ; iter = iter->next) {       	\
        {statements}                                    \
        if (iter == last) {                             \
            break;                                      \
        }                                               \
    }


int nr_msleep_stoppable(struct neuron_device *nd, uint32_t msec) 
{
	unsigned long timeout = msecs_to_jiffies(msec);

	while (timeout && !nd->nr.stop)
		timeout = schedule_timeout_interruptible(timeout);

	return jiffies_to_msecs(timeout);
}

static int nr_call_post_reset_config(struct neuron_device *nd, uint32_t nc_map, bool reset_succeeded)
{
	if (nc_map == NEURON_NC_MAP_DEVICE) {
		return ndhal->ndhal_reset.nr_post_reset_config(nd, reset_succeeded);
	}
	return 0;
}

static int nr_reset_thread_fn(void *arg)
{
	int ret;
	struct neuron_device *nd = (struct neuron_device *)arg;

	while (!kthread_should_stop() && !nd->nr.stop) {
		volatile struct neuron_reset_request *first_request = NULL;
		volatile struct neuron_reset_request *last_request = NULL;
		volatile struct neuron_reset_request *request_iter = NULL;
		uint32_t nc_map = 0;
		int coal_cnt = 0;
		enum neuron_reset_state state;
		// sleep until there is a request pending or asked to stop
		wait_event_interruptible(nd->nr.wait_queue, nd->nr.req_pending_head || nd->nr.stop);
		if (kthread_should_stop() || nd->nr.stop)
			break;

		// get the pending request list as it is now
		// mutex because nr_start_ncs could be adding a new request in parallel
		mutex_lock(&nd->nr.nr_lock);
		first_request = nd->nr.req_pending_head;
		last_request = nd->nr.req_pending_tail;
		mutex_unlock(&nd->nr.nr_lock);

		nc_map = first_request->nc_map;
		if (first_request->request_id != NEURON_RESET_REQUEST_ALL && first_request->next != NULL) {
			ITER_COAL_REQS(request_iter, first_request, last_request, {
				coal_cnt++;
				nc_map |= request_iter->nc_map; 
			})
		} else {
			last_request = first_request;
		}

		state = NEURON_RESET_STATE_STARTED;
		nd->nr.reset_start_time = get_jiffies_64();

		if (coal_cnt > 1) {
			pr_info("nd%d: Coalesced %d reset requests\n", nd->device_index, coal_cnt);
		}

		ITER_COAL_REQS(request_iter, first_request, last_request, 
					   pr_info("nd%d: initiating %s reset request %u\n", nd->device_index, 
																		 (nc_map == NEURON_NC_MAP_DEVICE) ? "device" : "TPB", 
																 		 request_iter->request_id);)

		ret = ndhal->ndhal_reset.nr_initiate_reset(nd, nc_map);
		if (ret) {
			nr_call_post_reset_config(nd, nc_map, false);
			ITER_COAL_REQS(request_iter, first_request, last_request,
				pr_info("nd%d: reset request %u failed\n", nd->device_index, request_iter->request_id);)
			state = NEURON_RESET_STATE_FAILED;
			nsysfsmetric_inc_reset_fail_count(nd);
		} else {
			// If the reset was successfully initiated the 
			// response we get back is a pass/fail and we don't need to retry.
			ret = ndhal->ndhal_reset.nr_wait_for_reset_completion(nd);
			if (ret) {
				nr_call_post_reset_config(nd, nc_map, false);
				ITER_COAL_REQS(request_iter, first_request, last_request,
					pr_info("nd%d: reset request %u was initiated, but failed to complete\n", nd->device_index, request_iter->request_id);)
				state = NEURON_RESET_STATE_FAILED;
				nsysfsmetric_inc_reset_fail_count(nd);
			} else {
				ret = ndmar_init_ncs(nd, nc_map);
				if (ret) {
					nr_call_post_reset_config(nd, nc_map, false);
					ITER_COAL_REQS(request_iter, first_request, last_request,
						pr_info("nd%d: failed to initialize dma after reset\n", nd->device_index);)
					state = NEURON_RESET_STATE_FAILED;
					nsysfsmetric_inc_reset_fail_count(nd);			
				} else {
					ret = nr_call_post_reset_config(nd, nc_map, true);
				    if (ret) {
						ITER_COAL_REQS(request_iter, first_request, last_request,
					    	pr_info("nd%d: failed to complete post reset configuration\n", nd->device_index);)
					    state = NEURON_RESET_STATE_FAILED;
					} else {
						ITER_COAL_REQS(request_iter, first_request, last_request,
							pr_info("nd%d: reset request %u completed\n", nd->device_index, request_iter->request_id);)
						state = NEURON_RESET_STATE_COMPLETED;
					}
				}
			}
		}
		nd->nr.reset_end_time = get_jiffies_64();
		mutex_lock(&nd->nr.nr_lock);
		// delete from pending list
		nd->nr.req_pending_head = last_request->next;
		if (!nd->nr.req_pending_head) {
			nd->nr.req_pending_tail = NULL;
		} else {
			nd->nr.req_pending_head->prev = NULL;
		}

		if (first_request->request_id == NEURON_RESET_REQUEST_ALL) {
			// Update the device state based on reset state, then move on
			// This path is taken by internal driver reset logic, there is no need to move
			// the request to the completion queue, since waiters will be polling device state instead
			if (state == NEURON_RESET_STATE_COMPLETED) {
				nd->device_state = NEURON_DEVICE_STATE_READY;
			} else {
				nd->device_state = NEURON_DEVICE_STATE_INVALID;
			}
			kfree((void *)first_request);
		} else {
			// add to completed list
			last_request->next = NULL;
			first_request->prev = nd->nr.req_cmpl_tail;
			if (nd->nr.req_cmpl_tail) {
				nd->nr.req_cmpl_tail->next = first_request;
			}
			nd->nr.req_cmpl_tail = last_request;
			if (!nd->nr.req_cmpl_head) {
				nd->nr.req_cmpl_head = first_request;
			}
			ITER_COAL_REQS(request_iter, first_request, last_request, request_iter->ret = state;)
		}
		mutex_unlock(&nd->nr.nr_lock);
	}
	return 0;
}

int nr_create_thread(struct neuron_device *nd)
{
	mutex_init(&nd->nr.nr_lock);
	init_waitqueue_head(&nd->nr.wait_queue);
	nd->nr.thread = kthread_run(nr_reset_thread_fn, nd, "nd%d reset", nd->device_index);
	if (IS_ERR_OR_NULL(nd->nr.thread)) {
		pr_err("nd%d reset thread creation failed\n", nd->device_index);
		return -1;
	}
	return 0;
}

static void nr_free_req_queue(volatile struct neuron_reset_request *req) {
	volatile struct neuron_reset_request *next = NULL;
	while (req) {
		next = req->next;
		kfree((void *)req);
		req = next;
	}
	return;
}

void nr_stop_thread(struct neuron_device *nd)
{
	if (nd->nr.thread == NULL)
		return;
	nd->device_state = NEURON_DEVICE_STATE_INVALID;
	nd->nr.stop = true;
	wake_up(&nd->nr.wait_queue);
	kthread_stop(nd->nr.thread); //blocks till the thread exits
	nd->nr.thread = NULL;
	nr_free_req_queue(nd->nr.req_pending_head);
	nr_free_req_queue(nd->nr.req_cmpl_head);
	mutex_destroy(&nd->nr.nr_lock);
}

// Expects nr_lock to be held
static volatile struct neuron_reset_request *nr_find_req(struct neuron_device *nd, uint32_t request_id) {
	volatile struct neuron_reset_request *curr = nd->nr.req_pending_head;
	while (curr) {
		if (curr->request_id == request_id) {
			return curr;
		}
		curr = curr->next;
	}
	curr = nd->nr.req_cmpl_head;
	while (curr) {
		if (curr->request_id == request_id) {
			return curr;
		}
		curr = curr->next;
	}
	return curr;
}

int nr_start_ncs(struct neuron_device *nd, uint32_t nc_map, uint32_t request_id)
{
	int nc_idx;
	if (no_reset) {
		// if we are in no-reset mode, we want to skip the device reset, 
		// perform the driver's reset related activities, then return so
		// that outside of not resetting HW, everything  else will look natural.
		//
		ndmar_init_ncs(nd, NEURON_NC_MAP_DEVICE);
		nr_call_post_reset_config(nd, nc_map, true);
		nd->device_state = NEURON_DEVICE_STATE_READY;
		return 0;
	}

	mutex_lock(&nd->nr.nr_lock);
	if (nr_find_req(nd, request_id)) {
		pr_err("Pending reset request for pid %u on device %u already exists!", request_id, nd->device_index);
		mutex_unlock(&nd->nr.nr_lock);
		return 1;
	}
	if (request_id == NEURON_RESET_REQUEST_ALL) {
		nd->device_state = NEURON_DEVICE_STATE_RESET;
	}
	for (nc_idx = 0; nc_idx < MAX_NC_PER_DEVICE; nc_idx++) {
		if (nc_map == NEURON_NC_MAP_DEVICE || ((1 << nc_idx) & nc_map)) {
			// After reset we want core init to be done again
			nci_reset_state_nc(nd, nc_idx);
			// Reset the model started counter
			nd->nc_model_started_count[nc_idx] = 0;
			nnq_destroy_nc(nd, nc_idx);

			nsysfsmetric_inc_reset_req_count(nd, nc_idx);
		}
	}
	struct neuron_reset_request *req = (struct neuron_reset_request *)kmalloc(sizeof(struct neuron_reset_request), GFP_KERNEL);
	if (!req) {
		pr_err("Failed to allocate memory for reset request %u for nd %u", request_id, nd->device_index);
		mutex_unlock(&nd->nr.nr_lock);
		return 1;
	}
	req->request_id = request_id;
	req->nc_map = nc_map;
	req->ret = NEURON_RESET_STATE_STARTED;
	req->next = NULL;
	req->prev = NULL;
	if (nd->nr.req_pending_tail) {
		nd->nr.req_pending_tail->next = req;
		req->prev = nd->nr.req_pending_tail;
	}
	nd->nr.req_pending_tail = req;
	if (!nd->nr.req_pending_head) {
		nd->nr.req_pending_head = req;
	}
	mutex_unlock(&nd->nr.nr_lock);
	wake_up_interruptible_sync(&nd->nr.wait_queue);

	return 0;
}

void nr_start(struct neuron_device *nd)
{
	uint32_t request_id = task_tgid_nr(current);
	nr_start_ncs(nd, NEURON_NC_MAP_DEVICE, request_id);
}

int nr_wait(struct neuron_device *nd, uint32_t request_id, bool check)
{
	volatile struct neuron_reset_request *req = NULL;
	if (no_reset) {
		return 0;
	}

	mutex_lock(&nd->nr.nr_lock);
	req = nr_find_req(nd, request_id);
	mutex_unlock(&nd->nr.nr_lock);
	if (req == NULL) {
		if (check) {
			return 0;
		} else {
			pr_err("Invalid reset request id %u", request_id);
			return 1;
		}
	}

	// TODO: be smarter about polling for completion
	// improve this to a per-request semaphore or a wait_queue
	while (req->ret == NEURON_RESET_STATE_STARTED) {
		schedule(); // yield to other processes

		if (sigismember(&current->pending.signal, SIGTERM) || sigismember(&current->pending.signal, SIGKILL)) {
			pr_info("Received termination signal while waiting for reset request id %u", request_id);
			return -EINTR;
		}
	}
	mutex_lock(&nd->nr.nr_lock);
	// Remove from list
	if (req->prev) {
		req->prev->next = req->next;
	} else {
		// first in list
		nd->nr.req_cmpl_head = req->next;
	}
	if (req->next) {
		req->next->prev = req->prev;
	} else {
		// last in list
		nd->nr.req_cmpl_tail = req->prev;
	}
	mutex_unlock(&nd->nr.nr_lock);
	enum neuron_reset_state ret = req->ret;
	kfree((void *)req);
	return ((ret == NEURON_RESET_STATE_COMPLETED) ? 0 : 1);
}

bool nr_op_in_reset_wnd(uint64_t op_start_time, struct neuron_device *nd)
{
	if (no_reset) {
		return 0;
	}

	if (time_before_eq64(nd->nr.reset_end_time, nd->nr.reset_start_time)) {
		return true;
	} else if (time_before_eq64(op_start_time, nd->nr.reset_end_time)) {
		return true;
	}

	return false;
}

int nr_initiate_reset_via_fw(struct neuron_device *nd, uint32_t nc_map, uint32_t tpb_reset_map)
{
	bool is_device_reset;
	uint32_t reset_retry_interval;
	ktime_t start_time;
	ktime_t next_reset_retry_time;
	uint32_t initial_poll_delay;
	ktime_t cur_time;
    s64 reset_time;

	is_device_reset = (nc_map == NEURON_NC_MAP_DEVICE); // do device reset or TPB reset

	/* Wait times are different for device reset and TPB reset */
	reset_retry_interval = (is_device_reset ? NR_DEVICE_RESET_RETRY_INTERVAL : NR_TPB_RESET_RETRY_INTERVAL);

	start_time = ktime_get();

	/* Send reset request to firmware */
	fw_io_initiate_reset(nd->npdev.bar0, is_device_reset, tpb_reset_map);
	next_reset_retry_time = ktime_add_ms(start_time, reset_retry_interval);

	/* V1 only. Sleep extra time before polling */
	initial_poll_delay = (nc_map == NEURON_NC_MAP_DEVICE ? 
											 ndhal->ndhal_reset.reset_device_initial_poll_delay : 
											 ndhal->ndhal_reset.reset_tpb_initial_poll_delay);
	if (nr_msleep_stoppable(nd, initial_poll_delay)) {
		return -1;
	}

	do {
		/* 
		 * After reset initiation, firmware becomes unresponsive until
		 * the device completes the reset. Wait before next polling cycle.
		 */
		if (nr_msleep_stoppable(nd, ndhal->ndhal_reset.reset_poll_interval)) {
			return -1;
		}

		/* Poll to check if firmware has acknowledged the reset request */
		if (fw_io_is_reset_initiated(nd->npdev.bar0)) {
			// Reset is done. Record the time to metrics.
			reset_time = ktime_ms_delta(ktime_get(), start_time);
			if (reset_time > 0) {
				nmetric_set_reset_time_metrics(nd, (uint64_t)reset_time, is_device_reset);
			} else {
				return -1;
			}
			return 0;
		}

		cur_time = ktime_get();
		if (ktime_after(cur_time, next_reset_retry_time)) {
			/* 
			 * If timed out, retry the reset.
			 * This handles cases where the initial/previous reset was missed.
			 */
			fw_io_initiate_reset(nd->npdev.bar0, is_device_reset, tpb_reset_map);

			next_reset_retry_time = ktime_add_ms(cur_time, reset_retry_interval);
		}
	} while (ktime_before(ktime_get(), ktime_add_ms(start_time, ndhal->ndhal_reset.initiate_max_wait_time)));

	/* Timeout reached - reset failed */
	nmetric_increment_reset_failure_count(nd, is_device_reset); // Record the reset failure in metrics
	return -1;
}
