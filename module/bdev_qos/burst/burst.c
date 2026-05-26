/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/bdev_qos_module.h"

#include "burst.h"

#define BDEV_QOS_TICK_PERIOD_US			10000
#define BDEV_QOS_MIN_IO_WITHDRAW_BATCH_SIZE	1
#define BDEV_QOS_MAX_IO_WITHDRAW_BATCH_SIZE	64
#define BDEV_QOS_IO_ADDITIVE_INCREASE_STEP	8
#define BDEV_QOS_MIN_BYTE_WITHDRAW_BATCH_SIZE	4096
#define BDEV_QOS_MAX_BYTE_WITHDRAW_BATCH_SIZE	131072
#define BDEV_QOS_BYTE_ADDITIVE_INCREASE_STEP	16384
#define BDEV_QOS_MIN_IO_PER_SEC			1000
#define BDEV_QOS_MIN_BYTE_PER_SEC		(1024 * 1024)
#define BDEV_QOS_DEFAULT_IO_BURST_SIZE		64
#define BDEV_QOS_DEFAULT_IO_BURST_BYTES		(64 * 65536)
#define BDEV_QOS_DEFAULT_RETRY_BUDGET		256

/** Metric controlled by QoS rate limit */
enum bdev_qos_metric {
	/** IOPS rate limit for both read and write */
	BDEV_QOS_RW_IOPS = 0,
	/** Byte per second rate limit for both read and write */
	BDEV_QOS_RW_BPS,
	/** Byte per second rate limit for read only */
	BDEV_QOS_R_BPS,
	/** Byte per second rate limit for write only */
	BDEV_QOS_W_BPS,
	/** Keep last */
	BDEV_QOS_NUM_METRICS,
};

/** Operating mode for QoS rate limit */
enum bdev_qos_mode {
	/**
	 * Leaky bucket. Enforces avg_rate strictly. Burst parameters are ignored.
	 */
	BDEV_QOS_MODE_STRICT,
	/**
	 * Standard token bucket. Starts ready to burst immediately.
	 */
	BDEV_QOS_MODE_BURST_READY,
	/**
	 * Starts strict like leaky bucket, but can burst later if credits are earned.
	 */
	BDEV_QOS_MODE_EARNED_BURST,
};

struct token_bucket {
	/*
	 * Current tokens available. Consumers subtract from this; it never goes
	 * below zero. steady_bucket->tokens is accessed by multiple I/O threads
	 * via atomics; burst_bucket->tokens is touched only by the app-thread
	 * refill poller and requires no atomic operations.
	 */
	uint64_t tokens;

	/* Upper bound on tokens. Adding income beyond this amount produces
	 * overflow that may be redirected to the burst bucket.
	 */
	uint64_t capacity;
};

struct local_token_bucket;

struct global_token_bucket {
	/*
	 * Steady bucket is the primary "spending account" that I/O threads consume
	 * tokens from directly (via atomic_sub_floor). Its capacity and initial
	 * fill depend on the operating mode:
	 *
	 *   STRICT:       capacity = income_per_refill; starts empty.
	 *                 Tokens are replenished at exactly avg_rate — no bursting.
	 *
	 *   BURST_READY:  capacity = burst_size (≥ avg_rate); starts fully loaded.
	 *                 The pre-filled headroom lets the workload burst immediately
	 *                 without having to earn any credits first.
	 *
	 *   EARNED_BURST: capacity = income_per_refill + transfer_per_refill;
	 *                 starts empty. Each tick receives income_per_refill new
	 *                 tokens; additional tokens are transferred from the burst
	 *                 bucket (up to transfer_per_refill) to deploy saved credits.
	 *
	 * Because multiple I/O threads subtract tokens concurrently, its tokens
	 * field is accessed with atomics.
	 */
	struct token_bucket steady_bucket;

	/*
	 * Burst bucket is the "savings account" used exclusively in EARNED_BURST
	 * mode (capacity = 0 in STRICT and BURST_READY, so it is never touched).
	 *
	 * It fills when the workload is idle: income added to a full steady bucket
	 * overflows into the burst bucket (up to burst_bucket->capacity).
	 *
	 * It drains when the workload is busy: each tick, up to transfer_per_refill
	 * tokens are moved from burst bucket → steady bucket, deploying the saved
	 * credits to sustain a burst above avg_rate.
	 *
	 * The burst bucket is read and written only by the single app-thread refill
	 * poller, so no atomic operations are required.
	 */
	struct token_bucket burst_bucket;

	/* Ticks at last refill was done. */
	uint64_t last_refill_ticks;

	/* Refill period in ticks */
	uint64_t refill_period_ticks;

	/* The number of new tokens generated on each refill, based on avg_rate. */
	uint64_t income_per_refill;

	/* The maximum rate at which saved tokens can be moved from burst_bucket
	 * to steady bucket.
	 */
	uint64_t transfer_per_refill;

	/* The metric this token bucket applies to. */
	enum bdev_qos_metric metric;

	/* The operating mode this token bucket uses. */
	enum bdev_qos_mode mode;

	/* Number of local buckets for this global bucket. */
	uint32_t num_local_buckets;

	/* Local buckets for this global bucket. */
	TAILQ_HEAD(, local_token_bucket) local_buckets;

	/* Spinlock to protect local bucket list and count. */
	struct spdk_spinlock spinlock;

	/* The long-term average rate set by user. */
	uint64_t avg_rate;

	/* The duration in seconds for which the burst rate is maintained. */
	uint64_t max_burst_time_in_sec;

	/* Max I/O allowed in a single burst */
	uint64_t io_burst;

	/* Minimum batch size to withdraw for this bucket. */
	uint64_t min_withdraw_batch_size;

	/* Maximum batch size to withdraw for this bucket. */
	uint64_t max_withdraw_batch_size;

	/* Step size to increase the withdraw batch on success. */
	uint64_t additive_increase_step;
};

/*
 * Note: The capacity of the local token bucket is unlimited. When a local bucket
 * needs tokens for a large I/O, if it does not have enough, it repeatedly withdraws
 * from the global bucket until it accumulates enough tokens for the I/O.
 * Hence, there is no unnecessary I/O rejection or split due to rate limiter.
 */
struct local_token_bucket {
	/* Number of tokens currently cached locally. Ready for immediate,
	 * lock-free spending.
	 */
	uint64_t tokens;

	/* The current size of the batch this local bucket will try to withdraw.
	 * This value is dynamically adjusted by the Additive Increase Multiplicative Decrease
	 * (AIMD) algorithm.
	 */
	uint64_t withdraw_batch_size;

	/* The threshold for sending guaranteed tokens back to the shared global bucket.
	 * This limits how many tokens an idle or low-load local bucket can buffer.
	 * This limit is strictly enforced when the I/O queue is empty, but effectively
	 * ignored when I/Os are queued to allow accumulation for requests larger than
	 * the bucket size.
	 */
	uint64_t capacity;

	/* Tokens allocated by the refill operation, waiting to be claimed. */
	uint64_t refill_grant;

	/* List to queue the I/Os blocked to this bucket. */
	bdev_io_tailq_t queued_io;

	/* Function to attempt consuming tokens for the I/O. */
	bool (*consume)(struct local_token_bucket *local_bucket, struct spdk_bdev_io *bdev_io);

	/* Pointer to the global token bucket. */
	struct global_token_bucket *global_bucket;

	STAILQ_ENTRY(local_token_bucket) slink;
	TAILQ_ENTRY(local_token_bucket) link;
};

struct bdev_burst_qos {
	struct spdk_bdev_qos_impl base;

	struct global_token_bucket global_buckets[BDEV_QOS_NUM_METRICS];

	TAILQ_ENTRY(bdev_burst_qos) link;
};

struct bdev_burst_qos_poll_group;

struct bdev_burst_qos_channel {
	struct spdk_bdev_qos_channel_impl base;

	struct local_token_bucket local_buckets[BDEV_QOS_NUM_METRICS];
	STAILQ_HEAD(, local_token_bucket) local_bucket_list;

	struct bdev_burst_qos *bqos;

	struct bdev_burst_qos_poll_group *bgroup;

	TAILQ_ENTRY(bdev_burst_qos_channel) link;
};

struct bdev_burst_qos_mgr {
	uint64_t last_tick_tsc;
	uint64_t tick_period_tsc;
	uint64_t ticks;

	struct spdk_poller *poller;

	TAILQ_HEAD(, bdev_burst_qos) bqos_list;
};

struct bdev_burst_qos_poll_group {
	TAILQ_HEAD(, bdev_burst_qos_channel) bqos_ch_list;
	struct spdk_poller *retry_poller;
	/* Continuation cursor */
	struct bdev_burst_qos_channel *next_ch;
};

