/*
 * Copyright (C) 2013-2015 Synopsys, Inc. All rights reserved.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>

DECLARE_GLOBAL_DATA_PTR;

int init_cache_f_r(void)
{
	flush_dcache_all();
	invalidate_dcache_all();

	/* Actually needed only in case of disabled dcache */
	invalidate_icache_all();

	return 0;
}
