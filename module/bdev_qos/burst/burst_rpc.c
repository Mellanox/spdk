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

static const char *const g_bdev_qos_metric_names[] = {
	"rw_iops", "rw_mbps", "r_mbps", "w_mbps",
};

/* Write the metrics array for one device (shared by aggregated and per-channel modes). */
static void
write_metrics_json(struct spdk_json_write_ctx *w,
		   const struct bdev_burst_qos_metric_stat metrics[4])
{
	int i;

	spdk_json_write_named_array_begin(w, "metrics");
	for (i = 0; i < 4; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "metric", g_bdev_qos_metric_names[i]);
		spdk_json_write_named_uint64(w, "throttled_events", metrics[i].throttled_events);
		spdk_json_write_named_uint64(w, "throttled_ticks",  metrics[i].throttled_ticks);
		spdk_json_write_named_uint64(w, "current_tokens",   metrics[i].current_tokens);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

/* ---- Shared outer context ---- */

struct rpc_burst_qos_get_stats_ctx {
	struct spdk_jsonrpc_request	*request;
	struct spdk_json_write_ctx	*w;
	int				 qos_count; /* guard + one per in-flight device */
	int				 rc;
};

static void
rpc_burst_qos_get_stats_started(struct rpc_burst_qos_get_stats_ctx *rpc_ctx)
{
	rpc_ctx->w = spdk_jsonrpc_begin_result(rpc_ctx->request);
	spdk_json_write_object_begin(rpc_ctx->w);
	spdk_json_write_named_uint64(rpc_ctx->w, "ticks_rate", spdk_get_ticks_hz());
}

static void
rpc_burst_qos_get_stats_done(struct rpc_burst_qos_get_stats_ctx *rpc_ctx)
{
	if (--rpc_ctx->qos_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_json_write_array_end(rpc_ctx->w);
		spdk_json_write_object_end(rpc_ctx->w);
		spdk_jsonrpc_end_result(rpc_ctx->request, rpc_ctx->w);
	} else {
		/* Return error response after processing all specified QoS devices
		 * completed or failed.
		 */
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));
	}

	free(rpc_ctx);
}

/* ---- Aggregated mode: per-device context ---- */

struct burst_qos_dev_stats_ctx {
	struct rpc_burst_qos_get_stats_ctx	*rpc_ctx;
	struct spdk_bdev_qos_desc		*desc;
	char					*name;
};

static void
burst_qos_dev_stats_done(void *cb_arg, const struct bdev_burst_qos_stats *stats, int status)
{
	struct burst_qos_dev_stats_ctx *dev_ctx = cb_arg;
	struct rpc_burst_qos_get_stats_ctx *rpc_ctx = dev_ctx->rpc_ctx;

	if (status != 0 && rpc_ctx->rc == 0) {
		rpc_ctx->rc = status;
	}

	/*
	 * Any failure while iterating the devices leaves the response unstarted, so
	 * rpc_ctx->w is NULL even for the devices that collected their stats fine.
	 */
	if (rpc_ctx->rc != 0) {
		goto done;
	}

	spdk_json_write_object_begin(rpc_ctx->w);
	spdk_json_write_named_string(rpc_ctx->w, "name", dev_ctx->name);
	write_metrics_json(rpc_ctx->w, stats->metrics);
	spdk_json_write_object_end(rpc_ctx->w);

done:
	spdk_bdev_qos_close(dev_ctx->desc);
	free(dev_ctx->name);
	free(dev_ctx);
	rpc_burst_qos_get_stats_done(rpc_ctx);
}

