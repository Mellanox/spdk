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

#include "spdk/config.h"
#include "io_pacer.h"
#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"

#define IO_PACER_DEFAULT_MAX_QUEUES 32

#ifdef SPDK_CONFIG_VTUNE
#include <ittnotify.h>
extern __itt_string_handle *io_pacer_poll_task;
extern __itt_domain *io_pacer_domain;
#endif /* SPDK_CONFIG_VTUNE */


#define MAX_DRIVES_STATS 256
static rte_spinlock_t drives_stats_create_lock = RTE_SPINLOCK_INITIALIZER;
struct spdk_io_pacer_drives_stats drives_stats = {0};

struct io_pacer_queue {
	uint64_t key;
	struct drive_stats *stats;
	STAILQ_HEAD(, io_pacer_queue_entry) queue;
};

struct spdk_io_pacer {
	uint64_t period_ticks;
	int64_t credit;
	int64_t remaining_credit;
	uint32_t max_queues;
	spdk_io_pacer_pop_cb pop_cb;
	uint32_t num_queues;
	uint32_t next_queue;
	uint64_t num_ios;
	uint64_t first_tick;
	uint64_t last_tick;
	struct spdk_nvmf_io_pacer_stat stat;
	struct io_pacer_queue *queues;
	struct spdk_poller *poller;
	uint32_t disk_credit;
};

struct spdk_io_pacer_tuner {
	struct spdk_io_pacer *pacer;
	uint64_t period_ns;
	uint64_t step_ns;
	uint64_t min_pacer_period_ticks;
	uint64_t max_pacer_period_ticks;
	uint64_t last_bytes;
	struct spdk_poller *poller;
};


static struct io_pacer_queue *
io_pacer_get_queue(struct spdk_io_pacer *pacer, uint64_t key)
{
	uint32_t i;
	for (i = 0; i < pacer->num_queues; ++i) {
		if (pacer->queues[i].key == key) {
			return &pacer->queues[i];
		}
	}

	/* @todo: Creating queue on demand due to limitations in rdma transport.
	 * To be removed.
	 */
	if (0 != spdk_io_pacer_create_queue(pacer, key)) {
		return NULL;
	}

	return io_pacer_get_queue(pacer, key);
}

static int
io_pacer_poll(void *arg)
{
	struct spdk_io_pacer *pacer = arg;
	struct io_pacer_queue_entry *entry;
	uint32_t next_queue = pacer->next_queue;
	int rc = 0;
	uint32_t ops_in_flight = 0;

	const uint64_t cur_tick = spdk_get_ticks();
	const uint64_t ticks_diff = cur_tick - pacer->last_tick;

	uint32_t attempts_cnt = 0;

#ifdef SPDK_CONFIG_VTUNE
	static __thread uint64_t poll_cnt;
	poll_cnt++;
	if (poll_cnt % 100 == 0)
		__itt_task_begin(io_pacer_domain, __itt_null, __itt_null, io_pacer_poll_task);
#endif /* SPDK_CONFIG_VTUNE */

	pacer->stat.calls++;
	if (ticks_diff < pacer->period_ticks) {
		return 0;
	}
	pacer->stat.total_ticks = cur_tick - pacer->first_tick;
	pacer->last_tick = cur_tick - ticks_diff % pacer->period_ticks;
	pacer->stat.polls++;

	pacer->remaining_credit = spdk_min(pacer->remaining_credit + pacer->credit,
					   pacer->credit);

	if (pacer->num_ios == 0) {
		pacer->stat.no_ios++;
	}

	while ((pacer->num_ios > 0) &&
	       (pacer->remaining_credit > 0) &&
	       (attempts_cnt < pacer->num_queues)) {
		next_queue %= pacer->num_queues;
		attempts_cnt++;

		if (pacer->disk_credit) {
			ops_in_flight = rte_atomic32_read(&pacer->queues[next_queue].stats->ops_in_flight);
			if (ops_in_flight > pacer->disk_credit) {
				next_queue++;
				continue;
			}
		}
		entry = STAILQ_FIRST(&pacer->queues[next_queue].queue);
		next_queue++;
		if (entry != NULL) {
			STAILQ_REMOVE_HEAD(&pacer->queues[next_queue - 1].queue, link);
			pacer->num_ios--;
			pacer->next_queue = next_queue;
			pacer->remaining_credit -= entry->size;
			pacer->stat.ios++;
			pacer->stat.bytes += entry->size;
			rte_atomic32_add(&pacer->queues[next_queue - 1].stats->ops_in_flight, 1);
			pacer->pop_cb(entry);
			rc++;
			attempts_cnt = 0;
		}
	}

#ifdef  SPDK_CONFIG_VTUNE
	if (poll_cnt % 100 == 0)
		__itt_task_end(io_pacer_domain);
#endif /* SPDK_CONFIG_VTUNE */

	return rc;
}

