/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/dma.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/dma.h"
#include "spdk/tree.h"
#include "spdk/accel_module.h"

#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/sgl.h"
#include "accel_mlx5.h"

#include <infiniband/mlx5dv.h>
#include <rdma/rdma_cma.h>

#define ACCEL_MLX5_QP_SIZE (64u)
#define ACCEL_MLX5_CQ_SIZE (1024u)
#define ACCEL_MLX5_NUM_MKEYS (2048u)

#define ACCEL_MLX5_MAX_SGE (16u)
#define ACCEL_MLX5_MAX_WC (32u)
#define ACCEL_MLX5_MAX_MKEYS_IN_TASK (16u)

#define ACCEL_MLX5_RECOVER_POLLER_PERIOD_US (10000)

struct accel_mlx5_iov_sgl {
	struct iovec	*iov;
	uint32_t	iovcnt;
	uint32_t        iov_offset;
};

struct accel_mlx5_qp;
struct accel_mlx5_io_channel;
struct accel_mlx5_task;

RB_HEAD(accel_mlx5_qpairs_map, accel_mlx5_qp);

enum accel_mlx5_opcode {
	ACCEL_MLX5_OPC_COPY,
	ACCEL_MLX5_OPC_CRYPTO,
	ACCEL_MLX5_OPC_ENCRYPT_MKEY,
	ACCEL_MLX5_OPC_DECRYPT_MKEY,
	ACCEL_MLX5_OPC_CRC32C,
	ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C,
	ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT,
	ACCEL_MLX5_OPC_LAST
};

struct accel_mlx5_stats {
	uint64_t crypto_umrs;
	uint64_t sig_umrs;
	uint64_t sig_crypto_umrs;
	uint64_t rdma_reads;
	uint64_t rdma_writes;
	uint64_t polls;
	uint64_t idle_polls;
	uint64_t completions;
	uint64_t nomem_qdepth;
	uint64_t nomem_mkey;
	uint64_t opcodes[ACCEL_MLX5_OPC_LAST];
};

struct accel_mlx5_dev_ctx {
	struct spdk_mempool *psv_pool;
	struct spdk_mlx5_psv **psvs;
	uint32_t *crc_dma_buf;
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct spdk_rdma_utils_memory_domain *domain;
	struct spdk_rdma_utils_mem_map *map;
	uint32_t num_mkeys;
	bool crypto_mkeys;
	bool sig_mkeys;
	bool crypto_sig_mkeys;
	bool crypto_multi_block;
};

struct accel_mlx5_module {
	struct spdk_accel_module_if module;
	struct accel_mlx5_dev_ctx *devices;
	struct accel_mlx5_stats stats;
	struct spdk_spinlock lock;
	uint32_t num_devs;
	uint16_t qp_size;
	uint16_t cq_size;
	uint32_t num_requests;
	uint32_t split_mb_blocks;
	bool siglast;
	bool qp_per_domain;
	/* copy of user input to make dump config easier */
	char *allowed_devs_str;
	char **allowed_devs;
	size_t allowed_devs_count;
	bool enabled;
	bool crypto_supported;
	bool crc_supported;
	bool merge;
	bool initialized;
	bool enable_driver;
};

struct accel_mlx5_klm {
	uint32_t src_klm_count;
	uint32_t dst_klm_count;
	struct mlx5_wqe_data_seg src_klm[ACCEL_MLX5_MAX_SGE];
	struct mlx5_wqe_data_seg dst_klm[ACCEL_MLX5_MAX_SGE];
};

struct accel_mlx5_psv_wrapper {
	uint32_t psv_index;
	struct {
		uint32_t error : 1;
		uint32_t reserved : 31;
	} bits;
	uint32_t *crc;
	uint32_t crc_lkey;
};

struct accel_mlx5_task {
	struct spdk_accel_task base;
	struct accel_mlx5_iov_sgl src;
	struct accel_mlx5_iov_sgl dst;
	STAILQ_ENTRY(accel_mlx5_task) link;
	struct accel_mlx5_qp *qp;
	struct accel_mlx5_psv_wrapper *psv;
	union {
		/* The struct is used for crypto */
		struct {
			uint32_t dek_obj_id;
			/* Number of data blocks per crypto operation */
			uint16_t blocks_per_req;
			/* total num_blocks in this task */
			uint16_t num_blocks;
			uint16_t num_processed_blocks;
			uint8_t tweak_mode;
		};
		/* The struct is used for crc/check_crc */
		struct {
			uint32_t last_umr_len;
			uint16_t last_mkey_idx;
		};
	};
	union {
		struct {
			uint32_t *crc;
			uint32_t seed;
		} encrypt_crc;
		struct {
			uint64_t iv;
			uint16_t block_size;
		} crc_decrypt;
	};
	union {
		uint8_t raw;
		struct {
			uint8_t inplace : 1;
			/* Set if the task is executed as a part of the previous task. */
			uint8_t merged : 1;
			/* This task is handled by mlx5 driver */
			uint8_t driver_seq : 1;
			/* The user requesteed to register mkey, without DMA operation */
			uint8_t driver_mkey : 1;
			/* If set, memory data will be encrypted during TX and wire data will be
			 decrypted during RX.
			 If not set, memory data will be decrypted during TX and wire data will
			 be encrypted during RX. */
			uint8_t enc_order : 2;
			uint8_t reserved : 2;
		} bits;
	} flags;
	uint8_t mlx5_opcode;

	uint16_t num_reqs;
	uint16_t num_completed_reqs;
	uint16_t num_submitted_reqs;
	uint16_t num_wrs;
	/* for crypto op - number of allocated mkeys
	 * for crypto and copy - number of operations allowed to be submitted to qp */
	uint16_t num_ops;
	/* Keep this array last since not all elements might be accessed, this reduces amount of data to be
	 * cached */
	struct spdk_mlx5_mkey_pool_obj *mkeys[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
};

struct accel_mlx5_qp {
	struct spdk_mlx5_qp *qp;
	struct accel_mlx5_dev *dev;
	RB_ENTRY(accel_mlx5_qp) node;
	/* Memory domain which this qpair serves */
	struct spdk_memory_domain *domain;
	uint16_t wrs_submitted;
	uint16_t max_wrs;
	bool recovering;
	/* tasks submitted to HW. We can't complete a task even in error case until we reap completions for all
	 * submitted requests */
	STAILQ_HEAD(, accel_mlx5_task) in_hw;
	struct spdk_poller *recover_poller;
};

struct accel_mlx5_dev {
	struct spdk_mlx5_cq *cq;
	struct accel_mlx5_qp mlx5_qp;
	/* Points to a map owned by dev_ctx */
	struct spdk_rdma_utils_mem_map *map_ref;
	struct accel_mlx5_qpairs_map qpairs_map;
	void *crypto_mkeys;
	void *sig_mkeys;
	void *crypto_sig_mkeys;
	/* Points to a pool owned by dev_ctx */
	struct spdk_mempool *psv_pool_ref;
	/* Points to a PD owned by dev_ctx */
	struct ibv_pd *pd_ref;
	/* Points to a memory domain owned by dev_ctx */
	struct spdk_memory_domain *domain_ref;
	/* IO channel this dev belongs to. The same channel is used by platform driver */
	struct spdk_io_channel *ch;
	/* Pending tasks waiting for requests resources */
	STAILQ_HEAD(, accel_mlx5_task) nomem;
	uint16_t wrs_in_cq;
	uint16_t max_wrs_in_cq;
	bool crypto_multi_block;
	struct accel_mlx5_stats stats;
};

struct accel_mlx5_io_channel {
	struct accel_mlx5_dev *devs;
	struct spdk_poller *poller;
	uint32_t num_devs;
	/* Index in \b devs to be used for round-robin */
	uint32_t dev_idx;
	bool pp_handler_registered;
};

struct accel_mlx5_psv_pool_iter_cb_args {
	struct accel_mlx5_dev_ctx *dev;
	int rc;
};

struct accel_mlx5_dump_stats_ctx {
	struct accel_mlx5_stats total;
	struct spdk_json_write_ctx *w;
	enum accel_mlx5_dump_state_level level;
	accel_mlx5_dump_stat_done_cb cb;
	void *ctx;
};

struct accel_mlx5_task_ops {
	int (*init)(struct accel_mlx5_task *task);
	int (*process)(struct accel_mlx5_task *task);
	int (*cont)(struct accel_mlx5_task *task);
	void (*complete)(struct accel_mlx5_task *task);
};

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, task)	\
do {							\
	assert((qp)->wrs_submitted < (qp)->max_wrs);	\
	(qp)->wrs_submitted++;				\
	(task)->num_wrs++;				\
} while (0)

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, task)	\
do {									\
	assert((dev)->wrs_in_cq < (dev)->max_wrs_in_cq);		\
	(dev)->wrs_in_cq++;						\
        assert((qp)->wrs_submitted < (qp)->max_wrs);			\
	(qp)->wrs_submitted++;						\
	(task)->num_wrs++;						\
} while (0)

static struct accel_mlx5_task_ops g_accel_mlx5_tasks_ops[];
static struct spdk_accel_driver g_accel_mlx5_driver;

static int accel_mlx5_create_qp(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp);
static inline int accel_mlx5_execute_sequence(struct spdk_io_channel *ch,
		struct spdk_accel_sequence *seq);
static void accel_mlx5_pp_handler(void *ctx);

static int
accel_mlx5_qpair_compare(struct accel_mlx5_qp *qp1, struct accel_mlx5_qp *qp2)
{
	return (uint64_t)qp1->domain < (uint64_t)qp2->domain ? -1 : (uint64_t)qp1->domain >
	       (uint64_t)qp2->domain;
}

RB_GENERATE_STATIC(accel_mlx5_qpairs_map, accel_mlx5_qp, node, accel_mlx5_qpair_compare);

static struct accel_mlx5_module g_accel_mlx5;
static void(*g_accel_mlx5_process_cpl_fn)(struct accel_mlx5_dev *dev,
		struct spdk_mlx5_cq_completion *wc, int reaped);

static inline void
accel_mlx5_iov_sgl_init(struct accel_mlx5_iov_sgl *s, struct iovec *iov, int iovcnt)
{
	s->iov = iov;
	s->iovcnt = iovcnt;
	s->iov_offset = 0;
}

static inline void
accel_mlx5_iov_sgl_advance(struct accel_mlx5_iov_sgl *s, uint32_t step)
{
	s->iov_offset += step;
	while (s->iovcnt > 0) {
		assert(s->iov != NULL);
		if (s->iov_offset < s->iov->iov_len) {
			break;
		}

		s->iov_offset -= s->iov->iov_len;
		s->iov++;
		s->iovcnt--;
	}
}

static inline void
accel_mlx5_iov_sgl_unwind(struct accel_mlx5_iov_sgl *s, uint32_t max_iovs, uint32_t step)
{
	SPDK_DEBUGLOG(accel_mlx5, "iov %p, iovcnt %u, max %u, offset %u, step %u\n", s->iov, s->iovcnt,
		      max_iovs, s->iov_offset, step);
	while (s->iovcnt <= max_iovs) {
		assert(s->iov != NULL);
		if (s->iov_offset >= step) {
			s->iov_offset -= step;
			SPDK_DEBUGLOG(accel_mlx5, "\tEND, iov %p, iovcnt %u, offset %u\n", s->iov, s->iovcnt,
				      s->iov_offset);
			return;
		}
		step -= s->iov_offset;
		s->iov--;
		s->iovcnt++;
		s->iov_offset = s->iov->iov_len;
		SPDK_DEBUGLOG(accel_mlx5, "\tiov %p, iovcnt %u, offset %u, step %u\n", s->iov, s->iovcnt,
			      s->iov_offset,
			      step);
	}

	SPDK_ERRLOG("Can't unwind iovs, remaining  %u\n", step);
	assert(0);
}

static void
accel_mlx5_add_stats(struct accel_mlx5_stats *stats, const struct accel_mlx5_stats *to_add)
{
	int i;

	stats->crypto_umrs += to_add->crypto_umrs;
	stats->sig_umrs += to_add->sig_umrs;
	stats->sig_crypto_umrs += to_add->sig_crypto_umrs;
	stats->rdma_reads += to_add->rdma_reads;
	stats->rdma_writes += to_add->rdma_writes;
	stats->polls += to_add->polls;
	stats->idle_polls += to_add->idle_polls;
	stats->completions += to_add->completions;
	stats->nomem_qdepth += to_add->nomem_qdepth;
	stats->nomem_mkey += to_add->nomem_mkey;
	for (i = 0; i < ACCEL_MLX5_OPC_LAST; i++) {
		stats->opcodes[i] += to_add->opcodes[i];
	}
}

static inline int
accel_mlx5_task_check_sigerr(struct accel_mlx5_task *task)
{
	unsigned i;
	int rc;

	assert(task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C);

	rc = 0;
	for (i = 0; i < task->num_ops; i++) {
		if (task->mkeys[i]->sig.sigerr) {
			task->mkeys[i]->sig.sigerr = false;
			rc = -EIO;
		}
	}

	if (spdk_unlikely(rc)) {
		task->psv->bits.error = 1;
	}

	return rc;
}

static inline uint16_t
accel_mlx5_dev_get_available_slots(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp)
{
	uint16_t qp_slot;
	uint16_t cq_slot;

	assert(qp->max_wrs >= qp->wrs_submitted);
	assert(dev->max_wrs_in_cq >= dev->wrs_in_cq);

	qp_slot = qp->max_wrs - qp->wrs_submitted;
	cq_slot = dev->max_wrs_in_cq - dev->wrs_in_cq;

	return spdk_min(qp_slot, cq_slot);
}

static inline void
accel_mlx5_copy_task_complete(struct accel_mlx5_task *mlx5_task)
{
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline void
accel_mlx5_crypto_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	assert(mlx5_task->num_processed_blocks == mlx5_task->num_blocks);
	spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, mlx5_task->mkeys, mlx5_task->num_ops);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline void
accel_mlx5_crypto_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_mlx5_driver_io_context *driver_ctx;

	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	assert(mlx5_task->num_processed_blocks == mlx5_task->num_blocks);
	assert(mlx5_task->base.seq);
	driver_ctx = spdk_accel_sequence_get_driver_ctx(mlx5_task->base.seq);
	assert(driver_ctx);
	driver_ctx->mkey = mlx5_task->mkeys[0];
	mlx5_task->flags.bits.driver_mkey = 0;

	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline void
accel_mlx5_crc_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int sigerr = 0;

	if (mlx5_task->base.op_code != SPDK_ACCEL_OPC_CHECK_CRC32C) {
		*mlx5_task->base.crc_dst = *mlx5_task->psv->crc ^ UINT32_MAX;
	} else {
		sigerr = accel_mlx5_task_check_sigerr(mlx5_task);
	}
	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, mlx5_task->mkeys, mlx5_task->num_ops);
	spdk_mempool_put(dev->psv_pool_ref, mlx5_task->psv);
	spdk_accel_task_complete(&mlx5_task->base, sigerr);
}

static inline void
accel_mlx5_encrypt_crc_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	*mlx5_task->encrypt_crc.crc = *mlx5_task->psv->crc ^ UINT32_MAX;
	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->crypto_sig_mkeys, mlx5_task->mkeys, mlx5_task->num_ops);
	spdk_mempool_put(dev->psv_pool_ref, mlx5_task->psv);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline void
accel_mlx5_crc_decrypt_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int sigerr = 0;

	assert(mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C);
	sigerr = accel_mlx5_task_check_sigerr(mlx5_task);

	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->crypto_sig_mkeys, mlx5_task->mkeys, mlx5_task->num_ops);
	spdk_mempool_put(dev->psv_pool_ref, mlx5_task->psv);
	spdk_accel_task_complete(&mlx5_task->base, sigerr);
}

static inline void
accel_mlx5_task_complete(struct accel_mlx5_task *task)
{
	struct spdk_accel_sequence *seq = task->base.seq;
	struct spdk_io_channel *ch = task->qp->dev->ch;
	struct spdk_accel_task *next;
	bool driver_seq;

	SPDK_DEBUGLOG(accel_mlx5, "Complete task %p, opc %d\n", task, task->base.op_code);

	next = spdk_accel_sequence_next_task(&task->base);
	driver_seq = task->flags.bits.driver_seq;
	task->flags.bits.driver_seq = 0;

	g_accel_mlx5_tasks_ops[task->mlx5_opcode].complete(task);

	if (driver_seq) {
		assert(seq);

		if (next) {
			accel_mlx5_execute_sequence(ch, seq);
		} else {
			spdk_accel_sequence_continue(seq);
		}
	}
}

static inline void
accel_mlx5_task_fail(struct accel_mlx5_task *task, int rc)
{
	struct accel_mlx5_dev *dev = task->qp->dev;

	assert(rc);
	SPDK_DEBUGLOG(accel_mlx5, "Fail task %p, opc %d, rc %d\n", task, task->base.op_code, rc);

	if (task->num_ops) {
		if (task->mlx5_opcode == ACCEL_MLX5_OPC_CRYPTO) {
			spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
		}
		if (task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C ||
		    task->mlx5_opcode == ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C ||
		    task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT) {
			spdk_mlx5_mkey_pool_put_bulk(dev->crypto_sig_mkeys, task->mkeys, task->num_ops);
			spdk_mempool_put(dev->psv_pool_ref, task->psv);
		}
	}
	spdk_accel_task_complete(&task->base, rc);
}

static int
accel_mlx5_translate_addr(void *addr, size_t size, struct spdk_memory_domain *domain,
			  void *domain_ctx,
			  struct accel_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm)
{
	struct spdk_rdma_utils_memory_translation map_translation;
	struct spdk_memory_domain_translation_result domain_translation;
	struct spdk_memory_domain_translation_ctx local_ctx;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;

	if (domain) {
		domain_translation.size = sizeof(struct spdk_memory_domain_translation_result);
		local_ctx.size = sizeof(local_ctx);
		local_ctx.rdma.ibv_qp = qp->qp->verbs_qp;
		rc = spdk_memory_domain_translate_data(domain, domain_ctx, dev->domain_ref,
						       &local_ctx, addr, size, &domain_translation);
		if (spdk_unlikely(rc || domain_translation.iov_count != 1)) {
			SPDK_ERRLOG("Memory domain translation failed, addr %p, length %zu\n", addr, size);
			if (rc == 0) {
				rc = -EINVAL;
			}

			return rc;
		}
		klm->lkey = domain_translation.rdma.lkey;
		klm->addr = (uint64_t) domain_translation.iov.iov_base;
		klm->byte_count = domain_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_utils_get_translation(dev->map_ref, addr, size,
						     &map_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Memory translation failed, addr %p, length %zu\n", addr, size);
			return rc;
		}
		klm->lkey = spdk_rdma_utils_memory_translation_get_lkey(&map_translation);
		klm->addr = (uint64_t)addr;
		klm->byte_count = size;
	}

	return 0;
}

static inline int
accel_mlx5_klm_unwind(struct mlx5_wqe_data_seg *klm, uint32_t klm_count, uint32_t step)
{
	int i;

	assert(klm_count > 0);
	SPDK_DEBUGLOG(accel_mlx5, "klm %p, count %u, step %u\n", klm, klm_count, step);
	for (i = (int)klm_count - 1; i >= 0; i--) {
		if (klm[i].byte_count > step) {
			klm[i].byte_count -= step;
			SPDK_DEBUGLOG(accel_mlx5, "\tklm[%u] len %u, step %u\n", i, klm[i].byte_count, step);
			return (int)i + 1;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\tklm[%u] len %u, step %u\n", i, klm[i].byte_count, step);
		step -= klm[i].byte_count;
	}

	SPDK_ERRLOG("Can't unwind KLM, remaining  %u\n", step);
	assert(step == 0);

	return 0;
}

static inline int
accel_mlx5_fill_block_sge(struct accel_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm,
			  struct accel_mlx5_iov_sgl *iovs, struct spdk_memory_domain *domain, void *domain_ctx,
			  uint32_t lkey, uint32_t block_len, uint32_t *_remaining)
{
	void *addr;
	uint32_t remaining;
	uint32_t size;
	int i = 0;
	int rc;

	remaining = block_len;
	*_remaining = block_len;

	while (remaining && i < (int)ACCEL_MLX5_MAX_SGE) {
		size = spdk_min(remaining, iovs->iov->iov_len - iovs->iov_offset);
		addr = (void *)iovs->iov->iov_base + iovs->iov_offset;
		if (!lkey) {
			/* No pre-translated lkey */
			rc = accel_mlx5_translate_addr(addr, size, domain, domain_ctx, qp, &klm[i]);
			if (spdk_unlikely(rc)) {
				return rc;
			}
		} else {
			klm[i].lkey = lkey;
			klm[i].addr = (uint64_t) addr;
			klm[i].byte_count = size;
		}

		SPDK_DEBUGLOG(accel_mlx5, "\t klm[%d] lkey %u, addr %p, len %u\n", i, klm[i].lkey,
			      (void *)klm[i].addr, klm[i].byte_count);
		accel_mlx5_iov_sgl_advance(iovs, size);
		i++;
		assert(remaining >= size);
		remaining -= size;
	}
	*_remaining = remaining;

	return i;
}

static inline bool
accel_mlx5_compare_iovs(struct iovec *v1, struct iovec *v2, uint32_t iovcnt)
{
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		if (v1[i].iov_base != v2[i].iov_base || v1[i].iov_len != v2[i].iov_len) {
			return false;
		}
	}

	return true;
}

