
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_MC_HANDLE_H
#define NEURON_MC_HANDLE_H

#include <linux/kernel.h>

typedef uint64_t neuron_mc_handle_t;

#define NMCH_INVALID_HANDLE 0

struct mem_chunk;
struct neuron_device;

/**
 * memchunk handle map entry.
 *   entry can contain either a MC pointer or a index to a next entry in the free list
 */
typedef union nmch_map_ent {
	struct mem_chunk* mc;
	uint64_t value;
} nmch_map_ent_t;

/**
 * Memchunk handle tracking structure
 *    ptr to level 1 of a 2 level table
 *    index of the free list head
 */
typedef struct neuron_mc_handle_map {
	struct mutex lock;
	nmch_map_ent_t **l1_tbl; // l1 table pointer
	uint64_t free;           // free list head (index based)
} neuron_mc_handle_map_t;

/**
 * nmch_handle_find() - find and return the memchunk associated with this mc handle
 *
 * @nd: Neuron device
 * @mc_handle: memchunk handle 
 *
 * Return: mc associated with mc_handle if successful, NULL on failure
 */
struct mem_chunk* mc_handle_find(struct neuron_device *nd, neuron_mc_handle_t mc_handle);

/**
 * nmch_handle_alloc() - allocate a mc map entry
 *
 * @nd: Neuron device
 * @mc: memchunk to add to the mapping table
 * @mc_handle: memchunk handle returned for the mapping
 *
 * Return: 0 on success, error code on failure 
 */
int nmch_handle_alloc(struct neuron_device *nd, struct mem_chunk *mc, neuron_mc_handle_t *mc_handle);

/**
 * nmch_handle_free() - free the mc map entry associated with this mc handle
 *
 * @nd: Neuron device
 * @mc_handle: memchunk handle 
 *
 * Return: 0 on success, error code -ENOENT if mc_handle doesn't correspond to a valid entry
 */
int nmch_handle_free(struct neuron_device *nd, neuron_mc_handle_t mc_handle);

/**
 * nmch_handle_init() - initialize mc handle map resources
 *
 * @nd: Neuron device
 *
 * Return: 0 if initialization was successfully completed, -1 otherwise.
 */
int nmch_handle_init(struct neuron_device *nd);

/**
 * nmch_handle_cleanup() - cleanup mc handle map resources
 *
 * @nd: Neuron device
 */
void nmch_handle_cleanup(struct neuron_device *nd);

#endif
