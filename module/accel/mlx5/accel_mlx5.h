/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

struct accel_mlx5_attr {
	/* Comma separated list of allowed crypto device names */
	char *allowed_crypto_devs;
	/* The number of entries in qp submission/receive queue */
	uint16_t qp_size;
	/* The number of requests in the global pool */
	uint32_t num_requests;
	/* The number of data blocks to be processed in 1 UMR.
	 * 0 means no limit. HW must support multi block crypto */
	uint32_t split_mb_blocks;
	/* Ignore CQ_UPDATE flags, mark last WQE with CQ_UPDATE before updating the DB */
	bool siglast;
};

void accel_mlx5_get_default_attr(struct accel_mlx5_attr *attr);
int accel_mlx5_enable(struct accel_mlx5_attr *attr);
