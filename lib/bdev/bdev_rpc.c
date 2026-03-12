/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/bdev.h"

#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/base64.h"
#include "spdk/bdev_module.h"
#include "spdk/dma.h"

#include "spdk/log.h"

#include "spdk_internal/bdev_qos_module.h"

#include "bdev_internal.h"

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

static const struct spdk_json_object_decoder rpc_set_bdev_opts_decoders[] = {
	{"bdev_io_pool_size", offsetof(struct spdk_bdev_opts, bdev_io_pool_size), spdk_json_decode_uint32, true},
	{"bdev_io_cache_size", offsetof(struct spdk_bdev_opts, bdev_io_cache_size), spdk_json_decode_uint32, true},
	{"bdev_auto_examine", offsetof(struct spdk_bdev_opts, bdev_auto_examine), spdk_json_decode_bool, true},
	{"iobuf_small_cache_size", offsetof(struct spdk_bdev_opts, iobuf_small_cache_size), spdk_json_decode_uint32, true},
	{"iobuf_large_cache_size", offsetof(struct spdk_bdev_opts, iobuf_large_cache_size), spdk_json_decode_uint32, true},
	{"bdev_rw_bypass", offsetof(struct spdk_bdev_opts, bdev_rw_bypass), spdk_json_decode_bool, true},
};

static void
rpc_bdev_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_bdev_opts opts;
	int rc;

	spdk_bdev_get_opts(&opts, sizeof(opts));
	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_bdev_opts_decoders,
					    SPDK_COUNTOF(rpc_set_bdev_opts_decoders), &opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	rc = spdk_bdev_set_opts(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Pool size %" PRIu32 " too small for cache size %" PRIu32,
						     opts.bdev_io_pool_size, opts.bdev_io_cache_size);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_set_options", rpc_bdev_set_options, SPDK_RPC_STARTUP)

