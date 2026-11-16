/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/util.h"
#include "spdk/stdinc.h"
#include <sys/random.h>

uint64_t
spdk_rand_xorshift64_seed(void)
{
	uint64_t seed = 0;
	ssize_t rc;

	rc = getrandom(&seed, sizeof(uint64_t), 0);
	if (rc != sizeof(uint64_t) || seed == 0) {
		/* getrandom failed, use clock_gettime as a fallback */
		struct timespec ts;

		seed = 0;
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
			seed = (((uint64_t) ts.tv_sec << 32) | ((uint64_t) ts.tv_nsec)) ^ (uint64_t)&ts;
		}
	}
	if (seed == 0) {
		/* getrandom and clock_gettime failed, use a default seed */
		seed = 1;
	}

	return seed;
}
