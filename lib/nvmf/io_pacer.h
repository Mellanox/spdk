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


struct spdk_io_pacer_drives_stats {
	struct rte_hash *h;
	rte_spinlock_t lock;
};

extern struct spdk_io_pacer_drives_stats drives_stats;
	
struct drive_stats {
	rte_atomic32_t ops_in_flight;
};



typedef void (*spdk_io_pacer_pop_cb)(void *io);
struct spdk_io_pacer *spdk_io_pacer_create(uint32_t period_ns,
					   uint32_t tuner_period_us,
					   uint32_t tuner_step_ns,
					   uint32_t disk_credit,
					   spdk_io_pacer_pop_cb pop_cb,
					   void *ctx);
void spdk_io_pacer_destroy(struct spdk_io_pacer *pacer);
int spdk_io_pacer_create_queue(struct spdk_io_pacer *pacer, uint64_t key);
int spdk_io_pacer_destroy_queue(struct spdk_io_pacer *pacer, uint64_t key);
int spdk_io_pacer_push(struct spdk_io_pacer *pacer, uint64_t key, void *io);
void spdk_io_pacer_get_stat(const struct spdk_io_pacer *pacer,
			    struct spdk_nvmf_transport_poll_group_stat *stat);


static inline void drive_stats_lock(struct spdk_io_pacer_drives_stats *stats) {
	rte_spinlock_lock(&stats->lock);
}

static inline void drive_stats_unlock(struct spdk_io_pacer_drives_stats *stats) {
	rte_spinlock_unlock(&stats->lock);
}


static inline struct drive_stats* spdk_io_pacer_drive_stats_create(struct spdk_io_pacer_drives_stats *stats, uint64_t key)
{
	
	int32_t ret = 0;
	struct drive_stats *data = NULL;    
	struct rte_hash *h = stats->h;

        ret = rte_hash_lookup(h, &key);
        if (ret != -ENOENT)
                return 0;

	drive_stats_lock(stats);
	data = calloc(1, sizeof(struct drive_stats));
	rte_atomic32_init(&data->ops_in_flight);
	ret = rte_hash_add_key_data(h, (void *) &key, data);
	if (ret < 0) {
		SPDK_ERRLOG("Can't add key to drive statistics dict: %" PRIx64 "\n", key);
		goto err;
	}
	goto exit;
err:
	free(data);
	data = NULL;
exit:
	drive_stats_unlock(stats);
	return data;
}


static inline struct drive_stats * spdk_io_pacer_drive_stats_get(struct spdk_io_pacer_drives_stats *stats, uint64_t key)
{
	struct drive_stats *data = NULL;
	int ret = 0;
	ret = rte_hash_lookup_data(stats->h, (void*) &key, (void**) &data);
	if (ret == -EINVAL) {
		SPDK_ERRLOG("Drive statistics seems broken\n");
	} else if (unlikely(ret == -ENOENT)) {
		SPDK_NOTICELOG("Creating drive stats for key: %" PRIx64 "\n", key);
		data = spdk_io_pacer_drive_stats_create(stats, key);
	}
	return data;
}

static inline void spdk_io_pacer_drive_stats_add(struct spdk_io_pacer_drives_stats *stats, uint64_t key, uint32_t val)
{
	struct drive_stats *drive_stats = spdk_io_pacer_drive_stats_get(stats, key);
	rte_atomic32_add(&drive_stats->ops_in_flight, val);
}

static inline void spdk_io_pacer_drive_stats_sub(struct spdk_io_pacer_drives_stats *stats, uint64_t key, uint32_t val)
{
	struct drive_stats *drive_stats = spdk_io_pacer_drive_stats_get(stats, key);
	rte_atomic32_sub(&drive_stats->ops_in_flight, val);
}
void spdk_io_pacer_drive_stats_setup(struct spdk_io_pacer_drives_stats *stats, int32_t entries);

#endif /* IO_PACER_H */
