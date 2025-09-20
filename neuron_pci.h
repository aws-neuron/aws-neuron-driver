// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Manages Neuron device's PCI configuration such as BAR access and MSI interrupt setup. */

#ifndef NEURON_PCI_H
#define NEURON_PCI_H

#include "neuron_device.h"

#define BAR_UNUSED -1

extern struct neuron_device *neuron_devices[MAX_NEURON_DEVICE_COUNT];
extern int total_neuron_devices;
extern int wc_enable;

/**
 * neuron_pci_get_device() - Returns devices associated with given index.
 *
 * @device_index: device index
 *
 * Return: NULL if device does not exists, neuron_device otherwise.
 */
struct neuron_device *neuron_pci_get_device(u8 device_index);

/**
 * neuron_pci_module_init() - Initialize Neuron PCI driver.
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise. 
 */
int neuron_pci_module_init(void);

/**
 * neuron_pci_module_exit() - Neuron PCI driver exit.
 */
void neuron_pci_module_exit(void);

#endif