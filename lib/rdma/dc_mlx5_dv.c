/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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

#include <sys/queue.h>

#include <rdma/rdma_cma.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"

#include "spdk_internal/rdma.h"
#include "spdk/log.h"
#include "spdk/util.h"


#include "ibv_helper.h"

struct spdk_rdma_poller_context {};

struct spdk_dc_mlx5_dv_qp;

struct qp_list_entry {
	struct spdk_dc_mlx5_dv_qp *qp;
	CIRCLEQ_ENTRY(qp_list_entry) entries;
};

struct spdk_dc_mlx5_dv_poller_context {
	struct spdk_rdma_poller_context common;
	struct ibv_qp *qp_dci;
	struct ibv_qp_ex *qp_dci_qpex;
	struct mlx5dv_qp_ex *qp_dci_mqpex;

	struct ibv_qp *srq; /*FIXME now owning. Just pointer to SRQ*/
	struct ibv_qp *qp_dct;
	bool   activated;
	struct spdk_dc_mlx5_dv_qp *current_qp;

	uint32_t qpair_counter;

	CIRCLEQ_HEAD(, qp_list_entry) qps;
	struct qp_list_entry *current_qpe; /*FIXME join with *current_qp*/
	uint32_t registered_qp;
	uint32_t available_in_dci;
	uint32_t max_send_wr;
	uint32_t quota;
	bool send_started;
};

struct spdk_dc_mlx5_dv_qp {
	struct spdk_rdma_qp common;
	struct spdk_dc_mlx5_dv_poller_context *poller_ctx;
	uint32_t remote_dctn;
	uint32_t remote_qp_id;
	uint32_t assigned_id;
	uint64_t remote_dc_key;
	struct ibv_ah *ah;
	/* we don't expect concurent usage of spdk_dc_mlx5_dv_qp */
	__be32 dctn;
	struct ibv_send_wr *bad_wr;
	struct ibv_send_wr *not_sent_yet;
	uint32_t qpn_reservation;
	uint32_t wrs_sent;
};

#define DC_KEY 0xDC00DC00DC00DC00 /*FIXME ???*/
#define MAX_SEND_WR 1024 /*FIXME*/

static int 
dc_mlx5_dv_init_dci(struct spdk_dc_mlx5_dv_poller_context *poller_ctx, struct rdma_cm_id *cm_id)
{
	int attr_mask = 0;
	int rc = 0;
	struct ibv_qp *qp = poller_ctx->qp_dci;        

	/* modify QP to INIT */
	{
		attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = cm_id->port_num,
		};

/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask);*/
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
			return rc;
		}
	}

	/* modify QP to RTR */
	{
		struct ibv_port_attr port_attr = {0};
		uint8_t is_global = 0;
		uint8_t sgid_index = 0;
		
		attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV;
		if (ibv_query_port(cm_id->verbs, cm_id->port_num, &port_attr)) {
			SPDK_ERRLOG("ib_query_port\n");
			return -1;
		}
		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
			is_global = 1;
		        sgid_index = ibv_find_sgid_type(cm_id->verbs, cm_id->port_num, IBV_GID_TYPE_ROCE_V2, AF_INET); /* ??? PF_INET */
		}
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_RTR,
			.path_mtu               = port_attr.active_mtu,
			  .min_rnr_timer          = 0x10,
			  .rq_psn                 = 0,
			  .ah_attr                = {
				.is_global      = is_global, /* ??? */
				.sl             = 0,
				.src_path_bits  = 0,
				.port_num       = cm_id->port_num,
				.grh.hop_limit  = 1,
				.grh.sgid_index = sgid_index,
				.grh.traffic_class = 0,
			  }
		};
/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask);*/
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, rc %d\n", rc);
			return rc;
		}

	}

	/* modify QP to RTS */
	attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT |
		IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
		IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
	// Optional: IB_QP_MIN_RNR_TIMER

	struct ibv_qp_attr attr = {
		.qp_state               = IBV_QPS_RTS,		
		  .timeout                = 0x10,
		  .retry_cnt              = 3,
		  .rnr_retry              = 7,
		  .sq_psn                 = 0,
		  .max_rd_atomic          = 1,
	};


	/* rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask); */
	/* if (rc) { */
	/* 	SPDK_ERRLOG("Failed to init attr IBV_QPS_RTS, errno %s (%d)\n", spdk_strerror(errno), errno); */
	/* 	return rc; */
	/* } */

	rc = ibv_modify_qp(qp, &attr, attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTS) failed, rc %d\n", rc);
	}
	return rc;
}


