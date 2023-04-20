/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev_group.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/bdev_module.h"
#include "bdev_qos_limits.h"
#include "bdev_internal.h"

struct spdk_bdev_node {
	struct spdk_bdev_desc *desc;
	TAILQ_ENTRY(spdk_bdev_node) link;
};

struct spdk_bdev_group {
	struct bdev_qos_limits *qos_limits;
	uint64_t qos_limits_usr_cfg[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
	bool qos_mod_in_progress;
	uint64_t qos_last_timeslice;
	uint64_t qos_timeslice_size;
	bool qos_reset_limits_in_progress;
	char *name;
	TAILQ_HEAD(, spdk_bdev_node) bdevs;
	struct spdk_spinlock spinlock;
	TAILQ_ENTRY(spdk_bdev_group) link;
};

TAILQ_HEAD(spdk_bdev_group_list, spdk_bdev_group);

struct spdk_bdev_group_mgr {
	struct spdk_bdev_group_list groups;

	struct spdk_spinlock spinlock;
};

static struct spdk_bdev_group_mgr g_bdev_group_mgr = {
	.groups = TAILQ_HEAD_INITIALIZER(g_bdev_group_mgr.groups),
};

static void
__attribute__((constructor))
_bdev_init(void)
{
	spdk_spin_init(&g_bdev_group_mgr.spinlock);
}

struct bdev_qos_limits *
bdev_group_get_qos_limits(struct spdk_bdev_group *group)
{
	return group->qos_limits;
}

bool
bdev_group_qos_bdev_poll(struct spdk_bdev_group *group, struct spdk_bdev *bdev, uint64_t now)
{
	struct spdk_bdev *next_bdev;
	struct spdk_bdev_node *node;
	bool qos_reset_limits_in_progress;
	bool res = false;

	assert(!TAILQ_EMPTY(&group->bdevs));

	if (group->qos_limits) {
		qos_reset_limits_in_progress = __atomic_test_and_set(&group->qos_reset_limits_in_progress,
					       __ATOMIC_RELAXED);
		if (!qos_reset_limits_in_progress) {
			if (now >= (group->qos_last_timeslice + group->qos_timeslice_size)) {
				bdev_qos_limits_reset_quota(
					group->qos_limits, now, group->qos_timeslice_size, &group->qos_last_timeslice);
				res = true;

				spdk_spin_lock(&group->spinlock);
				TAILQ_FOREACH(node, &group->bdevs, link) {
					next_bdev = spdk_bdev_desc_get_bdev(node->desc);
					if (next_bdev == bdev) {
						continue;
					}
					bdev_trigger_qos_queued_io_resend(next_bdev);
				}
				spdk_spin_unlock(&group->spinlock);
			}

			__atomic_clear(&group->qos_reset_limits_in_progress, __ATOMIC_RELAXED);
		}
	}

	return res;
}

struct spdk_bdev_group *
spdk_bdev_group_create(const char *group_name)
{
	struct spdk_bdev_group *group;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		SPDK_ERRLOG("failed to allocate memory\n");
		return NULL;
	}

	group->name = strdup(group_name);
	if (group->name == NULL) {
		SPDK_ERRLOG("Unable to allocate group name\n");
		free(group);
		return NULL;
	}

	TAILQ_INIT(&group->bdevs);
	spdk_spin_init(&group->spinlock);

	spdk_spin_lock(&g_bdev_group_mgr.spinlock);
	TAILQ_INSERT_TAIL(&g_bdev_group_mgr.groups, group, link);
	spdk_spin_unlock(&g_bdev_group_mgr.spinlock);

	return group;
}

struct spdk_bdev_group *
spdk_bdev_group_get_by_name(const char *group_name)
{
	struct spdk_bdev_group *group;

	spdk_spin_lock(&g_bdev_group_mgr.spinlock);
	TAILQ_FOREACH(group, &g_bdev_group_mgr.groups, link) {
		if (!strcmp(group->name, group_name)) {
			break;
		}
	}
	spdk_spin_unlock(&g_bdev_group_mgr.spinlock);

	return group;
}

int
spdk_for_each_bdev_group(void *cb_arg, int (*cb_fn)(void *cb_arg, struct spdk_bdev_group *group))
{
	struct spdk_bdev_group *group = NULL;
	int rc = 0;

	spdk_spin_lock(&g_bdev_group_mgr.spinlock);
	TAILQ_FOREACH(group, &g_bdev_group_mgr.groups, link) {
		rc = cb_fn(cb_arg, group);
		if (rc) {
			break;
		}
	}
	spdk_spin_unlock(&g_bdev_group_mgr.spinlock);

	return rc;
}

