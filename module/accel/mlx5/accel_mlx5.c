/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/accel_module.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/sgl.h"
#include "accel_mlx5.h"

#include <infiniband/mlx5dv.h>
#include <rdma/rdma_cma.h>

#define ACCEL_MLX5_QP_SIZE (256u)
#define ACCEL_MLX5_NUM_MKEYS (2048u)

#define ACCEL_MLX5_MAX_SGE (16u)
#define ACCEL_MLX5_MAX_WC (64u)
#define ACCEL_MLX5_TASK_CACHE_LINES (SPDK_CEIL_DIV(sizeof(struct accel_mlx5_task), 64))
#define ACCEL_MLX5_MAX_MKEYS_IN_TASK (32)

/* TODO: after review with Achiad:
 * 1. try to reduce number of pointer redirections like task->dev->dev_ctx
 * 2. CQ_UPDATE for last WQE in a batch. Needs more rework:
 * 	1. Mark all RDMA ops as non-signaled
 * 	2. Before ringing the DB update last WQE with CQ_UPDATE flag
 * 	3. Track completions of all tasks submitted in a batch */

struct accel_mlx5_io_channel;
struct accel_mlx5_task;

struct accel_mlx5_cryptodev_memory_domain {
	struct spdk_memory_domain_rdma_ctx rdma_ctx;
	struct spdk_memory_domain *domain;
};

/* TODO: rename structures to remove 'crypto' word */
struct accel_mlx5_crypto_dev_ctx {
	struct spdk_mempool *mkey_pool;
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct accel_mlx5_cryptodev_memory_domain domain;
	bool crypto_multi_block;
	TAILQ_ENTRY(accel_mlx5_crypto_dev_ctx) link;
};

struct accel_mlx5_module {
	struct spdk_accel_module_if module;
	struct accel_mlx5_crypto_dev_ctx *crypto_ctxs;
	uint32_t num_crypto_ctxs;
	struct accel_mlx5_attr attr;
	char **allowed_crypto_devs;
	size_t allowed_crypto_devs_count;
	bool enabled;
	bool crypto_supported;
};

enum accel_mlx5_wrid_type {
	ACCEL_MLX5_WRID_MKEY,
	ACCEL_MLX5_WRID_WRITE,
};

struct accel_mlx5_wrid {
	uint8_t wrid;
};

struct accel_mlx5_klm
{
	struct mlx5_wqe_data_seg src_klm[ACCEL_MLX5_MAX_SGE];
	struct mlx5_wqe_data_seg dst_klm[ACCEL_MLX5_MAX_SGE];
	uint32_t src_klm_count;
	uint32_t dst_klm_count;
};

struct accel_mlx5_key_wrapper {
	struct spdk_mlx5_indirect_mkey *mkey;
};

struct accel_mlx5_task {
	struct spdk_accel_task base;
	struct accel_mlx5_dev *dev;
	uint16_t num_reqs;
	uint16_t num_completed_reqs;
	uint16_t num_submitted_reqs;
	/* If set, memory data will be encrypted during TX and wire data will be
	 decrypted during RX.
	 If not set, memory data will be decrypted during TX and wire data will
	 be encrypted during RX. */
	uint8_t enc_order;
	struct accel_mlx5_wrid write_wrid;
	bool inplace;
	bool crypto_op;
	/* Number of data blocks per crypto operation */
	uint16_t blocks_per_req;
	/* total num_blocks in this task */
	uint16_t num_blocks;
	/* for crypto op - number of allocated mkeys
	 * for crypto and copy - number of operations allowed to be submitted to qp */
	uint16_t num_ops;
	struct spdk_iov_sgl src;
	struct spdk_iov_sgl dst;
	TAILQ_ENTRY(accel_mlx5_task) link;
	/* Keep this array last since not all elements might be accessed, this reduces amount of data to be
	 * cached */
	struct accel_mlx5_key_wrapper *mkeys[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
};

struct accel_mlx5_dev_stats {
	uint64_t tasks;
	uint64_t umrs;
	uint64_t rdma_writes;
	uint64_t polls;
	uint64_t idle_polls;
	uint64_t completions;
};

struct accel_mlx5_dev {
	struct spdk_mlx5_dma_qp *dma_qp;
	struct spdk_rdma_utils_mem_map *mmap;
	struct accel_mlx5_crypto_dev_ctx *dev_ctx;
	struct accel_mlx5_io_channel *ch;

	uint32_t reqs_submitted;
	uint32_t max_reqs;
	struct accel_mlx5_dev_stats stats;
	/* Pending tasks waiting for requests resources */
	TAILQ_HEAD(, accel_mlx5_task) nomem;
	/* tasks submitted to HW. We can't complete a task even in error case until we reap completions for all
	 * submitted requests */
	TAILQ_HEAD(, accel_mlx5_task) in_hw;
	/* tasks between wr_start and wr_complete */
	TAILQ_HEAD(, accel_mlx5_task) before_submit;
	TAILQ_ENTRY(accel_mlx5_dev) link;
};

struct accel_mlx5_io_channel {
	struct accel_mlx5_dev *devs;
	struct spdk_poller *poller;
	uint32_t num_devs;
	/* Index in \b devs to be used for crypto in round-robin way */
	uint32_t dev_idx;
	struct accel_mlx5_klm klms[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
};

struct accel_mlx5_mkey_init_ctx {
	struct ibv_pd *pd;
	int rc;
};

static struct accel_mlx5_module g_accel_mlx5;

static inline void
accel_mlx5_task_complete(struct accel_mlx5_task *task, int rc)
{
	assert(task->num_reqs == task->num_completed_reqs || rc);
	SPDK_DEBUGLOG(accel_mlx5, "Complete task %p, opc %d, rc %d\n", task, task->base.op_code, rc);

	if (task->num_ops && task->crypto_op) {
		spdk_mempool_put_bulk(task->dev->dev_ctx->mkey_pool, (void **) task->mkeys,
				      task->num_ops);
	}
	spdk_accel_task_complete(&task->base, rc);
}

static int
accel_mlx5_translate_addr(void *addr, size_t size, struct spdk_memory_domain *domain, void *domain_ctx,
			 struct accel_mlx5_dev *dev, struct mlx5_wqe_data_seg *klm)
{
	struct spdk_rdma_utils_memory_translation map_translation;
	struct spdk_memory_domain_translation_result domain_translation;
	struct spdk_memory_domain_translation_ctx local_ctx;
	int rc;

