/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "mlx5_priv.h"
#include "infiniband/mlx5dv.h"
#include "mlx5_ifc.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk_internal/rdma_utils.h"

#define MLX5_QP_RQ_PSN              0x4242
#define MLX5_QP_MAX_DEST_RD_ATOMIC      16
#define MLX5_QP_RNR_TIMER               12
#define MLX5_QP_HOP_LIMIT               64
#define MLX5_QP_GID_INDEX                0

/* RTS state params */
#define MLX5_QP_TIMEOUT            14
#define MLX5_QP_RETRY_COUNT         7
#define MLX5_QP_RNR_RETRY           7
#define MLX5_QP_MAX_RD_ATOMIC      16
#define MLX5_QP_SQ_PSN         0x4242

struct mlx5_qp_conn_caps {
	bool resources_on_nvme_emulation_manager;
	bool roce_enabled;
	uint8_t roce_version;
	bool fl_when_roce_disabled;
	bool fl_when_roce_enabled;
	uint16_t r_roce_max_src_udp_port;
	uint16_t r_roce_min_src_udp_port;
	uint8_t port;
	uint16_t pkey_idx;
	enum ibv_mtu mtu;
};

static int mlx5_qp_connect(struct spdk_mlx5_qp *qp);

static void
mlx5_cq_deinit(struct spdk_mlx5_cq *cq)
{
	if (cq->verbs_cq) {
		ibv_destroy_cq(cq->verbs_cq);
	}
}

static int
mlx5_cq_init(struct ibv_pd* pd, const struct spdk_mlx5_cq_attr *attr, struct spdk_mlx5_dma_qp *dma_qp)
{
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = attr->cqe_cnt,
		.cq_context = attr->cq_context,
		.channel = attr->comp_channel,
		.comp_vector = attr->comp_vector,
		.wc_flags = IBV_WC_STANDARD_FLAGS,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_IGNORE_OVERRUN
	};
	struct mlx5dv_cq_init_attr cq_ex_attr = {
		.comp_mask = MLX5DV_CQ_INIT_ATTR_MASK_CQE_SIZE,
		.cqe_size = attr->cqe_size
	};
	struct mlx5dv_obj dv_obj;
	struct mlx5dv_cq mlx5_cq;
	struct ibv_cq_ex *cq_ex;
	struct spdk_mlx5_cq *cq = &dma_qp->cq;
	int rc;

	if (!cq) {
		return -ENOMEM;
	}

	cq_ex = mlx5dv_create_cq(pd->context, &cq_attr, &cq_ex_attr);
	if (!cq_ex) {
		rc = -errno;
		SPDK_ERRLOG("mlx5dv_create_cq failed, errno %d\n", rc);
		return rc;
	}

	cq->verbs_cq = ibv_cq_ex_to_cq(cq_ex);
	assert(cq->verbs_cq);

	dv_obj.cq.in = cq->verbs_cq;
	dv_obj.cq.out = &mlx5_cq;

	/* Init CQ - CQ is marked as owned by DV for all consumer index related actions */
	rc = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_CQ);
	if (rc) {
		SPDK_ERRLOG("Failed to init DV CQ, rc %d\n", rc);
		ibv_destroy_cq(cq->verbs_cq);
		free(cq);
		return rc;
	}

	cq->hw.cq_addr = (uintptr_t)mlx5_cq.buf;
	cq->hw.ci = 0;
	cq->hw.cqe_cnt = mlx5_cq.cqe_cnt;
	cq->hw.cqe_size = mlx5_cq.cqe_size;
	cq->hw.cq_num = mlx5_cq.cqn;

	return 0;
}

static void
mlx5_qp_destroy(struct spdk_mlx5_qp *qp)
{
	if (qp->verbs_qp) {
		ibv_destroy_qp(qp->verbs_qp);
	}
	if (qp->completions) {
		free(qp->completions);
	}
}

