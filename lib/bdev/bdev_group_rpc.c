/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/bdev_group.h"

#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/base64.h"
#include "spdk/bdev_module.h"

#include "spdk/log.h"

struct group_bdev_opts {
	char *name;
	char *bdev;
};

static const struct spdk_json_object_decoder rpc_construct_group_decoders[] = {
	{"name", offsetof(struct group_bdev_opts, name), spdk_json_decode_string, false},
};

static void
free_rpc_construct_group(struct group_bdev_opts *r)
{
	free(r->name);
	free(r->bdev);
}

static void
rpc_bdev_group_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct group_bdev_opts req = {0};
	struct spdk_bdev_group *group;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_construct_group_decoders,
				    SPDK_COUNTOF(rpc_construct_group_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_group, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	group = spdk_bdev_group_get_by_name(req.name);
	if (group) {
		SPDK_DEBUGLOG(bdev_group, "group %s already exists\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "group already exists");
		goto cleanup;
	}

	group = spdk_bdev_group_create(req.name);
	if (!group) {
		SPDK_DEBUGLOG(bdev_group, "cannot create group %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "cannot create group");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_construct_group(&req);
}
SPDK_RPC_REGISTER("bdev_group_create", rpc_bdev_group_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_group_bdev_decoders[] = {
	{"name", offsetof(struct group_bdev_opts, name), spdk_json_decode_string, false},
	{"bdev", offsetof(struct group_bdev_opts, bdev), spdk_json_decode_string, false},
};

static void
rpc_bdev_group_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_group_add_bdev(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct group_bdev_opts req = {0};
	struct spdk_bdev_group *group = NULL;

	if (spdk_json_decode_object(params, rpc_bdev_group_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_group_bdev_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_group, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	group = spdk_bdev_group_get_by_name(req.name);
	if (!group) {
		SPDK_DEBUGLOG(bdev_group, "cannot find group %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "group doesn't exist");
		goto cleanup;
	}

	spdk_bdev_group_add_bdev(group, req.bdev, rpc_bdev_group_bdev_cb, request);

cleanup:
	free_rpc_construct_group(&req);
}
SPDK_RPC_REGISTER("bdev_group_add_bdev", rpc_bdev_group_add_bdev, SPDK_RPC_RUNTIME)

static void
rpc_bdev_group_remove_bdev(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct group_bdev_opts req = {0};
	struct spdk_bdev_group *group = NULL;

	if (spdk_json_decode_object(params, rpc_bdev_group_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_group_bdev_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_group, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	group = spdk_bdev_group_get_by_name(req.name);
	if (!group) {
		SPDK_DEBUGLOG(bdev_group, "cannot find group %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "group doesn't exist");
		goto cleanup;
	}

	spdk_bdev_group_remove_bdev(group, req.bdev, rpc_bdev_group_bdev_cb, request);

cleanup:
	free_rpc_construct_group(&req);
}
SPDK_RPC_REGISTER("bdev_group_remove_bdev", rpc_bdev_group_remove_bdev, SPDK_RPC_RUNTIME)

static void
rpc_bdev_group_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_group_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct group_bdev_opts req = {0};
	struct spdk_bdev_group *group;

	if (spdk_json_decode_object(params, rpc_construct_group_decoders,
				    SPDK_COUNTOF(rpc_construct_group_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_group, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	group = spdk_bdev_group_get_by_name(req.name);
	if (!group) {
		SPDK_DEBUGLOG(bdev_group, "cannot find group %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "group doesn't exist");
		goto cleanup;
	}

	spdk_bdev_group_destroy(group, rpc_bdev_group_delete_cb, request);

cleanup:
	free_rpc_construct_group(&req);
}
SPDK_RPC_REGISTER("bdev_group_delete", rpc_bdev_group_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_group_set_qos_limit {
	char		*name;
	uint64_t	limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
};

static void
free_rpc_bdev_group_set_qos_limit(struct rpc_bdev_group_set_qos_limit *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_set_qos_limit_decoders[] = {
	{"name", offsetof(struct rpc_bdev_group_set_qos_limit, name), spdk_json_decode_string},
	{
		"rw_ios_per_sec", offsetof(struct rpc_bdev_group_set_qos_limit,
					   limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		"rw_mbytes_per_sec", offsetof(struct rpc_bdev_group_set_qos_limit,
					      limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		"r_mbytes_per_sec", offsetof(struct rpc_bdev_group_set_qos_limit,
					     limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		"w_mbytes_per_sec", offsetof(struct rpc_bdev_group_set_qos_limit,
					     limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
};

static void
rpc_bdev_group_set_qos_limit_complete(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to configure rate limit: %s",
						     spdk_strerror(-status));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_group_set_qos_limit(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_group_set_qos_limit req = {NULL, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
	struct spdk_bdev_group *group;
	int i;

	if (spdk_json_decode_object(params, rpc_bdev_set_qos_limit_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qos_limit_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	group = spdk_bdev_group_get_by_name(req.name);
	if (!group) {
		SPDK_DEBUGLOG(bdev_group, "cannot find group %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "group doesn't exist");
		goto cleanup;
	}

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (req.limits[i] != UINT64_MAX) {
			break;
		}
	}
	if (i == SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES) {
		SPDK_ERRLOG("no rate limits specified\n");
		spdk_jsonrpc_send_error_response(request, -EINVAL, "No rate limits specified");
		goto cleanup;
	}

	spdk_bdev_group_set_qos_rate_limits(group, req.limits,
					    rpc_bdev_group_set_qos_limit_complete, request);

cleanup:
	free_rpc_bdev_group_set_qos_limit(&req);
}
SPDK_RPC_REGISTER("bdev_group_set_qos_limit", rpc_bdev_group_set_qos_limit, SPDK_RPC_RUNTIME)

static int
rpc_spdk_bdev_group_info_cb(void *cb_arg, struct spdk_bdev_group *group, struct spdk_bdev *bdev)
{
	struct spdk_json_write_ctx *w = cb_arg;

	spdk_json_write_string(w, spdk_bdev_get_name(bdev));

	return 0;
}

struct groups_get_ctx {
	struct spdk_json_write_ctx *w;
	char *name;
};

static int
rpc_spdk_get_bdev_groups_cb(void *cb_arg, struct spdk_bdev_group *group)
{
	struct groups_get_ctx *ctx = cb_arg;

	if (ctx->name && strcmp(ctx->name, spdk_bdev_group_get_name(group))) {
		return 0; /* we're seeking a specific group and this is not it, so just continue */
	}

	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_string(ctx->w, "name", spdk_bdev_group_get_name(group));
	spdk_json_write_named_array_begin(ctx->w, "bdevs");
	spdk_bdev_group_for_each_bdev(group, ctx->w, rpc_spdk_bdev_group_info_cb);
	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);

	/* if this is the specific group we're required to report, return non-0 as there's no need to iterate further */
	/* otherwise, return 0 to continue to the next group */
	return ctx->name ? 1 : 0;
}

static const struct spdk_json_object_decoder rpc_groups_get_decoders[] = {
	{"name", offsetof(struct groups_get_ctx, name), spdk_json_decode_string, false},
};

static void
rpc_spdk_bdev_groups_get(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct groups_get_ctx ctx = {0};

	if (params && spdk_json_decode_object(params, rpc_groups_get_decoders,
					      SPDK_COUNTOF(rpc_groups_get_decoders),
					      &ctx)) {
		SPDK_DEBUGLOG(bdev_group, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	ctx.w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(ctx.w);
	spdk_for_each_bdev_group(&ctx, rpc_spdk_get_bdev_groups_cb);
	spdk_json_write_array_end(ctx.w);
	spdk_jsonrpc_end_result(request, ctx.w);

	free(ctx.name);
}
SPDK_RPC_REGISTER("bdev_groups_get", rpc_spdk_bdev_groups_get, SPDK_RPC_RUNTIME)