static int 
dc_mlx5_dv_init_dct(struct spdk_dc_mlx5_dv_poller_context *poller_ctx, struct rdma_cm_id *cm_id)
{
	int attr_mask = 0;
	int rc = 0;
	struct ibv_qp *qp = poller_ctx->qp_dct;        

	/* modify QP to INIT */
	{
		attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = cm_id->port_num,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
		};

/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask); */
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
			return rc;
		}
	}

	/* modify QP to RTR */
	{
		attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV | IBV_QP_MIN_RNR_TIMER;
		struct ibv_port_attr port_attr = {0};
		uint8_t is_global = 0;
		uint8_t sgid_index = 0;
		
		if (ibv_query_port(cm_id->verbs, cm_id->port_num, &port_attr)) {
			SPDK_ERRLOG("ib_query_port\n");
			return -1;
		}
//		SPDK_NOTICELOG("port_attr: %
		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
			is_global = 1;
		        sgid_index = ibv_find_sgid_type(cm_id->verbs, cm_id->port_num, IBV_GID_TYPE_ROCE_V2, PF_INET); /* ??? PF_INET */
		}
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_RTR,
			.path_mtu               = port_attr.active_mtu,
			  .min_rnr_timer          = 0x10,
			  .rq_psn                 = 0,
			  .ah_attr                = {
				.is_global      = is_global, /* ??? */
				.sl             = 0,
				.src_path_bits  = 0,
				.port_num       = cm_id->port_num,
				.grh.hop_limit  = 1,
				.grh.sgid_index = sgid_index,
				.grh.traffic_class = 0,
			  }
		};

/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask);*/
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, errno %s (%d)\n", spdk_strerror(rc), rc);
			return rc;
		}

	}
	return rc;
}

static int
dc_mlx5_dv_init_qpairs(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc = 0;

	rc = dc_mlx5_dv_init_dci(qp->poller_ctx, qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("Failed to init dci\n");
		goto exit;
	}
	rc = dc_mlx5_dv_init_dct(qp->poller_ctx, qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("Failed to init dct\n");
		goto exit; /*FIXME destroy DCI first*/
	}

exit:
	return rc;
}

static struct spdk_dc_mlx5_dv_poller_context *
spdk_dc_mlx5_dv_create_poller_context(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	assert(cm_id);
	assert(qp_attr);

	struct spdk_dc_mlx5_dv_poller_context *poller_ctx = NULL;

	char *wr_str = getenv("MAX_SEND_WR");
	/* create DCT */

	struct ibv_qp_init_attr_ex attr_ex = {
		.qp_type = IBV_QPT_DRIVER,
		.send_cq = qp_attr->send_cq,
		.recv_cq = qp_attr->recv_cq,
		.comp_mask = IBV_QP_INIT_ATTR_PD,
/*		.pd = qp_attr->pd,*/
		.pd = qp_attr->pd ? qp_attr->pd : cm_id->pd,
		.srq = qp_attr->srq
	};
	struct mlx5dv_qp_init_attr attr_dv = {
		.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_DC,
		.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCT,
		.dc_init_attr.dct_access_key = DC_KEY,
	};
	poller_ctx = calloc(1, sizeof(*poller_ctx));
	if (!poller_ctx) {
		SPDK_ERRLOG("poller context memory allocation failed\n");
		goto exit_ok;
	}

	poller_ctx->max_send_wr = wr_str ? strtoul(wr_str, NULL, 0) : MAX_SEND_WR;

	poller_ctx->qp_dct = mlx5dv_create_qp(cm_id->verbs, &attr_ex, &attr_dv);
	if (!poller_ctx->qp_dct) {
		SPDK_ERRLOG("DCT creation failed. errno %s (%d)\n", spdk_strerror(errno), errno);
		goto exit_free_qps;
	}
	
	/* create DCI */
	attr_ex.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
	attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_READ;
	attr_ex.cap.max_send_wr = poller_ctx->max_send_wr;
	attr_ex.cap.max_send_sge = 59; /*FIXME  must be configured in runtime */
	attr_ex.srq = NULL;

	attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
	attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCI;
	attr_dv.create_flags |= MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE; /*driver doesnt support scatter2cqe data-path on DCI yet*/

	poller_ctx->qp_dci = mlx5dv_create_qp(cm_id->verbs, &attr_ex, &attr_dv);

	if (!poller_ctx->qp_dci) {
		SPDK_ERRLOG("DCI creation failed. errno %s (%d)\n", spdk_strerror(errno), errno);
		goto exit_destroy_dct;
	}
	poller_ctx->qp_dci_qpex = ibv_qp_to_qp_ex(poller_ctx->qp_dci);
	if (!poller_ctx->qp_dci_qpex) {
		SPDK_ERRLOG("ibv_qp_to_qp_ex(DC). errno %s (%d)\n", spdk_strerror(errno), errno);
		goto exit_destroy_dci;
	}
	poller_ctx->qp_dci_mqpex = mlx5dv_qp_ex_from_ibv_qp_ex(poller_ctx->qp_dci_qpex);
	if (!poller_ctx->qp_dci_mqpex) {
		SPDK_ERRLOG("mlx5dv_qp_ex_from_ibv_qp_ex(DC). errno %s (%d)\n", spdk_strerror(errno), errno);
	}
	
	CIRCLEQ_INIT(&poller_ctx->qps);
	poller_ctx->available_in_dci = poller_ctx->max_send_wr;

	goto exit_ok;
exit_destroy_dci:
	/*FIXME destroy DCI*/
exit_destroy_dct:
	/*FIXME destroy DCT*/
exit_free_qps:
	free(poller_ctx);
	poller_ctx=NULL;
exit_ok:
	return poller_ctx; 
}

