/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) Mellanox Technologies LTD. All rights reserved.
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

#ifndef SPDK_RDMA_H
#define SPDK_RDMA_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

struct spdk_rdma_qp_init_attr {
	void		       *qp_context;
	struct ibv_cq	       *send_cq;
	struct ibv_cq	       *recv_cq;
	struct ibv_srq	       *srq;
	struct ibv_qp_cap	cap;
	struct ibv_pd	       *pd;
	/* Determines whether a qpair is created on the active side (NVMEoF host)
	 * or on the passive side (NVMEoF target) */
	bool			active_side;
};

struct spdk_rdma_qp {
	struct ibv_qp *qp;
	struct rdma_cm_id *cm_id;
};

/**
 * Create RDMA provider specific qpair
 * \param cm_id Pointer to RDMACM cm_id
 * \param qp_attr Pointer to qpair init attributes
 * \return Pointer to a newly created qpair on success or NULL on failure
 */
struct spdk_rdma_qp *spdk_rdma_create_qp(struct rdma_cm_id *cm_id,
		struct spdk_rdma_qp_init_attr *qp_attr);

/**
 * Completes the connection process, must be called by the active
 * side (NVMEoF host) upon receipt RDMA_CM_EVENT_CONNECT_RESPONSE
 * @param spdk_rdma_qp pointer to a qpair
 * @return 0 on success, errno on failure
 */
int spdk_rdma_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Destory RDMA provider specific qpair
 * \param spdk_rdma_qp Pointer to qpair to be destroyed
 */
void spdk_rdma_destroy_qp(struct spdk_rdma_qp *spdk_rdma_qp);

#endif /* SPDK_RDMA_H */
