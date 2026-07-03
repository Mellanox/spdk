/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk/accel_module.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/sgl.h"
#include "accel_mlx5.h"
#include "spdk/net.h"

#include <infiniband/mlx5dv.h>
#include <rdma/rdma_cma.h>

#define ACCEL_MLX5_QP_SIZE (64u)
#define ACCEL_MLX5_CQ_SIZE (1024u)
#define ACCEL_MLX5_NUM_MKEYS (1024u)
#define ACCEL_MLX5_DEV_RR_THRESHOLD (4u)
#define ACCEL_MLX5_RECOVER_POLLER_PERIOD_US (10000)
#define ACCEL_MLX5_MAX_INLINE_SGE (16u)
#define ACCEL_MLX5_MAX_MB_SGE (256u)
#define ACCEL_MLX5_MAX_SGE ACCEL_MLX5_MAX_MB_SGE
#define ACCEL_MLX5_SGE_SIZE (16u)
#define ACCEL_MLX5_MAX_WC (32u)
#define ACCEL_MLX5_MAX_MKEYS_IN_TASK (16u)
#define ACCEL_MLX5_MAX_UMR_MB (1u)

/* Assume we have up to 16 devices */
#define ACCEL_MLX5_ALLOWED_DEVS_MAX_LEN ((SPDK_MLX5_DEV_MAX_NAME_LEN + 1) * 16)
#define ACCEL_MLX5_SYSFS_PATH_MAX 512

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, task)	\
do {							\
	assert((qp)->wrs_submitted < (qp)->wrs_max);	\
	(qp)->wrs_submitted++;				\
	assert((task)->num_wrs < UINT16_MAX);		\
	(task)->num_wrs++;				\
} while (0)

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, task)	\
do {									\
	assert((dev)->wrs_in_cq < (dev)->wrs_in_cq_max);		\
	(dev)->wrs_in_cq++;						\
        assert((qp)->wrs_submitted < (qp)->wrs_max);			\
	(qp)->wrs_submitted++;						\
	accel_mlx5_qp_complete_wr(qp);					\
	assert((task)->num_wrs < UINT16_MAX);				\
	(task)->num_wrs++;						\
} while (0)

struct accel_mlx5_io_channel;
struct accel_mlx5_task;

struct accel_mlx5_dev_ctx {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct spdk_memory_domain *domain;
	struct spdk_mlx5_psv_pool *psv_pool;
	struct spdk_mlx5_umr_mb_pool *umr_mb_pool;
	struct spdk_rdma_utils_mem_map *map;
	TAILQ_ENTRY(accel_mlx5_dev_ctx) link;
	bool mkeys;
	bool crypto_mkeys;
	bool sig_mkeys;
	struct spdk_mlx5_device_caps caps;
};

enum accel_mlx5_opcode {
	ACCEL_MLX5_OPC_COPY,
	ACCEL_MLX5_OPC_CRYPTO,
	ACCEL_MLX5_OPC_CRYPTO_MKEY,
	ACCEL_MLX5_OPC_CRYPTO_MKEY_EXT_QP,
	ACCEL_MLX5_OPC_MKEY,
	ACCEL_MLX5_OPC_CRC32C,
	ACCEL_MLX5_OPC_DIF_GENERATE_COPY,
	ACCEL_MLX5_OPC_DIF_VERIFY_COPY,
	ACCEL_MLX5_OPC_DIF_GENERATE_COPY_MKEY,
	ACCEL_MLX5_OPC_DIF_VERIFY_COPY_MKEY,
	ACCEL_MLX5_OPC_CRC32C_MKEY,
	ACCEL_MLX5_OPC_LAST
};

SPDK_STATIC_ASSERT(ACCEL_MLX5_OPC_LAST <= 0xf,
		   "accel opcode exceeds 4 bits, update accel_mlx5 struct");

struct accel_mlx5_stats {
	uint64_t crypto_umrs;
	uint64_t sig_umrs;
	uint64_t umrs;
	uint64_t rdma_reads;
	uint64_t rdma_writes;
	uint64_t polls;
	uint64_t idle_polls;
	uint64_t completions;
	uint64_t nomem_qdepth;
	uint64_t nomem_mkey;
	uint64_t opcodes[ACCEL_MLX5_OPC_LAST];
};

struct accel_mlx5_module {
	struct spdk_accel_module_if module;
	struct accel_mlx5_stats stats;
	struct spdk_spinlock lock;
	struct accel_mlx5_dev_ctx *dev_ctxs;
	uint32_t num_ctxs;
	struct accel_mlx5_attr attr;
	char **allowed_devs;
	size_t allowed_devs_count;
	bool initialized;
	bool enabled;
	bool crypto_supported;
	bool crc32c_supported;
};

struct accel_mlx5_sge {
	uint32_t src_sge_count;
	uint32_t dst_sge_count;
	struct ibv_sge src_sge[ACCEL_MLX5_MAX_INLINE_SGE];
	struct ibv_sge dst_sge[ACCEL_MLX5_MAX_INLINE_SGE];
};

struct accel_mlx5_iov_sgl {
	struct iovec	*iov;
	uint32_t	iovcnt;
	uint32_t	iov_offset;
};

struct accel_mlx5_task {
	struct spdk_accel_task base;
	struct accel_mlx5_iov_sgl src;
	struct accel_mlx5_iov_sgl dst;
	STAILQ_ENTRY(accel_mlx5_task) link;
	struct accel_mlx5_qp *qp;
	struct spdk_mlx5_psv_pool_obj *psv;
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
		struct {
			bool md_in_umr;
		};
	};
	union {
		struct {
			struct spdk_mlx5_qp *ext_qp;
		} crypto_external_qp;
	};
	union {
		uint8_t raw;
		struct {
			uint8_t inplace : 1;
			/* This task is handled by mlx5 driver */
			uint8_t driver_seq : 1;
			/* The task need memory domain data_transfer function to be completed */
			uint16_t needs_data_transfer : 1;
			/* If set, memory data will be encrypted during TX and wire data will be
			 decrypted during RX.
			 If not set, memory data will be decrypted during TX and wire data will
			 be encrypted during RX. */
			uint8_t enc_order : 2;
			uint8_t needs_epilogue : 1;
			uint8_t clear_psv_error : 1;
		};
	};
	uint8_t mlx5_opcode;

	uint16_t num_reqs;
	uint16_t num_completed_reqs;
	uint16_t num_submitted_reqs;
	uint16_t num_wrs;
	/* for crypto op - number of allocated mkeys
	 * for crypto and copy - number of operations allowed to be submitted to qp */
	uint16_t num_ops;
	uint16_t num_mbs;
	/* Keep this array last since not all elements might be accessed, this reduces amount of data to be
	 * cached */
	struct spdk_mlx5_mkey_pool_obj *mkeys[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct spdk_mlx5_umr_mb_pool_obj *umr_mb[ACCEL_MLX5_MAX_UMR_MB];
};

SPDK_STATIC_ASSERT(ACCEL_MLX5_MAX_MKEYS_IN_TASK <= UINT8_MAX, "uint8_t is used to iterate mkeys");

RB_HEAD(accel_mlx5_qpairs_map, accel_mlx5_qp);

struct accel_mlx5_qp {
	struct spdk_mlx5_qp *qp;
	struct ibv_qp *verbs_qp;
	struct accel_mlx5_dev *dev;
	/* tasks submitted to HW. We can't complete a task even in error case until we reap completions for all
	 * submitted requests */
	STAILQ_HEAD(, accel_mlx5_task) in_hw;
	RB_ENTRY(accel_mlx5_qp) node;
	/* Memory domain which this qpair serves */
	struct spdk_memory_domain *domain;
	uint16_t wrs_submitted;
	uint16_t wrs_max;
	bool ring_db;
	bool recovering;
	struct spdk_poller *recover_poller;
	STAILQ_ENTRY(accel_mlx5_qp) link;
};

struct accel_mlx5_dev {
	struct accel_mlx5_qp qp;
	struct spdk_mlx5_cq *cq;
	struct accel_mlx5_qpairs_map qpairs_map;
	struct spdk_mlx5_mkey_pool *mkeys;
	struct spdk_mlx5_mkey_pool *crypto_mkeys;
	struct spdk_mlx5_mkey_pool *sig_mkeys;
	struct spdk_rdma_utils_mem_map *mmap;
	struct accel_mlx5_dev_ctx *dev_ctx;
	struct spdk_io_channel *ch;
	uint16_t wrs_in_cq;
	uint16_t wrs_in_cq_max;
	uint16_t crypto_split_blocks;
	bool crypto_multi_block;
	STAILQ_HEAD(, accel_mlx5_qp) complete_wr_qps;
	/* Pending tasks waiting for requests resources */
	STAILQ_HEAD(, accel_mlx5_task) nomem;
	TAILQ_ENTRY(accel_mlx5_dev) link;
	struct accel_mlx5_stats stats;
};

struct accel_mlx5_io_channel {
	struct accel_mlx5_dev *devs;
	struct spdk_poller *poller;
	uint16_t num_devs;
	/* Index in \b devs to be used for operations in round-robin way */
	uint16_t dev_idx;
	uint16_t dev_rr_count;
	uint16_t dev_rr_threshold;
	bool poller_handler_registered;
};

struct accel_mlx5_task_operations {
	int (*init)(struct accel_mlx5_task *task);
	int (*process)(struct accel_mlx5_task *task);
	int (*cont)(struct accel_mlx5_task *task);
	void (*complete)(struct accel_mlx5_task *task);
};

struct accel_mlx5_dump_stats_ctx {
	struct accel_mlx5_stats total;
	struct spdk_json_write_ctx *w;
	enum accel_mlx5_dump_state_level level;
	accel_mlx5_dump_stat_done_cb cb;
	void *ctx;
};

static struct accel_mlx5_module g_accel_mlx5;
static struct spdk_accel_driver g_accel_mlx5_driver;

static int accel_mlx5_create_qp(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp);
static inline void accel_mlx5_qp_complete_wr(struct accel_mlx5_qp *qp);
static inline int accel_mlx5_execute_sequence(struct spdk_io_channel *ch,
		struct spdk_accel_sequence *seq);
static inline void accel_mlx5_task_complete(struct accel_mlx5_task *task);
static inline void accel_mlx5_memory_domain_transfer(struct accel_mlx5_task *task,
		struct spdk_mlx5_mkey_pool_obj *mkey);
static inline int accel_mlx5_task_alloc_sig_mkey_ctx(struct accel_mlx5_task *mlx5_task);

static int
accel_mlx5_qpair_compare(struct accel_mlx5_qp *qp1, struct accel_mlx5_qp *qp2)
{
	return (uint64_t)qp1->domain < (uint64_t)qp2->domain ? -1 : (uint64_t)qp1->domain >
	       (uint64_t)qp2->domain;
}

RB_GENERATE_STATIC(accel_mlx5_qpairs_map, accel_mlx5_qp, node, accel_mlx5_qpair_compare);

static inline void
accel_mlx5_iov_sgl_init(struct accel_mlx5_iov_sgl *s, struct iovec *iov, uint32_t iovcnt)
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
			      s->iov_offset, step);
	}

	SPDK_ERRLOG("Can't unwind iovs, remaining  %u\n", step);
	assert(0);
}

static inline int
accel_mlx5_sge_unwind(struct ibv_sge *sge, uint32_t sge_count, uint32_t step)
{
	int i;

	assert(sge_count > 0);
	SPDK_DEBUGLOG(accel_mlx5, "sge %p, count %u, step %u\n", sge, sge_count, step);
	for (i = (int)sge_count - 1; i >= 0; i--) {
		if (sge[i].length > step) {
			sge[i].length -= step;
			SPDK_DEBUGLOG(accel_mlx5, "\tsge[%u] len %u, step %u\n", i, sge[i].length, step);
			return (int)i + 1;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\tsge[%u] len %u, step %u\n", i, sge[i].length, step);
		step -= sge[i].length;
	}

	SPDK_ERRLOG("Can't unwind sge, remaining  %u\n", step);
	assert(step == 0);

	return 0;
}

static inline int
accel_mlx5_task_alloc_umr_mb(struct accel_mlx5_task *task, uint32_t count)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;

	assert(g_accel_mlx5.attr.umr_memory_buffer);
	assert(count <= SPDK_COUNTOF(task->umr_mb));

	rc = spdk_mlx5_umr_mb_pool_get_bulk(dev->dev_ctx->umr_mb_pool, task->umr_mb, count);
	if (rc != 0) {
		return rc;
	}

	task->num_mbs = count;
	return 0;
}

static inline void
accel_mlx5_task_free_umr_mb(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;

	if (task->num_mbs == 0) {
		return;
	}

	spdk_mlx5_umr_mb_pool_put_bulk(dev->dev_ctx->umr_mb_pool, task->umr_mb, task->num_mbs);
	task->num_mbs = 0;
}

static inline void
accel_mlx5_crypto_task_complete(struct accel_mlx5_task *task)
{
	struct accel_mlx5_dev *dev = task->qp->dev;

	assert(task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
	spdk_accel_task_complete(&task->base, 0);
}

static inline void
accel_mlx5_dev_nomem_task_qdepth(struct accel_mlx5_dev *dev, struct accel_mlx5_task *task)
{
	SPDK_DEBUGLOG(accel_mlx5, "queue task %p, dev %p\n", task, dev);
	dev->stats.nomem_qdepth++;
	STAILQ_INSERT_TAIL(&dev->nomem, task, link);
}

static inline void
accel_mlx5_dev_nomem_task_qdepth_repeat(struct accel_mlx5_dev *dev, struct accel_mlx5_task *task)
{
	SPDK_DEBUGLOG(accel_mlx5, "queue task %p, dev %p\n", task, dev);
	dev->stats.nomem_qdepth++;
	STAILQ_INSERT_HEAD(&dev->nomem, task, link);
}

static inline void
accel_mlx5_dev_nomem_task_mkey(struct accel_mlx5_dev *dev, struct accel_mlx5_task *task)
{
	SPDK_DEBUGLOG(accel_mlx5, "queue task %p, dev %p\n", task, dev);
	dev->stats.nomem_mkey++;
	STAILQ_INSERT_TAIL(&dev->nomem, task, link);
}

static inline void
accel_mlx5_dev_nomem_task_mkey_repeat(struct accel_mlx5_dev *dev, struct accel_mlx5_task *task)
{
	SPDK_DEBUGLOG(accel_mlx5, "queue task %p, dev %p\n", task, dev);
	dev->stats.nomem_mkey++;
	STAILQ_INSERT_HEAD(&dev->nomem, task, link);
}

static inline void
accel_mlx5_task_fail(struct accel_mlx5_task *task, int rc)
{
	struct accel_mlx5_dev *dev = task->qp->dev;
	struct spdk_accel_task *next;
	struct spdk_accel_sequence *seq;
	bool driver_seq;

	assert(rc);
	assert(task->num_submitted_reqs == task->num_completed_reqs);

	if (task->num_ops) {
		switch (task->mlx5_opcode) {
		case ACCEL_MLX5_OPC_CRYPTO:
			spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
			break;
		case ACCEL_MLX5_OPC_CRYPTO_MKEY:
		case ACCEL_MLX5_OPC_CRYPTO_MKEY_EXT_QP:
			spdk_mlx5_mkey_pool_put(dev->crypto_mkeys, task->mkeys[0]);
			break;
		case ACCEL_MLX5_OPC_CRC32C:
		case ACCEL_MLX5_OPC_DIF_GENERATE_COPY:
		case ACCEL_MLX5_OPC_DIF_VERIFY_COPY:
			spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, task->mkeys, task->num_ops);
			spdk_mlx5_psv_pool_put(&task->psv);
			break;
		case ACCEL_MLX5_OPC_CRC32C_MKEY:
			spdk_mlx5_mkey_pool_put(dev->sig_mkeys, task->mkeys[0]);
			spdk_mlx5_psv_pool_put(&task->psv);
			break;
		case ACCEL_MLX5_OPC_DIF_GENERATE_COPY_MKEY:
		case ACCEL_MLX5_OPC_DIF_VERIFY_COPY_MKEY:
			spdk_mlx5_mkey_pool_put(dev->sig_mkeys, task->mkeys[0]);
			spdk_mlx5_psv_pool_put(&task->psv);
			break;
		case ACCEL_MLX5_OPC_MKEY:
			spdk_mlx5_mkey_pool_put(dev->mkeys, task->mkeys[0]);
			accel_mlx5_task_free_umr_mb(task);
			break;
		default:
			break;
		}
	}
	next = spdk_accel_sequence_next_task(&task->base);
	seq = task->base.seq;
	driver_seq = task->driver_seq;

	SPDK_DEBUGLOG(accel_mlx5, "Fail task %p, opc %d, rc %d\n", task, task->mlx5_opcode, rc);
	spdk_accel_task_complete(&task->base, rc);

	if (driver_seq) {
		struct spdk_io_channel *ch = task->qp->dev->ch;

		assert(seq);
		if (next) {
			accel_mlx5_execute_sequence(ch, seq);
		} else {
			spdk_accel_sequence_continue(seq);
		}
	}
}

static int
accel_mlx5_translate_addr(void *addr, size_t size, struct spdk_memory_domain *domain,
			  void *domain_ctx, struct accel_mlx5_dev *dev, struct ibv_sge *sge)
{
	struct spdk_rdma_utils_memory_translation map_translation;
	struct spdk_memory_domain_translation_result domain_translation;
	struct spdk_memory_domain_translation_ctx local_ctx;
	int rc;

	if (domain) {
		domain_translation.size = sizeof(struct spdk_memory_domain_translation_result);
		local_ctx.size = sizeof(local_ctx);
		local_ctx.rdma.ibv_qp = dev->qp.verbs_qp;
		rc = spdk_memory_domain_translate_data(domain, domain_ctx, dev->dev_ctx->domain,
						       &local_ctx, addr, size, &domain_translation);
		if (spdk_unlikely(rc || domain_translation.iov_count != 1)) {
			SPDK_ERRLOG("Memory domain translation failed, addr %p, length %zu, iovcnt %u\n", addr, size,
				    domain_translation.iov_count);
			if (rc == 0) {
				rc = -EINVAL;
			}

			return rc;
		}
		sge->lkey = domain_translation.rdma.lkey;
		sge->addr = (uint64_t) domain_translation.iov.iov_base;
		sge->length = domain_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_utils_get_translation(dev->mmap, addr, size,
						     &map_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Memory translation failed, addr %p, length %zu\n", addr, size);
			return rc;
		}
		sge->lkey = map_translation.mr_or_key.mr->lkey;
		sge->addr = (uint64_t)addr;
		sge->length = size;
	}

	return 0;
}

static inline int
accel_mlx5_fill_block_sge(struct accel_mlx5_qp *qp, struct ibv_sge *sge, int sge_cnt,
			  struct accel_mlx5_iov_sgl *iovs, uint32_t lkey, uint32_t len, uint32_t *_remaining,
			  struct spdk_memory_domain *domain, void *domain_ctx)
{
	void *addr;
	uint32_t remaining = len;
	uint32_t size;
	int i = 0;
	int rc;

	*_remaining = len;