static
struct spdk_dc_mlx5_dv_qp * 
spdk_dc_mlx5_dv_create_qp(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	struct spdk_dc_mlx5_dv_qp *qp = NULL;

	qp = calloc(1, sizeof(*qp));
	if (!qp) {
		SPDK_ERRLOG("spdk_rdma_qp memory allocation failed\n");
	}
	qp->common.cm_id = cm_id; /* FIXME check if it is needed */
	qp->remote_dc_key = DC_KEY; /*FIXME*/

	return qp;
}

static uint32_t
spdk_dc_generate_qpair_id(struct spdk_dc_mlx5_dv_qp *qp) {
	qp->remote_qp_id = qp->poller_ctx->qpair_counter++;
	return qp->remote_qp_id;
}

static void
spdk_dc_qp_assign_id(struct spdk_dc_mlx5_dv_qp *qp, uint32_t assigned_id) {
	qp->assigned_id = assigned_id;
}

static
int
spdk_dc_qp_accept(struct spdk_dc_mlx5_dv_qp *qp, struct rdma_conn_param *conn_param)
{
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;
	struct ibv_qp_attr ah_qp_attr = {0};
	int ah_qp_attr_mask = 0;
	int rc = 0;

	assert(qp != NULL);
	assert(qp->common.cm_id != NULL);
	assert(qp->poller_ctx != NULL);

	ah_qp_attr.qp_state = IBV_QPS_RTR;
	rc = rdma_init_qp_attr(qp->common.cm_id, &ah_qp_attr, &ah_qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("rdma_init_qp_attr: %s\n",strerror(errno));
		return NULL;
	}
	/*FIXME it doesn't seem that ah_attr consists with such one in modify_qp part*/
	qp->ah = ibv_create_ah(qp->common.cm_id->pd, &ah_qp_attr.ah_attr);
	if (!qp->ah) {
		SPDK_ERRLOG("ibv_create_ah: %s\n", strerror(errno));
		return NULL;
	}

	accept_data = conn_param->private_data;
	accept_data->dctn = qp->poller_ctx->qp_dct->qp_num;
	accept_data->assigned_id = spdk_dc_generate_qpair_id(qp);
	
	/* NVMEoF target must move qpair to RTS state */
	/* if (dc_mlx5_dv_init_qpairs(qp) != 0) { */
	/* 	SPDK_ERRLOG("Failed to initialize qpairs\n"); */
	/* 	/\* Set errno to be compliant with rdma_accept behaviour *\/ */
	/* 	errno = ECONNABORTED; */
	/* 	return -1; */
	/* } */

	return rdma_accept(qp->common.cm_id, conn_param);
}

