/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "mlx5_priv.h"
#include "mlx5_ifc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk/likely.h"

#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma_provider_wc.h"

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

struct mlx5_sigerr_cqe {
	uint8_t rsvd0[16];
	__be32 expected_trans_sig;
	__be32 actual_trans_sig;
	__be32 expected_ref_tag;
	__be32 actual_ref_tag;
	__be16 syndrome;
	uint8_t sig_type;
	uint8_t domain;
	__be32 mkey;
	__be64 sig_err_offset;
	uint8_t rsvd30[14];
	uint8_t signature;
	uint8_t op_own;
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
		snprintf(info, sizeof(info), "Remote OP");
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
	SPDK_WARNLOG("Error on QP 0x%x wqe[%03d]: %s (synd 0x%x vend 0x%x hw 0x%x) opcode %s\n",
		     qp_num, wqe_counter, info, ecqe->syndrome, ecqe->vendor_err_synd, ecqe->hw_err_synd,
		     mlx5_cqe_err_opcode(ecqe));

	return ecqe->syndrome;
}

/*
 * DATA WQE LAYOUT:
 * ----------------------------------
 * | gen_ctrl |   rseg   |   dseg   |
 * ----------------------------------
 *   16bytes    16bytes    16bytes * sge_count
 */

static inline void
mlx5_dma_xfer_full(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count, uint64_t raddr,
		   uint32_t rkey, int op, uint32_t flags, uint64_t wr_id, uint32_t bb_count)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, pi;

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);

	/* absolute PI value */
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	SPDK_DEBUGLOG(mlx5, "opc %d, sge_count %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, sge_count,
		      bb_count, hw_qp->sq_pi, fm_ce_se);

	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS (data segment) accounts for all the segments in the WQE
	 * as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq_pi, op, 0, hw_qp->qp_num, fm_ce_se, 2 + sge_count, 0, 0);

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	rseg->raddr = htobe64(raddr);
	rseg->rkey  = htobe32(rkey);
	rseg->reserved = 0;

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	for (i = 0; i < sge_count; i++) {
		mlx5dv_set_data_seg(dseg, sge[i].length, sge[i].lkey, sge[i].addr);
		dseg = dseg + 1;
	}

	mlx5_qp_submit_sq_wqe(qp, ctrl, bb_count, pi);

	mlx5_qp_set_sq_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
}

static inline void
mlx5_dma_xfer_wrap_around(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
			  uint64_t raddr, uint32_t rkey, int op, uint32_t flags, uint64_t wr_id, uint32_t bb_count)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, to_end, pi;

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);

	/* absolute PI value */
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	SPDK_DEBUGLOG(mlx5, "opc %d, sge_count %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, sge_count,
		      bb_count, pi, fm_ce_se);

	to_end = (hw_qp->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS (data segment) accounts for all the segments in the WQE
	 * as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq_pi, op, 0, hw_qp->qp_num, fm_ce_se, 2 + sge_count, 0, 0);
	to_end -= sizeof(struct mlx5_wqe_ctrl_seg); /* 16 bytes */

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	rseg->raddr = htobe64(raddr);
	rseg->rkey  = htobe32(rkey);
	rseg->reserved = 0;
	to_end -= sizeof(struct mlx5_wqe_raddr_seg); /* 16 bytes */

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	for (i = 0; i < sge_count; i++) {
		mlx5dv_set_data_seg(dseg, sge[i].length, sge[i].lkey, sge[i].addr);
		to_end -= sizeof(struct mlx5_wqe_data_seg); /* 16 bytes */
		if (to_end != 0) {
			dseg = dseg + 1;
		} else {
			/* Start from the beginning of SQ */
			dseg = (struct mlx5_wqe_data_seg *)(hw_qp->sq_addr);
			to_end = hw_qp->sq_wqe_cnt * MLX5_SEND_WQE_BB;
		}
	}

	mlx5_qp_submit_sq_wqe(qp, ctrl, bb_count, pi);

	mlx5_qp_set_sq_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
}