struct spdk_io_pacer *
spdk_io_pacer_create(uint32_t period_ns,
		     uint32_t credit,
		     uint32_t disk_credit,
		     spdk_io_pacer_pop_cb pop_cb)
{
	struct spdk_io_pacer *pacer;

	assert(pop_cb != NULL);

	pacer = (struct spdk_io_pacer *)calloc(1, sizeof(struct spdk_io_pacer));
	if (!pacer) {
		SPDK_ERRLOG("Failed to allocate IO pacer\n");
		return NULL;
	}

	/* @todo: may overflow? */
	pacer->period_ticks = (period_ns * spdk_get_ticks_hz()) / SPDK_SEC_TO_NSEC;
	pacer->credit = credit;
	pacer->disk_credit = disk_credit;
	pacer->pop_cb = pop_cb;
	pacer->first_tick = spdk_get_ticks();
	pacer->last_tick = spdk_get_ticks();
	pacer->poller = SPDK_POLLER_REGISTER(io_pacer_poll, (void *)pacer, 0);
	if (!pacer->poller) {
		SPDK_ERRLOG("Failed to create poller for IO pacer\n");
		spdk_io_pacer_destroy(pacer);
		return NULL;
	}

	SPDK_NOTICELOG("Created IO pacer %p: period_ns %u, period_ticks %lu, max_queues %u, credit %ld, disk_credit %u, core %u\n",
		       pacer,
		       period_ns,
		       pacer->period_ticks,
		       pacer->max_queues,
		       pacer->credit,
		       pacer->disk_credit,
		       spdk_env_get_current_core());

	return pacer;
}

void
spdk_io_pacer_destroy(struct spdk_io_pacer *pacer)
{
	uint32_t i;

	assert(pacer != NULL);

	/* Check if we have something in the queues */
	for (i = 0; i < pacer->num_queues; ++i) {
		if (!STAILQ_EMPTY(&pacer->queues[i].queue)) {
			SPDK_WARNLOG("IO pacer queue is not empty on pacer destroy: pacer %p, key %016lx\n",
				     pacer, pacer->queues[i].key);
		}
	}

	spdk_poller_unregister(&pacer->poller);
	free(pacer->queues);
	free(pacer);
	SPDK_NOTICELOG("Destroyed IO pacer %p\n", pacer);
}

int
spdk_io_pacer_create_queue(struct spdk_io_pacer *pacer, uint64_t key)
{
	assert(pacer != NULL);

	if (pacer->num_queues >= pacer->max_queues) {
		const uint32_t new_max_queues = pacer->max_queues ?
			2 * pacer->max_queues : IO_PACER_DEFAULT_MAX_QUEUES;
		struct io_pacer_queue *new_queues =
			(struct io_pacer_queue *)realloc(pacer->queues,
							 new_max_queues * sizeof(*pacer->queues));
		if (!new_queues) {
			SPDK_NOTICELOG("Failed to allocate more queues for IO pacer %p: max_queues %u\n",
				       pacer, new_max_queues);
			return -1;
		}

		pacer->queues = new_queues;
		pacer->max_queues = new_max_queues;
		SPDK_NOTICELOG("Allocated more queues for IO pacer %p: max_queues %u\n",
			       pacer, pacer->max_queues);
	}

	pacer->queues[pacer->num_queues].key = key;
	STAILQ_INIT(&pacer->queues[pacer->num_queues].queue);
	pacer->queues[pacer->num_queues].stats = spdk_io_pacer_drive_stats_get(&drives_stats, key);
	pacer->num_queues++;
	SPDK_NOTICELOG("Created IO pacer queue: pacer %p, key %016lx\n",
		       pacer, key);

	return 0;
}