static inline void
accel_mlx5_dev_nomem_task_qdepth(struct accel_mlx5_dev *dev, struct accel_mlx5_task *task)
{
	SPDK_DEBUGLOG(accel_mlx5, "queue task %p, dev %p\n", task, dev);
	dev->stats.nomem_qdepth++;
	STAILQ_INSERT_TAIL(&dev->nomem, task, link);
}

static inline void
accel_mlx5_dev_nomem_task_mkey(struct accel_mlx5_dev *dev, struct accel_mlx5_task *task)
{
	SPDK_DEBUGLOG(accel_mlx5, "queue task %p, dev %p\n", task, dev);
	dev->stats.nomem_mkey++;
	STAILQ_INSERT_TAIL(&dev->nomem, task, link);
}

static inline int
accel_mlx5_task_alloc_mkeys(struct accel_mlx5_task *task, void *mkey_pool)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct  accel_mlx5_dev *dev = qp->dev;
	/* Each request consists of UMR and RDMA, or 2 operations.
	 * qp slot is the total number of operations available in qp */
	uint32_t num_ops = (task->num_reqs - task->num_completed_reqs) * 2;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint32_t num_mkeys;
	int rc;

	assert(task->num_reqs >= task->num_completed_reqs);
	assert(task->mlx5_opcode != ACCEL_MLX5_OPC_COPY);
	num_ops = spdk_min(num_ops, qp_slot);
	num_ops = spdk_min(num_ops, ACCEL_MLX5_MAX_MKEYS_IN_TASK * 2);
	if (spdk_unlikely(num_ops < 2)) {
		/* We must do at least 1 UMR and 1 RDMA operation */
		task->num_ops = 0;
		accel_mlx5_dev_nomem_task_qdepth(qp->dev, task);
		return -ENOMEM;
	}
	num_mkeys = num_ops / 2;
	rc = spdk_mlx5_mkey_pool_get_bulk(mkey_pool, task->mkeys, num_mkeys);
	if (spdk_unlikely(rc)) {
		task->num_ops = 0;
		accel_mlx5_dev_nomem_task_mkey(qp->dev, task);
		return -ENOMEM;
	}
	task->num_ops = num_mkeys;

	return 0;
}

static inline uint8_t
bs_to_bs_selector(uint32_t bs)
{
	switch (bs) {
	case 512:
		return 1;
	case 520:
		return 2;
	case 4048:
		return 6;
	case 4096:
		return 3;
	case 4160:
		return 4;
	default:
		return 0;
	}
}

static inline int
accel_mlx5_copy_task_process_one(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_qp *qp,
				 uint64_t wrid, uint32_t fence)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_klm klm;
	uint32_t remaining;
	uint32_t dst_len;
	int rc;

	/* Limit one RDMA_WRITE by length of dst buffer. Not all src buffers may fit into one dst buffer due to
	 * limitation on ACCEL_MLX5_MAX_SGE. If this is the case then remaining is not zero */
	assert(mlx5_task->dst.iov->iov_len > mlx5_task->dst.iov_offset);
	dst_len = mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset;
	rc = accel_mlx5_fill_block_sge(qp, klm.src_klm, &mlx5_task->src, task->src_domain,
				       task->src_domain_ctx, 0, dst_len, &remaining);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	klm.src_klm_count = rc;
	assert(dst_len > remaining);
	dst_len -= remaining;

	rc = accel_mlx5_fill_block_sge(qp, klm.dst_klm, &mlx5_task->dst, task->dst_domain,
				       task->dst_domain_ctx, 0, dst_len,  &remaining);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
		return rc;
	}
	if (spdk_unlikely(remaining)) {
		SPDK_ERRLOG("something wrong\n");
		abort();
	}
	klm.dst_klm_count = rc;

	rc = spdk_mlx5_qp_rdma_write(mlx5_task->qp->qp, klm.src_klm, klm.src_klm_count,
				     klm.dst_klm[0].addr, klm.dst_klm[0].lkey, wrid, fence);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("new RDMA WRITE failed with %d\n", rc);
		return rc;
	}

	return 0;
}

static inline int
accel_mlx5_copy_task_process(struct accel_mlx5_task *mlx5_task)
{

	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t i;
	int rc;

	mlx5_task->num_wrs = 0;
	assert(mlx5_task->num_reqs > 0);
	assert(mlx5_task->num_ops > 0);

	/* Handle n-1 reqs in order to simplify wrid and fence handling */
	for (i = 0; i < mlx5_task->num_ops - 1; i++) {
		rc = accel_mlx5_copy_task_process_one(mlx5_task, qp, 0, 0);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		dev->stats.rdma_writes++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		mlx5_task->num_submitted_reqs++;
	}

	rc = accel_mlx5_copy_task_process_one(mlx5_task, qp, (uint64_t)mlx5_task,
					      SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	dev->stats.rdma_writes++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	mlx5_task->num_submitted_reqs++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, copy task, %p\n", mlx5_task);

	return 0;
}

static inline int
accel_mlx5_configure_crypto_umr(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_qp *qp,
				struct accel_mlx5_klm *klm,
				uint32_t dv_mkey, uint32_t src_lkey, uint32_t dst_lkey,
				uint32_t num_blocks, uint64_t wrid, uint32_t flags)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_mlx5_umr_attr umr_attr;
	uint32_t block_size, length, remaining;
	int rc;

	block_size = task->block_size;
	length = num_blocks * block_size;
	SPDK_DEBUGLOG(accel_mlx5, "task %p, src KLM, domain %p, lkey %u, len %u\n", task, task->src_domain,
		      src_lkey, length);
	rc = accel_mlx5_fill_block_sge(qp, klm->src_klm, &mlx5_task->src, task->src_domain,
				       task->src_domain_ctx, src_lkey, length, &remaining);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	klm->src_klm_count = rc;
	if (spdk_unlikely(remaining)) {
		uint32_t new_len = length - remaining;
		uint32_t aligned_len, updated_num_blocks;

		SPDK_DEBUGLOG(accel_mlx5, "Incorrect src iovs, handled %u out of %u bytes\n", new_len, length);
		if (new_len < block_size) {
			/* We need to process at least 1 block. If buffer is too fragmented, we can't do
			 * anything */
			return -ERANGE;
		}

		/* Regular integer division, we need to round down to prev block size */
		updated_num_blocks = new_len / block_size;
		assert(updated_num_blocks);
		assert(updated_num_blocks < num_blocks);
		aligned_len = updated_num_blocks * block_size;

		if (aligned_len < new_len) {
			uint32_t dt = new_len - aligned_len;

			/* We can't process part of block, need to unwind src iov_sgl and klm to the
			 * prev block boundary */
			SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind src KLM for %u bytes\n", task, dt);
			accel_mlx5_iov_sgl_unwind(&mlx5_task->src, task->s.iovcnt, dt);
			klm->src_klm_count = accel_mlx5_klm_unwind(klm->src_klm, klm->src_klm_count, dt);
			if (!klm->src_klm_count) {
				return -ERANGE;
			}
		}
		SPDK_DEBUGLOG(accel_mlx5, "task %p, UMR len %u -> %u\n", task, length, aligned_len);
		length = aligned_len;
		num_blocks = updated_num_blocks;
	}
	cattr.xts_iv = task->iv + mlx5_task->num_processed_blocks;
	cattr.keytag = 0;
	cattr.dek_obj_id = mlx5_task->dek_obj_id;
	cattr.tweak_mode = mlx5_task->tweak_mode;
	cattr.enc_order = mlx5_task->flags.bits.enc_order;
	cattr.bs_selector = bs_to_bs_selector(block_size);
	if (spdk_unlikely(!cattr.bs_selector)) {
		SPDK_ERRLOG("unsupported block size %u\n", block_size);
		return -EINVAL;
	}
	umr_attr.dv_mkey = dv_mkey;
	umr_attr.klm = klm->src_klm;

	if (!mlx5_task->flags.bits.inplace) {
		SPDK_DEBUGLOG(accel_mlx5, "task %p, dst KLM, domain %p, lkey %u, len %u\n", task, task->dst_domain,
			      dst_lkey, length);
		rc = accel_mlx5_fill_block_sge(qp, klm->dst_klm, &mlx5_task->dst, task->dst_domain,
					       task->dst_domain_ctx, dst_lkey, length, &remaining);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		klm->dst_klm_count = rc;
		if (spdk_unlikely(remaining)) {
			uint32_t new_len = length - remaining;
			uint32_t aligned_len, updated_num_blocks, dt;

			SPDK_DEBUGLOG(accel_mlx5, "Incorrect dst iovs, handled %u out of %u bytes\n", new_len, length);
			if (new_len < block_size) {
				/* We need to process at least 1 block. If buffer is too fragmented, we can't do
				 * anything */
				return -ERANGE;
			}

			/* Regular integer division, we need to round down to prev block size */
			updated_num_blocks = new_len / block_size;
			assert(updated_num_blocks);
			assert(updated_num_blocks < num_blocks);
			aligned_len = updated_num_blocks * block_size;

			if (aligned_len < new_len) {
				dt = new_len - aligned_len;
				assert(dt > 0 && dt < length);
				/* We can't process part of block, need to unwind src and dst iov_sgl and klm to the
				 * prev block boundary */
				SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind dst KLM for %u bytes\n", task, dt);
				accel_mlx5_iov_sgl_unwind(&mlx5_task->dst, task->d.iovcnt, dt);
				klm->dst_klm_count = accel_mlx5_klm_unwind(klm->dst_klm, klm->dst_klm_count, dt);
				assert(klm->dst_klm_count > 0 && klm->dst_klm_count <= ACCEL_MLX5_MAX_SGE);
				if (!klm->dst_klm_count) {
					return -ERANGE;
				}
			}
			assert(length > aligned_len);
			dt = length - aligned_len;
			SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind src KLM for %u bytes\n", task, dt);
			/* The same for src iov_sgl and KLM. In worst case we can unwind SRC 2 times */
			accel_mlx5_iov_sgl_unwind(&mlx5_task->src, task->s.iovcnt, dt);
			klm->src_klm_count = accel_mlx5_klm_unwind(klm->src_klm, klm->src_klm_count, dt);
			assert(klm->src_klm_count > 0 && klm->src_klm_count <= ACCEL_MLX5_MAX_SGE);
			if (!klm->src_klm_count) {
				return -ERANGE;
			}
			SPDK_DEBUGLOG(accel_mlx5, "task %p, UMR len %u -> %u\n", task, length, aligned_len);
			length = aligned_len;
			num_blocks = updated_num_blocks;
		}
	}
	SPDK_DEBUGLOG(accel_mlx5,
		      "task %p: bs %u, iv %"PRIu64", enc_on_tx %d, tweak_mode %d, len %u, dv_mkey %x, blocks %u\n",
		      mlx5_task, task->block_size, cattr.xts_iv, mlx5_task->flags.bits.enc_order, cattr.tweak_mode,
		      length, dv_mkey, num_blocks);

	umr_attr.klm_count = klm->src_klm_count;
	umr_attr.umr_len = length;
	mlx5_task->num_processed_blocks += num_blocks;

	rc = spdk_mlx5_umr_configure_crypto(qp->qp, &umr_attr, &cattr, wrid, flags);

	return rc;
}

static inline int
accel_mlx5_task_pretranslate_single_mkey(struct accel_mlx5_task *mlx5_task,
		struct accel_mlx5_qp *qp,
		struct iovec *base_addr, struct spdk_memory_domain *domain,
		void *domain_ctx, uint32_t *mkey)
{
	struct mlx5_wqe_data_seg klm;
	struct spdk_accel_task *task = &mlx5_task->base;
	int rc;

	if (task->cached_lkey == NULL || *task->cached_lkey == 0 || !domain) {
		rc = accel_mlx5_translate_addr(base_addr->iov_base, base_addr->iov_len,
					       domain, domain_ctx, qp, &klm);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		if (task->cached_lkey && domain) {
			*task->cached_lkey = klm.lkey;
		}
		*mkey = klm.lkey;
	} else {
		*mkey = *task->cached_lkey;
	}

	return 0;
}

static inline int
accel_mlx5_task_pretranslate_mkeys(struct accel_mlx5_task *mlx5_task, size_t op_len,
				   uint32_t *src_lkey, uint32_t *dst_lkey)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	int rc = 0;

	if (op_len <= mlx5_task->src.iov->iov_len - mlx5_task->src.iov_offset || task->s.iovcnt == 1) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, mlx5_task->qp, &task->s.iovs[0],
				task->src_domain, task->src_domain_ctx, src_lkey);
	}
	if (!mlx5_task->flags.bits.inplace &&
	    (op_len <= mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset || task->d.iovcnt == 1)) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, mlx5_task->qp, &task->d.iovs[0],
				task->dst_domain, task->dst_domain_ctx, dst_lkey);
	}

	return rc;
}

static inline int
accel_mlx5_crypto_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_klm klms[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t src_lkey = 0, dst_lkey = 0;
	uint16_t i;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint32_t num_blocks;
	/* First RDMA after UMR must have a SMALL_FENCE */
	uint32_t first_rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	size_t ops_len = mlx5_task->blocks_per_req * num_ops;
	int rc;

	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	rc = accel_mlx5_task_pretranslate_mkeys(mlx5_task, ops_len, &src_lkey, &dst_lkey);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);
	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	for (i = 0; i < num_ops; i++) {
		if (mlx5_task->num_submitted_reqs + i + 1 == mlx5_task->num_reqs) {
			/* Last request may consume less than calculated */
			assert(mlx5_task->num_blocks > mlx5_task->num_processed_blocks);
			num_blocks = mlx5_task->num_blocks - mlx5_task->num_processed_blocks;
		} else {
			num_blocks = mlx5_task->blocks_per_req;
		}
		rc = accel_mlx5_configure_crypto_umr(mlx5_task, qp, &klms[i], mlx5_task->mkeys[i]->mkey,
						     src_lkey, dst_lkey, num_blocks, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		dev->stats.crypto_umrs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	for (i = 0; i < num_ops - 1; i++) {
		/* UMR is used as a destination for RDMA_READ - from UMR to klms
		 * XTS is applied on DPS */
		if (mlx5_task->flags.bits.inplace) {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, klms[i].src_klm,
						    klms[i].src_klm_count,
						    0, mlx5_task->mkeys[i]->mkey, 0,
						    first_rdma_fence);
		} else {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, klms[i].dst_klm,
						    klms[i].dst_klm_count,
						    0, mlx5_task->mkeys[i]->mkey, 0,
						    first_rdma_fence);
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
			return rc;
		}
		first_rdma_fence = 0;
		dev->stats.rdma_reads++;
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (mlx5_task->flags.bits.inplace) {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, klms[i].src_klm, klms[i].src_klm_count,
					    0, mlx5_task->mkeys[i]->mkey,
					    (uint64_t)mlx5_task, first_rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, klms[i].dst_klm, klms[i].dst_klm_count,
					    0, mlx5_task->mkeys[i]->mkey,
					    (uint64_t)mlx5_task, first_rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	}

	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA WRITE failed with %d\n", rc);
		return rc;
	}
	dev->stats.rdma_reads++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	if (spdk_unlikely(mlx5_task->num_submitted_reqs == mlx5_task->num_reqs &&
			  mlx5_task->num_blocks > mlx5_task->num_processed_blocks)) {
		/* We hit "out of KLM entries" case with highly fragmented payload. In that case
		 * accel_mlx5_configure_crypto_umr function handled fewer data blocks than expected
		 * That means we need at least 1 more request to complete this task, this request will be
		 * executed once all submitted ones are completed */
		SPDK_DEBUGLOG(accel_mlx5, "task %p, processed %u/%u blocks, add extra req\n", mlx5_task,
			      mlx5_task->num_processed_blocks, mlx5_task->num_blocks);
		mlx5_task->num_reqs++;
	}

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_encrypt_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_klm klm;
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t src_lkey = 0;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}

	mlx5_task->num_wrs = 0;
	if (task->s.iovcnt == 1) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, mlx5_task->qp, &task->s.iovs[0],
				task->src_domain, task->src_domain_ctx, &src_lkey);
	}

	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);
	rc = accel_mlx5_configure_crypto_umr(mlx5_task, qp, &klm, mlx5_task->mkeys[0]->mkey,
					     src_lkey, 0, mlx5_task->blocks_per_req,
					     (uint64_t)mlx5_task, SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.crypto_umrs++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_decrypt_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_klm klm;
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t dst_lkey = 0;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}

	mlx5_task->num_wrs = 0;
	if (task->d.iovcnt == 1) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, mlx5_task->qp, &task->d.iovs[0],
				task->dst_domain, task->dst_domain_ctx, &dst_lkey);
	}

	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);
	/* This is inplace task where mlx5_task::src poinst to task::d.iovs. dst_lkey describes mlx5_task::src,
	 * so it is passed to the function below as src_lkey parameter */
	rc = accel_mlx5_configure_crypto_umr(mlx5_task, qp, &klm, mlx5_task->mkeys[0]->mkey,
					     dst_lkey, 0, mlx5_task->blocks_per_req,
					     (uint64_t)mlx5_task, SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.crypto_umrs++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_configure_crypto_and_sig_umr(struct accel_mlx5_task *mlx5_task,
					struct accel_mlx5_qp *qp,
					struct accel_mlx5_klm *klm, struct spdk_mlx5_mkey_pool_obj *mkey,
					uint32_t src_lkey, uint32_t dst_lkey,
					enum spdk_mlx5_umr_sig_domain sig_domain, uint32_t psv_index,
					uint32_t *crc, uint32_t crc_seed, uint64_t iv, uint32_t block_size, uint32_t req_len,
					bool init_signature, bool gen_signature, bool encrypt)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_mlx5_umr_sig_attr sattr;
	struct spdk_mlx5_umr_attr umr_attr;
	uint32_t remaining;
	uint32_t umr_klm_count;
	int rc;

	assert(mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C ||
	       mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT);

	rc = accel_mlx5_fill_block_sge(qp, klm->src_klm, &mlx5_task->src, task->src_domain,
				       task->src_domain_ctx, src_lkey, req_len, &remaining);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	if (spdk_unlikely(remaining)) {
		SPDK_ERRLOG("Incorrect src iovs, handling not supported for crypto yet\n");
		abort();
	}
	umr_klm_count = klm->src_klm_count = rc;

	if (!mlx5_task->flags.bits.inplace) {
		rc = accel_mlx5_fill_block_sge(qp, klm->dst_klm, &mlx5_task->dst, task->dst_domain,
					       task->dst_domain_ctx, dst_lkey, req_len, &remaining);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		if (spdk_unlikely(remaining)) {
			SPDK_ERRLOG("Incorrect dst iovs, handling not supported for signature yet\n");
			abort();
		}
		klm->dst_klm_count = rc;
	}

	if (gen_signature && !encrypt) {
		/* Ensure that there is a free KLM */
		if (umr_klm_count >= ACCEL_MLX5_MAX_SGE) {
			SPDK_ERRLOG("No space left for crc_dst in klm\n");
			return -EINVAL;
		}

		*mlx5_task->psv->crc = *crc ^ UINT32_MAX;
		klm->src_klm[umr_klm_count].lkey = mlx5_task->psv->crc_lkey;
		klm->src_klm[umr_klm_count].addr = (uintptr_t)mlx5_task->psv->crc;
		klm->src_klm[umr_klm_count++].byte_count = sizeof(uint32_t);
	}

	SPDK_DEBUGLOG(accel_mlx5, "task %p crypto_attr: bs %u, iv %"PRIu64", enc_on_tx %d\n",
		      mlx5_task, block_size, iv, mlx5_task->flags.bits.enc_order);
	cattr.enc_order = mlx5_task->flags.bits.enc_order;
	cattr.bs_selector = bs_to_bs_selector(block_size);
	if (spdk_unlikely(!cattr.bs_selector)) {
		SPDK_ERRLOG("unsupported block size %u\n", block_size);
		return -EINVAL;
	}
	cattr.xts_iv = iv;
	cattr.keytag = 0;
	cattr.dek_obj_id = mlx5_task->dek_obj_id;
	cattr.tweak_mode = mlx5_task->tweak_mode;

	sattr.seed = crc_seed ^ UINT32_MAX;
	sattr.psv_index = psv_index;
	sattr.domain = sig_domain;
	sattr.sigerr_count = mkey->sig.sigerr_count;
	/* raw_data_size is a size of data without signature. */
	sattr.raw_data_size = req_len;
	sattr.init = init_signature;
	sattr.check_gen = gen_signature;

	umr_attr.dv_mkey = mkey->mkey;
	/*
	 * umr_len is the size of data addressed by MKey in memory and includes
	 * the size of the signature if it exists in memory.
	 */
	umr_attr.umr_len = encrypt ? req_len : req_len + sizeof(*crc);
	umr_attr.klm_count = umr_klm_count;
	umr_attr.klm = klm->src_klm;

	return spdk_mlx5_umr_configure_sig_crypto(qp->qp, &umr_attr, &sattr, &cattr, 0, 0);
}