static int
mlx5_qp_init(struct ibv_pd *pd, const struct spdk_mlx5_qp_attr *attr, struct ibv_cq *cq, struct spdk_mlx5_qp *qp)
{
	struct mlx5dv_qp dv_qp;
	struct mlx5dv_obj dv_obj;
	struct spdk_mlx5_crypto_caps crypto_caps = {};
	struct ibv_qp_init_attr_ex dv_qp_attr = {
		.cap = attr->cap,
		.qp_type = IBV_QPT_RC,
		.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
		.pd = pd,
		.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE |  IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_READ | IBV_QP_EX_WITH_BIND_MW,
		.send_cq = cq,
		.recv_cq = cq,
		.sq_sig_all = attr->sigall,
	};
	/* Attrs required for MKEYs registration */
	struct mlx5dv_qp_init_attr mlx5_qp_attr = {
		.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS,
		.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE
	};
	int rc;

	rc = spdk_mlx5_query_crypto_caps(pd->context, &crypto_caps);
	if (rc) {
		SPDK_ERRLOG("Failed to query dev %s crypto caps\n", pd->context->device->name);
		return rc;
	}

	qp->verbs_qp = mlx5dv_create_qp(pd->context, &dv_qp_attr, &mlx5_qp_attr);
	if (!qp->verbs_qp) {
		rc = -errno;
		SPDK_ERRLOG("Failed to create qp, rc %d\n", rc);
		return rc;
	}

	dv_obj.qp.in = qp->verbs_qp;
	dv_obj.qp.out = &dv_qp;

	rc = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP);
	if (rc) {
		SPDK_ERRLOG("Failed to init DV QP, rc %d\n", rc);
		return rc;
	}

	qp->hw.sq_addr = (uint64_t)dv_qp.sq.buf;
	qp->hw.dbr_addr = (uint64_t)dv_qp.dbrec;
	qp->hw.sq_bf_addr = (uint64_t)dv_qp.bf.reg;
	qp->hw.sq_wqe_cnt = dv_qp.sq.wqe_cnt;

	SPDK_NOTICELOG("mlx5 QP, sq size %u WQE_BB. %u send_wrs -> %u WQE_BB per send WR\n",
		       qp->hw.sq_wqe_cnt, attr->cap.max_send_wr, qp->hw.sq_wqe_cnt / attr->cap.max_send_wr);

	qp->hw.qp_num = qp->verbs_qp->qp_num;

	/*
	 * Verify that the memory is indeed non-cachable. It relies on a fact (hack) that
	 * rdma-core is going to allocate NC uar if blue flame is disabled.
	 * This is a short term solution.
	 *
	 * The right solution is to allocate uars explicitly with the
	 * mlx5dv_devx_alloc_uar()
	 */
	qp->hw.sq_tx_db_nc = dv_qp.bf.size == 0;
	qp->tx_available = qp->hw.sq_wqe_cnt;
	qp->max_sge = attr->cap.max_send_sge;
	qp->aes_xts_inc_64 = crypto_caps.tweak_inc_64;
	/* We have only mode BE mode, if it is not set then tweak is LE */
	qp->aes_xts_tweak_be = crypto_caps.multi_block_be_tweak;
	rc = posix_memalign((void **)&qp->completions, 4096, qp->hw.sq_wqe_cnt * sizeof(*qp->completions));
	if (rc) {
		SPDK_ERRLOG("Failed to alloc completions\n");
		return rc;
	}
	if (attr->sigall) {
		qp->tx_flags |= MLX5_WQE_CTRL_CQ_UPDATE;
	}
	qp->tx_revert_flags = (uint16_t)-1;
	if (attr->siglast) {
		qp->tx_revert_flags &= ~MLX5_WQE_CTRL_CQ_UPDATE;
	}

	rc = mlx5_qp_connect(qp);
	if (rc) {
		return rc;
	}

	return 0;
}

static int
mlx5_qp_get_port_pkey_idx(struct spdk_mlx5_qp *qp, uint8_t *port, uint16_t *pkey_idx)
{
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr init_attr = {};
	int attr_mask = IBV_QP_PKEY_INDEX |
			IBV_QP_PORT;
	int rc;

	rc = ibv_query_qp(qp->verbs_qp, &attr, attr_mask, &init_attr);
	if (rc) {
		SPDK_ERRLOG("Failed to query qp %p %u\n", qp, qp->hw.qp_num);
		return rc;
	}
	*port = attr.port_num;
	*pkey_idx = attr.pkey_index;

	return 0;
}