static int
burst_qos_get_stats_foreach(void *ctx, struct spdk_bdev_qos *qos)
{
	struct rpc_burst_qos_get_stats_ctx *rpc_ctx = ctx;
	struct burst_qos_dev_stats_ctx *dev_ctx;
	struct spdk_bdev_qos_desc *desc;
	int rc;

	dev_ctx = calloc(1, sizeof(*dev_ctx));
	if (dev_ctx == NULL) {
		return -ENOMEM;
	}

	dev_ctx->name = strdup(qos->name);
	if (dev_ctx->name == NULL) {
		free(dev_ctx);
		return -ENOMEM;
	}

	rc = spdk_bdev_qos_open(qos->name, &desc);
	if (rc != 0) {
		free(dev_ctx->name);
		free(dev_ctx);
		return rc;
	}

	dev_ctx->rpc_ctx = rpc_ctx;
	dev_ctx->desc = desc;
	rpc_ctx->qos_count++;

	bdev_burst_qos_get_stats(qos, burst_qos_dev_stats_done, dev_ctx);

	return 0;
}

/* ---- Per-channel mode ---- */

struct burst_qos_per_ch_ctx {
	struct rpc_burst_qos_get_stats_ctx	*rpc_ctx;
	struct spdk_bdev_qos_desc		*desc;
};

static void
burst_qos_per_ch_collect(struct spdk_io_channel_iter *it)
{
	struct burst_qos_per_ch_ctx *ch_ctx = spdk_io_channel_iter_get_ctx(it);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(it);
	struct spdk_bdev_qos_channel *qos_ch = spdk_io_channel_get_ctx(_ch);
	struct bdev_burst_qos_metric_stat metrics[4];

	if (!bdev_burst_qos_channel_get_per_channel_stats(qos_ch, metrics)) {
		spdk_for_each_channel_continue(it, 0);
		return;
	}

	spdk_json_write_object_begin(ch_ctx->rpc_ctx->w);
	spdk_json_write_named_uint64(ch_ctx->rpc_ctx->w, "thread_id",
				     spdk_thread_get_id(spdk_get_thread()));
	write_metrics_json(ch_ctx->rpc_ctx->w, metrics);
	spdk_json_write_object_end(ch_ctx->rpc_ctx->w);

	spdk_for_each_channel_continue(it, 0);
}

static void
burst_qos_per_ch_done(struct spdk_io_channel_iter *it, int status)
{
	struct burst_qos_per_ch_ctx *ch_ctx = spdk_io_channel_iter_get_ctx(it);
	struct rpc_burst_qos_get_stats_ctx *rpc_ctx = ch_ctx->rpc_ctx;

	if (status != 0 && rpc_ctx->rc == 0) {
		rpc_ctx->rc = status;
	}

	spdk_bdev_qos_close(ch_ctx->desc);
	free(ch_ctx);
	rpc_burst_qos_get_stats_done(rpc_ctx);
}

/* ---- RPC handler ---- */

struct rpc_bdev_burst_qos_get_stats {
	char	*name;
	bool	 per_channel;
};

