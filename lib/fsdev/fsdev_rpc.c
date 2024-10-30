/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/fsdev.h"
#include "spdk/thread.h"

static void
rpc_fsdev_get_opts(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_fsdev_opts opts = {};
	int rc;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'fsdev_get_opts' requires no arguments");
		return;
	}

	rc = spdk_fsdev_get_opts(&opts, sizeof(opts));
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_opts failed with %d", rc);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "fsdev_io_pool_size", opts.fsdev_io_pool_size);
	spdk_json_write_named_uint32(w, "fsdev_io_cache_size", opts.fsdev_io_cache_size);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("fsdev_get_opts", rpc_fsdev_get_opts, SPDK_RPC_RUNTIME)

struct rpc_fsdev_set_opts {
	uint32_t fsdev_io_pool_size;
	uint32_t fsdev_io_cache_size;
};

static const struct spdk_json_object_decoder rpc_fsdev_set_opts_decoders[] = {
	{"fsdev_io_pool_size", offsetof(struct rpc_fsdev_set_opts, fsdev_io_pool_size), spdk_json_decode_uint32, false},
	{"fsdev_io_cache_size", offsetof(struct rpc_fsdev_set_opts, fsdev_io_cache_size), spdk_json_decode_uint32, false},
};

static void
rpc_fsdev_set_opts(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_set_opts ctx = {};
	int rc;
	struct spdk_fsdev_opts opts = {};

	if (spdk_json_decode_object(params, rpc_fsdev_set_opts_decoders,
				    SPDK_COUNTOF(rpc_fsdev_set_opts_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_fsdev_get_opts(&opts, sizeof(opts));
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_opts failed with %d", rc);
		return;
	}

	opts.fsdev_io_pool_size = ctx.fsdev_io_pool_size;
	opts.fsdev_io_cache_size = ctx.fsdev_io_cache_size;

	rc = spdk_fsdev_set_opts(&opts);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_set_opts failed with %d", rc);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("fsdev_set_opts", rpc_fsdev_set_opts, SPDK_RPC_RUNTIME)

struct rpc_fsdev_get_fsdevs {
	char *name;
};

static int
rpc_dump_fsdev_info(void *ctx, struct spdk_fsdev *fsdev)
{
	struct spdk_json_write_ctx *w = ctx;
	const char *fsdev_name = spdk_fsdev_get_name(fsdev);
	int i, rc;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", fsdev_name);

	spdk_json_write_named_string(w, "module_name", spdk_fsdev_get_module_name(fsdev));
	rc = spdk_fsdev_get_memory_domains(fsdev, NULL, 0);
	if (rc > 0) {
		struct spdk_memory_domain **domains = calloc(rc, sizeof(struct spdk_memory_domain *));
		if (domains) {
			i = spdk_fsdev_get_memory_domains(fsdev, domains, rc);
			if (i == rc) {
				spdk_json_write_named_array_begin(w, "memory_domains");
				for (i = 0; i < rc; i++) {
					const char *domain_id = spdk_memory_domain_get_dma_device_id(domains[i]);
					spdk_json_write_object_begin(w);
					if (domain_id) {
						spdk_json_write_named_string(w, "dma_device_id", domain_id);
					} else {
						spdk_json_write_named_null(w, "dma_device_id");
					}
					spdk_json_write_named_int32(w, "dma_device_type",
								    spdk_memory_domain_get_dma_device_type(domains[i]));
					spdk_json_write_object_end(w);
				}
				spdk_json_write_array_end(w);
			} else {
				SPDK_ERRLOG("Unexpected number of memory domains %d (should be %d)\n", i, rc);
			}

			free(domains);
		} else {
			SPDK_ERRLOG("Memory allocation failed\n");
		}
	}

	spdk_json_write_named_object_begin(w, "module_specific");
	spdk_fsdev_dump_info_json(fsdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_json_object_decoder rpc_fsdev_get_fsdevs_decoders[] = {
	{"name", offsetof(struct rpc_fsdev_get_fsdevs, name), spdk_json_decode_string, true},
};

static void
_rpc_fsdev_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev, void *ctx)
{
	SPDK_NOTICELOG("Unexpected fsdev event type: %d\n", type);
}

static void
rpc_fsdev_get_fsdevs(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_fsdev_get_fsdevs ctx = {};
	struct spdk_json_write_ctx *w;

	if (params && spdk_json_decode_object(params, rpc_fsdev_get_fsdevs_decoders,
					      SPDK_COUNTOF(rpc_fsdev_get_fsdevs_decoders),
					      &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto out;
	}

	if (ctx.name) {
		struct spdk_fsdev_desc *fsdev_desc;
		int rc;

		rc = spdk_fsdev_open(ctx.name, _rpc_fsdev_event_cb, NULL, &fsdev_desc);
		if (rc) {
			SPDK_ERRLOG("spdk_fsdev_open failed for '%s': rc=%d\n", ctx.name, rc);
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
			goto out;
		}

		w = spdk_jsonrpc_begin_result(request);
		rpc_dump_fsdev_info(w, spdk_fsdev_desc_get_fsdev(fsdev_desc));
		spdk_jsonrpc_end_result(request, w);
		spdk_fsdev_close(fsdev_desc);
	} else {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_array_begin(w);
		spdk_for_each_fsdev(w, rpc_dump_fsdev_info);
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);
	}

out:
	free(ctx.name);
}
SPDK_RPC_REGISTER("fsdev_get_fsdevs", rpc_fsdev_get_fsdevs, SPDK_RPC_RUNTIME)

struct rpc_fsdev_get_iostat_ctx;

struct rpc_fsdev_get_iostat_node {
	struct rpc_fsdev_get_iostat_ctx *ctx;
	struct spdk_fsdev_desc *fsdev_desc;
	struct spdk_fsdev_io_stat stat;
	TAILQ_ENTRY(rpc_fsdev_get_iostat_node) link;
	char fsdev_name[];
};

struct rpc_fsdev_get_iostat_ctx {
	char *name;
	bool per_channel;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	TAILQ_HEAD(, rpc_fsdev_get_iostat_node) nodes;
};

static const struct spdk_json_object_decoder rpc_fsdev_get_iostat_decoders[] = {
	{"name", offsetof(struct rpc_fsdev_get_iostat_ctx, name), spdk_json_decode_string, true},
	{"per_channel", offsetof(struct rpc_fsdev_get_iostat_ctx, per_channel), spdk_json_decode_bool, true},
};

static void
fsdev_stab_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev, void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported fsdev event: type %d\n", type);
}

static void
fsdev_free_get_iostat_ctx(struct rpc_fsdev_get_iostat_ctx *ctx)
{
	free(ctx->name);
	while (!TAILQ_EMPTY(&ctx->nodes)) {
		struct rpc_fsdev_get_iostat_node *node = TAILQ_FIRST(&ctx->nodes);
		TAILQ_REMOVE(&ctx->nodes, node, link);
		if (node->fsdev_desc) {
			spdk_fsdev_close(node->fsdev_desc);
		}
		free(node);
	}
	free(ctx);
}

static void rpc_fsdev_get_iostat_next(struct rpc_fsdev_get_iostat_ctx *ctx);

static void
rpc_fsdev_get_iostat_write(struct spdk_json_write_ctx *w, struct spdk_fsdev_io_stat *stat,
			   const char *name)
{
	size_t i;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", name);
	spdk_json_write_named_object_begin(w, "num_ios");
	for (i = 0; i < SPDK_COUNTOF(stat->num_ios); i++) {
		const char *name = spdk_fsdev_io_type_get_name(i);
		if (!name) {
			SPDK_ERRLOG("Cannot get name for IO %zu\n", i);
			continue;
		}
		spdk_json_write_named_uint64(w, name, stat->num_ios[i]);
	}
	spdk_json_write_object_end(w);
	spdk_json_write_named_uint64(w, "bytes_read", stat->bytes_read);
	spdk_json_write_named_uint64(w, "bytes_written", stat->bytes_written);
	spdk_json_write_named_uint64(w, "num_out_of_io", stat->num_out_of_io);
	spdk_json_write_named_uint64(w, "num_errors", stat->num_errors);
	spdk_json_write_object_end(w);
}

static void
rpc_fsdev_get_iostat_cpl(struct spdk_fsdev *fsdev, struct spdk_fsdev_io_stat *stat, void *cb_arg,
			 int rc)
{
	struct rpc_fsdev_get_iostat_node *node = cb_arg;
	struct rpc_fsdev_get_iostat_ctx *ctx = node->ctx;
	struct spdk_jsonrpc_request *request = ctx->request;

	spdk_fsdev_close(node->fsdev_desc);

	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_device_stat failed with %d", rc);
		free(node);
		fsdev_free_get_iostat_ctx(ctx);
		return;
	}

	rpc_fsdev_get_iostat_write(ctx->w, stat, node->fsdev_name);

	rpc_fsdev_get_iostat_next(ctx);
	free(node);
}

static void
rpc_fsdev_get_ch_iostat_msg(struct spdk_fsdev_channel_iter *i, struct spdk_fsdev *fsdev,
			    struct spdk_io_channel *ch, void *_ctx)
{
	struct rpc_fsdev_get_iostat_node *node = _ctx;
	struct rpc_fsdev_get_iostat_ctx *ctx = node->ctx;

	spdk_fsdev_get_io_stat(spdk_fsdev_desc_get_fsdev(node->fsdev_desc), ch, &node->stat);

	rpc_fsdev_get_iostat_write(ctx->w, &node->stat,
				   spdk_thread_get_name(spdk_io_channel_get_thread(ch)));

	spdk_fsdev_for_each_channel_continue(i, 0);
}

static void
rpc_fsdev_get_ch_iostat_done(struct spdk_fsdev *fsdev, void *_ctx, int status)
{
	struct rpc_fsdev_get_iostat_node *node = _ctx;
	struct rpc_fsdev_get_iostat_ctx *ctx = node->ctx;

	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);

	rpc_fsdev_get_iostat_next(ctx);
	free(node);
}

static void
rpc_fsdev_get_iostat_next(struct rpc_fsdev_get_iostat_ctx *ctx)
{
	int rc;
	struct rpc_fsdev_get_iostat_node *node;

do_retry:
	if (TAILQ_EMPTY(&ctx->nodes)) {
		spdk_json_write_array_end(ctx->w);
		spdk_json_write_object_end(ctx->w);
		spdk_jsonrpc_end_result(ctx->request, ctx->w);

		fsdev_free_get_iostat_ctx(ctx);
		return;
	}

	node = TAILQ_FIRST(&ctx->nodes);
	TAILQ_REMOVE(&ctx->nodes, node, link);

	rc = spdk_fsdev_open(node->fsdev_name, fsdev_stab_event_cb, NULL, &node->fsdev_desc);
	if (rc) {
		SPDK_ERRLOG("spdk_fsdev_open(%s) failed with %d\n", node->fsdev_name, rc);
		free(node);
		goto do_retry;
	}


	if (ctx->per_channel) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "name", node->fsdev_name);
		spdk_json_write_named_array_begin(ctx->w, "channels");

		spdk_fsdev_for_each_channel(spdk_fsdev_desc_get_fsdev(node->fsdev_desc),
					    rpc_fsdev_get_ch_iostat_msg, node, rpc_fsdev_get_ch_iostat_done);
	} else {
		spdk_fsdev_get_device_stat(spdk_fsdev_desc_get_fsdev(node->fsdev_desc), &node->stat,
					   rpc_fsdev_get_iostat_cpl, node);
	}
}

