/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_BURST_QOS_H
#define SPDK_BDEV_BURST_QOS_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

struct bdev_burst_qos_opts {
	/* Size of this structure in bytes. */
	size_t opts_size;

	/* The period of a single tick in microseconds. */
	uint64_t tick_period_us;

	/* Maximum batch size to withdraw for IOPS limiting. */
	uint64_t max_io_withdraw_batch_size;

	/* Step size to increase the IOPS withdraw batch on success. */
	uint64_t io_additive_increase_step;

	/* Maximum batch size (in bytes) to withdraw for bandwidth limiting. */
	uint64_t max_byte_withdraw_batch_size;

	/* Step size to increase the bandwidth withdraw batch on success. */
	uint64_t byte_additive_increase_step;

	/*
	 * Maximum number of queued I/Os dispatched per thread per retry poller
	 * invocation. Bounds CPU time spent in the retry drain loop.
	 * 0 = use default (BDEV_QOS_DEFAULT_RETRY_BUDGET).
	 * The field is uint64_t for ABI alignment but the implementation
	 * narrows it to uint32_t; values above UINT32_MAX are rejected.
	 */
	uint64_t retry_budget;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct bdev_burst_qos_opts) == 56, "Incorrect size");

void bdev_burst_qos_get_opts(struct bdev_burst_qos_opts *opts, size_t opts_size);

int bdev_burst_qos_set_opts(const struct bdev_burst_qos_opts *opts);

void bdev_burst_qos_set_limit_json(struct spdk_bdev_qos *qos,
				   const struct spdk_json_val *params,
				   spdk_bdev_qos_op_cb cb_fn, void *cb_arg);

/*
 * Per-metric statistics collected by bdev_burst_qos_get_stats().
 * Indexed by enum bdev_qos_metric (rw_iops=0, rw_mbps=1, r_mbps=2, w_mbps=3).
 */
struct bdev_burst_qos_metric_stat {
	/*
	 * Cumulative number of I/Os that had to wait in the QoS queue
	 * (both FIFO hold-backs and token-exhaustion stalls).
	 * Aggregated across all I/O threads.
	 */
	uint64_t throttled_events;

	/*
	 * Cumulative ticks each bucket instance spent in a throttled state
	 * (queue non-empty). Divide by ticks_rate to convert to seconds.
	 * Aggregated across all I/O threads.
	 */
	uint64_t throttled_ticks;

	/*
	 * Token count snapshot at query time.
	 * In aggregated mode: global steady-bucket count; 0 for disabled metrics.
	 * In per-channel mode: this thread's local bucket cache.
	 */
	uint64_t current_tokens;
};

struct bdev_burst_qos_stats {
	/* Ticks per second; use to convert throttled_ticks to wall-clock seconds. */
	uint64_t ticks_rate;

	/* One entry per QoS metric, ordered rw_iops, rw_mbps, r_mbps, w_mbps. */
	struct bdev_burst_qos_metric_stat metrics[4];
};

typedef void (*bdev_burst_qos_get_stats_cb)(void *cb_arg,
		const struct bdev_burst_qos_stats *stats, int status);

/*
 * Asynchronously collect per-metric statistics for a burst QoS device.
 * Uses spdk_for_each_channel internally; must be called from the app thread.
 * On completion, cb_fn is called with a filled bdev_burst_qos_stats (or NULL
 * and a non-zero status on error).
 */
void bdev_burst_qos_get_stats(struct spdk_bdev_qos *qos,
			      bdev_burst_qos_get_stats_cb cb_fn, void *cb_arg);

/* Return the burst QoS impl for a QoS device, or NULL if not configured. */
struct spdk_bdev_qos_impl *bdev_burst_qos_find_impl(struct spdk_bdev_qos *qos);

/*
 * Extract per-channel stats from a single QoS channel's local buckets.
 * out[i].current_tokens is this thread's local token cache (not the global bucket).
 * Must be called from an spdk_for_each_channel callback on the owning thread.
 * Returns false (and leaves out unchanged) if the burst impl is not present on
 * this channel.
 */
bool bdev_burst_qos_channel_get_per_channel_stats(struct spdk_bdev_qos_channel *qos_ch,
		struct bdev_burst_qos_metric_stat out[4]);


#endif /* SPDK_BDEV_BURST_QOS_H */