	if (domain) {
		domain_translation.size = sizeof(struct spdk_memory_domain_translation_result);
		local_ctx.size = sizeof(local_ctx);
		local_ctx.rdma.ibv_qp = dev->dma_qp->qp.verbs_qp;
		rc = spdk_memory_domain_translate_data(domain, domain_ctx, dev->dev_ctx->domain.domain,
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
		rc = spdk_rdma_utils_get_translation(dev->mmap, addr, size,
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

static int
accel_mlx5_fill_block_sge(struct accel_mlx5_dev *dev, struct mlx5_wqe_data_seg *klm,
			  struct spdk_iov_sgl *iovs, struct spdk_memory_domain *domain, void *domain_ctx,
			  uint32_t lkey, uint32_t block_len, uint32_t *_remaining)
{
	void *addr;
	uint32_t remaining;
	uint32_t size;
	int i = 0;
	int rc;
	remaining = block_len;

	while (remaining && i < (int)ACCEL_MLX5_MAX_SGE) {
		size = spdk_min(remaining, iovs->iov->iov_len - iovs->iov_offset);
		addr = (void *)iovs->iov->iov_base + iovs->iov_offset;
		if (!lkey) {
			/* No pre-translated lkey */
			rc = accel_mlx5_translate_addr(addr, size, domain, domain_ctx, dev, &klm[i]);
			if (spdk_unlikely(rc)) {
				return rc;
			}
		} else {
			klm[i].lkey = lkey;
			klm[i].addr = (uint64_t) addr;
			klm[i].byte_count = size;
		}

		SPDK_DEBUGLOG(accel_mlx5, "\t klm[%d] lkey %u, addr %p, len %u\n", i, klm[i].lkey, (void*)klm[i].addr, klm[i].byte_count);
		spdk_iov_sgl_advance(iovs, size);
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

static inline int
accel_mlx5_task_alloc_mkeys(struct accel_mlx5_task *task)
{
	/* Each request consists of UMR and RDMA, or 2 operations.
	 * qp slot is the total number of operations available in qp */
	uint32_t num_ops = (task->num_reqs - task->num_completed_reqs) * 2;
	uint32_t qp_slot = task->dev->max_reqs - task->dev->reqs_submitted;
	uint32_t num_mkeys;
	int rc;

	assert(task->num_reqs >= task->num_completed_reqs);
	assert(task->crypto_op);
	num_ops = spdk_min(num_ops, qp_slot);
	num_ops = spdk_min(num_ops, ACCEL_MLX5_MAX_MKEYS_IN_TASK * 2);
	if (num_ops < 2) {
		/* We must do at least 1 UMR and 1 RDMA operation */
		task->num_ops = 0;
		return -ENOMEM;
	}
	num_mkeys = num_ops / 2;
	rc = spdk_mempool_get_bulk(task->dev->dev_ctx->mkey_pool, (void **)task->mkeys, num_mkeys);
	if (spdk_unlikely(rc)) {
		task->num_ops = 0;
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
accel_mlx5_copy_task_process_one(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_dev *dev, uint64_t wrid, uint32_t fence)
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
	rc = accel_mlx5_fill_block_sge(dev, klm.src_klm, &mlx5_task->src, task->src_domain,
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

	rc = accel_mlx5_fill_block_sge(dev, klm.dst_klm, &mlx5_task->dst, task->dst_domain,
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

	rc = spdk_mlx5_dma_qp_rdma_write(dev->dma_qp, klm.src_klm, klm.src_klm_count,
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

	struct accel_mlx5_dev *dev = mlx5_task->dev;
	uint16_t i;
	int rc;

	dev->stats.tasks++;
	assert(mlx5_task->num_reqs > 0);
	assert(mlx5_task->num_ops > 0);

	/* Handle n-1 reqs in order to simplify wrid and fence handling */
	for (i = 0; i < mlx5_task->num_ops - 1; i++) {
		rc = accel_mlx5_copy_task_process_one(mlx5_task, dev, 0, 0);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		dev->stats.rdma_writes++;
		dev->reqs_submitted++;
		mlx5_task->num_submitted_reqs++;
	}

	rc = accel_mlx5_copy_task_process_one(mlx5_task, dev, (uint64_t)&mlx5_task->write_wrid, MLX5_WQE_CTRL_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	dev->stats.rdma_writes++;
	dev->reqs_submitted++;
	mlx5_task->num_submitted_reqs++;

	SPDK_DEBUGLOG(accel_mlx5, "end, copy task, %p\n", mlx5_task);

	return 0;
}

static inline int
accel_mlx5_configure_crypto_umr(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_dev *dev, struct accel_mlx5_klm *klm,
				struct spdk_mlx5_indirect_mkey *dv_mkey, uint32_t src_lkey, uint32_t dst_lkey, uint64_t iv,
				uint64_t wrid, uint32_t fence, uint32_t req_len)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_mlx5_umr_attr umr_attr;
	uint32_t remaining;
	int rc;

	rc = accel_mlx5_fill_block_sge(dev, klm->src_klm, &mlx5_task->src, task->src_domain,
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
	klm->src_klm_count = rc;

	SPDK_DEBUGLOG(accel_mlx5, "task %p crypto_attr: bs %u, iv %"PRIu64", enc_on_tx %d\n",
		      mlx5_task, task->block_size, iv, mlx5_task->enc_order);
	rc = spdk_mlx5_crypto_get_dek_obj_id(task->crypto_key->priv, dev->dev_ctx->pd, &cattr.dek_obj_id);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to set crypto attr, rc %d\n", rc);
		return rc;
	}
	cattr.enc_order = mlx5_task->enc_order;
	cattr.bs_selector = bs_to_bs_selector(task->block_size);
	if (spdk_unlikely(!cattr.bs_selector)) {
		SPDK_ERRLOG("unsupported block size %u\n", task->block_size);
		return -EINVAL;
	}
	cattr.xts_iv = iv;
	cattr.keytag = 0;
	cattr.tweak_offset = task->crypto_key->param.tweak_offset;

	umr_attr.dv_mkey = dv_mkey;
	umr_attr.umr_len = req_len;
	umr_attr.klm_count = klm->src_klm_count;
	umr_attr.klm = klm->src_klm;

	if (!mlx5_task->inplace) {
		rc = accel_mlx5_fill_block_sge(dev, klm->dst_klm, &mlx5_task->dst, task->dst_domain,
					       task->dst_domain_ctx, dst_lkey, req_len, &remaining);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		if (spdk_unlikely(remaining)) {
			SPDK_ERRLOG("Incorrect dst iovs, handling not supported for crypto yet\n");
			abort();
		}
		klm->dst_klm_count = rc;
	}
	rc = spdk_mlx5_umr_configure_crypto(dev->dma_qp, &umr_attr, &cattr, wrid, fence);

	return rc;
}


static inline int
accel_mlx5_crypto_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_dev *dev = mlx5_task->dev;
	struct accel_mlx5_io_channel *ch = dev->ch;
	uint32_t src_lkey = 0, dst_lkey = 0;
	uint64_t iv;
	uint16_t i;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs, mlx5_task->num_ops);
	uint32_t req_len;
	/* First RDMA after UMR must have a SMALL_FENCE */
	uint32_t first_rdma_fence = MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	uint32_t blocks_processed;
	size_t ops_len = mlx5_task->blocks_per_req * num_ops;
	int rc;

	/* This degrades performance
	spdk_mlx5_dma_qp_prefetch_sq(dev->dma_qp, mlx5_task->num_ops * 5);
	 */
	if (spdk_unlikely(!num_ops)) {
		abort();
	}

	dev->stats.tasks++;

	if (ops_len <= mlx5_task->src.iov->iov_len - mlx5_task->src.iov_offset || task->s.iovcnt == 1) {
		rc = accel_mlx5_translate_addr(task->s.iovs[0].iov_base, task->s.iovs[0].iov_len, task->src_domain,
					       task->src_domain_ctx, dev, ch->klms[0].src_klm);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		src_lkey = ch->klms[0].src_klm->lkey;
	}
	if (!mlx5_task->inplace && (ops_len <= mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset || task->d.iovcnt == 1)) {
		rc = accel_mlx5_translate_addr(task->d.iovs[0].iov_base, task->d.iovs[0].iov_len, task->dst_domain,
					       task->dst_domain_ctx, dev, ch->klms[0].dst_klm);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		dst_lkey = ch->klms[0].dst_klm->lkey;
	}
	blocks_processed = mlx5_task->num_submitted_reqs * mlx5_task->blocks_per_req;
	iv = task->iv + blocks_processed;

	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);
	/* At this moment we have as many requests as can be submitted to a qp */
	for (i = 0; i < num_ops; i++) {
		if (mlx5_task->num_submitted_reqs + i + 1 == mlx5_task->num_reqs) {
			/* Last request may consume less than calculated */
			assert(mlx5_task->num_blocks > blocks_processed);
			req_len = (mlx5_task->num_blocks - blocks_processed) * task->block_size;
		} else {
			req_len = mlx5_task->blocks_per_req * task->block_size;
		}

		rc = accel_mlx5_configure_crypto_umr(mlx5_task, dev, &ch->klms[i], mlx5_task->mkeys[i]->mkey,
						     src_lkey, dst_lkey, iv, 0, 0, req_len);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		blocks_processed += mlx5_task->blocks_per_req;
		iv += mlx5_task->blocks_per_req;
		dev->stats.umrs++;
		assert(mlx5_task->num_submitted_reqs <= mlx5_task->num_reqs);
		dev->reqs_submitted++;
	}

	for (i = 0; i < num_ops - 1; i++) {
		/* UMR is used as a destination for RDMA_READ - from UMR to klms
		 * XTS is applied on DPS */
		if (mlx5_task->inplace) {
			rc = spdk_mlx5_dma_qp_rdma_read(dev->dma_qp, ch->klms[i].src_klm,
							ch->klms[i].src_klm_count,
							0, mlx5_task->mkeys[i]->mkey->mkey, 0,
							first_rdma_fence);
		} else {
			rc = spdk_mlx5_dma_qp_rdma_read(dev->dma_qp, ch->klms[i].dst_klm,
							ch->klms[i].dst_klm_count,
							0, mlx5_task->mkeys[i]->mkey->mkey, 0,
							first_rdma_fence);
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
			return rc;
		}
		first_rdma_fence = 0;
		dev->stats.rdma_writes++;
		mlx5_task->num_submitted_reqs++;
		assert(mlx5_task->num_submitted_reqs <= mlx5_task->num_reqs);
		dev->reqs_submitted++;
	}

	if (mlx5_task->inplace) {
		rc = spdk_mlx5_dma_qp_rdma_read(dev->dma_qp, ch->klms[i].src_klm, ch->klms[i].src_klm_count,
						0, mlx5_task->mkeys[i]->mkey->mkey,
						(uint64_t) &mlx5_task->write_wrid,
						first_rdma_fence | MLX5_WQE_CTRL_CQ_UPDATE);
	} else {
		rc = spdk_mlx5_dma_qp_rdma_read(dev->dma_qp, ch->klms[i].dst_klm, ch->klms[i].dst_klm_count,
						0, mlx5_task->mkeys[i]->mkey->mkey,
						(uint64_t) &mlx5_task->write_wrid,
						first_rdma_fence | MLX5_WQE_CTRL_CQ_UPDATE);
	}

	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA WRITE failed with %d\n", rc);
		return rc;
	}
	dev->stats.rdma_writes++;
	mlx5_task->num_submitted_reqs++;
	assert(mlx5_task->num_submitted_reqs <= mlx5_task->num_reqs);
	dev->reqs_submitted++;

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_task_continue(struct accel_mlx5_task *task)
{
	int rc;

	if (task->crypto_op) {
		if (task->num_ops == 0) {
			rc = accel_mlx5_task_alloc_mkeys(task);
			if (spdk_unlikely(rc != 0)) {
				/* Pool is empty, queue this task */
				TAILQ_INSERT_TAIL(&task->dev->nomem, task, link);
				return -ENOMEM;
			}
		}
		return accel_mlx5_crypto_task_process(task);
	} else {
		uint16_t qp_slot = task->dev->max_reqs - task->dev->reqs_submitted;
		task->num_ops = spdk_min(qp_slot, task->num_reqs - task->num_completed_reqs);
		if (task->num_ops == 0) {
			/* Pool is empty, queue this task */
			TAILQ_INSERT_TAIL(&task->dev->nomem, task, link);
			return -ENOMEM;
		}
		return accel_mlx5_copy_task_process(task);
	}
}

static inline uint32_t
accel_mlx5_get_copy_task_count(struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov, uint32_t dst_iovcnt)
{
	uint64_t src_len = 0;
	uint32_t src_counter = 0;
	uint32_t i, j;
	uint32_t num_ops = 0;
	uint32_t split_by_src_iov_counter = 0;

	for (i = 0; i < dst_iovcnt; i++) {
		for (;src_counter < src_iovcnt; src_counter++) {
			split_by_src_iov_counter++;
			if (split_by_src_iov_counter > ACCEL_MLX5_MAX_SGE) {
				num_ops++;
				split_by_src_iov_counter = 0;
			}

			src_len += src_iov[src_counter].iov_len;
			if (src_len >= dst_iov[i].iov_len) {
				/* We accumulated src iovs bigger than dst iovs */
				if (src_len > dst_iov[i].iov_len) {
					/* src iov might be bigger than several dst iovs, find how many dst iovs
					 * we should rewind starting from the current dst iov counter */
					src_len -= dst_iov[i].iov_len;
					/* check how many dst iovs in 1 src iov */
					for (j = i + 1; j < dst_iovcnt; j++) {
						if (dst_iov[j].iov_len > src_len) {
							break;
						}
						src_len -= dst_iov[j].iov_len;
						/* for each rewound dst iov element, increase the number of ops */
						num_ops++;
						i++;
					}
					/* Since src_len is bigger than last dst iov, remaining part of src iov will
					 * become first sge element in next op */
					split_by_src_iov_counter = 1;
				} else {
					src_len = 0;
				    	split_by_src_iov_counter = 0;
				}
				src_counter++;
				break;
			}
		}
		num_ops++;
	}

	return num_ops;
}

static inline int
accel_mlx5_task_init(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_dev *dev)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	size_t src_nbytes = 0;
	uint32_t num_blocks;
	uint32_t i;

	for (i = 0; i < task->s.iovcnt; i++) {
		src_nbytes += task->s.iovs[i].iov_len;
	}

	if (spdk_unlikely(mlx5_task->crypto_op && (src_nbytes % mlx5_task->base.block_size != 0))) {
		return -EINVAL;
	}

	mlx5_task->dev = dev;
	mlx5_task->num_completed_reqs = 0;
	mlx5_task->num_submitted_reqs = 0;
	mlx5_task->write_wrid.wrid = ACCEL_MLX5_WRID_WRITE;
	if (mlx5_task->crypto_op) {
		spdk_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt, 0);
		num_blocks = src_nbytes / mlx5_task->base.block_size;
		mlx5_task->num_blocks = num_blocks;
		if (task->d.iovcnt == 0 || (task->d.iovcnt == task->s.iovcnt &&
					    accel_mlx5_compare_iovs(task->d.iovs, task->s.iovs, task->s.iovcnt))) {
			mlx5_task->inplace = true;
		} else {
			mlx5_task->inplace = false;
			spdk_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt, 0);
		}
		if (mlx5_task->dev->dev_ctx->crypto_multi_block) {
			if (g_accel_mlx5.attr.split_mb_blocks) {
				mlx5_task->num_reqs = SPDK_CEIL_DIV(num_blocks, g_accel_mlx5.attr.split_mb_blocks);
				/* Last req may consume less blocks */
				mlx5_task->blocks_per_req = spdk_min(num_blocks, g_accel_mlx5.attr.split_mb_blocks);
			} else {
				mlx5_task->num_reqs = 1;
				mlx5_task->blocks_per_req = num_blocks;
			}
		} else {
			mlx5_task->num_reqs = num_blocks;
			mlx5_task->blocks_per_req = 1;
		}

		if (spdk_unlikely(accel_mlx5_task_alloc_mkeys(mlx5_task))) {
			/* Pool is empty, queue this task */
			SPDK_DEBUGLOG(accel_mlx5, "no reqs in pool, dev %s\n",
				      mlx5_task->dev->dev_ctx->context->device->name);
			return -ENOMEM;
		}
		SPDK_DEBUGLOG(accel_mlx5, "crypto task num_reqs %u, num_ops %u, num_blocks %u\n",
			      mlx5_task->num_reqs, mlx5_task->num_ops, mlx5_task->num_blocks);
	} else {
		uint32_t qp_slot = dev->max_reqs - dev->reqs_submitted;

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
		mlx5_task->inplace = false;
		spdk_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt, 0);
		spdk_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt, 0);
		mlx5_task->num_ops = spdk_min(qp_slot, mlx5_task->num_reqs);
		if (!mlx5_task->num_ops) {
			return -ENOMEM;
		}
		SPDK_DEBUGLOG(accel_mlx5, "copy task num_reqs %u, num_ops %u\n", mlx5_task->num_reqs, mlx5_task->num_ops);
	}

	SPDK_DEBUGLOG(accel_mlx5, "task %p, inplace %d, num_reqs %d\n", mlx5_task, mlx5_task->inplace,
		      mlx5_task->num_reqs);

	return 0;
}

static int
accel_mlx5_submit_tasks(struct spdk_io_channel *_ch, struct spdk_accel_task *task)
{
	struct accel_mlx5_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct accel_mlx5_task *mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	struct accel_mlx5_dev *dev;
	bool crypto_key_ok;
	int rc;

	switch (task->op_code) {
	case ACCEL_OPC_COPY:
		mlx5_task->crypto_op = false;
		break;
	case ACCEL_OPC_ENCRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->enc_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
		mlx5_task->crypto_op = true;
		crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
						    task->crypto_key->priv);
		break;
	case ACCEL_OPC_DECRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->enc_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
		mlx5_task->crypto_op = true;
		crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
						    task->crypto_key->priv);
		break;
	default:
		SPDK_ERRLOG("Unsupported accel opcode %d\n", task->op_code);
		return -ENOTSUP;
	}