int
spdk_io_pacer_destroy_queue(struct spdk_io_pacer *pacer, uint64_t key)
{
	uint32_t i;

	assert(pacer != NULL);

	for (i = 0; i < pacer->num_queues; ++i) {
		if (pacer->queues[i].key == key) {
			if (!STAILQ_EMPTY(&pacer->queues[i].queue)) {
				SPDK_WARNLOG("Destroying non empty IO pacer queue: key %016lx\n", key);
			}

			memmove(&pacer->queues[i], &pacer->queues[i + 1],
				(pacer->num_queues - i - 1) * sizeof(struct io_pacer_queue));
			pacer->num_queues--;
			SPDK_NOTICELOG("Destroyed IO pacer queue: pacer %p, key %016lx\n",
				       pacer, key);
			return 0;
		}
	}

	SPDK_ERRLOG("IO pacer queue not found: key %016lx\n", key);
	return -1;
}

int
spdk_io_pacer_push(struct spdk_io_pacer *pacer, uint64_t key, struct io_pacer_queue_entry *entry)
{
	struct io_pacer_queue *queue;

	assert(pacer != NULL);
	assert(entry != NULL);

	queue = io_pacer_get_queue(pacer, key);
	if (spdk_unlikely(queue == NULL)) {
		SPDK_ERRLOG("IO pacer queue not found: key %016lx\n", key);
		return -1;
	}

	STAILQ_INSERT_TAIL(&queue->queue, entry, link);
	pacer->num_ios++;
	return 0;
}

void
spdk_io_pacer_get_stat(const struct spdk_io_pacer *pacer,
		       struct spdk_nvmf_transport_poll_group_stat *stat)
{
	if (pacer && stat) {
		stat->io_pacer.total_ticks = pacer->stat.total_ticks;
		stat->io_pacer.polls = pacer->stat.polls;
		stat->io_pacer.ios = pacer->stat.ios;
		stat->io_pacer.bytes = pacer->stat.bytes;
		stat->io_pacer.calls = pacer->stat.calls;
		stat->io_pacer.no_ios = pacer->stat.no_ios;
		stat->io_pacer.period_ticks = pacer->period_ticks;
	}
}

static int
io_pacer_tune(void *arg)
{
	struct spdk_io_pacer_tuner *tuner = arg;
	struct spdk_io_pacer *pacer = tuner->pacer;
	const uint64_t ticks_hz = spdk_get_ticks_hz();
	const uint64_t bytes = pacer->stat.bytes - tuner->last_bytes;
	/* We do calculations in terms of credit sized IO */
	const uint64_t io_period_ns = tuner->period_ns / ((bytes != 0) ? (bytes / pacer->credit) : 1);

	const uint64_t cur_period_ns = (pacer->period_ticks * SPDK_SEC_TO_NSEC) / ticks_hz;
	/* We always want to set pacer period one step shorter than measured IO period.
	 * But we limit changes to one step at a time in any direction.
	 */
	uint64_t new_period_ns = io_period_ns - tuner->step_ns;
	if (new_period_ns > cur_period_ns + tuner->step_ns) {
		new_period_ns = cur_period_ns + tuner->step_ns;
	} else if (new_period_ns < cur_period_ns - tuner->step_ns) {
		new_period_ns = cur_period_ns - tuner->step_ns;
	}

	uint64_t new_period_ticks = (new_period_ns * ticks_hz) / SPDK_SEC_TO_NSEC;
	new_period_ticks = spdk_max(spdk_min(new_period_ticks, tuner->max_pacer_period_ticks),
				    tuner->min_pacer_period_ticks);

	static __thread uint32_t log_counter = 0;
	/* Try to log once per second */
	if (log_counter % (SPDK_SEC_TO_NSEC / tuner->period_ns) == 0) {
		SPDK_NOTICELOG("IO pacer tuner %p: pacer %p, bytes %lu, io period %lu ns, new period %lu ns, new period %lu ticks, min %lu, max %lu\n",
			       tuner,
			       pacer,
			       pacer->stat.bytes - tuner->last_bytes,
			       io_period_ns,
			       new_period_ns,
			       new_period_ticks,
			       tuner->min_pacer_period_ticks,
			       tuner->max_pacer_period_ticks);
	}
	log_counter++;

	pacer->period_ticks = new_period_ticks;
	tuner->last_bytes = pacer->stat.bytes;

	return 1;
}

