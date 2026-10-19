// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_TEST_H
#define NEURON_TEST_H

#include <linux/types.h>

enum neuron_test_trigger {
	NEURON_TEST_TRIGGER_RST_FAILURE = 0,
	NEURON_TEST_TRIGGER_MAX = 1,
};

extern int neuron_test_trigger_ena;

void ntest_init(void);

int ntest_trigger(enum neuron_test_trigger trigger, u64 trigger_data);

static inline int _ntest_trigger(enum neuron_test_trigger trigger, u64 trigger_data)
{
	if (!neuron_test_trigger_ena) {
		return 0;
	}
	return ntest_trigger(trigger, trigger_data);
}
#endif