	if (spdk_unlikely(!g_accel_mlx5.enabled || (mlx5_task->crypto_op && !crypto_key_ok))) {
		return -EINVAL;
	}

	dev = &ch->devs[ch->dev_idx];
	ch->dev_idx++;
	if (ch->dev_idx == ch->num_devs) {
		ch->dev_idx = 0;
	}

	rc = accel_mlx5_task_init(mlx5_task, dev);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(accel_mlx5, "no reqs to handle new task %p (requred %u), put to queue\n", mlx5_task,
				      mlx5_task->num_reqs);
			TAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
			return 0;
		}
		return rc;
	}

	if (mlx5_task->crypto_op) {
		return accel_mlx5_crypto_task_process(mlx5_task);
	} else {
		return accel_mlx5_copy_task_process(mlx5_task);
	}
}

static inline int64_t
accel_mlx5_poll_cq(struct accel_mlx5_dev *dev)
{
	struct spdk_mlx5_cq_completion wc[ACCEL_MLX5_MAX_WC];
	struct accel_mlx5_task *task;
	struct accel_mlx5_wrid *wr;
	uint32_t completed;
	int reaped, i, rc;

	dev->stats.polls++;
	reaped = spdk_mlx5_dma_qp_poll_completions(dev->dma_qp, wc, ACCEL_MLX5_MAX_WC);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return reaped;
	} else if (reaped == 0) {
		dev->stats.idle_polls++;
		return 0;
	}

	dev->stats.completions += reaped;
	SPDK_DEBUGLOG(accel_mlx5, "Reaped %d cpls on dev %s\n", reaped,
		      dev->dev_ctx->context->device->name);

	for (i = 0; i < reaped; i++) {
		wr = (struct accel_mlx5_wrid *)wc[i].wr_id;

		if (spdk_unlikely(!wr)) {
			/* That is unsignaled completion with error, just ignore it */
			continue;
		}

		switch (wr->wrid) {
		case ACCEL_MLX5_WRID_WRITE:
			task = SPDK_CONTAINEROF(wr, struct accel_mlx5_task, write_wrid);
			if (spdk_unlikely(wc[i].status)) {
				SPDK_ERRLOG("RDMA: qp %p, task %p WC status %d\n", dev->dma_qp, task, wc[i].status);
				abort();
				accel_mlx5_task_complete(task, -EIO);
				continue;
			}
			assert(task->num_submitted_reqs > task->num_completed_reqs);
			completed = task->num_submitted_reqs - task->num_completed_reqs;
			task->num_completed_reqs += completed;
			/* Crypto op consumes 2 ops, copy 1 op. To avoid branches and additional mlx5_task fields,
			 * re-use bool var to correctly reduce num of submitted requests */
			assert(dev->reqs_submitted >= completed * (((uint8_t) task->crypto_op) + 1));
			dev->reqs_submitted -= completed * (((uint8_t) task->crypto_op) + 1);

			SPDK_DEBUGLOG(accel_mlx5, "task %p, remaining %u\n", task,
				      task->num_reqs - task->num_completed_reqs);
			if (task->num_completed_reqs == task->num_reqs) {
				accel_mlx5_task_complete(task, 0);
			} else if (task->num_completed_reqs == task->num_submitted_reqs) {
				assert(task->num_submitted_reqs < task->num_reqs);
				rc = accel_mlx5_task_continue(task);
				if (spdk_unlikely(rc)) {
					if (rc != -ENOMEM) {
						accel_mlx5_task_complete(task, rc);
					}
				}
			}
			break;
		}
	}

	return reaped;
}