static struct spdk_bdev_qos_module bdev_burst_qos_if;

static struct bdev_burst_qos_mgr g_qos_mgr = {
	.last_tick_tsc = 0,
	.tick_period_tsc = 0,
	.ticks = 0,
	.poller = NULL,
	.bqos_list = TAILQ_HEAD_INITIALIZER(g_qos_mgr.bqos_list),
};

static struct bdev_burst_qos_opts g_qos_opts = {
	.tick_period_us = BDEV_QOS_TICK_PERIOD_US,
	.max_io_withdraw_batch_size = BDEV_QOS_MAX_IO_WITHDRAW_BATCH_SIZE,
	.io_additive_increase_step = BDEV_QOS_IO_ADDITIVE_INCREASE_STEP,
	.max_byte_withdraw_batch_size = BDEV_QOS_MAX_BYTE_WITHDRAW_BATCH_SIZE,
	.byte_additive_increase_step = BDEV_QOS_BYTE_ADDITIVE_INCREASE_STEP,
	.retry_budget = BDEV_QOS_DEFAULT_RETRY_BUDGET,
};

static inline struct bdev_burst_qos *
bdev_burst_qos(struct spdk_bdev_qos_impl *qos_impl)
{
	return SPDK_CONTAINEROF(qos_impl, struct bdev_burst_qos, base);
}

static inline struct bdev_burst_qos_channel *
bdev_burst_qos_channel(struct spdk_bdev_qos_channel_impl *qos_ch_impl)
{
	return SPDK_CONTAINEROF(qos_ch_impl, struct bdev_burst_qos_channel, base);
}

static const char *
bdev_qos_metric_str(enum bdev_qos_metric metric)
{
	switch (metric) {
	case BDEV_QOS_RW_IOPS:
		return "rw_iops";
	case BDEV_QOS_RW_BPS:
		return "rw_mbps";
	case BDEV_QOS_R_BPS:
		return "r_mbps";
	case BDEV_QOS_W_BPS:
		return "w_mbps";
	default:
		return NULL;
	}
}

static const char *
bdev_qos_mode_str(enum bdev_qos_mode mode)
{
	switch (mode) {
	case BDEV_QOS_MODE_STRICT:
		return "strict";
	case BDEV_QOS_MODE_BURST_READY:
		return "burst_ready";
	case BDEV_QOS_MODE_EARNED_BURST:
		return "earned_burst";
	default:
		return NULL;
	}
}

static inline bool
bdev_qos_metric_is_iops(enum bdev_qos_metric metric)
{
	assert(metric >= 0 && metric < BDEV_QOS_NUM_METRICS);

	switch (metric) {
	case BDEV_QOS_RW_IOPS:
		return true;
	case BDEV_QOS_RW_BPS:
	case BDEV_QOS_R_BPS:
	case BDEV_QOS_W_BPS:
		return false;
	case BDEV_QOS_NUM_METRICS:
	default:
		return false;
	}
}

