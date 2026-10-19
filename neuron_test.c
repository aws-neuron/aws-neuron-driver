// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Neuron driver test module
 *
 *  the purpose of this module is to prove error injection functionality
 *  for testing.  It should be lightweight, simple and have little to no
 *  knowledge of the driver's operation.  It requires sysadmin caps
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/types.h> 
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/bitmap.h>
#include "neuron_test.h"

#define _NEURON_TT_AT_LOAD_VALBITS		16
#define _NEURON_TT_AT_LOAD_VALSHIFT		 0
#define _NEURON_TT_AT_LOAD_VALMASK		((1 << _NEURON_TT_AT_LOAD_VALBITS)-1)
#define _NEURON_TT_AT_LOAD_VAL(val)		(((val) >> _NEURON_TT_AT_LOAD_VALSHIFT) & _NEURON_TT_AT_LOAD_VALMASK)

#define _NEURON_TT_AT_LOAD_DATABITS		16
#define _NEURON_TT_AT_LOAD_DATASHIFT	16
#define _NEURON_TT_AT_LOAD_DATAMASK		((1 << _NEURON_TT_AT_LOAD_DATABITS)-1)
#define _NEURON_TT_AT_LOAD_DATA(data)	(((data) >> _NEURON_TT_AT_LOAD_DATASHIFT) & _NEURON_TT_AT_LOAD_DATAMASK)

int neuron_test_trigger_ena = 0;
int neuron_test_trigger_at_load = 0;	// loadtime testing trigger (16 bits of trigger value, 16 bits trigger data)

module_param(neuron_test_trigger_ena, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(neuron_test_trigger_ena, "test trigger enable");

module_param(neuron_test_trigger_at_load, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(neuron_test_trigger_at_load, "test trigger at load time");

DECLARE_BITMAP(neuron_test_trigger_bitmap, NEURON_TEST_TRIGGER_MAX);

// test trigger data, interpretted per trigger.  Right now just u64, but we could get more sophisticated
// if needed.
//
static u64 neuron_test_trigger_data[NEURON_TEST_TRIGGER_MAX] = {0};

void ntest_init(void)
{
	bitmap_zero(neuron_test_trigger_bitmap, NEURON_TEST_TRIGGER_MAX);

	// set any load time test triggers
	//
	if (neuron_test_trigger_at_load) {
		int val = _NEURON_TT_AT_LOAD_VAL(neuron_test_trigger_at_load);
		if (val < NEURON_TEST_TRIGGER_MAX) {
			bitmap_set(neuron_test_trigger_bitmap, val, 1);
			neuron_test_trigger_data[val] = _NEURON_TT_AT_LOAD_DATA(neuron_test_trigger_at_load);
		}
	}
}

//inline int _ntest_trigger(enum neuron_test_trigger trigger, void * trigger_data) {}


int ntest_trigger(enum neuron_test_trigger trigger, u64 trigger_data)
{
	switch (trigger) {
		case NEURON_TEST_TRIGGER_RST_FAILURE:
			if (test_bit(NEURON_TEST_TRIGGER_RST_FAILURE, neuron_test_trigger_bitmap) &&
				(trigger_data == neuron_test_trigger_data[NEURON_TEST_TRIGGER_RST_FAILURE])) {
				return 1;
			}
			break;
		default:
			break;
	}
	return 0;
}