static inline void
accel_mlx5_resubmit_nomem_tasks(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_task *task, *tmp;
	int rc;

	TAILQ_FOREACH_SAFE(task, &dev->nomem, link, tmp) {
		TAILQ_REMOVE(&dev->nomem, task, link);
		rc = accel_mlx5_task_continue(task);
		if (rc) {
			if (rc == -ENOMEM) {
				break;
			} else {
				accel_mlx5_task_complete(task, rc);
			}
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
		if (dev->reqs_submitted) {
			rc = accel_mlx5_poll_cq(dev);
			if (spdk_unlikely(rc < 0)) {
				SPDK_ERRLOG("Error %"PRId64" on CQ, dev %s\n", rc, dev->dev_ctx->context->device->name);
			}
			completions += rc;
		}
		if (!TAILQ_EMPTY(&dev->nomem)) {
			accel_mlx5_resubmit_nomem_tasks(dev);
		}
	}

	return !!completions;
}

static bool
accel_mlx5_supports_opcode(enum accel_opcode opc)
{
	assert(g_accel_mlx5.enabled);

	switch (opc) {
	case ACCEL_OPC_COPY:
		return true;
	case ACCEL_OPC_ENCRYPT:
	case ACCEL_OPC_DECRYPT:
		return g_accel_mlx5.crypto_supported;
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
accel_mlx5_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_dev *dev;
	uint32_t i;

	spdk_poller_unregister(&ch->poller);
	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		if (dev->dma_qp) {
			spdk_mlx5_dma_qp_destroy(dev->dma_qp);
		}
		spdk_rdma_utils_free_mem_map(&dev->mmap);
		SPDK_NOTICELOG("Accel mlx5 device %p channel %p stats: tasks %lu, umrs %lu, "
			       "rdma_writes %lu, polls %lu, idle_polls %lu, completions %lu\n",
			       dev, ch, dev->stats.tasks, dev->stats.umrs,
			       dev->stats.rdma_writes, dev->stats.polls,
			       dev->stats.idle_polls, dev->stats.completions);
	}
	free(ch->devs);
}

static int
accel_mlx5_create_cb(void *io_device, void *ctx_buf)
{
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_crypto_dev_ctx *dev_ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;
	int rc;

	ch->devs = calloc(g_accel_mlx5.num_crypto_ctxs, sizeof(*ch->devs));
	if (!ch->devs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_accel_mlx5.num_crypto_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.crypto_ctxs[i];
		dev = &ch->devs[i];
		dev->ch = ch;
		dev->dev_ctx = dev_ctx;
		ch->num_devs++;

		struct spdk_mlx5_cq_attr mlx5_cq_attr = {};
		mlx5_cq_attr.cqe_cnt = g_accel_mlx5.attr.qp_size;
		mlx5_cq_attr.cqe_size = 64;
		mlx5_cq_attr.cq_context = dev;

		struct spdk_mlx5_qp_attr mlx5_qp_attr = {};
		mlx5_qp_attr.cap.max_send_wr = g_accel_mlx5.attr.qp_size;
		mlx5_qp_attr.cap.max_recv_wr = 0;
		mlx5_qp_attr.cap.max_send_sge = ACCEL_MLX5_MAX_SGE;
		mlx5_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_SGE;

		rc = spdk_mlx5_dma_qp_create(dev_ctx->pd, &mlx5_cq_attr, &mlx5_qp_attr, dev, &dev->dma_qp);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 dma QP, rc %d\n", rc);
			goto err_out;
		}

		TAILQ_INIT(&dev->nomem);
		TAILQ_INIT(&dev->in_hw);
		TAILQ_INIT(&dev->before_submit);
		dev->max_reqs = g_accel_mlx5.attr.qp_size;
		dev->mmap = spdk_rdma_utils_create_mem_map(dev_ctx->pd, NULL,
			    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
		if (!dev->mmap) {
			SPDK_ERRLOG("Failed to create memory map\n");
			goto err_out;
		}
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
	attr->num_requests = ACCEL_MLX5_NUM_MKEYS;
	attr->split_mb_blocks = 0;
}

static void
accel_mlx5_allowed_crypto_devs_free(void)
{
	size_t i;

	if (!g_accel_mlx5.allowed_crypto_devs || !g_accel_mlx5.allowed_crypto_devs_count) {
		return;
	}

	for (i = 0; i < g_accel_mlx5.allowed_crypto_devs_count; i++) {
		free(g_accel_mlx5.allowed_crypto_devs[i]);
	}
	free(g_accel_mlx5.allowed_crypto_devs);
}

static int
accel_mlx5_allowed_crypto_devs_parse(void)
{
	char *str, *tmp, *tok;
	size_t devs_count = 0;

	str = strdup(g_accel_mlx5.attr.allowed_crypto_devs);
	if (!str) {
		return -ENOMEM;
	}
	tmp = str;
	while ((tmp = strchr(tmp, ',')) != NULL) {
		tmp++;
		devs_count++;
	}
	devs_count++;

	g_accel_mlx5.allowed_crypto_devs = calloc(devs_count, sizeof(char *));
	if (!g_accel_mlx5.allowed_crypto_devs) {
		free(str);
		return -ENOMEM;
	}

	devs_count = 0;
	tok = strtok(str, ",");
	while (tok) {
		g_accel_mlx5.allowed_crypto_devs[devs_count] = strdup(tok);
		if (!g_accel_mlx5.allowed_crypto_devs[devs_count]) {
			free(str);
			accel_mlx5_allowed_crypto_devs_free();
			return -ENOMEM;
		}
		tok = strtok(NULL, ",");
		devs_count++;
		g_accel_mlx5.allowed_crypto_devs_count++;
	}

	free(str);

	return 0;
}

int
accel_mlx5_enable(struct accel_mlx5_attr *attr)
{
	if (attr) {
		g_accel_mlx5.attr = *attr;
		if (attr->allowed_crypto_devs) {
			int rc;

			g_accel_mlx5.attr.allowed_crypto_devs = strdup(attr->allowed_crypto_devs);
			if (!g_accel_mlx5.attr.allowed_crypto_devs) {
				return -ENOMEM;
			}
			rc = accel_mlx5_allowed_crypto_devs_parse();
			if (rc) {
				free(g_accel_mlx5.attr.allowed_crypto_devs);
				g_accel_mlx5.attr.allowed_crypto_devs = NULL;

				return rc;
			}
			rc = spdk_mlx5_crypto_devs_allow((const char * const *)g_accel_mlx5.allowed_crypto_devs,
							 g_accel_mlx5.allowed_crypto_devs_count);
			if (rc) {
				free(g_accel_mlx5.attr.allowed_crypto_devs);
				g_accel_mlx5.attr.allowed_crypto_devs = NULL;
				accel_mlx5_allowed_crypto_devs_free();

				return rc;
			}
		}
	}

	g_accel_mlx5.enabled = true;

	return 0;
}

static void
accel_mlx5_release_crypto_req(struct spdk_mempool *mp, void *cb_arg, void *_mkey, unsigned obj_idx)
{
	struct accel_mlx5_key_wrapper *wrapper = _mkey;

	if (wrapper->mkey) {
		spdk_mlx5_destroy_indirect_mkey(wrapper->mkey);
	}
}


static void
accel_mlx5_release_mkeys(struct accel_mlx5_crypto_dev_ctx *dev_ctx)
{
	size_t req_count;
	if (!dev_ctx->mkey_pool) {
		return;
	}

	req_count = spdk_mempool_count(dev_ctx->mkey_pool);
	if (req_count != g_accel_mlx5.attr.num_requests) {
		SPDK_ERRLOG("Expected %u reqs in the pool, but got only %zu\n", g_accel_mlx5.attr.num_requests, req_count);
	}
	spdk_mempool_obj_iter(dev_ctx->mkey_pool, accel_mlx5_release_crypto_req, NULL);
}

static void
accel_mlx5_free_resources(void)
{
	uint32_t i;

	for (i = 0; i < g_accel_mlx5.num_crypto_ctxs; i++) {
		accel_mlx5_release_mkeys(&g_accel_mlx5.crypto_ctxs[i]);
		spdk_memory_domain_destroy(g_accel_mlx5.crypto_ctxs[i].domain.domain);
		spdk_rdma_utils_put_pd(g_accel_mlx5.crypto_ctxs[i].pd);
	}

	free(g_accel_mlx5.crypto_ctxs);
	g_accel_mlx5.crypto_ctxs = NULL;
}

static void
accel_mlx5_deinit_cb(void *ctx)
{
	accel_mlx5_free_resources();
	spdk_accel_module_finish();
}

static void
accel_mlx5_deinit(void *ctx)
{
	if (g_accel_mlx5.attr.allowed_crypto_devs) {
		free(g_accel_mlx5.attr.allowed_crypto_devs);
		accel_mlx5_allowed_crypto_devs_free();
		spdk_mlx5_crypto_devs_allow(NULL, 0);
	}
	if (g_accel_mlx5.crypto_ctxs) {
		spdk_io_device_unregister(&g_accel_mlx5, accel_mlx5_deinit_cb);
	} else {
		spdk_accel_module_finish();
	}
}

static void
accel_mlx5_configure_crypto_mkey(struct spdk_mempool *mp, void *cb_arg, void *_mkey, unsigned obj_idx)
{
	struct accel_mlx5_key_wrapper *wrapper = _mkey;
	struct accel_mlx5_mkey_init_ctx *ctx = cb_arg;
	struct mlx5_devx_mkey_attr mkey_attr = {};
	struct spdk_mlx5_relaxed_ordering_caps caps = {};
	uint32_t bsf_size = 0;
	int rc;

	wrapper->mkey = NULL;
	if (ctx->rc) {
		return;
	}

	rc = spdk_mlx5_query_relaxed_ordering_caps(ctx->pd->context, &caps);
	if (rc) {
		SPDK_ERRLOG("Failed to get PCI relaxed ordering caps, rc %d\n", rc);
		ctx->rc = rc;
		return;
	}

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = caps.relaxed_ordering_write;
	mkey_attr.relaxed_ordering_read = caps.relaxed_ordering_read;
	mkey_attr.sg_count = 0;
	mkey_attr.sg = NULL;
	if (g_accel_mlx5.crypto_supported) {
		mkey_attr.crypto_en = true;
		bsf_size += 64;
	}
	mkey_attr.bsf_octowords = bsf_size / 16;

	wrapper->mkey = spdk_mlx5_create_indirect_mkey(ctx->pd, &mkey_attr);
	if (!wrapper->mkey) {
		SPDK_ERRLOG("Failed to create mkey on dev %s\n", ctx->pd->context->device->name);
		ctx->rc = -EINVAL;
		return;
	}
}

static int
accel_mlx5_crypto_ctx_mempool_create(struct accel_mlx5_crypto_dev_ctx *crypto_dev_ctx,
				     size_t num_entries)
{
	struct accel_mlx5_mkey_init_ctx init_ctx = {.pd = crypto_dev_ctx->pd };
	char pool_name[32];
	int rc;

	/* Compiler may produce a warning like
	 * warning: ‘%s’ directive output may be truncated writing up to 63 bytes into a region of size 21
	 * [-Wformat-truncation=]
	 * That is expected and that is due to ibv device name is 64 bytes while DPDK mempool API allows
	 * name to be max 32 bytes.
	 * To suppress this warning check the value returned by snprintf */
	rc = snprintf(pool_name, 32, "accel_mlx5_%s", crypto_dev_ctx->context->device->name);
	if (rc < 0) {
		assert(0);
		return -EINVAL;
	}
	uint32_t cache_size = num_entries / 4 * 3 / spdk_env_get_core_count();
	SPDK_NOTICELOG("Total pool size %zu, cache size %u\n", num_entries, cache_size);
	crypto_dev_ctx->mkey_pool = spdk_mempool_create_ctor(pool_name, num_entries,
							     sizeof(struct accel_mlx5_key_wrapper),
							     cache_size, SPDK_ENV_SOCKET_ID_ANY,
							     accel_mlx5_configure_crypto_mkey, &init_ctx);
	if (!crypto_dev_ctx->mkey_pool || init_ctx.rc) {
		SPDK_ERRLOG("Failed to create memory pool\n");
		return init_ctx.rc ? : -ENOMEM;
	}

	return 0;
}

static struct ibv_context *
accel_mlx5_rdma_get_mlx5_dev(struct ibv_context **devices, int num_devs)
{
	struct ibv_device_attr dev_attr = {};
	int rc, i;

	for (i = 0; i < num_devs; i++) {
		rc = ibv_query_device(devices[i], &dev_attr);
		if (rc) {
			continue;
		}
		if (dev_attr.vendor_id == SPDK_MLX5_VENDOR_ID_MELLANOX) {
			return devices[i];
		}
	}
	return NULL;
}

static int
accel_mlx5_init_mem_op(void)
{
	struct accel_mlx5_crypto_dev_ctx *crypto_dev_ctx;
	struct accel_mlx5_cryptodev_memory_domain *domain;
	struct ibv_context **rdma_devs, *dev;
	struct spdk_memory_domain_ctx ctx;
	struct ibv_pd *pd;
	int num_devs = 0, rc;

	rdma_devs = rdma_get_devices(&num_devs);
	if (!rdma_devs || !num_devs) {
		return -ENODEV;
	}

	dev = accel_mlx5_rdma_get_mlx5_dev(rdma_devs, num_devs);
	if (!dev) {
		SPDK_ERRLOG("No mlx devices found\n");
		rc = -ENODEV;
		goto cleanup;
	}

	g_accel_mlx5.crypto_ctxs = calloc(1, sizeof(*g_accel_mlx5.crypto_ctxs));
	if (!g_accel_mlx5.crypto_ctxs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	crypto_dev_ctx = &g_accel_mlx5.crypto_ctxs[0];

	pd = spdk_rdma_utils_get_pd(dev);
	if (!pd) {
		SPDK_ERRLOG("Failed to get PD for context %p, dev %s\n", dev, dev->device->name);
		rc = -EINVAL;
		goto cleanup;
	}
	crypto_dev_ctx->context = dev;
	crypto_dev_ctx->pd = pd;

	domain = &g_accel_mlx5.crypto_ctxs[0].domain;
	domain->rdma_ctx.size = sizeof(domain->rdma_ctx);
	domain->rdma_ctx.ibv_pd = (void *) pd;
	ctx.size = sizeof(ctx);
	ctx.user_ctx = &domain->rdma_ctx;

	rc = spdk_memory_domain_create(&domain->domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
				       SPDK_RDMA_DMA_DEVICE);
	if (rc) {
		goto cleanup;
	}

	g_accel_mlx5.num_crypto_ctxs = 1;

	SPDK_NOTICELOG("Accel framework mlx5 initialized\n");
	spdk_io_device_register(&g_accel_mlx5, accel_mlx5_create_cb, accel_mlx5_destroy_cb,
				sizeof(struct accel_mlx5_io_channel), "accel_mlx5");

	return 0;

cleanup:
	rdma_free_devices(rdma_devs);
	accel_mlx5_free_resources();

	return rc;
}

static int
accel_mlx5_init(void)
{
	struct accel_mlx5_crypto_dev_ctx *crypto_dev_ctx;
	struct accel_mlx5_cryptodev_memory_domain *domain;
	struct ibv_context **rdma_devs, *dev;
	struct spdk_memory_domain_ctx ctx;
	struct ibv_pd *pd;
	struct spdk_mlx5_crypto_caps crypto_caps;
	int num_devs = 0, rc = 0, i;

	if (!g_accel_mlx5.enabled) {
		return -EINVAL;
	}

	rdma_devs = spdk_mlx5_crypto_devs_get(&num_devs);
	if (!rdma_devs || !num_devs) {
		if (g_accel_mlx5.attr.allowed_crypto_devs) {
			SPDK_WARNLOG("No crypto devs found, only memory operations will be supported\n");
		} else {
			SPDK_NOTICELOG("No crypto devs found, only memory operations will be supported\n");
		}
		g_accel_mlx5.crypto_supported = false;
		return accel_mlx5_init_mem_op();
	} else {
		g_accel_mlx5.crypto_supported = true;
	}

	g_accel_mlx5.crypto_ctxs = calloc(num_devs, sizeof(*g_accel_mlx5.crypto_ctxs));
	if (!g_accel_mlx5.crypto_ctxs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	for (i = 0; i < num_devs; i++) {
		crypto_dev_ctx = &g_accel_mlx5.crypto_ctxs[i];
		dev = rdma_devs[i];
		memset(&crypto_caps, 0, sizeof(crypto_caps));
		rc = spdk_mlx5_query_crypto_caps(dev, &crypto_caps);
		if (rc) {
			SPDK_ERRLOG("Failed to get aes_xts caps, dev %s\n", dev->device->name);
			goto cleanup;
		}
		SPDK_NOTICELOG("Crypto dev %s, aes_xts: single block %d, mb_be %d, mb_le %d, inc_64 %d\n",
			       dev->device->name,
			       crypto_caps.single_block_le_tweak,
			       crypto_caps.multi_block_be_tweak,
			       crypto_caps.multi_block_le_tweak,
			       crypto_caps.tweak_inc_64);

		pd = spdk_rdma_utils_get_pd(dev);
		if (!pd) {
			SPDK_ERRLOG("Failed to get PD for context %p, dev %s\n", dev, dev->device->name);
			rc = -EINVAL;
			goto cleanup;
		}
		crypto_dev_ctx->context = dev;
		crypto_dev_ctx->pd = pd;
		rc = accel_mlx5_crypto_ctx_mempool_create(crypto_dev_ctx, g_accel_mlx5.attr.num_requests);
		if (rc) {
			goto cleanup;
		}

		domain = &g_accel_mlx5.crypto_ctxs[i].domain;
		domain->rdma_ctx.size = sizeof(domain->rdma_ctx);
		domain->rdma_ctx.ibv_pd = (void *) pd;
		ctx.size = sizeof(ctx);
		ctx.user_ctx = &domain->rdma_ctx;

		rc = spdk_memory_domain_create(&domain->domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
					       SPDK_RDMA_DMA_DEVICE);
		if (rc) {
			goto cleanup;
		}

		/* Explicitly disabled by default */
		crypto_dev_ctx->crypto_multi_block = false;
		if (crypto_caps.multi_block_be_tweak) {
			/* TODO: multi_block LE tweak will be checked later once LE BSF is fixed */
			crypto_dev_ctx->crypto_multi_block = true;
		} else if (g_accel_mlx5.attr.split_mb_blocks) {
			SPDK_WARNLOG("\"split_mb_block\" is set but dev %s doesn't support multi block crypto\n",
				     dev->device->name);
		}

		g_accel_mlx5.num_crypto_ctxs++;
	}

	SPDK_NOTICELOG("Accel framework mlx5 initialized, found %d devices.\n", num_devs);
	spdk_io_device_register(&g_accel_mlx5, accel_mlx5_create_cb, accel_mlx5_destroy_cb,
				sizeof(struct accel_mlx5_io_channel), "accel_mlx5");

	spdk_mlx5_crypto_devs_release(rdma_devs);

	return rc;

cleanup:
	spdk_mlx5_crypto_devs_release(rdma_devs);
	accel_mlx5_free_resources();

	return rc;
}

static void
accel_mlx5_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_accel_mlx5.enabled) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "mlx5_scan_accel_module");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_uint16(w, "qp_size", g_accel_mlx5.attr.qp_size);
		spdk_json_write_named_uint32(w, "num_requests", g_accel_mlx5.attr.num_requests);
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
		return (int)g_accel_mlx5.num_crypto_ctxs;
	}

	size = spdk_min(array_size, (int)g_accel_mlx5.num_crypto_ctxs);

	for (i = 0; i < size; i++) {
		domains[i] = g_accel_mlx5.crypto_ctxs[i].domain.domain;
	}

	return (int)g_accel_mlx5.num_crypto_ctxs;
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
		.get_memory_domains	= accel_mlx5_get_memory_domains,
	},
	.enabled = true,
	.attr = {
		.qp_size = ACCEL_MLX5_QP_SIZE,
		.num_requests = ACCEL_MLX5_NUM_MKEYS,
		.split_mb_blocks = 0
	}
};

SPDK_ACCEL_MODULE_REGISTER(mlx5, &g_accel_mlx5.module)
SPDK_LOG_REGISTER_COMPONENT(accel_mlx5)