static inline int
mlx5_qp_rdma_op(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count, uint64_t dstaddr,
		uint32_t rkey, uint64_t wrid, uint32_t flags, int op)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	uint32_t to_end, pi, bb_count;

	/* One bb (building block) is 64 bytes - 4 octowords
	 * It can hold control segment + raddr segment + 2 sge segments.
	 * If sge_count (data segments) is bigger than 2 then we consume additional bb */
	bb_count = (sge_count <= 2) ? 1 : 1 + SPDK_CEIL_DIV(sge_count - 2, 4);

	if (spdk_unlikely(bb_count > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(sge_count > qp->max_send_sge)) {
		return -E2BIG;
	}
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	to_end = (hw_qp->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	flags |= qp->extra_flags;

	if (spdk_likely(to_end >= bb_count * MLX5_SEND_WQE_BB)) {
		mlx5_dma_xfer_full(qp, sge, sge_count, dstaddr, rkey, op, flags, wrid, bb_count);
	} else {
		mlx5_dma_xfer_wrap_around(qp, sge, sge_count, dstaddr, rkey, op, flags, wrid, bb_count);
	}
	qp->extra_flags = 0;

	return 0;
}

int
spdk_mlx5_qp_rdma_write(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
			uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags)
{
	return mlx5_qp_rdma_op(qp, sge, sge_count, dstaddr, rkey, wrid, flags, MLX5_OPCODE_RDMA_WRITE);
}

int
spdk_mlx5_qp_rdma_read(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t sge_count,
		       uint64_t dstaddr, uint32_t rkey, uint64_t wrid, uint32_t flags)
{
	return mlx5_qp_rdma_op(qp, sge, sge_count, dstaddr, rkey, wrid, flags, MLX5_OPCODE_RDMA_READ);
}

static inline void
mlx5_dma_send_full(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t num_sge, int op,
		   uint32_t flags, uint32_t imm, uint64_t wr_id, uint32_t bb_count)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, pi;

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);

	/* absolute PI value */
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	SPDK_DEBUGLOG(mlx5, "opc %d, num_sge %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, num_sge,
		      bb_count, hw_qp->sq_pi, fm_ce_se);

	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS accounts for all the segments in the WQE as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq_pi, op, 0, hw_qp->qp_num, fm_ce_se, 1 + num_sge, 0, imm);

	dseg = (struct mlx5_wqe_data_seg *)(ctrl + 1);
	for (i = 0; i < num_sge; i++) {
		mlx5dv_set_data_seg(dseg, sge[i].length, sge[i].lkey, sge[i].addr);
		dseg = dseg + 1;
	}

	mlx5_qp_submit_sq_wqe(qp, ctrl, bb_count, pi);

	mlx5_qp_set_sq_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
}

static inline void
mlx5_dma_send_wrap_around(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t num_sge,
			  int op, uint32_t flags, uint32_t imm, uint64_t wr_id, uint32_t bb_count)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se;
	uint32_t i, to_end, pi;

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);

	/* absolute PI value */
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	SPDK_DEBUGLOG(mlx5, "opc %d, num_sge %u, bb_count %u, orig pi %u, fm_ce_se %x\n", op, num_sge,
		      bb_count, pi, fm_ce_se);

	to_end = (hw_qp->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	ctrl = (struct mlx5_wqe_ctrl_seg *) mlx5_qp_get_wqe_bb(hw_qp);
	/* WQE size in octowords (16-byte units). DS accounts for all the segments in the WQE as summarized in WQE construction */
	mlx5_set_ctrl_seg(ctrl, hw_qp->sq_pi, op, 0, hw_qp->qp_num, fm_ce_se, 1 + num_sge, 0, imm);
	to_end -= sizeof(struct mlx5_wqe_ctrl_seg); /* 16 bytes */

	dseg = (struct mlx5_wqe_data_seg *)(ctrl + 1);
	for (i = 0; i < num_sge; i++) {
		mlx5dv_set_data_seg(dseg, sge[i].length, sge[i].lkey, sge[i].addr);
		to_end -= sizeof(struct mlx5_wqe_data_seg); /* 16 bytes */
		if (to_end != 0) {
			dseg = dseg + 1;
		} else {
			/* Start from the beginning of SQ */
			dseg = (struct mlx5_wqe_data_seg *)(hw_qp->sq_addr);
			to_end = hw_qp->sq_wqe_cnt * MLX5_SEND_WQE_BB;
		}
	}

	mlx5_qp_submit_sq_wqe(qp, ctrl, bb_count, pi);

	mlx5_qp_set_sq_comp(qp, pi, wr_id, fm_ce_se, bb_count);
	assert(qp->tx_available >= bb_count);
	qp->tx_available -= bb_count;
}