static int
rpc_fsdev_get_iostat_add(struct rpc_fsdev_get_iostat_ctx *ctx, const char *fsdev_name)
{
	struct rpc_fsdev_get_iostat_node *node;
	size_t len = strlen(fsdev_name);

	node = calloc(1, sizeof(*node) + len + 1);
	if (!node) {
		SPDK_ERRLOG("Cannot allocate node buffer for %s\n", fsdev_name);
		return -ENOMEM;
	}

	node->ctx = ctx;
	memcpy(node->fsdev_name, fsdev_name, len + 1);
	TAILQ_INSERT_TAIL(&ctx->nodes, node, link);

	return 0;
}

static int
rpc_fsdev_get_add_fsdev(void *_ctx, struct spdk_fsdev *fsdev)
{
	struct rpc_fsdev_get_iostat_ctx *ctx = _ctx;

	return rpc_fsdev_get_iostat_add(ctx, spdk_fsdev_get_name(fsdev));
}

static void
rpc_fsdev_get_iostat(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_get_iostat_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("calloc failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "calloc failed");
		return;
	}

	ctx->request = request;
	ctx->per_channel = false;
	TAILQ_INIT(&ctx->nodes);

	if (params && spdk_json_decode_object(params, rpc_fsdev_get_iostat_decoders,
					      SPDK_COUNTOF(rpc_fsdev_get_iostat_decoders),
					      ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		fsdev_free_get_iostat_ctx(ctx);
		return;
	}

	rc = ctx->name ? rpc_fsdev_get_iostat_add(ctx, ctx->name) :
	     spdk_for_each_fsdev(ctx, rpc_fsdev_get_add_fsdev);
	if (rc) {
		SPDK_ERRLOG("Cannot add format stat list\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "cannot allocate a buffer");
		fsdev_free_get_iostat_ctx(ctx);
		return;
	}

	ctx->w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_array_begin(ctx->w, "fsdevs");
	rpc_fsdev_get_iostat_next(ctx);
}
SPDK_RPC_REGISTER("fsdev_get_iostat", rpc_fsdev_get_iostat, SPDK_RPC_RUNTIME)

struct rpc_fsdev_reset_iostat_node {
	struct rpc_fsdev_reset_iostat_ctx *ctx;
	struct spdk_fsdev_desc *fsdev_desc;
	TAILQ_ENTRY(rpc_fsdev_reset_iostat_node) link;
	char fsdev_name[];
};

struct rpc_fsdev_reset_iostat_ctx {
	char *name;
	struct spdk_jsonrpc_request *request;
	TAILQ_HEAD(, rpc_fsdev_reset_iostat_node) nodes;
};

static const struct spdk_json_object_decoder rpc_fsdev_reset_iostat_decoders[] = {
	{"name", offsetof(struct rpc_fsdev_reset_iostat_ctx, name), spdk_json_decode_string, true},
};

static void
fsdev_free_reset_iostat_ctx(struct rpc_fsdev_reset_iostat_ctx *ctx)
{
	free(ctx->name);
	while (!TAILQ_EMPTY(&ctx->nodes)) {
		struct rpc_fsdev_reset_iostat_node *node = TAILQ_FIRST(&ctx->nodes);
		TAILQ_REMOVE(&ctx->nodes, node, link);
		if (node->fsdev_desc) {
			spdk_fsdev_close(node->fsdev_desc);
		}
		free(node);
	}
	free(ctx);
}

static void rpc_fsdev_reset_iostat_next(struct rpc_fsdev_reset_iostat_ctx *ctx);

static void
rpc_fsdev_reset_iostat_cpl(struct spdk_fsdev *fsdev, void *cb_arg, int rc)
{
	struct rpc_fsdev_reset_iostat_node *node = cb_arg;
	struct rpc_fsdev_reset_iostat_ctx *ctx = node->ctx;
	struct spdk_jsonrpc_request *request = ctx->request;

	spdk_fsdev_close(node->fsdev_desc);

	if (rc) {
		SPDK_ERRLOG("spdk_fsdev_reset_device_stat(%s) failed with %d\n", spdk_fsdev_get_name(fsdev), rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "spdk_fsdev_reset_device_stat(%s) failed with %d",
						     spdk_fsdev_get_name(fsdev), rc);
		free(node);
		fsdev_free_reset_iostat_ctx(ctx);
		return;
	}

	rpc_fsdev_reset_iostat_next(ctx);
	free(node);
}

static void
rpc_fsdev_reset_iostat_next(struct rpc_fsdev_reset_iostat_ctx *ctx)
{
	int rc;
	struct rpc_fsdev_reset_iostat_node *node;

do_retry:
	if (TAILQ_EMPTY(&ctx->nodes)) {
		struct spdk_jsonrpc_request *request = ctx->request;
		spdk_jsonrpc_send_bool_response(request, true);
		fsdev_free_reset_iostat_ctx(ctx);
		return;
	}

	node = TAILQ_FIRST(&ctx->nodes);
	TAILQ_REMOVE(&ctx->nodes, node, link);

	rc = spdk_fsdev_open(node->fsdev_name, fsdev_stab_event_cb, NULL, &node->fsdev_desc);
	if (rc) {
		SPDK_ERRLOG("spdk_fsdev_open(%s) failed with %d\n", node->fsdev_name, rc);
		free(node);
		goto do_retry;
	}

	spdk_fsdev_reset_device_stat(spdk_fsdev_desc_get_fsdev(node->fsdev_desc),
				     rpc_fsdev_reset_iostat_cpl, node);
}

static int
rpc_fsdev_reset_iostat_add(struct rpc_fsdev_reset_iostat_ctx *ctx, const char *fsdev_name)
{
	struct rpc_fsdev_reset_iostat_node *node;
	size_t len = strlen(fsdev_name);

	node = calloc(1, sizeof(*node) + len + 1);
	if (!node) {
		SPDK_ERRLOG("Cannot allocate node buffer for %s\n", fsdev_name);
		return -ENOMEM;
	}

	node->ctx = ctx;
	memcpy(node->fsdev_name, fsdev_name, len + 1);
	TAILQ_INSERT_TAIL(&ctx->nodes, node, link);

	return 0;
}

static int
rpc_fsdev_reset_iostat_fsdev(void *_ctx, struct spdk_fsdev *fsdev)
{
	struct rpc_fsdev_reset_iostat_ctx *ctx = _ctx;

	return rpc_fsdev_reset_iostat_add(ctx, spdk_fsdev_get_name(fsdev));
}

static void
rpc_fsdev_reset_iostat(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_reset_iostat_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("calloc failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to allocate memory for the request context");
		return;
	}

	ctx->request = request;
	TAILQ_INIT(&ctx->nodes);

	if (params &&
	    spdk_json_decode_object(params, rpc_fsdev_reset_iostat_decoders,
				    SPDK_COUNTOF(rpc_fsdev_reset_iostat_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		fsdev_free_reset_iostat_ctx(ctx);
		return;
	}

	rc = ctx->name ? rpc_fsdev_reset_iostat_add(ctx, ctx->name) :
	     spdk_for_each_fsdev(ctx, rpc_fsdev_reset_iostat_fsdev);
	if (rc) {
		SPDK_ERRLOG("Cannot format nodes list\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to form fsdev list");
		fsdev_free_reset_iostat_ctx(ctx);
		return;
	}

	rpc_fsdev_reset_iostat_next(ctx);
}
SPDK_RPC_REGISTER("fsdev_reset_iostat", rpc_fsdev_reset_iostat, SPDK_RPC_RUNTIME)