static inline uint64_t
atomic_add_capped(uint64_t *value, uint64_t to_add, uint64_t cap)
{
	uint64_t current, new, potential, overflow;

	current = __atomic_load_n(value, __ATOMIC_RELAXED);
	while (true) {
		potential = current + to_add;
		if (potential > cap) {
			new = cap;
			overflow = potential - cap;
		} else {
			new = potential;
			overflow = 0;
		}
		if (__atomic_compare_exchange_n(value, &current, new, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			return overflow;
		}
		spdk_pause();
	}
}

static inline uint64_t
atomic_sub_floor(uint64_t *value, uint64_t to_sub)
{
	uint64_t current, new, actual_sub;

	current = __atomic_load_n(value, __ATOMIC_RELAXED);
	while (true) {
		if (current == 0) {
			/* There is nothing to withdraw. */
			return 0;
		}
		actual_sub = spdk_min(current, to_sub);
		new = current - actual_sub;
		if (__atomic_compare_exchange_n(value, &current, new, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			return actual_sub;
		}
		spdk_pause();
	}
}

/*
 * Calculate the next closest divisor >= n for a given dividend.
 * If n is already a divisor, it returns n.
 */
static inline uint64_t
calculate_next_divisor(uint64_t dividend, uint64_t n)
{
	uint64_t i;

	/* Handle the edge case where the dividend is 0. */
	if (dividend == 0) {
		return 0;
	}

	/* Start searching from n upwards. The loop continues as long as the potential
	 * divisor 'i' is less than or equal to the dividend.
	 */
	for (i = n; i <= dividend; i++) {
		if ((dividend % i) == 0) {
			return i;
		}
	}

	/* If no divisor is found in the range [n, dividend], the only remaining divisor is
	 * the dividend itself. The loop condition i <= dividend ensures we check the
	 * dividend itself.
	 *
	 * If we reach here, it implies n > dividend initially. The most logical return in
	 * this scenario is probably the dividend itself.
	 */
	return dividend;
}

void
bdev_burst_qos_get_opts(struct bdev_burst_qos_opts *opts, size_t opts_size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL.\n");
		return;
	}

	if (opts_size == 0) {
		SPDK_ERRLOG("opts_size should not be zero value.\n");
		return;
	}

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
	if (offsetof(struct bdev_burst_qos_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_qos_opts.field; \
	} \

	SET_FIELD(tick_period_us);
	SET_FIELD(max_io_withdraw_batch_size);
	SET_FIELD(io_additive_increase_step);
	SET_FIELD(max_byte_withdraw_batch_size);
	SET_FIELD(byte_additive_increase_step);
	SET_FIELD(retry_budget);

	/* Do not remove this statement, you should always update this statement when you adding
	 * a new field, and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct bdev_burst_qos_opts) == 56, "Incorrect size");

#undef SET_FIELD
}

int
bdev_burst_qos_set_opts(const struct bdev_burst_qos_opts *opts)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts cannot be NULL.\n");
		return -EINVAL;
	}

	if (opts->opts_size == 0) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value.\n");
		return -EINVAL;
	}

	if (opts->tick_period_us == 0 || opts->tick_period_us > SPDK_SEC_TO_USEC) {
		SPDK_ERRLOG("tick_period must not be 0 or larger than 1 second.\n");
		return -EINVAL;
	}

	if (opts->max_io_withdraw_batch_size < BDEV_QOS_MIN_IO_WITHDRAW_BATCH_SIZE) {
		SPDK_ERRLOG("max_io_withdraw_batch_size was %" PRIu64 " but must be not less than %d.\n",
			    opts->max_io_withdraw_batch_size, BDEV_QOS_MIN_IO_WITHDRAW_BATCH_SIZE);
		return -EINVAL;
	}

	if (opts->io_additive_increase_step == 0 ||
	    opts->io_additive_increase_step > opts->max_io_withdraw_batch_size) {
		SPDK_ERRLOG("io_additive_increase_step was %" PRIu64 " but must be non-zero and"
			    " not larger than max_io_withdraw_batch_size %" PRIu64 "\n",
			    opts->io_additive_increase_step,
			    opts->max_io_withdraw_batch_size);
		return -EINVAL;
	}

	if (opts->max_byte_withdraw_batch_size < BDEV_QOS_MIN_BYTE_WITHDRAW_BATCH_SIZE) {
		SPDK_ERRLOG("max_byte_withdraw_batch_size was %" PRIu64 " but must be not less than %d.\n",
			    opts->max_byte_withdraw_batch_size, BDEV_QOS_MIN_BYTE_WITHDRAW_BATCH_SIZE);
		return -EINVAL;
	}

	if (opts->byte_additive_increase_step == 0 ||
	    opts->byte_additive_increase_step > opts->max_byte_withdraw_batch_size) {
		SPDK_ERRLOG("byte_additive_increase_step was %" PRIu64 " but must be non-zero and"
			    " not larger than max_byte_withdraw_batch_size %" PRIu64 "\n",
			    opts->byte_additive_increase_step,
			    opts->max_byte_withdraw_batch_size);
		return -EINVAL;
	}

	if (opts->retry_budget > UINT32_MAX) {
		SPDK_ERRLOG("retry_budget %" PRIu64 " exceeds maximum supported value %u.\n",
			    opts->retry_budget, UINT32_MAX);
		return -EINVAL;
	}

#define SET_FIELD(field) \
        if (offsetof(struct bdev_burst_qos_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_qos_opts.field = opts->field; \
        } \

	SET_FIELD(tick_period_us);
	SET_FIELD(max_io_withdraw_batch_size);
	SET_FIELD(io_additive_increase_step);
	SET_FIELD(max_byte_withdraw_batch_size);
	SET_FIELD(byte_additive_increase_step);
	SET_FIELD(retry_budget);

	g_qos_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	/* A retry_budget of 0 selects the default. Resolve it here because opts may
	 * be smaller than this field.
	 */
	if (g_qos_opts.retry_budget == 0) {
		g_qos_opts.retry_budget = BDEV_QOS_DEFAULT_RETRY_BUDGET;
	}

	return 0;
}

/*
 * Attempt withdrawing tokens from the global bucket.
 *
 * \param global_bucket Global token bucket
 * \param tokens_to_withdraw Amount of tokens to withdraw.
 *
 * \return amount of tokens actually withdrawn.
 */
static inline uint64_t
global_token_bucket_withdraw(struct global_token_bucket *global_bucket,
			     uint64_t tokens_to_withdraw)
{
	struct token_bucket *steady_bucket = &global_bucket->steady_bucket;

	if (global_bucket->avg_rate == UINT64_MAX) {
		/* The bucket is disabled. */
		return tokens_to_withdraw;
	}

	return atomic_sub_floor(&steady_bucket->tokens, tokens_to_withdraw);
}

static inline void
_global_token_bucket_refill(struct global_token_bucket *global_bucket,
			    uint64_t tokens_to_refill)
{
	struct token_bucket *steady_bucket = &global_bucket->steady_bucket;
	struct token_bucket *burst_bucket = &global_bucket->burst_bucket;
	uint64_t overflow, tokens_to_transfer, transfer;

	/* Add income to steady bucket first. */
	overflow = atomic_add_capped(&steady_bucket->tokens, tokens_to_refill,
				     steady_bucket->capacity);

	if (overflow > 0 && burst_bucket->capacity != 0) {
		/* Handle overflow into burst bucket. */
		burst_bucket->tokens = spdk_min(burst_bucket->tokens + overflow,
						burst_bucket->capacity);

	} else if (burst_bucket->tokens > 0) {
		/* Eagerly transfer from burst bucket to steady bucket. */
		tokens_to_transfer = spdk_min(burst_bucket->tokens,
					      global_bucket->transfer_per_refill);

		overflow = atomic_add_capped(&steady_bucket->tokens, tokens_to_transfer,
					     steady_bucket->capacity);

		transfer = tokens_to_transfer - overflow;
		if (transfer > 0) {
			burst_bucket->tokens -= transfer;
		}
	}
}

static uint64_t local_token_bucket_refill(struct local_token_bucket *local_bucket,
		uint64_t tokens_to_refill);

/*
 * To ensure the average rate is maintained accurately even if the periodic timer is
 * delayed or missed, the refill function calculates elapsed_ticks by itself.
 * This allows it to calculate and the correct amount of tokens to compensate for
 * the time that has passed since the last call.
 *
 * \param global_bucket Global token bucket.
 */
static inline void
global_token_bucket_refill(struct global_token_bucket *global_bucket)
{
	uint64_t elapsed_ticks, num_refills;
	uint64_t tokens_to_refill, fair_share, to_global;
	struct local_token_bucket *local_bucket;

	assert(spdk_thread_is_app_thread(NULL));

	if (global_bucket->avg_rate == UINT64_MAX) {
		/* The bucket is disabled. */
		return;
	}

	assert(g_qos_mgr.ticks >= global_bucket->last_refill_ticks);
	elapsed_ticks = g_qos_mgr.ticks - global_bucket->last_refill_ticks;

	/* Check if the mandatory refill period passed. */
	if (elapsed_ticks < global_bucket->refill_period_ticks) {
		return;
	}

	num_refills = elapsed_ticks / global_bucket->refill_period_ticks;

	global_bucket->last_refill_ticks += num_refills * global_bucket->refill_period_ticks;

	tokens_to_refill = global_bucket->income_per_refill * num_refills;

	spdk_spin_lock(&global_bucket->spinlock);
	if (global_bucket->num_local_buckets == 0) {
		to_global = tokens_to_refill;
	} else {
		fair_share = tokens_to_refill / global_bucket->num_local_buckets;
		to_global = tokens_to_refill % global_bucket->num_local_buckets;

		TAILQ_FOREACH(local_bucket, &global_bucket->local_buckets, link) {
			to_global += local_token_bucket_refill(local_bucket, fair_share);
		}
	}
	spdk_spin_unlock(&global_bucket->spinlock);

	if (to_global > 0 || global_bucket->burst_bucket.tokens > 0) {
		/* Excess tokens are back to the global pool to maintain work-conserving
		 * behavior. Also allow burst credits to transfer even when there is no
		 * excess refill to the global pool.
		 */
		_global_token_bucket_refill(global_bucket, to_global);
	}
}

static void
token_bucket_reset(struct token_bucket *bucket)
{
	bucket->tokens = 0;
	bucket->capacity = 0;
}

static void
global_token_bucket_reset(struct global_token_bucket *global_bucket)
{
	token_bucket_reset(&global_bucket->burst_bucket);
	token_bucket_reset(&global_bucket->steady_bucket);
	global_bucket->mode = BDEV_QOS_MODE_STRICT;
	global_bucket->avg_rate = UINT64_MAX;
	global_bucket->last_refill_ticks = 0;
	global_bucket->income_per_refill = 0;
	global_bucket->transfer_per_refill = 0;
	global_bucket->refill_period_ticks = 0;
	global_bucket->max_burst_time_in_sec = 0;
	global_bucket->io_burst = 0;
	/* min_withdraw_batch_size is fixed by metric. */
	global_bucket->max_withdraw_batch_size = 0;
	global_bucket->additive_increase_step = 0;
}

static void
global_token_bucket_init(struct global_token_bucket *global_bucket,
			 enum bdev_qos_metric metric)
{
	global_bucket->metric = metric;
	if (bdev_qos_metric_is_iops(metric)) {
		global_bucket->min_withdraw_batch_size = BDEV_QOS_MIN_IO_WITHDRAW_BATCH_SIZE;
	} else {
		global_bucket->min_withdraw_batch_size = BDEV_QOS_MIN_BYTE_WITHDRAW_BATCH_SIZE;
	}
	TAILQ_INIT(&global_bucket->local_buckets);
	global_bucket->num_local_buckets = 0;
	spdk_spin_init(&global_bucket->spinlock);
	global_token_bucket_reset(global_bucket);
}

static void
global_token_bucket_fini(struct global_token_bucket *global_bucket)
{
	spdk_spin_destroy(&global_bucket->spinlock);
}

static void
global_token_bucket_add_local_bucket(struct global_token_bucket *global_bucket,
				     struct local_token_bucket *local_bucket)
{
	spdk_spin_lock(&global_bucket->spinlock);
	TAILQ_INSERT_TAIL(&global_bucket->local_buckets, local_bucket, link);
	global_bucket->num_local_buckets++;
	spdk_spin_unlock(&global_bucket->spinlock);
}

static void
global_token_bucket_remove_local_bucket(struct global_token_bucket *global_bucket,
					struct local_token_bucket *local_bucket)
{
	spdk_spin_lock(&global_bucket->spinlock);
	TAILQ_REMOVE(&global_bucket->local_buckets, local_bucket, link);
	assert(global_bucket->num_local_buckets > 0);
	global_bucket->num_local_buckets--;
	spdk_spin_unlock(&global_bucket->spinlock);
}

/*
 * Configure the global token bucket based on the passed average rate.
 * Particularly, calculate and set the number of tokens to add on each refill tick, and
 * the max_burst_rate is the maximum rate the system can temporarily achieve.
 * This peak rate is only possible by consuming the "burst credits" that have been
 * saved up when the system's throughput was below the avg_rate.
 *
 * \param global_bucket Global token bucket.
 * \param avg_rate The long-term sustained throughput.
 * \param mode Operating mode.
 * \param burst_size The burst limit (Relevant only for burst_ready mode).
 * \param max_burst_rate Peak rate allowed during a burst (Relevant only for earned_burst mode).
 * \param max_burst_time_in_sec The maximum duration max_burst_rate can be sustained (Relevant
 *                              only for earned_burst mode).
 * \param refill_period_us Refill period in microseconds.
 * \param io_burst Max I/O allowed in a single burst.
 */
static int
global_token_bucket_set(struct global_token_bucket *global_bucket, uint64_t avg_rate,
			enum bdev_qos_mode qos_mode, uint64_t burst_size,
			uint64_t max_burst_rate, uint64_t max_burst_time_in_sec,
			uint64_t refill_period_us, uint64_t io_burst,
			uint64_t max_withdraw_batch_size,
			uint64_t additive_increase_step)
{
	struct token_bucket *steady_bucket = &global_bucket->steady_bucket;
	struct token_bucket *burst_bucket = &global_bucket->burst_bucket;
	uint64_t min_rate, complement, ticks_per_sec, batch_demand, refills_per_sec;

	switch (qos_mode) {
	case BDEV_QOS_MODE_STRICT:
	case BDEV_QOS_MODE_BURST_READY:
	case BDEV_QOS_MODE_EARNED_BURST:
		break;
	default:
		return -EINVAL;
	}

	if (avg_rate == 0 || avg_rate == UINT64_MAX) {
		global_token_bucket_reset(global_bucket);
		return 0;
	}

	if (io_burst == 0) {
		return -EINVAL;
	}

	global_bucket->mode = qos_mode;

	if (bdev_qos_metric_is_iops(global_bucket->metric)) {
		min_rate = BDEV_QOS_MIN_IO_PER_SEC;
	} else {
		min_rate = BDEV_QOS_MIN_BYTE_PER_SEC;
		avg_rate *= 1024 * 1024;
		max_burst_rate *= 1024 * 1024;
	}

	if (max_withdraw_batch_size < global_bucket->min_withdraw_batch_size) {
		SPDK_ERRLOG("max_withdraw_batch_size was %" PRIu64 " but must be not less than %" PRIu64 ".\n",
			    max_withdraw_batch_size, global_bucket->min_withdraw_batch_size);
		return -EINVAL;
	}

	if (additive_increase_step == 0 || additive_increase_step > max_withdraw_batch_size) {
		SPDK_ERRLOG("additive_increase_step was %" PRIu64 " but must be non-zero and"
			    " not larger than max_withdraw_batch_size %" PRIu64 ".\n",
			    additive_increase_step, max_withdraw_batch_size);
		return -EINVAL;
	}

	complement = avg_rate % min_rate;
	if (complement != 0) {
		SPDK_WARNLOG("Requested avg_rate %" PRIu64 " is not multiple of %" PRIu64 "\n",
			     avg_rate, min_rate);
		avg_rate += min_rate - complement;
		SPDK_WARNLOG("Round up it to %" PRIu64 "\n", avg_rate);
	}

	global_bucket->last_refill_ticks = g_qos_mgr.ticks;

	ticks_per_sec = SPDK_SEC_TO_USEC / g_qos_opts.tick_period_us;

	global_bucket->avg_rate = avg_rate;

	global_bucket->io_burst = io_burst;
	global_bucket->max_withdraw_batch_size = max_withdraw_batch_size;
	global_bucket->additive_increase_step = additive_increase_step;

	/* Sanitize user input. */
	if (refill_period_us == 0) {
		refill_period_us = g_qos_opts.tick_period_us;
	}

	/* Align to the tick resolution. */
	refill_period_us = spdk_round_up(refill_period_us, g_qos_opts.tick_period_us);

	/* Calculate user's desired frequency. */
	refills_per_sec = SPDK_SEC_TO_USEC / refill_period_us;

	/* Calculate total batch demand. We want to refill enough to satisfy one
	 * batched consumption for every local bucket per period.
	 */
	batch_demand = io_burst * spdk_env_get_core_count();

	/* Align batch demand safely. Find a divisor of avg_rate which is >= batch demand.
	 * This ensures limit / batch_demand results in an integer max frequency. */
	batch_demand = calculate_next_divisor(avg_rate, batch_demand);

	/* The prioritize but protect logic. Use user's frequency unless it exceeds the safe max. */
	refills_per_sec = spdk_min(refills_per_sec, avg_rate / batch_demand);

	/* Now we derive the final income and period. Since refills_per_sec is derived from a
	 * divisor, this division is usually clean.
	 */
	global_bucket->income_per_refill = avg_rate / refills_per_sec;

	/* Recalculate ticks from the finalized frequency. */
	global_bucket->refill_period_ticks = ticks_per_sec / refills_per_sec;

	burst_bucket->capacity = 0;
	global_bucket->max_burst_time_in_sec = 0;
	global_bucket->transfer_per_refill = 0;

	if (qos_mode == BDEV_QOS_MODE_STRICT) {
		steady_bucket->capacity = global_bucket->income_per_refill;

		__atomic_store_n(&steady_bucket->tokens, 0, __ATOMIC_SEQ_CST);
		burst_bucket->tokens = 0;
		return 0;

	} else if (qos_mode == BDEV_QOS_MODE_BURST_READY) {
		if (burst_size == 0) {
			burst_size = avg_rate;
		}
		steady_bucket->capacity = burst_size;

		__atomic_store_n(&steady_bucket->tokens, steady_bucket->capacity, __ATOMIC_SEQ_CST);
		burst_bucket->tokens = 0; /* burst_bucket is unused in BURST_READY mode */
	} else {
		assert(qos_mode == BDEV_QOS_MODE_EARNED_BURST);
		if (max_burst_rate == 0) {
			max_burst_rate = avg_rate;
			SPDK_WARNLOG("EARNED_BURST: max_burst_rate unset, defaulting to avg_rate — "
				     "no burst credits will accumulate. Set max_burst_rate > avg_rate "
				     "to enable bursting.\n");
		} else if (max_burst_rate < avg_rate) {
			return -EINVAL;
		}

		if (max_burst_time_in_sec == 0) {
			max_burst_time_in_sec = 1;
		}

		global_bucket->max_burst_time_in_sec = max_burst_time_in_sec;
		global_bucket->transfer_per_refill = (max_burst_rate - avg_rate) / refills_per_sec;

		/* Steady bucket must be large enough to handle the peak rate. */
		steady_bucket->capacity = global_bucket->income_per_refill +
					  global_bucket->transfer_per_refill;

		/* Burst bucket holds the savings for the burst duration beyond
		 * the first second.
		 */
		burst_bucket->capacity = (max_burst_rate - avg_rate) * max_burst_time_in_sec;

		__atomic_store_n(&steady_bucket->tokens, 0, __ATOMIC_SEQ_CST);
		burst_bucket->tokens = 0;
	}

	return 0;
}

static void
global_token_bucket_get_limit(struct global_token_bucket *global_bucket, uint64_t *avg_rate,
			      uint64_t *burst_size, uint64_t *max_burst_rate)
{
	if (global_bucket->avg_rate == UINT64_MAX) {
		*avg_rate = 0;
		*burst_size = 0;
		*max_burst_rate = 0;
	} else {
		*avg_rate = global_bucket->avg_rate;
		if (!bdev_qos_metric_is_iops(global_bucket->metric)) {
			/* Change from byte to mebibyte which is user visible. */
			*avg_rate = *avg_rate / 1024 / 1024;
		}

		if (global_bucket->mode == BDEV_QOS_MODE_BURST_READY) {
			*burst_size = global_bucket->steady_bucket.capacity;
			if (!bdev_qos_metric_is_iops(global_bucket->metric)) {
				*burst_size = *burst_size / 1024 / 1024;
			}
		} else {
			*burst_size = 0;
		}

		if (global_bucket->mode == BDEV_QOS_MODE_EARNED_BURST) {
			*max_burst_rate = global_bucket->avg_rate +
					  global_bucket->burst_bucket.capacity / global_bucket->max_burst_time_in_sec;
			if (!bdev_qos_metric_is_iops(global_bucket->metric)) {
				*max_burst_rate = *max_burst_rate / 1024 / 1024;
			}
		} else {
			*max_burst_rate = 0;
		}
	}
}

static void
global_token_bucket_config_json(struct global_token_bucket *global_bucket, const char *name,
				struct spdk_json_write_ctx *w)
{
	uint64_t avg_rate = 0, burst_size = 0, max_burst_rate = 0;

	global_token_bucket_get_limit(global_bucket, &avg_rate, &burst_size, &max_burst_rate);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_burst_qos_set_limit");

	spdk_json_write_named_object_begin(w, "params");

	spdk_json_write_named_string(w, "name", name);
	spdk_json_write_named_string(w, "qos_metric", bdev_qos_metric_str(global_bucket->metric));
	spdk_json_write_named_uint64(w, "avg_rate", avg_rate);
	spdk_json_write_named_string(w, "qos_mode", bdev_qos_mode_str(global_bucket->mode));
	switch (global_bucket->mode) {
	case BDEV_QOS_MODE_BURST_READY:
		spdk_json_write_named_uint64(w, "burst_size", burst_size);
		break;
	case BDEV_QOS_MODE_EARNED_BURST:
		spdk_json_write_named_uint64(w, "max_burst_rate", max_burst_rate);
		spdk_json_write_named_uint64(w, "max_burst_time_in_sec", global_bucket->max_burst_time_in_sec);
		break;
	default:
		break;
	}
	spdk_json_write_named_uint64(w, "refill_period_us",
				     g_qos_opts.tick_period_us * global_bucket->refill_period_ticks);
	spdk_json_write_named_uint64(w, "io_burst", global_bucket->io_burst);
	spdk_json_write_named_uint64(w, "max_withdraw_batch_size",
				     global_bucket->max_withdraw_batch_size);
	spdk_json_write_named_uint64(w, "additive_increase_step",
				     global_bucket->additive_increase_step);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
global_token_bucket_info_json(struct global_token_bucket *global_bucket,
			      struct spdk_json_write_ctx *w)
{
	uint64_t avg_rate = 0, burst_size = 0, max_burst_rate = 0;

	global_token_bucket_get_limit(global_bucket, &avg_rate, &burst_size, &max_burst_rate);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "qos_metric", bdev_qos_metric_str(global_bucket->metric));
	spdk_json_write_named_uint64(w, "avg_rate", avg_rate);
	spdk_json_write_named_string(w, "qos_mode", bdev_qos_mode_str(global_bucket->mode));
	switch (global_bucket->mode) {
	case BDEV_QOS_MODE_BURST_READY:
		spdk_json_write_named_uint64(w, "burst_size", burst_size);
		break;
	case BDEV_QOS_MODE_EARNED_BURST:
		spdk_json_write_named_uint64(w, "max_burst_rate", max_burst_rate);
		spdk_json_write_named_uint64(w, "max_burst_time_in_sec", global_bucket->max_burst_time_in_sec);
		break;
	default:
		break;
	}
	spdk_json_write_named_uint64(w, "refill_period_us",
				     g_qos_opts.tick_period_us * global_bucket->refill_period_ticks);
	spdk_json_write_named_uint64(w, "io_burst", global_bucket->io_burst);

	spdk_json_write_object_end(w);
}

static uint64_t
local_token_bucket_refill(struct local_token_bucket *local_bucket, uint64_t tokens_to_refill)
{
	uint64_t effective_cap;

	/* Limit to prevent hoarding during idle periods. */
	effective_cap = __atomic_load_n(&local_bucket->capacity, __ATOMIC_RELAXED);

	if (!TAILQ_EMPTY(&local_bucket->queued_io)) {
		/* Remove the capacity effectively to allow a starved local bucket to
		 * accumulate its guaranteed share over multiple ticks to satisfy large
		 * I/O size.
		 */
		effective_cap = UINT64_MAX;
	}

	return atomic_add_capped(&local_bucket->refill_grant, tokens_to_refill, effective_cap);
}

/*
 * The core function to consume tokens, implementing Additive Increase Multiplicative Decrease
 * (AIMD) algorithm to withdraw tokens from the global bucket.
 *
 * \param local_bucket Local token bucket
 * \param tokens_needed The number of tokens required for the operation.
 *
 * \return true if consumption was successful, or false otherwise.
 */
static inline bool
_local_token_bucket_consume(struct local_token_bucket *local_bucket, uint64_t tokens_needed)
{
	struct global_token_bucket *global_bucket = local_bucket->global_bucket;
	int64_t shortfall;
	uint64_t additive_increase_step, max_withdraw_batch_size, min_withdraw_batch_size;
	uint64_t claimed, actual_withdrawn;

	if (global_bucket->avg_rate == UINT64_MAX) {
		/* The bucket is disabled */
		return true;
	}

	/* Try the local bucket first without atomic operation. */
	if (local_bucket->tokens >= tokens_needed) {
		local_bucket->tokens -= tokens_needed;
		return true;
	}

	additive_increase_step = global_bucket->additive_increase_step;
	max_withdraw_batch_size = global_bucket->max_withdraw_batch_size;
	min_withdraw_batch_size = global_bucket->min_withdraw_batch_size;

	/* Move from 'guaranteed' mailbox to 'tokens' wallet. */
	claimed = atomic_sub_floor(&local_bucket->refill_grant, local_bucket->withdraw_batch_size);
	if (claimed > 0) {
		/* Transfer tokens. */
		local_bucket->tokens += claimed;

		if (claimed == local_bucket->withdraw_batch_size) {
			/* Full claim: Apply Additive Increase. */
			local_bucket->withdraw_batch_size += additive_increase_step;
			if (local_bucket->withdraw_batch_size > max_withdraw_batch_size) {
				local_bucket->withdraw_batch_size = max_withdraw_batch_size;
			}

			if (local_bucket->tokens >= tokens_needed) {
				local_bucket->tokens -= tokens_needed;
				return true;
			}
		}
	}

	/* Not enough tokens. Calculate the shortfall and start taking from
	 * global bucket. */
	shortfall = tokens_needed - local_bucket->tokens;

	/* We will accumulate the needed tokens locally before spending. */
	while (shortfall > 0) {
		actual_withdrawn = global_token_bucket_withdraw(global_bucket, local_bucket->withdraw_batch_size);
		if (actual_withdrawn > 0) {
			/* Transfer tokens. */
			local_bucket->tokens += actual_withdrawn;
			/* Reduce shortfall. */
			shortfall -= actual_withdrawn;

			if (actual_withdrawn == local_bucket->withdraw_batch_size) {
				/* Full withdrawal: Apply Additive Increase. */
				local_bucket->withdraw_batch_size += additive_increase_step;
				if (local_bucket->withdraw_batch_size > max_withdraw_batch_size) {
					local_bucket->withdraw_batch_size = max_withdraw_batch_size;
				}
			} else {
				/* Partial withdrawal: Keep the current batch size.
				 * Global bucket is under pressure but we could still make progress.
				 */
			}
		} else {
			/* Failed withdrawal: Apply Multiplicative Decrease. */
			local_bucket->withdraw_batch_size >>= 1;
			if (local_bucket->withdraw_batch_size < min_withdraw_batch_size) {
				local_bucket->withdraw_batch_size = min_withdraw_batch_size;
			}

			/* Give up immediately to give time for the global bucket to refill and
			 * give an opportunity for other less aggressive local buckets.
			 */
			return false;
		}
	}

	/* If we exit the loop, we have successfully withdrawed enough tokens. */

	/* Now, finally consume the original amount. */
	local_bucket->tokens -= tokens_needed;
	return true;
}

/*
 * Consume tokens from the RW IOPS token bucket.
 */
static bool
local_token_bucket_rw_iops_consume(struct local_token_bucket *local_bucket,
				   struct spdk_bdev_io *bdev_io)
{
	return _local_token_bucket_consume(local_bucket, 1);
}

/*
 * Consume tokens from the RW BPS token bucket.
 */
static bool
local_token_bucket_rw_bps_consume(struct local_token_bucket *local_bucket,
				  struct spdk_bdev_io *bdev_io)
{
	return _local_token_bucket_consume(local_bucket,
					   spdk_bdev_io_get_io_size_in_bytes(bdev_io));
}

/*
 * Consume tokens from the R BPS token bucket.
 */
static bool
local_token_bucket_r_bps_consume(struct local_token_bucket *local_bucket,
				 struct spdk_bdev_io *bdev_io)
{
	if (!spdk_bdev_io_is_read_io(bdev_io)) {
		return true;
	}

	return _local_token_bucket_consume(local_bucket,
					   spdk_bdev_io_get_io_size_in_bytes(bdev_io));
}

/*
 * Consume tokens from the W BPS token bucket.
 */
static bool
local_token_bucket_w_bps_consume(struct local_token_bucket *local_bucket,
				 struct spdk_bdev_io *bdev_io)
{
	if (spdk_bdev_io_is_read_io(bdev_io)) {
		return true;
	}

	return _local_token_bucket_consume(local_bucket,
					   spdk_bdev_io_get_io_size_in_bytes(bdev_io));
}

static inline void local_token_bucket_queue_io(struct local_token_bucket *_local_bucket,
		struct spdk_bdev_io *bdev_io);

/*
 * This function is called when the I/O passed rate limit check of the bucket.
 * Move to the next QoS metric's bucket if exist, or call spdk_bdev_qos_channel_impl_queue_io_done()
 * if all passed.
 */
static inline void
local_token_bucket_queue_io_done(struct local_token_bucket *_local_bucket,
				 struct spdk_bdev_io *bdev_io)
{
	struct local_token_bucket *local_bucket = STAILQ_NEXT(_local_bucket, slink);

	if (local_bucket != NULL) {
		/* Do the rate limit check for the next QoS metric if exist. */
		local_token_bucket_queue_io(local_bucket, bdev_io);
	} else {
		/* Rate limit check passed for all QoS metrics. */
		spdk_bdev_qos_channel_impl_queue_io_done(bdev_io);
	}
}

/*
 * If the queue is empty and the I/O could consume tokens, the rate limiter allows the I/O.
 * Otherwise, the rate limiter places the I/O to the tail of the queue, ensuring FIFO ordering.
 */
static inline void
local_token_bucket_queue_io(struct local_token_bucket *local_bucket,
			    struct spdk_bdev_io *bdev_io)
{
	if (!TAILQ_EMPTY(&local_bucket->queued_io)) {
		/* To ensure FIFO locally, append this I/O to the list. */
		TAILQ_INSERT_TAIL(&local_bucket->queued_io, bdev_io, internal.link);
		return;
	}

	if (local_bucket->consume(local_bucket, bdev_io)) {
		/* Rate limit check passed for this QoS metric. */
		local_token_bucket_queue_io_done(local_bucket, bdev_io);
	} else {
		TAILQ_INSERT_TAIL(&local_bucket->queued_io, bdev_io, internal.link);
	}
}

/*
 * This function is called after the global token has been refilled, to spend
 * the new tokens by attempting as many queued I/Os as the rate limiter allows.
 * The original ordering is preserved.
 */
static uint32_t
local_token_bucket_retry_queued_io(struct local_token_bucket *local_bucket, uint32_t budget)
{
	struct spdk_bdev_io *bdev_io, *tmp_bdev_io;

	TAILQ_FOREACH_SAFE(bdev_io, &local_bucket->queued_io, internal.link, tmp_bdev_io) {
		if (!local_bucket->consume(local_bucket, bdev_io)) {
			/* Simply wait for the next refill. */
			break;
		}

		TAILQ_REMOVE(&local_bucket->queued_io, bdev_io, internal.link);
		local_token_bucket_queue_io_done(local_bucket, bdev_io);

		if (--budget == 0) {
			break;
		}
	}

	return budget;
}

static bool
local_token_bucket_abort_queued_io(struct local_token_bucket *local_bucket,
				   struct spdk_bdev_io *bio_to_abort)
{
	return spdk_bdev_abort_queued_io(&local_bucket->queued_io, bio_to_abort);
}

static void
local_token_bucket_abort_all_queued_io(struct local_token_bucket *local_bucket,
				       struct spdk_bdev_channel *bdev_ch)
{
	spdk_bdev_abort_all_queued_io(&local_bucket->queued_io, bdev_ch);
}

static void
local_token_bucket_unblock_all_queued_io(struct local_token_bucket *local_bucket,
		struct spdk_bdev_channel *bdev_ch)
{
	spdk_bdev_unblock_all_queued_io(&local_bucket->queued_io, bdev_ch);
}

static void
local_token_bucket_reset(struct local_token_bucket *local_bucket)
{
	struct global_token_bucket *global_bucket = local_bucket->global_bucket;

	local_bucket->tokens = 0;
	if (bdev_qos_metric_is_iops(local_bucket->global_bucket->metric)) {
		local_bucket->withdraw_batch_size = BDEV_QOS_MIN_IO_WITHDRAW_BATCH_SIZE;
	} else {
		local_bucket->withdraw_batch_size = BDEV_QOS_MIN_BYTE_WITHDRAW_BATCH_SIZE;
	}
	__atomic_store_n(&local_bucket->capacity, global_bucket->io_burst,
			 __ATOMIC_RELAXED);
}

static void
local_token_bucket_init(struct local_token_bucket *local_bucket,
			struct global_token_bucket *global_bucket)
{
	TAILQ_INIT(&local_bucket->queued_io);

	local_bucket->global_bucket = global_bucket;
	global_token_bucket_add_local_bucket(global_bucket, local_bucket);

	switch (global_bucket->metric) {
	case BDEV_QOS_RW_IOPS:
		local_bucket->consume = local_token_bucket_rw_iops_consume;
		break;
	case BDEV_QOS_RW_BPS:
		local_bucket->consume = local_token_bucket_rw_bps_consume;
		break;
	case BDEV_QOS_R_BPS:
		local_bucket->consume = local_token_bucket_r_bps_consume;
		break;
	case BDEV_QOS_W_BPS:
		local_bucket->consume = local_token_bucket_w_bps_consume;
		break;
	default:
		break;
	}

	local_token_bucket_reset(local_bucket);
}

static void
local_token_bucket_fini(struct local_token_bucket *local_bucket)
{
	global_token_bucket_remove_local_bucket(local_bucket->global_bucket, local_bucket);
}

static struct spdk_bdev_qos_impl *
bdev_burst_qos_get(void)
{
	struct bdev_burst_qos *bqos;
	int i;

	bqos = calloc(1, sizeof(*bqos));
	if (bqos == NULL) {
		return NULL;
	}

	bqos->base.module = &bdev_burst_qos_if;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		global_token_bucket_init(&bqos->global_buckets[i], i);
	}

	if (TAILQ_EMPTY(&g_qos_mgr.bqos_list)) {
		g_qos_mgr.last_tick_tsc = spdk_get_ticks();
		g_qos_mgr.ticks = 0;
		spdk_poller_resume(g_qos_mgr.poller);
	}

	TAILQ_INSERT_TAIL(&g_qos_mgr.bqos_list, bqos, link);

	return &bqos->base;
}

static void
bdev_burst_qos_put(struct spdk_bdev_qos_impl *qos_impl)
{
	struct bdev_burst_qos *bqos = bdev_burst_qos(qos_impl);
	int i;

	TAILQ_REMOVE(&g_qos_mgr.bqos_list, bqos, link);

	if (TAILQ_EMPTY(&g_qos_mgr.bqos_list) && g_qos_mgr.poller != NULL) {
		spdk_poller_pause(g_qos_mgr.poller);
	}

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		global_token_bucket_fini(&bqos->global_buckets[i]);
	}

	free(bqos);
}