static inline int
mlx5_qp_send(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t num_sge, int opcode,
	     uint32_t imm, uint64_t wrid, uint32_t flags)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	uint32_t to_end, pi, bb_count;

	/* One building block is 64 bytes - 4 octowords
	 * It can hold control segment + 3 data segments.
	 * If num_sge (data segments) is bigger than 3 then we consume additional bb */
	bb_count = (num_sge <= 3) ? 1 : 1 + SPDK_CEIL_DIV(num_sge - 3, 4);

	if (spdk_unlikely(bb_count > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(num_sge > qp->max_send_sge)) {
		return -E2BIG;
	}
	pi = hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1);
	to_end = (hw_qp->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	if (spdk_likely(to_end >= bb_count * MLX5_SEND_WQE_BB)) {
		mlx5_dma_send_full(qp, sge, num_sge, opcode, flags, imm, wrid, bb_count);
	} else {
		mlx5_dma_send_wrap_around(qp, sge, num_sge, opcode, flags, imm, wrid,
					  bb_count);
	}

	return 0;
}

int
spdk_mlx5_qp_send(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t num_sge,
		  uint64_t wrid, uint32_t flags)
{
	return mlx5_qp_send(qp, sge, num_sge, MLX5_OPCODE_SEND, 0, wrid, flags);
}

int
spdk_mlx5_qp_send_inv(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t num_sge,
		      uint32_t invalidate_rkey, uint64_t wrid, uint32_t flags)
{
	return mlx5_qp_send(qp, sge, num_sge, MLX5_OPCODE_SEND_INVAL, htobe32(invalidate_rkey), wrid,
			    flags);
}

int
spdk_mlx5_qp_recv(struct spdk_mlx5_qp *qp, struct ibv_sge *sge, uint32_t num_sge, uint64_t wrid)
{
	struct mlx5_hw_qp *hw_qp = &qp->hw;
	uint16_t wqe_index;
	struct mlx5_wqe_data_seg *dseg;
	uint32_t i;

	SPDK_DEBUGLOG(mlx5, "qp 0x%x, wrid 0x%lx\n", hw_qp->qp_num, wrid);
	if (spdk_unlikely(qp->rx_available == 0)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(num_sge > qp->max_recv_sge)) {
		return -E2BIG;
	}

	wqe_index = hw_qp->rq_pi++ & (hw_qp->rq_wqe_cnt - 1);
	qp->rq_completions[wqe_index].wr_id = wrid;
	dseg = (void *)hw_qp->rq_addr + wqe_index * hw_qp->rq_stride;

	for (i = 0; i < num_sge; i++, dseg++, sge++) {
		dseg->byte_count	= htobe32(sge->length);
		dseg->lkey		= htobe32(sge->lkey);
		dseg->addr		= htobe64(sge->addr);
	}

	/*
	 * The receive WQE has a fixed size defined in the create QP command. When num_sge is
	 * lower than qp->max_recv_sge, the last entries of the data segment are not used.
	 * Set lkey to MLX5_INVALID_LKEY for the first unused entry to let the HW recognize
	 * the end of the data segment.
	 */
	if (i < qp->max_recv_sge) {
		dseg->byte_count	= 0;
		dseg->lkey		= htobe32(MLX5_INVALID_LKEY);
		dseg->addr		= 0;
	}
	qp->rx_available--;
	mlx5_qp_dump_rq_wqe(qp, wqe_index);

	return 0;
}

void
spdk_mlx5_qp_complete_recv(struct spdk_mlx5_qp *qp)
{
	mlx5_ring_rx_db(qp);
}

/* polling start */

static inline void
mlx5_qp_update_sq_comp(struct spdk_mlx5_qp *qp)
{
	qp->sq_completions[qp->last_pi].completions = qp->nonsignaled_outstanding;
	qp->nonsignaled_outstanding = 0;
}

