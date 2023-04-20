/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "infiniband/mlx5dv.h"
#include "mlx5_ifc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk/likely.h"

#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/mlx5.h"
#include "mlx5_priv.h"

#define MLX5_DMA_Q_TX_MOD_COUNT 16
#define MLX5_DMA_Q_TX_CQE_SIZE  64

struct _mlx5_err_cqe {
	uint8_t		rsvd0[32];
	uint32_t	srqn;
	uint8_t		rsvd1[16];
	uint8_t		hw_err_synd;
	uint8_t		rsvd2[1];
	uint8_t		vendor_err_synd;
	uint8_t		syndrome;
	uint32_t	s_wqe_opcode_qpn;
	uint16_t	wqe_counter;
	uint8_t		signature;
	uint8_t		op_own;
};

static const char *
mlx5_cqe_err_opcode(struct _mlx5_err_cqe *ecqe)
{
	uint8_t wqe_err_opcode = be32toh(ecqe->s_wqe_opcode_qpn) >> 24;

	switch (ecqe->op_own >> 4) {
	case MLX5_CQE_REQ_ERR:
		switch (wqe_err_opcode) {
		case MLX5_OPCODE_RDMA_WRITE_IMM:
		case MLX5_OPCODE_RDMA_WRITE:
			return "RDMA_WRITE";
		case MLX5_OPCODE_SEND_IMM:
		case MLX5_OPCODE_SEND:
		case MLX5_OPCODE_SEND_INVAL:
			return "SEND";
		case MLX5_OPCODE_RDMA_READ:
			return "RDMA_READ";
		case MLX5_OPCODE_ATOMIC_CS:
			return "COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_FA:
			return "FETCH_ADD";
		case MLX5_OPCODE_ATOMIC_MASKED_CS:
			return "MASKED_COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_MASKED_FA:
			return "MASKED_FETCH_ADD";
		case MLX5_OPCODE_MMO:
			return "GGA_DMA";
		default:
			return "";
		}
	case MLX5_CQE_RESP_ERR:
		return "RECV";
	default:
		return "";
	}
}

static int
mlx5_cqe_err(struct mlx5_cqe64 *cqe)
{
	struct _mlx5_err_cqe *ecqe = (struct _mlx5_err_cqe *)cqe;
	uint16_t wqe_counter;
	uint32_t qp_num = 0;
	char info[200] = {0};

	wqe_counter = be16toh(ecqe->wqe_counter);
	qp_num = be32toh(ecqe->s_wqe_opcode_qpn) & ((1 << 24) - 1);

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR) {
		SPDK_DEBUGLOG(mlx5, "QP 0x%x wqe[%d] is flushed\n", qp_num, wqe_counter);
		return ecqe->syndrome;
	}

	switch (ecqe->syndrome) {
	case MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		snprintf(info, sizeof(info), "Local length");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		snprintf(info, sizeof(info), "Local QP operation");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_PROT_ERR:
		snprintf(info, sizeof(info), "Local protection");
		break;
	case MLX5_CQE_SYNDROME_WR_FLUSH_ERR:
		snprintf(info, sizeof(info), "WR flushed because QP in error state");
		break;
	case MLX5_CQE_SYNDROME_MW_BIND_ERR:
		snprintf(info, sizeof(info), "Memory window bind");
		break;
	case MLX5_CQE_SYNDROME_BAD_RESP_ERR:
		snprintf(info, sizeof(info), "Bad response");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		snprintf(info, sizeof(info), "Local access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		snprintf(info, sizeof(info), "Invalid request");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		snprintf(info, sizeof(info), "Remote access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_OP_ERR:
		snprintf(info, sizeof(info), "Remote QP");
		break;
	case MLX5_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Transport retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Receive-no-ready retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		snprintf(info, sizeof(info), "Remote side aborted");
		break;
	default:
		snprintf(info, sizeof(info), "Generic");
		break;
	}
	SPDK_ERRLOG("Error on QP 0x%x wqe[%03d]: %s (synd 0x%x vend 0x%x hw 0x%x) opcode %s\n",
		    qp_num, wqe_counter, info, ecqe->syndrome, ecqe->vendor_err_synd, ecqe->hw_err_synd,
		    mlx5_cqe_err_opcode(ecqe));

	return ecqe->syndrome;
}