static void
rpc_bdev_wait_for_examine_cpl(void *arg)
{
	struct spdk_jsonrpc_request *request = arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_wait_for_examine(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_wait_for_examine requires no parameters");
		return;
	}

	rc = spdk_bdev_wait_for_examine(rpc_bdev_wait_for_examine_cpl, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("bdev_wait_for_examine", rpc_bdev_wait_for_examine, SPDK_RPC_RUNTIME)

struct rpc_bdev_examine {
	char *name;
};

static void
free_rpc_bdev_examine(struct rpc_bdev_examine *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_examine_bdev_decoders[] = {
	{"name", offsetof(struct rpc_bdev_examine, name), spdk_json_decode_string},
};

static void
rpc_bdev_examine_bdev(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_examine req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_examine_bdev_decoders,
				    SPDK_COUNTOF(rpc_examine_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_examine(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_examine(&req);
}
SPDK_RPC_REGISTER("bdev_examine", rpc_bdev_examine_bdev, SPDK_RPC_RUNTIME)

struct rpc_get_iostat_ctx {
	int bdev_count;
	int rc;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	bool per_channel;
	enum spdk_bdev_reset_stat_mode reset_mode;
};

struct bdev_get_iostat_ctx {
	struct spdk_bdev_io_stat *stat;
	struct rpc_get_iostat_ctx *rpc_ctx;
	struct spdk_bdev_desc *desc;
};

static void
rpc_get_iostat_started(struct rpc_get_iostat_ctx *rpc_ctx)
{
	rpc_ctx->w = spdk_jsonrpc_begin_result(rpc_ctx->request);

	spdk_json_write_object_begin(rpc_ctx->w);
	spdk_json_write_named_uint64(rpc_ctx->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_uint64(rpc_ctx->w, "ticks", spdk_get_ticks());
}

static void
rpc_get_iostat_done(struct rpc_get_iostat_ctx *rpc_ctx)
{
	if (--rpc_ctx->bdev_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_json_write_array_end(rpc_ctx->w);
		spdk_json_write_object_end(rpc_ctx->w);
		spdk_jsonrpc_end_result(rpc_ctx->request, rpc_ctx->w);
	} else {
		/* Return error response after processing all specified bdevs
		 * completed or failed.
		 */
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));
	}

	free(rpc_ctx);
}

static struct bdev_get_iostat_ctx *
bdev_iostat_ctx_alloc(bool iostat_ext)
{
	struct bdev_get_iostat_ctx *ctx;

	ctx = calloc(1, sizeof(struct bdev_get_iostat_ctx));
	if (ctx == NULL) {
		return NULL;
	}

	ctx->stat = bdev_alloc_io_stat(iostat_ext);
	if (ctx->stat == NULL) {
		free(ctx);
		return NULL;
	}

	return ctx;
}

static void
bdev_iostat_ctx_free(struct bdev_get_iostat_ctx *ctx)
{
	bdev_free_io_stat(ctx->stat);
	free(ctx);
}

static void
bdev_get_iostat_done(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
		     void *cb_arg, int rc)
{
	struct bdev_get_iostat_ctx *bdev_ctx = cb_arg;
	struct rpc_get_iostat_ctx *rpc_ctx = bdev_ctx->rpc_ctx;
	struct spdk_json_write_ctx *w = rpc_ctx->w;

	if (rc != 0 || rpc_ctx->rc != 0) {
		if (rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
		goto done;
	}

	assert(stat == bdev_ctx->stat);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));

	spdk_bdev_dump_io_stat_json(stat, w);

	if (spdk_bdev_get_qd_sampling_period(bdev)) {
		spdk_json_write_named_uint64(w, "queue_depth_polling_period",
					     spdk_bdev_get_qd_sampling_period(bdev));

		spdk_json_write_named_uint64(w, "queue_depth", spdk_bdev_get_qd(bdev));

		spdk_json_write_named_uint64(w, "io_time", spdk_bdev_get_io_time(bdev));

		spdk_json_write_named_uint64(w, "weighted_io_time",
					     spdk_bdev_get_weighted_io_time(bdev));
	}

	if (bdev->fn_table->dump_device_stat_json) {
		spdk_json_write_named_object_begin(w, "driver_specific");
		bdev->fn_table->dump_device_stat_json(bdev->ctxt, w);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);

done:
	rpc_get_iostat_done(rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);
	bdev_iostat_ctx_free(bdev_ctx);
}

static int
bdev_get_iostat(void *ctx, struct spdk_bdev *bdev)
{
	struct rpc_get_iostat_ctx *rpc_ctx = ctx;
	struct bdev_get_iostat_ctx *bdev_ctx;
	int rc;

	bdev_ctx = bdev_iostat_ctx_alloc(true);
	if (bdev_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, dummy_bdev_event_cb, NULL,
				&bdev_ctx->desc);
	if (rc != 0) {
		bdev_iostat_ctx_free(bdev_ctx);
		SPDK_ERRLOG("Failed to open bdev\n");
		return rc;
	}

	rpc_ctx->bdev_count++;
	bdev_ctx->rpc_ctx = rpc_ctx;
	spdk_bdev_get_device_stat(bdev, bdev_ctx->stat, rpc_ctx->reset_mode, bdev_get_iostat_done,
				  bdev_ctx);

	return 0;
}

static void
bdev_get_per_channel_stat_done(struct spdk_bdev *bdev, void *ctx, int status)
{
	struct bdev_get_iostat_ctx *bdev_ctx = ctx;

	rpc_get_iostat_done(bdev_ctx->rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);

	bdev_iostat_ctx_free(bdev_ctx);
}

static void
bdev_get_per_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			  struct spdk_io_channel *ch, void *ctx)
{
	struct bdev_get_iostat_ctx *bdev_ctx = ctx;
	struct spdk_json_write_ctx *w = bdev_ctx->rpc_ctx->w;

	spdk_bdev_get_io_stat(bdev, ch, bdev_ctx->stat, bdev_ctx->rpc_ctx->reset_mode);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "thread_id", spdk_thread_get_id(spdk_get_thread()));
	spdk_bdev_dump_io_stat_json(bdev_ctx->stat, w);
	spdk_json_write_object_end(w);

	spdk_bdev_for_each_channel_continue(i, 0);
}

struct rpc_bdev_get_iostat {
	char *name;
	bool per_channel;
	enum spdk_bdev_reset_stat_mode reset_mode;
};

static void
free_rpc_bdev_get_iostat(struct rpc_bdev_get_iostat *r)
{
	free(r->name);
}

static int
rpc_decode_reset_iostat_mode(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_reset_stat_mode *mode = out;

	if (spdk_json_strequal(val, "all") == true) {
		*mode = SPDK_BDEV_RESET_STAT_ALL;
	} else if (spdk_json_strequal(val, "maxmin") == true) {
		*mode = SPDK_BDEV_RESET_STAT_MAXMIN;
	} else if (spdk_json_strequal(val, "none") == true) {
		*mode = SPDK_BDEV_RESET_STAT_NONE;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: mode\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_get_iostat_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_iostat, name), spdk_json_decode_string, true},
	{"per_channel", offsetof(struct rpc_bdev_get_iostat, per_channel), spdk_json_decode_bool, true},
	{"reset_mode", offsetof(struct rpc_bdev_get_iostat, reset_mode), rpc_decode_reset_iostat_mode, true},
};