static
int
spdk_dc_qp_complete_connect(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc;

	assert(qp);

	struct ibv_qp_attr qp_attr;
	int qp_attr_mask;

	/* rc = dc_mlx5_dv_init_qpairs(qp); */
	
	/* if (rc) { */
	/* 	SPDK_ERRLOG("Failed to initialize qpair\n"); */
	/* 	return rc; */
	/* } */

	qp_attr.qp_state = IBV_QPS_RTR;
	rc = rdma_init_qp_attr(qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("rdma_init_qp_attr");
		return rc;
	}
	
	qp->ah = ibv_create_ah(qp->poller_ctx->qp_dci->pd, &qp_attr.ah_attr);
	if (!qp->ah) {
		SPDK_ERRLOG("ibv_create_ah");
		rc = errno;
		return rc;
	}

	rc = rdma_establish(qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("rdma_establish failed, errno %s (%d)\n", spdk_strerror(errno), errno);
	}
	return rc;
}

static void  spdk_dc_poller_context_enqueqe_qp(struct spdk_dc_mlx5_dv_poller_context *poller_ctx,
					       struct spdk_dc_mlx5_dv_qp *spdk_rdma_qp)
{
	struct qp_list_entry *qpe;
	assert(poller_ctx);
	assert(spdk_rdma_qp);

	qpe = malloc(sizeof(struct qp_list_entry));
	qpe->qp = spdk_rdma_qp;
	CIRCLEQ_INSERT_TAIL(&poller_ctx->qps, qpe, entries);
	poller_ctx->registered_qp++;
	poller_ctx->quota = poller_ctx->max_send_wr / poller_ctx->registered_qp;
	if (poller_ctx->registered_qp == 1) {
		poller_ctx->current_qpe = qpe;
	}
}

static void  spdk_dc_poller_context_dequeqe_qp(struct spdk_dc_mlx5_dv_poller_context *poller_ctx,
					       struct spdk_dc_mlx5_dv_qp *spdk_rdma_qp)
{
	struct qp_list_entry *qpe;
	assert(poller_ctx);
	assert(spdk_rdma_qp);

	if (poller_ctx->current_qpe->qp == spdk_rdma_qp) {
		poller_ctx->current_qpe = CIRCLEQ_LOOP_NEXT(&poller_ctx->qps,
							    poller_ctx->current_qpe,
							    entries);
		if (poller_ctx->current_qpe->qp == spdk_rdma_qp) {
			poller_ctx->current_qpe == NULL;
		}
	}
	CIRCLEQ_FOREACH(qpe, &poller_ctx->qps, entries) {
		if (qpe->qp == spdk_rdma_qp) {
			poller_ctx->registered_qp--;
			poller_ctx->quota = poller_ctx->max_send_wr / (poller_ctx->registered_qp ? : 1);
			CIRCLEQ_REMOVE(&poller_ctx->qps, qpe, entries);
			break;
		}
	}
}

static
void
spdk_dc_poller_context_destroy(struct spdk_dc_mlx5_dv_poller_context *poller_ctx)
{
	int rc;
	struct qp_list_entry *qpe;
	struct qp_list_entry *qpe_tmp;

	assert(poller_ctx != NULL);

	if (poller_ctx->qp_dci) {
		rc = ibv_destroy_qp(poller_ctx->qp_dci);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy DCI ibv qp %p, rc %d\n", poller_ctx->qp_dci, rc);
		}
	}

	if (poller_ctx->qp_dct) {
		rc = ibv_destroy_qp(poller_ctx->qp_dct);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy DCT ibv qp %p, rc %d\n", poller_ctx->qp_dct, rc);
		}
	}
	/* empty qp's list from poller context if any */
	/* FIXME make sure that it is already empty at this moment */
	qpe = CIRCLEQ_FIRST(&poller_ctx->qps);
	while (qpe != (void *)&poller_ctx->qps) {
		qpe_tmp = CIRCLEQ_NEXT(qpe, entries);
		free(qpe);
		qpe = qpe_tmp;
	}
	free(poller_ctx);
}