static inline void
mlx5_dma_xfer_full(struct spdk_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm, uint32_t klm_count,
	      uint64_t raddr, uint32_t rkey, int op, uint32_t flags, uint64_t wr_id, uint32_t bb_count)
{
	struct spdk_mlx5_hw_qp *hw_qp = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, pi;

	fm_ce_se = flags | qp->tx_flags;

	/* absolute PI value */
	pi = hw_qp->sq.pi & (hw_qp->sq.wqe_cnt - 1);
	SPDK_DEBUGLOG(mlx5, "opc %d, sge_count %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, klm_count,
		      bb_count, pi, fm_ce_se);

	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS accounts for all the segments in the WQE as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq.pi, op, 0, hw_qp->qp_num, fm_ce_se, 2 + klm_count, 0, 0);

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	rseg->raddr = htobe64(raddr);
	rseg->rkey  = htobe32(rkey);

	rseg->reserved = 0;

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	for (i = 0; i < klm_count; i++) {
		mlx5dv_set_data_seg(dseg, klm[i].byte_count, klm[i].lkey, klm[i].addr);
		dseg = dseg + 1;
	}

	mlx5_qp_wqe_submit(qp, ctrl, bb_count);

	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
}

static inline void
mlx5_dma_xfer_wrap_around(struct spdk_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm, uint32_t klm_count,
	      uint64_t raddr, uint32_t rkey, int op, uint32_t flags, uint64_t wr_id, uint32_t bb_count)
{
	struct spdk_mlx5_hw_qp *hw_qp = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, to_end, pi;

	fm_ce_se = flags | qp->tx_flags;

	/* absolute PI value */
	pi = hw_qp->sq.pi & (hw_qp->sq.wqe_cnt - 1);
	SPDK_DEBUGLOG(mlx5, "opc %d, sge_count %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, klm_count,
		      bb_count, pi, fm_ce_se);

	to_end = (hw_qp->sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS accounts for all the segments in the WQE as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq.pi, op, 0, hw_qp->qp_num, fm_ce_se, 2 + klm_count, 0, 0);
	to_end -= sizeof(struct mlx5_wqe_ctrl_seg); /* 16 bytes */

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	rseg->raddr = htobe64(raddr);
	rseg->rkey  = htobe32(rkey);

	rseg->reserved = 0;
	to_end -= sizeof(struct mlx5_wqe_raddr_seg); /* 16 bytes */

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	for (i = 0; i < klm_count; i++) {
		mlx5dv_set_data_seg(dseg, klm[i].byte_count, klm[i].lkey, klm[i].addr);
		to_end -= sizeof(struct mlx5_wqe_data_seg); /* 16 bytes */
		if (to_end != 0) {
			dseg = dseg + 1;
		} else {
			/* Start from the beginning of SQ */
			dseg = (struct mlx5_wqe_data_seg *)(hw_qp->sq.addr);
			to_end = hw_qp->sq.wqe_cnt * MLX5_SEND_WQE_BB;
		}
	}

	mlx5_qp_wqe_submit(qp, ctrl, bb_count);

	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
}