static inline int
accel_mlx5_encrypt_and_crc_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_klm klms[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct spdk_accel_task *task;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint64_t iv;
	uint32_t src_lkey = 0, dst_lkey = 0;
	uint16_t i;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	uint32_t req_len;
	uint32_t blocks_processed;
	struct mlx5_wqe_data_seg *klm;
	uint32_t klm_count;
	size_t ops_len = mlx5_task->blocks_per_req * num_ops;
	bool init_signature = mlx5_task->num_submitted_reqs == 0;
	bool gen_signature = false;
	int rc = 0;

	task = &mlx5_task->base;
	assert(task);

	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	if (ops_len <= mlx5_task->src.iov->iov_len - mlx5_task->src.iov_offset ||
	    mlx5_task->src.iovcnt == 1) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, qp, mlx5_task->src.iov,
				task->src_domain, task->src_domain_ctx, &src_lkey);
	}
	if (!mlx5_task->flags.bits.inplace &&
	    (ops_len <= mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset ||
	     mlx5_task->dst.iovcnt == 1)) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, qp, mlx5_task->dst.iov,
				task->dst_domain, task->dst_domain_ctx, &dst_lkey);
	}
	if (spdk_unlikely(rc)) {
		return rc;
	}

	blocks_processed = mlx5_task->num_submitted_reqs * mlx5_task->blocks_per_req;
	iv = task->iv + blocks_processed;

	SPDK_DEBUGLOG(accel_mlx5,
		      "begin, encrypt_crc task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	for (i = 0; i < num_ops; i++) {
		gen_signature = false;
		if (mlx5_task->num_submitted_reqs + i + 1 == mlx5_task->num_reqs) {
			/* Last request may consume less than calculated */
			assert(mlx5_task->num_blocks > blocks_processed);
			req_len = (mlx5_task->num_blocks - blocks_processed) * task->block_size;
			gen_signature = true;
		} else {
			req_len = mlx5_task->blocks_per_req * task->block_size;
		}

		/*
		 * There is an HW limitation for the case when crypto and transactional signature are mixed in the same
		 * mkey. The HW only supports two following configurations in this case:
		 *
		 *   *  SX - encrypt-append (XTS first + transaction signature):
		 *      Mem (data) -> Wire sig(xts(data)). BSF.enc_order is encrypted_raw_wire.
		 *
		 *   *  SX - strip-decrypt (Sinature first + transaction signature):
		 *      Mem sig(xts(data)) -> Wire (data). Configuring signature on Wire is not allowed in this case.
		 *      BSF.enc_order is encrypted_raw_memory.
		 */
		rc = accel_mlx5_configure_crypto_and_sig_umr(mlx5_task, qp, &klms[i],
				mlx5_task->mkeys[i],
				src_lkey, dst_lkey,
				SPDK_MLX5_UMR_SIG_DOMAIN_WIRE,
				mlx5_task->psv->psv_index,
				mlx5_task->encrypt_crc.crc,
				mlx5_task->encrypt_crc.seed, iv, task->block_size, req_len,
				init_signature, gen_signature,
				true);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		init_signature = false;
		blocks_processed += mlx5_task->blocks_per_req;
		iv += mlx5_task->blocks_per_req;
		dev->stats.sig_crypto_umrs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		rc = spdk_mlx5_set_psv(qp->qp, mlx5_task->psv->psv_index, mlx5_task->encrypt_crc.seed, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	for (i = 0; i < num_ops - 1; i++) {
		/* UMR is used as a destination for RDMA_READ - from UMR to klms
		 * XTS is applied on DPS */
		if (mlx5_task->flags.bits.inplace) {
			klm = klms[i].src_klm;
			klm_count = klms[i].src_klm_count;
		} else {
			klm = klms[i].dst_klm;
			klm_count = klms[i].dst_klm_count;
		}
		rc = spdk_mlx5_qp_rdma_read(qp->qp, klm, klm_count, 0, mlx5_task->mkeys[i]->mkey, 0, rdma_fence);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ failed with %d\n", rc);
			return rc;
		}
		rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
		dev->stats.rdma_reads++;
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (mlx5_task->flags.bits.inplace) {
		klm = klms[i].src_klm;
		klm_count = klms[i].src_klm_count;
	} else {
		klm = klms[i].dst_klm;
		klm_count = klms[i].dst_klm_count;
	}

	/*
	 * TODO: Find a better solution and do not fail the task if klm_count == ACCEL_MLX5_MAX_SGE
	 *
	 * For now, the CRC offload feature is only used to calculate the data digest for write
	 * operations in the NVMe TCP initiator. Since one continues buffer is allocted for each IO
	 * in this case, klm_count is 1, and the below check does not fail.
	 */
	/* Last request, add crc_dst to the KLMs */
	if (mlx5_task->num_submitted_reqs + 1 == mlx5_task->num_reqs) {
		/* Ensure that there is a free KLM */
		if (klm_count >= ACCEL_MLX5_MAX_SGE) {
			SPDK_ERRLOG("No space left for crc_dst in klm\n");
			return -EINVAL;
		}

		klm[klm_count].lkey = mlx5_task->psv->crc_lkey;
		klm[klm_count].addr = (uintptr_t)mlx5_task->psv->crc;
		klm[klm_count++].byte_count = sizeof(uint32_t);
	}

	rc = spdk_mlx5_qp_rdma_read(qp->qp, klm, klm_count, 0, mlx5_task->mkeys[i]->mkey,
				    (uint64_t)mlx5_task, rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ failed with %d\n", rc);
		return rc;
	}
	dev->stats.rdma_reads++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5,
		      "end, encrypt_crc task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_crc_and_decrypt_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_klm klms[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct spdk_accel_task *task;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t src_lkey = 0, dst_lkey = 0;
	uint64_t iv;
	uint16_t i;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	uint32_t req_len;
	uint32_t blocks_processed;
	struct mlx5_wqe_data_seg *klm;
	uint32_t klm_count;
	size_t ops_len = mlx5_task->blocks_per_req * num_ops;
	bool init_signature = mlx5_task->num_submitted_reqs == 0;
	bool gen_signature = false;
	int rc = 0;

	assert(mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C);
	task = &mlx5_task->base;
	assert(task);

	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	if (ops_len <= mlx5_task->src.iov->iov_len - mlx5_task->src.iov_offset ||
	    mlx5_task->src.iovcnt == 1) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, qp, mlx5_task->src.iov,
				task->src_domain, task->src_domain_ctx, &src_lkey);
	}
	if (!mlx5_task->flags.bits.inplace &&
	    (ops_len <= mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset ||
	     mlx5_task->dst.iovcnt == 1)) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, qp, mlx5_task->dst.iov,
				task->dst_domain, task->dst_domain_ctx, &dst_lkey);
	}
	if (spdk_unlikely(rc)) {
		return rc;
	}

	blocks_processed = mlx5_task->num_submitted_reqs * mlx5_task->blocks_per_req;
	iv = mlx5_task->crc_decrypt.iv + blocks_processed;

	SPDK_DEBUGLOG(accel_mlx5,
		      "begin, crc_decrypt task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	for (i = 0; i < num_ops; i++) {
		gen_signature = false;
		if (mlx5_task->num_submitted_reqs + i + 1 == mlx5_task->num_reqs) {
			/* Last request may consume less than calculated */
			assert(mlx5_task->num_blocks > blocks_processed);
			req_len = (mlx5_task->num_blocks - blocks_processed) * mlx5_task->crc_decrypt.block_size;
			gen_signature = true;
		} else {
			req_len = mlx5_task->blocks_per_req * mlx5_task->crc_decrypt.block_size;
		}

		/*
		 * There is an HW limitation for the case when crypto and transactional signature are mixed in the same
		 * mkey. The HW only supports two following configurations in this case:
		 *
		 *   *  SX - encrypt-append (XTS first + transaction signature):
		 *      Mem (data) -> Wire sig(xts(data)). BSF.enc_order is encrypted_raw_wire.
		 *
		 *   *  SX - strip-decrypt (Sinature first + transaction signature):
		 *      Mem sig(xts(data)) -> Wire (data). Configuring signature on Wire is not allowed in this case.
		 *      BSF.enc_order is encrypted_raw_memory.
		 */
		rc = accel_mlx5_configure_crypto_and_sig_umr(mlx5_task, qp, &klms[i],
				mlx5_task->mkeys[i],
				src_lkey, dst_lkey,
				SPDK_MLX5_UMR_SIG_DOMAIN_MEMORY,
				mlx5_task->psv->psv_index,
				task->crc_dst,
				task->seed, iv, mlx5_task->crc_decrypt.block_size, req_len,
				init_signature, gen_signature,
				false);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		init_signature = false;
		blocks_processed += mlx5_task->blocks_per_req;
		iv += mlx5_task->blocks_per_req;
		dev->stats.sig_crypto_umrs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		rc = spdk_mlx5_set_psv(qp->qp, mlx5_task->psv->psv_index, task->seed, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	for (i = 0; i < num_ops - 1; i++) {
		/* UMR is used as a destination for RDMA_READ - from UMR to klms
		 * XTS is applied on DPS */
		if (mlx5_task->flags.bits.inplace) {
			klm = klms[i].src_klm;
			klm_count = klms[i].src_klm_count;
		} else {
			klm = klms[i].dst_klm;
			klm_count = klms[i].dst_klm_count;
		}
		rc = spdk_mlx5_qp_rdma_read(qp->qp, klm, klm_count, 0, mlx5_task->mkeys[i]->mkey, 0, rdma_fence);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ failed with %d\n", rc);
			return rc;
		}
		rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
		dev->stats.rdma_reads++;
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (mlx5_task->flags.bits.inplace) {
		klm = klms[i].src_klm;
		klm_count = klms[i].src_klm_count;
	} else {
		klm = klms[i].dst_klm;
		klm_count = klms[i].dst_klm_count;
	}

	rc = spdk_mlx5_qp_rdma_read(qp->qp, klm, klm_count, 0, mlx5_task->mkeys[i]->mkey,
				    (uint64_t)mlx5_task, rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ failed with %d\n", rc);
		return rc;
	}
	dev->stats.rdma_reads++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5,
		      "end, crc_decrypt task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_crc_task_configure_umr(struct accel_mlx5_task *mlx5_task, struct mlx5_wqe_data_seg *klm,
				  uint32_t klm_count, struct spdk_mlx5_mkey_pool_obj *mkey,
				  enum spdk_mlx5_umr_sig_domain sig_domain, uint32_t umr_len,
				  bool sig_init, bool sig_check_gen)
{
	struct spdk_mlx5_umr_sig_attr sattr = {
		.seed = mlx5_task->base.seed ^ UINT32_MAX,
		.psv_index = mlx5_task->psv->psv_index,
		.domain = sig_domain,
		.sigerr_count = mkey->sig.sigerr_count,
		.raw_data_size = umr_len,
		.init = sig_init,
		.check_gen = sig_check_gen,
	};
	struct spdk_mlx5_umr_attr umr_attr = {
		.dv_mkey = mkey->mkey,
		.umr_len = umr_len,
		.klm_count = klm_count,
		.klm = klm,
	};

	return spdk_mlx5_umr_configure_sig(mlx5_task->qp->qp, &umr_attr, &sattr, 0, 0);
}

static inline int
accel_mlx5_crc_task_fill_sge(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_klm *klm)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint32_t remaining;
	int rc;

	rc = accel_mlx5_fill_block_sge(mlx5_task->qp, klm->src_klm, &mlx5_task->src, task->src_domain,
				       task->src_domain_ctx, 0, task->nbytes, &remaining);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	klm->src_klm_count = rc;

	if (!mlx5_task->flags.bits.inplace) {
		rc = accel_mlx5_fill_block_sge(qp, klm->dst_klm, &mlx5_task->dst, task->dst_domain,
					       task->dst_domain_ctx, 0, task->nbytes, &remaining);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		assert(remaining == 0);
		klm->dst_klm_count = rc;
	}

	return 0;
}

static inline int
accel_mlx5_crc_task_process_one_req(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_klm klms;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
	bool check_op = mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C;
	struct mlx5_wqe_data_seg *klm;
	uint16_t klm_count;
	int rc;

	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	rc = accel_mlx5_crc_task_fill_sge(mlx5_task, &klms);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	rc = accel_mlx5_crc_task_configure_umr(mlx5_task, klms.src_klm, klms.src_klm_count,
					       mlx5_task->mkeys[0],
					       SPDK_MLX5_UMR_SIG_DOMAIN_WIRE, mlx5_task->base.nbytes, true, true);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.sig_umrs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);

	if (mlx5_task->flags.bits.inplace) {
		klm = klms.src_klm;
		klm_count = klms.src_klm_count;
	} else {
		klm = klms.dst_klm;
		klm_count = klms.dst_klm_count;
	}

	/*
	 * Add the crc destination to the end of KLMs. A free entry must be available for CRC
	 * because the task init function reserved it.
	 */
	assert(klm_count < ACCEL_MLX5_MAX_SGE);
	if (check_op) {
		*mlx5_task->psv->crc = *mlx5_task->base.crc_dst ^ UINT32_MAX;
	}
	klm[klm_count].lkey = mlx5_task->psv->crc_lkey;
	klm[klm_count].addr = (uintptr_t)mlx5_task->psv->crc;
	klm[klm_count++].byte_count = sizeof(uint32_t);

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		rc = spdk_mlx5_set_psv(qp->qp, mlx5_task->psv->psv_index, *mlx5_task->base.crc_dst, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (check_op) {
		/* Check with copy is not implemeted in this function */
		assert(mlx5_task->flags.bits.inplace);
		rc = spdk_mlx5_qp_rdma_write(qp->qp, klm, klm_count, 0, mlx5_task->mkeys[0]->mkey,
					     (uint64_t)mlx5_task, rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
		dev->stats.rdma_writes++;
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, klm, klm_count, 0, mlx5_task->mkeys[0]->mkey,
					    (uint64_t)mlx5_task, rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
		dev->stats.rdma_reads++;
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);

	return 0;
}

static inline int
accel_mlx5_crc_task_fill_umr_sge(struct accel_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm,
				 struct accel_mlx5_iov_sgl *umr_iovs, struct spdk_memory_domain *domain,
				 void *domain_ctx, struct accel_mlx5_iov_sgl *rdma_iovs, size_t *len)
{
	int umr_idx = 0;
	int rdma_idx = 0;
	int umr_iovcnt = spdk_min(umr_iovs->iovcnt, (int)ACCEL_MLX5_MAX_SGE);
	int rdma_iovcnt = spdk_min(umr_iovs->iovcnt, (int)ACCEL_MLX5_MAX_SGE);
	size_t umr_iov_offset;
	size_t rdma_iov_offset;
	size_t umr_len = 0;
	void *klm_addr;
	size_t klm_len;
	size_t umr_sge_len;
	size_t rdma_sge_len;
	int rc;

	umr_iov_offset = umr_iovs->iov_offset;
	rdma_iov_offset = rdma_iovs->iov_offset;

	while (umr_idx < umr_iovcnt && rdma_idx < rdma_iovcnt) {
		umr_sge_len = umr_iovs->iov[umr_idx].iov_len - umr_iov_offset;
		rdma_sge_len = rdma_iovs->iov[rdma_idx].iov_len - rdma_iov_offset;
		klm_addr = umr_iovs->iov[umr_idx].iov_base + umr_iov_offset;

		if (umr_sge_len == rdma_sge_len) {
			rdma_idx++;
			umr_iov_offset = 0;
			rdma_iov_offset = 0;
			klm_len = umr_sge_len;
		} else if (umr_sge_len < rdma_sge_len) {
			umr_iov_offset = 0;
			rdma_iov_offset += umr_sge_len;
			klm_len = umr_sge_len;
		} else {
			size_t remaining;

			remaining = umr_sge_len - rdma_sge_len;
			while (remaining) {
				rdma_idx++;
				if (rdma_idx == (int)ACCEL_MLX5_MAX_SGE) {
					break;
				}
				rdma_sge_len = rdma_iovs->iov[rdma_idx].iov_len;
				if (remaining == rdma_sge_len) {
					rdma_idx++;
					rdma_iov_offset = 0;
					remaining = 0;
					break;
				}
				if (remaining < rdma_sge_len) {
					rdma_iov_offset = remaining;
					remaining = 0;
					break;
				}
				remaining -= rdma_sge_len;
			}
			klm_len = umr_sge_len - remaining;
		}
		rc = accel_mlx5_translate_addr(klm_addr, klm_len, domain, domain_ctx, qp, &klm[umr_idx]);
		if (spdk_unlikely(rc)) {
			return -EINVAL;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\t klm[%d] lkey %u, addr %p, len %u\n", umr_idx, klm[umr_idx].lkey,
			      (void *)klm[umr_idx].addr, klm[umr_idx].byte_count);
		umr_len += klm_len;
		umr_idx++;
	}
	accel_mlx5_iov_sgl_advance(umr_iovs, umr_len);
	accel_mlx5_iov_sgl_advance(rdma_iovs, umr_len);
	*len = umr_len;

	return umr_idx;
}

static inline int
accel_mlx5_crc_task_process_multi_req(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	struct accel_mlx5_iov_sgl umr_sgl;
	struct accel_mlx5_iov_sgl *umr_sgl_ptr;
	struct accel_mlx5_iov_sgl rdma_sgl;
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	bool check_op = mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C;
	int klm_count;
	uint32_t remaining;
	uint64_t umr_offset;
	bool sig_init, sig_check_gen = false;
	uint16_t i;
	int rc;
	size_t umr_len[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct mlx5_wqe_data_seg klms[ACCEL_MLX5_MAX_SGE];

	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}
	/* Init signature on the first UMR */
	sig_init = !mlx5_task->num_submitted_reqs;

	/*
	 * accel_mlx5_crc_task_fill_umr_sge() and accel_mlx5_fill_block_sge() advance an IOV during iteration
	 * on it. We must copy accel_mlx5_iov_sgl to iterate twice or more on the same IOV.
	 *
	 * In the in-place case, we iterate on the source IOV three times. That's why we need two copies of
	 * the source accel_mlx5_iov_sgl.
	 *
	 * In the out-of-place case, we iterate on the source IOV once and on the destination IOV two times.
	 * So, we need one copy of the destination accel_mlx5_iov_sgl.
	 */
	if (mlx5_task->flags.bits.inplace) {
		accel_mlx5_iov_sgl_init(&umr_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
		umr_sgl_ptr = &umr_sgl;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
	} else {
		umr_sgl_ptr = &mlx5_task->src;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->dst.iov, mlx5_task->dst.iovcnt);
	}
	mlx5_task->num_wrs = 0;
	for (i = 0; i < num_ops; i++) {
		/*
		 * The last request may have only CRC. Skip UMR in this case because the MKey from
		 * the previous request is used.
		 */
		if (umr_sgl_ptr->iovcnt == 0) {
			assert((mlx5_task->num_completed_reqs + i + 1) == mlx5_task->num_reqs);
			break;
		}
		klm_count = accel_mlx5_crc_task_fill_umr_sge(qp, klms, umr_sgl_ptr, task->src_domain,
				task->src_domain_ctx, &rdma_sgl, &umr_len[i]);
		if (spdk_unlikely(klm_count <= 0)) {
			rc = (klm_count == 0) ? -EINVAL : klm_count;
			SPDK_ERRLOG("failed set UMR sge, rc %d\n", rc);
			return rc;
		}
		if (umr_sgl_ptr->iovcnt == 0) {
			/*
			 * We post RDMA without UMR if the last request has only CRC. We use an MKey from
			 * the last UMR in this case. Since the last request can be postponed to the next
			 * call of this function, we must save the MKey to the task structure.
			 */
			mlx5_task->last_umr_len = umr_len[i];
			mlx5_task->last_mkey_idx = i;
			sig_check_gen = true;
		}
		rc = accel_mlx5_crc_task_configure_umr(mlx5_task, klms, klm_count, mlx5_task->mkeys[i],
						       SPDK_MLX5_UMR_SIG_DOMAIN_WIRE, umr_len[i], sig_init,
						       sig_check_gen);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		sig_init = false;
		dev->stats.sig_umrs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		rc = spdk_mlx5_set_psv(qp->qp, mlx5_task->psv->psv_index, *mlx5_task->base.crc_dst, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	for (i = 0; i < num_ops - 1; i++) {
		if (mlx5_task->flags.bits.inplace) {
			klm_count = accel_mlx5_fill_block_sge(qp, klms, &mlx5_task->src, task->src_domain,
							      task->src_domain_ctx, 0, umr_len[i], &remaining);
		} else {
			klm_count = accel_mlx5_fill_block_sge(qp, klms, &mlx5_task->dst, task->dst_domain,
							      task->dst_domain_ctx, 0, umr_len[i], &remaining);
		}
		if (spdk_unlikely(klm_count <= 0)) {
			rc = (klm_count == 0) ? -EINVAL : klm_count;
			SPDK_ERRLOG("failed set RDMA sge, rc %d\n", rc);
			return rc;
		}
		if (check_op) {
			/* Check with copy is not implemeted in this function */
			assert(mlx5_task->flags.bits.inplace);
			rc = spdk_mlx5_qp_rdma_write(qp->qp, klms, klm_count, 0, mlx5_task->mkeys[i]->mkey,
						     0, rdma_fence);
			dev->stats.rdma_writes++;
		} else {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, klms, klm_count, 0, mlx5_task->mkeys[i]->mkey,
						    0, rdma_fence);
			dev->stats.rdma_reads++;
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
			return rc;
		}
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
	}
	if ((mlx5_task->flags.bits.inplace && mlx5_task->src.iovcnt == 0) ||
	    (!mlx5_task->flags.bits.inplace && mlx5_task->dst.iovcnt == 0)) {
		/*
		 * The last RDMA does not have any data, only CRC. It also does not have a paired Mkey.
		 * The CRC is handled in the previous MKey in this case.
		 */
		klm_count = 0;
		umr_offset = mlx5_task->last_umr_len;
	} else {
		umr_offset = 0;
		mlx5_task->last_mkey_idx = i;
		if (mlx5_task->flags.bits.inplace) {
			klm_count = accel_mlx5_fill_block_sge(qp, klms, &mlx5_task->src, task->src_domain,
							      task->src_domain_ctx, 0, umr_len[i], &remaining);
		} else {
			klm_count = accel_mlx5_fill_block_sge(qp, klms, &mlx5_task->dst, task->dst_domain,
							      task->dst_domain_ctx, 0, umr_len[i], &remaining);
		}
		if (spdk_unlikely(klm_count <= 0)) {
			rc = (klm_count == 0) ? -EINVAL : klm_count;
			SPDK_ERRLOG("failed set RDMA sge, rc %d\n", rc);
			return rc;
		}
		assert(remaining == 0);
	}
	if ((mlx5_task->num_completed_reqs + i + 1) == mlx5_task->num_reqs) {
		/* Ensure that there is a free KLM for the CRC destination. */
		assert(klm_count < (int)ACCEL_MLX5_MAX_SGE);
		/* Add the crc destination to the end of KLMs. */
		if (check_op) {
			*mlx5_task->psv->crc = *mlx5_task->base.crc_dst ^ UINT32_MAX;
		}
		klms[klm_count].lkey = mlx5_task->psv->crc_lkey;
		klms[klm_count].addr = (uintptr_t)mlx5_task->psv->crc;
		klms[klm_count++].byte_count = sizeof(uint32_t);
	}
	rdma_fence |= SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE;
	if (check_op) {
		/* Check with copy is not implemeted in this function */
		assert(mlx5_task->flags.bits.inplace);
		rc = spdk_mlx5_qp_rdma_write(qp->qp, klms, klm_count, umr_offset,
					     mlx5_task->mkeys[mlx5_task->last_mkey_idx]->mkey,
					     (uint64_t)mlx5_task, rdma_fence);
		dev->stats.rdma_writes++;
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, klms, klm_count, umr_offset,
					    mlx5_task->mkeys[mlx5_task->last_mkey_idx]->mkey,
					    (uint64_t)mlx5_task, rdma_fence);
		dev->stats.rdma_reads++;
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);

	return 0;
}
static inline int
accel_mlx5_crc_task_process(struct accel_mlx5_task *mlx5_task)
{
	int rc;

	assert(mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C);

	SPDK_DEBUGLOG(accel_mlx5, "begin, crc task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	if (mlx5_task->num_reqs == 1) {
		rc = accel_mlx5_crc_task_process_one_req(mlx5_task);
	} else {
		rc = accel_mlx5_crc_task_process_multi_req(mlx5_task);
	}

	if (rc == 0) {
		STAILQ_INSERT_TAIL(&mlx5_task->qp->in_hw, mlx5_task, link);
		SPDK_DEBUGLOG(accel_mlx5, "end, crc task, %p, reqs: total %u, submitted %u, completed %u\n",
			      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs,
			      mlx5_task->num_completed_reqs);
	}

	return rc;
}

static inline int
accel_mlx5_task_alloc_crc_ctx(struct accel_mlx5_task *task, void *mkey_pool)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;

	if (spdk_unlikely(accel_mlx5_task_alloc_mkeys(task, mkey_pool))) {
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in signature mkey pool, dev %s\n",
			      dev->pd_ref->context->device->name);
		return -ENOMEM;
	}
	task->psv = spdk_mempool_get(dev->psv_pool_ref);
	if (spdk_unlikely(!task->psv)) {
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in psv pool, dev %s\n", dev->pd_ref->context->device->name);
		spdk_mlx5_mkey_pool_put_bulk(mkey_pool, task->mkeys, task->num_ops);
		task->num_ops = 0;
		accel_mlx5_dev_nomem_task_mkey(qp->dev, task);
		return -ENOMEM;
	}
	/* One extra slot is needed for SET_PSV WQE to reset the error state in PSV. */
	if (spdk_unlikely(task->psv->bits.error)) {
		uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
		uint32_t n_slots = task->num_ops * 2 + 1;

		if (qp_slot < n_slots) {
			spdk_mempool_put(dev->psv_pool_ref, task->psv);
			spdk_mlx5_mkey_pool_put_bulk(mkey_pool, task->mkeys, task->num_ops);
			task->num_ops = 0;
			accel_mlx5_dev_nomem_task_qdepth(qp->dev, task);
			return -ENOMEM;
		}
	}
	return 0;
}