static inline struct mlx5_cqe64 *
mlx5_cq_get_cqe(struct mlx5_hw_cq *hw_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	/* note: that the cq_size is known at the compilation time. We pass it
	 * down here so that branch and multiplication will be done at the
	 * compile time during inlining
	 */
	cqe = (struct mlx5_cqe64 *)(hw_cq->cq_addr + (hw_cq->ci & (hw_cq->cqe_cnt - 1)) *
				    cqe_size);
	return cqe_size == 64 ? cqe : cqe + 1;
}


static inline struct mlx5_cqe64 *
mlx5_cq_poll_one(struct mlx5_hw_cq *hw_cq, int cqe_size)
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

	mlx5_cq_dump_cqe(hw_cq, cqe);

	return cqe;
}

static inline uint64_t
mlx5_qp_get_sq_comp_wr_id(struct spdk_mlx5_qp *qp, struct mlx5_cqe64 *cqe)
{
	uint16_t comp_idx;
	uint32_t sq_mask;

	sq_mask = qp->hw.sq_wqe_cnt - 1;
	comp_idx = be16toh(cqe->wqe_counter) & sq_mask;
	SPDK_DEBUGLOG(mlx5, "got send cpl, wqe_counter %u, comp_idx %u; wrid %"PRIx64", cpls %u\n",
		      cqe->wqe_counter, comp_idx, qp->sq_completions[comp_idx].wr_id,
		      qp->sq_completions[comp_idx].completions);
	/* If we have several unsignaled WRs, we accumulate them in the completion of the next signaled WR */
	qp->tx_available += qp->sq_completions[comp_idx].completions;

	return qp->sq_completions[comp_idx].wr_id;
}

static inline uint64_t
mlx5_qp_get_rq_comp_wr_id(struct spdk_mlx5_qp *qp, struct mlx5_cqe64 *cqe)
{
	uint16_t comp_idx;
	uint32_t rq_mask;

	rq_mask = qp->hw.rq_wqe_cnt - 1;
	comp_idx = be16toh(cqe->wqe_counter) & rq_mask;
	SPDK_DEBUGLOG(mlx5, "got recv cpl, wqe_counter %u, comp_idx %u; wrid %"PRIx64"\n",
		      cqe->wqe_counter, comp_idx, qp->rq_completions[comp_idx].wr_id);
	qp->rx_available++;

	return qp->rq_completions[comp_idx].wr_id;
}

static inline uint64_t
mlx5_srq_get_comp_wr_id(struct spdk_mlx5_srq *srq, struct mlx5_cqe64 *cqe)
{
	uint16_t comp_idx;

	/* A mask is not applied because the WQE index is reported for SRQ, not the WQE counter. */
	comp_idx = be16toh(cqe->wqe_counter);
	mlx5_srq_free_wqe(&srq->hw, comp_idx);
	SPDK_DEBUGLOG(mlx5, "got srq cpl, comp_idx %u, wrid %" PRIx64 "\n",
		      comp_idx, srq->wr_id[comp_idx]);

	return srq->wr_id[comp_idx];
}

enum {
	MLX5_SIGERR_CQE_SYNDROME_REFTAG = 1 << 11,
	MLX5_SIGERR_CQE_SYNDROME_APPTAG = 1 << 12,
	MLX5_SIGERR_CQE_SYNDROME_GUARD = 1 << 13,
};

static void
mlx5_cqe_sigerr_comp(struct mlx5_sigerr_cqe *cqe, struct spdk_mlx5_cq_completion *comp,
		     union spdk_mlx5_cq_error *err)
{
	comp->status = SPDK_MLX5_CQE_SYNDROME_SIGERR;
	comp->mkey = be32toh(cqe->mkey);

