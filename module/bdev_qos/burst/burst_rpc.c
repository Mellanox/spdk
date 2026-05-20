/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/bdev.h"

#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"

#include "spdk/log.h"

#include "spdk_internal/bdev_qos_module.h"

#include "burst.h"

static const struct spdk_json_object_decoder rpc_burst_qos_set_opts_decoders[] = {
	{"tick_period_us", offsetof(struct bdev_burst_qos_opts, tick_period_us), spdk_json_decode_uint64, true},
	{"max_io_withdraw_batch_size", offsetof(struct bdev_burst_qos_opts, max_io_withdraw_batch_size), spdk_json_decode_uint64, true},
	{"io_additive_increase_step", offsetof(struct bdev_burst_qos_opts, io_additive_increase_step), spdk_json_decode_uint64, true},
	{"max_byte_withdraw_batch_size", offsetof(struct bdev_burst_qos_opts, max_byte_withdraw_batch_size), spdk_json_decode_uint64, true},
	{"byte_additive_increase_step", offsetof(struct bdev_burst_qos_opts, byte_additive_increase_step), spdk_json_decode_uint64, true},
	{"retry_budget", offsetof(struct bdev_burst_qos_opts, retry_budget), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_burst_qos_set_options(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct bdev_burst_qos_opts opts;
	int rc;

	bdev_burst_qos_get_opts(&opts, sizeof(opts));

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_burst_qos_set_opts_decoders,
					    SPDK_COUNTOF(rpc_burst_qos_set_opts_decoders), &opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	rc = bdev_burst_qos_set_opts(&opts);
	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("bdev_burst_qos_set_options", rpc_bdev_burst_qos_set_options,
		  SPDK_RPC_STARTUP)

struct rpc_burst_qos {
	char *name;
};

static void
free_rpc_burst_qos(struct rpc_burst_qos *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_burst_qos_decoders[] = {
	{"name", offsetof(struct rpc_burst_qos, name), spdk_json_decode_string},
};

struct rpc_burst_qos_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_bdev_qos_desc *desc;
};

static void
rpc_bdev_burst_qos_set_limit_done(void *cb_arg, int status)
{
	struct rpc_burst_qos_ctx *ctx = cb_arg;

	if (status == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response_fmt(ctx->request,
						     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "failed to configure token bucket: %s",
						     spdk_strerror(-status));
	}
	spdk_bdev_qos_close(ctx->desc);
	free(ctx);
}

static void
rpc_bdev_burst_qos_set_limit(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_burst_qos req = {NULL};
	struct spdk_bdev_qos_desc *desc;
	struct spdk_bdev_qos *qos;
	struct rpc_burst_qos_ctx *ctx;
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_burst_qos_decoders,
					    SPDK_COUNTOF(rpc_burst_qos_decoders),
					    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_qos_open(req.name, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open QoS dev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	qos = spdk_bdev_qos_desc_get_qos(desc);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_bdev_qos_close(desc);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	ctx->request = request;
	ctx->desc = desc;

	bdev_burst_qos_set_limit_json(qos, params,
				      rpc_bdev_burst_qos_set_limit_done, ctx);

cleanup:
	free_rpc_burst_qos(&req);
}

SPDK_RPC_REGISTER("bdev_burst_qos_set_limit", rpc_bdev_burst_qos_set_limit,
		  SPDK_RPC_RUNTIME)