	while (remaining && i < sge_cnt) {
		size = spdk_min(remaining, iovs->iov->iov_len - iovs->iov_offset);
		addr = (void *)iovs->iov->iov_base + iovs->iov_offset;
		if (!lkey) {
			/* No pre-translated lkey */
			rc = accel_mlx5_translate_addr(addr, size, domain, domain_ctx, qp->dev, &sge[i]);
			if (spdk_unlikely(rc)) {
				return rc;
			}
		} else {
			sge[i].lkey = lkey;
			sge[i].addr = (uint64_t) addr;
			sge[i].length = size;
		}

		SPDK_DEBUGLOG(accel_mlx5, "\t sge[%d] lkey %u, addr %p, len %u\n", i, sge[i].lkey,
			      (void *)sge[i].addr, sge[i].length);
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
	return memcmp(v1, v2, sizeof(*v1) * iovcnt) == 0;
}

static inline uint16_t
accel_mlx5_dev_get_available_slots(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp)
{
	assert(qp->wrs_max >= qp->wrs_submitted);
	assert(dev->wrs_in_cq_max >= dev->wrs_in_cq);

	/* Each time we produce only 1 CQE, so we need 1 CQ slot */
	if (spdk_unlikely(dev->wrs_in_cq == dev->wrs_in_cq_max)) {
		return 0;
	}

	return qp->wrs_max - qp->wrs_submitted;
}

static inline int
accel_mlx5_task_alloc_mkeys(struct accel_mlx5_task *task, struct spdk_mlx5_mkey_pool *pool)
{
	uint32_t num_ops;
	int rc;

	assert(task->num_reqs > task->num_completed_reqs);
	num_ops = task->num_reqs - task->num_completed_reqs;
	num_ops = spdk_min(num_ops, ACCEL_MLX5_MAX_MKEYS_IN_TASK);
	if (spdk_unlikely(!num_ops)) {
		assert(0);
		return -EINVAL;
	}

	rc = spdk_mlx5_mkey_pool_get_bulk(pool, task->mkeys, num_ops);
	if (spdk_unlikely(rc)) {
		task->num_ops = 0;
		return -ENOMEM;
	}
	assert(num_ops <= UINT16_MAX);
	task->num_ops = num_ops;

	return 0;
}

static inline uint8_t
bs_to_bs_selector(uint32_t bs)
{
	switch (bs) {
	case 512:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_512;
	case 520:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_520;
	case 4096:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_4096;
	case 4160:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_4160;
	default:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_RESERVED;
	}
}

static inline int
accel_mlx5_task_pretranslate_single_mkey(struct accel_mlx5_task *mlx5_task,
		struct accel_mlx5_qp *qp,
		struct iovec *base_addr, struct spdk_memory_domain *domain,
		void *domain_ctx, uint32_t *mkey)
{
	struct ibv_sge sge;
	struct spdk_accel_task *task = &mlx5_task->base;
	int rc;

	if (task->cached_lkey == NULL || *task->cached_lkey == 0 || !domain) {
		rc = accel_mlx5_translate_addr(base_addr->iov_base, base_addr->iov_len,
					       domain, domain_ctx, qp->dev, &sge);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		if (task->cached_lkey && domain) {
			*task->cached_lkey = sge.lkey;
		}
		*mkey = sge.lkey;
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
	if (!mlx5_task->inplace &&
	    (op_len <= mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset || task->d.iovcnt == 1)) {
		rc = accel_mlx5_task_pretranslate_single_mkey(mlx5_task, mlx5_task->qp, &task->d.iovs[0],
				task->dst_domain, task->dst_domain_ctx, dst_lkey);
	}

	return rc;
}

static inline int
accel_mlx5_configure_crypto_umr(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_qp *qp,
				struct accel_mlx5_sge *sge, uint32_t mkey, uint32_t src_lkey, uint32_t dst_lkey,
				uint32_t num_blocks, uint64_t wr_id, uint32_t flags)
{
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mkey,
		.sge = sge->src_sge,
	};
	struct spdk_accel_task *task = &mlx5_task->base;
	uint32_t length, remaining = 0, block_size = task->block_size;
	int rc;

	length = num_blocks * block_size;
	SPDK_DEBUGLOG(accel_mlx5, "task %p, domain %p, len %u, blocks %u\n", task, task->src_domain, length,
		      num_blocks);
	rc = accel_mlx5_fill_block_sge(qp, sge->src_sge, SPDK_COUNTOF(sge->src_sge), &mlx5_task->src,
				       src_lkey, length, &remaining, task->src_domain,
				       task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	sge->src_sge_count = rc;
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

			/* We can't process part of block, need to unwind src iov_sgl and sge to the
			 * prev block boundary */
			SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind src sge for %u bytes\n", task, dt);
			accel_mlx5_iov_sgl_unwind(&mlx5_task->src, task->s.iovcnt, dt);
			sge->src_sge_count = accel_mlx5_sge_unwind(sge->src_sge, sge->src_sge_count, dt);
			if (!sge->src_sge_count) {
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
	cattr.enc_order = mlx5_task->enc_order;
	cattr.bs_selector = bs_to_bs_selector(block_size);
	if (spdk_unlikely(!cattr.bs_selector)) {
		SPDK_ERRLOG("unsupported block size %u\n", block_size);
		return -EINVAL;
	}

	if (!mlx5_task->inplace) {
		SPDK_DEBUGLOG(accel_mlx5, "task %p, dst sge, domain %p, len %u\n", task, task->dst_domain, length);
		rc = accel_mlx5_fill_block_sge(qp, sge->dst_sge, SPDK_COUNTOF(sge->dst_sge),
					       &mlx5_task->dst, dst_lkey, length, &remaining,
					       task->dst_domain, task->dst_domain_ctx);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		sge->dst_sge_count = rc;
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
				/* We can't process part of block, need to unwind src and dst iov_sgl and sge to the
				 * prev block boundary */
				SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind dst sge for %u bytes\n", task, dt);
				accel_mlx5_iov_sgl_unwind(&mlx5_task->dst, task->d.iovcnt, dt);
				sge->dst_sge_count = accel_mlx5_sge_unwind(sge->dst_sge, sge->dst_sge_count, dt);
				assert(sge->dst_sge_count > 0 && sge->dst_sge_count <= ACCEL_MLX5_MAX_INLINE_SGE);
				if (!sge->dst_sge_count) {
					return -ERANGE;
				}
			}
			assert(length > aligned_len);
			dt = length - aligned_len;
			SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind src sge for %u bytes\n", task, dt);
			/* The same for src iov_sgl and sge. In worst case we can unwind SRC 2 times */
			accel_mlx5_iov_sgl_unwind(&mlx5_task->src, task->s.iovcnt, dt);
			sge->src_sge_count = accel_mlx5_sge_unwind(sge->src_sge, sge->src_sge_count, dt);
			assert(sge->src_sge_count > 0 && sge->src_sge_count <= ACCEL_MLX5_MAX_INLINE_SGE);
			if (!sge->src_sge_count) {
				return -ERANGE;
			}
			SPDK_DEBUGLOG(accel_mlx5, "task %p, UMR len %u -> %u\n", task, length, aligned_len);
			length = aligned_len;
			num_blocks = updated_num_blocks;
		}
	}

	SPDK_DEBUGLOG(accel_mlx5,
		      "task %p: bs %u, iv %"PRIu64", enc_on_tx %d, tweak_mode %d, len %u, mkey %x, blocks %u\n",
		      mlx5_task, task->block_size, cattr.xts_iv, mlx5_task->enc_order, cattr.tweak_mode, length, mkey,
		      num_blocks);

	umr_attr.sge_count = sge->src_sge_count;
	umr_attr.umr_len = length;
	assert((uint32_t)mlx5_task->num_processed_blocks + num_blocks <= UINT16_MAX);
	mlx5_task->num_processed_blocks += num_blocks;

	rc = spdk_mlx5_umr_configure_crypto(qp->qp, &umr_attr, &cattr, wr_id, flags);

	return rc;
}

static inline int
accel_mlx5_crypto_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sges[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	/* First RDMA after UMR must have a SMALL_FENCE */
	uint32_t first_rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	uint32_t src_lkey = 0, dst_lkey = 0;
	uint16_t num_blocks;
	uint16_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint16_t i;
	size_t ops_len;
	int rc;

	assert(qp_slot > 1);
	num_ops = spdk_min(num_ops, qp_slot >> 1);
	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}
	ops_len = mlx5_task->blocks_per_req * num_ops;

	rc = accel_mlx5_task_pretranslate_mkeys(mlx5_task, ops_len, &src_lkey, &dst_lkey);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	mlx5_task->num_wrs = 0;
	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);
	for (i = 0; i < num_ops; i++) {
		if (mlx5_task->num_submitted_reqs + i + 1 == mlx5_task->num_reqs) {
			/* Last request may consume less than calculated */
			assert(mlx5_task->num_blocks > mlx5_task->num_processed_blocks);
			num_blocks = mlx5_task->num_blocks - mlx5_task->num_processed_blocks;
		} else {
			num_blocks = mlx5_task->blocks_per_req;
		}
		rc = accel_mlx5_configure_crypto_umr(mlx5_task, qp, &sges[i], mlx5_task->mkeys[i]->mkey,
						     src_lkey, dst_lkey, num_blocks, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		dev->stats.crypto_umrs++;
	}

	/* Loop `num_ops - 1` for easy flags handling */
	for (i = 0; i < num_ops - 1; i++) {
		/* UMR is used as a destination for RDMA_READ - from UMR to sge
		 * XTS is applied on DPS */
		if (mlx5_task->inplace) {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].src_sge, sges[i].src_sge_count, 0,
						    mlx5_task->mkeys[i]->mkey, 0, first_rdma_fence);
		} else {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].dst_sge, sges[i].dst_sge_count, 0,
						    mlx5_task->mkeys[i]->mkey, 0, first_rdma_fence);
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
			return rc;
		}

		first_rdma_fence = 0;
		assert(mlx5_task->num_submitted_reqs < mlx5_task->num_reqs);
		assert(mlx5_task->num_submitted_reqs < UINT16_MAX);
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		dev->stats.rdma_reads++;
	}

	if (mlx5_task->inplace) {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].src_sge, sges[i].src_sge_count, 0,
					    mlx5_task->mkeys[i]->mkey, (uint64_t)mlx5_task, first_rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].dst_sge, sges[i].dst_sge_count, 0,
					    mlx5_task->mkeys[i]->mkey, (uint64_t)mlx5_task, first_rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}

	assert(mlx5_task->num_submitted_reqs < mlx5_task->num_reqs);
	assert(mlx5_task->num_submitted_reqs < UINT16_MAX);
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	dev->stats.rdma_reads++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	if (spdk_unlikely(mlx5_task->num_submitted_reqs == mlx5_task->num_reqs &&
			  mlx5_task->num_blocks > mlx5_task->num_processed_blocks)) {
		/* We hit "out of sge
		 * entries" case with highly fragmented payload. In that case
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
accel_mlx5_crypto_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	/* We need to post at least 1 UMR and 1 RDMA operation */
	if (spdk_unlikely(qp_slot < 2)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	if (task->num_ops == 0) {
		rc = accel_mlx5_task_alloc_mkeys(task, dev->crypto_mkeys);
		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				accel_mlx5_dev_nomem_task_mkey_repeat(dev, task);
			}
			return rc;
		}
	}

	return accel_mlx5_crypto_task_process(task);
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

	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->dev_ctx->pd, &dek_data);
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
		mlx5_task->inplace = 1;
	} else {
		mlx5_task->inplace = 0;
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
		if (dev->crypto_split_blocks) {
			assert(SPDK_CEIL_DIV(num_blocks, dev->crypto_split_blocks) <= UINT16_MAX);
			mlx5_task->num_reqs = SPDK_CEIL_DIV(num_blocks, g_accel_mlx5.attr.crypto_split_blocks);
			/* Last req may consume less blocks */
			mlx5_task->blocks_per_req = spdk_min(num_blocks, g_accel_mlx5.attr.crypto_split_blocks);
		} else {
			if (task->s.iovcnt > ACCEL_MLX5_MAX_INLINE_SGE || task->d.iovcnt > ACCEL_MLX5_MAX_INLINE_SGE) {
				uint32_t max_sge_count = spdk_max(task->s.iovcnt, task->d.iovcnt);

				assert(SPDK_CEIL_DIV(max_sge_count, ACCEL_MLX5_MAX_INLINE_SGE) <= UINT16_MAX);
				mlx5_task->num_reqs = SPDK_CEIL_DIV(max_sge_count, ACCEL_MLX5_MAX_INLINE_SGE);
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

	if (spdk_unlikely(accel_mlx5_dev_get_available_slots(dev, mlx5_task->qp) < 2)) {
		SPDK_DEBUGLOG(accel_mlx5, "dev %s qp %p is full\n", dev->dev_ctx->context->device->name,
			      mlx5_task->qp);
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	rc = accel_mlx5_task_alloc_mkeys(mlx5_task, dev->crypto_mkeys);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		}
		return rc;
	}
	SPDK_DEBUGLOG(accel_mlx5, "task %p, src_iovs %u, dst_iovs %u, num_reqs %u, "
		      "blocks/req %u, blocks %u, inplace %d\n", task, task->s.iovcnt, task->d.iovcnt,
		      mlx5_task->num_reqs, mlx5_task->blocks_per_req, mlx5_task->num_blocks, mlx5_task->inplace);

	return 0;
}

static inline int
accel_mlx5_task_check_sigerr(struct accel_mlx5_task *task)
{
	unsigned i;
	int rc;

	assert(task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C ||
	       task->base.op_code == SPDK_ACCEL_OPC_COPY_CHECK_CRC32C ||
	       task->base.op_code == SPDK_ACCEL_OPC_DIF_VERIFY_COPY);

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

static inline void
accel_mlx5_copy_task_complete(struct accel_mlx5_task *mlx5_task)
{
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline int
accel_mlx5_copy_task_process_one(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_qp *qp,
				 uint64_t wrid, uint32_t fence)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_sge sgl;
	uint32_t remaining;
	uint32_t dst_len;
	int rc;

	/* Limit one RDMA_WRITE by length of dst buffer. Not all src buffers may fit into one dst buffer due to
	 * limitation on ACCEL_MLX5_MAX_INLINE_SGE. If this is the case then remaining is not zero */
	assert(mlx5_task->dst.iov->iov_len > mlx5_task->dst.iov_offset);
	dst_len = mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset;
	rc = accel_mlx5_fill_block_sge(qp, sgl.src_sge, SPDK_COUNTOF(sgl.src_sge),
				       &mlx5_task->src, 0, dst_len, &remaining, task->src_domain,
				       task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	sgl.src_sge_count = rc;
	assert(dst_len > remaining);
	dst_len -= remaining;

	rc = accel_mlx5_fill_block_sge(qp, sgl.dst_sge, SPDK_COUNTOF(sgl.dst_sge),
				       &mlx5_task->dst, 0, dst_len,  &remaining, task->dst_domain,
				       task->dst_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
		return rc;
	}
	if (spdk_unlikely(remaining)) {
		SPDK_ERRLOG("Incorrect dst length, remaining %u\n", remaining);
		assert(0);
		return -EINVAL;
	}

	rc = spdk_mlx5_qp_rdma_write(mlx5_task->qp->qp, sgl.src_sge, sgl.src_sge_count,
				     sgl.dst_sge[0].addr, sgl.dst_sge[0].lkey, wrid, fence);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("new RDMA WRITE failed with %d\n", rc);
		return rc;
	}
	qp->dev->stats.rdma_writes++;

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
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		mlx5_task->num_submitted_reqs++;
	}

	rc = accel_mlx5_copy_task_process_one(mlx5_task, qp, (uint64_t)mlx5_task,
					      SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	mlx5_task->num_submitted_reqs++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, copy task, %p\n", mlx5_task);

	return 0;
}

static inline int
accel_mlx5_copy_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	task->num_ops = spdk_min(qp_slot, task->num_reqs - task->num_completed_reqs);
	if (spdk_unlikely(task->num_ops == 0)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	return accel_mlx5_copy_task_process(task);
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
			dst_offset = 0;
			src_offset += dst_len;
			dst++;
			num_ops++;
			src_sge_count = 0;
		} else if (src_len < dst_len) {
			dst_offset += src_len;
			src_offset = 0;
			src++;
			if (++src_sge_count >= ACCEL_MLX5_MAX_INLINE_SGE) {
				num_ops++;
				src_sge_count = 0;
			}
		} else {
			dst_offset = 0;
			src_offset = 0;
			dst++;
			src++;
			num_ops++;
			src_sge_count = 0;
		}
	}

	assert(src == src_iovcnt);
	assert(dst == dst_iovcnt);
	assert(src_offset == 0);
	assert(dst_offset == 0);
	return num_ops;
}

static inline int
accel_mlx5_copy_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(qp->dev, qp);
#ifdef DEBUG
	uint64_t src_len = 0;
	uint64_t dst_len = 0;
	uint16_t i;

	for (i = 0; i < task->s.iovcnt; i++) {
		src_len += task->s.iovs[i].iov_len;
	}
	for (i = 0; i < task->d.iovcnt; i++) {
		dst_len += task->d.iovs[i].iov_len;
	}
	if (src_len != dst_len) {
		SPDK_ERRLOG("src len %"PRIu64" doesn't match dst len %"PRIu64"\n", src_len, dst_len);
		return -EINVAL;
	}
#endif

	if (spdk_likely(task->s.iovcnt <= ACCEL_MLX5_MAX_INLINE_SGE)) {
		mlx5_task->num_reqs = task->d.iovcnt;
	} else if (task->d.iovcnt == 1) {
		mlx5_task->num_reqs = SPDK_CEIL_DIV(task->s.iovcnt, ACCEL_MLX5_MAX_INLINE_SGE);
	} else {
		mlx5_task->num_reqs = accel_mlx5_get_copy_task_count(task->s.iovs, task->s.iovcnt,
				      task->d.iovs, task->d.iovcnt);
	}
	mlx5_task->inplace = 0;
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

static inline void
accel_mlx5_sig_task_complete(struct accel_mlx5_task *mlx5_task, int status)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, mlx5_task->mkeys, mlx5_task->num_ops);
	if (mlx5_task->clear_psv_error && status == 0) {
		mlx5_task->psv->bits.error = 0;
	}
	spdk_mlx5_psv_pool_put(&mlx5_task->psv);
	spdk_accel_task_complete(&mlx5_task->base, status);
}

static inline void
accel_mlx5_crc_task_complete(struct accel_mlx5_task *mlx5_task)
{
	int sigerr = 0;

	if (mlx5_task->base.op_code != SPDK_ACCEL_OPC_CHECK_CRC32C &&
	    mlx5_task->base.op_code != SPDK_ACCEL_OPC_COPY_CHECK_CRC32C) {
		*mlx5_task->base.crc_dst = mlx5_task->psv->crc ^ UINT32_MAX;
	} else {
		sigerr = accel_mlx5_task_check_sigerr(mlx5_task);
	}

	accel_mlx5_sig_task_complete(mlx5_task, sigerr);
}

static inline int
accel_mlx5_crc_task_configure_umr(struct accel_mlx5_task *mlx5_task, struct ibv_sge *sge,
				  uint32_t sge_count, struct spdk_mlx5_mkey_pool_obj *mkey,
				  enum spdk_mlx5_umr_sig_domain sig_domain, uint32_t umr_len,
				  bool sig_init, bool sig_check_gen)
{
	struct spdk_mlx5_umr_trans_sig_attr sattr = {
		.seed = mlx5_task->base.seed ^ UINT32_MAX,
		.psv_index = mlx5_task->psv->psv_index,
		.domain = sig_domain,
		.sigerr_count = mkey->sig.sigerr_count,
		.raw_data_size = umr_len,
		.init = sig_init,
		.check_gen = sig_check_gen,
	};
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mkey->mkey,
		.umr_len = umr_len,
		.sge_count = sge_count,
		.sge = sge,
	};

	return spdk_mlx5_umr_configure_trans_sig(mlx5_task->qp->qp, &umr_attr, &sattr, 0, 0);
}

static inline int
accel_mlx5_crc_task_fill_sge(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_sge *sge)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint32_t remaining;
	int rc;

	rc = accel_mlx5_fill_block_sge(qp, sge->src_sge, SPDK_COUNTOF(sge->src_sge),
				       &mlx5_task->src, 0, task->nbytes, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	sge->src_sge_count = rc;

	if (!mlx5_task->inplace) {
		rc = accel_mlx5_fill_block_sge(qp, sge->dst_sge, SPDK_COUNTOF(sge->dst_sge),
					       &mlx5_task->dst, 0, task->nbytes, &remaining,
					       task->dst_domain, task->dst_domain_ctx);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		assert(remaining == 0);
		sge->dst_sge_count = rc;
	}

	return 0;
}

static inline uint64_t
accel_mlx5_get_transient_signature_crc(struct accel_mlx5_task *mlx5_task)
{
	uint64_t crc_seed = *mlx5_task->base.crc_dst;

	return crc_seed << 32;
}

