/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev_group.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/init.h"

static void
bdev_group_subsystem_initialize(void)
{
	spdk_subsystem_init_next(0);
}

static void
bdev_group_subsystem_finish(void)
{
	spdk_subsystem_fini_next();
}

static void
bdev_group_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	spdk_bdev_group_subsystem_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_bdev_group = {
	.name = "bdev_group",
	.init = bdev_group_subsystem_initialize,
	.fini = bdev_group_subsystem_finish,
	.write_config_json = bdev_group_subsystem_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_bdev_group);
SPDK_SUBSYSTEM_DEPEND(bdev_group, bdev)