static struct global_token_bucket *
bdev_burst_qos_find_global_bucket(struct spdk_bdev_qos_impl *qos_impl,
				  enum bdev_qos_metric metric)
{
	struct bdev_burst_qos *bqos = bdev_burst_qos(qos_impl);

	if (metric >= BDEV_QOS_NUM_METRICS) {
		return NULL;
	}

	return &bqos->global_buckets[metric];
}

static void
bdev_burst_qos_refill(struct bdev_burst_qos *bqos)
{
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		global_token_bucket_refill(&bqos->global_buckets[i]);
	}
}

static struct spdk_bdev_qos_channel_impl *
bdev_burst_qos_channel_get(struct spdk_bdev_qos_impl *qos_impl)
{
	struct bdev_burst_qos *bqos = bdev_burst_qos(qos_impl);
	struct bdev_burst_qos_channel *bqos_ch;
	struct spdk_io_channel *pg_io_ch;
	struct local_token_bucket *local_bucket;
	int i;

	assert(qos_impl->module == &bdev_burst_qos_if);

	bqos_ch = calloc(1, sizeof(*bqos_ch));
	if (bqos_ch == NULL) {
		return NULL;
	}

	bqos_ch->bqos = bqos;
	bqos_ch->base.qos_impl = qos_impl;

	STAILQ_INIT(&bqos_ch->local_bucket_list);

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		local_bucket = &bqos_ch->local_buckets[i];

		local_token_bucket_init(local_bucket, &bqos->global_buckets[i]);
		STAILQ_INSERT_TAIL(&bqos_ch->local_bucket_list, local_bucket, slink);
	}

	pg_io_ch = spdk_get_io_channel(&g_qos_mgr);
	if (pg_io_ch == NULL) {
		for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
			local_token_bucket_fini(&bqos_ch->local_buckets[i]);
		}
		free(bqos_ch);
		return NULL;
	}

	bqos_ch->bgroup = spdk_io_channel_get_ctx(pg_io_ch);

	TAILQ_INSERT_TAIL(&bqos_ch->bgroup->bqos_ch_list, bqos_ch, link);

	return &bqos_ch->base;
}