static inline int
accel_mlx5_crc_task_process_one_req(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sges;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
	bool check_op = (mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C ||
			 mlx5_task->base.op_code == SPDK_ACCEL_OPC_COPY_CHECK_CRC32C);
	bool copy_check_op = (mlx5_task->base.op_code == SPDK_ACCEL_OPC_COPY_CHECK_CRC32C);
	bool copy_crc_op = (mlx5_task->base.op_code == SPDK_ACCEL_OPC_COPY_CRC32C);
	struct ibv_sge *sge;
	uint16_t sge_count;
	uint64_t ts;
	int rc;

	assert(accel_mlx5_dev_get_available_slots(dev, qp) > 1);
	assert(mlx5_task->num_ops == 1);
	assert(mlx5_task->num_reqs == 1);

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	rc = accel_mlx5_crc_task_fill_sge(mlx5_task, &sges);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	if (copy_check_op) {
		sge = sges.dst_sge;
		sge_count = sges.dst_sge_count;
	} else {
		sge = sges.src_sge;
		sge_count = sges.src_sge_count;
	}

	rc = accel_mlx5_crc_task_configure_umr(mlx5_task, sge, sge_count, mlx5_task->mkeys[0],
					       SPDK_MLX5_UMR_SIG_DOMAIN_WIRE, mlx5_task->base.nbytes, true, true);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.sig_umrs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);

	if (!copy_crc_op) {
		sge = sges.src_sge;
		sge_count = sges.src_sge_count;
	} else {
		sge = sges.dst_sge;
		sge_count = sges.dst_sge_count;
	}

	/*
	 * Add the crc destination to the end of sge. A free entry must be available for CRC
	 * because the task init function reserved it.
	 */
	assert(sge_count < ACCEL_MLX5_MAX_INLINE_SGE);
	if (check_op) {
		mlx5_task->psv->crc = *mlx5_task->base.crc_dst ^ UINT32_MAX;
	}
	sge[sge_count].lkey = mlx5_task->psv->crc_lkey;
	sge[sge_count].addr = (uintptr_t)&mlx5_task->psv->crc;
	sge[sge_count++].length = sizeof(uint32_t);

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		ts = accel_mlx5_get_transient_signature_crc(mlx5_task);
		rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, ts, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		mlx5_task->clear_psv_error = 1;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (check_op) {
		rc = spdk_mlx5_qp_rdma_write(qp->qp, sge, sge_count, 0, mlx5_task->mkeys[0]->mkey,
					     (uint64_t)mlx5_task, rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
		dev->stats.rdma_writes++;
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sge, sge_count, 0, mlx5_task->mkeys[0]->mkey,
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
accel_mlx5_crc_task_fill_umr_sge(struct accel_mlx5_qp *qp, struct ibv_sge *sge,
				 struct accel_mlx5_iov_sgl *umr_iovs, struct spdk_memory_domain *domain,
				 void *domain_ctx, struct accel_mlx5_iov_sgl *rdma_iovs, size_t *len)
{
	int umr_idx = 0;
	int rdma_idx = 0;
	int umr_iovcnt = spdk_min(umr_iovs->iovcnt, (int)ACCEL_MLX5_MAX_INLINE_SGE);
	int rdma_iovcnt = spdk_min(rdma_iovs->iovcnt, (int)ACCEL_MLX5_MAX_INLINE_SGE);
	size_t umr_iov_offset;
	size_t rdma_iov_offset;
	size_t umr_len = 0;
	void *sge_addr;
	size_t sge_len;
	size_t umr_sge_len;
	size_t rdma_sge_len;
	int rc;

	umr_iov_offset = umr_iovs->iov_offset;
	rdma_iov_offset = rdma_iovs->iov_offset;

	while (umr_idx < umr_iovcnt && rdma_idx < rdma_iovcnt) {
		umr_sge_len = umr_iovs->iov[umr_idx].iov_len - umr_iov_offset;
		rdma_sge_len = rdma_iovs->iov[rdma_idx].iov_len - rdma_iov_offset;
		sge_addr = umr_iovs->iov[umr_idx].iov_base + umr_iov_offset;

		if (umr_sge_len == rdma_sge_len) {
			rdma_idx++;
			umr_iov_offset = 0;
			rdma_iov_offset = 0;
			sge_len = umr_sge_len;
		} else if (umr_sge_len < rdma_sge_len) {
			umr_iov_offset = 0;
			rdma_iov_offset += umr_sge_len;
			sge_len = umr_sge_len;
		} else {
			size_t remaining;

			remaining = umr_sge_len - rdma_sge_len;
			while (remaining) {
				rdma_idx++;
				if (rdma_idx == (int)ACCEL_MLX5_MAX_INLINE_SGE) {
					break;
				}
				rdma_sge_len = rdma_iovs->iov[rdma_idx].iov_len;
				if (remaining == rdma_sge_len) {
					rdma_idx++;
					rdma_iov_offset = 0;
					umr_iov_offset = 0;
					remaining = 0;
					break;
				}
				if (remaining < rdma_sge_len) {
					rdma_iov_offset = remaining;
					umr_iov_offset = 0;
					remaining = 0;
					break;
				}
				remaining -= rdma_sge_len;
			}
			sge_len = umr_sge_len - remaining;
		}
		rc = accel_mlx5_translate_addr(sge_addr, sge_len, domain, domain_ctx, qp->dev, &sge[umr_idx]);
		if (spdk_unlikely(rc)) {
			return -EINVAL;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\t sge[%d] lkey %u, addr %p, len %u\n", umr_idx, sge[umr_idx].lkey,
			      (void *)sge[umr_idx].addr, sge[umr_idx].length);
		umr_len += sge_len;
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
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	struct accel_mlx5_iov_sgl umr_sgl;
	struct accel_mlx5_iov_sgl *sgl_ptr;
	struct accel_mlx5_iov_sgl rdma_sgl;
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	bool check_op = (mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C ||
			 mlx5_task->base.op_code == SPDK_ACCEL_OPC_COPY_CHECK_CRC32C);
	bool copy_crc_op = (mlx5_task->base.op_code == SPDK_ACCEL_OPC_COPY_CRC32C);
	int sge_count;
	uint32_t remaining;
	uint64_t umr_offset;
	bool sig_init, sig_check_gen = false;
	uint16_t i;
	int rc;
	size_t umr_len[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct ibv_sge sge[ACCEL_MLX5_MAX_INLINE_SGE];
	struct spdk_memory_domain *domain;
	void *domain_ctx;
	uint64_t ts;

	assert(qp_slot > 1);
	num_ops = spdk_min(num_ops, qp_slot >> 1);
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
	 * In the out-of-place CRC calculate case, we iterate on the source IOV once and on the destination IOV
	 * two times. So, we need one copy of the destination accel_mlx5_iov_sgl.
	 *
	 * In the copy check case, we iterate on the source IOV twice and on the destination IOV once.
	 * So, we need one copy of source accel_mlx5_iov_sgl.
	 */
	if (mlx5_task->inplace) {
		accel_mlx5_iov_sgl_init(&umr_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
		sgl_ptr = &umr_sgl;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
		domain = task->src_domain;
		domain_ctx = task->src_domain_ctx;
	} else if (copy_crc_op) {
		sgl_ptr = &mlx5_task->src;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->dst.iov, mlx5_task->dst.iovcnt);
		domain = task->src_domain;
		domain_ctx = task->src_domain_ctx;
	} else {
		sgl_ptr = &mlx5_task->dst;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
		domain = task->dst_domain;
		domain_ctx = task->dst_domain_ctx;
	}
	mlx5_task->num_wrs = 0;
	for (i = 0; i < num_ops; i++) {
		/*
		 * The last request may have only CRC. Skip UMR in this case because the MKey from
		 * the previous request is used.
		 */
		if (sgl_ptr->iovcnt == 0) {
			assert((mlx5_task->num_completed_reqs + i + 1) == mlx5_task->num_reqs);
			break;
		}
		sge_count = accel_mlx5_crc_task_fill_umr_sge(qp, sge, sgl_ptr, domain, domain_ctx,
				&rdma_sgl, &umr_len[i]);
		if (spdk_unlikely(sge_count <= 0)) {
			rc = (sge_count == 0) ? -EINVAL : sge_count;
			SPDK_ERRLOG("failed set UMR sge, rc %d\n", rc);
			return rc;
		}
		if (sgl_ptr->iovcnt == 0) {
			/*
			 * We post RDMA without UMR if the last request has only CRC. We use an MKey from
			 * the last UMR in this case. Since the last request can be postponed to the next
			 * call of this function, we must save the MKey to the task structure.
			 */
			mlx5_task->last_umr_len = umr_len[i];
			mlx5_task->last_mkey_idx = i;
			sig_check_gen = true;
		}
		rc = accel_mlx5_crc_task_configure_umr(mlx5_task, sge, sge_count, mlx5_task->mkeys[i],
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
		ts = accel_mlx5_get_transient_signature_crc(mlx5_task);
		rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, ts, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		mlx5_task->clear_psv_error = 1;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (copy_crc_op) {
		sgl_ptr = &mlx5_task->dst;
		domain = task->dst_domain;
		domain_ctx = task->dst_domain_ctx;
	} else {
		sgl_ptr = &mlx5_task->src;
		domain = task->src_domain;
		domain_ctx = task->src_domain_ctx;
	}


	for (i = 0; i < num_ops - 1; i++) {
		sge_count = accel_mlx5_fill_block_sge(qp, sge, SPDK_COUNTOF(sge), sgl_ptr, 0,
						      umr_len[i], &remaining, domain, domain_ctx);
		if (spdk_unlikely(sge_count <= 0)) {
			rc = (sge_count == 0) ? -EINVAL : sge_count;
			SPDK_ERRLOG("failed set RDMA sge, rc %d\n", rc);
			return rc;
		}
		if (check_op) {
			rc = spdk_mlx5_qp_rdma_write(qp->qp, sge, sge_count, 0, mlx5_task->mkeys[i]->mkey,
						     0, rdma_fence);
			dev->stats.rdma_writes++;
		} else {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, sge, sge_count, 0, mlx5_task->mkeys[i]->mkey,
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
	if (sgl_ptr->iovcnt == 0) {
		/*
		 * The last RDMA does not have any data, only CRC. It also does not have a paired Mkey.
		 * The CRC is handled in the previous MKey in this case.
		 */
		sge_count = 0;
		umr_offset = mlx5_task->last_umr_len;
	} else {
		umr_offset = 0;
		mlx5_task->last_mkey_idx = i;
		sge_count = accel_mlx5_fill_block_sge(qp, sge, SPDK_COUNTOF(sge), sgl_ptr, 0,
						      umr_len[i], &remaining, domain, domain_ctx);
		if (spdk_unlikely(sge_count <= 0)) {
			rc = (sge_count == 0) ? -EINVAL : sge_count;
			SPDK_ERRLOG("failed set RDMA sge, rc %d\n", rc);
			return rc;
		}
		assert(remaining == 0);
	}
	if ((mlx5_task->num_completed_reqs + i + 1) == mlx5_task->num_reqs) {
		/* Ensure that there is a free sge for the CRC destination. */
		assert(sge_count < (int)ACCEL_MLX5_MAX_INLINE_SGE);
		/* Add the crc destination to the end of sge. */
		if (check_op) {
			mlx5_task->psv->crc = *mlx5_task->base.crc_dst ^ UINT32_MAX;
		}
		sge[sge_count].lkey = mlx5_task->psv->crc_lkey;
		sge[sge_count].addr = (uintptr_t)&mlx5_task->psv->crc;
		sge[sge_count++].length = sizeof(uint32_t);
	}
	rdma_fence |= SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE;
	if (check_op) {
		rc = spdk_mlx5_qp_rdma_write(qp->qp, sge, sge_count, umr_offset,
					     mlx5_task->mkeys[mlx5_task->last_mkey_idx]->mkey,
					     (uint64_t)mlx5_task, rdma_fence);
		dev->stats.rdma_writes++;
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sge, sge_count, umr_offset,
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


/*
 * The following table describes UMR and RDMA operations configuration for crc tasks.
 *
 * | Operation         | Signature domain | UMR sge | RDMA operation | RDMA sge |
 * |-------------------+------------------+---------+----------------+----------|
 * | crc32c            | Wire             | src     | Read           | src      |
 * | copy_crc32c       | Wire             | src     | Read           | dst      |
 * | check_crc32c      | Wire             | src     | Write          | src      |
 * | copy_check_crc32c | Wire             | dst     | Write          | src      |
 * |-------------------+------------------+---------+----------------+----------|
 */
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
accel_mlx5_task_alloc_sig_ctx(struct accel_mlx5_task *task, struct spdk_mlx5_mkey_pool *pool)
{
	struct accel_mlx5_dev *dev = task->qp->dev;
	int rc;

	rc = accel_mlx5_task_alloc_mkeys(task, pool);
	if (spdk_unlikely(rc)) {
		SPDK_DEBUGLOG(accel_mlx5, "sig mkeys alloc failed, dev %s, rc %d\n",
			      dev->dev_ctx->pd->context->device->name, rc);
		return rc;
	}
	task->psv = spdk_mlx5_psv_pool_get(dev->dev_ctx->psv_pool);
	if (spdk_unlikely(!task->psv)) {
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in psv pool, dev %s\n", dev->dev_ctx->pd->context->device->name);
		spdk_mlx5_mkey_pool_put_bulk(pool, task->mkeys, task->num_ops);
		task->num_ops = 0;
		return -ENOMEM;
	}

	return 0;
}

static inline int
accel_mlx5_crc_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	/* Check that we have enough slots in QP */
	if (spdk_unlikely(qp_slot < 2)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	if (task->num_ops == 0) {
		rc = accel_mlx5_task_alloc_sig_ctx(task, dev->sig_mkeys);
		if (spdk_unlikely(rc != 0)) {
			if (rc == -ENOMEM) {
				accel_mlx5_dev_nomem_task_mkey_repeat(dev, task);
			}
			return rc;
		}
	}
	/* One extra slot is needed for SET_PSV WQE to reset the error state in PSV. */
	if (spdk_unlikely(task->psv->bits.error)) {
		uint32_t n_slots = task->num_ops * 2 + 1;

		if (qp_slot < n_slots) {
			accel_mlx5_dev_nomem_task_qdepth_repeat(qp->dev, task);
			return -ENOMEM;
		}
	}

	return accel_mlx5_crc_task_process(task);
}

static inline uint32_t
accel_mlx5_get_crc_task_count(struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov,
			      uint32_t dst_iovcnt, uint8_t opcode)
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
	bool copy_crc_op = (opcode == SPDK_ACCEL_OPC_COPY_CRC32C);

	/*
	 * One operation is enough if both iovs fit into ACCEL_MLX5_MAX_INLINE_SGE.
	 * One SGE is reserved for CRC on dst_iov for copy_crc operations and on src_iov for others.
	 */
	if (copy_crc_op) {
		if (src_iovcnt <= ACCEL_MLX5_MAX_INLINE_SGE && (dst_iovcnt + 1) <= ACCEL_MLX5_MAX_INLINE_SGE) {
			return 1;
		}
	} else {
		if (dst_iovcnt <= ACCEL_MLX5_MAX_INLINE_SGE && (src_iovcnt + 1) <= ACCEL_MLX5_MAX_INLINE_SGE) {
			return 1;
		}
	}

	while (src_idx < src_iovcnt && dst_idx < dst_iovcnt) {
		if (num_src_sge > ACCEL_MLX5_MAX_INLINE_SGE || num_dst_sge > ACCEL_MLX5_MAX_INLINE_SGE) {
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
							   spdk_min(ACCEL_MLX5_MAX_INLINE_SGE + 1 - num_src_sge,
									   src_iovcnt - src_idx),
							   &src_offset, &dst_len);
			src_idx += num_sge;
			num_src_sge += num_sge;
			if (dst_len != 0) {
				/*
				 * ACCEL_MLX5_MAX_INLINE_SGE is reached on src_iov, and dst_len bytes
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
							   spdk_min(ACCEL_MLX5_MAX_INLINE_SGE + 1 - num_dst_sge,
									   dst_iovcnt - dst_idx),
							   &dst_offset, &src_len);
			dst_idx += num_sge;
			num_dst_sge += num_sge;
			if (src_len != 0) {
				/*
				 * ACCEL_MLX5_MAX_INLINE_SGE is reached on dst_iov, and src_len bytes
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

	/*
	 * An extra operation is needed if no space is left on dst_iov (for copy_crc operations)
	 * or src_iov (for others) because CRC takes one SGE.
	 */
	if ((copy_crc_op && num_dst_sge > ACCEL_MLX5_MAX_INLINE_SGE) ||
	    (!copy_crc_op && num_src_sge > ACCEL_MLX5_MAX_INLINE_SGE)) {
		num_ops++;
	}

	/* The above loop must reach the end of both iovs simultaneously because their size is the same. */
	assert(src_idx == src_iovcnt);
	assert(dst_idx == dst_iovcnt);
	assert(src_offset == 0);
	assert(dst_offset == 0);

	return num_ops;
}

static inline int
accel_mlx5_crc_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	if (mlx5_task->inplace) {
		/* One entry is reserved for CRC */
		mlx5_task->num_reqs = SPDK_CEIL_DIV(mlx5_task->src.iovcnt + 1, ACCEL_MLX5_MAX_INLINE_SGE);
	} else {
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
		mlx5_task->num_reqs = accel_mlx5_get_crc_task_count(mlx5_task->src.iov, mlx5_task->src.iovcnt,
				      mlx5_task->dst.iov, mlx5_task->dst.iovcnt, task->op_code);
	}

	if (spdk_unlikely(qp_slot < 2)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, mlx5_task);
		return -ENOMEM;
	}

	rc = accel_mlx5_task_alloc_sig_ctx(mlx5_task, dev->sig_mkeys);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		}

		return rc;
	}
	/* One extra slot is needed for SET_PSV WQE to reset the error state in PSV. */
	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		uint32_t n_slots = mlx5_task->num_ops * 2 + 1;

		if (qp_slot < n_slots) {
			accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
			return -ENOMEM;
		}
	}

	return 0;
}

static inline int
accel_mlx5_crc_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sge;
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct spdk_mlx5_mkey_pool_obj *mkey = mlx5_task->mkeys[0];
	struct accel_mlx5_dev *dev = qp->dev;
	uint64_t ts;
	uint32_t remaining;
	int rc;
	bool check_op = (mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C);

	struct spdk_mlx5_umr_trans_sig_attr sattr = {
		.seed = mlx5_task->base.seed ^ UINT32_MAX,
		.psv_index = mlx5_task->psv->psv_index,
		.domain = SPDK_MLX5_UMR_SIG_DOMAIN_WIRE,
		.sigerr_count = mkey->sig.sigerr_count,
		.raw_data_size = mlx5_task->base.nbytes,
		.init = true,
		.check_gen = true,
	};
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mkey->mkey,
		.umr_len = mlx5_task->base.nbytes,
	};

	assert(mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C_MKEY);

	SPDK_DEBUGLOG(accel_mlx5, "begin, crc_mkey task, %p\n", mlx5_task);


	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}

	mlx5_task->num_wrs = 0;

	rc = accel_mlx5_fill_block_sge(qp, sge.src_sge, SPDK_COUNTOF(sge.src_sge),
				       &mlx5_task->src, 0, task->nbytes, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	sge.src_sge_count = rc;
	umr_attr.sge_count = sge.src_sge_count;
	umr_attr.sge = sge.src_sge;

	rc = spdk_mlx5_umr_configure_trans_sig(mlx5_task->qp->qp, &umr_attr, &sattr, (uint64_t)mlx5_task,
					       SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);

	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.sig_umrs++;

	if (check_op) {
		mlx5_task->psv->crc = *mlx5_task->base.crc_dst ^ UINT32_MAX;
	}

	if (spdk_likely(!mlx5_task->psv->bits.error)) {
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	} else {
		/* First unsignaled for UMR */
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);

		ts = accel_mlx5_get_transient_signature_crc(mlx5_task);
		rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, ts, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		mlx5_task->clear_psv_error = 1;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	}
	mlx5_task->num_submitted_reqs++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, crc_mkey task, %p\n", mlx5_task);

	return rc;
}

static inline int
accel_mlx5_crc_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	if (spdk_unlikely(task->s.iovcnt > ACCEL_MLX5_MAX_INLINE_SGE)) {
		SPDK_ERRLOG("src buffer is too fragmented: %u (max: %u)\n", task->s.iovcnt,
			    ACCEL_MLX5_MAX_INLINE_SGE);
		return -EINVAL;
	}
	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	mlx5_task->num_reqs = 1;

	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, mlx5_task);
		return -ENOMEM;
	}

	rc = accel_mlx5_task_alloc_sig_mkey_ctx(mlx5_task);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		}

		return rc;
	}
	/* One extra slot is needed for SET_PSV WQE to reset the error state in PSV. */
	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		if (qp_slot < 2) {
			accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
			return -ENOMEM;
		}
	}

	return 0;
}