	if (err) {
		struct spdk_mlx5_sig_err *sigerr = &err->sigerr;

		sigerr->syndrome = be16toh(cqe->syndrome);
		sigerr->expected = (uint64_t)be32toh(cqe->expected_trans_sig) << 32 |
				   be32toh(cqe->expected_ref_tag);
		sigerr->actual = (uint64_t)be32toh(cqe->actual_trans_sig) << 32 |
				 be32toh(cqe->actual_ref_tag);
		sigerr->offset = be64toh(cqe->sig_err_offset);
		sigerr->sig_type = cqe->sig_type & 0x7;
		sigerr->domain = cqe->domain & 7;

		if (sigerr->syndrome & MLX5_SIGERR_CQE_SYNDROME_REFTAG) {
			SPDK_WARNLOG("Failed to compare Ref Tag: Offset=%" PRIx64 "," \
				     " Expected=%" PRIx64 ", Actual=%" PRIx64 "\n",
				     sigerr->offset, sigerr->expected, sigerr->actual);
		} else if (sigerr->syndrome & MLX5_SIGERR_CQE_SYNDROME_GUARD) {
			SPDK_WARNLOG("Failed to compare Guard: Offset=%" PRIx64 "," \
				     " Expected=%" PRIx64 ", Actual=%" PRIx64 "\n",
				     sigerr->offset, sigerr->expected >> 48, sigerr->actual >> 48);
		} else if (sigerr->syndrome & MLX5_SIGERR_CQE_SYNDROME_APPTAG) {
			SPDK_WARNLOG("Failed to compare App Tag: Offset=%" PRIx64 "," \
				     " Expected=%" PRIx64 ", Actual=%" PRIx64 "\n",
				     sigerr->offset, sigerr->expected >> 32, sigerr->actual >> 32);
		}
	}

	SPDK_DEBUGLOG(mlx5,
		      "got SIGERR CQE, sig_type 0x%x, syndrome 0x%x, mkey 0x%x, expected_sig 0x%x, actual_trans_sig 0x%x\n",
		      cqe->sig_type, be16toh(cqe->syndrome), comp->mkey, be32toh(cqe->expected_trans_sig),
		      be32toh(cqe->actual_trans_sig));
}

int
spdk_mlx5_cq_poll_completions(struct spdk_mlx5_cq *cq, struct spdk_mlx5_cq_completion *comp,
			      union spdk_mlx5_cq_error *err, int max_completions)
{
	struct spdk_mlx5_qp *qp;
	struct mlx5_cqe64 *cqe;
	union spdk_mlx5_cq_error *_err;
	uint8_t opcode;
	int n = 0;

	do {
		cqe = mlx5_cq_poll_one(&cq->hw, MLX5_DMA_Q_TX_CQE_SIZE);
		if (!cqe) {
			break;
		}

		qp = mlx5_cq_find_qp(cq, be32toh(cqe->sop_drop_qpn) & 0xffffff);
		if (spdk_unlikely(!qp)) {
			return -ENODEV;
		}

		opcode = mlx5dv_get_cqe_opcode(cqe);
		if (spdk_likely(opcode == MLX5_CQE_REQ)) {
			comp[n].wr_id = mlx5_qp_get_sq_comp_wr_id(qp, cqe);
			comp[n].status = IBV_WC_SUCCESS;
		} else if (opcode == MLX5_CQE_SIG_ERR) {
			_err = err != NULL ? &err[n] : NULL;
			mlx5_cqe_sigerr_comp((struct mlx5_sigerr_cqe *)cqe, &comp[n], _err);
		} else {
			comp[n].wr_id = mlx5_qp_get_sq_comp_wr_id(qp, cqe);
			comp[n].status = mlx5_cqe_err(cqe);
		}
		n++;
	} while (n < max_completions);

	return n;
}

static inline int
handle_good_cqe_req(struct mlx5_cqe64 *cqe, struct ibv_wc *wc)
{
	assert(cqe);
	assert(wc);
	/* Inline data in CQE is not implemented for the send operations */
	assert(!(cqe->op_own & MLX5_INLINE_SCATTER_32));
	assert(!(cqe->op_own & MLX5_INLINE_SCATTER_64));

	uint8_t send_opcode = be32toh(cqe->sop_drop_qpn) >> 24;

	switch (send_opcode) {
	case MLX5_OPCODE_RDMA_WRITE_IMM:
		wc->wc_flags |= IBV_WC_WITH_IMM;
	/* FALLTHROUGH */
	case MLX5_OPCODE_RDMA_WRITE:
		wc->opcode = IBV_WC_RDMA_WRITE;
		break;
	case MLX5_OPCODE_SEND_IMM:
		wc->wc_flags |= IBV_WC_WITH_IMM;
	/* FALLTHROUGH */
	case MLX5_OPCODE_SEND:
	case MLX5_OPCODE_SEND_INVAL:
		wc->opcode = IBV_WC_SEND;
		break;
	case MLX5_OPCODE_RDMA_READ:
		wc->opcode = IBV_WC_RDMA_READ;
		wc->byte_len = be32toh(cqe->byte_cnt);
		break;
	default:
		SPDK_ERRLOG("Invalid send_opcode 0x%x\n", send_opcode);
		return -EINVAL;
	}

	return 0;
}