static
void
spdk_dc_qp_destroy(struct spdk_dc_mlx5_dv_qp *qp)
{
	assert(qp != NULL);

	if (qp->common.send_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (qp->bad_wr != NULL) {
		SPDK_WARNLOG("Destroying qpair with non-processed bad WR\n");
	}

	if (qp->poller_ctx->current_qp == qp) {
		qp->poller_ctx->current_qp = NULL;
	}

	spdk_dc_poller_context_dequeqe_qp(qp->poller_ctx, qp);

	free(qp);
}

static int
spdk_dc_qp_to_err_state(struct ibv_qp *qp)
{
	int rc = 0;
	struct ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};
	assert(qp != NULL);
	rc = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE);
	if (rc) {
		SPDK_ERRLOG("Failed to modify ibv qp %p state to ERR, rc %d\n", qp, rc);
	}
	return rc;
}

static
int
spdk_dc_poller_context_disconnect(struct spdk_dc_mlx5_dv_poller_context *poller_ctx)
{
	int rc = 0;
	assert(poller_ctx != NULL);
	if (poller_ctx->qp_dci) {
		rc = spdk_dc_qp_to_err_state(poller_ctx->qp_dci);
	}

	if (poller_ctx->qp_dct) {
		rc = spdk_dc_qp_to_err_state(poller_ctx->qp_dct);
	}
	return rc;
}

static void spdk_dc_qp_reset_dci(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc = 0;
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_RESET
	};

	rc = ibv_modify_qp(qp->poller_ctx->qp_dci, &attr, IBV_QP_STATE);
	if (rc) {
		SPDK_ERRLOG("Failed to reset dci\n");
		goto exit;
	}

	rc = dc_mlx5_dv_init_dci(qp->poller_ctx, qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("Failed to reinit dci\n");
		goto exit;
	}
exit:
	return rc;
}