struct bdev_group_add_set_qos_rate_limits_ctx {
	struct spdk_bdev_group *group;
	struct spdk_bdev_node *node;
	void (*cb_fn)(void *cb_arg, int status);
	void *cb_arg;
};

static void
bdev_group_add_set_qos_rate_limits_cb(void *cb_arg, int status)
{
	struct bdev_group_add_set_qos_rate_limits_ctx *ctx = cb_arg;
	struct spdk_bdev_group *group = ctx->group;
	struct spdk_bdev_node *node = ctx->node;

	/* if QoS is enabled for the bdev, now add it to the list */

	/* Add the bdev to the list */
	spdk_spin_lock(&group->spinlock);
	TAILQ_INSERT_TAIL(&group->bdevs, node, link);
	spdk_spin_unlock(&group->spinlock);

	/* Clear the in-progress flag, so QoS changes are allowed again */
	__atomic_clear(&ctx->group->qos_mod_in_progress, __ATOMIC_RELAXED);
	/* Set the new group pointer */
	bdev_set_group(spdk_bdev_desc_get_bdev(node->desc), group);

	ctx->cb_fn(ctx->cb_arg, status);
	/* we do not free node here as it's still in the device list */
	free(ctx);
}

static void
bdev_group_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	struct spdk_bdev_group *group = ctx;
	struct spdk_bdev_node *node;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		spdk_spin_lock(&group->spinlock);
		TAILQ_FOREACH(node, &group->bdevs, link) {
			if (spdk_bdev_desc_get_bdev(node->desc) == bdev) {
				TAILQ_REMOVE(&group->bdevs, node, link);
				spdk_bdev_close(node->desc);
				free(node);
				break;
			}
		}
		spdk_spin_unlock(&group->spinlock);
	} else {
		SPDK_NOTICELOG("Unexpected event type: %d\n", type);
	}
}

