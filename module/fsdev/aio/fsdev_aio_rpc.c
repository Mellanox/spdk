/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "fsdev_aio.h"

struct rpc_aio_create {
	char *name;
	char *root_path;
	uint8_t xattr_enabled;
	uint8_t writeback_cache;
	uint32_t max_write;
};

static void
free_rpc_aio_create(struct rpc_aio_create *req)
{
	free(req->name);
	free(req->root_path);
}

static const struct spdk_json_object_decoder rpc_aio_create_decoders[] = {
	{"name", offsetof(struct rpc_aio_create, name), spdk_json_decode_string},
	{"root_path", offsetof(struct rpc_aio_create, root_path), spdk_json_decode_string},
	{"xattr_enabled", offsetof(struct rpc_aio_create, xattr_enabled), spdk_json_decode_uint8, true},
	{"writeback_cache", offsetof(struct rpc_aio_create, writeback_cache), spdk_json_decode_uint8, true},
	{"max_write", offsetof(struct rpc_aio_create, max_write), spdk_json_decode_uint32, true},
};

static void
rpc_aio_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_aio_create req = {
		.xattr_enabled = UINT8_MAX,
		.writeback_cache = UINT8_MAX,
		.max_write = 0,
	};
	struct spdk_json_write_ctx *w;
	struct spdk_fsdev *fsdev;
	uint32_t max_write = SPDK_AIO_MAX_WRITE_UNDEFINED;
	enum spdk_aio_bool_param xattr_enabled = SPDK_AIO_UNDEFINED;
	enum spdk_aio_bool_param writeback_cache = SPDK_AIO_UNDEFINED;
	int rc;

	if (spdk_json_decode_object(params, rpc_aio_create_decoders,
				    SPDK_COUNTOF(rpc_aio_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free_rpc_aio_create(&req);
		return;
	}

	if (req.xattr_enabled != UINT8_MAX) {
		xattr_enabled = !!req.xattr_enabled;
	}

	if (req.writeback_cache != UINT8_MAX) {
		writeback_cache = !!req.writeback_cache;
	}

	if (req.max_write) {
		max_write = req.max_write;
	}

	rc = spdk_fsdev_aio_create(&fsdev, req.name, req.root_path, xattr_enabled, writeback_cache,
				   max_write);
	if (rc) {
		SPDK_ERRLOG("Failed to create aio %s: rc %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		free_rpc_aio_create(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, fsdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_aio_create(&req);
}
SPDK_RPC_REGISTER("fsdev_aio_create", rpc_aio_create, SPDK_RPC_RUNTIME)

struct rpc_aio_delete {
	char *name;
};

static const struct spdk_json_object_decoder rpc_aio_delete_decoders[] = {
	{"name", offsetof(struct rpc_aio_delete, name), spdk_json_decode_string},
};

static void
rpc_aio_delete_cb(void *cb_arg, int fsdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (fsdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, fsdeverrno, spdk_strerror(-fsdeverrno));
	}
}

static void
rpc_aio_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_aio_delete req = {};

	if (spdk_json_decode_object(params, rpc_aio_delete_decoders,
				    SPDK_COUNTOF(rpc_aio_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free(req.name);
		return;
	}

	spdk_fsdev_aio_delete(req.name, rpc_aio_delete_cb, request);
	free(req.name);
}
SPDK_RPC_REGISTER("fsdev_aio_delete", rpc_aio_delete, SPDK_RPC_RUNTIME)