static
int
spdk_dc_qp_disconnect(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc = 0;

	assert(qp != NULL);
	/*FIXME call poller_context_disconnect if it really needed. IN_USE counter? */

	if (qp->common.cm_id) {
		rc = rdma_disconnect(qp->common.cm_id);
		if (rc) {
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
		rc = mlx5dv_reserved_qpn_dealloc(qp->common.cm_id->verbs,  qp->qpn_reservation);
		if (rc) {
			SPDK_ERRLOG("mlx5dv_reserved_qpn_dealloc failed\n");
		}
	}
	return rc;
}

static
int
spdk_dc_qp_flush_send_wrs(struct spdk_dc_mlx5_dv_qp *qp, struct ibv_send_wr **bad_wr);


static
bool
spdk_dc_qp_queue_prepare_send_wrs(struct spdk_dc_mlx5_dv_qp *qp, struct ibv_send_wr *first)
{
	struct ibv_send_wr *last;
	bool rc;

	assert(qp);
	assert(qp->poller_ctx);
	assert(first);

	last = first;
	while (last->next != NULL) {
		last = last->next;
	}

	if (qp->common.send_wrs.first == NULL && qp->not_sent_yet == NULL) {
		qp->common.send_wrs.first = first;
		qp->not_sent_yet = first;
		qp->common.send_wrs.last = last;
		rc = true;
	} else if (qp->common.send_wrs.first != NULL && qp->not_sent_yet != NULL) {
		qp->common.send_wrs.last->next = first;
		qp->common.send_wrs.last = last;
		rc = false;
	} else if (qp->common.send_wrs.first == NULL && qp->not_sent_yet != NULL) {
		qp->common.send_wrs.last->next = first;
		qp->common.send_wrs.last = last;
		qp->common.send_wrs.first = qp->not_sent_yet;
		rc = false;
	} else /*(qp->common.send_wrs.first != NULL && qp->not_sent_yet == NULL)*/ {
		qp->not_sent_yet = first;
		qp->common.send_wrs.last = last;
		/*?????*/
		rc = false;
	}
	return rc;
}

static
uint32_t
spdk_dc_qp_queue_send_wrs(struct spdk_dc_mlx5_dv_qp *qp)
{
	struct ibv_send_wr *tmp;
	struct spdk_dc_mlx5_dv_poller_context *poller_ctx;
	assert(qp);
	assert(qp->poller_ctx);
	poller_ctx = qp->poller_ctx;

	if (!poller_ctx->send_started) {
		ibv_wr_start(qp->poller_ctx->qp_dci_qpex);
		poller_ctx->send_started = 1;
	}

	for (tmp = qp->not_sent_yet;
	     tmp != NULL && poller_ctx->available_in_dci !=0 && qp->wrs_sent < poller_ctx->quota;
	     tmp = tmp->next, poller_ctx->available_in_dci--, qp->wrs_sent++) {

		poller_ctx->qp_dci_qpex->wr_id = tmp->wr_id;
		poller_ctx->qp_dci_qpex->wr_flags = tmp->send_flags;
		switch (tmp->opcode) {
		case IBV_WR_SEND:
			ibv_wr_send_imm(poller_ctx->qp_dci_qpex, qp->assigned_id);
			break;
		/* case IBV_WR_SEND_WITH_INV: */
		/* 	SPDK_NOTICELOG("Impossible!!!\n"); */
		/* 	ibv_wr_send_inv(poller_ctx->qp_dci_qpex, tmp->invalidate_rkey); */
		/* 	break; */
		case IBV_WR_RDMA_READ:
			ibv_wr_rdma_read(poller_ctx->qp_dci_qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		case IBV_WR_RDMA_WRITE:
			ibv_wr_rdma_write(poller_ctx->qp_dci_qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		default:
			SPDK_ERRLOG("Unexpected opcode %d\n", tmp->opcode);
			assert(0);
		}

		mlx5dv_wr_set_dc_addr(poller_ctx->qp_dci_mqpex,
				      qp->ah, qp->remote_dctn,
				      qp->remote_dc_key);
		ibv_wr_set_sge_list(poller_ctx->qp_dci_qpex, tmp->num_sge, tmp->sg_list);
		qp->not_sent_yet = tmp->next;
//		SPDK_NOTICELOG("qp->not_sent_yet now is %p on qp: %p\n",qp->not_sent_yet, qp);
		//qp->common.send_wrs.last = tmp; /*FIXME strange place */
	}
exit:
	return qp->wrs_sent;
}

static uint32_t spdk_dc_poller_context_submit_wrs(struct spdk_dc_mlx5_dv_poller_context *poller_ctx)
{
	assert(poller_ctx->current_qpe);
	uint32_t registered_qp = poller_ctx->registered_qp;
	uint32_t wrs_sent = 0;
	while (registered_qp-- && poller_ctx->available_in_dci && wrs_sent == 0) {
		struct spdk_dc_mlx5_dv_qp *qp = poller_ctx->current_qpe->qp;
		if (qp->not_sent_yet) {
			wrs_sent = spdk_dc_qp_queue_send_wrs(qp);
			/* TODO check if we can reduce latency by scheduling the next qp at the moment */
		}
		poller_ctx->current_qpe = CIRCLEQ_LOOP_NEXT(&poller_ctx->qps,
							    poller_ctx->current_qpe,
							    entries);
	}
exit:
	return wrs_sent;
}


static
int
spdk_dc_qp_flush_send_wrs(struct spdk_dc_mlx5_dv_qp *qp, struct ibv_send_wr **bad_wr)
{
	int rc;

	assert(bad_wr);
	assert(qp);
	assert(qp->poller_ctx);

	if (spdk_unlikely(qp->poller_ctx->send_started == 0)) {
		rc = 0;
	} else {
		rc =  ibv_wr_complete(qp->poller_ctx->qp_dci_qpex);
//		SPDK_NOTICELOG("ibv_wr_complete called on qp: %p\n", qp);
		qp->poller_ctx->send_started = 0;
		if (spdk_unlikely(rc)) {
			/* If ibv_wr_complete reports an error that means that no WRs are posted to NIC */
			*bad_wr = qp->common.send_wrs.first;
		}
	}
	qp->common.send_wrs.first = NULL;
	return rc;
}

int spdk_rdma_qp_get_qpn_reservation(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t *qpn_reservation)
{
	int ret;
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);

	ret = mlx5dv_reserved_qpn_alloc(qp->common.cm_id->verbs, qpn_reservation);
	if (ret) {
		SPDK_ERRLOG("mlx5dv_reserved_qpn_alloc failed\n");
	} else {
		qp->qpn_reservation = *qpn_reservation;
	}
	return ret;
}

struct spdk_rdma_qp *spdk_rdma_qp_create(struct rdma_cm_id *cm_id,
					 struct spdk_rdma_qp_init_attr *qp_attr)
{
	return &spdk_dc_mlx5_dv_create_qp(cm_id, qp_attr)->common;
}

int spdk_rdma_qp_accept(struct spdk_rdma_qp *spdk_rdma_qp, struct rdma_conn_param *conn_param)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_accept(qp, conn_param);
}

int spdk_rdma_qp_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_complete_connect(qp);
}