static inline int
accel_mlx5_crc_mkey_task_epilogue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct ibv_sge src_sge;
	int rc;

	assert(task->mkeys[0]);
	/* psv was attached to mkey */
	src_sge.lkey = task->psv->crc_lkey;
	src_sge.addr = (uintptr_t)&task->psv->crc;
	src_sge.length = sizeof(uint32_t);

	if (task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C) {
		rc = spdk_mlx5_qp_rdma_write(qp->qp, &src_sge, 1, task->base.nbytes, task->mkeys[0]->mkey,
					     (uint64_t)task, SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
		dev->stats.rdma_writes++;
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, &src_sge, 1, task->base.nbytes, task->mkeys[0]->mkey,
					    (uint64_t)task, SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
		dev->stats.rdma_reads++;
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}
	task->num_submitted_reqs = 1;
	task->num_completed_reqs = 0;
	task->num_wrs = 0;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, task);
	STAILQ_INSERT_TAIL(&qp->in_hw, task, link);
	task->needs_epilogue = false;

	return 0;
}

static inline int
accel_mlx5_crc_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	/* Check that we have enough slots in QP */
	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}

	if (task->needs_epilogue && !task->needs_data_transfer) {
		/* data has been transferred, now we need to issue 4 bytes RDMA_WRITE to update the CRC */
		rc = accel_mlx5_crc_mkey_task_epilogue(task);
		if (spdk_unlikely(rc)) {
			accel_mlx5_task_fail(task, rc);
			return rc;
		}
		return 0;
	}
	if (task->num_ops == 0) {
		rc = accel_mlx5_task_alloc_sig_mkey_ctx(task);
		if (spdk_unlikely(rc != 0)) {
			if (rc == -ENOMEM) {
				accel_mlx5_dev_nomem_task_mkey_repeat(dev, task);
			}
			return rc;
		}
	}
	/* One extra slot is needed for SET_PSV WQE to reset the error state in PSV. */
	if (spdk_unlikely(task->psv->bits.error)) {
		if (qp_slot < 2) {
			accel_mlx5_dev_nomem_task_qdepth_repeat(qp->dev, task);
			return -ENOMEM;
		}
	}

	return accel_mlx5_crc_mkey_task_process(task);
}

static inline void
accel_mlx5_crc_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int sigerr = 0;

	if (mlx5_task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C) {
		sigerr = accel_mlx5_task_check_sigerr(mlx5_task);
	} else {
		*mlx5_task->base.crc_dst = mlx5_task->psv->crc ^ UINT32_MAX;
	}
	if (mlx5_task->clear_psv_error && sigerr == 0) {
		mlx5_task->psv->bits.error = 0;
	}
	spdk_mlx5_mkey_pool_put(dev->sig_mkeys, mlx5_task->mkeys[0]);
	spdk_mlx5_psv_pool_put(&mlx5_task->psv);
	spdk_accel_task_complete(&mlx5_task->base, sigerr);
}

static inline int
accel_mlx5_crypto_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sge;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(accel_mlx5, "begin, task %p, dst_domain_ctx %p\n", mlx5_task,
		      mlx5_task->base.dst_domain_ctx);

	mlx5_task->num_wrs = 0;
	rc = accel_mlx5_configure_crypto_umr(mlx5_task, qp, &sge, mlx5_task->mkeys[0]->mkey, 0, 0,
					     mlx5_task->num_blocks, (uint64_t)mlx5_task, SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.crypto_umrs++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task %p, dst_domain_ctx %p\n", mlx5_task,
		      mlx5_task->base.dst_domain_ctx);

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	if (task->num_ops == 0) {
		task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->crypto_mkeys);
		if (spdk_unlikely(task->mkeys[0] == NULL)) {
			accel_mlx5_dev_nomem_task_mkey_repeat(qp->dev, task);
			return -ENOMEM;
		}
		task->num_ops = 1;
	}

	return accel_mlx5_crypto_mkey_task_process(task);
}

static inline void
accel_mlx5_crypto_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	assert(mlx5_task->num_ops);
	assert(mlx5_task->num_processed_blocks == mlx5_task->num_blocks);
	assert(mlx5_task->base.seq);

	spdk_mlx5_mkey_pool_put(dev->crypto_mkeys, mlx5_task->mkeys[0]);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline int
accel_mlx5_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	size_t max_sges;
	int rc;

	assert(g_accel_mlx5.attr.enable_driver);
	max_sges = g_accel_mlx5.attr.umr_memory_buffer ? ACCEL_MLX5_MAX_MB_SGE :
		   ACCEL_MLX5_MAX_INLINE_SGE;
	if (spdk_unlikely(task->s.iovcnt > max_sges)) {
		/* With `external mkey` we can't split task or register several UMRs */
		SPDK_ERRLOG("src buffer is too fragmented: %u (max: %zu)\n", task->s.iovcnt,
			    max_sges);
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

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	mlx5_task->num_reqs = 1;

	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->mkeys);
	if (spdk_unlikely(!mlx5_task->mkeys[0])) {
		accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		return -ENOMEM;
	}
	if (task->s.iovcnt > ACCEL_MLX5_MAX_INLINE_SGE) {
		rc = accel_mlx5_task_alloc_umr_mb(mlx5_task, 1);
		if (spdk_unlikely(rc != 0)) {
			spdk_mlx5_mkey_pool_put(dev->mkeys, mlx5_task->mkeys[0]);
			accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
			return -ENOMEM;
		}
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "mkey task num_blocks %u, src_len %zu\n", mlx5_task->num_reqs,
		      task->nbytes);

	return 0;
}

static inline int
accel_mlx5_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct ibv_sge src_sge[ACCEL_MLX5_MAX_SGE];
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mlx5_task->mkeys[0]->mkey,
		.mb = mlx5_task->num_mbs ? mlx5_task->umr_mb[0] : NULL,
		.sge = src_sge,
		.umr_len = task->nbytes,
	};
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t remaining = 0;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(accel_mlx5, "begin, task %p, dst_domain_ctx %p\n", mlx5_task, task->dst_domain_ctx);

	mlx5_task->num_wrs = 0;

	rc = accel_mlx5_fill_block_sge(qp, src_sge, SPDK_COUNTOF(src_sge), &mlx5_task->src, 0,
				       task->nbytes, &remaining, task->src_domain,
				       task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0 || remaining)) {
		rc = rc ? rc : -EINVAL;
		SPDK_ERRLOG("Failed to set src sge, rc %d, remaining %u\n", rc, remaining);
		return rc;
	}

	umr_attr.sge_count = rc;

	rc = spdk_mlx5_umr_configure(qp->qp, &umr_attr, (uint64_t)mlx5_task,
				     SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.umrs++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task %p, dst_domain_ctx %p\n", mlx5_task, task->dst_domain_ctx);

	return 0;
}

static inline int
accel_mlx5_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	if (task->num_ops == 0) {
		task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->mkeys);
		if (spdk_unlikely(!task->mkeys[0])) {
			accel_mlx5_dev_nomem_task_mkey_repeat(dev, task);
			return -ENOMEM;
		}
		if (task->base.s.iovcnt > ACCEL_MLX5_MAX_INLINE_SGE) {
			rc = accel_mlx5_task_alloc_umr_mb(task, 1);
			if (spdk_unlikely(rc != 0)) {
				spdk_mlx5_mkey_pool_put(dev->mkeys, task->mkeys[0]);
				accel_mlx5_dev_nomem_task_mkey_repeat(dev, task);
				return -ENOMEM;
			}
		}
		task->num_ops = 1;
	}

	return accel_mlx5_mkey_task_process(task);
}

static inline void
accel_mlx5_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	assert(mlx5_task->num_ops);
	assert(mlx5_task->base.seq);

	spdk_mlx5_mkey_pool_put(dev->mkeys, mlx5_task->mkeys[0]);
	accel_mlx5_task_free_umr_mb(mlx5_task);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline int
accel_mlx5_crypto_mkey_task_init_common(struct accel_mlx5_task *mlx5_task,
					struct accel_mlx5_dev *dev)
{
	struct spdk_mlx5_crypto_dek_data dek_data = {};
	struct spdk_accel_task *task = &mlx5_task->base;
	uint32_t num_blocks;
	int rc;
	bool crypto_key_ok;

	if (spdk_unlikely(task->s.iovcnt > ACCEL_MLX5_MAX_INLINE_SGE)) {
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
	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->dev_ctx->pd, &dek_data);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	mlx5_task->dek_obj_id = dek_data.dek_obj_id;
	mlx5_task->tweak_mode = dek_data.tweak_mode;

	num_blocks = task->nbytes / mlx5_task->base.block_size;
	if (dev->crypto_multi_block) {
		if (spdk_unlikely(g_accel_mlx5.attr.crypto_split_blocks &&
				  num_blocks > g_accel_mlx5.attr.crypto_split_blocks)) {
			SPDK_ERRLOG("Number of blocks in task %u exceeds split threshold %u, can't handle\n",
				    num_blocks, g_accel_mlx5.attr.crypto_split_blocks);
			return -E2BIG;
		}
	} else if (num_blocks != 1) {
		SPDK_ERRLOG("Task contains more than 1 block, can't handle\n");
		return -E2BIG;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	mlx5_task->num_blocks = num_blocks;
	mlx5_task->num_processed_blocks = 0;
	mlx5_task->num_reqs = 1;
	mlx5_task->blocks_per_req = num_blocks;

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_ext_qp_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;

	rc = accel_mlx5_crypto_mkey_task_init_common(mlx5_task, dev);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	mlx5_task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->crypto_mkeys);
	if (spdk_unlikely(mlx5_task->mkeys[0] == NULL)) {
		accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "crypto_mkey_ext_qp task num_blocks %u, src_len %zu\n",
		      mlx5_task->num_reqs, mlx5_task->base.nbytes);

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	rc = accel_mlx5_crypto_mkey_task_init_common(mlx5_task, dev);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->crypto_mkeys);
	if (spdk_unlikely(mlx5_task->mkeys[0] == NULL)) {
		accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "crypto_mkey task num_blocks %u, src_len %zu\n", mlx5_task->num_reqs,
		      mlx5_task->base.nbytes);

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_ext_qp_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_sge sge;
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mlx5_task->mkeys[0]->mkey,
		.sge = sge.src_sge,
		.umr_len = task->nbytes,
	};
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t block_size, remaining, num_blocks;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(accel_mlx5, "begin, task %p, dst_domain_ctx %p\n", mlx5_task,
		      mlx5_task->base.dst_domain_ctx);

	num_blocks = mlx5_task->num_blocks;
	block_size = task->block_size;
	SPDK_DEBUGLOG(accel_mlx5, "task %p, src sge, domain %p, len %"PRIu64"\n", task,
		      task->src_domain, task->nbytes);
	rc = accel_mlx5_fill_block_sge(qp, sge.src_sge, SPDK_COUNTOF(sge.src_sge),
				       &mlx5_task->src, 0, task->nbytes, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	sge.src_sge_count = rc;
	if (spdk_unlikely(remaining)) {
		SPDK_ERRLOG("task %p, can't handle fragmented payload, remaining %u\n", task, remaining);
		return -ERANGE;
	}
	cattr.xts_iv = task->iv + mlx5_task->num_processed_blocks;
	cattr.keytag = 0;
	cattr.dek_obj_id = mlx5_task->dek_obj_id;
	cattr.tweak_mode = mlx5_task->tweak_mode;
	cattr.enc_order = mlx5_task->enc_order;
	cattr.bs_selector = bs_to_bs_selector(block_size);
	if (spdk_unlikely(!cattr.bs_selector)) {
		SPDK_ERRLOG("unsupported block size %u\n", block_size);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(accel_mlx5,
		      "task %p: bs %u, iv %"PRIu64", enc_on_tx %d, tweak_mode %d, len %"PRIu64", dv_mkey %x, blocks %u\n",
		      mlx5_task, task->block_size, cattr.xts_iv, mlx5_task->enc_order, cattr.tweak_mode,
		      task->nbytes, umr_attr.mkey, num_blocks);

	umr_attr.sge_count = sge.src_sge_count;
	mlx5_task->num_processed_blocks += num_blocks;

	rc = spdk_mlx5_umr_configure_crypto(mlx5_task->crypto_external_qp.ext_qp, &umr_attr, &cattr, 0, 0);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	dev->stats.crypto_umrs++;
	mlx5_task->num_submitted_reqs++;
	mlx5_task->num_completed_reqs++;

	SPDK_DEBUGLOG(accel_mlx5, "end, task %p, dst_domain_ctx %p\n", mlx5_task,
		      mlx5_task->base.dst_domain_ctx);

	accel_mlx5_memory_domain_transfer(mlx5_task, mlx5_task->mkeys[0]);

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_ext_qp_task_continue(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	/* We can only be here if mkey pool was empty */
	assert(mlx5_task->num_ops == 0);
	mlx5_task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->crypto_mkeys);
	if (spdk_unlikely(mlx5_task->mkeys[0] == NULL)) {
		accel_mlx5_dev_nomem_task_mkey_repeat(dev, mlx5_task);
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	return accel_mlx5_crypto_mkey_ext_qp_task_process(mlx5_task);
}

static inline int
accel_mlx5_dif_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	if (spdk_max(task->s.iovcnt, task->d.iovcnt) > ACCEL_MLX5_MAX_INLINE_SGE) {
		/* Only a single request is supported for now. */
		SPDK_ERRLOG("buffer is too fragmented\n");
		return -ENOTSUP;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
	mlx5_task->num_reqs = 1;

	if (spdk_unlikely(qp_slot < 3)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	rc = accel_mlx5_task_alloc_sig_ctx(mlx5_task, dev->sig_mkeys);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			accel_mlx5_dev_nomem_task_mkey(dev, mlx5_task);
		}
		return rc;
	}

	return 0;
}

static inline void
accel_mlx5_set_sig_block_domain(struct spdk_mlx5_sig_block_domain *domain,
				struct accel_mlx5_task *mlx5_task,
				const struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_mlx5_sig_t10dif *dif = &domain->sig.dif;

	domain->psv_index = mlx5_task->psv->psv_index;
	domain->bs_selector = bs_to_bs_selector(dif_ctx->block_size - dif_ctx->md_size);

	dif->ref_tag = dif_ctx->init_ref_tag;
	dif->app_tag = dif_ctx->app_tag;
	dif->apptag_mask = dif_ctx->apptag_mask;
	if (dif_ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		dif->flags |= SPDK_MLX5_SIG_T10DIF_FLAGS_REF_REMAP;
	}
	switch (dif_ctx->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		dif->flags |= SPDK_MLX5_SIG_T10DIF_FLAGS_APP_ESCAPE;
		break;
	case SPDK_DIF_TYPE3:
		dif->flags |= SPDK_MLX5_SIG_T10DIF_FLAGS_APP_REF_ESCAPE;
		break;
	default:
		break;
	}
}

static inline int
accel_mlx5_dif_task_configure_umr(struct accel_mlx5_task *mlx5_task,
				  enum spdk_mlx5_sig_type mem_sig_type,
				  enum spdk_mlx5_sig_type wire_sig_type,
				  struct ibv_sge *sge, uint32_t sge_count,
				  struct spdk_mlx5_mkey_pool_obj *mkey,
				  uint32_t umr_len)
{
	struct spdk_mlx5_umr_block_sig_attr sig_attr = {
		.sigerr_count = mkey->sig.sigerr_count,
	};
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mkey->mkey,
		.umr_len = umr_len,
		.sge_count = sge_count,
		.sge = sge,
	};
	struct spdk_mlx5_sig_block_domain *mem = &sig_attr.mem;
	struct spdk_mlx5_sig_block_domain *wire = &sig_attr.wire;
	const struct spdk_dif_ctx *dif_ctx = mlx5_task->base.dif.ctx;

	if (dif_ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		sig_attr.check_mask = SPDK_MLX5_SIG_MASK_T10DIF_GUARD;
	}
	if (dif_ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		sig_attr.check_mask |= SPDK_MLX5_SIG_MASK_T10DIF_REFTAG;
	}
	if (dif_ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		sig_attr.check_mask |= SPDK_MLX5_SIG_MASK_T10DIF_APPTAG;
	}

	mem->sig_type = mem_sig_type;
	if (mem_sig_type == SPDK_MLX5_SIG_TYPE_T10DIF) {
		accel_mlx5_set_sig_block_domain(mem, mlx5_task, dif_ctx);
	}

	wire->sig_type = wire_sig_type;
	if (wire_sig_type == SPDK_MLX5_SIG_TYPE_T10DIF) {
		accel_mlx5_set_sig_block_domain(wire, mlx5_task, dif_ctx);
	}

	return spdk_mlx5_umr_configure_block_sig(mlx5_task->qp->qp, &umr_attr,
			&sig_attr, 0, 0);
}

static inline int
accel_mlx5_dif_task_fill_sge(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_sge *sge,
			     uint32_t src_len, uint32_t dst_len)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint32_t remaining;
	int rc;

	rc = accel_mlx5_fill_block_sge(qp, sge->src_sge, SPDK_COUNTOF(sge->src_sge),
				       &mlx5_task->src, 0, src_len, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	sge->src_sge_count = rc;

	rc = accel_mlx5_fill_block_sge(qp, sge->dst_sge, SPDK_COUNTOF(sge->dst_sge),
				       &mlx5_task->dst, 0, dst_len, &remaining,
				       task->dst_domain, task->dst_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	sge->dst_sge_count = rc;

	return 0;
}

static inline uint64_t
accel_mlx5_get_transient_signature_dif(struct accel_mlx5_task *mlx5_task)
{
	const struct spdk_dif_ctx *dif_ctx = mlx5_task->base.dif.ctx;

	return (uint64_t)dif_ctx->app_tag << 32 | dif_ctx->init_ref_tag;
}

static inline int
accel_mlx5_dif_task_process_one_req(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_sge sges;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	const struct spdk_dif_ctx *dif_ctx = task->dif.ctx;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
	uint32_t data_block_size, src_len, dst_len;
	uint64_t ts;
	int rc;

	num_ops = spdk_min(num_ops, qp_slot >> 1);
	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	data_block_size = dif_ctx->block_size - dif_ctx->md_size;

	if (mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_DIF_GENERATE_COPY) {
		src_len = data_block_size * task->dif.num_blocks;
		dst_len = task->nbytes;
	} else {
		src_len = task->nbytes;
		dst_len = data_block_size * task->dif.num_blocks;
	}

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	rc = accel_mlx5_dif_task_fill_sge(mlx5_task, &sges, src_len, dst_len);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	if (mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_DIF_GENERATE_COPY) {
		rc = accel_mlx5_dif_task_configure_umr(mlx5_task,
						       SPDK_MLX5_SIG_TYPE_NONE,
						       SPDK_MLX5_SIG_TYPE_T10DIF,
						       sges.src_sge, sges.src_sge_count,
						       mlx5_task->mkeys[0], src_len);
	} else {
		rc = accel_mlx5_dif_task_configure_umr(mlx5_task,
						       SPDK_MLX5_SIG_TYPE_NONE,
						       SPDK_MLX5_SIG_TYPE_T10DIF,
						       sges.dst_sge, sges.dst_sge_count,
						       mlx5_task->mkeys[0], dst_len);
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	dev->stats.sig_umrs++;

	ts = accel_mlx5_get_transient_signature_dif(mlx5_task);
	rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, ts, 0, 0);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);

	if (mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_DIF_GENERATE_COPY) {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sges.dst_sge, sges.dst_sge_count, 0,
					    mlx5_task->mkeys[0]->mkey, (uint64_t)mlx5_task,
					    rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	} else {
		rc = spdk_mlx5_qp_rdma_write(qp->qp, sges.src_sge, sges.src_sge_count, 0,
					     mlx5_task->mkeys[0]->mkey, (uint64_t)mlx5_task,
					     rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	dev->stats.rdma_reads++;

	return 0;
}

static inline int
accel_mlx5_dif_task_process(struct accel_mlx5_task *mlx5_task)
{
	int rc = 0;

	SPDK_DEBUGLOG(accel_mlx5,
		      "begin, dif task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs,
		      mlx5_task->num_completed_reqs);

	rc = accel_mlx5_dif_task_process_one_req(mlx5_task);

	if (rc == 0) {
		STAILQ_INSERT_TAIL(&mlx5_task->qp->in_hw, mlx5_task, link);
		SPDK_DEBUGLOG(accel_mlx5,
			      "end, dif task, %p, reqs: total %u, submitted %u, completed %u\n",
			      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs,
			      mlx5_task->num_completed_reqs);
	}

	return rc;
}

static inline int
accel_mlx5_dif_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	/* We need to post at least 1 UMR and 1 RDMA operation */
	if (spdk_unlikely(qp_slot < 2)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	assert(task->num_reqs > task->num_completed_reqs);
	if (task->num_ops == 0) {
		/* No mkeys allocated, try to allocate now. */
		rc = accel_mlx5_task_alloc_sig_ctx(task, dev->sig_mkeys);
		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
			}
			return rc;
		}
	}

	return accel_mlx5_dif_task_process(task);
}