static void
bdev_burst_qos_channel_put(struct spdk_bdev_qos_channel_impl *qos_ch_impl)
{
	struct bdev_burst_qos_channel *bqos_ch = bdev_burst_qos_channel(qos_ch_impl);
	struct bdev_burst_qos_poll_group *bgroup = bqos_ch->bgroup;
	int i;

	if (bgroup->next_ch == bqos_ch) {
		bgroup->next_ch = NULL;
	}
	TAILQ_REMOVE(&bgroup->bqos_ch_list, bqos_ch, link);

	spdk_put_io_channel(spdk_io_channel_from_ctx(bgroup));

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		local_token_bucket_fini(&bqos_ch->local_buckets[i]);
	}

	free(qos_ch_impl);
}

static void
bdev_burst_qos_channel_queue_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
				struct spdk_bdev_io *bdev_io)
{
	struct bdev_burst_qos_channel *bqos_ch = bdev_burst_qos_channel(qos_ch_impl);
	struct local_token_bucket *local_bucket;

	local_bucket = STAILQ_FIRST(&bqos_ch->local_bucket_list);
	assert(local_bucket != NULL);

	local_token_bucket_queue_io(local_bucket, bdev_io);
}

static bool
bdev_burst_qos_channel_abort_queued_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
				       struct spdk_bdev_io *bio_to_abort)
{
	struct bdev_burst_qos_channel *bqos_ch = bdev_burst_qos_channel(qos_ch_impl);
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		if (local_token_bucket_abort_queued_io(&bqos_ch->local_buckets[i],
						       bio_to_abort)) {
			return true;
		}
	}

	return false;
}