static inline int
accel_mlx5_crypto_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint32_t num_ops = (task->num_reqs - task->num_completed_reqs) * 2;
	int rc;

	if (task->num_ops == 0) {
		rc = accel_mlx5_task_alloc_mkeys(task, dev->crypto_mkeys);
		if (spdk_unlikely(rc != 0)) {
			return -ENOMEM;
		}
	}
	/* Check that we have enough slots in QP */
	num_ops = spdk_min(num_ops, 2 * task->num_ops);
	if (spdk_unlikely(num_ops > qp_slot)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, task);
		return -ENOMEM;
	}

	return accel_mlx5_crypto_task_process(task);
}

static inline int
accel_mlx5_encrypt_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	if (task->num_ops == 0) {
		rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, task->mkeys, 1);
		if (spdk_unlikely(rc)) {
			accel_mlx5_dev_nomem_task_mkey(dev, task);
			return -ENOMEM;
		}
		task->num_ops = 1;
	}
	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, task);
		return -ENOMEM;
	}
	return accel_mlx5_encrypt_mkey_task_process(task);
}

static inline int
accel_mlx5_decrypt_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	if (task->num_ops == 0) {
		rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, task->mkeys, 1);
		if (spdk_unlikely(rc)) {
			accel_mlx5_dev_nomem_task_mkey(dev, task);
			return -ENOMEM;
		}
		task->num_ops = 1;
	}
	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, task);
		return -ENOMEM;
	}
	return accel_mlx5_decrypt_mkey_task_process(task);
}

static inline int
accel_mlx5_crc_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint32_t num_ops = (task->num_reqs - task->num_completed_reqs) * 2;
	int rc;

	if (task->num_ops == 0) {
		rc = accel_mlx5_task_alloc_crc_ctx(task, dev->sig_mkeys);
		if (spdk_unlikely(rc != 0)) {
			return rc;
		}
	}
	/* Check that we have enough slots in QP */
	num_ops = spdk_min(num_ops, 2 * task->num_ops);
	if (spdk_unlikely(num_ops > qp_slot)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, task);
		return -ENOMEM;
	}

	return accel_mlx5_crc_task_process(task);
}

static inline int
accel_mlx5_crypto_crc_task_continue_init(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint32_t num_ops = (task->num_reqs - task->num_completed_reqs) * 2;
	int rc;

	if (task->num_ops == 0) {
		rc = accel_mlx5_task_alloc_crc_ctx(task, dev->crypto_sig_mkeys);
		if (spdk_unlikely(rc != 0)) {
			return rc;
		}
	}
	/* Check that we have enough slots in QP */
	num_ops = spdk_min(num_ops, 2 * task->num_ops);
	if (spdk_unlikely(num_ops > qp_slot)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, task);
		return -ENOMEM;
	}

	return 0;
}

static inline int
accel_mlx5_encrypt_and_crc_task_continue(struct accel_mlx5_task *task)
{
	int rc;

	rc = accel_mlx5_crypto_crc_task_continue_init(task);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	return accel_mlx5_encrypt_and_crc_task_process(task);
}

static inline int
accel_mlx5_crc_and_decrypt_task_continue(struct accel_mlx5_task *task)
{
	int rc;

	rc = accel_mlx5_crypto_crc_task_continue_init(task);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	return accel_mlx5_crc_and_decrypt_task_process(task);
}

static inline int
accel_mlx5_copy_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	task->num_ops = spdk_min(qp_slot, task->num_reqs - task->num_completed_reqs);
	if (spdk_unlikely(task->num_ops == 0)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, task);
		return -ENOMEM;
	}
	return accel_mlx5_copy_task_process(task);
}

static inline int
accel_mlx5_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;

	if (spdk_unlikely(qp->recovering)) {
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		return 0;
	}

	return g_accel_mlx5_tasks_ops[task->mlx5_opcode].cont(task);
}

static inline uint32_t
accel_mlx5_get_copy_task_count(struct iovec *src_iov, uint32_t src_iovcnt,
			       struct iovec *dst_iov, uint32_t dst_iovcnt)
{
	uint32_t src = 0;
	uint32_t dst = 0;
	uint64_t src_offset = 0;
	uint64_t dst_offset = 0;
	uint32_t num_ops = 0;
	uint32_t src_sge_count = 0;

	while (src < src_iovcnt && dst < dst_iovcnt) {
		uint64_t src_len = src_iov[src].iov_len - src_offset;
		uint64_t dst_len = dst_iov[dst].iov_len - dst_offset;

		if (dst_len < src_len) {
			num_ops++;
			dst_offset = 0;
			dst++;
			src_offset += dst_len;
			src_sge_count = 0;
		} else if (src_len < dst_len) {
			src_offset = 0;
			dst_offset += src_len;
			src++;
			if (++src_sge_count >= ACCEL_MLX5_MAX_SGE) {
				num_ops++;
				src_sge_count = 0;
			}
		} else {
			num_ops++;
			src_offset = dst_offset = 0;
			src++;
			dst++;
			src_sge_count = 0;
		}
	}

	assert(src == src_iovcnt);
	assert(dst == dst_iovcnt);
	assert(src_offset == 0);
	assert(dst_offset == 0);
	return num_ops;
}

static inline uint32_t
accel_mlx5_advance_iovec(struct iovec *iov, uint32_t iovcnt, size_t *iov_offset, size_t *len)
{
	uint32_t i;
	size_t iov_len;

	for (i = 0; *len != 0 && i < iovcnt; i++) {
		iov_len = iov[i].iov_len - *iov_offset;

		if (iov_len < *len) {
			*iov_offset = 0;
			*len -= iov_len;
			continue;
		}
		if (iov_len == *len) {
			*iov_offset = 0;
			i++;
		} else { /* iov_len > *len */
			*iov_offset += *len;
		}
		*len = 0;
		break;
	}

	return i;
}

static inline uint32_t
accel_mlx5_get_crc_task_count(struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov,
			      uint32_t dst_iovcnt)
{
	uint32_t src_idx = 0;
	uint32_t dst_idx = 0;
	uint32_t num_ops = 1;
	uint32_t num_src_sge = 1;
	uint32_t num_dst_sge = 1;
	size_t src_offset = 0;
	size_t dst_offset = 0;
	uint32_t num_sge;
	size_t src_len;
	size_t dst_len;

	/* One operation is enough if both iovs fit into ACCEL_MLX5_MAX_SGE. One SGE is reserved for CRC on dst_iov. */
	if (src_iovcnt <= ACCEL_MLX5_MAX_SGE && (dst_iovcnt + 1) <= ACCEL_MLX5_MAX_SGE) {
		return 1;
	}

	while (src_idx < src_iovcnt && dst_idx < dst_iovcnt) {
		if (num_src_sge > ACCEL_MLX5_MAX_SGE || num_dst_sge > ACCEL_MLX5_MAX_SGE) {
			num_ops++;
			num_src_sge = 1;
			num_dst_sge = 1;
		}
		src_len = src_iov[src_idx].iov_len - src_offset;
		dst_len = dst_iov[dst_idx].iov_len - dst_offset;

		if (src_len == dst_len) {
			num_src_sge++;
			num_dst_sge++;
			src_offset = 0;
			dst_offset = 0;
			src_idx++;
			dst_idx++;
			continue;
		}
		if (src_len < dst_len) {
			/* Advance src_iov to reach the point that corresponds to the end of the current dst_iov. */
			num_sge = accel_mlx5_advance_iovec(&src_iov[src_idx],
							   spdk_min(ACCEL_MLX5_MAX_SGE + 1 - num_src_sge,
									   src_iovcnt - src_idx),
							   &src_offset, &dst_len);
			src_idx += num_sge;
			num_src_sge += num_sge;
			if (dst_len != 0) {
				/*
				 * ACCEL_MLX5_MAX_SGE is reached on src_iov, and dst_len bytes
				 * are left on the current dst_iov.
				 */
				dst_offset = dst_iov[dst_idx].iov_len - dst_len;
			} else {
				/* The src_iov advance is completed, shift to the next dst_iov. */
				dst_idx++;
				num_dst_sge++;
				dst_offset = 0;
			}
		} else { /* src_len > dst_len */
			/* Advance dst_iov to reach the point that corresponds to the end of the current src_iov. */
			num_sge = accel_mlx5_advance_iovec(&dst_iov[dst_idx],
							   spdk_min(ACCEL_MLX5_MAX_SGE + 1 - num_dst_sge,
									   dst_iovcnt - dst_idx),
							   &dst_offset, &src_len);
			dst_idx += num_sge;
			num_dst_sge += num_sge;
			if (src_len != 0) {
				/*
				 * ACCEL_MLX5_MAX_SGE is reached on dst_iov, and src_len bytes
				 * are left on the current src_iov.
				 */
				src_offset = src_iov[src_idx].iov_len - src_len;
			} else {
				/* The dst_iov advance is completed, shift to the next src_iov. */
				src_idx++;
				num_src_sge++;
				src_offset = 0;
			}
		}
	}
	/* An extra operation is needed if no space is left on dst_iov because CRC takes one SGE. */
	if (num_dst_sge > ACCEL_MLX5_MAX_SGE) {
		num_ops++;
	}

	/* The above loop must reach the end of both iovs simultaneously because their size is the same. */
	assert(src_idx == src_iovcnt);
	assert(dst_idx == dst_iovcnt);
	assert(src_offset == 0);
	assert(dst_offset == 0);

	return num_ops;
}

static inline struct accel_mlx5_qp *
accel_mlx5_qp_find(struct accel_mlx5_qpairs_map *map, struct spdk_memory_domain *domain)
{
	struct accel_mlx5_qp _qp;

	_qp.domain = domain;

	return RB_FIND(accel_mlx5_qpairs_map, map, &_qp);
}

static void
accel_mlx5_del_qps_on_ch_done(struct spdk_io_channel_iter *i, int status)
{

}

static void
accel_mlx5_destroy_qp_with_domain(struct accel_mlx5_qp *qp)
{
	if (qp->wrs_submitted == 0) {
		if (qp->qp) {
			spdk_mlx5_qp_destroy(qp->qp);
			qp->qp = NULL;
		}
		RB_REMOVE(accel_mlx5_qpairs_map, &qp->dev->qpairs_map, qp);
		free(qp);
	} else {
		/* Move QP to error state, that will flush all outstanding requests.
		 * QP will be deleted once empty */
		spdk_mlx5_qp_set_error_state(qp->qp);
	}
}

static void
accel_mlx5_del_qps_on_ch(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct accel_mlx5_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_memory_domain *domain = spdk_io_channel_iter_get_ctx(i);
	struct accel_mlx5_qp *qp;
	uint32_t j;

	for (j = 0; j < ch->num_devs; j++) {
		qp = accel_mlx5_qp_find(&ch->devs[j].qpairs_map, domain);
		if (qp) {
			accel_mlx5_destroy_qp_with_domain(qp);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
accel_mlx5_domain_notification(void *user_ctx,
			       struct spdk_memory_domain_update_notification_ctx *ctx)
{
	assert(user_ctx == &g_accel_mlx5);

	if (ctx->type == SPDK_MEMORY_DOMAIN_UPDATE_NOTIFICATION_TYPE_DELETED) {
		spdk_for_each_channel(user_ctx, accel_mlx5_del_qps_on_ch, ctx->domain,
				      accel_mlx5_del_qps_on_ch_done);
	}
}

static inline struct accel_mlx5_qp *
accel_mlx5_dev_get_qp_by_domain(struct accel_mlx5_dev *dev, struct spdk_memory_domain *domain)
{
	struct accel_mlx5_qp *qp;
	int rc;

	qp = accel_mlx5_qp_find(&dev->qpairs_map, domain);
	if (!qp) {
		qp = calloc(1, sizeof(*qp));
		if (!qp) {
			SPDK_ERRLOG("Memory allocation failed\n");
			return NULL;
		}
		rc = accel_mlx5_create_qp(dev, qp);
		if (rc) {
			SPDK_ERRLOG("Failed to create qp, rc %d\n", rc);
			free(qp);
			return NULL;
		}
		qp->domain = domain;
		RB_INSERT(accel_mlx5_qpairs_map, &dev->qpairs_map, qp);
		SPDK_NOTICELOG("created new qp num %u for domain %p\n", qp->qp->hw.qp_num, domain);
	}
	assert(qp->dev == dev);

	return qp;
}

static inline struct accel_mlx5_qp *
accel_mlx5_task_assign_qp(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_dev *dev)
{
	if (!g_accel_mlx5.qp_per_domain || (!mlx5_task->base.src_domain && !mlx5_task->base.dst_domain)) {
		return &dev->mlx5_qp;
	}

	/* TODO: find a way to distinguish between SPDK internal and app external domains.
	 * We need to use app domain to find a qp. For now, make an assumption that app domain (src or dst)
	 * depends on the opcode */
	switch (mlx5_task->mlx5_opcode) {
	case ACCEL_MLX5_OPC_CRYPTO:
	case ACCEL_MLX5_OPC_ENCRYPT_MKEY:
	case ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C:
		if (mlx5_task->base.src_domain) {
			return accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.src_domain);
		} else {
			return &dev->mlx5_qp;
		}
		break;
	case ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT:
	case ACCEL_MLX5_OPC_DECRYPT_MKEY:
		if (mlx5_task->base.dst_domain) {
			return accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.dst_domain);
		} else {
			return &dev->mlx5_qp;
		}
		break;
	case ACCEL_MLX5_OPC_COPY:
		if (mlx5_task->base.dst_domain) {
			return accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.dst_domain);
		} else {
			return &dev->mlx5_qp;
		}
		break;
	case ACCEL_MLX5_OPC_CRC32C:
		if (mlx5_task->base.src_domain) {
			return accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.dst_domain);
		} else {
			return &dev->mlx5_qp;
		}
		break;
	default:
		return NULL;
	}
}