static inline void
accel_mlx5_dif_task_complete(struct accel_mlx5_task *mlx5_task)
{
	int sigerr = 0;

	if (mlx5_task->base.op_code == SPDK_ACCEL_OPC_DIF_VERIFY_COPY) {
		sigerr = accel_mlx5_task_check_sigerr(mlx5_task);
	}

	accel_mlx5_sig_task_complete(mlx5_task, sigerr);
}

static inline int
accel_mlx5_task_alloc_sig_mkey_ctx(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;

	mlx5_task->mkeys[0] = spdk_mlx5_mkey_pool_get(dev->sig_mkeys);
	if (spdk_unlikely(mlx5_task->mkeys[0] == NULL)) {
		return -ENOMEM;
	}

	mlx5_task->num_ops = 1;

	mlx5_task->psv = spdk_mlx5_psv_pool_get(dev->dev_ctx->psv_pool);
	if (spdk_unlikely(mlx5_task->psv == NULL)) {
		spdk_mlx5_mkey_pool_put(dev->sig_mkeys, mlx5_task->mkeys[0]);
		mlx5_task->num_ops = 0;
		return -ENOMEM;
	}

	return 0;
}

static inline int
accel_mlx5_dif_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	if (spdk_max(task->s.iovcnt, task->d.iovcnt) > ACCEL_MLX5_MAX_INLINE_SGE) {
		SPDK_ERRLOG("buffer is too fragmented\n");
		return -ENOTSUP;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
	mlx5_task->num_reqs = 1;

	if (spdk_unlikely(qp_slot < 2)) {
		accel_mlx5_dev_nomem_task_qdepth(dev, mlx5_task);
		return -ENOMEM;
	}
	rc = accel_mlx5_task_alloc_sig_mkey_ctx(mlx5_task);
	if (spdk_unlikely(rc)) {
		accel_mlx5_dev_nomem_task_mkey_repeat(dev, mlx5_task);
		return rc;
	}

	return 0;
}

static inline int
accel_mlx5_dif_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct ibv_sge sges[ACCEL_MLX5_MAX_INLINE_SGE];
	uint32_t sge_count, remaining, umr_len;
	enum spdk_mlx5_sig_type mem_sig_type, wire_sig_type;
	uint64_t ts;
	int rc;

	if (spdk_unlikely(mlx5_task->num_ops == 0)) {
		return -EINVAL;
	}

	SPDK_DEBUGLOG(accel_mlx5, "begin, task %p, dst_domain_ctx %p\n",
		      mlx5_task, task->dst_domain_ctx);

	if (mlx5_task->md_in_umr) {
		umr_len = task->nbytes;
		mem_sig_type = SPDK_MLX5_SIG_TYPE_T10DIF;
		wire_sig_type = SPDK_MLX5_SIG_TYPE_NONE;
	} else {
		umr_len = (task->dif.ctx->block_size - task->dif.ctx->md_size) *
			  task->dif.num_blocks;
		mem_sig_type = SPDK_MLX5_SIG_TYPE_NONE;
		wire_sig_type = SPDK_MLX5_SIG_TYPE_T10DIF;
	}

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	rc = accel_mlx5_fill_block_sge(qp, sges, SPDK_COUNTOF(sges), &mlx5_task->src, 0, umr_len,
				       &remaining, task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	sge_count = rc;

	rc = accel_mlx5_dif_task_configure_umr(mlx5_task, mem_sig_type, wire_sig_type,
					       sges, sge_count, mlx5_task->mkeys[0], umr_len);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	dev->stats.sig_umrs++;

	ts = accel_mlx5_get_transient_signature_dif(mlx5_task);
	rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, ts, (uint64_t)mlx5_task,
				  SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);

	mlx5_task->num_submitted_reqs++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task %p, dst_domain_ctx %p\n",
		      mlx5_task, task->dst_domain_ctx);

	return 0;
}

static inline int
accel_mlx5_dif_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	if (spdk_unlikely(qp_slot == 0)) {
		accel_mlx5_dev_nomem_task_qdepth_repeat(dev, task);
		return -ENOMEM;
	}
	if (task->num_ops == 0) {
		/* No mkeys allocated, try to allocate now. */
		rc = accel_mlx5_task_alloc_sig_mkey_ctx(task);
		if (spdk_unlikely(rc)) {
			accel_mlx5_dev_nomem_task_mkey_repeat(dev, task);
			return -ENOMEM;
		}
	}

	return accel_mlx5_dif_mkey_task_process(task);
}

static inline void
accel_mlx5_dif_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int sigerr = 0;

	if (mlx5_task->base.op_code == SPDK_ACCEL_OPC_DIF_VERIFY_COPY) {
		sigerr = accel_mlx5_task_check_sigerr(mlx5_task);
	}

	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	if (mlx5_task->clear_psv_error && sigerr == 0) {
		mlx5_task->psv->bits.error = 0;
	}
	spdk_mlx5_mkey_pool_put(dev->sig_mkeys, mlx5_task->mkeys[0]);
	spdk_mlx5_psv_pool_put(&mlx5_task->psv);

	spdk_accel_task_complete(&mlx5_task->base, sigerr);
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

static struct accel_mlx5_task_operations g_accel_mlx5_tasks_ops[] = {
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
	[ACCEL_MLX5_OPC_CRYPTO_MKEY] = {
		.init = accel_mlx5_crypto_mkey_task_init,
		.process = accel_mlx5_crypto_mkey_task_process,
		.cont = accel_mlx5_crypto_mkey_task_continue,
		.complete = accel_mlx5_crypto_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_CRYPTO_MKEY_EXT_QP] = {
		.init = accel_mlx5_crypto_mkey_ext_qp_task_init,
		.process = accel_mlx5_crypto_mkey_ext_qp_task_process,
		.cont = accel_mlx5_crypto_mkey_ext_qp_task_continue,
		.complete = accel_mlx5_crypto_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_MKEY] = {
		.init = accel_mlx5_mkey_task_init,
		.process = accel_mlx5_mkey_task_process,
		.cont = accel_mlx5_mkey_task_continue,
		.complete = accel_mlx5_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_CRC32C] = {
		.init = accel_mlx5_crc_task_init,
		.process = accel_mlx5_crc_task_process,
		.cont = accel_mlx5_crc_task_continue,
		.complete = accel_mlx5_crc_task_complete,
	},
	[ACCEL_MLX5_OPC_DIF_GENERATE_COPY] = {
		.init = accel_mlx5_dif_task_init,
		.process = accel_mlx5_dif_task_process,
		.cont = accel_mlx5_dif_task_continue,
		.complete = accel_mlx5_dif_task_complete,
	},
	[ACCEL_MLX5_OPC_DIF_VERIFY_COPY] = {
		.init = accel_mlx5_dif_task_init,
		.process = accel_mlx5_dif_task_process,
		.cont = accel_mlx5_dif_task_continue,
		.complete = accel_mlx5_dif_task_complete,
	},
	[ACCEL_MLX5_OPC_DIF_GENERATE_COPY_MKEY] = {
		.init = accel_mlx5_dif_mkey_task_init,
		.process = accel_mlx5_dif_mkey_task_process,
		.cont = accel_mlx5_dif_mkey_task_continue,
		.complete = accel_mlx5_dif_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_DIF_VERIFY_COPY_MKEY] = {
		.init = accel_mlx5_dif_mkey_task_init,
		.process = accel_mlx5_dif_mkey_task_process,
		.cont = accel_mlx5_dif_mkey_task_continue,
		.complete = accel_mlx5_dif_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_CRC32C_MKEY] = {
		.init = accel_mlx5_crc_mkey_task_init,
		.process = accel_mlx5_crc_mkey_task_process,
		.cont = accel_mlx5_crc_mkey_task_continue,
		.complete = accel_mlx5_crc_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_LAST] = {
		.init = accel_mlx5_task_op_not_supported,
		.process = accel_mlx5_task_op_not_implemented,
		.cont = accel_mlx5_task_op_not_implemented,
		.complete = accel_mlx5_task_op_not_implemented_v
	},
};

static void
accel_mlx5_memory_domain_transfer_cpl(void *ctx, int rc)
{
	struct accel_mlx5_task *task = ctx;

	assert(task->needs_data_transfer);
	task->needs_data_transfer = 0;

	if (spdk_likely(!rc)) {
		SPDK_DEBUGLOG(accel_mlx5, "task %p, data transfer done\n", task);
		accel_mlx5_task_complete(task);
	} else {
		SPDK_ERRLOG("Task %p, data transfer failed, rc %d\n", task, rc);
		accel_mlx5_task_fail(task, rc);
	}
}

static inline void
accel_mlx5_memory_domain_transfer(struct accel_mlx5_task *task,
				  struct spdk_mlx5_mkey_pool_obj *mkey)
{
	struct spdk_memory_domain_translation_result translation;
	struct spdk_accel_task *base = &task->base;
	struct accel_mlx5_dev *dev = task->qp->dev;
	int rc;

	assert(task->mlx5_opcode == ACCEL_MLX5_OPC_CRYPTO_MKEY ||
	       task->mlx5_opcode == ACCEL_MLX5_OPC_CRYPTO_MKEY_EXT_QP ||
	       task->mlx5_opcode == ACCEL_MLX5_OPC_MKEY ||
	       task->mlx5_opcode == ACCEL_MLX5_OPC_DIF_GENERATE_COPY_MKEY ||
	       task->mlx5_opcode == ACCEL_MLX5_OPC_DIF_VERIFY_COPY_MKEY ||
	       task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C_MKEY);

	if (mkey) {
		spdk_mlx5_mkey_pool_obj_set_psv(mkey, &task->psv);
	}

	/* UMR is an offset in the addess space, so the start address is 0 */
	translation.iov.iov_base = NULL;
	translation.iov.iov_len = base->nbytes;
	translation.iov_count = 1;
	translation.iovs = &translation.iov;
	translation.size = sizeof(translation);
	translation.rdma.rkey = task->mkeys[0]->mkey;
	translation.rdma.lkey = task->mkeys[0]->mkey;
	translation.rdma.memory_key = mkey;
	translation.offset = 0;

	SPDK_DEBUGLOG(accel_mlx5, "start transfer, task %p, dst_domain_ctx %p, mkey %u\n", task,
		      task->base.dst_domain_ctx, task->mkeys[0]->mkey);
	rc = spdk_memory_domain_transfer_data(base->dst_domain, base->dst_domain_ctx, &translation.iov, 1,
					      dev->dev_ctx->domain, task, &translation.iov, 1, &translation,
					      accel_mlx5_memory_domain_transfer_cpl, task);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to start data transfer, task %p rc %d\n", task, rc);
		accel_mlx5_task_fail(task, rc);
	}
}

static inline void
accel_mlx5_task_complete(struct accel_mlx5_task *task)
{
	struct spdk_accel_sequence *seq = task->base.seq;
	struct spdk_accel_task *next;
	bool driver_seq;

	if (task->needs_data_transfer) {
		/* Passing mkey to the transfer callback tells the implementer that data can be transferred later, when IO completes and
		comes back to the originator. But in some cases we need to transfer data before completing the accel sequence - e.g. to store
		CRC result in local memory. We use needs_epilogue flag to indicate that we need to transfer data before completing the accel sequence. */
		accel_mlx5_memory_domain_transfer(task, task->needs_epilogue ? NULL : task->mkeys[0]);
		return;
	}
	if (task->needs_epilogue) {
		g_accel_mlx5_tasks_ops[task->mlx5_opcode].cont(task);
		return;
	}

	next = spdk_accel_sequence_next_task(&task->base);
	driver_seq = task->driver_seq;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Complete task %p, opc %d\n", task, task->mlx5_opcode);

	g_accel_mlx5_tasks_ops[task->mlx5_opcode].complete(task);

	if (driver_seq) {
		struct spdk_io_channel *ch = task->qp->dev->ch;

		assert(seq);
		if (next) {
			accel_mlx5_execute_sequence(ch, seq);
		} else {
			spdk_accel_sequence_continue(seq);
		}
	}
}

static inline int
accel_mlx5_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;

	if (spdk_unlikely(qp->recovering)) {
		return -EIO;
	}

	return g_accel_mlx5_tasks_ops[task->mlx5_opcode].cont(task);
}

/**
 * Used to set mlx5 opcode for a regular task
 */
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
		mlx5_task->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO;
		break;
	case SPDK_ACCEL_OPC_DECRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO;
		break;
	case SPDK_ACCEL_OPC_CRC32C:
		mlx5_task->inplace = 1;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	case SPDK_ACCEL_OPC_CHECK_CRC32C:
		mlx5_task->inplace = 1;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		mlx5_task->inplace = 0;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	case SPDK_ACCEL_OPC_COPY_CHECK_CRC32C:
		mlx5_task->inplace = 0;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
		mlx5_task->inplace = 0;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_DIF_GENERATE_COPY;
		break;
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
		mlx5_task->inplace = 0;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_DIF_VERIFY_COPY;
		break;
	default:
		SPDK_ERRLOG("wrong opcode %d\n", base_opcode);
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_LAST;
	}
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

static inline void
accel_mlx5_qp_complete_wr(struct accel_mlx5_qp *qp)
{
	/* Delay completing WRs */
	if (!qp->ring_db) {
		qp->ring_db = true;
		STAILQ_INSERT_TAIL(&qp->dev->complete_wr_qps, qp, link);
	}
}

static inline void
accel_mlx5_dev_complete_wrs(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_qp *qp, *tqp;

	STAILQ_FOREACH_SAFE(qp, &dev->complete_wr_qps, link, tqp) {
		STAILQ_REMOVE_HEAD(&dev->complete_wr_qps, link);
		qp->ring_db = false;
		spdk_mlx5_qp_complete_send(qp->qp);
	}
}

static void
accel_mlx5_post_poller_handler(void *ctx)
{
	struct accel_mlx5_io_channel *ch = ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;

	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];

		if (dev->wrs_in_cq) {
			accel_mlx5_dev_complete_wrs(dev);
		}
	}

	ch->poller_handler_registered = false;
}

static inline int
_accel_mlx5_submit_tasks(struct accel_mlx5_io_channel *accel_ch, struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int rc;

	/* We should not receive any tasks if the module was not enabled */
	assert(g_accel_mlx5.enabled);

	dev->stats.opcodes[mlx5_task->mlx5_opcode]++;
	rc = g_accel_mlx5_tasks_ops[mlx5_task->mlx5_opcode].init(mlx5_task);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			return 0;
		}
		SPDK_ERRLOG("Task opc %d init failed, rc %d\n", mlx5_task->base.op_code, rc);
		return rc;
	}

	if (!accel_ch->poller_handler_registered) {
		spdk_thread_register_post_poller_handler(accel_mlx5_post_poller_handler, accel_ch);
		/* Function above may fail to register our handler, in that case we ring doorbells on next polling
		 * cycle. That is less efficient but still works */
		accel_ch->poller_handler_registered = true;
	}

	return g_accel_mlx5_tasks_ops[mlx5_task->mlx5_opcode].process(mlx5_task);
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
	}
	assert(qp->dev == dev);

	return qp;
}

static inline struct accel_mlx5_dev *
accel_mlx5_ch_get_dev(struct accel_mlx5_io_channel *ch)
{
	struct accel_mlx5_dev *dev;

	dev = &ch->devs[ch->dev_idx];
	ch->dev_rr_count++;
	if (ch->dev_rr_count >= ch->dev_rr_threshold) {
		ch->dev_idx++;
		if (ch->dev_idx == ch->num_devs) {
			ch->dev_idx = 0;
		}
		ch->dev_rr_count = 0;
	}
	return dev;
}

static inline int
accel_mlx5_task_assign_qp(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_io_channel *ch)
{
	struct accel_mlx5_dev *dev = NULL;

	dev = accel_mlx5_ch_get_dev(ch);
	assert(dev);

	if (!g_accel_mlx5.attr.qp_per_domain || (!mlx5_task->base.src_domain &&
			!mlx5_task->base.dst_domain)) {
		mlx5_task->qp = &dev->qp;
		return 0;
	}

	/* TODO: find a way to distinguish between SPDK internal and app external domains.
	 * We need to use app domain to find a qp. For now, make an assumption that app domain (src or dst)
	 * depends on the opcode */
	switch (mlx5_task->mlx5_opcode) {
	case ACCEL_MLX5_OPC_CRYPTO:
		if (mlx5_task->base.src_domain) {
			mlx5_task->qp = accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.src_domain);
		} else {
			mlx5_task->qp = &dev->qp;
		}
		break;
	case ACCEL_MLX5_OPC_COPY:
		if (mlx5_task->base.dst_domain) {
			mlx5_task->qp =  accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.dst_domain);
		} else {
			mlx5_task->qp = &dev->qp;
		}
		break;
	case ACCEL_MLX5_OPC_CRC32C:
		if (mlx5_task->base.src_domain) {
			mlx5_task->qp =  accel_mlx5_dev_get_qp_by_domain(dev, mlx5_task->base.dst_domain);
		} else {
			mlx5_task->qp = &dev->qp;
		}
		break;
	default:
		return -ENOTSUP;
	}

	if (spdk_unlikely(!mlx5_task->qp || mlx5_task->qp->recovering)) {
		return -ENODEV;
	}

	return 0;
}

static inline void
accel_mlx5_task_reset(struct accel_mlx5_task *mlx5_task)
{
	mlx5_task->num_completed_reqs = 0;
	mlx5_task->num_submitted_reqs = 0;
	mlx5_task->num_ops = 0;
	mlx5_task->num_reqs = 0;
	mlx5_task->num_mbs = 0;
	mlx5_task->raw = 0;
}

static inline int
accel_mlx5_submit_tasks(struct spdk_io_channel *_ch, struct spdk_accel_task *task)
{
	struct accel_mlx5_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct accel_mlx5_task *mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	int rc;

	assert(g_accel_mlx5.enabled);

	accel_mlx5_task_reset(mlx5_task);
	accel_mlx5_task_init_opcode(mlx5_task);
	rc = accel_mlx5_task_assign_qp(mlx5_task, ch);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("no qp for task %p opc %d; domain src %p, dst %p\n", mlx5_task, mlx5_task->mlx5_opcode,
			    task->src_domain, task->dst_domain);
		return -ENODEV;
	}

	return _accel_mlx5_submit_tasks(ch, mlx5_task);
}

