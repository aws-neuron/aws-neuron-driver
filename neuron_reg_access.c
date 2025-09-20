// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#include "neuron_dhal.h"


inline int reg_read32(const u32 __iomem *addr, u32 *value)
{
	return ndhal->ndhal_reg_access.reg_read32_array((void **)&addr, value, 1);
}

inline void reg_write32(u32 __iomem *addr, u32 value)
{
	writel(value, addr);
}

/* take bits selected by mask from one data, the rest from background */
#define MASK_VAL(mask, data, background) (((mask) & (data)) | ((~(mask)) & (background)))

inline int reg_write32_masked(u32 __iomem *addr, u32 mask, u32 data)
{
	u32 temp;
	int ret;

	ret = reg_read32(addr, &temp);
	if (ret)
		return ret;

	reg_write32(addr, MASK_VAL(mask, data, temp));
	return 0;
}
