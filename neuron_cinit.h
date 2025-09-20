// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef NEURON_CINIT_H
#define NEURON_CINIT_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include "share/neuron_driver_shared.h"


struct neuron_cinit {
	struct mutex nci_lock;
	volatile enum neuron_cinit_state state; // state of cinit
};

/**
 * nci_set_state() - Sets the state
 *
 * @nd: Neuron device
 * @nc_id: Neuron Core index
 * @state: state to set
 * @new_state: current state after the set
 */
void nci_set_state(struct neuron_device *nd, u32 nc_id, u32 state, u32 *new_state);

/**
 * nci_reset_state() - Resets the device init state to invalid
 *
 * @nd: Neuron device
 */
void nci_reset_state(struct neuron_device *nd);

/**
 * nci_reset_state_nc() - Resets the core init state to invalid
 *
 * @nd: Neuron device
 * @nc_id: Neuron Core index
 */
void nci_reset_state_nc(struct neuron_device *nd, u32 nc_id);

#endif