static inline void
accel_mlx5_task_clear_on_recovery(struct accel_mlx5_task *task, struct accel_mlx5_qp *qp)
{
	if (task->qp != qp) {
		return;
	}
	if (task->base.cached_lkey) {
		*task->base.cached_lkey = 0;
	}
	STAILQ_REMOVE(&qp->dev->nomem, task, accel_mlx5_task, link);
	accel_mlx5_task_fail(task, -EIO);
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
	struct accel_mlx5_task *task, *tmp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;

	SPDK_NOTICELOG("Recovering qp %p, core %u\n", qp, spdk_env_get_current_core());
	if (qp->qp) {
		spdk_mlx5_qp_destroy(qp->qp);
		qp->qp = NULL;
	}
	/* There is a good chance that WR failure was caused by invalidated cached mkey.
	 * Clear the cache to avoid new failures.
	 * a task might use a qpair assigned to a memory domain. Such qpair is removed, so we should
	 * fail all tasks referencing the qpair
	 */
	STAILQ_FOREACH_SAFE(task, &dev->nomem, link, tmp) {
		accel_mlx5_task_clear_on_recovery(task, qp);
	}
	if (qp->domain) {
		/* No need to re-create a qp created for a specific domain, it will be created when needed */
		assert(qp != &qp->dev->qp);
		RB_REMOVE(accel_mlx5_qpairs_map, &dev->qpairs_map, qp);
		free(qp);
		return;
	}

	rc = accel_mlx5_create_qp(dev, qp);
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
	if (task->base.op_code == SPDK_ACCEL_OPC_CHECK_CRC32C ||
	    task->base.op_code == SPDK_ACCEL_OPC_COPY_CHECK_CRC32C ||
	    task->base.op_code == SPDK_ACCEL_OPC_DIF_VERIFY_COPY) {
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

static inline struct spdk_mlx5_mkey_pool_obj *
accel_mlx5_find_mkey_by_id(struct accel_mlx5_dev *dev, uint32_t mkey_id)
{
	struct spdk_mlx5_mkey_pool_obj *mkey = NULL;


	if (dev->sig_mkeys) {
		mkey = spdk_mlx5_mkey_pool_find_mkey_by_id(dev->sig_mkeys, mkey_id);
	}

	return mkey;
}

static struct accel_mlx5_task *
accel_mlx5_find_task_by_mkey(struct accel_mlx5_dev *dev, struct spdk_mlx5_mkey_pool_obj *mkey)
{
	struct accel_mlx5_qp *qp;
	struct accel_mlx5_task *task;
	unsigned i;

	RB_FOREACH(qp, accel_mlx5_qpairs_map, &dev->qpairs_map) {
		STAILQ_FOREACH(task, &qp->in_hw, link) {
			for (i = 0; i < task->num_ops; i++) {
				if (task->mkeys[i] == mkey) {
					return task;
				}
			}
		}
	}

	STAILQ_FOREACH(task, &dev->qp.in_hw, link) {
		if (task->qp == &dev->qp) {
			return task;
		}
	}

	return NULL;
}

static void
accel_mlx5_process_sigerr_cpl(struct accel_mlx5_dev *dev, uint32_t mkey_id,
			      union spdk_mlx5_cq_error *err)
{
	struct spdk_mlx5_mkey_pool_obj *mkey = accel_mlx5_find_mkey_by_id(dev, mkey_id);
	struct accel_mlx5_task *task;

	assert(mkey);
	mkey->sig.sigerr_count++;
	mkey->sig.sigerr = true;

	task = accel_mlx5_find_task_by_mkey(dev, mkey);
	if (task != NULL) {
		struct spdk_mlx5_sig_err *sigerr = &err->sigerr;
		struct spdk_dif_error *accel_err = task->base.dif.err;

		if (!(task->base.op_code == SPDK_ACCEL_OPC_DIF_VERIFY ||
		      task->base.op_code == SPDK_ACCEL_OPC_DIF_VERIFY_COPY ||
		      task->base.op_code == SPDK_ACCEL_OPC_DIF_GENERATE ||
		      task->base.op_code == SPDK_ACCEL_OPC_DIF_GENERATE_COPY ||
		      task->base.op_code == SPDK_ACCEL_OPC_DIX_GENERATE ||
		      task->base.op_code == SPDK_ACCEL_OPC_DIX_VERIFY)) {
			SPDK_DEBUGLOG(accel_mlx5, "op_code %d\n", task->base.op_code);
			return;
		}

		accel_err->err_offset = sigerr->offset;

		if (sigerr->syndrome & SPDK_MLX5_SIGERR_CQE_SYNDROME_REFTAG) {
			accel_err->err_type = SPDK_DIF_REFTAG_ERROR;
			accel_err->expected = sigerr->expected & 0xffffffff;
			accel_err->actual = sigerr->actual & 0xffffffff;
		} else if (sigerr->syndrome & SPDK_MLX5_SIGERR_CQE_SYNDROME_APPTAG) {
			accel_err->err_type = SPDK_DIF_APPTAG_ERROR;
			accel_err->expected = (sigerr->expected >> 32) & 0xffff;
			accel_err->actual = (sigerr->actual >> 32) & 0xffff;
		} else if (sigerr->syndrome & SPDK_MLX5_SIGERR_CQE_SYNDROME_GUARD) {
			accel_err->err_type = SPDK_DIF_GUARD_ERROR;
			if (sigerr->sig_type == SPDK_MLX5_SIGERR_CQE_SIG_TYPE_BLOCK) {
				accel_err->expected = sigerr->expected >> 48;
				accel_err->actual = sigerr->actual >> 48;
			} else {
				accel_err->expected = sigerr->expected >> 32;
				accel_err->actual = sigerr->actual >> 32;
			}
		}
	}
}

static inline int64_t
accel_mlx5_poll_cq(struct accel_mlx5_dev *dev)
{
	struct spdk_mlx5_cq_completion wc[ACCEL_MLX5_MAX_WC];
	union spdk_mlx5_cq_error err[ACCEL_MLX5_MAX_WC];
	struct accel_mlx5_task *task;
	struct accel_mlx5_qp *qp;
	int reaped, i, rc;
	uint16_t completed;

	dev->stats.polls++;
	reaped = spdk_mlx5_cq_poll_completions(dev->cq, wc, err, ACCEL_MLX5_MAX_WC);
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
		if (spdk_unlikely(!wc[i].wr_id)) {
			/* Unsignaled completion with error, ignore */
			continue;
		}
		if (spdk_unlikely(wc[i].status == SPDK_MLX5_CQE_SYNDROME_SIGERR)) {
			accel_mlx5_process_sigerr_cpl(dev, wc[i].mkey, &err[i]);
			continue;
		}
		task = (struct accel_mlx5_task *)wc[i].wr_id;
		qp = task->qp;
		assert(task == STAILQ_FIRST(&qp->in_hw) && "submission mismatch");
		STAILQ_REMOVE_HEAD(&qp->in_hw, link);
		assert(task->num_submitted_reqs > task->num_completed_reqs);
		completed = task->num_submitted_reqs - task->num_completed_reqs;
		assert((uint32_t)task->num_completed_reqs + completed <= UINT16_MAX);
		task->num_completed_reqs += completed;
		assert(qp->wrs_submitted >= task->num_wrs);
		qp->wrs_submitted -= task->num_wrs;
		assert(dev->wrs_in_cq > 0);
		dev->wrs_in_cq--;
		SPDK_DEBUGLOG(accel_mlx5, "task %p, remaining %u\n", task,
			      task->num_reqs - task->num_completed_reqs);

		if (spdk_unlikely(wc[i].status)) {
			accel_mlx5_process_error_cpl(&wc[i], task);
			continue;
		}

		SPDK_DEBUGLOG(accel_mlx5, "task %p, remaining %u\n", task,
			      task->num_reqs - task->num_completed_reqs);
		if (task->num_completed_reqs == task->num_reqs) {
			accel_mlx5_task_complete(task);
		} else {
			assert(task->num_submitted_reqs < task->num_reqs);
			assert(task->num_completed_reqs == task->num_submitted_reqs);
			rc = accel_mlx5_task_continue(task);
			if (spdk_unlikely(rc)) {
				if (rc != -ENOMEM) {
					accel_mlx5_task_fail(task, rc);
				}
			}
		}
	}

	return reaped;
}

static inline void
accel_mlx5_resubmit_nomem_tasks(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_task *task, *tmp;
	int rc;

	STAILQ_FOREACH_SAFE(task, &dev->nomem, link, tmp) {
		STAILQ_REMOVE_HEAD(&dev->nomem, link);
		rc = accel_mlx5_task_continue(task);
		if (spdk_unlikely(rc)) {
			if (rc != -ENOMEM) {
				accel_mlx5_task_fail(task, rc);
			}
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

	/* reaped completions may register a post poller handler, that makes no sense in the scope of our own poller */
	ch->poller_handler_registered = true;
	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		if (dev->wrs_in_cq) {
			rc = accel_mlx5_poll_cq(dev);
			if (spdk_unlikely(rc < 0)) {
				SPDK_ERRLOG("Error %"PRId64" on CQ, dev %s\n", rc, dev->dev_ctx->context->device->name);
			} else {
				completions += rc;
			}
		}
		accel_mlx5_dev_complete_wrs(dev);
		accel_mlx5_resubmit_nomem_tasks(dev);
	}
	ch->poller_handler_registered = false;

	return !!completions;
}

static bool
accel_mlx5_supports_opcode(enum spdk_accel_opcode opc)
{
	if (!g_accel_mlx5.enabled) {
		return false;
	}

	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
		return true;
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
		return g_accel_mlx5.crypto_supported;
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_COPY_CRC32C:
	case SPDK_ACCEL_OPC_CHECK_CRC32C:
	case SPDK_ACCEL_OPC_COPY_CHECK_CRC32C:
		return g_accel_mlx5.crc32c_supported;
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

static int
accel_mlx5_create_qp(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp)
{
	struct spdk_mlx5_qp_attr mlx5_qp_attr = {};
	int rc;

	mlx5_qp_attr.cap.max_send_wr = g_accel_mlx5.attr.qp_size;
	mlx5_qp_attr.cap.max_recv_wr = 0;
	mlx5_qp_attr.cap.max_send_sge = ACCEL_MLX5_MAX_INLINE_SGE;
	mlx5_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_INLINE_SGE;

	rc = spdk_mlx5_qp_create(dev->dev_ctx->pd, dev->cq, &mlx5_qp_attr, &qp->qp);
	if (rc) {
		return rc;
	}

	rc = spdk_mlx5_qp_connect_loopback(qp->qp);
	if (rc) {
		spdk_mlx5_qp_destroy(qp->qp);
		qp->qp = NULL;
		return rc;
	}

	STAILQ_INIT(&qp->in_hw);
	qp->dev = dev;
	qp->wrs_max = g_accel_mlx5.attr.qp_size;
	qp->verbs_qp = spdk_mlx5_qp_get_verbs_qp(qp->qp);

	return 0;
}

static void
accel_mlx5_add_stats(struct accel_mlx5_stats *stats, const struct accel_mlx5_stats *to_add)
{
	int i;

	stats->umrs += to_add->umrs;
	stats->crypto_umrs += to_add->crypto_umrs;
	stats->sig_umrs += to_add->sig_umrs;
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

static void
accel_mlx5_dev_destroy_qps(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_qp *qpair, *tmp_qpair;

	if (dev->qp.qp) {
		spdk_mlx5_qp_destroy(dev->qp.qp);
		dev->qp.qp = NULL;
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
		spdk_poller_unregister(&dev->qp.recover_poller);
		if (dev->mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->mkeys);
		}
		if (dev->crypto_mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->crypto_mkeys);
		}
		if (dev->sig_mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->sig_mkeys);
		}
		if (dev->mmap) {
			spdk_rdma_utils_free_mem_map(&dev->mmap);
		}
		spdk_spin_lock(&g_accel_mlx5.lock);
		accel_mlx5_add_stats(&g_accel_mlx5.stats, &dev->stats);
		spdk_spin_unlock(&g_accel_mlx5.lock);
	}
	free(ch->devs);
}

static int
accel_mlx5_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_mlx5_cq_attr cq_attr = {};
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_dev_ctx *dev_ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;
	int rc;

	ch->devs = calloc(g_accel_mlx5.num_ctxs, sizeof(*ch->devs));
	if (!ch->devs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_accel_mlx5.num_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.dev_ctxs[i];
		dev = &ch->devs[i];
		dev->dev_ctx = dev_ctx;
		dev->crypto_multi_block = (dev_ctx->caps.crypto_supported == 1) ?
					  dev_ctx->caps.crypto.multi_block_be_tweak : false;
		dev->ch = spdk_io_channel_from_ctx(ctx_buf);
		RB_INIT(&dev->qpairs_map);

		if (dev_ctx->mkeys) {
			dev->mkeys = spdk_mlx5_mkey_pool_get_ref(dev_ctx->pd, 0);
			if (!dev->mkeys) {
				SPDK_ERRLOG("Failed to get mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}

		if (dev_ctx->crypto_mkeys) {
			dev->crypto_mkeys = spdk_mlx5_mkey_pool_get_ref(dev_ctx->pd, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
			if (!dev->crypto_mkeys) {
				SPDK_ERRLOG("Failed to get crypto mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}
		if (dev_ctx->sig_mkeys) {
			dev->sig_mkeys = spdk_mlx5_mkey_pool_get_ref(dev_ctx->pd, SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
			if (!dev->sig_mkeys) {
				SPDK_ERRLOG("Failed to get sig mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}

		memset(&cq_attr, 0, sizeof(cq_attr));
		cq_attr.cqe_cnt = g_accel_mlx5.attr.qp_size;
		cq_attr.cqe_size = 64;
		cq_attr.cq_context = dev;

		ch->num_devs++;
		rc = spdk_mlx5_cq_create(dev_ctx->pd, &cq_attr, &dev->cq);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 CQ, rc %d\n", rc);
			goto err_out;
		}

		rc = accel_mlx5_create_qp(dev, &dev->qp);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 QP, rc %d\n", rc);
			goto err_out;
		}

		dev->mmap = spdk_rdma_utils_create_mem_map(dev_ctx->pd, NULL,
				IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
		if (!dev->mmap) {
			SPDK_ERRLOG("Failed to create memory map\n");
			rc = -ENOMEM;
			goto err_out;
		}
		dev->crypto_split_blocks = dev->crypto_multi_block ? g_accel_mlx5.attr.crypto_split_blocks : 0;
		dev->wrs_in_cq_max = g_accel_mlx5.attr.qp_size;
		dev->ch = spdk_io_channel_from_ctx(ctx_buf);
		STAILQ_INIT(&dev->nomem);
		STAILQ_INIT(&dev->complete_wr_qps);
	}

	ch->dev_rr_threshold = g_accel_mlx5.attr.dev_rr_threshold;
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
	attr->dev_rr_threshold = ACCEL_MLX5_DEV_RR_THRESHOLD;
	attr->crypto_split_blocks = 0;
	attr->enable_driver = false;
	attr->qp_per_domain = true;
	attr->enable_module = true;
	attr->disable_signature = false;
	attr->disable_crypto = false;
	attr->umr_memory_buffer = false;
}

static void
accel_mlx5_allowed_devs_free(void)
{
	size_t i;

	if (!g_accel_mlx5.allowed_devs) {
		return;
	}

	for (i = 0; i < g_accel_mlx5.allowed_devs_count; i++) {
		free(g_accel_mlx5.allowed_devs[i]);
	}
	free(g_accel_mlx5.attr.allowed_devs);
	free(g_accel_mlx5.allowed_devs);
	g_accel_mlx5.attr.allowed_devs = NULL;
	g_accel_mlx5.allowed_devs = NULL;
	g_accel_mlx5.allowed_devs_count = 0;
}

static int
accel_mlx5_allowed_devs_parse(const char *allowed_devs)
{
	char *str, *tmp, *tok, *sp = NULL;
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
	tok = strtok_r(str, ",", &sp);
	while (tok) {
		g_accel_mlx5.allowed_devs[devs_count] = strdup(tok);
		if (!g_accel_mlx5.allowed_devs[devs_count]) {
			free(str);
			accel_mlx5_allowed_devs_free();
			return -ENOMEM;
		}
		tok = strtok_r(NULL, ",", &sp);
		devs_count++;
		g_accel_mlx5.allowed_devs_count++;
	}

	free(str);

	return 0;
}

int
accel_mlx5_enable(struct accel_mlx5_attr *attr)
{
	int rc;

	if (g_accel_mlx5.attr.allowed_devs) {
		/* If RPC is called several times, free saved config */
		free(g_accel_mlx5.attr.allowed_devs);
		g_accel_mlx5.attr.allowed_devs = NULL;
	}
	if (attr) {
		if (attr->num_requests / spdk_env_get_core_count() < ACCEL_MLX5_MAX_MKEYS_IN_TASK) {
			SPDK_ERRLOG("num requests per core must not be less than %u, current value %u\n",
				    ACCEL_MLX5_MAX_MKEYS_IN_TASK, attr->num_requests / spdk_env_get_core_count());
			return -EINVAL;
		}
		if (attr->num_requests < attr->qp_size) {
			SPDK_ERRLOG("num requests (%u) must be at least qp_size (%u)\n",
				    attr->num_requests, attr->qp_size);
			return -EINVAL;
		}
		if (attr->qp_size < 8) {
			SPDK_ERRLOG("qp_size must be at least 8\n");
			return -EINVAL;
		}
		g_accel_mlx5.attr = *attr;
		g_accel_mlx5.attr.allowed_devs = NULL;

		if (attr->allowed_devs) {
			/* Contains a copy of user's string */
			g_accel_mlx5.attr.allowed_devs = strndup(attr->allowed_devs, ACCEL_MLX5_ALLOWED_DEVS_MAX_LEN);
			if (!g_accel_mlx5.attr.allowed_devs) {
				return -ENOMEM;
			}
			rc = accel_mlx5_allowed_devs_parse(g_accel_mlx5.attr.allowed_devs);
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
	} else {
		accel_mlx5_get_default_attr(&g_accel_mlx5.attr);
	}

	g_accel_mlx5.enabled = attr ? attr->enable_module : true;

	return 0;
}

static void
accel_mlx5_free_resources(void)
{
	struct accel_mlx5_dev_ctx *dev_ctx;
	uint32_t i;

	for (i = 0; i < g_accel_mlx5.num_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.dev_ctxs[i];
		if (dev_ctx->umr_mb_pool) {
			spdk_mlx5_umr_mb_pool_destroy(dev_ctx->umr_mb_pool);
		}
		if (dev_ctx->psv_pool) {
			spdk_mlx5_psv_pool_destroy(dev_ctx->psv_pool);
		}
		spdk_rdma_utils_put_memory_domain(dev_ctx->domain);
		if (dev_ctx->map) {
			spdk_rdma_utils_free_mem_map(&dev_ctx->map);
		}
		if (dev_ctx->sig_mkeys) {
			spdk_mlx5_mkey_pool_destroy(SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE, dev_ctx->pd);
		}
		if (dev_ctx->crypto_mkeys) {
			spdk_mlx5_mkey_pool_destroy(SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO, dev_ctx->pd);
		}
		if (dev_ctx->mkeys) {
			spdk_mlx5_mkey_pool_destroy(0, dev_ctx->pd);
		}
		if (dev_ctx->pd) {
			spdk_rdma_utils_put_pd(dev_ctx->pd);
		}
	}

	free(g_accel_mlx5.dev_ctxs);
	g_accel_mlx5.dev_ctxs = NULL;
	g_accel_mlx5.initialized = false;
}

static void
accel_mlx5_deinit_cb(void *ctx)
{
	struct accel_mlx5_stats *stats = &g_accel_mlx5.stats;

	SPDK_NOTICELOG("mlx5 stats: umrs: crypto %lu, sig %lu, total %lu;\n"
		       "rdma: writes %lu, reads %lu, total %lu, polls %lu, idle_polls %lu, completions %lu\n",
		       stats->crypto_umrs, stats->sig_umrs,
		       stats->crypto_umrs + stats->sig_umrs,
		       stats->rdma_writes, stats->rdma_reads, stats->rdma_writes + stats->rdma_reads,
		       stats->polls, stats->idle_polls, stats->completions);

	spdk_mlx5_umr_implementer_register(false);
	accel_mlx5_free_resources();
	spdk_spin_destroy(&g_accel_mlx5.lock);
	spdk_accel_module_finish();
}

static void
accel_mlx5_deinit(void *ctx)
{
	if (!g_accel_mlx5.enabled) {
		spdk_accel_module_finish();
		return;
	}

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
accel_mlx5_mkeys_create(struct ibv_pd *pd, uint32_t num_mkeys, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool_param pool_param = {};

	pool_param.mkey_count = num_mkeys;
	pool_param.cache_per_thread = num_mkeys * 3 / 4 / spdk_env_get_core_count();
	if (!spdk_u32_is_pow2(pool_param.cache_per_thread)) {
		pool_param.cache_per_thread = spdk_align32pow2(pool_param.cache_per_thread);
		if (pool_param.cache_per_thread) {
			pool_param.cache_per_thread >>= 1;
		}
	}

	pool_param.flags = flags;
	pool_param.max_sges = g_accel_mlx5.attr.umr_memory_buffer ?
			      ACCEL_MLX5_MAX_MB_SGE :
			      ACCEL_MLX5_MAX_INLINE_SGE;

	return spdk_mlx5_mkey_pool_init(&pool_param, pd);
}

static int
accel_mlx5_psvs_create(struct accel_mlx5_dev_ctx *dev_ctx)
{
	struct spdk_mlx5_psv_pool_param params = {
		.psv_count = g_accel_mlx5.attr.num_requests,
		.map = dev_ctx->map,
	};

	dev_ctx->psv_pool = spdk_mlx5_psv_pool_create(&params, dev_ctx->pd);
	if (!dev_ctx->psv_pool) {
		SPDK_ERRLOG("Failed to create PSV memory pool\n");
		return -1;
	}

	return 0;
}

static int
accel_mlx5_umr_mb_create(struct accel_mlx5_dev_ctx *dev_ctx)
{
	struct spdk_mlx5_umr_mb_pool_param params = {
		.mb_count = g_accel_mlx5.attr.num_requests,
		.map = dev_ctx->map,
		.mb_size = SPDK_ALIGN_CEIL(ACCEL_MLX5_MAX_MB_SGE * ACCEL_MLX5_SGE_SIZE, 64),
	};

	dev_ctx->umr_mb_pool = spdk_mlx5_umr_mb_pool_create(&params, dev_ctx->pd);
	if (!dev_ctx->umr_mb_pool) {
		SPDK_ERRLOG("Failed to create UMR Memory Buffer pool\n");
		return -1;
	}

	return 0;
}

static int
accel_mlx5_dev_ctx_init(struct accel_mlx5_dev_ctx *dev_ctx, struct ibv_context *dev,
			struct spdk_mlx5_device_caps *caps)
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
	dev_ctx->caps = *caps;
	dev_ctx->map = spdk_rdma_utils_create_mem_map(dev_ctx->pd, NULL,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!dev_ctx->map) {
		return -ENOMEM;
	}
	dev_ctx->domain = spdk_rdma_utils_get_memory_domain(pd, SPDK_DMA_DEVICE_TYPE_RDMA);
	if (!dev_ctx->domain) {
		return -ENOMEM;
	}

	rc = accel_mlx5_mkeys_create(pd, g_accel_mlx5.attr.num_requests, 0);
	if (rc) {
		SPDK_ERRLOG("Failed to create mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
		return rc;
	}
	dev_ctx->mkeys = true;

	if (g_accel_mlx5.crypto_supported) {
		if (!dev_ctx->caps.crypto.multi_block_be_tweak && g_accel_mlx5.attr.crypto_split_blocks) {
			SPDK_WARNLOG("\"crypto_split_blocks\" is set but dev %s doesn't support multi block crypto\n",
				     dev->device->name);
		}
		rc = accel_mlx5_mkeys_create(pd, g_accel_mlx5.attr.num_requests, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
		if (rc) {
			SPDK_ERRLOG("Failed to create crypto mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
		if (g_accel_mlx5.attr.umr_memory_buffer) {
			rc = accel_mlx5_umr_mb_create(dev_ctx);
			if (rc) {
				SPDK_ERRLOG("Failed to create UMR MB pool, rc %d, dev %s\n", rc, dev->device->name);
				return rc;
			}
		}
		dev_ctx->crypto_mkeys = true;
	}
	if (g_accel_mlx5.crc32c_supported) {
		rc = accel_mlx5_mkeys_create(pd, g_accel_mlx5.attr.num_requests,
					     SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
		if (rc) {
			SPDK_ERRLOG("Failed to create signature mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
		dev_ctx->sig_mkeys = true;
		rc = accel_mlx5_psvs_create(dev_ctx);
		if (rc) {
			SPDK_ERRLOG("Failed to create PSVs pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
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
accel_mlx5_dev_supports_crypto(struct spdk_mlx5_device_caps *caps)
{
	return caps->crypto_supported && !caps->crypto.wrapped_import_method_aes_xts &&
	       (caps->crypto.single_block_le_tweak ||
		caps->crypto.multi_block_le_tweak || caps->crypto.multi_block_be_tweak);
}

static int
accel_mlx5_netdev_ipv4_get(const char *ifname, char *addr, size_t addr_len)
{
	struct ifaddrs *ifa, *ifa_start;
	int rc = -ENOENT;

	if (getifaddrs(&ifa_start) != 0) {
		return -errno;
	}

	for (ifa = ifa_start; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL || ifa->ifa_name == NULL) {
			continue;
		}
		if (strcmp(ifa->ifa_name, ifname) != 0) {
			continue;
		}
		if (ifa->ifa_addr->sa_family != AF_INET) {
			continue;
		}

		rc = spdk_net_get_address_string(ifa->ifa_addr, addr, addr_len);
		if (rc == 0) {
			break;
		}
	}

	freeifaddrs(ifa_start);
	return rc;
}

/* Find the first netdev with IPv4 address */
static int
accel_mlx5_ibv_get_ipv4_addr(struct ibv_context *ctx, char *addr, size_t addr_len)
{
	const char *ibdev_path;
	char path[ACCEL_MLX5_SYSFS_PATH_MAX];
	DIR *dir;
	struct dirent *dent;
	int rc = -ENOENT;

	if (ctx == NULL || addr == NULL || addr_len == 0) {
		return -EINVAL;
	}

	ibdev_path = ctx->device->ibdev_path;
	if (ibdev_path == NULL) {
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "%s/device/net", ibdev_path);
	dir = opendir(path);
	if (dir == NULL) {
		return -errno;
	}

	while ((dent = readdir(dir)) != NULL) {
		if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0) {
			continue;
		}
		if (accel_mlx5_netdev_ipv4_get(dent->d_name, addr, addr_len) == 0) {
			rc = 0;
			break;
		}
	}
	closedir(dir);

	return rc;
}

static int
accel_mlx5_init(void)
{
	struct {
		struct spdk_mlx5_device_caps caps;
		bool has_ip;
		bool selected;
	} *dev_ctx;
	char ip_addr[INET_ADDRSTRLEN];
	struct ibv_context **rdma_devs, *dev;
	int num_devs = 0, rc = 0, i, num_selected_devs, num_devs_with_ip = 0;
	int best_dev = -1;
	int best_dev_stat = 0, dev_stat;
	bool supports_crypto;
	bool find_best_dev = g_accel_mlx5.allowed_devs_count == 0;
	bool filter_by_ip = true;
	const char **dev_names = NULL;

	if (!g_accel_mlx5.enabled) {
		return 0;
	}

	spdk_spin_init(&g_accel_mlx5.lock);
	rdma_devs = accel_mlx5_get_devices(&num_devs);
	if (!rdma_devs || !num_devs) {
		SPDK_ERRLOG("No Nvidia RDMA devices found");
		return -ENODEV;
	}
	dev_ctx = calloc(num_devs, sizeof(*dev_ctx));
	if (!dev_ctx) {
		rc = -ENOMEM;
		goto cleanup;
	}

	dev_names = calloc(num_devs, sizeof(*dev_names));
	if (!dev_names) {
		rc = -ENOMEM;
		goto cleanup;
	}

	g_accel_mlx5.crypto_supported = true;
	g_accel_mlx5.crc32c_supported = true;
	g_accel_mlx5.num_ctxs = 0;

	/* Iterate devices. We support an offload if all devices support it */
	for (i = 0; i < num_devs; i++) {
		dev = rdma_devs[i];

		rc = spdk_mlx5_device_query_caps(dev, &dev_ctx[i].caps);
		if (rc) {
			SPDK_ERRLOG("Failed to get crypto caps, dev %s\n", dev->device->name);
			goto cleanup;
		}
		dev_ctx[i].has_ip = (accel_mlx5_ibv_get_ipv4_addr(dev, ip_addr, sizeof(ip_addr)) == 0);
		num_devs_with_ip += !!dev_ctx[i].has_ip;

		supports_crypto = accel_mlx5_dev_supports_crypto(&dev_ctx[i].caps);
		if (!supports_crypto) {
			SPDK_DEBUGLOG(accel_mlx5, "Disable crypto support because dev %s doesn't support it\n",
				      rdma_devs[i]->device->name);
			g_accel_mlx5.crypto_supported = false;
		}
		if (!dev_ctx[i].caps.crc32c_supported) {
			SPDK_DEBUGLOG(accel_mlx5, "Disable crc32c support because dev %s doesn't support it\n",
				      rdma_devs[i]->device->name);
			g_accel_mlx5.crc32c_supported = false;
		}
		if (find_best_dev) {
			/* Find device which supports max number of offloads */
			dev_stat = (int)supports_crypto + (int)dev_ctx[i].caps.crc32c_supported;
			if (dev_stat > best_dev_stat) {
				best_dev_stat = dev_stat;
				best_dev = i;
			} else if (dev_stat == best_dev_stat && best_dev != -1 && !dev_ctx[best_dev].has_ip &&
				   dev_ctx[i].has_ip) {
				/* Prioritize device with IP address */
				best_dev = i;
			}
		}
	}

	/* User didn't specify devices to use, try to select the best one */
	if (find_best_dev) {
		if (best_dev == -1) {
			best_dev = 0;
		}

		num_selected_devs = 0;
		if (num_devs_with_ip == 0 || !dev_ctx[best_dev].has_ip) {
			filter_by_ip = false;
		}

		g_accel_mlx5.crypto_supported = accel_mlx5_dev_supports_crypto(&dev_ctx[best_dev].caps);
		g_accel_mlx5.crc32c_supported = dev_ctx[best_dev].caps.crc32c_supported;
		for (i = 0; i < num_devs; i++) {
			if (g_accel_mlx5.crypto_supported &&
			    !accel_mlx5_dev_supports_crypto(&dev_ctx[i].caps)) {
				continue;
			}
			if (g_accel_mlx5.crc32c_supported &&
			    !dev_ctx[i].caps.crc32c_supported) {
				continue;
			}
			if (filter_by_ip && !dev_ctx[i].has_ip) {
				SPDK_DEBUGLOG(accel_mlx5, "dev %s doesn't have IP, skipping\n", rdma_devs[i]->device->name);
				continue;
			}

			SPDK_NOTICELOG("Select dev %s, crypto %d, crc32c %d\n", rdma_devs[i]->device->name,
				       g_accel_mlx5.crypto_supported, g_accel_mlx5.crc32c_supported);
			dev_ctx[i].selected = true;
			dev_names[num_selected_devs] = rdma_devs[i]->device->name;
			num_selected_devs++;
		}

		if (g_accel_mlx5.crypto_supported) {
			spdk_mlx5_crypto_devs_allow(dev_names, num_selected_devs);
		}
	} else {
		SPDK_NOTICELOG("Found %d devices, crypto %d\n", num_devs, g_accel_mlx5.crypto_supported);
		for (i = 0; i < num_devs; i++) {
			dev_ctx[i].selected = true;
		}
		num_selected_devs = num_devs;
	}

	g_accel_mlx5.dev_ctxs = calloc(num_selected_devs, sizeof(*g_accel_mlx5.dev_ctxs));
	if (!g_accel_mlx5.dev_ctxs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	for (i = 0; i < num_devs; i++) {
		if (!dev_ctx[i].selected) {
			continue;
		}

		rc = accel_mlx5_dev_ctx_init(&g_accel_mlx5.dev_ctxs[g_accel_mlx5.num_ctxs++],
					     rdma_devs[i], &dev_ctx[i].caps);
		if (rc) {
			goto cleanup;
		}
	}

	rc = spdk_memory_domain_update_notification_subscribe(&g_accel_mlx5,
			accel_mlx5_domain_notification);
	if (rc) {
		SPDK_WARNLOG("Failed to subscribe on memory domain updates (rc %d), ignoring\n", rc);
		rc = 0;
	}

	SPDK_NOTICELOG("Accel framework mlx5 initialized, found %d devices.\n", num_selected_devs);
	spdk_io_device_register(&g_accel_mlx5, accel_mlx5_create_cb, accel_mlx5_destroy_cb,
				sizeof(struct accel_mlx5_io_channel), "accel_mlx5");
	g_accel_mlx5.initialized = true;
	free(rdma_devs);
	free(dev_names);
	free(dev_ctx);

	if (g_accel_mlx5.attr.enable_driver) {
		const char *driver_name = spdk_accel_get_driver_name();

		SPDK_NOTICELOG("Enabling mlx5 platform driver\n");
		/* The app can be loaded from json config which could have already set the driver.
		 * Check if mlx5 driver is already set to avoid errors
		 */
		if (driver_name == NULL || strcmp(driver_name, g_accel_mlx5_driver.name) != 0) {
			spdk_accel_set_driver(g_accel_mlx5_driver.name);
		}
		spdk_mlx5_umr_implementer_register(true);
	}

	return 0;

cleanup:
	free(rdma_devs);
	free(dev_names);
	free(dev_ctx);
	accel_mlx5_free_resources();
	spdk_spin_destroy(&g_accel_mlx5.lock);

	return rc;
}

static void
accel_mlx5_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "mlx5_scan_accel_module");
	spdk_json_write_named_object_begin(w, "params");
	if (g_accel_mlx5.enabled) {
		spdk_json_write_named_uint16(w, "qp_size", g_accel_mlx5.attr.qp_size);
		spdk_json_write_named_uint16(w, "cq_size", g_accel_mlx5.attr.cq_size);
		spdk_json_write_named_uint32(w, "num_requests", g_accel_mlx5.attr.num_requests);
		spdk_json_write_named_bool(w, "enable_driver", g_accel_mlx5.attr.enable_driver);
		spdk_json_write_named_uint32(w, "crypto_split_blocks", g_accel_mlx5.attr.crypto_split_blocks);
		spdk_json_write_named_uint16(w, "dev_rr_threshold", g_accel_mlx5.attr.dev_rr_threshold);
		if (g_accel_mlx5.attr.allowed_devs) {
			spdk_json_write_named_string(w, "allowed_devs", g_accel_mlx5.attr.allowed_devs);
		}
		spdk_json_write_named_bool(w, "qp_per_domain", g_accel_mlx5.attr.qp_per_domain);
		spdk_json_write_named_bool(w, "disable_signature", g_accel_mlx5.attr.disable_signature);
		spdk_json_write_named_bool(w, "disable_crypto", g_accel_mlx5.attr.disable_crypto);
		spdk_json_write_named_bool(w, "umr_memory_buffer", g_accel_mlx5.attr.umr_memory_buffer);
	} else {
		spdk_json_write_named_bool(w, "enable_module", g_accel_mlx5.enabled);
	}
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
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

	spdk_json_write_named_object_begin(w, "umrs");
	spdk_json_write_named_uint64(w, "crypto_umrs", stats->crypto_umrs);
	spdk_json_write_named_uint64(w, "sig_umrs", stats->sig_umrs);
	spdk_json_write_named_uint64(w, "umrs", stats->umrs);
	spdk_json_write_named_uint64(w, "total",
				     stats->crypto_umrs + stats->sig_umrs + stats->umrs);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "rdma");
	spdk_json_write_named_uint64(w, "read", stats->rdma_reads);
	spdk_json_write_named_uint64(w, "write", stats->rdma_writes);
	spdk_json_write_named_uint64(w, "total", stats->rdma_reads + stats->rdma_writes);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "polling");
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
	spdk_json_write_named_uint64(w, "crypto_mkey", stats->opcodes[ACCEL_MLX5_OPC_CRYPTO_MKEY]);
	spdk_json_write_named_uint64(w, "crc32c", stats->opcodes[ACCEL_MLX5_OPC_CRC32C]);
	spdk_json_write_named_uint64(w, "mkey", stats->opcodes[ACCEL_MLX5_OPC_MKEY]);
	spdk_json_write_named_uint64(w, "crypto_mkey_ext_qp",
				     stats->opcodes[ACCEL_MLX5_OPC_CRYPTO_MKEY_EXT_QP]);
	spdk_json_write_named_uint64(w, "dif_generate_copy",
				     stats->opcodes[ACCEL_MLX5_OPC_DIF_GENERATE_COPY]);
	spdk_json_write_named_uint64(w, "dif_verify_copy", stats->opcodes[ACCEL_MLX5_OPC_DIF_VERIFY_COPY]);
	spdk_json_write_named_uint64(w, "dif_generate_copy_mkey",
				     stats->opcodes[ACCEL_MLX5_OPC_DIF_GENERATE_COPY_MKEY]);
	spdk_json_write_named_uint64(w, "dif_verify_copy_mkey",
				     stats->opcodes[ACCEL_MLX5_OPC_DIF_VERIFY_COPY_MKEY]);
	spdk_json_write_named_uint64(w, "crc32c_mkey", stats->opcodes[ACCEL_MLX5_OPC_CRC32C_MKEY]);
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
			accel_mlx5_dump_stats_json(ctx->w, dev->dev_ctx->context->device->name, &dev->stats);
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

	accel_mlx5_dump_stats_json(ctx->w, "total", &ctx->total);

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
	if (!g_accel_mlx5.initialized) {
		return -ENODEV;
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

static bool
accel_mlx5_crypto_supports_tweak_mode(enum spdk_accel_crypto_tweak_mode tweak_mode)
{
	if (!g_accel_mlx5.crypto_supported) {
		return false;
	}

	if (tweak_mode == SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA) {
		return true;
	}

	return false;
}

static int
accel_mlx5_get_memory_domains(struct spdk_memory_domain **domains, int array_size)
{
	int i, size;

	if (!domains || !array_size) {
		return (int)g_accel_mlx5.num_ctxs;
	}

	size = spdk_min(array_size, (int)g_accel_mlx5.num_ctxs);

	for (i = 0; i < size; i++) {
		domains[i] = g_accel_mlx5.dev_ctxs[i].domain;
	}

	return (int)g_accel_mlx5.num_ctxs;
}

static inline struct accel_mlx5_dev *
accel_mlx5_ch_get_dev_by_pd(struct accel_mlx5_io_channel *accel_ch, struct ibv_pd *pd)
{
	uint32_t i;

	for (i = 0; i < accel_ch->num_devs; i++) {
		if (accel_ch->devs[i].dev_ctx->pd == pd) {
			return &accel_ch->devs[i];
		}
	}

	return NULL;
}

static inline int
accel_mlx5_task_assign_qp_by_pd(struct accel_mlx5_task *task,
				struct accel_mlx5_io_channel *accel_ch,
				struct ibv_pd *pd)
{
	struct accel_mlx5_dev *dev;

	dev = accel_mlx5_ch_get_dev_by_pd(accel_ch, pd);
	if (spdk_unlikely(!dev)) {
		SPDK_ERRLOG("No dev for PD %p dev %s\n", pd, pd->context->device->name);
		return -ENODEV;
	}

	if (spdk_unlikely(!dev)) {
		return -ENODEV;
	}
	if (!g_accel_mlx5.attr.qp_per_domain || !task->base.dst_domain) {
		task->qp = accel_mlx5_dev_get_qp_by_domain(dev, task->base.src_domain);
	} else {
		task->qp = &dev->qp;
	}
	if (spdk_unlikely(!task->qp || task->qp->recovering)) {
		return -ENODEV;
	}

	return 0;
}

static inline int
accel_mlx5_task_assign_qp_by_domain_pd(struct accel_mlx5_task *task,
				       struct accel_mlx5_io_channel *accel_ch, struct spdk_memory_domain *domain)
{
	struct spdk_memory_domain_rdma_ctx *domain_ctx;
	struct ibv_pd *domain_pd;
	size_t ctx_size;

	domain_ctx = spdk_memory_domain_get_user_context(domain, &ctx_size);
	if (spdk_unlikely(!domain_ctx || domain_ctx->size != ctx_size)) {
		SPDK_ERRLOG("no domain context or wrong size, ctx ptr %p, size %zu\n", domain_ctx, ctx_size);
		return -ENOTSUP;
	}
	domain_pd = domain_ctx->ibv_pd;
	if (spdk_unlikely(!domain_pd)) {
		SPDK_ERRLOG("no destination domain PD, task %p", task);
		return -ENOTSUP;
	}

	return accel_mlx5_task_assign_qp_by_pd(task, accel_ch, domain_pd);
}


static inline int
accel_mlx5_task_merge_copy_crypto(struct accel_mlx5_task *crypto, struct accel_mlx5_task *copy,
				  struct accel_mlx5_io_channel *ch,
				  struct spdk_memory_domain *domain_override, void *domain_ctx_override,
				  enum spdk_mlx5_encryption_order enc_order)
{
	struct spdk_memory_domain_rdma_ctx *domain_ctx;
	struct spdk_accel_task *crypto_base = &crypto->base;
	struct spdk_accel_task *copy_base = &copy->base;
	size_t ctx_size;
	int rc;

	domain_ctx = spdk_memory_domain_get_user_context(domain_override, &ctx_size);
	if (spdk_unlikely(!domain_ctx || domain_ctx->size != ctx_size)) {
		SPDK_ERRLOG("no domain context or wrong size, ctx ptr %p, size %zu\n", domain_ctx, ctx_size);
		return -ENOTSUP;
	}
	if (spdk_unlikely(!domain_ctx->ibv_pd)) {
		SPDK_ERRLOG("no destination domain PD, task %p", crypto);
		return -ENOTSUP;
	}

	rc = accel_mlx5_task_assign_qp_by_pd(crypto, ch, domain_ctx->ibv_pd);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update crypto task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and %s (%p)\n", copy_base,
		      spdk_accel_get_opcode_name(crypto_base->op_code), crypto_base);
	crypto_base->dst_domain = domain_override;
	crypto_base->dst_domain_ctx = domain_ctx_override;
	accel_mlx5_task_reset(crypto);
	if (domain_ctx->qp) {
		crypto->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO_MKEY_EXT_QP;
		crypto->crypto_external_qp.ext_qp = domain_ctx->qp;
	} else {
		crypto->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO_MKEY;
	}
	crypto->enc_order = enc_order;
	crypto->needs_data_transfer = 1;
	crypto->inplace = 1;
	spdk_accel_task_complete(copy_base, 0);

	return 0;
}

static inline int
accel_mlx5_task_merge_copy_crc32c(struct accel_mlx5_task *crc32c, struct accel_mlx5_task *copy,
				  struct accel_mlx5_io_channel *ch,
				  struct spdk_memory_domain *domain_override, void *domain_ctx_override)
{
	struct spdk_memory_domain_rdma_ctx *domain_ctx;
	struct spdk_accel_task *crc32c_base = &crc32c->base;
	struct spdk_accel_task *copy_base = &copy->base;
	size_t ctx_size;
	int rc;

	domain_ctx = spdk_memory_domain_get_user_context(domain_override, &ctx_size);
	if (spdk_unlikely(!domain_ctx || domain_ctx->size != ctx_size)) {
		SPDK_ERRLOG("no domain context or wrong size, ctx ptr %p, size %zu\n", domain_ctx, ctx_size);
		return -ENOTSUP;
	}
	if (spdk_unlikely(!domain_ctx->ibv_pd)) {
		SPDK_ERRLOG("no destination domain PD, task %p\n", crc32c);
		return -ENOTSUP;
	}

	rc = accel_mlx5_task_assign_qp_by_pd(crc32c, ch, domain_ctx->ibv_pd);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update crypto task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and crc32c (%p)\n", copy_base, crc32c_base);
	crc32c_base->dst_domain = domain_override;
	crc32c_base->dst_domain_ctx = domain_ctx_override;
	accel_mlx5_task_reset(crc32c);
	crc32c->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C_MKEY;
	crc32c->needs_data_transfer = 1;
	crc32c->needs_epilogue = 1;
	crc32c->inplace = 1;
	spdk_accel_task_complete(copy_base, 0);

	return 0;
}

static inline int
accel_mlx5_task_merge_copy_dif_generate_copy(struct spdk_accel_task *dif_base,
		struct spdk_accel_task *copy_base, struct accel_mlx5_io_channel *ch,
		struct spdk_memory_domain *domain_override, void *domain_ctx_override,
		bool md_in_umr)
{
	struct accel_mlx5_task *dif = SPDK_CONTAINEROF(dif_base, struct accel_mlx5_task, base);
	int rc;

	rc = accel_mlx5_task_assign_qp_by_domain_pd(dif, ch, domain_override);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update DIF generate copy task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and dif generate copy task\n", copy_base);

	dif_base->dst_domain = domain_override;
	dif_base->dst_domain_ctx = domain_ctx_override;
	accel_mlx5_task_reset(dif);
	dif->mlx5_opcode = ACCEL_MLX5_OPC_DIF_GENERATE_COPY_MKEY;
	dif->needs_data_transfer = 1;
	dif->inplace = 1;
	dif->md_in_umr = md_in_umr;
	spdk_accel_task_complete(copy_base, 0);

	return 0;
}

static inline int
accel_mlx5_task_merge_copy_dif_verify_copy(struct spdk_accel_task *dif_base,
		struct spdk_accel_task *copy_base, struct accel_mlx5_io_channel *ch,
		struct spdk_memory_domain *domain_override, void *domain_ctx_override,
		bool md_in_umr)
{
	struct accel_mlx5_task *dif = SPDK_CONTAINEROF(dif_base, struct accel_mlx5_task, base);
	int rc;

	rc = accel_mlx5_task_assign_qp_by_domain_pd(dif, ch, domain_override);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update DIF verify copy task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and dif verify copy task\n", copy_base);

	if (!md_in_umr) {
		dif_base->src_domain = dif_base->dst_domain;
		dif_base->src_domain_ctx = dif_base->dst_domain_ctx;
	}
	dif_base->dst_domain = domain_override;
	dif_base->dst_domain_ctx = domain_ctx_override;
	accel_mlx5_task_reset(dif);
	dif->mlx5_opcode = ACCEL_MLX5_OPC_DIF_VERIFY_COPY_MKEY;
	dif->needs_data_transfer = 1;
	dif->inplace = 1;
	dif->md_in_umr = md_in_umr;
	spdk_accel_task_complete(copy_base, 0);

	return 0;
}

static inline int
accel_mlx5_task_merge_copy_write_copy(struct accel_mlx5_task *write_copy,
				      struct accel_mlx5_task *copy, struct accel_mlx5_io_channel *ch,
				      struct spdk_memory_domain *domain_override, void *domain_ctx_override)
{
	struct spdk_accel_task *write_copy_base = &write_copy->base;
	struct spdk_accel_task *copy_base = &copy->base;
	int rc;

	rc = accel_mlx5_task_assign_qp_by_domain_pd(write_copy, ch, domain_override);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update write copy task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and write copy task\n", copy_base);

	write_copy_base->dst_domain = domain_override;
	write_copy_base->dst_domain_ctx = domain_ctx_override;
	accel_mlx5_task_reset(write_copy);
	write_copy->mlx5_opcode = ACCEL_MLX5_OPC_MKEY;
	write_copy->needs_data_transfer = 1;
	write_copy->inplace = 1;
	spdk_accel_task_complete(copy_base, 0);

	return 0;
}

static inline int
accel_mlx5_task_merge_copy_read_copy(struct accel_mlx5_task *read_copy,
				     struct accel_mlx5_task *copy, struct accel_mlx5_io_channel *ch,
				     struct spdk_memory_domain *domain_override, void *domain_ctx_override)
{
	struct spdk_accel_task *read_copy_base = &read_copy->base;
	struct spdk_accel_task *copy_base = &copy->base;
	int rc;

	rc = accel_mlx5_task_assign_qp_by_domain_pd(read_copy, ch, domain_override);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update read copy task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and read copy task\n", copy_base);

	read_copy_base->dst_domain = domain_override;
	read_copy_base->dst_domain_ctx = domain_ctx_override;
	accel_mlx5_task_reset(read_copy);
	read_copy->mlx5_opcode = ACCEL_MLX5_OPC_MKEY;
	read_copy->needs_data_transfer = 1;
	read_copy->inplace = 1;
	spdk_accel_task_complete(copy_base, 0);

	return 0;
}

static inline int
accel_mlx5_task_merge_copy_mkey(struct accel_mlx5_task *task, struct accel_mlx5_io_channel *ch)
{
	int rc;

	SPDK_DEBUGLOG(accel_mlx5, "MKEY task %p\n", task);
	rc = accel_mlx5_task_assign_qp_by_domain_pd(task, ch, task->base.dst_domain);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	accel_mlx5_task_reset(task);
	task->mlx5_opcode = ACCEL_MLX5_OPC_MKEY;
	task->needs_data_transfer = 1;
	task->inplace = 1;
	return 0;
}

static inline int
accel_mlx5_task_merge_crc32c_mkey(struct accel_mlx5_task *crc32c, struct accel_mlx5_io_channel *ch)
{
	struct spdk_memory_domain_rdma_ctx *domain_ctx;
	size_t ctx_size;
	int rc;

	domain_ctx = spdk_memory_domain_get_user_context(crc32c->base.src_domain, &ctx_size);
	if (spdk_unlikely(!domain_ctx || domain_ctx->size != ctx_size)) {
		SPDK_ERRLOG("no domain context or wrong size, ctx ptr %p, size %zu\n", domain_ctx, ctx_size);
		return -ENOTSUP;
	}
	if (spdk_unlikely(!domain_ctx->ibv_pd)) {
		SPDK_ERRLOG("no destination domain PD, task %p\n", crc32c);
		return -ENOTSUP;
	}

	rc = accel_mlx5_task_assign_qp_by_pd(crc32c, ch, domain_ctx->ibv_pd);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	/* Update crypto task memory domain, complete copy task */
	SPDK_DEBUGLOG(accel_mlx5, "Merge crc32c task (%p)\n", crc32c);
	accel_mlx5_task_reset(crc32c);
	crc32c->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C_MKEY;
	crc32c->needs_data_transfer = 1;
	crc32c->needs_epilogue = 1;
	crc32c->inplace = 1;
	crc32c->base.dst_domain = crc32c->base.src_domain;
	crc32c->base.dst_domain_ctx = crc32c->base.src_domain_ctx;
	crc32c->base.src_domain = NULL;
	crc32c->base.src_domain_ctx = NULL;

	return 0;
}

static inline int
accel_mlx5_driver_examine_sequence(struct spdk_accel_sequence *seq,
				   struct accel_mlx5_io_channel *accel_ch)
{
	struct spdk_accel_task *first_base = spdk_accel_sequence_first_task(seq);
	struct accel_mlx5_task *first = SPDK_CONTAINEROF(first_base, struct accel_mlx5_task, base);
	struct spdk_accel_task *next_base = TAILQ_NEXT(first_base, seq_link);
	int rc;

	SPDK_DEBUGLOG(accel_mlx5, "first %p, opc %d; next %p, opc %d\n", first_base, first_base->op_code,
		      next_base,  next_base ? next_base->op_code : -1);
	if (!next_base) {
		switch (first_base->op_code) {
		case SPDK_ACCEL_OPC_COPY:
			if (first_base->dst_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->dst_domain) ==
			    SPDK_DMA_DEVICE_TYPE_RDMA &&
			    accel_mlx5_compare_iovs(first_base->d.iovs, first_base->s.iovs, first_base->s.iovcnt)) {
				return accel_mlx5_task_merge_copy_mkey(first, accel_ch);
			}
			break;
		case SPDK_ACCEL_OPC_CRC32C:
			if (first_base->src_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->src_domain) ==
			    SPDK_DMA_DEVICE_TYPE_RDMA) {
				return accel_mlx5_task_merge_crc32c_mkey(first, accel_ch);
			}
			break;
		default:
			break;
		}
	}

	switch (first_base->op_code) {
	case SPDK_ACCEL_OPC_ENCRYPT:
		assert(g_accel_mlx5.crypto_supported);

		if (next_base && next_base->op_code == SPDK_ACCEL_OPC_COPY &&
		    next_base->dst_domain && spdk_memory_domain_get_dma_device_type(next_base->dst_domain) ==
		    SPDK_DMA_DEVICE_TYPE_RDMA && TAILQ_NEXT(next_base, seq_link) == NULL) {
			return accel_mlx5_task_merge_copy_crypto(first, SPDK_CONTAINEROF(next_base, struct accel_mlx5_task,
					base), accel_ch,
					next_base->dst_domain, next_base->dst_domain_ctx, SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE);
		}
		break;
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
		if (next_base && next_base->op_code == SPDK_ACCEL_OPC_COPY &&
		    next_base->dst_domain && spdk_memory_domain_get_dma_device_type(next_base->dst_domain) ==
		    SPDK_DMA_DEVICE_TYPE_RDMA && TAILQ_NEXT(next_base, seq_link) == NULL) {
			return accel_mlx5_task_merge_copy_dif_generate_copy(first_base, next_base,
					accel_ch, next_base->dst_domain, next_base->dst_domain_ctx,
					false);
		}
		break;
	case SPDK_ACCEL_OPC_COPY:
		if (!next_base || TAILQ_NEXT(next_base, seq_link) != NULL) {
			break;
		}
		switch (next_base->op_code) {
		case SPDK_ACCEL_OPC_DECRYPT:
			if (first_base->dst_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->dst_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				return accel_mlx5_task_merge_copy_crypto(SPDK_CONTAINEROF(next_base, struct accel_mlx5_task, base),
						first, accel_ch, first_base->dst_domain, first_base->dst_domain_ctx,
						SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE);
			}
			break;
		case SPDK_ACCEL_OPC_ENCRYPT:
			if (first_base->src_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->src_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				rc = accel_mlx5_task_merge_copy_crypto(SPDK_CONTAINEROF(next_base, struct accel_mlx5_task, base),
								       first, accel_ch, first_base->src_domain, first_base->src_domain_ctx,
								       SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY);
				if (spdk_unlikely(rc)) {
					return rc;
				}
				next_base->s.iovs = next_base->d.iovs;
				next_base->s.iovcnt = next_base->d.iovcnt;
				return 0;
			}
			break;
		case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
			if (first_base->src_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->src_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				rc = accel_mlx5_task_merge_copy_dif_generate_copy(next_base, first_base,
						accel_ch, first_base->src_domain, first_base->src_domain_ctx,
						true);
				if (spdk_unlikely(rc)) {
					return rc;
				}
				next_base->s.iovs = next_base->d.iovs;
				next_base->s.iovcnt = next_base->d.iovcnt;
				return 0;
			}
			break;
		case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
			if (first_base->dst_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->dst_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				rc = accel_mlx5_task_merge_copy_dif_verify_copy(next_base, first_base,
						accel_ch, first_base->dst_domain, first_base->dst_domain_ctx,
						false);
				if (spdk_unlikely(rc)) {
					return rc;
				}
				next_base->s.iovs = next_base->d.iovs;
				next_base->s.iovcnt = next_base->d.iovcnt;
				return 0;
			}
			break;
		case SPDK_ACCEL_OPC_COPY:
			if (first_base->src_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->src_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				rc = accel_mlx5_task_merge_copy_write_copy(
					     SPDK_CONTAINEROF(next_base, struct accel_mlx5_task, base),
					     first, accel_ch, first_base->src_domain, first_base->src_domain_ctx);
				if (spdk_unlikely(rc)) {
					return rc;
				}
				next_base->s.iovs = next_base->d.iovs;
				next_base->s.iovcnt = next_base->d.iovcnt;
				return 0;
			} else if (next_base->src_domain &&
				   spdk_memory_domain_get_dma_device_type(next_base->src_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				rc = accel_mlx5_task_merge_copy_read_copy(first,
						SPDK_CONTAINEROF(next_base, struct accel_mlx5_task, base),
						accel_ch, next_base->src_domain, next_base->src_domain_ctx);
				if (spdk_unlikely(rc)) {
					return rc;
				}
				first_base->d.iovs = first_base->s.iovs;
				first_base->d.iovcnt = first_base->s.iovcnt;
				return 0;
			}
			break;
		case SPDK_ACCEL_OPC_CRC32C:
		case SPDK_ACCEL_OPC_CHECK_CRC32C:
			/* The flow is symmetric for remote->local and local->remote cases */
			if (first_base->src_domain &&
			    spdk_memory_domain_get_dma_device_type(first_base->src_domain) == SPDK_DMA_DEVICE_TYPE_RDMA) {
				return accel_mlx5_task_merge_copy_crc32c(SPDK_CONTAINEROF(next_base, struct accel_mlx5_task, base),
						first, accel_ch, first_base->src_domain,
						first_base->src_domain_ctx);
			}
			break;
		default:
			break;
		}
		break;
	case SPDK_ACCEL_OPC_DECRYPT:
		assert(g_accel_mlx5.crypto_supported);

		if (next_base && next_base->op_code == SPDK_ACCEL_OPC_COPY &&
		    next_base->src_domain && spdk_memory_domain_get_dma_device_type(next_base->src_domain) ==
		    SPDK_DMA_DEVICE_TYPE_RDMA && TAILQ_NEXT(next_base, seq_link) == NULL) {
			return accel_mlx5_task_merge_copy_crypto(first, SPDK_CONTAINEROF(next_base, struct accel_mlx5_task,
					base), accel_ch, next_base->src_domain, next_base->src_domain_ctx,
					SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY);
		}
		break;
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
		if (next_base && next_base->op_code == SPDK_ACCEL_OPC_COPY &&
		    next_base->src_domain && spdk_memory_domain_get_dma_device_type(next_base->src_domain) ==
		    SPDK_DMA_DEVICE_TYPE_RDMA && TAILQ_NEXT(next_base, seq_link) == NULL) {
			rc = accel_mlx5_task_merge_copy_dif_verify_copy(first_base, next_base,
					accel_ch, next_base->src_domain, next_base->src_domain_ctx,
					true);
			if (spdk_unlikely(rc)) {
				return rc;
			}
			first_base->d.iovs = first_base->s.iovs;
			first_base->d.iovcnt = first_base->s.iovcnt;
			return 0;
		}
		break;
	default:
		break;
	}

	/* Nothing special, execute in regular way */
	accel_mlx5_task_reset(first);
	accel_mlx5_task_init_opcode(first);
	return accel_mlx5_task_assign_qp(first, accel_ch);
}

static inline int
accel_mlx5_execute_sequence(struct spdk_io_channel *_ch, struct spdk_accel_sequence *seq)
{
	struct accel_mlx5_io_channel *accel_ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_accel_task *task;
	struct accel_mlx5_task *mlx5_task;
	int rc;

	assert(g_accel_mlx5.enabled);

	rc = accel_mlx5_driver_examine_sequence(seq, accel_ch);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Driver failed to process seq %p on ch %p, rc %d\n", seq, _ch, rc);
		return rc;
	}
	task = spdk_accel_sequence_first_task(seq);
	assert(task);
	mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	mlx5_task->driver_seq = 1;

	SPDK_DEBUGLOG(accel_mlx5, "driver starts seq %p, ch %p, task %p\n", seq, accel_ch, task);

	return _accel_mlx5_submit_tasks(accel_ch, mlx5_task);
}

static int
accel_mlx5_get_operation_info(enum spdk_accel_opcode opcode,
			      const struct spdk_accel_operation_exec_ctx *ctx,
			      struct spdk_accel_opcode_info *info)
{
	bool use_data_transfer;

	if (!accel_mlx5_supports_opcode(opcode)) {
		return -ENOTSUP;
	}

	/*
	 * If the user doesn't pass us ctx->use_data_transfer, we assume it's true, since that's
	 * more restrictive.
	 */
	use_data_transfer = SPDK_GET_FIELD(ctx, use_data_transfer, true);
	if (!use_data_transfer) {
		/* There's no limit if we do the RDMA ourselves */
		info->max_segments = UINT32_MAX;
		switch (opcode) {
		case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
		case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
			/* Except for DIF which is limited to ACCEL_MLX5_MAX_INLINE_SGE */
			info->max_segments = ACCEL_MLX5_MAX_INLINE_SGE;
			break;
		default:
			break;
		}
	} else {
		info->max_segments = ACCEL_MLX5_MAX_INLINE_SGE;
		switch (opcode) {
		case SPDK_ACCEL_OPC_COPY:
			if (g_accel_mlx5.attr.enable_driver &&
			    g_accel_mlx5.attr.umr_memory_buffer) {
				info->max_segments = ACCEL_MLX5_MAX_MB_SGE;
			}
			break;
		default:
			break;
		}
	}

	return 0;
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
		.get_operation_info	= accel_mlx5_get_operation_info,
	},
	.attr = {
		.qp_size = ACCEL_MLX5_QP_SIZE,
		.cq_size = ACCEL_MLX5_CQ_SIZE,
		.num_requests = ACCEL_MLX5_NUM_MKEYS,
		.dev_rr_threshold = ACCEL_MLX5_DEV_RR_THRESHOLD,
		.crypto_split_blocks = 0,
		.disable_signature = false,
		.disable_crypto = false,
		.umr_memory_buffer = false,
		.enable_driver = true,
	},
	.enabled = true,
};

static struct spdk_accel_driver g_accel_mlx5_driver = {
	.name			= SPDK_MLX5_DRIVER_NAME,
	.execute_sequence	= accel_mlx5_execute_sequence,
	.get_io_channel		= accel_mlx5_get_io_channel,
	.get_operation_info	= accel_mlx5_get_operation_info,
};

SPDK_ACCEL_MODULE_REGISTER(mlx5, &g_accel_mlx5.module)
SPDK_ACCEL_DRIVER_REGISTER(mlx5, &g_accel_mlx5_driver)
SPDK_LOG_REGISTER_COMPONENT(accel_mlx5)
