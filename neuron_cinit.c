// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Creates a thread which handles the core init state of the app
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/delay.h>

#include "neuron_ioctl.h"
#include "neuron_device.h"


#define NCI_RETRY_COUNT 10
#define NCI_RETRY_SLEEP_MS 500

static inline void nci_start(struct neuron_device *nd, u32 nc_id)
{
	nd->nci[nc_id].state = NEURON_CINIT_STATE_STARTED;
}

static void nci_wait(struct neuron_device *nd, u32 nc_id)
{
	int i;

	for (i = 0; i < NCI_RETRY_COUNT; i++) {
		if (nd->nci[nc_id].state == NEURON_CINIT_STATE_COMPLETED)
			return;
		msleep(NCI_RETRY_SLEEP_MS);
	}
	return;
}

void nci_set_state(struct neuron_device *nd, u32 nc_id, u32 state, u32 *new_state)
{
	struct neuron_cinit *nci = &nd->nci[nc_id];
	u32 current_state;

	if (state == NEURON_CINIT_STATE_STARTED) {
		mutex_lock(&nci->nci_lock);
		current_state = nci->state;
		if (current_state == NEURON_CINIT_STATE_INVALID)
			nci_start(nd, nc_id);
		mutex_unlock(&nci->nci_lock);
		if (current_state == NEURON_CINIT_STATE_STARTED) {
			nci_wait(nd, nc_id);
		}
	} else if (state == NEURON_CINIT_STATE_COMPLETED) {
		mutex_lock(&nci->nci_lock);
		current_state = nci->state;
		if (nci->state == NEURON_CINIT_STATE_STARTED)
			nci->state = NEURON_CINIT_STATE_COMPLETED;
		else
			pr_err("nd%d nc:%d can't set init state to complete without starting\n", nd->device_index, nc_id);
		mutex_unlock(&nci->nci_lock);
	} else {
		pr_err("nd%d nc:%d invalid set init state\n", nd->device_index, nc_id);
	}
	*new_state = nci->state;
	return;
}

void nci_reset_state_nc(struct neuron_device *nd, u32 nc_id)
{
	struct neuron_cinit *nci;
	u32 current_state;

	nci = &nd->nci[nc_id];
	mutex_lock(&nci->nci_lock);
	current_state = nci->state;
	nci->state = NEURON_CINIT_STATE_INVALID;
	mutex_unlock(&nci->nci_lock);
}

void nci_reset_state(struct neuron_device *nd)
{
	int i;
	for (i = 0; i < MAX_NC_PER_DEVICE; i++) {
		nci_reset_state_nc(nd, i);
	}
}
