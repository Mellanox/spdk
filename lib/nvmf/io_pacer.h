/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef IO_PACER_H
#define IO_PACER_H

#include <stdint.h>
#include <rte_config.h>
#include <rte_hash.h>
#include <rte_spinlock.h>
#include <rte_atomic.h>
#include <rte_jhash.h>
#include "spdk/stdinc.h"
#include "spdk_internal/log.h"
#include "spdk/nvmf.h"

struct spdk_io_pacer;
struct spdk_io_pacer_tuner;
struct spdk_io_pacer_tuner2;
typedef void (*spdk_io_pacer_pop_cb)(void *io);
struct io_pacer_queue_entry {
	uint64_t size;
	STAILQ_ENTRY(io_pacer_queue_entry) link;
};


struct spdk_io_pacer_drives_stats {
	struct rte_hash *h;
	rte_spinlock_t lock;
};

extern struct spdk_io_pacer_drives_stats drives_stats;

struct drive_stats {
	rte_atomic32_t ops_in_flight;
	struct spdk_bdev *bdev;
};

typedef void (*spdk_io_pacer_pop_cb)(void *io);

struct spdk_io_pacer *spdk_io_pacer_create(uint32_t period_ns,
					   uint32_t credit,
					   uint32_t disk_credit,
					   spdk_io_pacer_pop_cb pop_cb);
void spdk_io_pacer_destroy(struct spdk_io_pacer *pacer);
int spdk_io_pacer_create_queue(struct spdk_io_pacer *pacer, uint64_t key);
int spdk_io_pacer_destroy_queue(struct spdk_io_pacer *pacer, uint64_t key);
int spdk_io_pacer_push(struct spdk_io_pacer *pacer,
		       uint64_t key,
		       struct io_pacer_queue_entry *entry);
void spdk_io_pacer_get_stat(const struct spdk_io_pacer *pacer,
			    struct spdk_nvmf_transport_poll_group_stat *stat);
struct spdk_io_pacer_tuner *spdk_io_pacer_tuner_create(struct spdk_io_pacer *pacer,
						       uint32_t tuner_period_us,
						       uint32_t tuner_step_ns);
void spdk_io_pacer_tuner_destroy(struct spdk_io_pacer_tuner *tuner);
struct spdk_io_pacer_tuner2 *spdk_io_pacer_tuner2_create(struct spdk_io_pacer *pacer,
							 uint32_t period_us,
							 uint32_t min_threshold,
							 uint64_t factor);
void spdk_io_pacer_tuner2_destroy(struct spdk_io_pacer_tuner2 *tuner);
void spdk_io_pacer_tuner2_add(struct spdk_io_pacer_tuner2 *tuner, uint32_t value);
void spdk_io_pacer_tuner2_sub(struct spdk_io_pacer_tuner2 *tuner, uint32_t value);

struct drive_stats* spdk_io_pacer_drive_stats_create(struct spdk_io_pacer_drives_stats *stats,
						     uint64_t key, struct spdk_bdev *bdev);
struct drive_stats * spdk_io_pacer_drive_stats_get(struct spdk_io_pacer_drives_stats *stats,
						   uint64_t key);
void spdk_io_pacer_drive_stats_try_init(uint64_t key, struct spdk_bdev *bdev);

static inline void drive_stats_lock(struct spdk_io_pacer_drives_stats *stats) {
	rte_spinlock_lock(&stats->lock);
}

static inline void drive_stats_unlock(struct spdk_io_pacer_drives_stats *stats) {
	rte_spinlock_unlock(&stats->lock);
}

static inline void spdk_io_pacer_drive_stats_add(struct spdk_io_pacer_drives_stats *stats,
						 uint64_t key,
						 uint32_t val)
{
	struct drive_stats *drive_stats = spdk_io_pacer_drive_stats_get(stats, key);
	rte_atomic32_add(&drive_stats->ops_in_flight, val);
}

static inline void spdk_io_pacer_drive_stats_sub(struct spdk_io_pacer_drives_stats *stats,
						 uint64_t key,
						 uint32_t val)
{
	struct drive_stats *drive_stats = spdk_io_pacer_drive_stats_get(stats, key);
	rte_atomic32_sub(&drive_stats->ops_in_flight, val);
}

#endif /* IO_PACER_H */