static inline int
accel_mlx5_copy_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(qp->dev, qp);

	if (spdk_unlikely(mlx5_task->flags.bits.driver_seq &&
			  (mlx5_task->base.src_domain == spdk_accel_get_memory_domain() ||
			   mlx5_task->base.dst_domain == spdk_accel_get_memory_domain()))) {
		SPDK_ERRLOG("Can't handle driver seq with accel memory domain\n");
		return -ENOTSUP;
	}

	if (spdk_unlikely(task->s.iovcnt > ACCEL_MLX5_MAX_SGE)) {
		if (task->d.iovcnt == 1) {
			mlx5_task->num_reqs = SPDK_CEIL_DIV(task->s.iovcnt, ACCEL_MLX5_MAX_SGE);
		} else {
			mlx5_task->num_reqs = accel_mlx5_get_copy_task_count(task->s.iovs, task->s.iovcnt,
					      task->d.iovs, task->d.iovcnt);
		}
	} else {
		mlx5_task->num_reqs = task->d.iovcnt;
	}
	mlx5_task->flags.bits.inplace = 0;
	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
	mlx5_task->num_ops = spdk_min(qp_slot, mlx5_task->num_reqs);
	if (spdk_unlikely(!mlx5_task->num_ops)) {
		accel_mlx5_dev_nomem_task_qdepth(mlx5_task->qp->dev, mlx5_task);
		return -ENOMEM;
	}
	SPDK_DEBUGLOG(accel_mlx5, "copy task num_reqs %u, num_ops %u\n", mlx5_task->num_reqs,
		      mlx5_task->num_ops);

	return 0;
}

static inline int
accel_mlx5_crypto_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	struct spdk_mlx5_crypto_dek_data dek_data;
#ifdef DEBUG
	size_t dst_nbytes = 0;
	uint32_t i;
#endif
	uint32_t num_blocks;
	int rc;
	bool crypto_key_ok;

	if (spdk_unlikely(mlx5_task->flags.bits.driver_seq &&
			  (mlx5_task->base.src_domain == spdk_accel_get_memory_domain() ||
			   mlx5_task->base.dst_domain == spdk_accel_get_memory_domain()))) {
		SPDK_ERRLOG("Can't handle driver seq with accel memory domain\n");
		return -ENOTSUP;
	}

	crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
			 task->crypto_key->priv);
	if (spdk_unlikely((task->nbytes % mlx5_task->base.block_size != 0) || !crypto_key_ok)) {
		if (crypto_key_ok) {
			SPDK_ERRLOG("src length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
				    mlx5_task->base.block_size);
		} else {
			SPDK_ERRLOG("Wrong crypto key provided\n");
		}
		return -EINVAL;
	}

	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->pd_ref, &dek_data);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to get DEK data, rc %d\n", rc);
		return rc;
	}
	mlx5_task->dek_obj_id = dek_data.dek_obj_id;
	mlx5_task->tweak_mode = dek_data.tweak_mode;

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	num_blocks = task->nbytes / mlx5_task->base.block_size;
	mlx5_task->num_blocks = num_blocks;
	mlx5_task->num_processed_blocks = 0;
	if (task->d.iovcnt == 0 || (task->d.iovcnt == task->s.iovcnt &&
				    accel_mlx5_compare_iovs(task->d.iovs, task->s.iovs, task->s.iovcnt))) {
		mlx5_task->flags.bits.inplace = 1;
	} else {
		mlx5_task->flags.bits.inplace = 0;
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
#ifdef DEBUG
		for (i = 0; i < task->d.iovcnt; i++) {
			dst_nbytes += task->d.iovs[i].iov_len;
		}
		if (task->nbytes != dst_nbytes) {
			SPDK_ERRLOG("src len %"PRIu64" doesn't match dst len %zu\n", task->nbytes, dst_nbytes);
			return -EINVAL;
		}
#endif
	}
	if (dev->crypto_multi_block) {
		if (g_accel_mlx5.split_mb_blocks) {
			mlx5_task->num_reqs = SPDK_CEIL_DIV(num_blocks, g_accel_mlx5.split_mb_blocks);
			/* Last req may consume less blocks */
			mlx5_task->blocks_per_req = spdk_min(num_blocks, g_accel_mlx5.split_mb_blocks);
		} else {
			if (task->s.iovcnt > ACCEL_MLX5_MAX_SGE || task->d.iovcnt > ACCEL_MLX5_MAX_SGE) {
				uint32_t max_sge_count = spdk_max(task->s.iovcnt, task->d.iovcnt);

				mlx5_task->num_reqs = SPDK_CEIL_DIV(max_sge_count, ACCEL_MLX5_MAX_SGE);
				mlx5_task->blocks_per_req = SPDK_CEIL_DIV(num_blocks, mlx5_task->num_reqs);
				SPDK_DEBUGLOG(accel_mlx5, "task %p, src_iovs %u, dst_iovs %u, num_reqs %u, "
					      "blocks/req %u, blocks %u\n", task, task->s.iovcnt, task->d.iovcnt,
					      mlx5_task->num_reqs, mlx5_task->blocks_per_req, num_blocks);

			} else {
				mlx5_task->num_reqs = 1;
				mlx5_task->blocks_per_req = num_blocks;
			}
		}
	} else {
		mlx5_task->num_reqs = num_blocks;
		mlx5_task->blocks_per_req = 1;
	}

	if (spdk_unlikely(accel_mlx5_task_alloc_mkeys(mlx5_task, dev->crypto_mkeys))) {
		return -ENOMEM;
	}
	SPDK_DEBUGLOG(accel_mlx5, "crypto task num_reqs %u, num_ops %u, num_blocks %u, src_len %zu\n",
		      mlx5_task->num_reqs, mlx5_task->num_ops, mlx5_task->num_blocks, task->nbytes);

	return 0;
}

static inline int
accel_mlx5_encrypt_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_mlx5_crypto_dek_data dek_data;
	uint32_t num_blocks;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	bool crypto_key_ok;

	if (spdk_unlikely(task->s.iovcnt > ACCEL_MLX5_MAX_SGE)) {
		/* With `external mkey` we can't split task or register several UMRs */
		SPDK_ERRLOG("src buffer is too fragmented\n");
		return -EINVAL;
	}
	if (spdk_unlikely(task->src_domain == spdk_accel_get_memory_domain())) {
		SPDK_ERRLOG("accel domain is not supported\n");
		return -EINVAL;
	}
	if (spdk_unlikely(spdk_accel_sequence_next_task(task) != NULL)) {
		SPDK_ERRLOG("Mkey registartion is only supported for single task\n");
		return -ENOTSUP;
	}

	crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
			 task->crypto_key->priv);
	if (spdk_unlikely((task->nbytes % mlx5_task->base.block_size != 0) || !crypto_key_ok)) {
		if (crypto_key_ok) {
			SPDK_ERRLOG("src length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
				    mlx5_task->base.block_size);
		} else {
			SPDK_ERRLOG("Wrong crypto key provided\n");
		}
		return -EINVAL;
	}

	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->pd_ref, &dek_data);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to get DEK data, rc %d\n", rc);
		return rc;
	}
	mlx5_task->dek_obj_id = dek_data.dek_obj_id;
	mlx5_task->tweak_mode = dek_data.tweak_mode;

	num_blocks = task->nbytes / mlx5_task->base.block_size;
	if (dev->crypto_multi_block) {
		if (spdk_unlikely(g_accel_mlx5.split_mb_blocks && num_blocks > g_accel_mlx5.split_mb_blocks)) {
			SPDK_ERRLOG("Number of blocks in task %u exceeds split threshold %u, can't handle\n",
				    num_blocks, g_accel_mlx5.split_mb_blocks);
			return -E2BIG;
		}
	} else if (num_blocks != 1) {
		SPDK_ERRLOG("Task contains more than 1 block, can't handle\n");
		return -E2BIG;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	mlx5_task->num_blocks = num_blocks;
	mlx5_task->num_processed_blocks = 0;
	mlx5_task->flags.bits.inplace = 1;
	mlx5_task->num_reqs = 1;
	mlx5_task->blocks_per_req = num_blocks;

	if (spdk_unlikely(qp_slot == 0)) {
		mlx5_task->num_ops = 0;
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, mlx5_task->mkeys, 1);
	if (spdk_unlikely(rc)) {
		mlx5_task->num_ops = 0;
		accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "crypto task num_reqs %u, num_ops %u, num_blocks %u, src_len %zu\n",
		      mlx5_task->num_reqs, mlx5_task->num_ops, mlx5_task->num_blocks, task->nbytes);

	return 0;
}

static inline int
accel_mlx5_decrypt_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_mlx5_crypto_dek_data dek_data;
	uint32_t num_blocks;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	bool crypto_key_ok;

	if (spdk_unlikely(task->d.iovcnt > ACCEL_MLX5_MAX_SGE)) {
		/* With `external mkey` we can't split task or register several UMRs */
		SPDK_ERRLOG("dst buffer is too fragmented\n");
		return -EINVAL;
	}
	if (spdk_unlikely(task->dst_domain == spdk_accel_get_memory_domain())) {
		SPDK_ERRLOG("accel domain is not supported\n");
		return -EINVAL;
	}
	if (spdk_unlikely(spdk_accel_sequence_next_task(task) != NULL)) {
		SPDK_ERRLOG("Mkey registartion is only supported for single task\n");
		return -ENOTSUP;
	}

	crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
			 task->crypto_key->priv);
	if (spdk_unlikely((task->nbytes % mlx5_task->base.block_size != 0) || !crypto_key_ok)) {
		if (crypto_key_ok) {
			SPDK_ERRLOG("dst length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
				    mlx5_task->base.block_size);
		} else {
			SPDK_ERRLOG("Wrong crypto key provided\n");
		}
		return -EINVAL;
	}

	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->pd_ref, &dek_data);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to get DEK data, rc %d\n", rc);
		return rc;
	}
	mlx5_task->dek_obj_id = dek_data.dek_obj_id;
	mlx5_task->tweak_mode = dek_data.tweak_mode;

	num_blocks = task->nbytes / mlx5_task->base.block_size;
	if (dev->crypto_multi_block) {
		if (spdk_unlikely(g_accel_mlx5.split_mb_blocks && num_blocks > g_accel_mlx5.split_mb_blocks)) {
			SPDK_ERRLOG("Number of blocks in task %u exceeds split threshold %u, can't handle\n",
				    num_blocks, g_accel_mlx5.split_mb_blocks);
			return -E2BIG;
		}
	} else if (num_blocks != 1) {
		SPDK_ERRLOG("Task contains more than 1 block, can't handle\n");
		return -E2BIG;
	}

	/* Initialize src iov_sgl using dst iov to reuse existing UMR configuration path */
	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->d.iovs, task->d.iovcnt);
	mlx5_task->num_blocks = num_blocks;
	mlx5_task->num_processed_blocks = 0;
	mlx5_task->flags.bits.inplace = 1;
	mlx5_task->num_reqs = 1;
	mlx5_task->blocks_per_req = num_blocks;

	if (spdk_unlikely(qp_slot == 0)) {
		mlx5_task->num_ops = 0;
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, mlx5_task->mkeys, 1);
	if (spdk_unlikely(rc)) {
		mlx5_task->num_ops = 0;
		accel_mlx5_dev_nomem_task_mkey(qp->dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "crypto task num_reqs %u, num_ops %u, num_blocks %u, dst_len %zu\n",
		      mlx5_task->num_reqs, mlx5_task->num_ops, mlx5_task->num_blocks, task->nbytes);

	return 0;
}

static inline int
accel_mlx5_crc_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int rc;

	if (spdk_unlikely(mlx5_task->flags.bits.driver_seq &&
			  (mlx5_task->base.src_domain == spdk_accel_get_memory_domain() ||
			   mlx5_task->base.dst_domain == spdk_accel_get_memory_domain()))) {
		SPDK_ERRLOG("Can't handle driver seq with accel memory domain\n");
		return -ENOTSUP;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	if (mlx5_task->flags.bits.inplace) {
		/* One entry is reserved for CRC */
		mlx5_task->num_reqs = SPDK_CEIL_DIV(mlx5_task->src.iovcnt + 1, ACCEL_MLX5_MAX_SGE);
	} else {
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
		mlx5_task->num_reqs = accel_mlx5_get_crc_task_count(mlx5_task->src.iov, mlx5_task->src.iovcnt,
				      mlx5_task->dst.iov, mlx5_task->dst.iovcnt);
	}

	rc = accel_mlx5_task_alloc_crc_ctx(mlx5_task, dev->sig_mkeys);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	return 0;
}

static inline int
accel_mlx5_encrypt_and_crc_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct spdk_accel_task *task_crc;
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	struct spdk_mlx5_crypto_dek_data dek_data;
	uint32_t num_blocks;
	int rc;
	bool crypto_key_ok;

	if (spdk_unlikely(mlx5_task->flags.bits.driver_seq &&
			  (mlx5_task->base.src_domain == spdk_accel_get_memory_domain() ||
			   mlx5_task->base.dst_domain == spdk_accel_get_memory_domain()))) {
		SPDK_ERRLOG("Can't handle driver seq with accel memory domain\n");
		return -ENOTSUP;
	}

	crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
			 task->crypto_key->priv);
	if (spdk_unlikely((task->nbytes % mlx5_task->base.block_size != 0) || !crypto_key_ok)) {
		if (crypto_key_ok) {
			SPDK_ERRLOG("dst length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
				    mlx5_task->base.block_size);
		} else {
			SPDK_ERRLOG("Wrong crypto key provided\n");
		}
		return -EINVAL;
	}

	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->pd_ref, &dek_data);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to get DEK data, rc %d\n", rc);
		return rc;
	}
	mlx5_task->dek_obj_id = dek_data.dek_obj_id;
	mlx5_task->tweak_mode = dek_data.tweak_mode;

	task_crc = TAILQ_NEXT(task, seq_link);
	assert(task_crc);
	mlx5_task->encrypt_crc.crc = task_crc->crc_dst;
	mlx5_task->encrypt_crc.seed = task_crc->seed;

	accel_mlx5_iov_sgl_init(&mlx5_task->src, mlx5_task->base.s.iovs, mlx5_task->base.s.iovcnt);
	if (!mlx5_task->flags.bits.inplace) {
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, mlx5_task->base.d.iovs, mlx5_task->base.d.iovcnt);
	}
	num_blocks = task->nbytes / mlx5_task->base.block_size;
	mlx5_task->num_blocks = num_blocks;
	if (dev->crypto_multi_block) {
		if (g_accel_mlx5.split_mb_blocks) {
			mlx5_task->num_reqs = SPDK_CEIL_DIV(num_blocks, g_accel_mlx5.split_mb_blocks);
			/* Last req may consume less blocks */
			mlx5_task->blocks_per_req = spdk_min(num_blocks, g_accel_mlx5.split_mb_blocks);
		} else {
			mlx5_task->num_reqs = 1;
			mlx5_task->blocks_per_req = num_blocks;
		}
	} else {
		mlx5_task->num_reqs = num_blocks;
		mlx5_task->blocks_per_req = 1;
	}

	/* We stored all necessary info about next task, now we can complete it to re-use as fast as possible */
	spdk_accel_task_complete(task_crc, 0);

	rc = accel_mlx5_task_alloc_crc_ctx(mlx5_task, dev->crypto_sig_mkeys);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	SPDK_DEBUGLOG(accel_mlx5, "crypto and crc task num_reqs %u, num_ops %u, num_blocks %u\n",
		      mlx5_task->num_reqs, mlx5_task->num_ops, mlx5_task->num_blocks);

	return 0;
}

static inline int
accel_mlx5_crc_and_decrypt_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task_crypto;
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	struct spdk_mlx5_crypto_dek_data dek_data;
	uint32_t num_blocks;
	int rc;
	bool crypto_key_ok;

	if (spdk_unlikely(mlx5_task->flags.bits.driver_seq &&
			  (mlx5_task->base.src_domain == spdk_accel_get_memory_domain() ||
			   mlx5_task->base.dst_domain == spdk_accel_get_memory_domain()))) {
		SPDK_ERRLOG("Can't handle driver seq with accel memory domain\n");
		return -ENOTSUP;
	}

	task_crypto = TAILQ_NEXT(task, seq_link);
	assert(task_crypto);

	crypto_key_ok = (task_crypto->crypto_key &&
			 task_crypto->crypto_key->module_if == &g_accel_mlx5.module &&
			 task_crypto->crypto_key->priv);
	if (spdk_unlikely((task->nbytes % task_crypto->block_size != 0) || !crypto_key_ok)) {
		if (crypto_key_ok) {
			SPDK_ERRLOG("dst length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
				    task_crypto->block_size);
		} else {
			SPDK_ERRLOG("Wrong crypto key provided\n");
		}
		return -EINVAL;
	}

	rc = spdk_mlx5_crypto_get_dek_data(task_crypto->crypto_key->priv, dev->pd_ref, &dek_data);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to get DEK data, rc %d\n", rc);
		return rc;
	}
	mlx5_task->dek_obj_id = dek_data.dek_obj_id;
	mlx5_task->tweak_mode = dek_data.tweak_mode;

	mlx5_task->crc_decrypt.iv = task_crypto->iv;
	mlx5_task->crc_decrypt.block_size = task_crypto->block_size;

	/* We are going to store crypto tasks's memory domain pointer into crc task structure. Make sure we don't
	 * overwrite any values */
	assert(task->src_domain == NULL);
	assert(task->src_domain_ctx == NULL);
	assert(task->dst_domain == NULL);
	assert(task->dst_domain_ctx == NULL);
	assert(task->cached_lkey == NULL);

	task->src_domain = task_crypto->src_domain;
	task->src_domain_ctx = task_crypto->src_domain_ctx;
	task->dst_domain = task_crypto->dst_domain;
	task->dst_domain_ctx = task_crypto->dst_domain_ctx;
	task->cached_lkey = task_crypto->cached_lkey;

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task_crypto->s.iovs, task_crypto->s.iovcnt);
	if (!mlx5_task->flags.bits.inplace) {
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, task_crypto->d.iovs, task_crypto->d.iovcnt);
	}
	num_blocks = task->nbytes / task_crypto->block_size;
	mlx5_task->num_blocks = num_blocks;
	if (dev->crypto_multi_block) {
		if (g_accel_mlx5.split_mb_blocks) {
			mlx5_task->num_reqs = SPDK_CEIL_DIV(num_blocks, g_accel_mlx5.split_mb_blocks);
			/* Last req may consume less blocks */
			mlx5_task->blocks_per_req = spdk_min(num_blocks, g_accel_mlx5.split_mb_blocks);
		} else {
			mlx5_task->num_reqs = 1;
			mlx5_task->blocks_per_req = num_blocks;
		}
	} else {
		mlx5_task->num_reqs = num_blocks;
		mlx5_task->blocks_per_req = 1;
	}

	/* We stored all necessary info about next task, now we can complete it to re-use as fast as possible */
	spdk_accel_task_complete(task_crypto, 0);

	rc = accel_mlx5_task_alloc_crc_ctx(mlx5_task, dev->crypto_sig_mkeys);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	SPDK_DEBUGLOG(accel_mlx5, "crypto and crc task num_reqs %u, num_ops %u, num_blocks %u\n",
		      mlx5_task->num_reqs, mlx5_task->num_ops, mlx5_task->num_blocks);

	return 0;
}

static inline void
accel_mlx5_task_merge_encrypt_and_crc(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct spdk_accel_task *task_next = TAILQ_NEXT(task, seq_link);
	struct iovec *crypto_dst_iovs;
	uint32_t crypto_dst_iovcnt;

	assert(task->op_code == SPDK_ACCEL_OPC_ENCRYPT);

	if (!task_next || task_next->op_code != SPDK_ACCEL_OPC_CRC32C) {
		return;
	}

	if (task->d.iovcnt == 0 || (task->d.iovcnt == task->s.iovcnt &&
				    accel_mlx5_compare_iovs(task->d.iovs, task->s.iovs, task->s.iovcnt))) {
		mlx5_task->flags.bits.inplace = 1;
		crypto_dst_iovs = task->s.iovs;
		crypto_dst_iovcnt = task->s.iovcnt;
	} else {
		mlx5_task->flags.bits.inplace = 0;
		crypto_dst_iovs = task->d.iovs;
		crypto_dst_iovcnt = task->d.iovcnt;
	}

	if ((crypto_dst_iovcnt != task_next->s.iovcnt) ||
	    !accel_mlx5_compare_iovs(crypto_dst_iovs, task_next->s.iovs,
				     crypto_dst_iovcnt)) {
		return;
	}

	assert(!mlx5_task->flags.bits.driver_mkey);
	mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C;
}