struct spdk_io_pacer_tuner *
spdk_io_pacer_tuner_create(struct spdk_io_pacer *pacer,
			   uint32_t period_us,
			   uint32_t step_ns)
{
	struct spdk_io_pacer_tuner *tuner;

	assert(pacer != NULL);

	tuner = (struct spdk_io_pacer_tuner *)calloc(1, sizeof(struct spdk_io_pacer_tuner));
	if (!tuner) {
		SPDK_ERRLOG("Failed to allocate IO pacer tuner\n");
		return NULL;
	}

	tuner->pacer = pacer;
	tuner->period_ns = 1000ULL * period_us;
	tuner->step_ns = step_ns;
	tuner->min_pacer_period_ticks = pacer->period_ticks;
	tuner->max_pacer_period_ticks = 2 * tuner->min_pacer_period_ticks;

	if (0 != period_us) {
		tuner->poller = SPDK_POLLER_REGISTER(io_pacer_tune, (void *)tuner, period_us);
		if (!tuner->poller) {
			SPDK_ERRLOG("Failed to create tuner poller for IO pacer\n");
			spdk_io_pacer_tuner_destroy(tuner);
			return NULL;
		}
	}

	SPDK_NOTICELOG("Created IO pacer tuner %p: pacer %p, period_ns %lu, step_ns %lu, min_pacer_period_ticks %lu, max_pacer_period_ticks %lu\n",
		       tuner,
		       pacer,
		       tuner->period_ns,
		       tuner->step_ns,
		       tuner->min_pacer_period_ticks,
		       tuner->max_pacer_period_ticks);

	return tuner;
}

void
spdk_io_pacer_tuner_destroy(struct spdk_io_pacer_tuner *tuner)
{
	assert(tuner != NULL);

	spdk_poller_unregister(&tuner->poller);
	free(tuner);
	SPDK_NOTICELOG("Destroyed IO pacer tuner %p\n", tuner);
}

struct spdk_io_pacer_tuner2 {
	struct spdk_io_pacer *pacer;
	uint64_t period_ns;
	uint32_t value;
	uint32_t min_threshold;
	uint64_t factor;
	uint64_t min_pacer_period_ticks;
	uint64_t max_pacer_period_ticks;
	struct spdk_poller *poller;
};

static int
io_pacer_tune2(void *arg)
{
	struct spdk_io_pacer_tuner2 *tuner = arg;
	struct spdk_io_pacer *pacer = tuner->pacer;
	uint32_t v = tuner->value;

	uint64_t new_period_ticks = (v <= tuner->min_threshold) ?
		tuner->min_pacer_period_ticks :
		(v - tuner->min_threshold) * tuner->factor / 1000 + tuner->min_pacer_period_ticks;
	new_period_ticks = spdk_min(new_period_ticks, tuner->max_pacer_period_ticks);

	static __thread uint32_t log_counter = 0;
	/* Try to log once per second */
	if (log_counter % (SPDK_SEC_TO_NSEC / tuner->period_ns) == 0) {
		SPDK_NOTICELOG("IO pacer tuner %p: pacer %p, value %u, new period %lu ticks, min %lu, polls %u. ios %u\n",
			       tuner,
			       pacer,
			       v,
			       new_period_ticks,
			       tuner->min_pacer_period_ticks,
			       pacer->stat.polls,
			       pacer->stat.ios);
	}
	log_counter++;

	pacer->period_ticks = new_period_ticks;

	return 1;
}

struct spdk_io_pacer_tuner2 *
spdk_io_pacer_tuner2_create(struct spdk_io_pacer *pacer,
			    uint32_t period_us,
			    uint32_t min_threshold,
			    uint64_t factor)
{
	struct spdk_io_pacer_tuner2 *tuner;

	assert(pacer != NULL);

	tuner = (struct spdk_io_pacer_tuner2 *)calloc(1, sizeof(struct spdk_io_pacer_tuner2));
	if (!tuner) {
		SPDK_ERRLOG("Failed to allocate IO pacer tuner\n");
		return NULL;
	}

	tuner->pacer = pacer;
	tuner->period_ns = 1000ULL * period_us;
	tuner->value = 0;
	tuner->min_threshold = min_threshold;
	tuner->factor = factor;
	tuner->min_pacer_period_ticks = pacer->period_ticks;
	tuner->max_pacer_period_ticks = 4 * tuner->min_pacer_period_ticks;

	if (0 != period_us) {
		tuner->poller = SPDK_POLLER_REGISTER(io_pacer_tune2, (void *)tuner, period_us);
		if (!tuner->poller) {
			SPDK_ERRLOG("Failed to create tuner poller for IO pacer\n");
			spdk_io_pacer_tuner2_destroy(tuner);
			return NULL;
		}
	}

	SPDK_NOTICELOG("Created IO pacer tuner %p: pacer %p, period_ns %lu, threshold %u, factor %lu\n",
		       tuner,
		       pacer,
		       tuner->period_ns,
		       tuner->min_threshold,
		       tuner->factor);

	return tuner;
}