static void
bdev_burst_qos_channel_abort_all_queued_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
		struct spdk_bdev_channel *bdev_ch)
{
	struct bdev_burst_qos_channel *bqos_ch = bdev_burst_qos_channel(qos_ch_impl);
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		local_token_bucket_abort_all_queued_io(&bqos_ch->local_buckets[i], bdev_ch);
	}
}

static void
bdev_burst_qos_channel_unblock_all_queued_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
		struct spdk_bdev_channel *bdev_ch)
{
	struct bdev_burst_qos_channel *bqos_ch = bdev_burst_qos_channel(qos_ch_impl);
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		local_token_bucket_unblock_all_queued_io(&bqos_ch->local_buckets[i], bdev_ch);
	}
}

static bool
bdev_burst_qos_channel_is_throttled(struct spdk_bdev_qos_channel_impl *qos_ch_impl)
{
	struct bdev_burst_qos_channel *bqos_ch = SPDK_CONTAINEROF(qos_ch_impl,
			struct bdev_burst_qos_channel, base);
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		if (!TAILQ_EMPTY(&bqos_ch->local_buckets[i].queued_io)) {
			return true;
		}
	}
	return false;
}

static uint32_t
bdev_burst_qos_channel_retry_queued_io(struct bdev_burst_qos_channel *bqos_ch, uint32_t budget)
{
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		budget = local_token_bucket_retry_queued_io(&bqos_ch->local_buckets[i], budget);
		if (budget == 0) {
			break;
		}
	}

	return budget;
}