static int
mlx5_check_port(struct ibv_context *ctx, int port_num, bool *roce_en,
		bool *ib_en, enum ibv_mtu *mtu)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_nic_vport_context_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_nic_vport_context_out)] = {0};
	uint8_t devx_v;
	struct ibv_port_attr port_attr = {};
	int rc;

	*roce_en = false;
	*ib_en = false;

	rc = ibv_query_port(ctx, port_num, &port_attr);
	if (rc) {
		return rc;
	}

	if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
		/* we only support local IB addressing for now */
		if (port_attr.flags & IBV_QPF_GRH_REQUIRED) {
			SPDK_ERRLOG("IB enabled and GRH addressing is required but only local addressing is supported\n");
			return -1;
		}
		*mtu = port_attr.active_mtu;
		*ib_en = true;
		return 0;
	}

	if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
		return -1;
	}

	/* port may be ethernet but still have roce disabled */
	DEVX_SET(query_nic_vport_context_in, in, opcode,
		 MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
	rc = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out,
				     sizeof(out));
	if (rc) {
		SPDK_ERRLOG("Failed to get VPORT context - assuming ROCE is disabled\n");
		return rc;
	}
	devx_v = DEVX_GET(query_nic_vport_context_out, out,
			  nic_vport_context.roce_en);
	if (devx_v) {
		*roce_en = true;
	}

	/* Always use 4K MTU */
	*mtu = IBV_MTU_4096;

	return 0;
}

static int
mlx5_fill_qp_conn_caps(struct ibv_context *context,
		       struct mlx5_qp_conn_caps *conn_caps)
{

	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		return ret;
	}

	conn_caps->resources_on_nvme_emulation_manager =
		DEVX_GET(query_hca_cap_out, out,
			 capability.cmd_hca_cap.resources_on_nvme_emulation_manager);
	conn_caps->fl_when_roce_disabled = DEVX_GET(query_hca_cap_out, out,
					   capability.cmd_hca_cap.fl_rc_qp_when_roce_disabled);
	conn_caps->roce_enabled = DEVX_GET(query_hca_cap_out, out,
					   capability.cmd_hca_cap.roce);
	if (!conn_caps->roce_enabled) {
		goto out;
	}

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_SET_HCA_CAP_OP_MOD_ROCE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		return ret;
	}

	conn_caps->roce_version = DEVX_GET(query_hca_cap_out, out,
					   capability.roce_cap.roce_version);
	conn_caps->fl_when_roce_enabled = DEVX_GET(query_hca_cap_out,
					  out, capability.roce_cap.fl_rc_qp_when_roce_enabled);
	conn_caps->r_roce_max_src_udp_port = DEVX_GET(query_hca_cap_out,
					     out, capability.roce_cap.r_roce_max_src_udp_port);
	conn_caps->r_roce_min_src_udp_port = DEVX_GET(query_hca_cap_out,
					     out, capability.roce_cap.r_roce_min_src_udp_port);
out:
	SPDK_DEBUGLOG(mlx5, "RoCE Caps: enabled %d ver %d fl allowed %d\n",
		       conn_caps->roce_enabled, conn_caps->roce_version,
		       conn_caps->roce_enabled ? conn_caps->fl_when_roce_enabled :
		       conn_caps->fl_when_roce_disabled);
	return 0;
}

static int
mlx5_qp_loopback_conn_rts_2_init(struct spdk_mlx5_qp *qp, struct ibv_qp_attr *qp_attr,
				 int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rst2init_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rst2init_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);
	int rc;

	DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
	DEVX_SET(rst2init_qp_in, in, qpn, qp->hw.qp_num);
	DEVX_SET(qpc, qpc, pm_state, MLX5_QP_PM_MIGRATED);

	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);

	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);

	if (attr_mask & IBV_QP_ACCESS_FLAGS) {
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_READ) {
			DEVX_SET(qpc, qpc, rre, 1);
		}
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_WRITE) {
			DEVX_SET(qpc, qpc, rwe, 1);
		}
	}

	rc = mlx5dv_devx_qp_modify(qp->verbs_qp, in, sizeof(in), out, sizeof(out));
	if (rc) {
		SPDK_ERRLOG("failed to modify qp to init, errno = %d\n", rc);
	}

	return rc;

}

