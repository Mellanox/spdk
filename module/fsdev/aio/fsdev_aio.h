/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Operations on aio filesystem device
 */

#ifndef SPDK_FSDEV_AIO_H
#define SPDK_FSDEV_AIO_H

#include "spdk/stdinc.h"
#include "spdk/fsdev_module.h"

enum spdk_aio_bool_param {
	SPDK_AIO_UNDEFINED = -1,
	SPDK_AIO_TRUE,
	SPDK_AIO_FALSE,
};

#define SPDK_AIO_MAX_WRITE_UNDEFINED 0

typedef void (*spdk_delete_aio_fsdev_complete)(void *cb_arg, int fsdeverrno);

int spdk_fsdev_aio_create(struct spdk_fsdev **fsdev, const char *name, const char *root_path,
			  enum spdk_aio_bool_param xattr_enabled, enum spdk_aio_bool_param writeback_cache_enabled,
			  uint32_t max_write);
void spdk_fsdev_aio_delete(const char *name, spdk_delete_aio_fsdev_complete cb_fn, void *cb_arg);

#endif /* SPDK_FSDEV_AIO_H */