static inline void
accel_mlx5_task_merge_crc_and_decrypt(struct accel_mlx5_task *mlx5_task_crc)
{
	struct spdk_accel_task *task_crc = &mlx5_task_crc->base;
	struct spdk_accel_task *task_crypto = TAILQ_NEXT(task_crc, seq_link);

	assert(task_crc->op_code == SPDK_ACCEL_OPC_CHECK_CRC32C);

	if (!task_crypto || task_crypto->op_code != SPDK_ACCEL_OPC_DECRYPT) {
		return;
	}

	if (task_crypto->d.iovcnt == 0 ||
	    (task_crypto->d.iovcnt == task_crypto->s.iovcnt &&
	     accel_mlx5_compare_iovs(task_crypto->d.iovs, task_crypto->s.iovs, task_crypto->s.iovcnt))) {
		mlx5_task_crc->flags.bits.inplace = 1;
	} else {
		mlx5_task_crc->flags.bits.inplace = 0;
	}

	if ((task_crypto->s.iovcnt != task_crc->s.iovcnt) ||
	    !accel_mlx5_compare_iovs(task_crypto->s.iovs, task_crc->s.iovs,
				     task_crypto->s.iovcnt)) {
		return;
	}

	mlx5_task_crc->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT;
	mlx5_task_crc->flags.bits.enc_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
}

static inline void
accel_mlx5_task_init_opcode(struct accel_mlx5_task *mlx5_task)
{
	int8_t base_opcode = mlx5_task->base.op_code;

	switch (base_opcode) {
	case SPDK_ACCEL_OPC_COPY:
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_COPY;
		break;
	case SPDK_ACCEL_OPC_ENCRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->flags.bits.enc_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
		mlx5_task->mlx5_opcode = mlx5_task->flags.bits.driver_mkey ? ACCEL_MLX5_OPC_ENCRYPT_MKEY :
					 ACCEL_MLX5_OPC_CRYPTO;
		if (g_accel_mlx5.merge) {
			accel_mlx5_task_merge_encrypt_and_crc(mlx5_task);
		}
		break;
	case SPDK_ACCEL_OPC_DECRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->flags.bits.enc_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
		mlx5_task->mlx5_opcode = mlx5_task->flags.bits.driver_mkey ? ACCEL_MLX5_OPC_DECRYPT_MKEY :
					 ACCEL_MLX5_OPC_CRYPTO;
		break;
	case SPDK_ACCEL_OPC_CRC32C:
		mlx5_task->flags.bits.inplace = 1;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	case SPDK_ACCEL_OPC_CHECK_CRC32C:
		mlx5_task->flags.bits.inplace = 1;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		if (g_accel_mlx5.merge) {
			accel_mlx5_task_merge_crc_and_decrypt(mlx5_task);
		}
		break;
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		mlx5_task->flags.bits.inplace = 0;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	default:
		SPDK_ERRLOG("wrong opcode %d\n", base_opcode);
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_LAST;
	}
}

static int
accel_mlx5_task_op_not_implemented(struct accel_mlx5_task *mlx5_task)
{
	SPDK_ERRLOG("wrong function called\n");
	SPDK_UNREACHABLE();
}

static void
accel_mlx5_task_op_not_implemented_v(struct accel_mlx5_task *mlx5_task)
{
	SPDK_ERRLOG("wrong function called\n");
	SPDK_UNREACHABLE();
}

static int
accel_mlx5_task_op_not_supported(struct accel_mlx5_task *mlx5_task)
{
	SPDK_ERRLOG("Unsupported opcode %d\n", mlx5_task->base.op_code);

	return -ENOTSUP;
}

static struct accel_mlx5_task_ops g_accel_mlx5_tasks_ops[] = {
	[ACCEL_MLX5_OPC_COPY] = {
		.init = accel_mlx5_copy_task_init,
		.process = accel_mlx5_copy_task_process,
		.cont = accel_mlx5_copy_task_continue,
		.complete = accel_mlx5_copy_task_complete,
	},
	[ACCEL_MLX5_OPC_CRYPTO] = {
		.init = accel_mlx5_crypto_task_init,
		.process = accel_mlx5_crypto_task_process,
		.cont = accel_mlx5_crypto_task_continue,
		.complete = accel_mlx5_crypto_task_complete,
	},
	[ACCEL_MLX5_OPC_ENCRYPT_MKEY] = {
		.init = accel_mlx5_encrypt_mkey_task_init,
		.process = accel_mlx5_encrypt_mkey_task_process,
		.cont = accel_mlx5_encrypt_mkey_task_continue,
		.complete = accel_mlx5_crypto_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_DECRYPT_MKEY] = {
		.init = accel_mlx5_decrypt_mkey_task_init,
		.process = accel_mlx5_decrypt_mkey_task_process,
		.cont = accel_mlx5_decrypt_mkey_task_continue,
		.complete = accel_mlx5_crypto_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_CRC32C] = {
		.init = accel_mlx5_crc_task_init,
		.process = accel_mlx5_crc_task_process,
		.cont = accel_mlx5_crc_task_continue,
		.complete = accel_mlx5_crc_task_complete,
	},
	[ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C] = {
		.init = accel_mlx5_encrypt_and_crc_task_init,
		.process = accel_mlx5_encrypt_and_crc_task_process,
		.cont = accel_mlx5_encrypt_and_crc_task_continue,
		.complete = accel_mlx5_encrypt_crc_task_complete,
	},
	[ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT] = {
		.init = accel_mlx5_crc_and_decrypt_task_init,
		.process = accel_mlx5_crc_and_decrypt_task_process,
		.cont = accel_mlx5_crc_and_decrypt_task_continue,
		.complete = accel_mlx5_crc_decrypt_task_complete,
	},
	[ACCEL_MLX5_OPC_LAST] = {
		.init = accel_mlx5_task_op_not_supported,
		.process = accel_mlx5_task_op_not_implemented,
		.cont = accel_mlx5_task_op_not_implemented,
		.complete = accel_mlx5_task_op_not_implemented_v
	},
};

static int
accel_mlx5_submit_tasks(struct spdk_io_channel *_ch, struct spdk_accel_task *task)
{
	struct accel_mlx5_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct accel_mlx5_task *mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	struct accel_mlx5_dev *dev;
	struct spdk_mlx5_driver_io_context *driver_ctx;
	uint32_t i;
	int rc;

	assert(g_accel_mlx5.enabled);

	if (task->seq && (driver_ctx = spdk_accel_sequence_get_driver_ctx(task->seq)) != NULL) {
		if (spdk_unlikely(task->op_code != SPDK_ACCEL_OPC_ENCRYPT &&
				  task->op_code != SPDK_ACCEL_OPC_DECRYPT)) {
			SPDK_ERRLOG("Mkey registartion is only supported for encrypt\n");
			return -ENOTSUP;
		}
		if (spdk_unlikely(driver_ctx->mkey != NULL)) {
			SPDK_ERRLOG("driver IO ctx already has an mkey\n");
			return -ENOTSUP;
		}
		dev = NULL;
		for (i = 0; i < ch->num_devs; i++) {
			if (ch->devs[i].pd_ref == driver_ctx->qp->pd) {
				dev = &ch->devs[i];
				break;
			}
		}
		if (spdk_unlikely(!dev)) {
			SPDK_ERRLOG("Can't find MLX5 dev for pd %p dev %s\n", driver_ctx->qp->pd,
				    driver_ctx->qp->pd->context->device->name);
			return -ENODEV;
		}

		mlx5_task->flags.bits.driver_mkey = 1;
	} else {
		dev = &ch->devs[ch->dev_idx];
		ch->dev_idx++;
		if (ch->dev_idx == ch->num_devs) {
			ch->dev_idx = 0;
		}
	}

	accel_mlx5_task_init_opcode(mlx5_task);

	mlx5_task->qp = accel_mlx5_task_assign_qp(mlx5_task, dev);
	if (spdk_unlikely(!mlx5_task->qp)) {
		SPDK_ERRLOG("no qp for task %p opc %d; domain src %p, dst %p\n", mlx5_task, mlx5_task->mlx5_opcode,
			    task->src_domain, task->dst_domain);
		return -ENODEV;
	}

	mlx5_task->num_completed_reqs = 0;
	mlx5_task->num_submitted_reqs = 0;
	dev->stats.opcodes[mlx5_task->mlx5_opcode]++;

	rc = g_accel_mlx5_tasks_ops[mlx5_task->mlx5_opcode].init(mlx5_task);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			return 0;
		}
		SPDK_ERRLOG("Task opc %d init failed, rc %d\n", task->op_code, rc);
		return rc;
	}

	if (spdk_unlikely(mlx5_task->qp->recovering)) {
		STAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
		return 0;
	}

	if (!ch->pp_handler_registered) {
		ch->pp_handler_registered = spdk_thread_post_poller_handler_register(accel_mlx5_pp_handler,
					    ch) == 0;
	}
	rc = g_accel_mlx5_tasks_ops[mlx5_task->mlx5_opcode].process(mlx5_task);

	return rc;
}

static inline int
accel_mlx5_execute_sequence(struct spdk_io_channel *ch, struct spdk_accel_sequence *seq)
{
	struct spdk_accel_task *task;
	struct accel_mlx5_task *mlx5_task;

	task = spdk_accel_sequence_first_task(seq);
	mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	mlx5_task->flags.bits.driver_seq = 1;
	SPDK_DEBUGLOG(accel_mlx5, "new driver seq %p, ch %p, task %p\n", seq, ch, task);

	return accel_mlx5_submit_tasks(ch, task);
}

static inline void
accel_mlx5_task_clear_mkey_cache(struct accel_mlx5_task *task, struct accel_mlx5_qp *qp)
{
	struct spdk_accel_task *next_task;

	if (task->qp != qp) {
		return;
	}
	if (task->base.cached_lkey) {
		*task->base.cached_lkey = 0;
	}
	/* Clear the mkey cache when the decrypt task is merged into check CRC. */
	if (task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT) {
		next_task = TAILQ_NEXT(&task->base, seq_link);
		if (next_task->cached_lkey) {
			*next_task->cached_lkey = 0;
		}
	}
}

static void accel_mlx5_recover_qp(struct accel_mlx5_qp *qp);

static int
accel_mlx5_recover_qp_poller(void *arg)
{
	struct accel_mlx5_qp *qp = arg;

	spdk_poller_unregister(&qp->recover_poller);
	accel_mlx5_recover_qp(qp);
	return SPDK_POLLER_BUSY;
}

static void
accel_mlx5_recover_qp(struct accel_mlx5_qp *qp)
{
	struct accel_mlx5_task *task;
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_mlx5_qp_attr mlx5_qp_attr = {};
	int rc;

	SPDK_NOTICELOG("Recovering qp %p, core %u\n", qp, spdk_env_get_current_core());
	if (qp->qp) {
		spdk_mlx5_qp_destroy(qp->qp);
		qp->qp = NULL;
	}
	/* There is a good chance that WR failure was caused by invalidated cached mkey.
	 * Clear the cache to avoid new failures. We clear cache for all tasks here,
	 * including ones queued in nomem queue. This may clear mkeys that are still
	 * valid, but it is better than triggering another QP recovery. Caches will be
	 * refilled quickly.
	 */
	STAILQ_FOREACH(task, &dev->nomem, link) {
		accel_mlx5_task_clear_mkey_cache(task, qp);
	}
	if (qp->domain) {
		/* No need to re-create a qp created for a specific domain, it will be created when needed */
		assert(qp != &qp->dev->mlx5_qp);
		RB_REMOVE(accel_mlx5_qpairs_map, &dev->qpairs_map, qp);
		free(qp);
		return;
	}

	mlx5_qp_attr.cap.max_send_wr = g_accel_mlx5.qp_size;
	mlx5_qp_attr.cap.max_recv_wr = 0;
	mlx5_qp_attr.cap.max_send_sge = ACCEL_MLX5_MAX_SGE;
	mlx5_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_SGE;
	mlx5_qp_attr.siglast = g_accel_mlx5.siglast;

	rc = spdk_mlx5_qp_create(dev->pd_ref, dev->cq, &mlx5_qp_attr, &qp->qp);
	if (rc) {
		SPDK_ERRLOG("Failed to create mlx5 dma QP, rc %d. Retry in %d usec\n",
			    rc, ACCEL_MLX5_RECOVER_POLLER_PERIOD_US);
		qp->recover_poller = SPDK_POLLER_REGISTER(accel_mlx5_recover_qp_poller, qp,
				     ACCEL_MLX5_RECOVER_POLLER_PERIOD_US);
		return;
	}

	qp->recovering = false;
}

static inline void
accel_mlx5_process_error_cpl(struct spdk_mlx5_cq_completion *wc, struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;

	if (wc->status != IBV_WC_WR_FLUSH_ERR) {
		SPDK_WARNLOG("RDMA: qp %p, task %p, WC status %d, core %u\n",
			     qp, task, wc->status, spdk_env_get_current_core());
	} else {
		SPDK_DEBUGLOG(accel_mlx5,
			      "RDMA: qp %p, task %p, WC status %d, core %u\n",
			      qp, task, wc->status, spdk_env_get_current_core());
	}
	/* Check if SIGERR CQE happened before the WQE error or flush.
	 * It is needed to recover the affected MKey and PSV properly.
	 */
	if (task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C) {
		accel_mlx5_task_check_sigerr(task);
	}

	qp->recovering = true;
	assert(task->num_completed_reqs <= task->num_submitted_reqs);
	if (task->num_completed_reqs == task->num_submitted_reqs) {
		accel_mlx5_task_fail(task, -EIO);
	}
	if (qp->wrs_submitted == 0) {
		assert(STAILQ_EMPTY(&qp->in_hw));
		accel_mlx5_recover_qp(qp);
	}
}

static inline void
accel_mlx5_process_cpls_siglast(struct accel_mlx5_dev *dev, struct spdk_mlx5_cq_completion *wc,
				int reaped)
{
	struct accel_mlx5_task *task, *signaled_task, *task_tmp;
	struct accel_mlx5_qp *qp;
	uint32_t completed;
	int i, rc;

	for (i = 0; i < reaped; i++) {
		if (spdk_unlikely(wc[i].status == MLX5_CQE_SYNDROME_SIGERR)) {
			struct spdk_mlx5_mkey_pool_obj *mkey;

			/* We don't know which pool (sig or crypto_sig) this mkey belongs to, so try both */
			mkey = spdk_mlx5_mkey_pool_find_mkey_by_id(dev->crypto_sig_mkeys, wc[i].mkey);
			if (!mkey) {
				mkey = spdk_mlx5_mkey_pool_find_mkey_by_id(dev->sig_mkeys, wc[i].mkey);
			}
			assert(mkey);
			mkey->sig.sigerr_count++;
			mkey->sig.sigerr = true;
			continue;
		}

		signaled_task = (struct accel_mlx5_task *)wc[i].wr_id;
		if (spdk_unlikely(!signaled_task)) {
			/* That is unsignaled completion with error, just ignore it */
			continue;
		}

		qp = signaled_task->qp;
		STAILQ_FOREACH_SAFE(task, &qp->in_hw, link, task_tmp) {
			STAILQ_REMOVE_HEAD(&qp->in_hw, link);
			assert(task->num_submitted_reqs > task->num_completed_reqs);
			completed = task->num_submitted_reqs - task->num_completed_reqs;
			assert(qp->wrs_submitted >= task->num_wrs);
			qp->wrs_submitted -= task->num_wrs;
			assert(dev->wrs_in_cq > 0);
			dev->wrs_in_cq--;
			task->num_completed_reqs += completed;
			SPDK_DEBUGLOG(accel_mlx5, "task %p, remaining %u\n", task,
				      task->num_reqs - task->num_completed_reqs);
			if (spdk_unlikely(wc[i].status) && (signaled_task == task)) {
				/* We may have X unsignaled tasks queued in in_hw, if an error happens,
				 * then HW generates completions for every unsignaled WQE.
				 * If cpl with error generated for task X+1 then we still can process
				 * previous tasks as usual */
				accel_mlx5_process_error_cpl(&wc[i], task);
				break;
			}

			if (task->num_completed_reqs == task->num_reqs) {
				accel_mlx5_task_complete(task);
			} else if (task->num_completed_reqs == task->num_submitted_reqs) {
				assert(task->num_submitted_reqs < task->num_reqs);
				rc = accel_mlx5_task_continue(task);
				if (spdk_unlikely(rc)) {
					if (rc != -ENOMEM) {
						accel_mlx5_task_fail(task, rc);
					}
					break;
				}
			}
			if (task == signaled_task) {
				break;
			}
		}
	}
}

static inline void
accel_mlx5_process_cpls(struct accel_mlx5_dev *dev, struct spdk_mlx5_cq_completion *wc, int reaped)
{
	struct accel_mlx5_task *task;
	struct accel_mlx5_qp *qp;
	uint32_t completed;
	int i, rc;

	for (i = 0; i < reaped; i++) {
		if (spdk_unlikely(wc[i].status == MLX5_CQE_SYNDROME_SIGERR)) {
			struct spdk_mlx5_mkey_pool_obj *mkey;

			/* We don't know which pool (sig or crypto_sig) this mkey belongs to, so try both */
			mkey = spdk_mlx5_mkey_pool_find_mkey_by_id(dev->crypto_sig_mkeys, wc[i].mkey);
			if (!mkey) {
				mkey = spdk_mlx5_mkey_pool_find_mkey_by_id(dev->sig_mkeys, wc[i].mkey);
			}
			assert(mkey);
			mkey->sig.sigerr_count++;
			mkey->sig.sigerr = true;
			continue;
		}

		task = (struct accel_mlx5_task *)wc[i].wr_id;
		if (spdk_unlikely(!task)) {
			/* That is unsignaled completion with error, just ignore it */
			continue;
		}

		qp = task->qp;
		assert(task == STAILQ_FIRST(&qp->in_hw) && "submission mismatch");
		STAILQ_REMOVE_HEAD(&qp->in_hw, link);
		assert(task->num_submitted_reqs > task->num_completed_reqs);
		completed = task->num_submitted_reqs - task->num_completed_reqs;
		assert(qp->wrs_submitted >= task->num_wrs);
		qp->wrs_submitted -= task->num_wrs;
		assert(dev->wrs_in_cq > 0);
		dev->wrs_in_cq--;
		task->num_completed_reqs += completed;
		SPDK_DEBUGLOG(accel_mlx5, "task %p, remaining %u\n", task,
			      task->num_reqs - task->num_completed_reqs);

		if (spdk_unlikely(wc[i].status)) {
			accel_mlx5_process_error_cpl(&wc[i], task);
			continue;
		}

		if (task->num_completed_reqs == task->num_reqs) {
			accel_mlx5_task_complete(task);
		} else if (task->num_completed_reqs == task->num_submitted_reqs) {
			assert(task->num_submitted_reqs < task->num_reqs);
			rc = accel_mlx5_task_continue(task);
			if (spdk_unlikely(rc)) {
				if (rc != -ENOMEM) {
					accel_mlx5_task_fail(task, rc);
				}
				break;
			}
		}
	}
}

static inline int64_t
accel_mlx5_poll_cq(struct accel_mlx5_dev *dev)
{
	struct spdk_mlx5_cq_completion wc[ACCEL_MLX5_MAX_WC];
	int reaped;

	dev->stats.polls++;
	reaped = spdk_mlx5_cq_poll_completions(dev->cq, wc, ACCEL_MLX5_MAX_WC);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return reaped;
	} else if (reaped == 0) {
		dev->stats.idle_polls++;
		return 0;
	}

	dev->stats.completions += reaped;
	SPDK_DEBUGLOG(accel_mlx5, "Reaped %d cpls on dev %s\n", reaped,
		      dev->pd_ref->context->device->name);

	g_accel_mlx5_process_cpl_fn(dev, wc, reaped);

	return reaped;
}

static inline void
accel_mlx5_resubmit_nomem_tasks(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_task *task, *tmp, *last;
	int rc;

	last = STAILQ_LAST(&dev->nomem, accel_mlx5_task, link);
	STAILQ_FOREACH_SAFE(task, &dev->nomem, link, tmp) {
		STAILQ_REMOVE_HEAD(&dev->nomem, link);
		rc = accel_mlx5_task_continue(task);
		if (spdk_unlikely(rc)) {
			if (rc != -ENOMEM) {
				accel_mlx5_task_fail(task, rc);
			}
			break;
		}
		/* If qpair is recovering, task is added back to the nomem list and 0 is returned. In that case we
		 * need a special condition to iterate the list once and stop this FOREACH loop */
		if (task == last) {
			break;
		}
	}
}