int
spdk_mlx5_dma_qp_rdma_write(struct spdk_mlx5_dma_qp *dma_qp, struct mlx5_wqe_data_seg *klm, uint32_t klm_count,
			    uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags)
{
	struct spdk_mlx5_qp *qp = &dma_qp->qp;
	struct spdk_mlx5_hw_qp *hw_qp = &qp->hw;
	uint32_t to_end, pi, bb_count;

	/* One building block is 64 bytes - 4 octowords
	 * It can hold control segment + raddr segment + 2 data segments.
	 * If sge_count (data segments) is bigger than 2 then we consume additional bb */
	bb_count = (klm_count <= 2) ? 1 : 1 + SPDK_CEIL_DIV(klm_count - 2, 4);

	if (spdk_unlikely(bb_count > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(klm_count > qp->max_sge)) {
		return -E2BIG;
	}
	pi = hw_qp->sq.pi & (hw_qp->sq.wqe_cnt - 1);
	to_end = (hw_qp->sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	if (spdk_likely(to_end >= bb_count * MLX5_SEND_WQE_BB)) {
		mlx5_dma_xfer_full(qp, klm, klm_count, dstaddr, rkey, MLX5_OPCODE_RDMA_WRITE, flags, wrid, bb_count);
	} else {
		mlx5_dma_xfer_wrap_around(qp, klm, klm_count, dstaddr, rkey, MLX5_OPCODE_RDMA_WRITE, flags, wrid, bb_count);
	}

	return 0;
}

int
spdk_mlx5_dma_qp_rdma_read(struct spdk_mlx5_dma_qp *dma_qp, struct mlx5_wqe_data_seg *klm, uint32_t klm_count,
			    uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags)
{
	struct spdk_mlx5_qp *qp = &dma_qp->qp;
	struct spdk_mlx5_hw_qp *hw_qp = &qp->hw;
	uint32_t to_end, pi, bb_count;

	/* One building block is 64 bytes - 4 octowords
	 * It can hold control segment + raddr segment + 2 data segments.
	 * If sge_count (data segments) is bigger than 2 then we consume additional bb */
	bb_count = (klm_count <= 2) ? 1 : 1 + SPDK_CEIL_DIV(klm_count - 2, 4);

	if (spdk_unlikely(bb_count > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(klm_count > qp->max_sge)) {
		return -E2BIG;
	}
	pi = hw_qp->sq.pi & (hw_qp->sq.wqe_cnt - 1);
	to_end = (hw_qp->sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	if (spdk_likely(to_end >= bb_count * MLX5_SEND_WQE_BB)) {
		mlx5_dma_xfer_full(qp, klm, klm_count, dstaddr, rkey, MLX5_OPCODE_RDMA_READ, flags, wrid, bb_count);
	} else {
		mlx5_dma_xfer_wrap_around(qp, klm, klm_count, dstaddr, rkey, MLX5_OPCODE_RDMA_READ, flags, wrid, bb_count);
	}

	return 0;
}


// polling start

static inline void
mlx5_qp_tx_complete(struct spdk_mlx5_qp *qp)
{
	if (qp->tx_need_ring_db) {
		qp->tx_need_ring_db = false;
		mlx5_ring_tx_db(qp, qp->ctrl);
	}
}

static inline struct mlx5_cqe64 *
mlx5_cq_get_cqe(struct spdk_mlx5_hw_cq *hw_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	/* note: that the cq_size is known at the compilation time. We pass it
	 * down here so that branch and multiplication will be done at the
	 * compile time during inlining
	 **/
	cqe = (struct mlx5_cqe64 *)(hw_cq->cq_addr + (hw_cq->ci & (hw_cq->cqe_cnt - 1)) *
				    cqe_size);
	return cqe_size == 64 ? cqe : cqe + 1;
}


static inline struct mlx5_cqe64 *
mlx5_cq_poll_one(struct spdk_mlx5_hw_cq *hw_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	cqe = mlx5_cq_get_cqe(hw_cq, cqe_size);

	/* cqe is hw owned */
	if (mlx5dv_get_cqe_owner(cqe) == !(hw_cq->ci & hw_cq->cqe_cnt)) {
		return NULL;
	}

	/* and must have valid opcode */
	if (mlx5dv_get_cqe_opcode(cqe) == MLX5_CQE_INVALID) {
		return NULL;
	}

	hw_cq->ci++;

	SPDK_DEBUGLOG(mlx5,
		      "cq: 0x%x ci: %d CQ opcode %d size %d wqe_counter %d scatter32 %d scatter64 %d\n",
		      hw_cq->cq_num, hw_cq->ci,
		      mlx5dv_get_cqe_opcode(cqe),
		      be32toh(cqe->byte_cnt),
		      be16toh(cqe->wqe_counter),
		      cqe->op_own & MLX5_INLINE_SCATTER_32,
		      cqe->op_own & MLX5_INLINE_SCATTER_64);
	return cqe;
}

static inline uint64_t
mlx5_qp_get_comp_wr_id(struct spdk_mlx5_qp *qp, struct mlx5_cqe64 *cqe)
{
	uint16_t comp_idx;
	uint32_t sq_mask;

	sq_mask = qp->hw.sq.wqe_cnt - 1;
	comp_idx = be16toh(cqe->wqe_counter) & sq_mask;
	SPDK_DEBUGLOG(mlx5, "got cpl, wqe_counter %u, comp_idx %u; wrid %"PRIx64", cpls %u\n",
		      cqe->wqe_counter, comp_idx, qp->completions[comp_idx].wr_id, qp->completions[comp_idx].completions);
	/* If we have several unsignaled WRs, we accumulate them in the completion of the next signaled WR */
	qp->tx_available += qp->completions[comp_idx].completions;

	return qp->completions[comp_idx].wr_id;
}

int
spdk_mlx5_dma_qp_poll_completions(struct spdk_mlx5_dma_qp *dma_qp,
				  struct spdk_mlx5_cq_completion *comp, int max_completions)
{
	struct spdk_mlx5_cq *cq = &dma_qp->cq;
	struct mlx5_cqe64 *cqe;
	int n = 0;

	do {
		cqe = mlx5_cq_poll_one(&cq->hw, MLX5_DMA_Q_TX_CQE_SIZE);
		if (!cqe) {
			break;
		}

		comp[n].wr_id = mlx5_qp_get_comp_wr_id(&dma_qp->qp, cqe);
		if (spdk_unlikely(mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_REQ)) {
			comp[n].status = mlx5_cqe_err(cqe);
		} else {
			comp[n].status = IBV_WC_SUCCESS;
		}

		n++;

	} while (n < max_completions);

	mlx5_qp_tx_complete(&dma_qp->qp);

	return n;
}

#ifdef DEBUG
void
mlx5_qp_dump_wqe(struct spdk_mlx5_qp *qp, int n_wqe_bb)
{
	struct spdk_mlx5_hw_qp *hw = &qp->hw;
	uint32_t pi;
	uint32_t to_end;
	uint32_t *wqe;
	int i;
	extern struct spdk_log_flag SPDK_LOG_mlx5_sq;

	if (!SPDK_LOG_mlx5_sq.enabled) {
		return;
	}

	pi = hw->sq.pi & (hw->sq.wqe_cnt - 1);
	to_end = (hw->sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	wqe = mlx5_qp_get_wqe_bb(hw);

	SPDK_DEBUGLOG(mlx5_sq, "QP: qpn 0x%" PRIx32 ", wqe_index 0x%" PRIx32 ", addr %p\n",
		      hw->qp_num, pi, wqe);
	for (i = 0; i < n_wqe_bb; i++) {
		fprintf(stderr,
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n"
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n"
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n"
			"%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			be32toh(wqe[0]),  be32toh(wqe[1]),  be32toh(wqe[2]),  be32toh(wqe[3]),
			be32toh(wqe[4]),  be32toh(wqe[5]),  be32toh(wqe[6]),  be32toh(wqe[7]),
			be32toh(wqe[8]),  be32toh(wqe[9]),  be32toh(wqe[10]), be32toh(wqe[11]),
			be32toh(wqe[12]), be32toh(wqe[13]), be32toh(wqe[14]), be32toh(wqe[15]));
		wqe = mlx5_qp_get_next_wqbb(hw, &to_end, wqe);
	}
}
#endif

SPDK_LOG_REGISTER_COMPONENT(mlx5_sq)