static void
rpc_bdev_get_iostat(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_get_iostat req = { .reset_mode = SPDK_BDEV_RESET_STAT_NONE };
	struct spdk_bdev_desc *desc = NULL;
	struct rpc_get_iostat_ctx *rpc_ctx;
	struct bdev_get_iostat_ctx *bdev_ctx;
	struct spdk_bdev *bdev;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_get_iostat_decoders,
					    SPDK_COUNTOF(rpc_bdev_get_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_get_iostat(&req);
			return;
		}

		if (req.per_channel == true && !req.name) {
			SPDK_ERRLOG("Bdev name is required for per channel IO statistics\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
			free_rpc_bdev_get_iostat(&req);
			return;
		}

		if (req.name) {
			rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
				spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
				free_rpc_bdev_get_iostat(&req);
				return;
			}
		}
	}

	free_rpc_bdev_get_iostat(&req);

	rpc_ctx = calloc(1, sizeof(struct rpc_get_iostat_ctx));
	if (rpc_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	rpc_ctx->bdev_count++;
	rpc_ctx->request = request;
	rpc_ctx->per_channel = req.per_channel;
	rpc_ctx->reset_mode = req.reset_mode;

	if (desc != NULL) {
		bdev = spdk_bdev_desc_get_bdev(desc);

		bdev_ctx = bdev_iostat_ctx_alloc(req.per_channel == false);
		if (bdev_ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
			rpc_ctx->rc = -ENOMEM;

			spdk_bdev_close(desc);
		} else {
			bdev_ctx->desc = desc;

			rpc_ctx->bdev_count++;
			bdev_ctx->rpc_ctx = rpc_ctx;
			if (req.per_channel == false) {
				spdk_bdev_get_device_stat(bdev, bdev_ctx->stat, rpc_ctx->reset_mode,
							  bdev_get_iostat_done, bdev_ctx);
			} else {
				/* If per_channel is true, there is no failure after here and
				 * we have to start RPC response before executing
				 * spdk_bdev_for_each_channel().
				 */
				rpc_get_iostat_started(rpc_ctx);
				spdk_json_write_named_string(rpc_ctx->w, "name", spdk_bdev_get_name(bdev));
				spdk_json_write_named_array_begin(rpc_ctx->w, "channels");

				spdk_bdev_for_each_channel(bdev,
							   bdev_get_per_channel_stat,
							   bdev_ctx,
							   bdev_get_per_channel_stat_done);

				rpc_get_iostat_done(rpc_ctx);
				return;
			}
		}
	} else {
		rc = spdk_for_each_bdev(rpc_ctx, bdev_get_iostat);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	if (rpc_ctx->rc == 0) {
		/* We want to fail the RPC for all failures. If per_channel is false,
		 * it is enough to defer starting RPC response until it is ensured that
		 * all spdk_bdev_for_each_channel() calls will succeed or there is no bdev.
		 */
		rpc_get_iostat_started(rpc_ctx);
		spdk_json_write_named_array_begin(rpc_ctx->w, "bdevs");
	}

	rpc_get_iostat_done(rpc_ctx);
}
SPDK_RPC_REGISTER("bdev_get_iostat", rpc_bdev_get_iostat, SPDK_RPC_RUNTIME)

struct rpc_reset_iostat_ctx {
	int bdev_count;
	int rc;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	enum spdk_bdev_reset_stat_mode mode;
};

struct bdev_reset_iostat_ctx {
	struct rpc_reset_iostat_ctx *rpc_ctx;
	struct spdk_bdev_desc *desc;
};

static void
rpc_reset_iostat_done(struct rpc_reset_iostat_ctx *rpc_ctx)
{
	if (--rpc_ctx->bdev_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_jsonrpc_send_bool_response(rpc_ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));
	}

	free(rpc_ctx);
}

