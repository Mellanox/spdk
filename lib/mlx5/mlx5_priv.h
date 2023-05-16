/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/barrier.h"

#include "infiniband/mlx5dv.h"
#include "spdk_internal/mlx5.h"

#define MLX5_WQE_UMR_CTRL_MKEY_MASK_BSF_OCTOWORD_SIZE (0x1 << 5)
#define MLX5_CRYPTO_BSF_SIZE_64B (0x2)
#define MLX5_CRYPTO_BSF_P_TYPE_CRYPTO (0x1)

struct mlx5_crypto_bsf_seg {
	uint8_t		size_type;
	uint8_t		enc_order;
	uint8_t		rsvd0;
	uint8_t		enc_standard;
	__be32		raw_data_size;
	uint8_t		crypto_block_size_pointer;
	uint8_t		rsvd1[7];
	uint8_t		xts_initial_tweak[16];
	__be32		dek_pointer;
	uint8_t		rsvd2[4];
	uint8_t		keytag[8];
	uint8_t		rsvd3[16];
};

static inline void *
mlx5_qp_get_wqe_bb(struct spdk_mlx5_hw_qp *hw_qp)
{
	return (void *)hw_qp->sq_addr + (hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1)) * MLX5_SEND_WQE_BB;
}

static inline void *
mlx5_qp_get_next_wqbb(struct spdk_mlx5_hw_qp *qp, uint32_t *to_end, void *cur)
{
	*to_end -= MLX5_SEND_WQE_BB;
	if (*to_end == 0) { /* wqe buffer wap around */
		*to_end = qp->sq_wqe_cnt * MLX5_SEND_WQE_BB;
		return (void *)(uintptr_t)qp->sq_addr;
	}

	return ((char *)cur) + MLX5_SEND_WQE_BB;
}

static inline void
mlx5_qp_set_comp(struct spdk_mlx5_qp *dv_qp, uint16_t pi,
		 uint64_t wr_id, uint32_t fm_ce_se, uint32_t n_bb)
{
	dv_qp->completions[pi].wr_id = wr_id;
	if ((fm_ce_se & MLX5_WQE_CTRL_CQ_UPDATE) != MLX5_WQE_CTRL_CQ_UPDATE) {
		/* non-signaled WQE, accumulate it in outstanding */
		dv_qp->nonsignaled_outstanding += n_bb;
		dv_qp->completions[pi].completions = 0;
		return;
	}

	/* Store number of previous nonsignaled WQEs */
	dv_qp->completions[pi].completions = dv_qp->nonsignaled_outstanding + n_bb;
	dv_qp->nonsignaled_outstanding = 0;
}


#if defined(__aarch64__)
#define spdk_memory_bus_store_fence()  asm volatile("dmb oshst" ::: "memory")
#elif defined(__i386__) || defined(__x86_64__)
#define spdk_memory_bus_store_fence() spdk_wmb()
#endif

static inline void
mlx5_update_tx_db(struct spdk_mlx5_qp *qp)
{
	/*
	 * Use cpu barrier to prevent code reordering
	 */
	spdk_smp_wmb();

	((uint32_t *)qp->hw.dbr_addr)[MLX5_SND_DBR] = htobe32(qp->hw.sq_pi);
}

static inline void
mlx5_flush_tx_db(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	*(uint64_t *)(qp->hw.sq_bf_addr) = *(uint64_t *)ctrl;
}

static inline void
mlx5_ring_tx_db(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	/* 8.9.3.1  Posting a Work Request to Work Queue
	 * 1. Write WQE to the WQE buffer sequentially to previously-posted
	 *    WQE (on WQEBB granularity)
	 *
	 * 2. Update Doorbell Record associated with that queue by writing
	 *    the sq_wqebb_counter or wqe_counter for send and RQ respectively
	 **/
	mlx5_update_tx_db(qp);

	/* Make sure that doorbell record is written before ringing the doorbell
	 **/
	spdk_memory_bus_store_fence();

	/* 3. For send request ring DoorBell by writing to the Doorbell
	 *    Register field in the UAR associated with that queue
	 */
	mlx5_flush_tx_db(qp, ctrl);

	/* If UAR is mapped as WC (write combined) we need another fence to
	 * force write. Otherwise it may take a long time.
	 * On BF2/1 uar is mapped as NC (non combined) and fence is not needed
	 * here.
	 */
#if !defined(__aarch64__)
	if (!qp->hw.sq_tx_db_nc) {
		spdk_memory_bus_store_fence();
	}
#endif
}

#ifdef DEBUG
void mlx5_qp_dump_wqe(struct spdk_mlx5_qp *qp, int n_wqe_bb);
#else
#define mlx5_qp_dump_wqe(...) do { } while (0)
#endif

static inline void
mlx5_qp_wqe_submit(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl, uint16_t n_wqe_bb, uint16_t ctrlr_pi)
{
	mlx5_qp_dump_wqe(qp, n_wqe_bb);

	/* Delay ringing the doorbell */
	qp->hw.sq_pi += n_wqe_bb;
	qp->tx_need_ring_db = true;
	qp->last_pi = ctrlr_pi;
	qp->ctrl = ctrl;
}

static inline void
mlx5_set_ctrl_seg(struct mlx5_wqe_ctrl_seg *ctrl, uint16_t pi,
		  uint8_t opcode, uint8_t opmod, uint32_t qp_num,
		  uint8_t fm_ce_se, uint8_t ds,
		  uint8_t signature, uint32_t imm)
{
	*(uint32_t *)((void *)ctrl + 8) = 0;
	mlx5dv_set_ctrl_seg(ctrl, pi, opcode, opmod, qp_num,
			    fm_ce_se, ds, signature, imm);
}