static struct local_token_bucket *
bdev_burst_qos_channel_find_local_bucket(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
		enum bdev_qos_metric metric)
{
	struct bdev_burst_qos_channel *bqos_ch = bdev_burst_qos_channel(qos_ch_impl);

	assert(metric < BDEV_QOS_NUM_METRICS);

	return &bqos_ch->local_buckets[metric];
}

static int
bdev_burst_qos_poll_group_drain_queued_io(void *arg)
{
	struct bdev_burst_qos_poll_group *bgroup = arg;
	struct bdev_burst_qos_channel *bqos_ch;
	/* retry_budget is validated to fit in uint32_t by bdev_burst_qos_set_opts. */
	uint32_t budget = (uint32_t)g_qos_opts.retry_budget;
	uint32_t initial_budget = budget;

	bqos_ch = bgroup->next_ch;

	TAILQ_FOREACH_FROM(bqos_ch, &bgroup->bqos_ch_list, link) {
		budget = bdev_burst_qos_channel_retry_queued_io(bqos_ch, budget);
		if (budget == 0) {
			/* Resume from this channel on next invocation. */
			bgroup->next_ch = bqos_ch;
			return SPDK_POLLER_BUSY;
		}
	}

	/* Reached end of list - reset cursor and sleep until next refill. */
	bgroup->next_ch = NULL;
	spdk_poller_pause(bgroup->retry_poller);
	return (budget == initial_budget) ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

static void
bdev_burst_qos_poll_group_destroy(void *io_device, void *ctx_buf)
{
	struct bdev_burst_qos_poll_group *bgroup = ctx_buf;

	spdk_poller_unregister(&bgroup->retry_poller);
}

static int
bdev_burst_qos_poll_group_create(void *io_device, void *ctx_buf)
{
	struct bdev_burst_qos_poll_group *bgroup = ctx_buf;

	TAILQ_INIT(&bgroup->bqos_ch_list);

	bgroup->retry_poller = SPDK_POLLER_REGISTER(bdev_burst_qos_poll_group_drain_queued_io,
			       bgroup, 0);
	spdk_poller_pause(bgroup->retry_poller);
	bgroup->next_ch = NULL;

	return 0;
}

static void
bdev_burst_qos_poll_group_retry_queued_io(struct spdk_io_channel *ch, void *ctx)
{
	struct bdev_burst_qos_poll_group *bgroup = spdk_io_channel_get_ctx(ch);

	spdk_poller_resume(bgroup->retry_poller);
}

static void
bdev_burst_qos_library_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_burst_qos_set_options");

	spdk_json_write_named_object_begin(w, "params");

	spdk_json_write_named_uint64(w, "tick_period_us", g_qos_opts.tick_period_us);
	spdk_json_write_named_uint64(w, "max_io_withdraw_batch_size",
				     g_qos_opts.max_io_withdraw_batch_size);
	spdk_json_write_named_uint64(w, "io_additive_increase_step", g_qos_opts.io_additive_increase_step);
	spdk_json_write_named_uint64(w, "max_byte_withdraw_batch_size",
				     g_qos_opts.max_byte_withdraw_batch_size);
	spdk_json_write_named_uint64(w, "byte_additive_increase_step",
				     g_qos_opts.byte_additive_increase_step);
	spdk_json_write_named_uint64(w, "retry_budget",
				     g_qos_opts.retry_budget);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
bdev_burst_qos_config_json(struct spdk_bdev_qos_impl *qos_impl, struct spdk_json_write_ctx *w)
{
	struct bdev_burst_qos *bqos = bdev_burst_qos(qos_impl);
	const char *name = qos_impl->qos->name;
	struct global_token_bucket *global_bucket;
	int i;

	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		global_bucket = &bqos->global_buckets[i];

		if (global_bucket->avg_rate != UINT64_MAX) {
			global_token_bucket_config_json(global_bucket, name, w);
		}
	}
}