void
spdk_bdev_group_add_bdev(struct spdk_bdev_group *group, const char *bdev_name,
			 void (*cb_fn)(void *cb_arg, int status),
			 void *cb_arg)
{
	int rc;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct bdev_group_add_set_qos_rate_limits_ctx *ctx;
	bool qos_mod_in_progress;

	rc = spdk_bdev_open_ext(bdev_name, false, bdev_group_bdev_event_cb, group, &desc);
	if (rc < 0) {
		SPDK_ERRLOG("Could not open bdev %s: %s\n", bdev_name, strerror(-rc));
		cb_fn(cb_arg, rc);
		return;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	if (bdev_get_group(bdev)) {
		SPDK_ERRLOG("bdev %s is already a part of a group\n", bdev_name);
		spdk_bdev_close(desc);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	ctx = (struct bdev_group_add_set_qos_rate_limits_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_bdev_close(desc);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->node = (struct spdk_bdev_node *)calloc(1, sizeof(*ctx->node));
	if (!ctx->node) {
		spdk_bdev_close(desc);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Make sure nobody is changing the group QoS settings while we're adding the bdev */
	qos_mod_in_progress = __atomic_test_and_set(&group->qos_mod_in_progress, __ATOMIC_RELAXED);
	if (qos_mod_in_progress) {
		spdk_bdev_close(desc);
		free(ctx->node);
		free(ctx);
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	ctx->group = group;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->node->desc = desc;

	/* NOTE: it's safe to check group->qos_limits here, as it's protected by the in-progress atomic */
	if (!group->qos_limits) {
		/* if QoS is not enabled for the group, proceed with adding the bdev */
		bdev_group_add_set_qos_rate_limits_cb(ctx, 0);
	} else {
		/* if QoS is enabled for the group, enable it for the bdev first */
		bdev_set_qos_rate_limits(bdev, NULL, false,
					 bdev_group_add_set_qos_rate_limits_cb, ctx);
	}
}

struct bdev_group_remove_cb_ctx {
	void (*cb_fn)(void *cb_arg, int status);
	void *cb_arg;
	struct spdk_bdev_group *group;
	struct spdk_bdev_node *node;
};

static void
bdev_group_remove_msg(struct spdk_bdev_channel_iter *i,
		      struct spdk_bdev *bdev,
		      struct spdk_io_channel *ch,
		      void *_ctx)
{
	/* We have nothing to do here, we just make sure that this device doesn't
	 * refer to the group anymore */
	spdk_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_group_remove_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct bdev_group_remove_cb_ctx *ctx = _ctx;
	struct spdk_bdev_node *node = ctx->node;

	spdk_bdev_close(node->desc);
	ctx->cb_fn(ctx->cb_arg, 0);
	free(node);
	free(ctx);
}

static void
bdev_group_remove_set_qos_rate_limits_cb(void *cb_arg, int status)
{
	struct bdev_group_remove_cb_ctx *ctx = cb_arg;
	struct spdk_bdev_group *group = ctx->group;
	struct spdk_bdev_node *node = ctx->node;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(node->desc);

	/* Now when QoS is not enabled for the group, proceed with the detach */

	/* Set bdev's group pointer to NULL */
	bdev_set_group(bdev, NULL);
	/* Clear the in-progress flag, so QoS changes are allowed again */
	__atomic_clear(&group->qos_mod_in_progress, __ATOMIC_RELAXED);
	/* Make sure that the previous group pointer is not referenced anymore */
	spdk_bdev_for_each_channel(
		bdev, bdev_group_remove_msg, ctx, bdev_group_remove_done);
}

void
spdk_bdev_group_remove_bdev(struct spdk_bdev_group *group,
			    const char *bdev_name,
			    void (*cb_fn)(void *cb_arg, int status),
			    void *cb_arg)
{
	struct bdev_group_remove_cb_ctx *ctx;
	struct spdk_bdev_node *node;
	struct spdk_bdev *bdev = NULL;
	bool qos_mod_in_progress;

	/* Make sure nobody is changing the group QoS settings while we're detaching the bdev */
	qos_mod_in_progress = __atomic_test_and_set(&group->qos_mod_in_progress, __ATOMIC_RELAXED);
	if (qos_mod_in_progress) {
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	ctx = (struct bdev_group_remove_cb_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx) {
		__atomic_clear(&group->qos_mod_in_progress, __ATOMIC_RELAXED);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	spdk_spin_lock(&group->spinlock);
	TAILQ_FOREACH(node, &group->bdevs, link) {
		bdev = spdk_bdev_desc_get_bdev(node->desc);
		if (!strcmp(spdk_bdev_get_name(bdev), bdev_name)) {
			TAILQ_REMOVE(&group->bdevs, node, link);
			break;
		}
		bdev = NULL;
	}
	spdk_spin_unlock(&group->spinlock);

	if (!bdev) {
		SPDK_ERRLOG("bdev %s is not a part of the group %s\n", bdev_name, group->name);
		__atomic_clear(&group->qos_mod_in_progress, __ATOMIC_RELAXED);
		cb_fn(cb_arg, -ENOENT);
		free(ctx);
		return;
	}

	ctx->group = group;
	ctx->node = node;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* NOTE: it's safe to check group->qos_limits here, as it's protected by the in-progress atomic */
	if (!group->qos_limits) {
		/* if QoS is not enabled for the group, proceed with the detach */
		bdev_group_remove_set_qos_rate_limits_cb(ctx, 0);
	} else {
		/* if QoS is enabled for the group, firts we have to disable it for the bdev */
		bdev_set_qos_rate_limits(bdev, NULL, false,
					 bdev_group_remove_set_qos_rate_limits_cb, ctx);
	}
}

int
spdk_bdev_group_for_each_bdev(struct spdk_bdev_group *group, void *cb_arg,
			      int (*cb_fn)(void *cb_arg, struct spdk_bdev_group *group, struct spdk_bdev *bdev))
{
	struct spdk_bdev_node *node;
	int rc = 0;

	spdk_spin_lock(&group->spinlock);
	TAILQ_FOREACH(node, &group->bdevs, link) {
		rc = cb_fn(cb_arg, group, spdk_bdev_desc_get_bdev(node->desc));
		if (rc) {
			break;
		}
	}
	spdk_spin_unlock(&group->spinlock);

	return rc;
}

const char *
spdk_bdev_group_get_name(struct spdk_bdev_group *group)
{
	return group->name;
}

void
spdk_bdev_group_get_qos_rate_limits(struct spdk_bdev_group *group, uint64_t *limits)
{
	spdk_spin_lock(&group->spinlock);
	memcpy(limits, group->qos_limits_usr_cfg, sizeof(group->qos_limits_usr_cfg));
	spdk_spin_unlock(&group->spinlock);
}

struct bdev_group_set_qos_rate_limits_ctx {
	struct spdk_bdev_group *group;
	uint64_t ref_cnt;
	struct bdev_qos_limits *old_qos_limits;
	void (*cb_fn)(void *cb_arg, int status);
	void *cb_arg;
	int status;
};

static void
bdev_group_set_qos_rate_limits_cb(void *cb_arg, int status)
{
	struct bdev_group_set_qos_rate_limits_ctx *ctx = cb_arg;
	uint64_t ref_cnt;

	if (status != 0) {
		ctx->status = status;
	}

	/* de-reference */
	ref_cnt = __atomic_sub_fetch(&ctx->ref_cnt, 1, __ATOMIC_RELAXED);
	if (!ref_cnt) { /* if there'are no mo references, we're done */
		ctx->cb_fn(ctx->cb_arg, ctx->status);
		__atomic_clear(&ctx->group->qos_mod_in_progress, __ATOMIC_RELAXED);
		free(ctx->old_qos_limits); /* old group QoS limits can now be freed */
		free(ctx);
	}
}

void
spdk_bdev_group_set_qos_rate_limits(struct spdk_bdev_group *group, const uint64_t *limits,
				    void (*cb_fn)(void *cb_arg, int status),
				    void *cb_arg)
{
	struct bdev_group_set_qos_rate_limits_ctx *ctx;
	struct spdk_bdev_node *node;
	struct bdev_qos_limits *new_qos_limits = NULL;
	bool qos_mod_in_progress;
	bool disable_rate_limit;

	ctx = (struct bdev_group_set_qos_rate_limits_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	qos_mod_in_progress = __atomic_test_and_set(&group->qos_mod_in_progress, __ATOMIC_RELAXED);
	if (qos_mod_in_progress) {
		cb_fn(cb_arg, -EAGAIN);
		free(ctx);
		return;
	}

	spdk_spin_lock(&group->spinlock);

	disable_rate_limit = bdev_qos_limits_check_disabled(
				     group->qos_limits_usr_cfg, limits);

	if (!disable_rate_limit) {
		/* Allocate and init new group QoS limits */
		new_qos_limits = (struct bdev_qos_limits *)calloc(1, sizeof(*new_qos_limits));
		if (new_qos_limits == NULL) {
			SPDK_ERRLOG("Unable to allocate QoS Limits\n");
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		bdev_qos_limits_init(new_qos_limits);
		bdev_qos_limits_set(new_qos_limits, limits);
		bdev_qos_limits_update_max_quota_per_timeslice(new_qos_limits);

		group->qos_last_timeslice = spdk_get_ticks();
		group->qos_timeslice_size =
			SPDK_BDEV_QOS_TIMESLICE_IN_USEC * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;

	}

	/* Store new group QoS limits config */
	memcpy(group->qos_limits_usr_cfg, limits, sizeof(group->qos_limits_usr_cfg));

	/* Store the old QoS group limits */
	ctx->old_qos_limits = group->qos_limits;

	/* Store the new QoS group limits */
	group->qos_limits = new_qos_limits;

	/* Replace the group QoS limits */
	ctx->ref_cnt = 1; /* reference by the caller */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->group = group;

	TAILQ_FOREACH(node, &group->bdevs, link) {
		__atomic_add_fetch(&ctx->ref_cnt, 1, __ATOMIC_RELAXED); /* add reference by the device */
		bdev_set_qos_rate_limits(spdk_bdev_desc_get_bdev(node->desc), NULL, disable_rate_limit,
						bdev_group_set_qos_rate_limits_cb, ctx);
	}

	bdev_group_set_qos_rate_limits_cb(ctx, 0); /* release the reference by the caller */

	spdk_spin_unlock(&group->spinlock);
}

struct bdev_group_destroy_ctx {
	void (*cb_fn)(void *cb_arg, int status);
	void *cb_arg;
	struct spdk_bdev_group *group;
};

static void
bdev_group_destroy_cb(void *_ctx, int status)
{
	struct bdev_group_destroy_ctx *ctx = _ctx;
	struct spdk_bdev_group *group = ctx->group;
	struct spdk_bdev_node *node;
	struct spdk_bdev *bdev;

	if (!TAILQ_EMPTY(&group->bdevs)) {
		node = TAILQ_FIRST(&group->bdevs);
		bdev = spdk_bdev_desc_get_bdev(node->desc);
		spdk_bdev_group_remove_bdev(group, spdk_bdev_get_name(bdev), bdev_group_destroy_cb, group);
	} else {
		ctx->cb_fn(ctx->cb_arg, 0);
		spdk_spin_destroy(&group->spinlock);
		free(group->name);
		free(group);
		free(ctx);
	}
}

void
spdk_bdev_group_destroy(struct spdk_bdev_group *group,
			void (*cb_fn)(void *cb_arg, int status),
			void *cb_arg)
{
	struct bdev_group_destroy_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->group = group;

	spdk_spin_lock(&g_bdev_group_mgr.spinlock);
	TAILQ_REMOVE(&g_bdev_group_mgr.groups, group, link);
	spdk_spin_unlock(&g_bdev_group_mgr.spinlock);

	bdev_group_destroy_cb(ctx, 0);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_group)