static inline int
handle_good_cqe_resp(struct mlx5_cqe64 *cqe, struct ibv_wc *wc)
{
	assert(cqe);
	assert(wc);

	uint8_t opcode = cqe->op_own >> 4;
	uint8_t grh;

	switch (opcode) {
	case MLX5_CQE_RESP_WR_IMM:
		wc->opcode = IBV_WC_RECV_RDMA_WITH_IMM;
		wc->wc_flags |= IBV_WC_WITH_IMM;
		wc->imm_data = cqe->imm_inval_pkey;
		break;
	case MLX5_CQE_RESP_SEND_IMM:
		wc->wc_flags |= IBV_WC_WITH_IMM;
		wc->imm_data = cqe->imm_inval_pkey;
	/* FALLTHROUGH */
	case MLX5_CQE_RESP_SEND:
		wc->opcode = IBV_WC_RECV;
		break;
	case MLX5_CQE_RESP_SEND_INV:
		wc->opcode = IBV_WC_RECV;
		wc->wc_flags |= IBV_WC_WITH_INV;
		wc->invalidated_rkey = be32toh(cqe->imm_inval_pkey);
		break;
	default:
		SPDK_ERRLOG("Invalid recv opcode 0x%x\n", opcode);
		return -EINVAL;
	}

	wc->slid		= be16toh(cqe->slid);
	wc->sl			= (be32toh(cqe->flags_rqpn) >> 24) & 0xf;
	wc->src_qp		= be32toh(cqe->flags_rqpn) & 0xffffff;
	wc->dlid_path_bits	= cqe->ml_path & 0x7f;
	grh			= (be32toh(cqe->flags_rqpn) >> 28) & 3;
	wc->wc_flags		|= grh ? IBV_WC_GRH : 0;
	wc->pkey_index		= be32toh(cqe->imm_inval_pkey) & 0xffff;

	return 0;
}

static inline void
handle_err_cqe(struct mlx5_err_cqe *cqe, struct ibv_wc *wc)
{
	struct _mlx5_err_cqe *ecqe = (struct _mlx5_err_cqe *)cqe;
	enum ibv_wc_status status;

	switch (cqe->syndrome) {
	case MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		status = IBV_WC_LOC_LEN_ERR;
		break;
	case MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		status = IBV_WC_LOC_QP_OP_ERR;
		break;
	case MLX5_CQE_SYNDROME_LOCAL_PROT_ERR:
		status = IBV_WC_LOC_PROT_ERR;
		break;
	case MLX5_CQE_SYNDROME_WR_FLUSH_ERR:
		status = IBV_WC_WR_FLUSH_ERR;
		break;
	case MLX5_CQE_SYNDROME_MW_BIND_ERR:
		status = IBV_WC_MW_BIND_ERR;
		break;
	case MLX5_CQE_SYNDROME_BAD_RESP_ERR:
		status = IBV_WC_BAD_RESP_ERR;
		break;
	case MLX5_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		status = IBV_WC_LOC_ACCESS_ERR;
		break;
	case MLX5_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		status = IBV_WC_REM_INV_REQ_ERR;
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		status = IBV_WC_REM_ACCESS_ERR;
		break;
	case MLX5_CQE_SYNDROME_REMOTE_OP_ERR:
		status = IBV_WC_REM_OP_ERR;
		break;
	case MLX5_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		status = IBV_WC_RETRY_EXC_ERR;
		break;
	case MLX5_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		status = IBV_WC_RNR_RETRY_EXC_ERR;
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		status = IBV_WC_REM_ABORT_ERR;
		break;
	default:
		status = IBV_WC_GENERAL_ERR;
	}

	wc->status	= status;
	wc->vendor_err	= ((uint32_t)ecqe->vendor_err_synd << 8) | (ecqe->hw_err_synd);
}