static int
accel_mlx5_poller(void *ctx)
{
	struct accel_mlx5_io_channel *ch = ctx;
	struct accel_mlx5_dev *dev;

	int64_t completions = 0, rc;
	uint32_t i;

	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		rc = 0;

		if (dev->wrs_in_cq) {
			rc = accel_mlx5_poll_cq(dev);
		}

		if (spdk_unlikely(rc < 0)) {
			SPDK_ERRLOG("Error %"PRId64" on CQ, dev %s\n", rc,
				    dev->pd_ref->context->device->name);
			continue;
		}
		completions += rc;

		if (!STAILQ_EMPTY(&dev->nomem)) {
			accel_mlx5_resubmit_nomem_tasks(dev);
		}
	}

	return !!completions;
}

static void
accel_mlx5_pp_handler(void *ctx)
{
	struct accel_mlx5_io_channel *ch = ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;
	int rc;

	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		rc = 0;

		if (dev->wrs_in_cq) {
			rc = spdk_mlx5_cq_flush_doorbells(dev->cq);
		}

		if (spdk_unlikely(rc < 0)) {
			SPDK_ERRLOG("Error %d on CQ, dev %s\n", rc, dev->pd_ref->context->device->name);
			continue;
		}
	}

	ch->pp_handler_registered = false;
}

static bool
accel_mlx5_supports_opcode(enum spdk_accel_opcode opc)
{
	assert(g_accel_mlx5.enabled);

	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
		return true;
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
		return g_accel_mlx5.crypto_supported;
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_COPY_CRC32C:
	case SPDK_ACCEL_OPC_CHECK_CRC32C:
		return g_accel_mlx5.crc_supported;
	default:
		return false;
	}
}

static struct spdk_io_channel *
accel_mlx5_get_io_channel(void)
{
	assert(g_accel_mlx5.enabled);
	return spdk_get_io_channel(&g_accel_mlx5);
}

static void
accel_mlx5_dev_destroy_qps(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_qp *qpair, *tmp_qpair;

	if (dev->mlx5_qp.qp) {
		spdk_mlx5_qp_destroy(dev->mlx5_qp.qp);
	}

	RB_FOREACH_SAFE(qpair, accel_mlx5_qpairs_map, &dev->qpairs_map, tmp_qpair) {
		if (qpair->dev == dev) {
			RB_REMOVE(accel_mlx5_qpairs_map, &dev->qpairs_map, qpair);
			spdk_mlx5_qp_destroy(qpair->qp);
			free(qpair);
		}
	}
}

static void
accel_mlx5_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_dev *dev;
	uint32_t i;

	spdk_poller_unregister(&ch->poller);
	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		accel_mlx5_dev_destroy_qps(dev);
		if (dev->cq) {
			spdk_mlx5_cq_destroy(dev->cq);
		}
		spdk_poller_unregister(&dev->mlx5_qp.recover_poller);
		if (dev->crypto_mkeys) {
			spdk_mlx5_mkey_pool_put_channel(dev->crypto_mkeys);
		}
		if (dev->sig_mkeys) {
			spdk_mlx5_mkey_pool_put_channel(dev->sig_mkeys);
		}
		if (dev->crypto_sig_mkeys) {
			spdk_mlx5_mkey_pool_put_channel(dev->crypto_sig_mkeys);
		}

		spdk_spin_lock(&g_accel_mlx5.lock);
		accel_mlx5_add_stats(&g_accel_mlx5.stats, &dev->stats);
		spdk_spin_unlock(&g_accel_mlx5.lock);
	}
	free(ch->devs);
}

static int
accel_mlx5_create_qp(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp)
{
	struct spdk_mlx5_qp_attr mlx5_qp_attr = {};
	int rc;

	mlx5_qp_attr.cap.max_send_wr = g_accel_mlx5.qp_size;
	mlx5_qp_attr.cap.max_recv_wr = 0;
	mlx5_qp_attr.cap.max_send_sge = ACCEL_MLX5_MAX_SGE;
	mlx5_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_SGE;
	mlx5_qp_attr.siglast = g_accel_mlx5.siglast;

	rc = spdk_mlx5_qp_create(dev->pd_ref, dev->cq, &mlx5_qp_attr, &qp->qp);
	if (rc) {
		return rc;
	}

	STAILQ_INIT(&qp->in_hw);
	qp->dev = dev;
	qp->max_wrs = g_accel_mlx5.qp_size;

	return 0;
}