void
spdk_io_pacer_tuner2_destroy(struct spdk_io_pacer_tuner2 *tuner)
{
	assert(tuner != NULL);

	spdk_poller_unregister(&tuner->poller);
	free(tuner);
	SPDK_NOTICELOG("Destroyed IO pacer tuner %p\n", tuner);
}

void
spdk_io_pacer_tuner2_add(struct spdk_io_pacer_tuner2 *tuner, uint32_t value)
{
	assert(tuner != NULL);
	tuner->value += value;
}

void
spdk_io_pacer_tuner2_sub(struct spdk_io_pacer_tuner2 *tuner, uint32_t value)
{
	assert(tuner != NULL);
	tuner->value -= value;
}

struct drive_stats* spdk_io_pacer_drive_stats_create(struct spdk_io_pacer_drives_stats *stats,
								   uint64_t key,
								   struct spdk_bdev *bdev)
{
	int32_t ret = 0;
	struct drive_stats *data = NULL;
	struct rte_hash *h = stats->h;

	ret = rte_hash_lookup(h, &key);
	if (ret != -ENOENT)
		return 0;

	drive_stats_lock(stats);

	ret = rte_hash_lookup(h, &key);
	if (ret != -ENOENT)
		return 0;

	data = calloc(1, sizeof(struct drive_stats));
	rte_atomic32_init(&data->ops_in_flight);
	/* FIXME just workaround to test */
	data->bdev = bdev;
	ret = rte_hash_add_key_data(h, (void *) &key, data);
	if (ret < 0) {
		SPDK_ERRLOG("Can't add key to drive statistics dict: %" PRIx64 "\n",
			    key);
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

struct drive_stats * spdk_io_pacer_drive_stats_get(struct spdk_io_pacer_drives_stats *stats,
								 uint64_t key)
{
	struct drive_stats *data = NULL;
	int ret = 0;
	ret = rte_hash_lookup_data(stats->h, (void*) &key, (void**) &data);
	if (unlikely(ret < 0)) {
		SPDK_ERRLOG("Drive statistics seems broken\n");
    }
	return data;
}

static inline void spdk_io_pacer_drive_stats_setup(struct spdk_io_pacer_drives_stats *stats, int32_t entries)
{
	struct rte_hash_parameters hash_params = {
		.name = "DRIVE_STATS",
		.entries = entries,
		.key_len = sizeof(uint64_t),
		.socket_id = rte_socket_id(),
		.hash_func = rte_jhash,
		.hash_func_init_val = 0,
	};
	struct rte_hash *h = NULL;

	if (stats->h != NULL) {
		return;
	}

	rte_spinlock_lock(&drives_stats_create_lock);
	if (stats->h != NULL) {
		goto exit;
	}

	h = rte_hash_create(&hash_params);
	if (h == NULL) {
		SPDK_ERRLOG("IO pacer can't create drive statistics dict\n");
	}

	stats->h = h;
	rte_spinlock_init(&stats->lock);
	SPDK_NOTICELOG("Drives stats setup done\n");

 exit:
	rte_spinlock_unlock(&drives_stats_create_lock);
}

void spdk_io_pacer_drive_stats_try_init(uint64_t key, struct spdk_bdev *bdev)
{
	struct drive_stats *data;
	int ret = 0;

	spdk_io_pacer_drive_stats_setup(&drives_stats, MAX_DRIVES_STATS);
	ret = rte_hash_lookup_data(drives_stats.h, (void*) &key, (void**) &data);
	if (unlikely(ret == -EINVAL)) {
		SPDK_ERRLOG("Drive statistics seems broken\n");
	} else if (unlikely(ret == -ENOENT)) {
		SPDK_NOTICELOG("Creating drive stats for key: %" PRIx64 "\n", key);
		data = spdk_io_pacer_drive_stats_create(&drives_stats, key, bdev);
	}
}

