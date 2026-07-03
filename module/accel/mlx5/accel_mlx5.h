/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/rpc.h"

struct accel_mlx5_attr {
	/* The number of entries in qp submission/receive queue */
	uint16_t qp_size;
	/* The number of entries in completion queue */
	uint16_t cq_size;
	/* The number of requests in the global pool */
	uint32_t num_requests;
	/* Comma separated list of allowed device names */
	char *allowed_devs;
	/* Apply crypto operation for each X data blocks. Works only if multiblock crypto operation is supported by HW.
	 * 0 means no limit */
	uint16_t crypto_split_blocks;
	/* The number of tasks to be submitted to a single device before switching to the next device */
	uint16_t dev_rr_threshold;
	/* Enables accel_mlx5 platform driver. The driver can execute a limited scope of operations */
	bool enable_driver;
	bool qp_per_domain;
	/* Enable accel_mlx5 module */
	bool enable_module;
	/* Disable signature capabilities */
	bool disable_signature;
	/* Disable crypto capabilities */
	bool disable_crypto;
	/* Enable UMR Memory Buffer */
	bool umr_memory_buffer;
};

enum accel_mlx5_dump_state_level {
	/** Dump only grand total statistics */
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
