/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/rpc.h"

struct accel_mlx5_attr {
	/* Comma separated list of allowed crypto device names */
	char *allowed_devs;
	/* The number of entries in qp submission/receive queue */
	uint16_t qp_size;
	/* The number of entries in completion queue */
	uint16_t cq_size;
	/* The number of requests in the global pool */
	uint32_t num_requests;
	/* The number of data blocks to be processed in 1 UMR.
	 * 0 means no limit. HW must support multi block crypto */
	uint32_t split_mb_blocks;
	/* Ignore CQ_UPDATE flags, mark last WQE with CQ_UPDATE before updating the DB */
	bool siglast;
	/* Merge tasks in the sequence when possible. */
	bool merge;
	bool qp_per_domain;
};

enum accel_mlx5_dump_state_level {
	/** Dump only grand total statistics*/
	ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL,
	/** Dump grand total statistics and per channel statistics over all devices */
	ACCEL_MLX5_DUMP_STAT_LEVEL_CHANNEL,
	/** Dump grand total statistics and per channel statistics for each individual device */
	ACCEL_MLX5_DUMP_STAT_LEVEL_DEV
};

typedef void(*accel_mlx5_dump_stat_done_cb)(void *ctx, int rc);

void accel_mlx5_get_default_attr(struct accel_mlx5_attr *attr);
int accel_mlx5_enable(struct accel_mlx5_attr *attr);
int accel_mlx5_dump_stats(struct spdk_json_write_ctx *w, enum accel_mlx5_dump_state_level level,
			  accel_mlx5_dump_stat_done_cb cb, void *ctx);
