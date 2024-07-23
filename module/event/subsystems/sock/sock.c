/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk/init.h"
#include "spdk/config.h"

static void
sock_subsystem_init(void)
{
	spdk_sock_initialize();
	spdk_subsystem_init_next(0);
}

static void
sock_subsystem_fini(void)
{
	spdk_subsystem_fini_next();
}

static void
sock_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_sock_write_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_sock = {
	.name = "sock",
	.init = sock_subsystem_init,
	.fini = sock_subsystem_fini,
	.write_config_json = sock_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_sock);
#ifdef SPDK_CONFIG_XLIO
SPDK_SUBSYSTEM_DEPEND(sock, xlio);
#endif