static int
accel_mlx5_create_cb(void *io_device, void *ctx_buf)
{
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_dev_ctx *dev_ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;
	int rc;

	ch->devs = calloc(g_accel_mlx5.num_devs, sizeof(*ch->devs));
	if (!ch->devs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_accel_mlx5.num_devs; i++) {
		struct spdk_mlx5_cq_attr mlx5_cq_attr = {};

		dev_ctx = &g_accel_mlx5.devices[i];
		dev = &ch->devs[i];
		dev->psv_pool_ref = dev_ctx->psv_pool;
		dev->pd_ref = dev_ctx->pd;
		dev->domain_ref = dev_ctx->domain->domain;
		dev->crypto_multi_block = dev_ctx->crypto_multi_block;
		dev->map_ref = dev_ctx->map;
		dev->ch = spdk_io_channel_from_ctx(ctx_buf);
		RB_INIT(&dev->qpairs_map);

		if (dev_ctx->crypto_mkeys) {
			dev->crypto_mkeys = spdk_mlx5_mkey_pool_get_channel(dev->pd_ref, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
			if (!dev->crypto_mkeys) {
				SPDK_ERRLOG("Failed to get crypto mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}
		if (dev_ctx->sig_mkeys) {
			dev->sig_mkeys = spdk_mlx5_mkey_pool_get_channel(dev->pd_ref, SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
			if (!dev->sig_mkeys) {
				SPDK_ERRLOG("Failed to get sig mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}
		if (dev_ctx->crypto_sig_mkeys) {
			dev->crypto_sig_mkeys = spdk_mlx5_mkey_pool_get_channel(dev->pd_ref,
						SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
			if (!dev->crypto_sig_mkeys) {
				SPDK_ERRLOG("Failed to get crypto_sig mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}

		ch->num_devs++;

		mlx5_cq_attr.cqe_cnt = g_accel_mlx5.cq_size;
		mlx5_cq_attr.cqe_size = 64;
		mlx5_cq_attr.cq_context = dev;

		rc = spdk_mlx5_cq_create(dev->pd_ref, &mlx5_cq_attr, &dev->cq);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 CQ, rc %d\n", rc);
			goto err_out;
		}

		rc = accel_mlx5_create_qp(dev, &dev->mlx5_qp);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 QP, rc %d\n", rc);
			goto err_out;
		}

		dev->max_wrs_in_cq = g_accel_mlx5.cq_size;
		STAILQ_INIT(&dev->nomem);
	}

	ch->poller = SPDK_POLLER_REGISTER(accel_mlx5_poller, ch, 0);

	return 0;

err_out:
	accel_mlx5_destroy_cb(&g_accel_mlx5, ctx_buf);
	return rc;
}

void
accel_mlx5_get_default_attr(struct accel_mlx5_attr *attr)
{
	assert(attr);

	memset(attr, 0, sizeof(*attr));

	attr->qp_size = ACCEL_MLX5_QP_SIZE;
	attr->cq_size = ACCEL_MLX5_CQ_SIZE;
	attr->num_requests = ACCEL_MLX5_NUM_MKEYS;
	attr->split_mb_blocks = 0;
	attr->siglast = false;
	attr->enable_driver = false;
	attr->qp_per_domain = true;
}

static void
accel_mlx5_allowed_devs_free(void)
{
	size_t i;

	if (!g_accel_mlx5.allowed_devs || !g_accel_mlx5.allowed_devs_count) {
		return;
	}

	for (i = 0; i < g_accel_mlx5.allowed_devs_count; i++) {
		free(g_accel_mlx5.allowed_devs[i]);
	}
	free(g_accel_mlx5.allowed_devs);
	free(g_accel_mlx5.allowed_devs_str);
	g_accel_mlx5.allowed_devs = NULL;
	g_accel_mlx5.allowed_devs_count = 0;
}

static int
accel_mlx5_allowed_devs_parse(const char *allowed_devs)
{
	char *str, *tmp, *tok;
	size_t devs_count = 0;

	str = strdup(allowed_devs);
	if (!str) {
		return -ENOMEM;
	}

	accel_mlx5_allowed_devs_free();

	tmp = str;
	while ((tmp = strchr(tmp, ',')) != NULL) {
		tmp++;
		devs_count++;
	}
	devs_count++;

	g_accel_mlx5.allowed_devs = calloc(devs_count, sizeof(char *));
	if (!g_accel_mlx5.allowed_devs) {
		free(str);
		return -ENOMEM;
	}

	devs_count = 0;
	tok = strtok(str, ",");
	while (tok) {
		g_accel_mlx5.allowed_devs[devs_count] = strdup(tok);
		if (!g_accel_mlx5.allowed_devs[devs_count]) {
			free(str);
			accel_mlx5_allowed_devs_free();
			return -ENOMEM;
		}
		tok = strtok(NULL, ",");
		devs_count++;
		g_accel_mlx5.allowed_devs_count++;
	}

	free(str);

	return 0;
}

int
accel_mlx5_enable(struct accel_mlx5_attr *attr)
{
	if (attr) {
		/* Copy attributes */
		g_accel_mlx5.qp_size = attr->qp_size;
		g_accel_mlx5.cq_size = attr->cq_size;
		g_accel_mlx5.num_requests = attr->num_requests;
		g_accel_mlx5.split_mb_blocks = attr->split_mb_blocks;
		g_accel_mlx5.siglast = attr->siglast;
		g_accel_mlx5.qp_per_domain = attr->qp_per_domain;
		g_accel_mlx5.enable_driver = attr->enable_driver;

		if (attr->allowed_devs) {
			int rc;

			g_accel_mlx5.allowed_devs_str = strdup(attr->allowed_devs);
			if (!g_accel_mlx5.allowed_devs_str) {
				return -ENOMEM;
			}
			rc = accel_mlx5_allowed_devs_parse(attr->allowed_devs);
			if (rc) {
				return rc;
			}
			rc = spdk_mlx5_crypto_devs_allow((const char *const *)g_accel_mlx5.allowed_devs,
							 g_accel_mlx5.allowed_devs_count);
			if (rc) {
				accel_mlx5_allowed_devs_free();
				return rc;
			}
		}
	}

	g_accel_mlx5.enabled = true;

	return 0;
}

static void
accel_mlx5_psvs_release(struct accel_mlx5_dev_ctx *dev_ctx)
{
	uint32_t i, num_psvs, num_psvs_in_pool;

	spdk_dma_free(dev_ctx->crc_dma_buf);

	if (!dev_ctx->psvs) {
		return;
	}

	num_psvs = dev_ctx->num_mkeys;

	for (i = 0; i < num_psvs; i++) {
		if (dev_ctx->psvs[i]) {
			spdk_mlx5_destroy_psv(dev_ctx->psvs[i]);
			dev_ctx->psvs[i] = NULL;
		}
	}

	if (!dev_ctx->psv_pool) {
		return;
	}

	num_psvs_in_pool = spdk_mempool_count(dev_ctx->psv_pool);
	if (num_psvs_in_pool != num_psvs) {
		SPDK_ERRLOG("Expected %u reqs in the pool, but got only %u\n", num_psvs, num_psvs_in_pool);
	}
	spdk_mempool_free(dev_ctx->psv_pool);
	free(dev_ctx->psvs);
}

static void
accel_mlx5_free_resources(void)
{
	struct accel_mlx5_dev_ctx *dev;
	uint32_t i;

	for (i = 0; i < g_accel_mlx5.num_devs; i++) {
		dev = &g_accel_mlx5.devices[i];
		accel_mlx5_psvs_release(dev);
		spdk_rdma_utils_put_memory_domain(dev->domain);
		spdk_rdma_utils_free_mem_map(&dev->map);
		if (dev->sig_mkeys) {
			spdk_mlx5_mkey_pools_destroy(&dev->pd, 1, SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
		}
		if (dev->crypto_mkeys) {
			spdk_mlx5_mkey_pools_destroy(&dev->pd, 1, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
		}
		if (dev->crypto_sig_mkeys) {
			spdk_mlx5_mkey_pools_destroy(&dev->pd, 1,
						     SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
		}
		spdk_rdma_utils_put_pd(dev->pd);
	}
	free(g_accel_mlx5.devices);
	g_accel_mlx5.devices = NULL;
	g_accel_mlx5.initialized = false;
}

static void
accel_mlx5_deinit_cb(void *ctx)
{
	struct accel_mlx5_stats *stats = &g_accel_mlx5.stats;

	SPDK_NOTICELOG("mlx5 stats: umrs: crypto %lu, sig %lu, crypto+sig %lu, total %lu;\n"
		       "rdma: writes %lu, reads %lu, total %lu, polls %lu, idle_polls %lu, completions %lu\n",
		       stats->crypto_umrs, stats->sig_umrs, stats->sig_crypto_umrs,
		       stats->crypto_umrs + stats->sig_umrs + stats->sig_crypto_umrs,
		       stats->rdma_writes, stats->rdma_reads, stats->rdma_writes + stats->rdma_reads,
		       stats->polls, stats->idle_polls, stats->completions);

	accel_mlx5_free_resources();
	spdk_spin_destroy(&g_accel_mlx5.lock);
	spdk_accel_module_finish();
}

static void
accel_mlx5_deinit(void *ctx)
{
	spdk_memory_domain_update_notification_unsubscribe(&g_accel_mlx5);
	if (g_accel_mlx5.allowed_devs) {
		accel_mlx5_allowed_devs_free();
	}
	spdk_mlx5_crypto_devs_allow(NULL, 0);
	if (g_accel_mlx5.initialized) {
		spdk_io_device_unregister(&g_accel_mlx5, accel_mlx5_deinit_cb);
	} else {
		spdk_accel_module_finish();
	}
}

static int
accel_mlx5_mkeys_create(struct accel_mlx5_dev_ctx *dev_ctx, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool_param pool_param = {};

	pool_param.mkey_count = dev_ctx->num_mkeys;
	pool_param.cache_per_thread = dev_ctx->num_mkeys * 3 / 4 / spdk_env_get_core_count();
	pool_param.flags = flags;

	return spdk_mlx5_mkey_pools_init(&pool_param, &dev_ctx->pd, 1);
}

static void
accel_mlx5_set_psv_in_pool(struct spdk_mempool *mp, void *cb_arg, void *_psv, unsigned obj_idx)
{
	struct spdk_rdma_utils_memory_translation translation = {};
	struct accel_mlx5_psv_pool_iter_cb_args *args = cb_arg;
	struct accel_mlx5_psv_wrapper *wrapper = _psv;
	struct accel_mlx5_dev_ctx *dev_ctx = args->dev;
	int rc;

	if (args->rc) {
		return;
	}
	assert(obj_idx < dev_ctx->num_mkeys);
	assert(dev_ctx->psvs[obj_idx] != NULL);
	memset(wrapper, 0, sizeof(*wrapper));
	wrapper->psv_index = dev_ctx->psvs[obj_idx]->index;
	wrapper->crc = &dev_ctx->crc_dma_buf[obj_idx];

	rc = spdk_rdma_utils_get_translation(dev_ctx->map, wrapper->crc, sizeof(uint32_t), &translation);
	if (rc) {
		SPDK_ERRLOG("Memory translation failed, addr %p, length %zu\n", wrapper->crc, sizeof(uint32_t));
		args->rc = -EINVAL;
	} else {
		wrapper->crc_lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
	}
}

static int
accel_mlx5_psvs_create(struct accel_mlx5_dev_ctx *dev_ctx)
{
	struct accel_mlx5_psv_pool_iter_cb_args args = {
		.dev = dev_ctx
	};
	char pool_name[32];
	uint32_t i;
	uint32_t num_psvs = dev_ctx->num_mkeys;
	int rc;

	dev_ctx->crc_dma_buf = spdk_dma_malloc(sizeof(uint32_t) * num_psvs, sizeof(uint32_t), NULL);
	if (!dev_ctx->crc_dma_buf) {
		SPDK_ERRLOG("Failed to allocate memory for CRC DMA buffer\n");
		return -ENOMEM;
	}
	dev_ctx->psvs = calloc(num_psvs, (sizeof(struct spdk_mlx5_psv *)));
	if (!dev_ctx->psvs) {
		SPDK_ERRLOG("Failed to alloc PSVs array\n");
		return -ENOMEM;
	}
	for (i = 0; i < num_psvs; i++) {
		dev_ctx->psvs[i] = spdk_mlx5_create_psv(dev_ctx->pd);
		if (!dev_ctx->psvs[i]) {
			SPDK_ERRLOG("Failed to create PSV on dev %s\n", dev_ctx->context->device->name);
			return -EINVAL;
		}
	}

	rc = snprintf(pool_name, sizeof(pool_name), "accel_psv_%s", dev_ctx->context->device->name);
	if (rc < 0) {
		assert(0);
		return -EINVAL;
	}
	uint32_t cache_size = dev_ctx->num_mkeys / 4 * 3 / spdk_env_get_core_count();
	SPDK_NOTICELOG("PSV pool name %s size %u cache size %u\n", pool_name, num_psvs, cache_size);
	dev_ctx->psv_pool = spdk_mempool_create_ctor(pool_name, num_psvs,
			    sizeof(struct accel_mlx5_psv_wrapper),
			    cache_size, SPDK_ENV_SOCKET_ID_ANY,
			    accel_mlx5_set_psv_in_pool, &args);
	if (!dev_ctx->psv_pool) {
		SPDK_ERRLOG("Failed to create PSV memory pool\n");
		return -ENOMEM;
	}
	if (args.rc) {
		SPDK_ERRLOG("Failed to init PSV memory pool objects, rc %d\n", args.rc);
		return args.rc;
	}

	return 0;
}

static int
accel_mlx5_dev_ctx_init(struct accel_mlx5_dev_ctx *dev_ctx, struct ibv_context *dev,
			struct spdk_mlx5_crypto_caps *crypto_caps)
{
	struct ibv_pd *pd;
	int rc;

	pd = spdk_rdma_utils_get_pd(dev);
	if (!pd) {
		SPDK_ERRLOG("Failed to get PD for context %p, dev %s\n", dev, dev->device->name);
		return -EINVAL;
	}
	dev_ctx->context = dev;
	dev_ctx->pd = pd;
	dev_ctx->num_mkeys = g_accel_mlx5.num_requests;
	dev_ctx->domain = spdk_rdma_utils_get_memory_domain(pd, SPDK_DMA_DEVICE_TYPE_RDMA);
	if (!dev_ctx->domain) {
		return -ENOMEM;
	}
	dev_ctx->map = spdk_rdma_utils_create_mem_map(pd, NULL,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!dev_ctx->map) {
		return -ENOMEM;
	}

	if (g_accel_mlx5.crypto_supported) {
		assert(crypto_caps);
		rc = accel_mlx5_mkeys_create(dev_ctx, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
		if (rc) {
			SPDK_ERRLOG("Failed to create crypto mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
		dev_ctx->crypto_mkeys = true;
		/* Explicitly disabled by default */
		dev_ctx->crypto_multi_block = false;
		if (crypto_caps->multi_block_be_tweak) {
			/* TODO: multi_block LE tweak will be checked later once LE BSF is fixed */
			dev_ctx->crypto_multi_block = true;
		} else if (g_accel_mlx5.split_mb_blocks) {
			SPDK_WARNLOG("\"split_mb_block\" is set but dev %s doesn't support multi block crypto\n",
				     dev->device->name);
		}
		SPDK_DEBUGLOG(accel_mlx5, "dev %s, crypto: sb_le %d, mb_be %d, mb_le %d, inc_64 %d\n",
			      dev->device->name,
			      crypto_caps->single_block_le_tweak,
			      crypto_caps->multi_block_be_tweak,
			      crypto_caps->multi_block_le_tweak,
			      crypto_caps->tweak_inc_64);
	}

	if (g_accel_mlx5.crc_supported) {
		rc = accel_mlx5_mkeys_create(dev_ctx, SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
		if (rc) {
			SPDK_ERRLOG("Failed to create sig mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
		dev_ctx->sig_mkeys = true;

		rc = accel_mlx5_psvs_create(dev_ctx);
		if (rc) {
			SPDK_ERRLOG("Failed to create PSVs pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
	}
	if (g_accel_mlx5.enable_driver) {
		if (g_accel_mlx5.crypto_supported && g_accel_mlx5.crc_supported) {
			rc = accel_mlx5_mkeys_create(dev_ctx,
						     SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
			if (rc) {
				SPDK_ERRLOG("Failed to create crypto_sig mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
				return rc;
			}
			dev_ctx->crypto_sig_mkeys = true;
			g_accel_mlx5.merge = true;
			SPDK_NOTICELOG("driver enabled, crypto and crc merge supported\n");
		} else {
			g_accel_mlx5.merge = false;
			SPDK_NOTICELOG("driver enabled, but crypto and crc merge is not supported\n");
		}
	}

	return 0;
}

static struct ibv_context **
accel_mlx5_get_devices(int *_num_devs)
{
	struct ibv_context **rdma_devs, **rdma_devs_out = NULL, *dev;
	struct ibv_device_attr dev_attr;
	size_t j;
	int num_devs = 0, i, rc;
	int num_devs_out = 0;
	bool dev_allowed;

	rdma_devs = rdma_get_devices(&num_devs);
	if (!rdma_devs || !num_devs) {
		*_num_devs = 0;
		return NULL;
	}

	rdma_devs_out = calloc(num_devs + 1, sizeof(struct ibv_context *));
	if (!rdma_devs_out) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rdma_free_devices(rdma_devs);
		*_num_devs = 0;
		return NULL;
	}

	for (i = 0; i < num_devs; i++) {
		dev = rdma_devs[i];
		rc = ibv_query_device(dev, &dev_attr);
		if (rc) {
			SPDK_ERRLOG("Failed to query dev %s, skipping\n", dev->device->name);
			continue;
		}
		if (dev_attr.vendor_id != SPDK_MLX5_VENDOR_ID_MELLANOX) {
			SPDK_DEBUGLOG(accel_mlx5, "dev %s is not Mellanox device, skipping\n", dev->device->name);
			continue;
		}

		if (g_accel_mlx5.allowed_devs_count) {
			dev_allowed = false;
			for (j = 0; j < g_accel_mlx5.allowed_devs_count; j++) {
				if (strcmp(g_accel_mlx5.allowed_devs[j], dev->device->name) == 0) {
					dev_allowed = true;
					break;
				}
			}
			if (!dev_allowed) {
				continue;
			}
		}

		rdma_devs_out[num_devs_out] = dev;
		num_devs_out++;
	}

	rdma_free_devices(rdma_devs);
	*_num_devs = num_devs_out;

	return rdma_devs_out;
}

static inline bool
accel_mlx5_dev_supports_crypto(struct spdk_mlx5_crypto_caps *caps)
{
	return caps->crypto && !caps->wrapped_import_method_aes_xts && (caps->single_block_le_tweak ||
			caps->multi_block_le_tweak || caps->multi_block_be_tweak);
}

static int
accel_mlx5_init(void)
{
	struct ibv_context **rdma_devs, *dev;
	struct spdk_mlx5_crypto_caps *crypto_caps = NULL;
	int num_devs = 0, rc = 0, i;
	int best_dev = -1, first_dev = 0;
	int best_dev_stat = 0, dev_stat;
	bool find_best_dev = g_accel_mlx5.allowed_devs_count == 0;
	bool supports_crypto;

	if (!g_accel_mlx5.enabled) {
		return -EINVAL;
	}

	spdk_spin_init(&g_accel_mlx5.lock);

	if (g_accel_mlx5.siglast) {
		g_accel_mlx5_process_cpl_fn = accel_mlx5_process_cpls_siglast;
	} else {
		g_accel_mlx5_process_cpl_fn = accel_mlx5_process_cpls;
	}

	g_accel_mlx5.crypto_supported = true;
	g_accel_mlx5.crc_supported = true;
	g_accel_mlx5.num_devs = 0;

	rdma_devs = accel_mlx5_get_devices(&num_devs);
	if (!rdma_devs || !num_devs) {
		SPDK_ERRLOG("No Nvidia RDMA devices found");
		return -ENODEV;
	}

	crypto_caps = calloc(num_devs, sizeof(*crypto_caps));
	if (!crypto_caps) {
		rc = -ENOMEM;
		goto cleanup;
	}

	/* Iterate devices. We support an offload if all devices support it */
	for (i = 0; i < num_devs; i++) {
		dev = rdma_devs[i];

		rc = spdk_mlx5_query_crypto_caps(dev, &crypto_caps[i]);
		if (rc) {
			SPDK_ERRLOG("Failed to get crypto caps, dev %s\n", dev->device->name);
			goto cleanup;
		}
		supports_crypto = accel_mlx5_dev_supports_crypto(&crypto_caps[i]);
		if (!supports_crypto) {
			SPDK_DEBUGLOG(accel_mlx5, "Disable crypto support because dev %s doesn't support it\n",
				      rdma_devs[i]->device->name);
			g_accel_mlx5.crypto_supported = false;
		}
		if (!crypto_caps[i].crc32c) {
			SPDK_DEBUGLOG(accel_mlx5, "Disable crc32c support because dev %s doesn't support it\n",
				      rdma_devs[i]->device->name);
			g_accel_mlx5.crc_supported = false;
		}
		if (find_best_dev) {
			/* Find device which supports max number of offloads */
			dev_stat = (int)supports_crypto + crypto_caps[i].crc32c;
			if (dev_stat > best_dev_stat) {
				best_dev_stat = dev_stat;
				best_dev = i;
			}
		}
	}

	/* User didn't specify devices to use, we select the best one. */
	if (find_best_dev) {
		if (best_dev == -1) {
			best_dev = 0;
		}
		supports_crypto = accel_mlx5_dev_supports_crypto(&crypto_caps[best_dev]);
		SPDK_NOTICELOG("Select dev %s, crypto %d, crc %d\n", rdma_devs[best_dev]->device->name,
			       supports_crypto, crypto_caps[best_dev].crc32c);
		g_accel_mlx5.crypto_supported = supports_crypto;
		g_accel_mlx5.crc_supported = crypto_caps[best_dev].crc32c;
		first_dev = best_dev;
		num_devs = 1;
		if (supports_crypto) {
			const char *const dev_name[] = { rdma_devs[best_dev]->device->name };

			spdk_mlx5_crypto_devs_allow(dev_name, 1);
		}
	} else {
		SPDK_NOTICELOG("Found %d devices, crypto %d, crc %d\n", num_devs, g_accel_mlx5.crypto_supported,
			       g_accel_mlx5.crc_supported);
	}

	g_accel_mlx5.devices = calloc(num_devs, sizeof(*g_accel_mlx5.devices));
	if (!g_accel_mlx5.devices) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	for (i = first_dev; i < first_dev + num_devs; i++) {
		rc = accel_mlx5_dev_ctx_init(&g_accel_mlx5.devices[g_accel_mlx5.num_devs], rdma_devs[i],
					     &crypto_caps[i]);
		if (rc) {
			goto cleanup;
		}

		g_accel_mlx5.num_devs++;
	}

	rc = spdk_memory_domain_update_notification_subscribe(&g_accel_mlx5,
			accel_mlx5_domain_notification);
	if (rc) {
		SPDK_WARNLOG("Failed to subscribe on memory domain updates (rc %d), ignoring\n", rc);
		rc = 0;
	}

	SPDK_NOTICELOG("Accel framework mlx5 initialized, found %d devices.\n", num_devs);
	spdk_io_device_register(&g_accel_mlx5, accel_mlx5_create_cb, accel_mlx5_destroy_cb,
				sizeof(struct accel_mlx5_io_channel), "accel_mlx5");

	free(crypto_caps);
	free(rdma_devs);
	g_accel_mlx5.initialized = true;

	if (g_accel_mlx5.enable_driver) {
		SPDK_NOTICELOG("Enabling mlx5 platform driver\n");
		spdk_accel_driver_register(&g_accel_mlx5_driver);
		spdk_accel_set_driver(g_accel_mlx5_driver.name);
	}

	return rc;

cleanup:
	free(crypto_caps);
	free(rdma_devs);
	accel_mlx5_free_resources();
	spdk_spin_destroy(&g_accel_mlx5.lock);

	return rc;
}

static void
accel_mlx5_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_accel_mlx5.enabled) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "mlx5_scan_accel_module");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_uint16(w, "qp_size", g_accel_mlx5.qp_size);
		spdk_json_write_named_uint16(w, "cq_size", g_accel_mlx5.cq_size);
		spdk_json_write_named_uint32(w, "num_requests", g_accel_mlx5.num_requests);
		spdk_json_write_named_bool(w, "enable_driver", g_accel_mlx5.enable_driver);
		spdk_json_write_named_uint32(w, "split_mb_blocks", g_accel_mlx5.split_mb_blocks);
		if (g_accel_mlx5.allowed_devs_str) {
			spdk_json_write_named_string(w, "allowed_devs", g_accel_mlx5.allowed_devs_str);
		}
		spdk_json_write_named_bool(w, "siglast", g_accel_mlx5.siglast);
		spdk_json_write_named_bool(w, "qp_per_domain", g_accel_mlx5.qp_per_domain);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

static size_t
accel_mlx5_get_ctx_size(void)
{
	return sizeof(struct accel_mlx5_task);
}

static int
accel_mlx5_crypto_key_init(struct spdk_accel_crypto_key *key)
{
	struct spdk_mlx5_crypto_dek_create_attr attr = {};
	struct spdk_mlx5_crypto_keytag *keytag;
	int rc;

	if (!key || !key->key || !key->key2 || !key->key_size || !key->key2_size) {
		return -EINVAL;
	}

	attr.dek = calloc(1, key->key_size + key->key2_size);
	if (!attr.dek) {
		return -ENOMEM;
	}

	memcpy(attr.dek, key->key, key->key_size);
	memcpy(attr.dek + key->key_size, key->key2, key->key2_size);
	attr.dek_len = key->key_size + key->key2_size;
	attr.tweak_upper_lba = key->tweak_mode == SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_UPPER_LBA;

	rc = spdk_mlx5_crypto_keytag_create(&attr, &keytag);
	spdk_memset_s(attr.dek, attr.dek_len, 0, attr.dek_len);
	free(attr.dek);
	if (rc) {
		SPDK_ERRLOG("Failed to create a keytag, rc %d\n", rc);
		return rc;
	}

	key->priv = keytag;

	return 0;
}

static void
accel_mlx5_crypto_key_deinit(struct spdk_accel_crypto_key *key)
{
	if (!key || key->module_if != &g_accel_mlx5.module || !key->priv) {
		return;
	}

	spdk_mlx5_crypto_keytag_destroy(key->priv);
}
static int
accel_mlx5_get_memory_domains(struct spdk_memory_domain **domains, int array_size)
{
	int i, size;

	if (!domains || !array_size) {
		return (int)g_accel_mlx5.num_devs;
	}

	size = spdk_min(array_size, (int)g_accel_mlx5.num_devs);

	for (i = 0; i < size; i++) {
		domains[i] = g_accel_mlx5.devices[i].domain->domain;
	}

	return (int)g_accel_mlx5.num_devs;
}

static bool
accel_mlx5_crypto_supports_tweak_mode(enum spdk_accel_crypto_tweak_mode tweak_mode)
{
	struct ibv_context **devs;
	struct spdk_mlx5_crypto_caps dev_caps;
	int devs_count, i, rc;
	bool upper_lba_supported;

	if (!g_accel_mlx5.crypto_supported) {
		return false;
	}

	if (tweak_mode == SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA) {
		return true;
	}
	if (tweak_mode == SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_UPPER_LBA) {
		upper_lba_supported = true;
		devs = spdk_mlx5_crypto_devs_get(&devs_count);
		assert(devs);
		for (i = 0; i < devs_count; i++) {
			rc = spdk_mlx5_query_crypto_caps(devs[i], &dev_caps);
			if (rc || !dev_caps.tweak_inc_64) {
				upper_lba_supported = false;
				break;
			}
		}
		spdk_mlx5_crypto_devs_release(devs);
		return upper_lba_supported;
	}

	return false;
}

static void
accel_mlx5_dump_stats_json(struct spdk_json_write_ctx *w, const char *header,
			   const struct accel_mlx5_stats *stats)
{
	double idle_polls_percentage = 0;
	double cpls_per_poll = 0;
	uint64_t total_tasks = 0;
	int i;

	if (stats->polls) {
		idle_polls_percentage = (double) stats->idle_polls * 100 / stats->polls;
	}
	if (stats->polls > stats->idle_polls) {
		cpls_per_poll = (double) stats->completions / (stats->polls - stats->idle_polls);
	}
	for (i = 0; i < ACCEL_MLX5_OPC_LAST; i++) {
		total_tasks += stats->opcodes[i];
	}

	spdk_json_write_named_object_begin(w, header);

	spdk_json_write_named_object_begin(w, "UMRs");
	spdk_json_write_named_uint64(w, "crypto_umrs", stats->crypto_umrs);
	spdk_json_write_named_uint64(w, "sig_umrs", stats->sig_umrs);
	spdk_json_write_named_uint64(w, "sig_crypto_umrs", stats->sig_crypto_umrs);
	spdk_json_write_named_uint64(w, "total",
				     stats->crypto_umrs + stats->sig_umrs + stats->sig_crypto_umrs);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "RDMA");
	spdk_json_write_named_uint64(w, "read", stats->rdma_reads);
	spdk_json_write_named_uint64(w, "write", stats->rdma_writes);
	spdk_json_write_named_uint64(w, "total", stats->rdma_reads + stats->rdma_writes);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "Polling");
	spdk_json_write_named_uint64(w, "polls", stats->polls);
	spdk_json_write_named_uint64(w, "idle_polls", stats->idle_polls);
	spdk_json_write_named_uint64(w, "completions", stats->completions);
	spdk_json_write_named_double(w, "idle_polls_percentage", idle_polls_percentage);
	spdk_json_write_named_double(w, "cpls_per_poll", cpls_per_poll);
	spdk_json_write_named_uint64(w, "nomem_qdepth", stats->nomem_qdepth);
	spdk_json_write_named_uint64(w, "nomem_mkey", stats->nomem_mkey);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "tasks");
	spdk_json_write_named_uint64(w, "copy", stats->opcodes[ACCEL_MLX5_OPC_COPY]);
	spdk_json_write_named_uint64(w, "crypto", stats->opcodes[ACCEL_MLX5_OPC_CRYPTO]);
	spdk_json_write_named_uint64(w, "encrypt_mkey", stats->opcodes[ACCEL_MLX5_OPC_ENCRYPT_MKEY]);
	spdk_json_write_named_uint64(w, "decrypt_mkey", stats->opcodes[ACCEL_MLX5_OPC_DECRYPT_MKEY]);
	spdk_json_write_named_uint64(w, "crc32c", stats->opcodes[ACCEL_MLX5_OPC_CRC32C]);
	spdk_json_write_named_uint64(w, "encrypt_crc", stats->opcodes[ACCEL_MLX5_OPC_ENCRYPT_AND_CRC32C]);
	spdk_json_write_named_uint64(w, "crc_decrypt", stats->opcodes[ACCEL_MLX5_OPC_CRC32C_AND_DECRYPT]);
	spdk_json_write_named_uint64(w, "total", total_tasks);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
accel_mlx5_dump_channel_stat(struct spdk_io_channel_iter *i)
{
	struct accel_mlx5_stats ch_stat = {};
	struct accel_mlx5_dump_stats_ctx *ctx;
	struct spdk_io_channel *_ch;
	struct accel_mlx5_io_channel *ch;
	struct accel_mlx5_dev *dev;
	uint32_t j;

	ctx = spdk_io_channel_iter_get_ctx(i);
	_ch = spdk_io_channel_iter_get_channel(i);
	ch = spdk_io_channel_get_ctx(_ch);

	if (ctx->level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_object_begin(ctx->w, spdk_thread_get_name(spdk_get_thread()));
	}
	if (ctx->level == ACCEL_MLX5_DUMP_STAT_LEVEL_DEV) {
		spdk_json_write_named_array_begin(ctx->w, "devices");
	}

	for (j = 0; j < ch->num_devs; j++) {
		dev = &ch->devs[j];
		/* Save grand total and channel stats */
		accel_mlx5_add_stats(&ctx->total, &dev->stats);
		accel_mlx5_add_stats(&ch_stat, &dev->stats);
		if (ctx->level == ACCEL_MLX5_DUMP_STAT_LEVEL_DEV) {
			spdk_json_write_object_begin(ctx->w);
			accel_mlx5_dump_stats_json(ctx->w, dev->pd_ref->context->device->name, &dev->stats);
			spdk_json_write_object_end(ctx->w);
		}
	}

	if (ctx->level == ACCEL_MLX5_DUMP_STAT_LEVEL_DEV) {
		spdk_json_write_array_end(ctx->w);
	}
	if (ctx->level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		accel_mlx5_dump_stats_json(ctx->w, "channel_total", &ch_stat);
		spdk_json_write_object_end(ctx->w);
		spdk_json_write_object_end(ctx->w);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
accel_mlx5_dump_channel_stat_done(struct spdk_io_channel_iter *i, int status)
{
	struct accel_mlx5_dump_stats_ctx *ctx;

	ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_spin_lock(&g_accel_mlx5.lock);
	/* Add statistics from destroyed channels */
	accel_mlx5_add_stats(&ctx->total, &g_accel_mlx5.stats);
	spdk_spin_unlock(&g_accel_mlx5.lock);

	if (ctx->level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		/* channels[] */
		spdk_json_write_array_end(ctx->w);
	}

	accel_mlx5_dump_stats_json(ctx->w, "Total", &ctx->total);

	/* Ends the whole response which was begun in accel_mlx5_dump_stats */
	spdk_json_write_object_end(ctx->w);

	ctx->cb(ctx->ctx, 0);
	free(ctx);
}

int
accel_mlx5_dump_stats(struct spdk_json_write_ctx *w, enum accel_mlx5_dump_state_level level,
		      accel_mlx5_dump_stat_done_cb cb, void *ctx)
{
	struct accel_mlx5_dump_stats_ctx *stat_ctx;

	if (!w || !cb) {
		return -EINVAL;
	}

	stat_ctx = calloc(1, sizeof(*stat_ctx));
	if (!stat_ctx) {
		return -ENOMEM;
	}
	stat_ctx->cb = cb;
	stat_ctx->ctx = ctx;
	stat_ctx->level = level;
	stat_ctx->w = w;

	spdk_json_write_object_begin(w);

	if (level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		spdk_json_write_named_array_begin(w, "channels");
	}

	spdk_for_each_channel(&g_accel_mlx5, accel_mlx5_dump_channel_stat, stat_ctx,
			      accel_mlx5_dump_channel_stat_done);

	return 0;
}

static bool
accel_mlx5_crypto_supports_cipher(enum spdk_accel_cipher cipher, size_t key_size)
{
	switch (cipher) {
	case SPDK_ACCEL_CIPHER_AES_XTS:
		return key_size == SPDK_ACCEL_AES_XTS_128_KEY_SIZE || key_size == SPDK_ACCEL_AES_XTS_256_KEY_SIZE;
	default:
		return false;
	}
}

static struct accel_mlx5_module g_accel_mlx5 = {
	.module = {
		.module_init		= accel_mlx5_init,
		.module_fini		= accel_mlx5_deinit,
		.write_config_json	= accel_mlx5_write_config_json,
		.get_ctx_size		= accel_mlx5_get_ctx_size,
		.name			= "mlx5",
		.supports_opcode	= accel_mlx5_supports_opcode,
		.get_io_channel		= accel_mlx5_get_io_channel,
		.submit_tasks		= accel_mlx5_submit_tasks,
		.crypto_key_init	= accel_mlx5_crypto_key_init,
		.crypto_key_deinit	= accel_mlx5_crypto_key_deinit,
		.crypto_supports_cipher	= accel_mlx5_crypto_supports_cipher,
		.crypto_supports_tweak_mode	= accel_mlx5_crypto_supports_tweak_mode,
		.get_memory_domains	= accel_mlx5_get_memory_domains,
	},
	.enabled = true,
	.qp_size = ACCEL_MLX5_QP_SIZE,
	.cq_size = ACCEL_MLX5_CQ_SIZE,
	.num_requests = ACCEL_MLX5_NUM_MKEYS,
	.split_mb_blocks = 0
};

static struct spdk_accel_driver g_accel_mlx5_driver = {
	.name			= SPDK_MLX5_DRIVER_NAME,
	.execute_sequence	= accel_mlx5_execute_sequence,
	.get_io_channel		= accel_mlx5_get_io_channel
};

SPDK_ACCEL_MODULE_REGISTER(mlx5, &g_accel_mlx5.module)
SPDK_LOG_REGISTER_COMPONENT(accel_mlx5)
