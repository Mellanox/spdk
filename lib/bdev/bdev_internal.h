/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_INTERNAL_H
#define SPDK_BDEV_INTERNAL_H

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

#define ZERO_BUFFER_SIZE	0x100000

struct spdk_bdev;
struct spdk_bdev_io;
struct spdk_bdev_channel;
struct spdk_bdev_group;

struct spdk_bdev_io *bdev_channel_get_io(struct spdk_bdev_channel *channel);

void bdev_io_init(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb);

void bdev_io_submit(struct spdk_bdev_io *bdev_io);

struct spdk_bdev_io_stat *bdev_alloc_io_stat(bool io_error_stat);
void bdev_free_io_stat(struct spdk_bdev_io_stat *stat);

enum spdk_bdev_reset_stat_mode;

typedef void (*bdev_reset_device_stat_cb)(struct spdk_bdev *bdev, void *cb_arg, int rc);

void bdev_reset_device_stat(struct spdk_bdev *bdev, enum spdk_bdev_reset_stat_mode mode,
			    bdev_reset_device_stat_cb cb, void *cb_arg);

static inline void
bdev_set_group(struct spdk_bdev *bdev, struct spdk_bdev_group *group)
{
	bdev->internal.group = group;
}

static inline struct spdk_bdev_group *
bdev_get_group(struct spdk_bdev *bdev)
{
	return bdev->internal.group;
}

void bdev_trigger_qos_queued_io_resend(struct spdk_bdev *bdev);

bool bdev_group_qos_bdev_poll(struct spdk_bdev_group *group, struct spdk_bdev *bdev,
				uint64_t now);

void bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *new_bdev_limits,
			      bool group_disable_rate_limit,
			      void (*cb_fn)(void *cb_arg, int status), void *cb_arg);

struct bdev_qos_limits *
bdev_group_get_qos_limits(struct spdk_bdev_group *group);

void bdev_qos_limits_rewind(struct bdev_qos_limits *limits, struct spdk_bdev_io *bdev_io);

#endif /* SPDK_BDEV_INTERNAL_H */
