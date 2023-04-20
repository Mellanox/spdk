/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights
 * reserved.
 */

#ifndef SPDK_BDEV_QOS_LIMIT_H
#define SPDK_BDEV_QOS_LIMIT_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

#define SPDK_BDEV_QOS_LIMIT_NOT_DEFINED UINT64_MAX
#define SPDK_BDEV_QOS_TIMESLICE_IN_USEC 1000
#define SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE 1
#define SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE 512
#define SPDK_BDEV_QOS_MIN_IOS_PER_SEC		1000
#define SPDK_BDEV_QOS_MIN_BYTES_PER_SEC		(1024 * 1024)

struct bdev_qos_limit {
	/** IOs or bytes allowed per second (i.e., 1s). */
	uint64_t limit;

	/** Remaining IOs or bytes allowed in current timeslice (e.g., 1ms).
	 *  For remaining bytes, allowed to run negative if an I/O is submitted when
	 *  some bytes are remaining, but the I/O is bigger than that amount. The
	 *  excess will be deducted from the next timeslice.
	 */
	volatile int64_t remaining_this_timeslice;

	/** Minimum allowed IOs or bytes to be issued in one timeslice (e.g., 1ms). */
	uint32_t min_per_timeslice;

	/** Maximum allowed IOs or bytes to be issued in one timeslice (e.g., 1ms). */
	uint32_t max_per_timeslice;

	/** Function to check whether to queue the IO.
	 * If The IO is allowed to pass, the quota will be reduced correspondingly.
	 */
	bool (*queue_io)(struct bdev_qos_limit *limit, struct spdk_bdev_io *io);

	/** Function to rewind the quota once the IO was allowed to be sent by this
	 * limit but queued due to one of the further limits.
	 */
	void (*rewind_quota)(struct bdev_qos_limit *limit, struct spdk_bdev_io *io);
};

struct bdev_qos_limits {
	struct bdev_qos_limit rate_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
};

void bdev_qos_limits_init(struct bdev_qos_limits *limits);
void bdev_qos_limits_set(struct bdev_qos_limits *limits,
			      const uint64_t *values);
void bdev_qos_limits_update_max_quota_per_timeslice(
	struct bdev_qos_limits *limits);
bool bdev_qos_limits_queue_io(struct bdev_qos_limits *limits,
				   struct spdk_bdev_io *bdev_io);
void bdev_qos_limits_reset_quota(struct bdev_qos_limits *limits,
				      uint64_t now,
				      uint64_t timeslice_size,
				      uint64_t *last_timeslice);
bool bdev_qos_limits_check_disabled(const uint64_t *old_limits,
		const uint64_t *new_limits);

#endif /* SPDK_BDEV_QOS_LIMIT_H */