/* Minimal information for the caller to handle signature error is
 * status, mkey, and err_type. We can use ibv_wc::status to store status
 * but struct ibv_wc does not have variables for mkey and err_type.
 *
 * For now, use vendor_err for mkey and wr_id for err_type.
 */
static void
handle_sigerr_cqe(struct mlx5_sigerr_cqe *cqe, struct ibv_wc *wc)
{
	uint16_t syndrome;

	wc->status = (enum ibv_wc_status)SPDK_RDMA_PROVIDER_WC_SIG_ERR;

	wc->wr_id = be32toh(cqe->mkey);

	syndrome = be16toh(cqe->syndrome);
	if (syndrome & MLX5_SIGERR_CQE_SYNDROME_REFTAG) {
		wc->vendor_err = SPDK_DIF_REFTAG_ERROR;
	} else if (syndrome & MLX5_SIGERR_CQE_SYNDROME_GUARD) {
		wc->vendor_err = SPDK_DIF_GUARD_ERROR;
	} else if (syndrome & MLX5_SIGERR_CQE_SYNDROME_APPTAG) {
		wc->vendor_err = SPDK_DIF_CHECK_TYPE_APPTAG;
	}
}

static int
mlx5_copy_to_recv_wqe(struct spdk_mlx5_qp *qp, uint16_t index, void *buf, uint32_t size)
{
	struct mlx5_wqe_data_seg *dseg;
	int max;
	int copy;
	int i;

	dseg = (void *)qp->hw.rq_addr + index * qp->hw.rq_stride;
	max = qp->hw.rq_stride / sizeof(*dseg);

	for (i = 0; i < max; i++) {
		if (spdk_unlikely(dseg->lkey == htobe32(MLX5_INVALID_LKEY))) {
			return IBV_WC_LOC_LEN_ERR;
		}

		copy = spdk_min(size, be32toh(dseg->byte_count));

		memcpy((void *)(uintptr_t)be64toh(dseg->addr), buf, copy);

		size -= copy;
		if (size == 0) {
			return IBV_WC_SUCCESS;
		}

		buf += copy;
		dseg++;
	}

	return IBV_WC_LOC_LEN_ERR;
}

static int
mlx5_copy_to_recv_srq(struct spdk_mlx5_srq *srq, uint16_t index, void *buf, uint32_t size)
{
	struct mlx5_wqe_srq_next_seg *next_seg;
	struct mlx5_wqe_data_seg *data_seg;
	uint32_t copy;
	uint32_t i;

	next_seg = mlx5_srq_get_wqe(&srq->hw, index);
	data_seg = (struct mlx5_wqe_data_seg *)(next_seg + 1);

	for (i = 0; i < srq->hw.max_sge; i++) {
		if (spdk_unlikely(data_seg->lkey == htobe32(MLX5_INVALID_LKEY))) {
			return IBV_WC_LOC_LEN_ERR;
		}

		copy = spdk_min(size, be32toh(data_seg->byte_count));

		memcpy((void *)(uintptr_t)be64toh(data_seg->addr), buf, copy);

		size -= copy;
		if (size == 0) {
			return IBV_WC_SUCCESS;
		}

		buf += copy;
		data_seg++;
	}

	return IBV_WC_LOC_LEN_ERR;
}