void spdk_rdma_qp_destroy(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_destroy(qp);
}

int spdk_rdma_qp_disconnect(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_disconnect(qp);
}

bool spdk_rdma_qp_queue_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr *first) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	uint32_t sent;
	spdk_dc_qp_queue_prepare_send_wrs(qp, first);
	sent = spdk_dc_poller_context_submit_wrs(qp->poller_ctx);
//	SPDK_NOTICELOG("really submitted to qp: %p - %d\n", qp, sent);
	return true;
}

int spdk_rdma_qp_flush_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr **bad_wr) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_flush_send_wrs(qp, bad_wr);
}

struct spdk_rdma_poller_context *
spdk_rdma_create_poller_context(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	return &spdk_dc_mlx5_dv_create_poller_context(cm_id, qp_attr)->common;
}

int spdk_rdma_qp_set_poller_context(struct spdk_rdma_qp *spdk_rdma_qp,
				    struct spdk_rdma_poller_context *poller_ctx)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	if (qp->poller_ctx) {
		SPDK_NOTICELOG("WARNING resetting poller_ctx. old=%p new=%p\n", qp->poller_ctx, poller_ctx);
	}
	qp->poller_ctx = SPDK_CONTAINEROF(poller_ctx, struct spdk_dc_mlx5_dv_poller_context, common);
	if (!qp->poller_ctx->activated && (dc_mlx5_dv_init_qpairs(qp) != 0)) {
		SPDK_ERRLOG("Failed to initialize qpairs\n");
		/* Set errno to be compliant with rdma_accept behaviour */
		errno = ECONNABORTED;
		return -1;
	}
	spdk_dc_poller_context_enqueqe_qp(poller_ctx, qp);
	qp->poller_ctx->activated = true;
	return 0;
}

uint32_t spdk_rdma_send_qp_num(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dci->qp_num;
}

uint32_t spdk_rdma_recv_qp_num(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct->qp_num;
}


struct ibv_pd *
spdk_rdma_qp_pd(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct->pd;
}

int spdk_rdma_query_qp_dci(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
			   enum ibv_qp_attr_mask attr_mask,
			   struct ibv_qp_init_attr *init_attr)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return ibv_query_qp(qp->poller_ctx->qp_dci, attr, attr_mask, init_attr);
}

int spdk_rdma_query_qp_dct(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
			   enum ibv_qp_attr_mask attr_mask,
			   struct ibv_qp_init_attr *init_attr)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return ibv_query_qp(qp->poller_ctx->qp_dct, attr, attr_mask, init_attr);
}

struct ibv_qp *
spdk_rdma_receive_qp(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct;
}

struct ibv_qp *
spdk_rdma_send_qp(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dci;
}

void spdk_rdma_qp_set_remote_dctn(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dctn) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	qp->remote_dctn = dctn;      
}

uint32_t spdk_rdma_qp_get_local_dctn(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct->qp_num;
}

bool spdk_rdma_is_corresponded_qp(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_wc *wc) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->remote_qp_id == wc->imm_data;
}

uint32_t spdk_rdma_generate_qpair_id(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_generate_qpair_id(qp);
}

void spdk_rdma_qp_assign_id(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t assigned_id)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	spdk_dc_qp_assign_id(qp, assigned_id);
}

void spdk_rdma_qp_reset(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	/* SPDK_NOTICELOG("Reset called for qp: %p\n", qp); */ /* TODO clarify if it should be called so ofter */
	spdk_dc_qp_reset_dci(qp);
	return;
}

void spdk_rdma_notify_qp_on_send_completion(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t wrs_released) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	qp->poller_ctx->available_in_dci += wrs_released;
	qp->wrs_sent = 0;
	spdk_dc_poller_context_submit_wrs(qp->poller_ctx);
}