static void
free_rpc_bdev_burst_qos_get_stats(struct rpc_bdev_burst_qos_get_stats *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_burst_qos_get_stats_decoders[] = {
	{"name", offsetof(struct rpc_bdev_burst_qos_get_stats, name), spdk_json_decode_string, true},
	{"per_channel", offsetof(struct rpc_bdev_burst_qos_get_stats, per_channel), spdk_json_decode_bool, true},
};

static void
rpc_bdev_burst_qos_get_stats(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_burst_qos_get_stats req = {};
	struct rpc_burst_qos_get_stats_ctx *rpc_ctx;
	struct spdk_bdev_qos_desc *desc = NULL;
	struct spdk_bdev_qos *qos = NULL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_burst_qos_get_stats_decoders,
					    SPDK_COUNTOF(rpc_bdev_burst_qos_get_stats_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_burst_qos_get_stats(&req);
			return;
		}

		if (req.per_channel && !req.name) {
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "QoS device name is required for per_channel stats");
			free_rpc_bdev_burst_qos_get_stats(&req);
			return;
		}

		if (req.name) {
			rc = spdk_bdev_qos_open(req.name, &desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open QoS dev '%s': %d\n", req.name, rc);
				spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
				free_rpc_bdev_burst_qos_get_stats(&req);
				return;
			}
			qos = spdk_bdev_qos_desc_get_qos(desc);
		}
	}

	rpc_ctx = calloc(1, sizeof(*rpc_ctx));
	if (rpc_ctx == NULL) {
		if (desc != NULL) {
			spdk_bdev_qos_close(desc);
		}
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free_rpc_bdev_burst_qos_get_stats(&req);
		return;
	}

	/*
	 * Increment initial qos_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	rpc_ctx->qos_count++;
	rpc_ctx->request = request;

	if (desc != NULL && req.per_channel) {
		/* Per-channel mode: single device, stream JSON inline per channel. */
		struct burst_qos_per_ch_ctx *ch_ctx;
		struct spdk_bdev_qos_impl *qos_impl;

		ch_ctx = calloc(1, sizeof(*ch_ctx));
		if (ch_ctx == NULL) {
			goto per_ch_oom;
		}

		qos_impl = bdev_burst_qos_find_impl(qos);
		if (qos_impl == NULL) {
			free(ch_ctx);
			spdk_bdev_qos_close(desc);
			free(rpc_ctx);
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "burst QoS not configured on this device");
			free_rpc_bdev_burst_qos_get_stats(&req);
			return;
		}

		ch_ctx->rpc_ctx = rpc_ctx;
		ch_ctx->desc = desc;
		rpc_ctx->qos_count++; /* for the per-channel async op */

		/* If per_channel is true, there is no failure after here and
		 * we have to start RPC response before executing
		 * spdk_for_each_channel().
		 */
		rpc_burst_qos_get_stats_started(rpc_ctx);
		spdk_json_write_named_string(rpc_ctx->w, "name", qos->name);
		spdk_json_write_named_array_begin(rpc_ctx->w, "channels");

		spdk_for_each_channel(qos_impl->qos,
				      burst_qos_per_ch_collect,
				      ch_ctx,
				      burst_qos_per_ch_done);

		rpc_burst_qos_get_stats_done(rpc_ctx); /* release guard */
		free_rpc_bdev_burst_qos_get_stats(&req);
		return;

per_ch_oom:
		spdk_bdev_qos_close(desc);
		free(rpc_ctx);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free_rpc_bdev_burst_qos_get_stats(&req);
		return;

	} else if (desc != NULL) {
		/* Aggregated mode: single named device. */
		struct burst_qos_dev_stats_ctx *dev_ctx;

		dev_ctx = calloc(1, sizeof(*dev_ctx));
		if (dev_ctx == NULL) {
			spdk_bdev_qos_close(desc);
			free(rpc_ctx);
			spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
			free_rpc_bdev_burst_qos_get_stats(&req);
			return;
		}

		dev_ctx->name = strdup(req.name);
		if (dev_ctx->name == NULL) {
			spdk_bdev_qos_close(desc);
			free(dev_ctx);
			free(rpc_ctx);
			spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
			free_rpc_bdev_burst_qos_get_stats(&req);
			return;
		}

		dev_ctx->rpc_ctx = rpc_ctx;
		dev_ctx->desc = desc;
		rpc_ctx->qos_count++; /* for this device */

		bdev_burst_qos_get_stats(qos, burst_qos_dev_stats_done, dev_ctx);

	} else {
		/* Aggregated mode: all QoS devices. */
		rc = spdk_bdev_for_each_qos(rpc_ctx, burst_qos_get_stats_foreach);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	if (rpc_ctx->rc == 0) {
		/* We want to fail the RPC for all failures. If per_channel is false,
		 * it is enough to defer starting RPC response until it is ensured that
		 * all async stats collections will succeed or there is no QoS device.
		 */
		rpc_burst_qos_get_stats_started(rpc_ctx);
		spdk_json_write_named_array_begin(rpc_ctx->w, "qos_devices");
	}

	rpc_burst_qos_get_stats_done(rpc_ctx); /* release guard */
	free_rpc_bdev_burst_qos_get_stats(&req);
}

SPDK_RPC_REGISTER("bdev_burst_qos_get_stats", rpc_bdev_burst_qos_get_stats,
		  SPDK_RPC_RUNTIME)