int
spdk_mlx5_cq_poll_wc(struct spdk_mlx5_cq *cq, int num_entries, struct ibv_wc *wc)
{
	struct spdk_mlx5_qp *qp;
	struct spdk_mlx5_srq *srq;
	struct mlx5_cqe64 *cqe;
	uint16_t wqe_index;
	int n = 0;

	do {
again:
		cqe = mlx5_cq_poll_one(&cq->hw, 64);
		if (!cqe) {
			break;
		}

		qp = mlx5_cq_find_qp(cq, be32toh(cqe->sop_drop_qpn) & 0xffffff);
		if (spdk_unlikely(!qp)) {
			return -ENODEV;
		}

		wc[n].wc_flags = 0;
		wc[n].qp_num = qp->hw.qp_num;

		switch (mlx5dv_get_cqe_opcode(cqe)) {
		case MLX5_CQE_REQ:
			if (handle_good_cqe_req(cqe, &wc[n])) {
				return -EINVAL;
			}
			wc[n].wr_id = mlx5_qp_get_sq_comp_wr_id(qp, cqe);
			wc[n].status = IBV_WC_SUCCESS;
			break;
		case MLX5_CQE_RESP_WR_IMM:
		case MLX5_CQE_RESP_SEND:
		case MLX5_CQE_RESP_SEND_IMM:
		case MLX5_CQE_RESP_SEND_INV:
			wc[n].status = IBV_WC_SUCCESS;
			wc[n].byte_len = be32toh(cqe->byte_cnt);

			srq = qp->srq;
			if (!srq) {
				wc[n].wr_id = mlx5_qp_get_rq_comp_wr_id(qp, cqe);
				if (cqe->op_own & MLX5_INLINE_SCATTER_32) {
					wqe_index = be16toh(cqe->wqe_counter) & (qp->hw.rq_wqe_cnt - 1);
					wc[n].status = mlx5_copy_to_recv_wqe(qp, wqe_index, cqe, wc[n].byte_len);
				} else if (cqe->op_own & MLX5_INLINE_SCATTER_64) {
					wqe_index = be16toh(cqe->wqe_counter) & (qp->hw.rq_wqe_cnt - 1);
					wc[n].status = mlx5_copy_to_recv_wqe(qp, wqe_index, cqe - 1, wc[n].byte_len);
				}
			} else {
				wc[n].wr_id = mlx5_srq_get_comp_wr_id(srq, cqe);

				if (cqe->op_own & MLX5_INLINE_SCATTER_32) {
					wqe_index = be16toh(cqe->wqe_counter);
					wc[n].status = mlx5_copy_to_recv_srq(srq, wqe_index, cqe, wc[n].byte_len);
				} else if (cqe->op_own & MLX5_INLINE_SCATTER_64) {
					wqe_index = be16toh(cqe->wqe_counter);
					wc[n].status = mlx5_copy_to_recv_srq(srq, wqe_index, cqe - 1, wc[n].byte_len);
				}
			}
			if (handle_good_cqe_resp(cqe, &wc[n])) {
				return -EINVAL;
			}
			break;
		case MLX5_CQE_RESIZE_CQ:
			goto again;
		case MLX5_CQE_REQ_ERR:
			wc[n].wr_id = mlx5_qp_get_sq_comp_wr_id(qp, cqe);
			handle_err_cqe((struct mlx5_err_cqe *)cqe, &wc[n]);
			break;
		case MLX5_CQE_RESP_ERR:
			srq = qp->srq;
			if (!srq) {
				wc[n].wr_id = mlx5_qp_get_rq_comp_wr_id(qp, cqe);
			} else {
				wc[n].wr_id = mlx5_srq_get_comp_wr_id(srq, cqe);
			}
			handle_err_cqe((struct mlx5_err_cqe *)cqe, &wc[n]);
			break;
		case MLX5_CQE_SIG_ERR:
			handle_sigerr_cqe((struct mlx5_sigerr_cqe *)cqe, &wc[n]);
			break;
		default:
			SPDK_ERRLOG("Invalid CQE opcode 0x%x\n", mlx5dv_get_cqe_opcode(cqe));
			return -EINVAL;
		}
		n++;

	} while (n < num_entries);

	return n;
}

void
spdk_mlx5_qp_complete_send(struct spdk_mlx5_qp *qp)
{
	if (qp->sigmode == SPDK_MLX5_QP_SIG_LAST) {
		qp->ctrl->fm_ce_se &= ~SPDK_MLX5_WQE_CTRL_CE_MASK;
		qp->ctrl->fm_ce_se |= SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE;
		mlx5_qp_update_sq_comp(qp);
	}
	mlx5_ring_tx_db(qp, qp->ctrl);
}