static void
bdev_burst_qos_info_json(struct spdk_bdev_qos_impl *qos_impl, struct spdk_json_write_ctx *w)
{
	struct bdev_burst_qos *bqos = bdev_burst_qos(qos_impl);
	int i;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", "bdev_burst_qos");

	spdk_json_write_named_array_begin(w, "token_buckets");
	for (i = 0; i < BDEV_QOS_NUM_METRICS; i++) {
		global_token_bucket_info_json(&bqos->global_buckets[i], w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

static int
bdev_burst_qos_poll(void *arg)
{
	uint64_t now = spdk_get_ticks();
	struct bdev_burst_qos *bqos;

	if (now < g_qos_mgr.last_tick_tsc + g_qos_mgr.tick_period_tsc) {
		return SPDK_POLLER_IDLE;
	}

	while (now >= g_qos_mgr.last_tick_tsc + g_qos_mgr.tick_period_tsc) {
		g_qos_mgr.last_tick_tsc += g_qos_mgr.tick_period_tsc;
		g_qos_mgr.ticks++;
	}

	TAILQ_FOREACH(bqos, &g_qos_mgr.bqos_list, link) {
		bdev_burst_qos_refill(bqos);
	}

	spdk_for_each_channel_broadcast_rr(&g_qos_mgr,
					   bdev_burst_qos_poll_group_retry_queued_io,
					   NULL);

	return SPDK_POLLER_BUSY;
}

static int
bdev_burst_qos_library_init(void)
{
	g_qos_mgr.tick_period_tsc = g_qos_opts.tick_period_us * spdk_get_ticks_hz() /
				    SPDK_SEC_TO_USEC;
	g_qos_mgr.last_tick_tsc = spdk_get_ticks();
	g_qos_mgr.ticks = 0;

	g_qos_mgr.poller = SPDK_POLLER_REGISTER(bdev_burst_qos_poll, NULL,
						g_qos_opts.tick_period_us);
	spdk_poller_pause(g_qos_mgr.poller);

	spdk_io_device_register(&g_qos_mgr, bdev_burst_qos_poll_group_create,
				bdev_burst_qos_poll_group_destroy,
				sizeof(struct bdev_burst_qos_poll_group),
				"bdev_burst_qos_mgr");

	return 0;
}

static void
bdev_burst_qos_mgr_unregister_cb(void *io_device)
{
	spdk_bdev_qos_module_fini_done();
}

static void
bdev_burst_qos_library_fini(void)
{
	spdk_poller_unregister(&g_qos_mgr.poller);

	spdk_io_device_unregister(&g_qos_mgr, bdev_burst_qos_mgr_unregister_cb);
}

static struct spdk_bdev_qos_module bdev_burst_qos_if = {
	.name = "burst",
	.module_init = bdev_burst_qos_library_init,
	.module_fini = bdev_burst_qos_library_fini,
	.module_config_json = bdev_burst_qos_library_config_json,
	.get_impl = bdev_burst_qos_get,
	.put_impl = bdev_burst_qos_put,
	.impl_config_json = bdev_burst_qos_config_json,
	.impl_info_json = bdev_burst_qos_info_json,
	.get_channel_impl = bdev_burst_qos_channel_get,
	.put_channel_impl = bdev_burst_qos_channel_put,
	.queue_io = bdev_burst_qos_channel_queue_io,
	.abort_queued_io = bdev_burst_qos_channel_abort_queued_io,
	.abort_all_queued_io = bdev_burst_qos_channel_abort_all_queued_io,
	.unblock_all_queued_io = bdev_burst_qos_channel_unblock_all_queued_io,
	.is_throttled = bdev_burst_qos_channel_is_throttled,
	.async_fini = true,
};

SPDK_BDEV_QOS_MODULE_REGISTER(bdev_burst_qos, &bdev_burst_qos_if)

struct burst_qos_set_limit_ctx {
	enum bdev_qos_metric metric;
	spdk_bdev_qos_op_cb cb_fn;
	void *cb_arg;
};


static void
bdev_burst_qos_set_limit_done(struct burst_qos_set_limit_ctx *ctx, int status)
{
	if (ctx->cb_fn != NULL) {
		ctx->cb_fn(ctx->cb_arg, status);
	}
	free(ctx);
}

static void
bdev_burst_qos_channel_reset(struct spdk_io_channel_iter *i)
{
	struct burst_qos_set_limit_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_bdev_qos_channel *qos_ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev_qos_channel_impl *qos_ch_impl;
	struct local_token_bucket *local_bucket;

	qos_ch_impl = spdk_bdev_qos_channel_find_impl(qos_ch, &bdev_burst_qos_if);
	assert(qos_ch_impl != NULL);

	local_bucket = bdev_burst_qos_channel_find_local_bucket(qos_ch_impl, ctx->metric);
	local_token_bucket_reset(local_bucket);

	spdk_for_each_channel_continue(i, 0);
}

static void
bdev_burst_qos_channel_reset_done(struct spdk_io_channel_iter *i, int status)
{
	struct burst_qos_set_limit_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	bdev_burst_qos_set_limit_done(ctx, status);
}

static void
bdev_burst_qos_set_limit(struct spdk_bdev_qos_impl *qos_impl,
			 enum bdev_qos_metric qos_metric, uint64_t avg_rate,
			 enum bdev_qos_mode qos_mode, uint64_t burst_size,
			 uint64_t max_burst_rate, uint64_t max_burst_time_in_sec,
			 uint64_t refill_period_us, uint64_t io_burst,
			 uint64_t max_withdraw_batch_size,
			 uint64_t additive_increase_step,
			 spdk_bdev_qos_op_cb cb_fn, void *cb_arg)
{
	struct burst_qos_set_limit_ctx *ctx;
	struct global_token_bucket *global_bucket;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->metric = qos_metric;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	global_bucket = bdev_burst_qos_find_global_bucket(qos_impl, qos_metric);
	if (global_bucket == NULL) {
		SPDK_ERRLOG("global_bucket was not found\n");
		bdev_burst_qos_set_limit_done(ctx, -EINVAL);
		return;
	}

	rc = global_token_bucket_set(global_bucket, avg_rate, qos_mode, burst_size,
				     max_burst_rate, max_burst_time_in_sec,
				     refill_period_us, io_burst,
				     max_withdraw_batch_size, additive_increase_step);
	if (rc != 0) {
		bdev_burst_qos_set_limit_done(ctx, rc);
		return;
	}

	spdk_for_each_channel(qos_impl->qos, bdev_burst_qos_channel_reset,
			      ctx, bdev_burst_qos_channel_reset_done);
}

struct burst_qos_json {
	enum bdev_qos_metric qos_metric;
	uint64_t avg_rate;
	enum bdev_qos_mode qos_mode;
	uint64_t burst_size;
	uint64_t max_burst_rate;
	uint64_t max_burst_time_in_sec;
	uint64_t refill_period_us;
	uint64_t io_burst;
	uint64_t max_withdraw_batch_size;
	uint64_t additive_increase_step;
};

static int
rpc_decode_qos_metric(const struct spdk_json_val *val, void *out)
{
	enum bdev_qos_metric *qos_metric = out;

	if (spdk_json_strequal(val, "rw_iops") == true) {
		*qos_metric = BDEV_QOS_RW_IOPS;
	} else if (spdk_json_strequal(val, "rw_mbps") == true) {
		*qos_metric = BDEV_QOS_RW_BPS;
	} else if (spdk_json_strequal(val, "r_mbps") == true) {
		*qos_metric = BDEV_QOS_R_BPS;
	} else if (spdk_json_strequal(val, "w_mbps") == true) {
		*qos_metric = BDEV_QOS_W_BPS;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: qos_metric\n");
		return -EINVAL;
	}

	return 0;
}

static int
rpc_decode_qos_mode(const struct spdk_json_val *val, void *out)
{
	enum bdev_qos_mode *qos_mode = out;

	if (spdk_json_strequal(val, "strict") == true) {
		*qos_mode = BDEV_QOS_MODE_STRICT;
	} else if (spdk_json_strequal(val, "burst_ready") == true) {
		*qos_mode = BDEV_QOS_MODE_BURST_READY;
	} else if (spdk_json_strequal(val, "earned_burst") == true) {
		*qos_mode = BDEV_QOS_MODE_EARNED_BURST;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: qos_mode\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder burst_qos_json_decoders[] = {
	{"qos_metric", offsetof(struct burst_qos_json, qos_metric), rpc_decode_qos_metric},
	{"avg_rate", offsetof(struct burst_qos_json, avg_rate), spdk_json_decode_uint64},
	{"qos_mode", offsetof(struct burst_qos_json, qos_mode), rpc_decode_qos_mode, true},
	{"burst_size", offsetof(struct burst_qos_json, burst_size), spdk_json_decode_uint64, true},
	{"max_burst_rate", offsetof(struct burst_qos_json, max_burst_rate), spdk_json_decode_uint64, true},
	{"max_burst_time_in_sec", offsetof(struct burst_qos_json, max_burst_time_in_sec), spdk_json_decode_uint64, true},
};

static const struct spdk_json_object_decoder burst_qos_json_decoders2[] = {
	{"refill_period_us", offsetof(struct burst_qos_json, refill_period_us), spdk_json_decode_uint64, true},
	{"io_burst", offsetof(struct burst_qos_json, io_burst), spdk_json_decode_uint64, true},
	{"max_withdraw_batch_size", offsetof(struct burst_qos_json, max_withdraw_batch_size), spdk_json_decode_uint64, true},
	{"additive_increase_step", offsetof(struct burst_qos_json, additive_increase_step), spdk_json_decode_uint64, true},
};

void
bdev_burst_qos_set_limit_json(struct spdk_bdev_qos *qos,
			      const struct spdk_json_val *params,
			      spdk_bdev_qos_op_cb cb_fn, void *cb_arg)
{
	struct burst_qos_json req = {
		.qos_mode = BDEV_QOS_MODE_STRICT,
	};
	struct spdk_bdev_qos_impl *qos_impl;

	if (!spdk_thread_is_app_thread(NULL)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	qos_impl = spdk_bdev_qos_find_impl(qos, &bdev_burst_qos_if);
	if (qos_impl == NULL) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (spdk_json_decode_object_relaxed(params, burst_qos_json_decoders,
					    SPDK_COUNTOF(burst_qos_json_decoders), &req)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	req.refill_period_us = 0;
	if (bdev_qos_metric_is_iops(req.qos_metric)) {
		req.io_burst = BDEV_QOS_DEFAULT_IO_BURST_SIZE;
		req.max_withdraw_batch_size = g_qos_opts.max_io_withdraw_batch_size;
		req.additive_increase_step = g_qos_opts.io_additive_increase_step;
	} else {
		req.io_burst = BDEV_QOS_DEFAULT_IO_BURST_BYTES;
		req.max_withdraw_batch_size = g_qos_opts.max_byte_withdraw_batch_size;
		req.additive_increase_step = g_qos_opts.byte_additive_increase_step;
	}

	if (spdk_json_decode_object_relaxed(params, burst_qos_json_decoders2,
					    SPDK_COUNTOF(burst_qos_json_decoders2), &req)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	bdev_burst_qos_set_limit(qos_impl, req.qos_metric, req.avg_rate, req.qos_mode,
				 req.burst_size,
				 req.max_burst_rate, req.max_burst_time_in_sec,
				 req.refill_period_us, req.io_burst,
				 req.max_withdraw_batch_size,
				 req.additive_increase_step,
				 cb_fn, cb_arg);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_burst_qos)