static int
mlx5_qp_loopback_conn_init_2_rtr(struct spdk_mlx5_qp *qp, struct ibv_qp_attr *qp_attr,
				 int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(init2rtr_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(init2rtr_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);
	int rc;

	DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
	DEVX_SET(init2rtr_qp_in, in, qpn, qp->hw.qp_num);

	/* 30 is the maximum value for Infiniband QPs*/
	DEVX_SET(qpc, qpc, log_msg_max, 30);

	/* TODO: add more attributes */
	if (attr_mask & IBV_QP_PATH_MTU) {
		DEVX_SET(qpc, qpc, mtu, qp_attr->path_mtu);
	}
	if (attr_mask & IBV_QP_DEST_QPN) {
		DEVX_SET(qpc, qpc, remote_qpn, qp_attr->dest_qp_num);
	}
	if (attr_mask & IBV_QP_RQ_PSN) {
		DEVX_SET(qpc, qpc, next_rcv_psn, qp_attr->rq_psn & 0xffffff);
	}
	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);
	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);
	if (attr_mask & IBV_QP_MAX_DEST_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_rra_max,
			 spdk_u32log2(qp_attr->max_dest_rd_atomic));
	if (attr_mask & IBV_QP_MIN_RNR_TIMER) {
		DEVX_SET(qpc, qpc, min_rnr_nak, qp_attr->min_rnr_timer);
	}
	if (attr_mask & IBV_QP_AV) {
		DEVX_SET(qpc, qpc, primary_address_path.fl, 1);
	}

	rc = mlx5dv_devx_qp_modify(qp->verbs_qp, in, sizeof(in), out, sizeof(out));
	if (rc) {
		SPDK_ERRLOG("failed to modify qp to rtr with errno = %d\n", rc);
	}

	return rc;
}

static int
mlx5_qp_loopback_conn_rtr_2_rts(struct spdk_mlx5_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rtr2rts_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rtr2rts_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);
	int rc;

	DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
	DEVX_SET(rtr2rts_qp_in, in, qpn, qp->hw.qp_num);

	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_RETRY_CNT) {
		DEVX_SET(qpc, qpc, retry_count, qp_attr->retry_cnt);
	}
	if (attr_mask & IBV_QP_SQ_PSN) {
		DEVX_SET(qpc, qpc, next_send_psn, qp_attr->sq_psn & 0xffffff);
	}
	if (attr_mask & IBV_QP_RNR_RETRY) {
		DEVX_SET(qpc, qpc, rnr_retry, qp_attr->rnr_retry);
	}
	if (attr_mask & IBV_QP_MAX_QP_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_sra_max,
			 spdk_u32log2(qp_attr->max_rd_atomic));

	rc = mlx5dv_devx_qp_modify(qp->verbs_qp, in, sizeof(in), out, sizeof(out));
	if (rc) {
		SPDK_ERRLOG("failed to modify qp to rts with errno = %d\n", rc);
	}

	return rc;
}


