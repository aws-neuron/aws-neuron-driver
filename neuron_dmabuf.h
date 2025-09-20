// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DMABUF_H
#define NEURON_DMABUF_H

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#include <linux/dma-buf.h>
#endif
#include "neuron_mmap.h"

/**
 * Get the anonymous file-descriptor of dma-buf associated with
 * a Neuron device memory region if it was registered for EFA peer direct
 *
 * @addr: Device buffer virtual address
 * @size: Device buffer size (in bytes)
 * @fd: dma-buf fd
 *
 * Return: 0 on success, error code on failure
 */
int ndmabuf_get_fd(u64 va, u64 size, int *fd);

#endif