static void
bdev_reset_iostat_done(struct spdk_bdev *bdev, void *cb_arg, int rc)
{
	struct bdev_reset_iostat_ctx *bdev_ctx = cb_arg;
	struct rpc_reset_iostat_ctx *rpc_ctx = bdev_ctx->rpc_ctx;

	if (rc != 0 || rpc_ctx->rc != 0) {
		if (rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	rpc_reset_iostat_done(rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);
	free(bdev_ctx);
}

static int
bdev_reset_iostat(void *ctx, struct spdk_bdev *bdev)
{
	struct rpc_reset_iostat_ctx *rpc_ctx = ctx;
	struct bdev_reset_iostat_ctx *bdev_ctx;
	int rc;

	bdev_ctx = calloc(1, sizeof(struct bdev_reset_iostat_ctx));
	if (bdev_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, dummy_bdev_event_cb, NULL,
				&bdev_ctx->desc);
	if (rc != 0) {
		free(bdev_ctx);
		SPDK_ERRLOG("Failed to open bdev\n");
		return rc;
	}

	if (bdev->fn_table->reset_device_stat) {
		bdev->fn_table->reset_device_stat(bdev->ctxt);
	}

	rpc_ctx->bdev_count++;
	bdev_ctx->rpc_ctx = rpc_ctx;
	bdev_reset_device_stat(bdev, rpc_ctx->mode, bdev_reset_iostat_done, bdev_ctx);

	return 0;
}

struct rpc_bdev_reset_iostat {
	char *name;
	enum spdk_bdev_reset_stat_mode mode;
};

static void
free_rpc_bdev_reset_iostat(struct rpc_bdev_reset_iostat *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_reset_iostat_decoders[] = {
	{"name", offsetof(struct rpc_bdev_reset_iostat, name), spdk_json_decode_string, true},
	{"mode", offsetof(struct rpc_bdev_reset_iostat, mode), rpc_decode_reset_iostat_mode, true},
};

static void
rpc_bdev_reset_iostat(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_reset_iostat req = { .mode = SPDK_BDEV_RESET_STAT_ALL, };
	struct spdk_bdev_desc *desc = NULL;
	struct rpc_reset_iostat_ctx *rpc_ctx;
	struct bdev_reset_iostat_ctx *bdev_ctx;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_reset_iostat_decoders,
					    SPDK_COUNTOF(rpc_bdev_reset_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_reset_iostat(&req);
			return;
		}

		if (req.mode == SPDK_BDEV_RESET_STAT_NONE) {
			SPDK_NOTICELOG("bdev_reset_iostat called with mode none, aborting operation\n");
			spdk_jsonrpc_send_bool_response(request, true);
			free_rpc_bdev_reset_iostat(&req);
			return;
		}

		if (req.name) {
			rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
				spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
				free_rpc_bdev_reset_iostat(&req);
				return;
			}
		}
	}


	rpc_ctx = calloc(1, sizeof(struct rpc_reset_iostat_ctx));
	if (rpc_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free_rpc_bdev_reset_iostat(&req);
		return;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	rpc_ctx->bdev_count++;
	rpc_ctx->request = request;
	rpc_ctx->mode = req.mode;

	free_rpc_bdev_reset_iostat(&req);

	if (desc != NULL) {
		bdev_ctx = calloc(1, sizeof(struct bdev_reset_iostat_ctx));
		if (bdev_ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
			rpc_ctx->rc = -ENOMEM;

			spdk_bdev_close(desc);
		} else {
			bdev_ctx->desc = desc;

			rpc_ctx->bdev_count++;
			bdev_ctx->rpc_ctx = rpc_ctx;
			bdev_reset_device_stat(spdk_bdev_desc_get_bdev(desc), rpc_ctx->mode,
					       bdev_reset_iostat_done, bdev_ctx);
		}
	} else {
		rc = spdk_for_each_bdev(rpc_ctx, bdev_reset_iostat);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	rpc_reset_iostat_done(rpc_ctx);
}
SPDK_RPC_REGISTER("bdev_reset_iostat", rpc_bdev_reset_iostat, SPDK_RPC_RUNTIME)

static int
rpc_dump_bdev_info(void *ctx, struct spdk_bdev *bdev)
{
	struct spdk_json_write_ctx *w = ctx;
	struct spdk_bdev_alias *tmp;
	enum spdk_dma_device_type types[16];
	enum spdk_bdev_io_type io_type;
	const char *name = NULL;
	struct spdk_bdev_qos *qos;
	struct spdk_bdev_qos_impl *qos_impl;
	int i, rc;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));

	spdk_json_write_named_array_begin(w, "aliases");

	TAILQ_FOREACH(tmp, spdk_bdev_get_aliases(bdev), tailq) {
		spdk_json_write_string(w, tmp->alias.name);
	}

	spdk_json_write_array_end(w);

	spdk_json_write_named_string(w, "product_name", spdk_bdev_get_product_name(bdev));
	spdk_json_write_named_uint32(w, "block_size", spdk_bdev_get_block_size(bdev));
	spdk_json_write_named_uint64(w, "num_blocks", spdk_bdev_get_num_blocks(bdev));
	spdk_json_write_named_uint32(w, "preferred_write_alignment",
				     spdk_bdev_get_preferred_write_alignment(bdev));
	spdk_json_write_named_uint32(w, "preferred_write_granularity",
				     spdk_bdev_get_preferred_write_granularity(bdev));
	spdk_json_write_named_uint32(w, "optimal_write_size", spdk_bdev_get_optimal_write_size(bdev));
	spdk_json_write_named_uint32(w, "preferred_unmap_alignment",
				     spdk_bdev_get_preferred_unmap_alignment(bdev));
	spdk_json_write_named_uint32(w, "preferred_unmap_granularity",
				     spdk_bdev_get_preferred_unmap_granularity(bdev));
	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
	if (bdev->numa.id_valid) {
		spdk_json_write_named_int32(w, "numa_id", bdev->numa.id);
	}

	if (spdk_bdev_get_md_size(bdev) != 0) {
		spdk_json_write_named_uint32(w, "md_size", spdk_bdev_get_md_size(bdev));
		spdk_json_write_named_bool(w, "md_interleave", spdk_bdev_is_md_interleaved(bdev));
		spdk_json_write_named_uint32(w, "dif_type", spdk_bdev_get_dif_type(bdev));
		if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
			spdk_json_write_named_bool(w, "dif_is_head_of_md", spdk_bdev_is_dif_head_of_md(bdev));
			spdk_json_write_named_object_begin(w, "enabled_dif_check_types");
			spdk_json_write_named_bool(w, "reftag",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG));
			spdk_json_write_named_bool(w, "apptag",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_APPTAG));
			spdk_json_write_named_bool(w, "guard",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD));
			spdk_json_write_object_end(w);

			spdk_json_write_named_uint32(w, "dif_pi_format", spdk_bdev_get_dif_pi_format(bdev));
		}
	}

	qos = bdev->internal.qos;
	if (qos != NULL) {
		spdk_json_write_named_object_begin(w, "qos");
		spdk_json_write_named_string(w, "name", qos->name);

		spdk_json_write_named_array_begin(w, "module_specific");

		TAILQ_FOREACH(qos_impl, &qos->impl_list, link) {
			qos_impl->module->impl_info_json(qos_impl, w);
		}

		spdk_json_write_array_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_named_bool(w, "claimed",
				   (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE));
	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		spdk_json_write_named_string(w, "claim_type",
					     spdk_bdev_claim_get_name(bdev->internal.claim_type));
	}

	spdk_json_write_named_bool(w, "zoned", bdev->zoned);
	if (bdev->zoned) {
		spdk_json_write_named_uint64(w, "zone_size", bdev->zone_size);
		spdk_json_write_named_uint64(w, "max_open_zones", bdev->max_open_zones);
		spdk_json_write_named_uint64(w, "optimal_open_zones", bdev->optimal_open_zones);
	}

	spdk_json_write_named_object_begin(w, "supported_io_types");
	for (io_type = SPDK_BDEV_IO_TYPE_READ; io_type < SPDK_BDEV_NUM_IO_TYPES; ++io_type) {
		name = spdk_bdev_get_io_type_name(io_type);
		spdk_json_write_named_bool(w, name, spdk_bdev_io_type_supported(bdev, io_type));
	}
	spdk_json_write_object_end(w);

	rc = spdk_bdev_get_memory_domain_types(bdev, types, SPDK_COUNTOF(types));
	if (rc > 0) {
		if (rc > (int)SPDK_COUNTOF(types)) {
			SPDK_ERRLOG("Unexpected high number (%d) of memory domain types, listing only "
				    "first %d.\n", rc, (int)SPDK_COUNTOF(types));
		}
		spdk_json_write_named_array_begin(w, "memory_domains");
		for (i = 0; i < spdk_min(rc, (int)SPDK_COUNTOF(types)); i++) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "dma_device_type",
						     spdk_dma_device_type_get_name(types[i]));
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}

	spdk_json_write_named_object_begin(w, "driver_specific");
	spdk_bdev_dump_info_json(bdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

struct rpc_bdev_get_bdevs {
	char		*name;
	uint64_t	timeout;
};

static void
free_rpc_bdev_get_bdevs(struct rpc_bdev_get_bdevs *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_get_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_bdevs, name), spdk_json_decode_string, true},
	{"timeout", offsetof(struct rpc_bdev_get_bdevs, timeout), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_get_bdev_cb(struct spdk_bdev_desc *desc, int rc, void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (rc == 0) {
		w = spdk_jsonrpc_begin_result(request);

		spdk_json_write_array_begin(w);
		rpc_dump_bdev_info(w, spdk_bdev_desc_get_bdev(desc));
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);

		spdk_bdev_close(desc);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

static void
rpc_bdev_get_bdevs(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_bdev_get_bdevs req = {};
	struct spdk_bdev_open_async_opts opts = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (params && spdk_json_decode_object(params, rpc_bdev_get_bdevs_decoders,
					      SPDK_COUNTOF(rpc_bdev_get_bdevs_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_bdev_get_bdevs(&req);
		return;
	}

	if (req.name) {
		opts.size = sizeof(opts);
		opts.timeout_ms = req.timeout;

		rc = spdk_bdev_open_async(req.name, false, dummy_bdev_event_cb, NULL, &opts,
					  rpc_bdev_get_bdev_cb, request);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_bdev_open_async failed for '%s': rc=%d\n", req.name, rc);
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		}

		free_rpc_bdev_get_bdevs(&req);
		return;
	}

	free_rpc_bdev_get_bdevs(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	spdk_for_each_bdev(w, rpc_dump_bdev_info);

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_get_bdevs", rpc_bdev_get_bdevs, SPDK_RPC_RUNTIME)

struct rpc_bdev_set_qd_sampling_period {
	char *name;
	uint64_t period;
};

static void
free_rpc_bdev_set_qd_sampling_period(struct rpc_bdev_set_qd_sampling_period *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder
	rpc_bdev_set_qd_sampling_period_decoders[] = {
	{"name", offsetof(struct rpc_bdev_set_qd_sampling_period, name), spdk_json_decode_string},
	{"period", offsetof(struct rpc_bdev_set_qd_sampling_period, period), spdk_json_decode_uint64},
};

static void
rpc_bdev_set_qd_sampling_period(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_set_qd_sampling_period req = {0};
	struct spdk_bdev_desc *desc;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_set_qd_sampling_period_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qd_sampling_period_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_bdev_set_qd_sampling_period(spdk_bdev_desc_get_bdev(desc), req.period);
	spdk_jsonrpc_send_bool_response(request, true);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_set_qd_sampling_period(&req);
}
SPDK_RPC_REGISTER("bdev_set_qd_sampling_period",
		  rpc_bdev_set_qd_sampling_period,
		  SPDK_RPC_RUNTIME)

#define MAX_NUM_MODULES	8

struct rpc_qos_modules {
	char *modules[MAX_NUM_MODULES];
	size_t num_modules;
};

static int
decode_rpc_modules(const struct spdk_json_val *val, void *out)
{
	struct rpc_qos_modules *list = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, list->modules,
				      MAX_NUM_MODULES, &list->num_modules, sizeof(char *));
}

static void
free_rpc_qos_modules(struct rpc_qos_modules *list)
{
	size_t i;

	for (i = 0; i < list->num_modules; i++) {
		free(list->modules[i]);
	}
}

struct rpc_qos_create {
	char *name;
	char *parent_name;
	struct rpc_qos_modules modules;
};

static void
free_rpc_qos_create(struct rpc_qos_create *req)
{
	free(req->name);
	free(req->parent_name);
	free_rpc_qos_modules(&req->modules);
}

static const struct spdk_json_object_decoder rpc_qos_create_decoders[] = {
	{"name", offsetof(struct rpc_qos_create, name), spdk_json_decode_string},
	{"parent_name", offsetof(struct rpc_qos_create, parent_name), spdk_json_decode_string, true},
	{"modules", offsetof(struct rpc_qos_create, modules), decode_rpc_modules, true},
};

static void
rpc_bdev_qos_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_qos_create req = {};
	struct spdk_bdev_qos_desc *desc = NULL;
	struct spdk_bdev_qos *parent = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_qos_create_decoders,
				    SPDK_COUNTOF(rpc_qos_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Default to use "burst" module if user omitted modules */
	if (req.modules.num_modules == 0) {
		req.modules.modules[0] = strdup("burst");
		if (req.modules.modules[0] == NULL) {
			spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
			goto cleanup;
		}
		req.modules.num_modules = 1;
	}

	if (req.parent_name != NULL) {
		rc = spdk_bdev_qos_open(req.parent_name, &desc);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
			goto cleanup;
		}
		parent = spdk_bdev_qos_desc_get_qos(desc);
	}

	rc = spdk_bdev_qos_create(req.name, parent, (const char **)req.modules.modules,
				  req.modules.num_modules, NULL, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create QoS %s, %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	if (desc != NULL) {
		spdk_bdev_qos_close(desc);
	}
	free_rpc_qos_create(&req);
}
SPDK_RPC_REGISTER("bdev_qos_create", rpc_bdev_qos_create, SPDK_RPC_RUNTIME)

struct rpc_qos_destroy {
	char *name;
};

static void
free_rpc_qos_destroy(struct rpc_qos_destroy *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_qos_destroy_decoders[] = {
	{"name", offsetof(struct rpc_qos_destroy, name), spdk_json_decode_string},
};

static void
rpc_bdev_qos_destroy(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_qos_destroy req = {};
	struct spdk_bdev_qos_desc *desc = NULL;
	struct spdk_bdev_qos *qos;
	int rc;

	if (spdk_json_decode_object(params, rpc_qos_destroy_decoders,
				    SPDK_COUNTOF(rpc_qos_destroy_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_qos_open(req.name, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open QoS %s, %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	qos = spdk_bdev_qos_desc_get_qos(desc);

	rc = spdk_bdev_qos_destroy(qos);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to delete QoS %s, %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	if (desc != NULL) {
		spdk_bdev_qos_close(desc);
	}
	free_rpc_qos_destroy(&req);
}
SPDK_RPC_REGISTER("bdev_qos_destroy", rpc_bdev_qos_destroy, SPDK_RPC_RUNTIME)

struct qos_add_remove_bdev_ctx {
	char *name;
	char *bdev_name;
	struct spdk_jsonrpc_request *request;
	struct spdk_bdev_qos_desc *qos_desc;
	struct spdk_bdev_desc *bdev_desc;
};

static void
free_qos_add_remove_bdev_ctx(struct qos_add_remove_bdev_ctx *ctx)
{
	if (ctx->bdev_desc != NULL) {
		spdk_bdev_close(ctx->bdev_desc);
	}
	if (ctx->qos_desc != NULL) {
		spdk_bdev_qos_close(ctx->qos_desc);
	}
	free(ctx->name);
	free(ctx->bdev_name);
	free(ctx);
}

static const struct spdk_json_object_decoder qos_add_remove_bdev_decoders[] = {
	{"name", offsetof(struct qos_add_remove_bdev_ctx, name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct qos_add_remove_bdev_ctx, bdev_name), spdk_json_decode_string},
};

static void
bdev_qos_add_remove_bdev_done(void *cb_arg, int status)
{
	struct qos_add_remove_bdev_ctx *ctx = cb_arg;

	if (status == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, status,
						 spdk_strerror(-status));
	}

	free_qos_add_remove_bdev_ctx(ctx);
}

static void
_rpc_bdev_qos_add_remove_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params,
			      bool add)
{
	struct qos_add_remove_bdev_ctx *ctx;
	struct spdk_bdev_qos *qos;
	struct spdk_bdev *bdev;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, qos_add_remove_bdev_decoders,
				    SPDK_COUNTOF(qos_add_remove_bdev_decoders), ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_qos_open(ctx->name, &ctx->qos_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open QoS %s, %d\n", ctx->name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(ctx->bdev_name, false, dummy_bdev_event_cb,
				NULL, &ctx->bdev_desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	qos = spdk_bdev_qos_desc_get_qos(ctx->qos_desc);
	bdev = spdk_bdev_desc_get_bdev(ctx->bdev_desc);

	if (add) {
		spdk_bdev_qos_add_bdev(qos, bdev, bdev_qos_add_remove_bdev_done, ctx);
	} else {
		spdk_bdev_qos_remove_bdev(qos, bdev, bdev_qos_add_remove_bdev_done, ctx);
	}
	return;

cleanup:
	free_qos_add_remove_bdev_ctx(ctx);
}

static void
rpc_bdev_qos_add_bdev(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	_rpc_bdev_qos_add_remove_bdev(request, params, true);
}
SPDK_RPC_REGISTER("bdev_qos_add_bdev", rpc_bdev_qos_add_bdev, SPDK_RPC_RUNTIME)

static void
rpc_bdev_qos_remove_bdev(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	_rpc_bdev_qos_add_remove_bdev(request, params, false);
}
SPDK_RPC_REGISTER("bdev_qos_remove_bdev", rpc_bdev_qos_remove_bdev, SPDK_RPC_RUNTIME)

static int
rpc_bdev_qos_dump_info(void *ctx, struct spdk_bdev_qos *qos)
{
	struct spdk_json_write_ctx *w = ctx;
	struct spdk_bdev *bdev;
	struct spdk_bdev_qos *parent;
	struct spdk_bdev_qos_impl *qos_impl;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", qos->name);

	parent = qos->parent;
	if (parent != NULL) {
		spdk_json_write_named_string(w, "parent", parent->name);
	}

	spdk_json_write_named_array_begin(w, "bdevs");
	TAILQ_FOREACH(bdev, &qos->bdevs, internal.qos_link) {
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_array_begin(w, "module_specific");

	TAILQ_FOREACH(qos_impl, &qos->impl_list, link) {
		qos_impl->module->impl_info_json(qos_impl, w);
	}

	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static void
rpc_bdev_get_qos_devs(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_get_qos_devs requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	spdk_bdev_for_each_qos(w, rpc_bdev_qos_dump_info);

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_get_qos_devs", rpc_bdev_get_qos_devs, SPDK_RPC_RUNTIME)

/* SPDK_RPC_ENABLE_BDEV_HISTOGRAM */

struct rpc_bdev_enable_histogram_request {
	char *name;
	bool enable;
	char *opc;
	uint8_t granularity;
	uint64_t min_nsec;
	uint64_t max_nsec;
};

static void
free_rpc_bdev_enable_histogram_request(struct rpc_bdev_enable_histogram_request *r)
{
	free(r->name);
	free(r->opc);
}

static const struct spdk_json_object_decoder rpc_bdev_enable_histogram_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_enable_histogram_request, name), spdk_json_decode_string},
	{"enable", offsetof(struct rpc_bdev_enable_histogram_request, enable), spdk_json_decode_bool},
	{"opc", offsetof(struct rpc_bdev_enable_histogram_request, opc), spdk_json_decode_string, true},
	{"granularity", offsetof(struct rpc_bdev_enable_histogram_request, granularity), spdk_json_decode_uint8, true},
	{"min_nsec", offsetof(struct rpc_bdev_enable_histogram_request, min_nsec), spdk_json_decode_uint64, true},
	{"max_nsec", offsetof(struct rpc_bdev_enable_histogram_request, max_nsec), spdk_json_decode_uint64, true},
};

static void
bdev_histogram_status_cb(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (status == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
	}
}

static void
rpc_bdev_enable_histogram(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_enable_histogram_request req = {.granularity = SPDK_HISTOGRAM_GRANULARITY_DEFAULT,
		       .min_nsec = 0,
		       .max_nsec = UINT64_MAX
	};
	struct spdk_bdev_desc *desc;
	int rc;
	struct spdk_bdev_enable_histogram_opts opts = {};
	int io_type = 0;

	if (spdk_json_decode_object(params, rpc_bdev_enable_histogram_request_decoders,
				    SPDK_COUNTOF(rpc_bdev_enable_histogram_request_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_bdev_enable_histogram_opts_init(&opts, sizeof(opts));

	if (req.opc != NULL) {
		io_type = spdk_bdev_get_io_type(req.opc);
		if (io_type == -1) {
			SPDK_ERRLOG("Invalid IO type\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Invalid Io type");
			goto cleanup;
		}
		opts.io_type = (uint8_t) io_type;
	}

	opts.granularity = req.granularity;
	opts.min_nsec = req.min_nsec;
	opts.max_nsec = req.max_nsec;

	spdk_bdev_histogram_enable_ext(spdk_bdev_desc_get_bdev(desc), bdev_histogram_status_cb,
				       request, req.enable, &opts);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_enable_histogram_request(&req);
}

SPDK_RPC_REGISTER("bdev_enable_histogram", rpc_bdev_enable_histogram, SPDK_RPC_RUNTIME)

/* SPDK_RPC_GET_BDEV_HISTOGRAM */

struct rpc_bdev_get_histogram_request {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_get_histogram_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_histogram_request, name), spdk_json_decode_string}
};

static void
free_rpc_bdev_get_histogram_request(struct rpc_bdev_get_histogram_request *r)
{
	free(r->name);
}

static void
_rpc_bdev_histogram_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;
	int rc;
	char *encoded_histogram;
	size_t src_len, dst_len;


	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-status));
		goto invalid;
	}

	src_len = SPDK_HISTOGRAM_NUM_BUCKETS(histogram) * sizeof(uint64_t);
	dst_len = spdk_base64_get_encoded_strlen(src_len) + 1;

	encoded_histogram = malloc(dst_len);
	if (encoded_histogram == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		goto invalid;
	}

	rc = spdk_base64_encode(encoded_histogram, histogram->bucket, src_len);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto free_encoded_histogram;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "histogram", encoded_histogram);
	spdk_json_write_named_int64(w, "granularity", histogram->granularity);
	spdk_json_write_named_uint32(w, "min_range", histogram->min_range);
	spdk_json_write_named_uint32(w, "max_range", histogram->max_range);
	spdk_json_write_named_int64(w, "tsc_rate", spdk_get_ticks_hz());
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

free_encoded_histogram:
	free(encoded_histogram);
invalid:
	spdk_histogram_data_free(histogram);
}

static void
rpc_bdev_get_histogram(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_get_histogram_request req = {NULL};
	struct spdk_histogram_data *histogram;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_get_histogram_request_decoders,
				    SPDK_COUNTOF(rpc_bdev_get_histogram_request_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	histogram = spdk_histogram_data_alloc_sized_ext(bdev->internal.histogram_granularity,
			bdev->internal.histogram_min_val, bdev->internal.histogram_max_val);
	if (histogram == NULL) {
		spdk_bdev_close(desc);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	spdk_bdev_histogram_get(bdev, histogram,
				_rpc_bdev_histogram_data_cb, request);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_get_histogram_request(&req);
}

SPDK_RPC_REGISTER("bdev_get_histogram", rpc_bdev_get_histogram, SPDK_RPC_RUNTIME)

struct rpc_bdev_set_ro_in {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_set_ro_decoders[] = {
	{"name", offsetof(struct rpc_bdev_set_ro_in, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_set_ro(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_bdev_set_ro_in attr = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_set_ro_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_ro_decoders),
				    &attr)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	bdev = spdk_bdev_get_by_name((const char *)attr.name);
	if (!bdev) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Block device with the requested name doesn't exist");
		return;
	}

	spdk_bdev_set_ro(bdev, true);

	spdk_jsonrpc_send_bool_response(request, true);
	return;
}

SPDK_RPC_REGISTER("bdev_set_ro", rpc_bdev_set_ro, SPDK_RPC_RUNTIME);

struct rpc_bdev_set_rw_in {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_set_rw_decoders[] = {
	{"name", offsetof(struct rpc_bdev_set_rw_in, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_set_rw(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_bdev_set_rw_in attr = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_set_rw_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_rw_decoders),
				    &attr)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	bdev = spdk_bdev_get_by_name((const char *)attr.name);
	if (!bdev) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Block device with the requested name doesn't exist");
		return;
	}

	spdk_bdev_set_ro(bdev, false);

	spdk_jsonrpc_send_bool_response(request, true);
	return;
}

SPDK_RPC_REGISTER("bdev_set_rw", rpc_bdev_set_rw, SPDK_RPC_RUNTIME);