static int
mlx5_qp_loopback_conn(struct spdk_mlx5_qp *qp, struct mlx5_qp_conn_caps *caps)
{
	struct ibv_qp_attr qp_attr = {};
	int rc, attr_mask = IBV_QP_STATE |
			    IBV_QP_PKEY_INDEX |
			    IBV_QP_PORT |
			    IBV_QP_ACCESS_FLAGS;

	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.pkey_index = caps->pkey_idx;
	qp_attr.port_num = caps->port;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

	rc = mlx5_qp_loopback_conn_rts_2_init(qp, &qp_attr, attr_mask);
	if (rc) {
		return rc;
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.dest_qp_num = qp->hw.qp_num;
	qp_attr.qp_state = IBV_QPS_RTR;
	qp_attr.path_mtu = caps->mtu;
	qp_attr.rq_psn = MLX5_QP_RQ_PSN;
	qp_attr.max_dest_rd_atomic = MLX5_QP_MAX_DEST_RD_ATOMIC;
	qp_attr.min_rnr_timer = MLX5_QP_RNR_TIMER;
	qp_attr.ah_attr.port_num = caps->port;
	qp_attr.ah_attr.grh.hop_limit = MLX5_QP_HOP_LIMIT;

	attr_mask = IBV_QP_STATE               |
		    IBV_QP_AV                 |
		    IBV_QP_PATH_MTU           |
		    IBV_QP_DEST_QPN           |
		    IBV_QP_RQ_PSN             |
		    IBV_QP_MAX_DEST_RD_ATOMIC |
		    IBV_QP_MIN_RNR_TIMER;

	rc = mlx5_qp_loopback_conn_init_2_rtr(qp, &qp_attr, attr_mask);
	if (rc) {
		return rc;
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTS;
	qp_attr.timeout = MLX5_QP_TIMEOUT;
	qp_attr.retry_cnt = MLX5_QP_RETRY_COUNT;
	qp_attr.sq_psn = MLX5_QP_SQ_PSN;
	qp_attr.rnr_retry = MLX5_QP_RNR_RETRY;
	qp_attr.max_rd_atomic = MLX5_QP_MAX_RD_ATOMIC;
	attr_mask = IBV_QP_STATE               |
		    IBV_QP_TIMEOUT            |
		    IBV_QP_RETRY_CNT          |
		    IBV_QP_RNR_RETRY          |
		    IBV_QP_SQ_PSN             |
		    IBV_QP_MAX_QP_RD_ATOMIC;
	/* once QPs were moved to RTR using devx, they must also move to RTS
	 * using devx since kernel doesn't know QPs are on RTR state
	 **/
	rc = mlx5_qp_loopback_conn_rtr_2_rts(qp, &qp_attr, attr_mask);

	return rc;
}

static int
mlx5_qp_connect(struct spdk_mlx5_qp *qp)
{
	struct mlx5_qp_conn_caps conn_caps = {};
	struct ibv_context *context = qp->verbs_qp->context;
	bool roce_en, ib_en;
	uint8_t port;
	uint16_t pkey_idx;
	enum ibv_mtu mtu;
	int rc;

	rc = mlx5_qp_get_port_pkey_idx(qp, &port, &pkey_idx);
	if (rc) {
		return rc;
	}
	rc = mlx5_check_port(context, port, &roce_en, &ib_en, &mtu);
	if (rc) {
		return rc;
	}
	rc = mlx5_fill_qp_conn_caps(context, &conn_caps);
	if (rc) {
		return rc;
	}
	conn_caps.port = port;
	conn_caps.pkey_idx = pkey_idx;
	conn_caps.mtu = mtu;

	/* Check if force-loopback is supported */
	if (ib_en || (conn_caps.resources_on_nvme_emulation_manager &&
		      ((conn_caps.roce_enabled && conn_caps.fl_when_roce_enabled) ||
		       (!conn_caps.roce_enabled && conn_caps.fl_when_roce_disabled)))) {
	} else if (conn_caps.resources_on_nvme_emulation_manager) {
		SPDK_ERRLOG("Force-loopback QP is not supported. Cannot create queue.\n");
		return -ENOTSUP;
	}

	return mlx5_qp_loopback_conn(qp, &conn_caps);
}

void
spdk_mlx5_dma_qp_destroy(struct spdk_mlx5_dma_qp *dma_qp)
{
	mlx5_qp_destroy(&dma_qp->qp);
	mlx5_cq_deinit(&dma_qp->cq);
	free(dma_qp);
}

int
spdk_mlx5_dma_qp_create(struct ibv_pd *pd, struct spdk_mlx5_cq_attr *cq_attr,
			struct spdk_mlx5_qp_attr *qp_attr, void *context, struct spdk_mlx5_dma_qp **dma_qp_out)
{
	int rc;
	struct spdk_mlx5_dma_qp *dma_qp = calloc(1, sizeof(*dma_qp));

	if (!dma_qp) {
		return -ENOMEM;
	}

	rc = mlx5_cq_init(pd, cq_attr, dma_qp);
	if (rc) {
		spdk_mlx5_dma_qp_destroy(dma_qp);
		return rc;
	}
	rc = mlx5_qp_init(pd, qp_attr, dma_qp->cq.verbs_cq, &dma_qp->qp);
	if (rc) {
		spdk_mlx5_dma_qp_destroy(dma_qp);
		return rc;
	}

	*dma_qp_out = dma_qp;

	return 0;
}
