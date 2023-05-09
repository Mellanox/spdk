/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev_reservations.h"

#include "spdk/log.h"
#include "spdk/likely.h"

#include "bdev_internal.h"

int
spdk_bdev_reservation_register(struct spdk_bdev_desc *desc,
			       struct spdk_io_channel *ch,
			       uint64_t crkey, uint64_t nrkey,
			       bool ignore_key,
			       enum spdk_bdev_reservation_register_action action,
			       enum spdk_bdev_reservation_register_cptpl cptpl,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (spdk_unlikely(!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESERVATION_REGISTER))) {
		SPDK_DEBUGLOG(bdev, "Reservation Register IO type is not supported\n");
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESERVATION_REGISTER;

	bdev_io->u.reservation_register.crkey = crkey;
	bdev_io->u.reservation_register.nrkey = nrkey;
	bdev_io->u.reservation_register.cptpl = cptpl;
	bdev_io->u.reservation_register.action = action;
	bdev_io->u.reservation_register.ignore_key = ignore_key;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_reservation_acquire(struct spdk_bdev_desc *desc,
			      struct spdk_io_channel *ch,
			      uint64_t crkey, uint64_t prkey, bool ignore_key,
			      enum spdk_bdev_reservation_acquire_action action,
			      enum spdk_bdev_reservation_type type,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (spdk_unlikely(!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESERVATION_ACQUIRE))) {
		SPDK_DEBUGLOG(bdev, "Reservation Acquire IO type is not supported\n");
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESERVATION_ACQUIRE;

	bdev_io->u.reservation_acquire.crkey = crkey;
	bdev_io->u.reservation_acquire.prkey = prkey;
	bdev_io->u.reservation_acquire.type = type;
	bdev_io->u.reservation_acquire.action = action;
	bdev_io->u.reservation_acquire.ignore_key = ignore_key;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_reservation_release(struct spdk_bdev_desc *desc,
			      struct spdk_io_channel *ch,
			      uint64_t crkey, bool ignore_key,
			      enum spdk_bdev_reservation_release_action action,
			      enum spdk_bdev_reservation_type type,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (spdk_unlikely(!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESERVATION_RELEASE))) {
		SPDK_DEBUGLOG(bdev, "Reservation Release IO type is not supported\n");
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESERVATION_RELEASE;

	bdev_io->u.reservation_release.type = type;
	bdev_io->u.reservation_release.action = action;
	bdev_io->u.reservation_release.crkey = crkey;
	bdev_io->u.reservation_release.ignore_key = ignore_key;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_reservation_report(struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch,
			     struct spdk_bdev_reservation_status_data *status_data,
			     uint32_t len,
			     spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (spdk_unlikely(!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESERVATION_REPORT))) {
		SPDK_DEBUGLOG(bdev, "Reservation Report IO type is not supported\n");
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	if (len < (sizeof(struct spdk_bdev_reservation_status_data) + sizeof(struct spdk_bdev_registered_ctrlr_data))) {
		/* Buffer length should be enough to 
		 * accommodate atleast 1 entry */
		return -EINVAL;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESERVATION_REPORT;

	bdev_io->u.reservation_report.len = len;
	bdev_io->u.reservation_report.status_data = status_data;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

const struct spdk_bdev_reservation_caps *
spdk_bdev_get_reservation_caps(struct spdk_bdev *bdev)
{
	return &bdev->reservation_caps;
}
