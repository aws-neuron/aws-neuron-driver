// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Exposes device node interface(/dev/neuron0) for each device.
 *  see neuron_ioctl.h for all the operations that can be done this node.
 */

#ifndef NEURON_CDEV_H
#define NEURON_CDEV_H

#include "neuron_device.h"

#ifndef RHEL_RELEASE_VERSION
#define RHEL_RELEASE_VERSION(a,b) 1
#endif

#define MMAP_BAR0_APB_MISC_RAM_INVALID -1
#define NCDEV_MEM_REGION_INVALID -1 // mark the end of ncdev_mem_regions array

struct ncdev_mem_region {
    u64 start;
    u64 size;
};

/**
 * ncdev_create_device_node() - Create a neuron device node
 * 
 * @param ndev: the device node to be created
 * @return int: return 0 on success, otherwise failure
 */
int ncdev_create_device_node(struct neuron_device *ndev);

/**
 * ncdev_delete_device_node() - Remove a neuron device node
 * 
 * @param ndev: the neuron device to be deleted
 * @return int: return 0 on success, otherwise failure
 */
int ncdev_delete_device_node(struct neuron_device *ndev);

/**
 * ncdev_module_init() - Initialize the kernel module that creates the character devices
 * 
 * @return int: return 0 on success, otherwise failure
 */
int ncdev_module_init(void);

/**
 * ncdev_module_exit() - Clean up and release resources associated with the character devices
 * 
 */
void ncdev_module_exit(void);

#endif
