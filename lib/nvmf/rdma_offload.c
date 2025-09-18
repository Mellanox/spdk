/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <doca_ctx.h>
#include <doca_comch_producer.h>
#include <doca_sta.h>
#include <doca_sta_event.h>
#include <doca_sta_be.h>
#include <doca_sta_subsystem.h>
#include <doca_sta_mem.h>
#include <doca_sta_stats.h>
#include <doca_sta_io.h>
#include <doca_sta_io_qp.h>
#include <doca_sta_io_non_offload.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/tree.h"
#include "spdk/util.h"
#include "spdk/rpc.h"

#include "spdk_internal/assert.h"
#include "spdk/log.h"
#include "spdk_internal/rdma_provider.h"
#include "spdk_internal/rdma_utils.h"

#include "nvmf_internal.h"
#include "transport.h"
#include "../nvme/nvme_internal.h"
#include "../nvme/nvme_pcie_internal.h"

#include "spdk_internal/trace_defs.h"

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma_offload;

#define NVMF_RDMA_STA_DEFAULT_POLL_RATE_US 10000

/*
 RDMA Connection Resource Defaults
 */
#define NVMF_DEFAULT_MSDBD		16
#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES
#define NVMF_DEFAULT_RSP_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

#define NVMF_RDMA_MAX_EVENTS_PER_POLL	32

SPDK_STATIC_ASSERT(NVMF_DEFAULT_MSDBD <= SPDK_NVMF_MAX_SGL_ENTRIES,
		   "MSDBD must not exceed SPDK_NVMF_MAX_SGL_ENTRIES");

/* The RDMA completion queue size */
#define DEFAULT_NVMF_RDMA_CQ_SIZE	4096
#define MAX_WR_PER_QP(queue_depth)	(queue_depth * 3 + 2)

enum spdk_nvmf_rdma_request_state {
	/* The request is not currently in use */
	RDMA_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	RDMA_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	RDMA_REQUEST_STATE_NEED_BUFFER,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the host to the controller.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,

	/* The request is currently transferring data from the host to the controller. */
	RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	RDMA_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	RDMA_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	RDMA_REQUEST_STATE_EXECUTED,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the controller to the host.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,

	/* The request is waiting on RDMA queue depth availability
	 * to send response to the host.
	 */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING,

	/* The request is ready to send a completion */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request is currently transferring data from the controller to the host. */
	RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,

	/* The request currently has an outstanding completion without an
	 * associated data transfer.
	 */
	RDMA_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	RDMA_REQUEST_STATE_COMPLETED,

	/* Terminator */
	RDMA_REQUEST_NUM_STATES,
};

/* NVMe-of RDMA Offload tracepoint definitions */
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEW					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x0)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEED_BUFFER				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x1)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x2)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x3)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_EXECUTE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x4)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x5)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x6)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x7)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x8)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x9)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0xA)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0xB)
#define TRACE_RDMA_OFFLOAD_QP_CREATE						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0xC)
#define TRACE_RDMA_OFFLOAD_IBV_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0xD)
#define TRACE_RDMA_OFFLOAD_CM_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0xE)
#define TRACE_RDMA_OFFLOAD_QP_DISCONNECT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x10)
#define TRACE_RDMA_OFFLOAD_QP_DESTROY						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x11)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE_PENDING		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x12)
#define TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEED_ACCEL_TASK			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA_OFFLOAD, 0x13)

static void
nvmf_rdma_offload_trace(void)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_OFFLOAD_IO, 'r');

	struct spdk_trace_tpoint_opts opts[] = {
		{
			"RDMA_REQ_NEW", TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEW,
			OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 1,
			{
				{ "qpair", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
		{
			"RDMA_REQ_COMPLETED", TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETED,
			OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
			{
				{ "qpair", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEED_BUFFER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_C2H",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H2C",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_H2C",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_EXECUTING",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_EXECUTED",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTED,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY2COMPL_PEND",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPL",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETING_C2H",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETING",
					TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");

	spdk_trace_register_description("RDMA_QP_CREATE", TRACE_RDMA_OFFLOAD_QP_CREATE,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("RDMA_IBV_ASYNC_EVENT", TRACE_RDMA_OFFLOAD_IBV_ASYNC_EVENT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");
	spdk_trace_register_description("RDMA_CM_ASYNC_EVENT", TRACE_RDMA_OFFLOAD_CM_ASYNC_EVENT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");
	spdk_trace_register_description("RDMA_QP_DISCONNECT", TRACE_RDMA_OFFLOAD_QP_DISCONNECT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("RDMA_QP_DESTROY", TRACE_RDMA_OFFLOAD_QP_DESTROY,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");

	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_NVMF_RDMA_OFFLOAD_IO, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_NVMF_RDMA_OFFLOAD_IO, 0);
}
SPDK_TRACE_REGISTER_FN(nvmf_rdma_offload_trace, "nvmf_rdma_offload", TRACE_GROUP_NVMF_RDMA_OFFLOAD)

enum spdk_nvmf_rdma_wr_type {
	RDMA_WR_TYPE_RECV,
	RDMA_WR_TYPE_SEND,
	RDMA_WR_TYPE_DATA,
};

struct spdk_nvmf_rdma_wr {
	/* Uses enum spdk_nvmf_rdma_wr_type */
	uint8_t type;
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr			wr;
	struct ibv_sge				sgl[NVMF_DEFAULT_RX_SGE];

	struct spdk_nvmf_rdma_qpair		*qpair;

	/* In-capsule data buffer */
	uint8_t					*buf;

	struct spdk_nvmf_rdma_wr		rdma_wr;
	uint64_t				receive_tsc;

	STAILQ_ENTRY(spdk_nvmf_rdma_recv)	link;
};

struct spdk_nvmf_rdma_request_data {
	struct ibv_send_wr		wr;
	struct ibv_sge			sgl[SPDK_NVMF_MAX_SGL_ENTRIES];
};

enum nvmf_offload_request_type {
	NVMF_OFFLOAD_REQUEST_TYPE_RDMA,
	NVMF_OFFLOAD_REQUEST_TYPE_NON_OFFLOAD
};

struct nvmf_offload_common_request {
	struct spdk_nvmf_request	req;
	enum nvmf_offload_request_type	type;
};

struct spdk_nvmf_rdma_request {
	struct nvmf_offload_common_request	common;

	bool					fused_failed;

	struct spdk_nvmf_rdma_wr		data_wr;
	struct spdk_nvmf_rdma_wr		rsp_wr;

	/* Uses enum spdk_nvmf_rdma_request_state */
	uint8_t					state;

	/* Data offset in req.iov */
	uint32_t				offset;

	struct spdk_nvmf_rdma_recv		*recv;

	struct {
		struct	ibv_send_wr		wr;
		struct	ibv_sge			sgl[NVMF_DEFAULT_RSP_SGE];
	} rsp;

	uint16_t				iovpos;
	uint16_t				num_outstanding_data_wr;
	/* Used to split Write IO with multi SGL payload */
	uint16_t				num_remaining_data_wr;
	uint64_t				receive_tsc;
	struct spdk_nvmf_rdma_request		*fused_pair;
	STAILQ_ENTRY(spdk_nvmf_rdma_request)	state_link;
	struct ibv_send_wr			*remaining_tranfer_in_wrs;
	struct ibv_send_wr			*transfer_wr;
	struct spdk_nvmf_rdma_request_data	data;
};

static inline struct spdk_nvmf_rdma_request *
nvmf_rdma_request_get(struct spdk_nvmf_request *req)
{
	struct nvmf_offload_common_request *creq;

	creq = SPDK_CONTAINEROF(req, struct nvmf_offload_common_request, req);
	assert(creq->type == NVMF_OFFLOAD_REQUEST_TYPE_RDMA);
	return SPDK_CONTAINEROF(creq, struct spdk_nvmf_rdma_request, common);
}

struct spdk_nvmf_rdma_resource_opts {
	struct spdk_nvmf_rdma_qpair	*qpair;
	/* qp points either to an ibv_qp object or an ibv_srq object depending on the value of shared. */
	void				*qp;
	struct spdk_rdma_utils_mem_map	*map;
	uint32_t			max_queue_depth;
	uint32_t			in_capsule_data_size;
	bool				shared;
};

struct spdk_nvmf_rdma_resources {
	/* Array of size "max_queue_depth" containing RDMA requests. */
	struct spdk_nvmf_rdma_request		*reqs;

	/* Array of size "max_queue_depth" containing RDMA recvs. */
	struct spdk_nvmf_rdma_recv		*recvs;

	/* Array of size "max_queue_depth" containing 64 byte capsules
	 * used for receive.
	 */
	union nvmf_h2c_msg			*cmds;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void					*bufs;

	/* Receives that are waiting for a request object */
	STAILQ_HEAD(, spdk_nvmf_rdma_recv)	incoming_queue;

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_rdma_request)	free_queue;
};

typedef void (*spdk_nvmf_rdma_qpair_ibv_event)(struct spdk_nvmf_rdma_qpair *rqpair);

typedef void (*spdk_poller_destroy_cb)(void *ctx);

struct spdk_nvmf_rdma_ibv_event_ctx {
	struct spdk_nvmf_rdma_qpair			*rqpair;
};

enum spdk_nvmf_common_qpair_type {
	SPDK_NVMF_COMMON_QPAIR_RDMA,
	SPDK_NVMF_COMMON_QPAIR_OFFLOAD
};

struct spdk_nvmf_common_qpair {
	struct spdk_nvmf_qpair			qpair;
	enum spdk_nvmf_common_qpair_type	type;
};

struct spdk_nvmf_rdma_qpair {
	struct spdk_nvmf_common_qpair		common;

	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;

	struct spdk_rdma_provider_qp		*rdma_qp;
	struct rdma_cm_id			*cm_id;
	struct spdk_rdma_provider_srq		*srq;
	struct rdma_cm_id			*listen_id;

	/* Cache the QP number to improve QP search by RB tree. */
	uint32_t				qp_num;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* The maximum number of active RDMA READ and ATOMIC operations at one time */
	uint16_t				max_read_depth;

	/* The maximum number of RDMA SEND operations at one time */
	uint32_t				max_send_depth;

	/* The current number of outstanding WRs from this qpair's
	 * recv queue. Should not exceed device->attr.max_queue_depth.
	 */
	uint16_t				current_recv_depth;

	/* The current number of active RDMA READ operations */
	uint16_t				current_read_depth;

	/* The current number of posted WRs from this qpair's
	 * send queue. Should not exceed max_send_depth.
	 */
	uint32_t				current_send_depth;

	/* The maximum number of SGEs per WR on the send queue */
	uint32_t				max_send_sge;

	struct spdk_nvmf_rdma_resources		*resources;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_read_queue;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_write_queue;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_send_queue;

	/* Number of requests not in the free state */
	uint32_t				qd;

	bool					ibv_in_error_state;
	bool					in_destroy;

	RB_ENTRY(spdk_nvmf_rdma_qpair)		node;

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	recv_link;

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	send_link;

	/* Points to the a request that has fuse bits set to
	 * SPDK_NVME_CMD_FUSE_FIRST, when the qpair is waiting
	 * for the request that has SPDK_NVME_CMD_FUSE_SECOND.
	 */
	struct spdk_nvmf_rdma_request		*fused_first;

	/*
	 * io_channel which is used to destroy qpair when it is removed from poll group
	 */
	struct spdk_io_channel		*destruct_channel;

	/* ctx for async processing of last_wqe_reached event */
	struct spdk_nvmf_rdma_ibv_event_ctx	*last_wqe_reached_ctx;

	/* Lets us know that we have received the last_wqe event. */
	bool					last_wqe_reached;

	/* Indicate that nvmf_rdma_close_qpair is called */
	bool					to_close;
};

static inline struct spdk_nvmf_rdma_qpair *
nvmf_rdma_qpair_get(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_common_qpair *cqpair;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);
	assert(cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA);
	return SPDK_CONTAINEROF(cqpair, struct spdk_nvmf_rdma_qpair, common);
}

struct spdk_nvmf_rdma_poller_stat {
	uint64_t				completions;
	uint64_t				polls;
	uint64_t				idle_polls;
	uint64_t				requests;
	uint64_t				request_latency;
	uint64_t				pending_free_request;
	uint64_t				pending_rdma_read;
	uint64_t				pending_rdma_write;
	uint64_t				pending_rdma_send;
	struct spdk_rdma_provider_qp_stats	qp_stats;
};

struct spdk_nvmf_rdma_poller {
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poll_group	*group;

	int					num_cqe;
	int					required_num_wr;
	struct spdk_rdma_provider_cq		*cq;
	struct ibv_comp_channel			*comp_channel;
	struct spdk_interrupt			*interrupt;

	/* The maximum number of I/O outstanding on the shared receive queue at one time */
	uint16_t				max_srq_depth;
	bool					need_destroy;

	/* Shared receive queue */
	struct spdk_rdma_provider_srq		*srq;

	struct spdk_nvmf_rdma_resources		*resources;
	struct spdk_nvmf_rdma_poller_stat	stat;

	spdk_poller_destroy_cb			destroy_cb;
	void					*destroy_cb_ctx;

	RB_HEAD(qpairs_tree, spdk_nvmf_rdma_qpair) qpairs;

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_recv;

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_send;
	bool					pp_handler_registered;
	TAILQ_ENTRY(spdk_nvmf_rdma_poller)	link;
};

struct spdk_nvmf_offload_qpair;

struct nvmf_non_offload_request {
	struct nvmf_offload_common_request	common;
	/* Uses enum spdk_nvmf_rdma_request_state */
	uint8_t					state;
	const uint8_t				*nvme_cmd;
	uint8_t					*payload;
	uint32_t				payload_len;
	bool					payload_valid;
	union doca_data				sta_context;
	uint64_t				receive_tsc;
	struct doca_sta_producer_task_send	*task;
	STAILQ_ENTRY(nvmf_non_offload_request)	state_link;
};

static inline struct nvmf_non_offload_request *
nvmf_non_offload_request_get(struct spdk_nvmf_request *req)
{
	struct nvmf_offload_common_request *creq;

	creq = SPDK_CONTAINEROF(req, struct nvmf_offload_common_request, req);
	assert(creq->type == NVMF_OFFLOAD_REQUEST_TYPE_NON_OFFLOAD);
	return SPDK_CONTAINEROF(creq, struct nvmf_non_offload_request, common);
}

struct nvmf_sta_non_offload_resources {
	/* Array of size "max_queue_depth" containing DOCA STA non-offloaded requests. */
	struct nvmf_non_offload_request		*reqs;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;

	/* Queue to track free requests */
	STAILQ_HEAD(, nvmf_non_offload_request)	free_queue;
	STAILQ_HEAD(, nvmf_non_offload_request)	incoming_queue;
};

struct spdk_nvmf_offload_poller;

enum spdk_nvmf_offload_qpair_state {
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_UNINITIALIZED = 0,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_REJECT,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_CONNECTED,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTING,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECT_FAILED,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTED,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINING,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINED,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_PENDING_TO_CLOSE,
	SPDK_NVMF_OFFLOAD_QPAIR_STATE_CLOSE
};

struct spdk_nvmf_offload_qpair {
	struct spdk_nvmf_common_qpair		common;
	struct spdk_nvmf_rdma_device		*device;
	struct doca_sta_qp_handle		*handle;
	struct spdk_nvmf_rdma_subsystem		*rsubsystem;
	struct spdk_nvmf_offload_poller		*opoller;
	struct rdma_cm_id			*cm_id;
	struct rdma_cm_id			*listen_id;

	/* Stored transport information for when cm_id is destroyed */
	struct sockaddr_storage			peer_addr;
	struct sockaddr_storage			local_addr;
	uint16_t				peer_port;
	uint16_t				local_port;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* The maximum number of active RDMA READ and ATOMIC operations at one time */
	uint16_t				max_read_depth;

	STAILQ_HEAD(, nvmf_non_offload_request)	pending_rdma_read_queue;

	STAILQ_HEAD(, nvmf_non_offload_request)	pending_rdma_write_queue;

	STAILQ_HEAD(, nvmf_non_offload_request)	pending_rdma_send_queue;

	/* Number of requests not in the free state */
	uint32_t				qd;
	/*
	 * io_channel which is used to destroy qpair when it is removed from poll group
	 */
	struct spdk_io_channel			*destruct_channel;

	enum spdk_nvmf_offload_qpair_state	state;

	/* Indicate that nvmf_rdma_close_qpair is called */
	uint8_t					to_close : 1;
	uint8_t					in_close : 1;
	uint8_t					in_error_state : 1;
	struct doca_sta_producer_task_send	*destroy_task;

	RB_ENTRY(spdk_nvmf_offload_qpair)	node;
};

static inline struct spdk_nvmf_offload_qpair *
nvmf_offload_qpair_get(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_common_qpair *cqpair;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);
	assert(cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD);
	return SPDK_CONTAINEROF(cqpair, struct spdk_nvmf_offload_qpair, common);
}

struct spdk_nvmf_offload_poller {
	struct spdk_nvmf_rdma_poll_group	*group;
	struct doca_pe				*pe;
	struct doca_sta_io			*sta_io;
	struct doca_ctx				*io_ctx;
	struct nvmf_sta_non_offload_resources	*resources;
	uint32_t				max_queue_depth;
	enum doca_ctx_states			state;
	doca_notification_handle_t		notification_handle;
	struct spdk_interrupt			*interrupt;
	bool					need_destroy;

	RB_HEAD(offload_qpairs_tree, spdk_nvmf_offload_qpair)	qpairs;
};

struct spdk_nvmf_rdma_poll_group_stat {
	uint64_t				pending_data_buffer;
};

struct spdk_nvmf_rdma_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	struct spdk_nvmf_rdma_poll_group_stat		stat;
	TAILQ_HEAD(, spdk_nvmf_rdma_poller)		pollers;
	struct spdk_nvmf_offload_poller			*offload_poller;
	TAILQ_ENTRY(spdk_nvmf_rdma_poll_group)		link;
};

struct spdk_nvmf_rdma_conn_sched {
	struct spdk_nvmf_rdma_poll_group *next_admin_pg;
	struct spdk_nvmf_rdma_poll_group *next_io_pg;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	struct ibv_context			*context;
	struct doca_dev				*doca_dev;

	struct spdk_rdma_utils_mem_map		*map;
	struct ibv_pd				*pd;

	int					num_srq;
	bool					need_destroy;
	bool					ready_to_destroy;
	bool					is_ready;

	TAILQ_ENTRY(spdk_nvmf_rdma_device)	link;
};

struct spdk_nvmf_rdma_port {
	const struct spdk_nvme_transport_id	*trid;
	struct rdma_cm_id			*id;
	struct spdk_nvmf_rdma_device		*device;
	TAILQ_ENTRY(spdk_nvmf_rdma_port)	link;
};

enum spdk_nvmf_rdma_bdev_type {
	SPDK_NVMF_RDMA_BDEV_TYPE_NVME,
	SPDK_NVMF_RDMA_BDEV_TYPE_NULL
};

struct spdk_nvmf_rdma_bdev_queue_destroy_ctx {
	bool destroy_completed;
	bool destroy_failed;
};

struct spdk_nvmf_rdma_bdev_nvme_queue {
	struct spdk_nvme_qpair				*nvme_qpair;
	struct doca_sta_be_q_handle			*handle;
	struct doca_mmap				*sq_mmap;
	struct doca_mmap				*cq_mmap;
	struct spdk_nvmf_rdma_bdev_queue_destroy_ctx	destroy_ctx;
};

struct spdk_nvmf_rdma_bdev_null_queue {
	void						*sq;
	void						*cq;
	uint32_t					*sqdb;
	uint32_t					*cqdb;
	struct doca_sta_be_q_handle			*handle;
	struct doca_mmap				*sq_mmap;
	struct doca_mmap				*cq_mmap;
	struct doca_mmap				*db_mmap;
	struct doca_sta_producer_task_send		*destroy_task;
	struct spdk_nvmf_rdma_bdev_queue_destroy_ctx	destroy_ctx;
};

struct spdk_nvmf_rdma_bdev {
	char							*name;
	struct spdk_nvmf_rdma_sta				*sta;
	int							refs;
	struct doca_sta_be_handle				*handle;
	enum spdk_nvmf_rdma_bdev_type				type;
	uint32_t						num_queues;
	union {
		struct {
			struct spdk_nvme_ctrlr			*ctrlr;
			struct spdk_pci_device			*pci_dev;
			struct spdk_dmabuf			*bar0_dmabuf;
			struct doca_mmap			*bar0_mmap;
			struct spdk_nvmf_rdma_bdev_nvme_queue	*queues;
		} nvme;
		struct {
			uint32_t				ns_id;
			struct spdk_nvmf_rdma_bdev_null_queue	*queues;
		} null;
	};
	bool							delete_started;
	bool							delete_completed;
	bool							delete_failed;
	TAILQ_ENTRY(spdk_nvmf_rdma_bdev)			link;
};

struct spdk_nvmf_rdma_ns {
	struct spdk_nvmf_ns			*ns;
	struct spdk_nvmf_rdma_subsystem		*rsubsystem;
	struct spdk_nvmf_rdma_bdev		*rbdev;
	struct doca_sta_ns_handle		*handle;
	uint32_t				fe_ns_id;
	uint32_t				be_ns_id;
	struct doca_sta_producer_task_send	*delete_task;
	bool					delete_started;
	bool					delete_completed;
	bool					delete_failed;
	TAILQ_ENTRY(spdk_nvmf_rdma_ns)		link;
};

struct spdk_nvmf_rdma_subsystem {
	const struct spdk_nvmf_subsystem		*subsystem;
	struct spdk_nvmf_rdma_transport			*rtransport;
	struct doca_sta_subs_handle			*handle;
	TAILQ_HEAD(, spdk_nvmf_rdma_ns)			namespaces;
	TAILQ_ENTRY(spdk_nvmf_rdma_subsystem)		link;
};

struct rdma_transport_opts {
	int		num_cqe;
	uint32_t	max_srq_depth;
	bool		no_srq;
	bool		no_wr_batching;
	int		acceptor_backlog;
	char		*doca_log_level;
	char		*doca_device;
	char		*rdma_devices_str;
	char		**rdma_devices;
	int		num_rdma_devices;
};

struct nvmf_rdma_sta_caps {
	uint32_t max_devs;
	uint32_t max_eus;
	uint32_t max_connected_qps;
	uint32_t max_subsys;
	uint32_t max_ns_per_subsys;
	uint32_t max_qps;
	uint32_t max_io_threads;
	uint32_t max_io_size;
	uint32_t max_ios;
	uint32_t max_io_queue_size;
	uint32_t min_ioccsz;
	uint32_t max_ioccsz;
	uint32_t min_iorcsz;
	uint32_t max_iorcsz;
	uint32_t max_icdoff;
	uint32_t max_be;
	uint32_t max_qs_per_be;
};

struct spdk_nvmf_rdma_sta {
	struct doca_pe				*pe;
	struct doca_dev				*dev;
	struct doca_sta				*sta;
	struct doca_ctx				*ctx;
	struct spdk_poller			*poller;
	struct nvmf_rdma_sta_caps		caps;
	enum doca_ctx_states			state;
	uint32_t				num_cq_handlers;
	TAILQ_HEAD(, spdk_nvmf_rdma_bdev)	bdevs;
};

struct spdk_nvmf_rdma_transport {
	struct spdk_nvmf_transport	transport;
	struct rdma_transport_opts	rdma_opts;

	struct spdk_nvmf_rdma_conn_sched conn_sched;

	struct rdma_event_channel	*event_channel;

	struct spdk_mempool		*data_wr_pool;

	struct spdk_poller		*accept_poller;

	/* fields used to poll RDMA/IB events */
	nfds_t			npoll_fds;
	struct pollfd		*poll_fds;

	struct spdk_nvmf_rdma_sta	sta;

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
	TAILQ_HEAD(, spdk_nvmf_rdma_poll_group)	poll_groups;

	/* ports that are removed unexpectedly and need retry listen */
	TAILQ_HEAD(, spdk_nvmf_rdma_port)		retry_ports;

	TAILQ_HEAD(, spdk_nvmf_rdma_subsystem)	subsystems;
	TAILQ_HEAD(, spdk_nvmf_rdma_bdev)	bdevs;
};

struct poller_manage_ctx {
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_device		*device;

	struct spdk_thread			*thread;
	volatile int				*inflight_op_counter;
};

static const struct spdk_json_object_decoder rdma_transport_opts_decoder[] = {
	{
		"num_cqe", offsetof(struct rdma_transport_opts, num_cqe),
		spdk_json_decode_int32, true
	},
	{
		"max_srq_depth", offsetof(struct rdma_transport_opts, max_srq_depth),
		spdk_json_decode_uint32, true
	},
	{
		"no_srq", offsetof(struct rdma_transport_opts, no_srq),
		spdk_json_decode_bool, true
	},
	{
		"no_wr_batching", offsetof(struct rdma_transport_opts, no_wr_batching),
		spdk_json_decode_bool, true
	},
	{
		"acceptor_backlog", offsetof(struct rdma_transport_opts, acceptor_backlog),
		spdk_json_decode_int32, true
	},
	{
		"doca_log_level", offsetof(struct rdma_transport_opts, doca_log_level),
		spdk_json_decode_string, true
	},
	{
		"doca_device", offsetof(struct rdma_transport_opts, doca_device),
		spdk_json_decode_string, true
	},
	{
		"rdma_device_list", offsetof(struct rdma_transport_opts, rdma_devices_str),
		spdk_json_decode_string, true
	},
};

static int
nvmf_rdma_qpair_compare(struct spdk_nvmf_rdma_qpair *rqpair1, struct spdk_nvmf_rdma_qpair *rqpair2)
{
	return rqpair1->qp_num < rqpair2->qp_num ? -1 : rqpair1->qp_num > rqpair2->qp_num;
}

RB_GENERATE_STATIC(qpairs_tree, spdk_nvmf_rdma_qpair, node, nvmf_rdma_qpair_compare);

static bool nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_request *rdma_req);

static int
nvmf_offload_qpair_compare(struct spdk_nvmf_offload_qpair *oqpair1,
			   struct spdk_nvmf_offload_qpair *oqpair2)
{
	return oqpair1->handle < oqpair2->handle ? -1 : oqpair1->handle > oqpair2->handle;
}

RB_GENERATE_STATIC(offload_qpairs_tree, spdk_nvmf_offload_qpair, node, nvmf_offload_qpair_compare);

static void _poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

static void _poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

static void _nvmf_rdma_remove_destroyed_device(void *c);

static int nvmf_rdma_bdev_destroy(struct spdk_nvmf_rdma_bdev *rbdev);

static void nvmf_rdma_offload_qpair_close_process(struct spdk_nvmf_offload_qpair *oqpair);

static void nvmf_sta_io_disconnect_comp_hadler(struct doca_sta_producer_task_send *task,
		union doca_data task_user_data);

static void nvmf_sta_io_disconnect_error_hadler(struct doca_sta_producer_task_send *task,
		union doca_data task_user_data);

static int nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

static inline enum spdk_nvme_media_error_status_code
nvmf_rdma_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;
	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return result;
}

/*
 * Return data_wrs to pool starting from \b data_wr
 * Request's own response and data WR are excluded
 */
static void
_nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			     struct ibv_send_wr *data_wr,
			     struct spdk_mempool *pool)
{
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];
	struct spdk_nvmf_rdma_request_data	*nvmf_data;
	struct ibv_send_wr			*next_send_wr;
	uint64_t				req_wrid = (uint64_t)&rdma_req->data_wr;
	uint32_t				num_wrs = 0;

	while (data_wr && data_wr->wr_id == req_wrid) {
		nvmf_data = SPDK_CONTAINEROF(data_wr, struct spdk_nvmf_rdma_request_data, wr);
		memset(nvmf_data->sgl, 0, sizeof(data_wr->sg_list[0]) * data_wr->num_sge);
		data_wr->num_sge = 0;
		next_send_wr = data_wr->next;
		if (data_wr != &rdma_req->data.wr) {
			data_wr->next = NULL;
			assert(num_wrs < SPDK_NVMF_MAX_SGL_ENTRIES);
			work_requests[num_wrs] = nvmf_data;
			num_wrs++;
		}
		data_wr = (!next_send_wr || next_send_wr == &rdma_req->rsp.wr) ? NULL : next_send_wr;
	}

	if (num_wrs) {
		spdk_mempool_put_bulk(pool, (void **) work_requests, num_wrs);
	}
}

static void
nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			    struct spdk_nvmf_rdma_transport *rtransport)
{
	rdma_req->num_outstanding_data_wr = 0;

	_nvmf_rdma_request_free_data(rdma_req, rdma_req->transfer_wr, rtransport->data_wr_pool);

	if (rdma_req->remaining_tranfer_in_wrs) {
		_nvmf_rdma_request_free_data(rdma_req, rdma_req->remaining_tranfer_in_wrs,
					     rtransport->data_wr_pool);
		rdma_req->remaining_tranfer_in_wrs = NULL;
	}

	rdma_req->data.wr.next = NULL;
	rdma_req->rsp.wr.next = NULL;
}

static void
nvmf_rdma_dump_request(struct spdk_nvmf_rdma_request *req)
{
	SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", req->common.req.data_from_pool);
	if (req->common.req.cmd) {
		SPDK_ERRLOG("\t\tRequest opcode: %d\n", req->common.req.cmd->nvmf_cmd.opcode);
	}
	if (req->recv) {
		SPDK_ERRLOG("\t\tRequest recv wr_id%lu\n", req->recv->wr.wr_id);
	}
}

static void
nvmf_rdma_dump_qpair_contents(struct spdk_nvmf_rdma_qpair *rqpair)
{
	int i;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", rqpair->common.qpair.qid);
	for (i = 0; i < rqpair->max_queue_depth; i++) {
		if (rqpair->resources->reqs[i].state != RDMA_REQUEST_STATE_FREE) {
			nvmf_rdma_dump_request(&rqpair->resources->reqs[i]);
		}
	}
}

static void
nvmf_rdma_resources_destroy(struct spdk_nvmf_rdma_resources *resources)
{
	spdk_free(resources->cmds);
	spdk_free(resources->cpls);
	spdk_free(resources->bufs);
	spdk_free(resources->reqs);
	spdk_free(resources->recvs);
	free(resources);
}


static struct spdk_nvmf_rdma_resources *
nvmf_rdma_resources_create(struct spdk_nvmf_rdma_resource_opts *opts)
{
	struct spdk_nvmf_rdma_resources		*resources;
	struct spdk_nvmf_rdma_request		*rdma_req;
	struct spdk_nvmf_rdma_recv		*rdma_recv;
	struct spdk_rdma_provider_qp		*qp = NULL;
	struct spdk_rdma_provider_srq		*srq = NULL;
	struct ibv_recv_wr			*bad_wr = NULL;
	struct spdk_rdma_utils_memory_translation translation;
	uint32_t				i;
	int					rc = 0;

	resources = calloc(1, sizeof(struct spdk_nvmf_rdma_resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
		return NULL;
	}

	resources->reqs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->recvs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->recvs),
					0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cmds = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cmds),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cpls = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (opts->in_capsule_data_size > 0) {
		resources->bufs = spdk_zmalloc(opts->max_queue_depth * opts->in_capsule_data_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	}

	if (!resources->reqs || !resources->recvs || !resources->cmds ||
	    !resources->cpls || (opts->in_capsule_data_size && !resources->bufs)) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		goto cleanup;
	}

	SPDK_DEBUGLOG(rdma_offload, "Command Array: %p Length: %lx\n",
		      resources->cmds, opts->max_queue_depth * sizeof(*resources->cmds));
	SPDK_DEBUGLOG(rdma_offload, "Completion Array: %p Length: %lx\n",
		      resources->cpls, opts->max_queue_depth * sizeof(*resources->cpls));
	if (resources->bufs) {
		SPDK_DEBUGLOG(rdma_offload, "In Capsule Data Array: %p Length: %x\n",
			      resources->bufs, opts->max_queue_depth *
			      opts->in_capsule_data_size);
	}

	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	if (opts->shared) {
		srq = (struct spdk_rdma_provider_srq *)opts->qp;
	} else {
		qp = (struct spdk_rdma_provider_qp *)opts->qp;
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_recv = &resources->recvs[i];
		rdma_recv->qpair = opts->qpair;

		/* Set up memory to receive commands */
		if (resources->bufs) {
			rdma_recv->buf = (void *)((uintptr_t)resources->bufs + (i *
						  opts->in_capsule_data_size));
		}

		rdma_recv->rdma_wr.type = RDMA_WR_TYPE_RECV;

		rdma_recv->sgl[0].addr = (uintptr_t)&resources->cmds[i];
		rdma_recv->sgl[0].length = sizeof(resources->cmds[i]);
		rc = spdk_rdma_utils_get_translation(opts->map, &resources->cmds[i], sizeof(resources->cmds[i]),
						     &translation);
		if (rc) {
			goto cleanup;
		}
		rdma_recv->sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
		rdma_recv->wr.num_sge = 1;

		if (rdma_recv->buf) {
			rdma_recv->sgl[1].addr = (uintptr_t)rdma_recv->buf;
			rdma_recv->sgl[1].length = opts->in_capsule_data_size;
			rc = spdk_rdma_utils_get_translation(opts->map, rdma_recv->buf, opts->in_capsule_data_size,
							     &translation);
			if (rc) {
				goto cleanup;
			}
			rdma_recv->sgl[1].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
			rdma_recv->wr.num_sge++;
		}

		rdma_recv->wr.wr_id = (uintptr_t)&rdma_recv->rdma_wr;
		rdma_recv->wr.sg_list = rdma_recv->sgl;
		if (srq) {
			spdk_rdma_provider_srq_queue_recv_wrs(srq, &rdma_recv->wr);
		} else {
			spdk_rdma_provider_qp_queue_recv_wrs(qp, &rdma_recv->wr);
		}
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_req = &resources->reqs[i];

		rdma_req->common.type = NVMF_OFFLOAD_REQUEST_TYPE_RDMA;

		if (opts->qpair != NULL) {
			rdma_req->common.req.qpair = &opts->qpair->common.qpair;
		} else {
			rdma_req->common.req.qpair = NULL;
		}
		rdma_req->common.req.cmd = NULL;
		rdma_req->common.req.iovcnt = 0;
		rdma_req->common.req.stripped_data = NULL;

		/* Set up memory to send responses */
		rdma_req->common.req.rsp = &resources->cpls[i];

		rdma_req->rsp.sgl[0].addr = (uintptr_t)&resources->cpls[i];
		rdma_req->rsp.sgl[0].length = sizeof(resources->cpls[i]);
		rc = spdk_rdma_utils_get_translation(opts->map, &resources->cpls[i], sizeof(resources->cpls[i]),
						     &translation);
		if (rc) {
			goto cleanup;
		}
		rdma_req->rsp.sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);

		rdma_req->rsp_wr.type = RDMA_WR_TYPE_SEND;
		rdma_req->rsp.wr.wr_id = (uintptr_t)&rdma_req->rsp_wr;
		rdma_req->rsp.wr.next = NULL;
		rdma_req->rsp.wr.opcode = IBV_WR_SEND;
		rdma_req->rsp.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->rsp.wr.sg_list = rdma_req->rsp.sgl;
		rdma_req->rsp.wr.num_sge = SPDK_COUNTOF(rdma_req->rsp.sgl);

		/* Set up memory for data buffers */
		rdma_req->data_wr.type = RDMA_WR_TYPE_DATA;
		rdma_req->data.wr.wr_id = (uintptr_t)&rdma_req->data_wr;
		rdma_req->data.wr.next = NULL;
		rdma_req->data.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->data.wr.sg_list = rdma_req->data.sgl;
		rdma_req->data.wr.num_sge = SPDK_COUNTOF(rdma_req->data.sgl);

		/* Initialize request state to FREE */
		rdma_req->state = RDMA_REQUEST_STATE_FREE;
		STAILQ_INSERT_TAIL(&resources->free_queue, rdma_req, state_link);
	}

	if (srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(srq, &bad_wr);
	} else {
		rc = spdk_rdma_provider_qp_flush_recv_wrs(qp, &bad_wr);
	}

	if (rc) {
		goto cleanup;
	}

	return resources;

cleanup:
	nvmf_rdma_resources_destroy(resources);
	return NULL;
}

static void
nvmf_rdma_qpair_clean_ibv_events(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;

	ctx = rqpair->last_wqe_reached_ctx;
	if (ctx) {
		ctx->rqpair = NULL;
		/* Memory allocated for ctx is freed in nvmf_rdma_qpair_process_last_wqe_event */
		rqpair->last_wqe_reached_ctx = NULL;
	}
}

static void nvmf_rdma_poller_destroy(struct spdk_nvmf_rdma_poller *poller);

static void
nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	int				rc;

	spdk_trace_record(TRACE_RDMA_OFFLOAD_QP_DESTROY, 0, 0, (uintptr_t)rqpair);

	if (rqpair->qd != 0) {
		struct spdk_nvmf_qpair *qpair = &rqpair->common.qpair;
		struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
				struct spdk_nvmf_rdma_transport, transport);
		struct spdk_nvmf_rdma_request *req;
		uint32_t i, max_req_count = 0;

		SPDK_WARNLOG("Destroying qpair when queue depth is %d\n", rqpair->qd);

		if (rqpair->srq == NULL) {
			nvmf_rdma_dump_qpair_contents(rqpair);
			max_req_count = rqpair->max_queue_depth;
		} else if (rqpair->poller && rqpair->resources) {
			max_req_count = rqpair->poller->max_srq_depth;
		}

		SPDK_DEBUGLOG(rdma_offload, "Release incomplete requests\n");
		for (i = 0; i < max_req_count; i++) {
			req = &rqpair->resources->reqs[i];
			if (req->common.req.qpair == qpair && req->state != RDMA_REQUEST_STATE_FREE) {
				/* nvmf_rdma_request_process checks qpair ibv and internal state
				 * and completes a request */
				nvmf_rdma_request_process(rtransport, req);
			}
		}
		assert(rqpair->qd == 0);
	}

	if (rqpair->poller) {
		RB_REMOVE(qpairs_tree, &rqpair->poller->qpairs, rqpair);

		if (rqpair->srq != NULL && rqpair->resources != NULL) {
			/* Drop all received but unprocessed commands for this queue and return them to SRQ */
			STAILQ_FOREACH_SAFE(rdma_recv, &rqpair->resources->incoming_queue, link, recv_tmp) {
				if (rqpair == rdma_recv->qpair) {
					STAILQ_REMOVE(&rqpair->resources->incoming_queue, rdma_recv, spdk_nvmf_rdma_recv, link);
					spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
					if (rc) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
					}
				}
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp != NULL) {
			spdk_rdma_provider_qp_destroy(rqpair->rdma_qp);
			rqpair->rdma_qp = NULL;
		}

		if (rqpair->poller != NULL && rqpair->srq == NULL) {
			rqpair->poller->required_num_wr -= MAX_WR_PER_QP(rqpair->max_queue_depth);
		}
	}

	if (rqpair->srq == NULL && rqpair->resources != NULL) {
		nvmf_rdma_resources_destroy(rqpair->resources);
	}

	nvmf_rdma_qpair_clean_ibv_events(rqpair);

	if (rqpair->destruct_channel) {
		spdk_put_io_channel(rqpair->destruct_channel);
		rqpair->destruct_channel = NULL;
	}

	if (rqpair->poller && rqpair->poller->need_destroy && RB_EMPTY(&rqpair->poller->qpairs)) {
		nvmf_rdma_poller_destroy(rqpair->poller);
	}

	/* destroy cm_id last so cma device will not be freed before we destroy the cq. */
	if (rqpair->cm_id) {
		rdma_destroy_id(rqpair->cm_id);
	}

	free(rqpair);
}

static int
nvmf_rdma_resize_cq(struct spdk_nvmf_rdma_qpair *rqpair, struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_poller	*rpoller;
	int				rc, num_cqe, required_num_wr;

	/* Enlarge CQ size dynamically */
	rpoller = rqpair->poller;
	required_num_wr = rpoller->required_num_wr + MAX_WR_PER_QP(rqpair->max_queue_depth);
	num_cqe = rpoller->num_cqe;
	if (num_cqe < required_num_wr) {
		num_cqe = spdk_max(num_cqe * 2, required_num_wr);
		num_cqe = spdk_min(num_cqe, device->attr.max_cqe);
	}

	if (rpoller->num_cqe != num_cqe) {
		if (device->context->device->transport_type == IBV_TRANSPORT_IWARP) {
			SPDK_ERRLOG("iWARP doesn't support CQ resize. Current capacity %u, required %u\n"
				    "Using CQ of insufficient size may lead to CQ overrun\n", rpoller->num_cqe, num_cqe);
			return -1;
		}
		if (required_num_wr > device->attr.max_cqe) {
			SPDK_ERRLOG("RDMA CQE requirement (%d) exceeds device max_cqe limitation (%d)\n",
				    required_num_wr, device->attr.max_cqe);
			return -1;
		}

		SPDK_DEBUGLOG(rdma_offload, "Resize RDMA CQ from %d to %d\n", rpoller->num_cqe, num_cqe);
		rc = spdk_rdma_provider_cq_resize(rpoller->cq, num_cqe);
		if (rc) {
			SPDK_ERRLOG("RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		rpoller->num_cqe = num_cqe;
	}

	rpoller->required_num_wr = required_num_wr;
	return 0;
}

static int
nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_rdma_resource_opts	opts;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_rdma_provider_qp_init_attr	qp_init_attr = {};

	rqpair = nvmf_rdma_qpair_get(qpair);
	device = rqpair->device;

	qp_init_attr.qp_context	= rqpair;
	qp_init_attr.pd		= device->pd;
	qp_init_attr.cq		= rqpair->poller->cq;

	if (rqpair->srq) {
		qp_init_attr.srq		= rqpair->srq;
	} else {
		qp_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth;
	}

	/* SEND, READ, and WRITE operations */
	qp_init_attr.cap.max_send_wr	= (uint32_t)rqpair->max_queue_depth * 2;
	qp_init_attr.cap.max_send_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
	qp_init_attr.cap.max_recv_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
	qp_init_attr.stats		= &rqpair->poller->stat.qp_stats;

	if (rqpair->srq == NULL && nvmf_rdma_resize_cq(rqpair, device) < 0) {
		SPDK_ERRLOG("Failed to resize the completion queue. Cannot initialize qpair.\n");
		goto error;
	}

	rqpair->rdma_qp = spdk_rdma_provider_qp_create(rqpair->cm_id, &qp_init_attr);
	if (!rqpair->rdma_qp) {
		goto error;
	}

	rqpair->qp_num = rqpair->rdma_qp->qp->qp_num;

	rqpair->max_send_depth = spdk_min((uint32_t)(rqpair->max_queue_depth * 2),
					  qp_init_attr.cap.max_send_wr);
	rqpair->max_send_sge = spdk_min(NVMF_DEFAULT_TX_SGE, qp_init_attr.cap.max_send_sge);
	spdk_trace_record(TRACE_RDMA_OFFLOAD_QP_CREATE, 0, 0, (uintptr_t)rqpair);
	SPDK_DEBUGLOG(rdma_offload, "New RDMA Connection: %p\n", qpair);

	if (rqpair->poller->srq == NULL) {
		rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
		transport = &rtransport->transport;

		opts.qp = rqpair->rdma_qp;
		opts.map = device->map;
		opts.qpair = rqpair;
		opts.shared = false;
		opts.max_queue_depth = rqpair->max_queue_depth;
		opts.in_capsule_data_size = transport->opts.in_capsule_data_size;

		rqpair->resources = nvmf_rdma_resources_create(&opts);

		if (!rqpair->resources) {
			SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
			rdma_destroy_qp(rqpair->cm_id);
			goto error;
		}
	} else {
		rqpair->resources = rqpair->poller->resources;
	}

	rqpair->current_recv_depth = 0;
	STAILQ_INIT(&rqpair->pending_rdma_read_queue);
	STAILQ_INIT(&rqpair->pending_rdma_write_queue);
	STAILQ_INIT(&rqpair->pending_rdma_send_queue);
	qpair->queue_depth = 0;

	return 0;

error:
	rdma_destroy_id(rqpair->cm_id);
	rqpair->cm_id = NULL;
	return -1;
}

static void
nvmf_sta_non_offload_resources_destroy(struct nvmf_sta_non_offload_resources *resources)
{
	spdk_free(resources->cpls);
	spdk_free(resources->reqs);
	free(resources);
}


static struct nvmf_sta_non_offload_resources *
nvmf_sta_non_offload_resources_create(uint32_t max_queue_depth)
{
	struct nvmf_sta_non_offload_resources	*resources;
	struct nvmf_non_offload_request		*non_offload_req;
	struct spdk_nvmf_request		*req;
	uint32_t				i;

	resources = calloc(1, sizeof(struct nvmf_sta_non_offload_resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate resources for non-offload.\n");
		return NULL;
	}

	resources->reqs = spdk_zmalloc(max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cpls = spdk_zmalloc(max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (!resources->reqs || !resources->cpls) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for non-offload queue.\n");
		goto cleanup;
	}

	SPDK_DEBUGLOG(rdma_offload, "Completion Array: %p Length: %lx\n",
		      resources->cpls, max_queue_depth * sizeof(*resources->cpls));

	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	for (i = 0; i < max_queue_depth; i++) {
		non_offload_req = &resources->reqs[i];
		non_offload_req->common.type = NVMF_OFFLOAD_REQUEST_TYPE_NON_OFFLOAD;
		req = &non_offload_req->common.req;

		req->cmd = NULL;
		req->iovcnt = 0;
		req->stripped_data = NULL;

		/* Set up memory to send responses */
		req->rsp = &resources->cpls[i];

		/* Initialize request state to FREE */
		non_offload_req->state = RDMA_REQUEST_STATE_FREE;
		STAILQ_INSERT_TAIL(&resources->free_queue, non_offload_req, state_link);
	}

	return resources;

cleanup:
	nvmf_sta_non_offload_resources_destroy(resources);
	return NULL;
}

static int
nvmf_offload_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_offload_qpair *oqpair;
	struct spdk_nvmf_offload_poller *opoller;
	struct spdk_nvmf_rdma_accept_private_data accept_data;
	doca_error_t drc;

	oqpair = nvmf_offload_qpair_get(qpair);
	opoller = oqpair->opoller;

	accept_data.recfmt = 0;
	accept_data.crqsize = oqpair->max_queue_depth;

	drc = doca_sta_io_qp_alloc(opoller->sta_io, oqpair->device->doca_dev, &oqpair->handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to alloc offload qpair: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_io_qp_accept(opoller->sta_io, oqpair->handle, oqpair->device->doca_dev,
				    oqpair->cm_id, &accept_data, sizeof(accept_data),
				    oqpair->rsubsystem ? oqpair->rsubsystem->handle : 0);
	/*
	 * DOCA STA will call rdma_destroy_id for cm_id. It happens
	 * regardless of the doca_sta_io_qp_accept() return code.
	 */
	oqpair->cm_id = NULL;
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Cannot connect offload qpair: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_io_qp_connect_established(opoller->sta_io, oqpair->handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to establish connection: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	return 0;
}

static void
nvmf_rdma_pp_handler(void *fn_arg)
{
	struct spdk_nvmf_rdma_poller *rpoller = fn_arg;
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rpoller->group->group.transport,
			struct spdk_nvmf_rdma_transport, transport);

	rpoller->pp_handler_registered = false;
	_poller_submit_recvs(rtransport, rpoller);
	_poller_submit_sends(rtransport, rpoller);
}

/* Append the given recv wr structure to the resource structs outstanding recvs list. */
/* This function accepts either a single wr or the first wr in a linked list. */
static void
nvmf_rdma_qpair_queue_recv_wrs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *first)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->common.qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	if (rqpair->srq != NULL) {
		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, first);
	} else {
		if (spdk_rdma_provider_qp_queue_recv_wrs(rqpair->rdma_qp, first)) {
			STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_recv, rqpair, recv_link);
		}
	}

	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_recvs(rtransport, rqpair->poller);
	} else if (!rqpair->poller->pp_handler_registered) {
		spdk_thread_register_post_poller_handler(nvmf_rdma_pp_handler, rqpair->poller);
		rqpair->poller->pp_handler_registered = true;
	}
}

static inline void
request_transfer_in(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;

	qpair = req->qpair;
	rdma_req = nvmf_rdma_request_get(req);
	rqpair = nvmf_rdma_qpair_get(qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->common.qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);

	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->rdma_qp, rdma_req->transfer_wr)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	} else if (!rqpair->poller->pp_handler_registered) {
		spdk_thread_register_post_poller_handler(nvmf_rdma_pp_handler, rqpair->poller);
		rqpair->poller->pp_handler_registered = true;
	}

	assert(rqpair->current_read_depth + rdma_req->num_outstanding_data_wr <= rqpair->max_read_depth);
	rqpair->current_read_depth += rdma_req->num_outstanding_data_wr;
	assert(rqpair->current_send_depth + rdma_req->num_outstanding_data_wr <= rqpair->max_send_depth);
	rqpair->current_send_depth += rdma_req->num_outstanding_data_wr;
}

static inline void
nvmf_rdma_request_reset_transfer_in(struct spdk_nvmf_rdma_request *rdma_req,
				    struct spdk_nvmf_rdma_transport *rtransport)
{
	/* Put completed WRs back to pool and move transfer_wr pointer */
	_nvmf_rdma_request_free_data(rdma_req, rdma_req->transfer_wr, rtransport->data_wr_pool);
	rdma_req->transfer_wr = rdma_req->remaining_tranfer_in_wrs;
	rdma_req->remaining_tranfer_in_wrs = NULL;
	rdma_req->num_outstanding_data_wr = rdma_req->num_remaining_data_wr;
	rdma_req->num_remaining_data_wr = 0;
}

static inline int
request_prepare_transfer_in_part(struct spdk_nvmf_request *req, uint32_t num_reads_available)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct ibv_send_wr		*wr;
	uint32_t i;

	rdma_req = nvmf_rdma_request_get(req);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);
	assert(num_reads_available > 0);
	assert(rdma_req->num_outstanding_data_wr > num_reads_available);
	wr = rdma_req->transfer_wr;

	for (i = 0; i < num_reads_available - 1; i++) {
		wr = wr->next;
	}

	rdma_req->remaining_tranfer_in_wrs = wr->next;
	rdma_req->num_remaining_data_wr = rdma_req->num_outstanding_data_wr - num_reads_available;
	rdma_req->num_outstanding_data_wr = num_reads_available;
	/* Break chain of WRs to send only part. Once this portion completes, we continue sending RDMA_READs */
	wr->next = NULL;

	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req, int *data_posted)
{
	int				num_outstanding_data_wr = 0;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_send_wr		*first = NULL;
	struct spdk_nvmf_rdma_transport *rtransport;

	*data_posted = 0;
	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	rdma_req = nvmf_rdma_request_get(req);
	rqpair = nvmf_rdma_qpair_get(qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->common.qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	/* queue the capsule for the recv buffer */
	assert(rdma_req->recv != NULL);

	nvmf_rdma_qpair_queue_recv_wrs(rqpair, &rdma_req->recv->wr);

	rdma_req->recv = NULL;
	assert(rqpair->current_recv_depth > 0);
	rqpair->current_recv_depth--;

	/* Build the response which consists of optional
	 * RDMA WRITEs to transfer data, plus an RDMA SEND
	 * containing the response.
	 */
	first = &rdma_req->rsp.wr;

	if (spdk_unlikely(rsp->status.sc != SPDK_NVME_SC_SUCCESS)) {
		/* On failure, data was not read from the controller. So clear the
		 * number of outstanding data WRs to zero.
		 */
		rdma_req->num_outstanding_data_wr = 0;
	} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		first = rdma_req->transfer_wr;
		*data_posted = 1;
		num_outstanding_data_wr = rdma_req->num_outstanding_data_wr;
	}
	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->rdma_qp, first)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	} else if (!rqpair->poller->pp_handler_registered) {
		spdk_thread_register_post_poller_handler(nvmf_rdma_pp_handler, rqpair->poller);
		rqpair->poller->pp_handler_registered = true;
	}

	/* +1 for the rsp wr */
	assert(rqpair->current_send_depth + num_outstanding_data_wr + 1 <= rqpair->max_send_depth);
	rqpair->current_send_depth += num_outstanding_data_wr + 1;

	return 0;
}

static int
nvmf_rdma_event_accept(struct rdma_cm_id *id, struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_accept_private_data	accept_data;
	struct rdma_conn_param				ctrlr_event_data = {};
	int						rc;

	accept_data.recfmt = 0;
	accept_data.crqsize = rqpair->max_queue_depth;

	ctrlr_event_data.private_data = &accept_data;
	ctrlr_event_data.private_data_len = sizeof(accept_data);
	if (id->ps == RDMA_PS_TCP) {
		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
		ctrlr_event_data.initiator_depth = rqpair->max_read_depth;
	}

	/* Configure infinite retries for the initiator side qpair.
	 * We need to pass this value to the initiator to prevent the
	 * initiator side NIC from completing SEND requests back to the
	 * initiator with status rnr_retry_count_exceeded. */
	ctrlr_event_data.rnr_retry_count = 0x7;

	/* When qpair is created without use of rdma cm API, an additional
	 * information must be provided to initiator in the connection response:
	 * whether qpair is using SRQ and its qp_num
	 * Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	ctrlr_event_data.srq = rqpair->srq ? 1 : 0;
	ctrlr_event_data.qp_num = rqpair->qp_num;

	rc = spdk_rdma_provider_qp_accept(rqpair->rdma_qp, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error %d on spdk_rdma_provider_qp_accept\n", errno);
	} else {
		SPDK_DEBUGLOG(rdma_offload, "Sent back the accept\n");
	}

	return rc;
}

static void
nvmf_rdma_event_reject(struct rdma_cm_id *id, enum spdk_nvmf_rdma_transport_error error)
{
	struct spdk_nvmf_rdma_reject_private_data	rej_data;

	rej_data.recfmt = 0;
	rej_data.sts = error;

	rdma_reject(id, &rej_data, sizeof(rej_data));
}

static void
copy_sockaddr(const struct sockaddr *src, struct sockaddr_storage *dst)
{
	socklen_t len;

	memset(dst, 0, sizeof(*dst));

	switch (src->sa_family) {
	case AF_INET:
		len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		len = sizeof(struct sockaddr_in6);
		break;
	default:
		SPDK_ERRLOG("Unsupported sa_family %d\n", src->sa_family);
		assert(0);
		return;
	}
	memcpy(dst, src, len);
}

static int
nvmf_rdma_connect(struct spdk_nvmf_transport *transport, struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct spdk_nvmf_offload_qpair	*oqpair = NULL;
	struct spdk_nvmf_qpair		*qpair = NULL;
	struct spdk_nvmf_rdma_port	*port;
	struct rdma_conn_param		*rdma_param = NULL;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	uint16_t			max_queue_depth;
	uint16_t			max_read_depth;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	assert(event->id != NULL); /* Impossible. Can't even reject the connection. */
	assert(event->id->verbs != NULL); /* Impossible. No way to handle this. */

	rdma_param = &event->param.conn;
	if (rdma_param->private_data == NULL ||
	    rdma_param->private_data_len < sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_ERRLOG("connect request: no private data provided\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
		return -1;
	}

	private_data = rdma_param->private_data;
	if (private_data->recfmt != 0) {
		SPDK_ERRLOG("Received RDMA private data with RECFMT != 0\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT);
		return -1;
	}

	SPDK_DEBUGLOG(rdma_offload, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      event->id->verbs->device->name, event->id->verbs->device->dev_name);

	port = event->listen_id->context;
	SPDK_DEBUGLOG(rdma_offload, "Listen Id was %p with verbs %p. ListenAddr: %p\n",
		      event->listen_id, event->listen_id->verbs, port);

	/* Figure out the supported queue depth. This is a multi-step process
	 * that takes into account hardware maximums, host provided values,
	 * and our target's internal memory limits */

	SPDK_DEBUGLOG(rdma_offload, "Calculating Queue Depth\n");

	/* Start with the maximum queue depth allowed by the target */
	max_queue_depth = rtransport->transport.opts.max_queue_depth;
	max_read_depth = rtransport->transport.opts.max_queue_depth;
	SPDK_DEBUGLOG(rdma_offload, "Target Max Queue Depth: %d\n",
		      rtransport->transport.opts.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma_offload,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_read_depth = spdk_min(max_read_depth, port->device->attr.max_qp_init_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma_offload,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	/* from man3 rdma_get_cm_event
	 * responder_resources - Specifies the number of responder resources that is requested by the recipient.
	 * The responder_resources field must match the initiator depth specified by the remote node when running
	 * the rdma_connect and rdma_accept functions. */
	if (rdma_param->responder_resources != 0) {
		if (private_data->qid) {
			SPDK_DEBUGLOG(rdma_offload, "Host (Initiator) is not allowed to use RDMA operations,"
				      " responder_resources must be 0 but set to %u\n",
				      rdma_param->responder_resources);
		} else {
			SPDK_WARNLOG("Host (Initiator) is not allowed to use RDMA operations,"
				     " responder_resources must be 0 but set to %u\n",
				     rdma_param->responder_resources);
		}
	}
	/* from man3 rdma_get_cm_event
	 * initiator_depth - Specifies the maximum number of outstanding RDMA read operations that the recipient holds.
	 * The initiator_depth field must match the responder resources specified by the remote node when running
	 * the rdma_connect and rdma_accept functions. */
	if (rdma_param->initiator_depth == 0) {
		SPDK_ERRLOG("Host (Initiator) doesn't support RDMA_READ or atomic operations\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_IRD);
		return -1;
	}
	max_read_depth = spdk_min(max_read_depth, rdma_param->initiator_depth);

	SPDK_DEBUGLOG(rdma_offload, "Host Receive Queue Size: %d\n", private_data->hrqsize);
	SPDK_DEBUGLOG(rdma_offload, "Host Send Queue Size: %d\n", private_data->hsqsize);
	max_queue_depth = spdk_min(max_queue_depth, private_data->hrqsize);
	max_queue_depth = spdk_min(max_queue_depth, private_data->hsqsize + 1);

	SPDK_DEBUGLOG(rdma_offload, "Final Negotiated Queue Depth: %d R/W Depth: %d\n",
		      max_queue_depth, max_read_depth);

	if (private_data->qid == 0) {
		rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
		if (rqpair == NULL) {
			SPDK_ERRLOG("Could not allocate new connection.\n");
			nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
			return -1;
		}

		rqpair->common.type = SPDK_NVMF_COMMON_QPAIR_RDMA;
		rqpair->device = port->device;
		rqpair->max_queue_depth = max_queue_depth;
		rqpair->max_read_depth = max_read_depth;
		rqpair->cm_id = event->id;
		rqpair->listen_id = event->listen_id;

		qpair = &rqpair->common.qpair;
		qpair->numa.id_valid = 1;
		qpair->numa.id = spdk_rdma_cm_id_get_numa_id(rqpair->cm_id);
	} else {
		oqpair = calloc(1, sizeof(struct spdk_nvmf_offload_qpair));
		if (oqpair == NULL) {
			SPDK_ERRLOG("Could not allocate new connection.\n");
			nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
			return -1;
		}
		oqpair->common.type = SPDK_NVMF_COMMON_QPAIR_OFFLOAD;
		oqpair->device = port->device;
		oqpair->max_queue_depth = max_queue_depth;
		oqpair->max_read_depth = max_read_depth;
		oqpair->cm_id = event->id;
		oqpair->listen_id = event->listen_id;

		/* Initialize transport info fields */
		copy_sockaddr(rdma_get_peer_addr(oqpair->cm_id), &oqpair->peer_addr);
		copy_sockaddr(rdma_get_local_addr(oqpair->cm_id), &oqpair->local_addr);
		oqpair->peer_port = ntohs(rdma_get_dst_port(oqpair->cm_id));
		oqpair->local_port = ntohs(rdma_get_src_port(oqpair->cm_id));

		STAILQ_INIT(&oqpair->pending_rdma_read_queue);
		STAILQ_INIT(&oqpair->pending_rdma_write_queue);
		STAILQ_INIT(&oqpair->pending_rdma_send_queue);

		qpair = &oqpair->common.qpair;
		qpair->numa.id_valid = 1;
		qpair->numa.id = spdk_rdma_cm_id_get_numa_id(oqpair->cm_id);
	}
	qpair->transport = transport;
	/* use qid from the private data to determine the qpair type
	 * qid will be set to the appropriate value when the controller is created */
	qpair->qid = private_data->qid;
	qpair->qid = private_data->qid;
	event->id->context = qpair;

	spdk_nvmf_tgt_new_qpair(transport->tgt, qpair);

	return 0;
}

static inline void
nvmf_rdma_setup_wr(struct ibv_send_wr *wr, struct ibv_send_wr *next,
		   enum spdk_nvme_data_transfer xfer)
{
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		wr->opcode = IBV_WR_RDMA_WRITE;
		wr->send_flags = 0;
		wr->next = next;
	} else if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		wr->opcode = IBV_WR_RDMA_READ;
		wr->send_flags = IBV_SEND_SIGNALED;
		wr->next = NULL;
	} else {
		assert(0);
	}
}

static int
nvmf_request_alloc_wrs(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_request *rdma_req,
		       uint32_t num_sgl_descriptors)
{
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];
	struct spdk_nvmf_rdma_request_data	*current_data_wr;
	uint32_t				i;

	if (spdk_unlikely(num_sgl_descriptors > SPDK_NVMF_MAX_SGL_ENTRIES)) {
		SPDK_ERRLOG("Requested too much entries (%u), the limit is %u\n",
			    num_sgl_descriptors, SPDK_NVMF_MAX_SGL_ENTRIES);
		return -EINVAL;
	}

	if (spdk_unlikely(spdk_mempool_get_bulk(rtransport->data_wr_pool, (void **)work_requests,
						num_sgl_descriptors))) {
		return -ENOMEM;
	}

	current_data_wr = &rdma_req->data;

	for (i = 0; i < num_sgl_descriptors; i++) {
		nvmf_rdma_setup_wr(&current_data_wr->wr, &work_requests[i]->wr, rdma_req->common.req.xfer);
		current_data_wr->wr.next = &work_requests[i]->wr;
		current_data_wr = work_requests[i];
		current_data_wr->wr.sg_list = current_data_wr->sgl;
		current_data_wr->wr.wr_id = rdma_req->data.wr.wr_id;
	}

	nvmf_rdma_setup_wr(&current_data_wr->wr, &rdma_req->rsp.wr, rdma_req->common.req.xfer);

	return 0;
}

static inline void
nvmf_rdma_setup_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->common.req.cmd->nvme_cmd.dptr.sgl1;

	wr->wr.rdma.rkey = sgl->keyed.key;
	wr->wr.rdma.remote_addr = sgl->address;
	nvmf_rdma_setup_wr(wr, &rdma_req->rsp.wr, rdma_req->common.req.xfer);
}

static inline void
nvmf_rdma_update_remote_addr(struct spdk_nvmf_rdma_request *rdma_req, uint32_t num_wrs)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->common.req.cmd->nvme_cmd.dptr.sgl1;
	uint32_t			i;
	int				j;
	uint64_t			remote_addr_offset = 0;

	for (i = 0; i < num_wrs; ++i) {
		wr->wr.rdma.rkey = sgl->keyed.key;
		wr->wr.rdma.remote_addr = sgl->address + remote_addr_offset;
		for (j = 0; j < wr->num_sge; ++j) {
			remote_addr_offset += wr->sg_list[j].length;
		}
		wr = wr->next;
	}
}

static int
nvmf_rdma_fill_wr_sgl(struct spdk_nvmf_rdma_device *device,
		      struct spdk_nvmf_rdma_request *rdma_req,
		      struct ibv_send_wr *wr,
		      uint32_t total_length)
{
	struct spdk_rdma_utils_memory_translation mem_translation;
	struct ibv_sge	*sg_ele;
	struct iovec *iov;
	uint32_t lkey, remaining;
	int rc;

	wr->num_sge = 0;

	while (total_length && wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES) {
		iov = &rdma_req->common.req.iov[rdma_req->iovpos];
		rc = spdk_rdma_utils_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return rc;
		}

		lkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);
		sg_ele = &wr->sg_list[wr->num_sge];
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		sg_ele->lkey = lkey;
		sg_ele->addr = (uintptr_t)iov->iov_base + rdma_req->offset;
		sg_ele->length = remaining;
		SPDK_DEBUGLOG(rdma_offload, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele,
			      sg_ele->addr, sg_ele->length);
		rdma_req->offset += sg_ele->length;
		total_length -= sg_ele->length;
		wr->num_sge++;

		if (rdma_req->offset == iov->iov_len) {
			rdma_req->offset = 0;
			rdma_req->iovpos++;
		}
	}

	if (spdk_unlikely(total_length)) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;
}

static int
nvmf_rdma_fill_wr_sgl_with_dif(struct spdk_nvmf_rdma_device *device,
			       struct spdk_nvmf_rdma_request *rdma_req,
			       struct ibv_send_wr *wr,
			       uint32_t total_length,
			       uint32_t num_extra_wrs)
{
	struct spdk_rdma_utils_memory_translation mem_translation;
	struct spdk_dif_ctx *dif_ctx = &rdma_req->common.req.dif.dif_ctx;
	struct ibv_sge *sg_ele;
	struct iovec *iov;
	struct iovec *rdma_iov;
	uint32_t lkey, remaining;
	uint32_t remaining_data_block, data_block_size, md_size;
	uint32_t sge_len;
	int rc;

	data_block_size = dif_ctx->block_size - dif_ctx->md_size;

	if (spdk_likely(!rdma_req->common.req.stripped_data)) {
		rdma_iov = rdma_req->common.req.iov;
		remaining_data_block = data_block_size;
		md_size = dif_ctx->md_size;
	} else {
		rdma_iov = rdma_req->common.req.stripped_data->iov;
		total_length = total_length / dif_ctx->block_size * data_block_size;
		remaining_data_block = total_length;
		md_size = 0;
	}

	wr->num_sge = 0;

	while (total_length && (num_extra_wrs || wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES)) {
		iov = rdma_iov + rdma_req->iovpos;
		rc = spdk_rdma_utils_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return rc;
		}

		lkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);
		sg_ele = &wr->sg_list[wr->num_sge];
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		while (remaining) {
			if (wr->num_sge >= SPDK_NVMF_MAX_SGL_ENTRIES) {
				if (num_extra_wrs > 0 && wr->next) {
					wr = wr->next;
					wr->num_sge = 0;
					sg_ele = &wr->sg_list[wr->num_sge];
					num_extra_wrs--;
				} else {
					break;
				}
			}
			sg_ele->lkey = lkey;
			sg_ele->addr = (uintptr_t)((char *)iov->iov_base + rdma_req->offset);
			sge_len = spdk_min(remaining, remaining_data_block);
			sg_ele->length = sge_len;
			SPDK_DEBUGLOG(rdma_offload, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele,
				      sg_ele->addr, sg_ele->length);
			remaining -= sge_len;
			remaining_data_block -= sge_len;
			rdma_req->offset += sge_len;
			total_length -= sge_len;

			sg_ele++;
			wr->num_sge++;

			if (remaining_data_block == 0) {
				/* skip metadata */
				rdma_req->offset += md_size;
				total_length -= md_size;
				/* Metadata that do not fit this IO buffer will be included in the next IO buffer */
				remaining -= spdk_min(remaining, md_size);
				remaining_data_block = data_block_size;
			}

			if (remaining == 0) {
				/* By subtracting the size of the last IOV from the offset, we ensure that we skip
				   the remaining metadata bits at the beginning of the next buffer */
				rdma_req->offset -= spdk_min(iov->iov_len, rdma_req->offset);
				rdma_req->iovpos++;
			}
		}
	}

	if (spdk_unlikely(total_length)) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;
}

static inline uint32_t
nvmf_rdma_calc_num_wrs(uint32_t length, uint32_t io_unit_size, uint32_t block_size)
{
	/* estimate the number of SG entries and WRs needed to process the request */
	uint32_t num_sge = 0;
	uint32_t i;
	uint32_t num_buffers = SPDK_CEIL_DIV(length, io_unit_size);

	for (i = 0; i < num_buffers && length > 0; i++) {
		uint32_t buffer_len = spdk_min(length, io_unit_size);
		uint32_t num_sge_in_block = SPDK_CEIL_DIV(buffer_len, block_size);

		if (num_sge_in_block * block_size > buffer_len) {
			++num_sge_in_block;
		}
		num_sge += num_sge_in_block;
		length -= buffer_len;
	}
	return SPDK_CEIL_DIV(num_sge, SPDK_NVMF_MAX_SGL_ENTRIES);
}

static int
nvmf_rdma_request_fill_iovs(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_request		*req = &rdma_req->common.req;
	struct ibv_send_wr			*wr = &rdma_req->data.wr;
	int					rc;
	uint32_t				num_wrs = 1;
	uint32_t				length;

	rqpair = nvmf_rdma_qpair_get(req->qpair);
	rgroup = rqpair->poller->group;

	/* rdma wr specifics */
	nvmf_rdma_setup_request(rdma_req);

	length = req->length;
	if (spdk_unlikely(req->dif_enabled)) {
		req->dif.orig_length = length;
		length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
		req->dif.elba_length = length;
	}

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport,
					   length);
	if (spdk_unlikely(rc != 0)) {
		return rc;
	}

	assert(req->iovcnt <= rqpair->max_send_sge);

	/* When dif_insert_or_strip is true and the I/O data length is greater than one block,
	 * the stripped_buffers are got for DIF stripping. */
	if (spdk_unlikely(req->dif_enabled && (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST)
			  && (req->dif.elba_length > req->dif.dif_ctx.block_size))) {
		rc = nvmf_request_get_stripped_buffers(req, &rgroup->group,
						       &rtransport->transport, req->dif.orig_length);
		if (rc != 0) {
			SPDK_INFOLOG(rdma_offload, "Get stripped buffers fail %d, fallback to req.iov.\n", rc);
		}
	}

	rdma_req->iovpos = 0;

	if (spdk_unlikely(req->dif_enabled)) {
		num_wrs = nvmf_rdma_calc_num_wrs(length, rtransport->transport.opts.io_unit_size,
						 req->dif.dif_ctx.block_size);
		if (num_wrs > 1) {
			rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_wrs - 1);
			if (spdk_unlikely(rc != 0)) {
				goto err_exit;
			}
		}

		rc = nvmf_rdma_fill_wr_sgl_with_dif(device, rdma_req, wr, length, num_wrs - 1);
		if (spdk_unlikely(rc != 0)) {
			goto err_exit;
		}

		if (num_wrs > 1) {
			nvmf_rdma_update_remote_addr(rdma_req, num_wrs);
		}
	} else {
		rc = nvmf_rdma_fill_wr_sgl(device, rdma_req, wr, length);
		if (spdk_unlikely(rc != 0)) {
			goto err_exit;
		}
	}

	/* set the number of outstanding data WRs for this request. */
	rdma_req->num_outstanding_data_wr = num_wrs;

	return rc;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	req->iovcnt = 0;
	return rc;
}

static int
nvmf_rdma_request_fill_iovs_multi_sgl(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_device *device,
				      struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct ibv_send_wr			*current_wr;
	struct spdk_nvmf_request		*req = &rdma_req->common.req;
	struct spdk_nvme_sgl_descriptor		*inline_segment, *desc;
	uint32_t				num_sgl_descriptors;
	uint32_t				lengths[SPDK_NVMF_MAX_SGL_ENTRIES], total_length = 0;
	uint32_t				i;
	int					rc;

	rqpair = nvmf_rdma_qpair_get(rdma_req->common.req.qpair);
	rgroup = rqpair->poller->group;

	inline_segment = &req->cmd->nvme_cmd.dptr.sgl1;
	assert(inline_segment->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	assert(inline_segment->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);

	num_sgl_descriptors = inline_segment->unkeyed.length / sizeof(struct spdk_nvme_sgl_descriptor);
	assert(num_sgl_descriptors <= SPDK_NVMF_MAX_SGL_ENTRIES);

	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		if (spdk_likely(!req->dif_enabled)) {
			lengths[i] = desc->keyed.length;
		} else {
			req->dif.orig_length += desc->keyed.length;
			lengths[i] = spdk_dif_get_length_with_md(desc->keyed.length, &req->dif.dif_ctx);
			req->dif.elba_length += lengths[i];
		}
		total_length += lengths[i];
		desc++;
	}

	if (spdk_unlikely(total_length > rtransport->transport.opts.max_io_size)) {
		SPDK_ERRLOG("Multi SGL length 0x%x exceeds max io size 0x%x\n",
			    total_length, rtransport->transport.opts.max_io_size);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return -EINVAL;
	}

	rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_sgl_descriptors - 1);
	if (spdk_unlikely(rc != 0)) {
		return -ENOMEM;
	}

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport, total_length);
	if (spdk_unlikely(rc != 0)) {
		nvmf_rdma_request_free_data(rdma_req, rtransport);
		return rc;
	}

	/* When dif_insert_or_strip is true and the I/O data length is greater than one block,
	 * the stripped_buffers are got for DIF stripping. */
	if (spdk_unlikely(req->dif_enabled && (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST)
			  && (req->dif.elba_length > req->dif.dif_ctx.block_size))) {
		rc = nvmf_request_get_stripped_buffers(req, &rgroup->group,
						       &rtransport->transport, req->dif.orig_length);
		if (spdk_unlikely(rc != 0)) {
			SPDK_INFOLOG(rdma_offload, "Get stripped buffers fail %d, fallback to req.iov.\n", rc);
		}
	}

	/* The first WR must always be the embedded data WR. This is how we unwind them later. */
	current_wr = &rdma_req->data.wr;
	assert(current_wr != NULL);

	req->length = 0;
	rdma_req->iovpos = 0;
	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		/* The descriptors must be keyed data block descriptors with an address, not an offset. */
		if (spdk_unlikely(desc->generic.type != SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK ||
				  desc->keyed.subtype != SPDK_NVME_SGL_SUBTYPE_ADDRESS)) {
			rc = -EINVAL;
			goto err_exit;
		}

		if (spdk_likely(!req->dif_enabled)) {
			rc = nvmf_rdma_fill_wr_sgl(device, rdma_req, current_wr, lengths[i]);
		} else {
			rc = nvmf_rdma_fill_wr_sgl_with_dif(device, rdma_req, current_wr,
							    lengths[i], 0);
		}
		if (spdk_unlikely(rc != 0)) {
			rc = -ENOMEM;
			goto err_exit;
		}

		req->length += desc->keyed.length;
		current_wr->wr.rdma.rkey = desc->keyed.key;
		current_wr->wr.rdma.remote_addr = desc->address;
		current_wr = current_wr->next;
		desc++;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	/* Go back to the last descriptor in the list. */
	desc--;
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
		if (desc->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
			rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
			rdma_req->rsp.wr.imm_data = desc->keyed.key;
		}
	}
#endif

	rdma_req->num_outstanding_data_wr = num_sgl_descriptors;

	return 0;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	return rc;
}

static int
nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request		*req = &rdma_req->common.req;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;
	int					rc;
	uint32_t				length;

	rsp = &req->rsp->nvme_cpl;
	sgl = &req->cmd->nvme_cmd.dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {

		length = sgl->keyed.length;
		if (spdk_unlikely(length > rtransport->transport.opts.max_io_size)) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, rtransport->transport.opts.max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}
#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
		if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
			if (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
				rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
				rdma_req->rsp.wr.imm_data = sgl->keyed.key;
			}
		}
#endif

		/* fill request length and populate iovs */
		req->length = length;

		rc = nvmf_rdma_request_fill_iovs(rtransport, device, rdma_req);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EINVAL) {
				SPDK_ERRLOG("SGL length exceeds the max I/O size\n");
				rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				return -1;
			}
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(rdma_offload, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}

		SPDK_DEBUGLOG(rdma_offload, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = rtransport->transport.opts.in_capsule_data_size;

		SPDK_DEBUGLOG(nvmf, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, sgl->unkeyed.length);

		if (spdk_unlikely(offset > max_len)) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
			rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
			return -1;
		}
		max_len -= (uint32_t)offset;

		if (spdk_unlikely(sgl->unkeyed.length > max_len)) {
			SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
				    sgl->unkeyed.length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		rdma_req->num_outstanding_data_wr = 0;
		req->data_from_pool = false;
		req->length = sgl->unkeyed.length;

		req->iov[0].iov_base = rdma_req->recv->buf + offset;
		req->iov[0].iov_len = req->length;
		req->iovcnt = 1;

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {

		rc = nvmf_rdma_request_fill_iovs_multi_sgl(rtransport, device, rdma_req);
		if (spdk_unlikely(rc == -ENOMEM)) {
			SPDK_DEBUGLOG(rdma_offload, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		} else if (spdk_unlikely(rc == -EINVAL)) {
			SPDK_ERRLOG("Multi SGL element request length exceeds the max I/O size\n");
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		SPDK_DEBUGLOG(rdma_offload, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static void
_nvmf_rdma_request_free(struct spdk_nvmf_rdma_request *rdma_req,
			struct spdk_nvmf_rdma_transport	*rtransport)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;

	rqpair = nvmf_rdma_qpair_get(rdma_req->common.req.qpair);
	if (rdma_req->common.req.data_from_pool) {
		rgroup = rqpair->poller->group;

		spdk_nvmf_request_free_buffers(&rdma_req->common.req, &rgroup->group, &rtransport->transport);
	}
	if (rdma_req->common.req.stripped_data) {
		nvmf_request_free_stripped_buffers(&rdma_req->common.req,
						   &rqpair->poller->group->group,
						   &rtransport->transport);
	}
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	rdma_req->common.req.length = 0;
	rdma_req->common.req.iovcnt = 0;
	rdma_req->offset = 0;
	rdma_req->common.req.dif_enabled = false;
	rdma_req->fused_failed = false;
	rdma_req->transfer_wr = NULL;
	if (rdma_req->fused_pair) {
		/* This req was part of a valid fused pair, but failed before it got to
		 * READ_TO_EXECUTE state.  This means we need to fail the other request
		 * in the pair, because it is no longer part of a valid pair.  If the pair
		 * already reached READY_TO_EXECUTE state, we need to kick it.
		 */
		rdma_req->fused_pair->fused_failed = true;
		if (rdma_req->fused_pair->state == RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
			nvmf_rdma_request_process(rtransport, rdma_req->fused_pair);
		}
		rdma_req->fused_pair = NULL;
	}
	memset(&rdma_req->common.req.dif, 0, sizeof(rdma_req->common.req.dif));
	rqpair->qd--;

	STAILQ_INSERT_HEAD(&rqpair->resources->free_queue, rdma_req, state_link);
	rqpair->common.qpair.queue_depth--;
	rdma_req->state = RDMA_REQUEST_STATE_FREE;
}

static void
nvmf_rdma_check_fused_ordering(struct spdk_nvmf_rdma_transport *rtransport,
			       struct spdk_nvmf_rdma_qpair *rqpair,
			       struct spdk_nvmf_rdma_request *rdma_req)
{
	enum spdk_nvme_cmd_fuse last, next;

	last = rqpair->fused_first ? rqpair->fused_first->common.req.cmd->nvme_cmd.fuse :
	       SPDK_NVME_CMD_FUSE_NONE;
	next = rdma_req->common.req.cmd->nvme_cmd.fuse;

	assert(last != SPDK_NVME_CMD_FUSE_SECOND);

	if (spdk_likely(last == SPDK_NVME_CMD_FUSE_NONE && next == SPDK_NVME_CMD_FUSE_NONE)) {
		return;
	}

	if (last == SPDK_NVME_CMD_FUSE_FIRST) {
		if (next == SPDK_NVME_CMD_FUSE_SECOND) {
			/* This is a valid pair of fused commands.  Point them at each other
			 * so they can be submitted consecutively once ready to be executed.
			 */
			rqpair->fused_first->fused_pair = rdma_req;
			rdma_req->fused_pair = rqpair->fused_first;
			rqpair->fused_first = NULL;
			return;
		} else {
			/* Mark the last req as failed since it wasn't followed by a SECOND. */
			rqpair->fused_first->fused_failed = true;

			/* If the last req is in READY_TO_EXECUTE state, then call
			 * nvmf_rdma_request_process(), otherwise nothing else will kick it.
			 */
			if (rqpair->fused_first->state == RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
				nvmf_rdma_request_process(rtransport, rqpair->fused_first);
			}

			rqpair->fused_first = NULL;
		}
	}

	if (next == SPDK_NVME_CMD_FUSE_FIRST) {
		/* Set rqpair->fused_first here so that we know to check that the next request
		 * is a SECOND (and to fail this one if it isn't).
		 */
		rqpair->fused_first = rdma_req;
	} else if (next == SPDK_NVME_CMD_FUSE_SECOND) {
		/* Mark this req failed since it ia SECOND and the last one was not a FIRST. */
		rdma_req->fused_failed = true;
	}
}

static void
nvmf_rdma_poll_group_insert_need_buffer_req(struct spdk_nvmf_rdma_poll_group *rgroup,
		struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request *r;

	/* CONNECT commands have a timeout, so we need to avoid a CONNECT command
	 * from getting buried behind a long list of other non-FABRIC requests
	 * waiting for a buffer. Note that even though the CONNECT command's data is
	 * in-capsule, the request still goes to this STAILQ.
	 */
	if (spdk_likely(rdma_req->common.req.cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC)) {
		/* This is the most likely case. */
		STAILQ_INSERT_TAIL(&rgroup->group.pending_buf_queue, &rdma_req->common.req, buf_link);
		return;
	} else {
		/* STAILQ doesn't have INSERT_BEFORE, so we need to either INSERT_HEAD
		 * or INSERT_AFTER. Put it after any other FABRIC commands that are
		 * already in the queue.
		 */
		r = STAILQ_FIRST(&rgroup->group.pending_buf_queue);
		if (r == NULL || r->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC) {
			STAILQ_INSERT_HEAD(&rgroup->group.pending_buf_queue, &rdma_req->common.req, buf_link);
			return;
		}
		while (true) {
			struct spdk_nvmf_request *next;

			next = STAILQ_NEXT(r, buf_link);
			if (next == NULL || next->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC) {
				STAILQ_INSERT_AFTER(&rgroup->group.pending_buf_queue, r, &rdma_req->common.req, buf_link);
				return;
			}
			r = next;
		}
	}
}

static struct doca_mmap *
nvmf_rdma_create_doca_mmap(struct doca_dev *dev, void *addr, size_t len, int dmabuf_fd,
			   size_t dmabuf_offset)
{
	struct doca_mmap *mmap;
	uint32_t access_mask = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE;
	doca_error_t drc;

	drc = doca_mmap_create(&mmap);
	if (drc) {
		SPDK_ERRLOG("Failed to create doca_mmap, drc %d\n", drc);
		return NULL;
	}

	drc = doca_mmap_add_dev(mmap, dev);
	if (drc) {
		SPDK_ERRLOG("Failed to add device to doca_mmap, drc %d\n", drc);
		goto err;
	}

	if (dmabuf_fd < 0) {
		drc = doca_mmap_set_memrange(mmap, addr, len);
		if (drc) {
			SPDK_ERRLOG("Failed to set memrange for doca_mmap, drc %d\n", drc);
			goto err;
		}
	} else {
		access_mask |= DOCA_ACCESS_FLAG_PCI_RELAXED_ORDERING;
		drc = doca_mmap_set_dmabuf_memrange(mmap, dmabuf_fd, addr, dmabuf_offset, len);
		if (drc) {
			SPDK_ERRLOG("Failed to set dmabuf memrange for doca_mmap, drc %d\n", drc);
			goto err;
		}
	}

	drc = doca_mmap_set_permissions(mmap, access_mask);
	if (drc) {
		SPDK_ERRLOG("Failed to set premissions for doca_mmap, drc %d\n", drc);
		goto err;
	}

	drc = doca_mmap_start(mmap);
	if (drc) {
		SPDK_ERRLOG("Failed to start doca_mmap, drc %d\n", drc);
		goto err;
	}

	return mmap;

err:
	drc = doca_mmap_destroy(mmap);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to destroy doca_mmap: %s\n", doca_error_get_descr(drc));
	}
	return NULL;
}

bool
nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			  struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request	*req = &rdma_req->common.req;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvme_cpl		*rsp = &req->rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	enum spdk_nvmf_rdma_request_state prev_state;
	bool				progress = false;
	int				data_posted;
	uint32_t			num_blocks, num_rdma_reads_available, qdepth;

	rqpair = nvmf_rdma_qpair_get(req->qpair);
	device = rqpair->device;
	rgroup = rqpair->poller->group;

	assert(rdma_req->state != RDMA_REQUEST_STATE_FREE);

	/* If the queue pair is in an error state, force the request to the completed state
	 * to release resources. */
	if (spdk_unlikely(rqpair->ibv_in_error_state ||
			  !spdk_nvmf_qpair_is_active(&rqpair->common.qpair))) {
		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			STAILQ_REMOVE(&rgroup->group.pending_buf_queue, &req, spdk_nvmf_request, buf_link);
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			if (rdma_req->num_remaining_data_wr) {
				/* Partially sent request is still in the pending_rdma_read_queue,
				 * remove it before completing */
				rdma_req->num_remaining_data_wr = 0;
				STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			STAILQ_REMOVE(&rqpair->pending_rdma_send_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		default:
			break;
		}
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	}

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = rdma_req->state;

		SPDK_DEBUGLOG(rdma_offload, "Request %p entering state %d\n", rdma_req, prev_state);

		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair, rqpair->common.qpair.queue_depth);
			rdma_recv = rdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			req->cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
			memset(req->rsp, 0, sizeof(*req->rsp));
			rdma_req->transfer_wr = &rdma_req->data.wr;

			if (spdk_unlikely(rqpair->ibv_in_error_state ||
					  !spdk_nvmf_qpair_is_active(&rqpair->common.qpair))) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				break;
			}

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(req, &req->dif.dif_ctx))) {
				req->dif_enabled = true;
			}

			nvmf_rdma_check_fused_ordering(rtransport, rqpair, rdma_req);

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
			rdma_req->rsp.wr.opcode = IBV_WR_SEND;
			rdma_req->rsp.wr.imm_data = 0;
#endif

			/* The next state transition depends on the data transfer needs of this request. */
			req->xfer = spdk_nvmf_req_get_xfer(req);

			if (spdk_unlikely(req->xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				SPDK_DEBUGLOG(rdma_offload, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", rdma_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (req->xfer == SPDK_NVME_DATA_NONE) {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_NEED_BUFFER;
			nvmf_rdma_poll_group_insert_need_buffer_req(rgroup, rdma_req);
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEED_BUFFER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			assert(req->xfer != SPDK_NVME_DATA_NONE);

			if (req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
			if (spdk_unlikely(rc < 0)) {
				STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				break;
			}

			if (req->iovcnt == 0) {
				/* No buffers available. */
				rgroup->stat.pending_data_buffer++;
				break;
			}

			STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
			    req->data_from_pool) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_read_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_read_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			assert(rqpair->max_send_depth >= rqpair->current_send_depth);
			qdepth = rqpair->max_send_depth - rqpair->current_send_depth;
			assert(rqpair->max_read_depth >= rqpair->current_read_depth);
			num_rdma_reads_available = rqpair->max_read_depth - rqpair->current_read_depth;
			if (rdma_req->num_outstanding_data_wr > qdepth ||
			    rdma_req->num_outstanding_data_wr > num_rdma_reads_available) {
				if (num_rdma_reads_available && qdepth) {
					/* Send as much as we can */
					request_prepare_transfer_in_part(req, spdk_min(num_rdma_reads_available, qdepth));
				} else {
					/* We can only have so many WRs outstanding. we have to wait until some finish. */
					rqpair->poller->stat.pending_rdma_read++;
					break;
				}
			}

			/* We have already verified that this request is the head of the queue. */
			if (rdma_req->num_remaining_data_wr == 0) {
				STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_read_queue, state_link);
			}

			request_transfer_in(&rdma_req->common.req);
			rdma_req->state = RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;

			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (spdk_unlikely(req->dif_enabled)) {
				if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
					/* generate DIF for write operation */
					num_blocks = SPDK_CEIL_DIV(req->dif.elba_length, req->dif.dif_ctx.block_size);
					assert(num_blocks > 0);

					rc = spdk_dif_generate(req->iov, req->iovcnt, num_blocks, &req->dif.dif_ctx);
					if (rc != 0) {
						SPDK_ERRLOG("DIF generation failed\n");
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
						break;
					}
				}

				assert(req->dif.elba_length >= req->length);
				/* set extended length before IO operation */
				req->length = req->dif.elba_length;
			}

			if (req->cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
				if (rdma_req->fused_failed) {
					/* This request failed FUSED semantics.  Fail it immediately, without
					 * even sending it to the target layer.
					 */
					rsp->status.sct = SPDK_NVME_SCT_GENERIC;
					rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
					STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					break;
				}

				if (rdma_req->fused_pair == NULL ||
				    rdma_req->fused_pair->state != RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
					/* This request is ready to execute, but either we don't know yet if it's
					 * valid - i.e. this is a FIRST but we haven't received the next
					 * request yet or the other request of this fused pair isn't ready to
					 * execute.  So break here and this request will get processed later either
					 * when the other request is ready or we find that this request isn't valid.
					 */
					break;
				}
			}

			/* If we get to this point, and this request is a fused command, we know that
			 * it is part of valid sequence (FIRST followed by a SECOND) and that both
			 * requests are READY_TO_EXECUTE. So call spdk_nvmf_request_exec() both on this
			 * request, and the other request of the fused pair, in the correct order.
			 * Also clear the ->fused_pair pointers on both requests, since after this point
			 * we no longer need to maintain the relationship between these two requests.
			 */
			if (req->cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND) {
				assert(rdma_req->fused_pair != NULL);
				assert(rdma_req->fused_pair->fused_pair != NULL);
				rdma_req->fused_pair->state = RDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&rdma_req->fused_pair->common.req);
				rdma_req->fused_pair->fused_pair = NULL;
				rdma_req->fused_pair = NULL;
			}
			rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(req);
			if (req->cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST) {
				assert(rdma_req->fused_pair != NULL);
				assert(rdma_req->fused_pair->fused_pair != NULL);
				rdma_req->fused_pair->state = RDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&rdma_req->fused_pair->common.req);
				rdma_req->fused_pair->fused_pair = NULL;
				rdma_req->fused_pair = NULL;
			}
			break;
		case RDMA_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
			    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_write_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
			} else {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			}
			if (spdk_unlikely(req->dif_enabled)) {
				/* restore the original length */
				req->length = req->dif.orig_length;

				if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
					struct spdk_dif_error error_blk;

					num_blocks = SPDK_CEIL_DIV(req->dif.elba_length, req->dif.dif_ctx.block_size);
					if (!req->stripped_data) {
						rc = spdk_dif_verify(req->iov, req->iovcnt, num_blocks,
								     &req->dif.dif_ctx, &error_blk);
					} else {
						rc = spdk_dif_verify_copy(req->stripped_data->iov,
									  req->stripped_data->iovcnt,
									  req->iov, req->iovcnt, num_blocks,
									  &req->dif.dif_ctx, &error_blk);
					}
					if (rc) {
						struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

						SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n", error_blk.err_type,
							    error_blk.err_offset);
						rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
						rsp->status.sc = nvmf_rdma_dif_error_to_compl_status(error_blk.err_type);
						STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
						STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req, state_link);
						rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					}
				}
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_write_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			if ((rqpair->current_send_depth + rdma_req->num_outstanding_data_wr + 1) >
			    rqpair->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish.
				 * +1 since each request has an additional wr in the resp. */
				rqpair->poller->stat.pending_rdma_write++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_write_queue, state_link);

			/* The data transfer will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 * We verified that data + response fit into send queue, so we can go to the next state directly
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_send_queue)) {
				/* This request needs to wait in line to send the completion */
				break;
			}

			assert(rqpair->current_send_depth <= rqpair->max_send_depth);
			if (rqpair->current_send_depth == rqpair->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish */
				rqpair->poller->stat.pending_rdma_send++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_send_queue, state_link);

			/* The response sending will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			rc = request_transfer_out(&rdma_req->common.req, &data_posted);
			assert(rc == 0); /* No good way to handle this currently */
			if (spdk_unlikely(rc)) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			} else {
				rdma_req->state = data_posted ? RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST :
						  RDMA_REQUEST_STATE_COMPLETING;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair, rqpair->common.qpair.queue_depth);

			rqpair->poller->stat.request_latency += spdk_get_ticks() - rdma_req->receive_tsc;
			_nvmf_rdma_request_free(rdma_req, rtransport);
			break;
		case RDMA_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (rdma_req->state != prev_state) {
			progress = true;
		}
	} while (rdma_req->state != prev_state);

	return progress;
}

static int
nvmf_non_offload_request_parse_sgl(struct nvmf_non_offload_request *non_offload_req)
{
	struct spdk_nvmf_request	*req = &non_offload_req->common.req;
	struct spdk_nvmf_transport	*transport = req->qpair->transport;
	struct spdk_nvme_cpl		*rsp;
	struct spdk_nvme_sgl_descriptor	*sgl;
	uint32_t			length;
	uint32_t			max_len;

	rsp = &req->rsp->nvme_cpl;
	sgl = &req->cmd->nvme_cmd.dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {

		max_len = spdk_min(transport->opts.max_io_size, non_offload_req->payload_len);
		length = sgl->keyed.length;
		if (spdk_unlikely(length > max_len)) {
			SPDK_ERRLOG("SGL length 0x%x exceeds the max I/O size 0x%x\n", length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}
		req->length = length;
		req->iov[0].iov_base = non_offload_req->payload;
		req->iov[0].iov_len = length;
		req->iovcnt = 1;

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;

		SPDK_DEBUGLOG(rdma_offload, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, sgl->unkeyed.length);

		max_len = spdk_min(transport->opts.in_capsule_data_size, non_offload_req->payload_len);
		if (spdk_unlikely(offset > max_len)) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
		}
		max_len -= (uint32_t)offset;

		if (spdk_unlikely(sgl->unkeyed.length > max_len)) {
			SPDK_ERRLOG("In-capsule + SGL length 0x%lx exceeds the maximum size 0x%x\n",
				    offset + sgl->unkeyed.length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		req->length = sgl->unkeyed.length;
		req->iov[0].iov_base = non_offload_req->payload + offset;
		req->iov[0].iov_len = req->length;
		req->iovcnt = 1;

		return 0;
	}
	/* SPDK_NVME_SGL_TYPE_LAST_SEGMENT is not implemented here because DOCA STA does not support it.
	 * MSDBD is set to 1 to prevent an initiator from using the unsupported SGL type.
	 */

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static struct spdk_nvmf_rdma_subsystem *
nvmf_rdma_subsystem_find(struct spdk_nvmf_rdma_transport *rtransport,
			 const struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_rdma_subsystem *rsubsystem;

	TAILQ_FOREACH(rsubsystem, &rtransport->subsystems, link) {
		if (subsystem_cmp(rsubsystem->subsystem, subsystem) == 0) {
			break;
		}
	}

	return rsubsystem;
}

static int
nvmf_sta_fabric_connect(struct nvmf_non_offload_request *non_offload_req)
{
	struct spdk_nvmf_request *req = &non_offload_req->common.req;
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_data *data = req->iov[0].iov_base;
	struct spdk_nvmf_offload_qpair *oqpair = nvmf_offload_qpair_get(req->qpair);
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_rdma_subsystem *rsubsystem;
	doca_error_t drc;

	assert(cmd->opcode == SPDK_NVME_OPC_FABRIC);
	assert(cmd->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT);

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		return -1;
	}

	if (req->iovcnt > 1) {
		SPDK_ERRLOG("Connect command invalid iovcnt: %d\n", req->iovcnt);
		return -1;
	}

	oqpair = nvmf_offload_qpair_get(req->qpair);
	rtransport = SPDK_CONTAINEROF(req->qpair->transport, struct spdk_nvmf_rdma_transport, transport);

	subsystem = spdk_nvmf_tgt_find_subsystem(rtransport->transport.tgt, data->subnqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem is not found for nqn %s\n", data->subnqn);
		return -1;
	}

	rsubsystem = nvmf_rdma_subsystem_find(rtransport, subsystem);
	if (!rsubsystem) {
		SPDK_ERRLOG("Offload subsystem is not found for nqn %s\n", data->subnqn);
		return -1;
	}

	if (oqpair->rsubsystem) {
		if (oqpair->rsubsystem != rsubsystem) {
			SPDK_ERRLOG("Failed to assign QP to subsystem %s: is already assign to subsystem %s\n",
				    rsubsystem->subsystem->subnqn, oqpair->rsubsystem->subsystem->subnqn);
			return -1;
		}
	} else {
		oqpair->rsubsystem = rsubsystem;
		drc = doca_sta_io_qp_add_subsystem(oqpair->opoller->sta_io, rsubsystem->handle, oqpair->handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to add qpair to subsystem: %s\n", doca_error_get_descr(drc));
			return -1;
		}
	}

	/*
	 * The SQ size is zero-based in the command, but doca_sta_io_qp_connect_set_sq_size() expects a regular
	 * (non-zero-based) value.
	 */
	drc = doca_sta_io_qp_connect_set_sq_size(oqpair->opoller->sta_io, oqpair->handle, cmd->sqsize + 1);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to set sq_size for qpair: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	return 0;
}

static inline doca_error_t
submit_doca_task(struct doca_task *task)
{
	doca_error_t drc;

	/*
	 * The submit task function can return DOCA_ERROR_AGAIN when several
	 * threads submit tasks to the same DPA EU. Multiple submit calls are
	 * synchronized by atomic operations. Immediate retry is a recommended
	 * way to handle DOCA_ERROR_AGAIN.
	 */
	do {
		drc = doca_task_submit(task);
	} while (drc == DOCA_ERROR_AGAIN);

	return drc;
}

static int
nvmf_non_offload_request_transfer_in(struct nvmf_non_offload_request *non_offload_req)
{
	struct spdk_nvmf_offload_qpair	*oqpair;
	union doca_data			task_user_data;
	doca_error_t			drc;

	oqpair = nvmf_offload_qpair_get(non_offload_req->common.req.qpair);
	task_user_data.ptr = non_offload_req;

	SPDK_DEBUGLOG(rdma_offload, "RDMA_READ task, req %p\n", non_offload_req);
	drc = doca_sta_io_task_non_offload_rdma_read_alloc_init(oqpair->opoller->sta_io,
			task_user_data,
			oqpair->handle,
			non_offload_req->sta_context,
			&non_offload_req->task);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to allocate RDMA_READ task: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = submit_doca_task(doca_sta_producer_send_task_as_task(non_offload_req->task));
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to submit RDMA_READ task: %s\n", doca_error_get_descr(drc));
		doca_task_free(doca_sta_producer_send_task_as_task(non_offload_req->task));
		return -1;
	}

	return 0;
}

static int
nvmf_non_offload_request_transfer_out(struct nvmf_non_offload_request *non_offload_req,
				      int *data_posted)
{
	struct spdk_nvmf_request	*req;
	struct spdk_nvme_cpl		*rsp;
	struct spdk_nvmf_offload_qpair	*oqpair;
	union doca_data			task_user_data;
	doca_error_t			drc;

	*data_posted = 0;
	req = &non_offload_req->common.req;
	rsp = &req->rsp->nvme_cpl;
	oqpair = nvmf_offload_qpair_get(req->qpair);
	task_user_data.ptr = non_offload_req;

	SPDK_STATIC_ASSERT(sizeof(*rsp) == sizeof(doca_sta_nvmef_completion_t), "size mismatch");

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		*data_posted = 1;
		SPDK_DEBUGLOG(rdma_offload, "RDMA_WRITE and SEND task, req %p\n", non_offload_req);
		drc = doca_sta_io_task_non_offload_rdma_write_send_alloc_init(oqpair->opoller->sta_io,
				task_user_data,
				oqpair->handle,
				(uint8_t *)rsp,
				non_offload_req->sta_context,
				&non_offload_req->task);
	} else {
		SPDK_DEBUGLOG(rdma_offload, "SEND task, req %p\n", non_offload_req);
		drc = doca_sta_io_task_non_offload_rdma_send_alloc_init(oqpair->opoller->sta_io,
				task_user_data,
				oqpair->handle,
				(uint8_t *)rsp,
				non_offload_req->sta_context,
				&non_offload_req->task);
	}
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to allocate task: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = submit_doca_task(doca_sta_producer_send_task_as_task(non_offload_req->task));
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to submit RDMA_READ task: %s\n", doca_error_get_descr(drc));
		doca_task_free(doca_sta_producer_send_task_as_task(non_offload_req->task));
		return -1;
	}

	return 0;
}

static int nvmf_non_offload_request_free(struct nvmf_non_offload_request *non_offload_req);

static bool
nvmf_sta_io_non_offload_request_process(struct nvmf_non_offload_request *non_offload_req)
{
	struct spdk_nvmf_request		*req = &non_offload_req->common.req;
	struct spdk_nvme_cpl			*rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_offload_qpair		*oqpair = nvmf_offload_qpair_get(req->qpair);
	enum spdk_nvmf_rdma_request_state	prev_state;
	int					data_posted;
	bool					progress = false;
	int					rc;

	assert(non_offload_req->state != RDMA_REQUEST_STATE_FREE);

	if (spdk_unlikely(oqpair->in_error_state || !spdk_nvmf_qpair_is_active(&oqpair->common.qpair))) {
		switch (non_offload_req->state) {
		/* The request cannot be completed while a non-offload task is in progress. */
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
		case RDMA_REQUEST_STATE_COMPLETING:
		/* The I/O buffer provided by DOCA STA might be in use while the bdev is executing the request.
		 * That's why the request cannot be completed at this stage.
		 */
		case RDMA_REQUEST_STATE_EXECUTING:
			goto end_processing;
		/* Free the request in the regular way because it is already completed. */
		case RDMA_REQUEST_STATE_COMPLETED:
			goto start_processing;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			STAILQ_REMOVE(&oqpair->pending_rdma_read_queue, non_offload_req,
				      nvmf_non_offload_request, state_link);
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			STAILQ_REMOVE(&oqpair->pending_rdma_write_queue, non_offload_req,
				      nvmf_non_offload_request, state_link);
			break;
		default:
			break;
		}
		/* DOCA STA cannot complete the non-offload I/O while it is in use by the software.
		 * Submit a send task with an error status to allow DOCA STA to complete the I/O.
		 */
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		if (non_offload_req->state != RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING) {
			STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req, state_link);
			non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
		}
	}

start_processing:
	do {
		prev_state = non_offload_req->state;

		SPDK_DEBUGLOG(rdma_offload, "Request %p entering state %d\n", non_offload_req, prev_state);

		switch (non_offload_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			req->cmd = (union nvmf_h2c_msg *)non_offload_req->nvme_cmd;
			memset(req->rsp, 0, sizeof(*req->rsp));

			req->xfer = spdk_nvmf_req_get_xfer(req);
			if (spdk_unlikely(req->xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req, state_link);
				non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				SPDK_DEBUGLOG(rdma_offload, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", non_offload_req);
				break;
			}
			if (req->xfer == SPDK_NVME_DATA_NONE) {
				SPDK_DEBUGLOG(rdma_offload, "SPDK_NVME_DATA_NONE\n");
				non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}
			if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && non_offload_req->payload_valid) {
				SPDK_DEBUGLOG(rdma_offload, "SPDK_NVME_DATA_HOST_TO_CONTROLLER: payload_valid\n");
				/* DOCA STA already executed the data transfer. */
				req->length		= non_offload_req->payload_len;
				req->iov[0].iov_base	= non_offload_req->payload;
				req->iov[0].iov_len	= non_offload_req->payload_len;
				req->iovcnt		= 1;
				non_offload_req->state	= RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			non_offload_req->state = RDMA_REQUEST_STATE_NEED_BUFFER;
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_NEED_BUFFER, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			assert(req->xfer != SPDK_NVME_DATA_NONE);
			/*
			 * DOCA STA provides a buffer for each non-offloaded IO.
			 * Parse SGL segment to calculate a length of the payload and
			 * add the given IO buffer to the request IOV.
			 */
			rc = nvmf_non_offload_request_parse_sgl(non_offload_req);
			if (rc < 0) {
				STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req, state_link);
				non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				break;
			}
			if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				STAILQ_INSERT_TAIL(&oqpair->pending_rdma_read_queue, non_offload_req, state_link);
				non_offload_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
				break;
			}

			non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			if (non_offload_req != STAILQ_FIRST(&oqpair->pending_rdma_read_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&oqpair->pending_rdma_read_queue, state_link);

			rc = nvmf_non_offload_request_transfer_in(non_offload_req);
			if (!rc) {
				non_offload_req->state = RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;
			} else {
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req, state_link);
				non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
				struct spdk_nvmf_capsule_cmd *cap_hdr;

				cap_hdr = &req->cmd->nvmf_cmd;
				if (cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
					rc = nvmf_sta_fabric_connect(non_offload_req);
					if (rc) {
						rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
						STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req,
								   state_link);
						non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
						break;
					}
				}
			}

			non_offload_req->state = RDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(req);
			break;
		case RDMA_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTING, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);
			if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
			    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				STAILQ_INSERT_TAIL(&oqpair->pending_rdma_write_queue, non_offload_req, state_link);
				non_offload_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
			} else {
				STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req, state_link);
				non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			if (non_offload_req != STAILQ_FIRST(&oqpair->pending_rdma_write_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&oqpair->pending_rdma_write_queue, state_link);

			/* The data transfer will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 * We verified that data + response fit into send queue,
			 * so we can go to the next state directly
			 */
			non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE_PENDING, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			if (non_offload_req != STAILQ_FIRST(&oqpair->pending_rdma_send_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&oqpair->pending_rdma_send_queue, state_link);

			non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);
			rc = nvmf_non_offload_request_transfer_out(non_offload_req, &data_posted);
			assert(rc == 0); /* No good way to handle this currently */
			if (rc) {
				non_offload_req->state = RDMA_REQUEST_STATE_COMPLETED;
			} else {
				non_offload_req->state = data_posted ?
							 RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST :
							 RDMA_REQUEST_STATE_COMPLETING;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_RDMA_OFFLOAD_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)non_offload_req, (uintptr_t)oqpair);

			nvmf_non_offload_request_free(non_offload_req);
			break;
		case RDMA_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (non_offload_req->state != prev_state) {
			progress = true;
		}
	} while (non_offload_req->state != prev_state);

end_processing:
	return progress;
}

/* Public API callbacks begin here */

#define SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE (SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE UINT32_MAX
#define SPDK_NVMF_RDMA_DEFAULT_NO_SRQ false
#define SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG 100
#define SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING false
#define SPDK_NVMF_RDMA_DEFAULT_DATA_WR_POOL_SIZE 4095
#define SPDK_NVMF_RDMA_DEFAULT_DOCA_DEVICE "mlx5_0"
#define SPDK_NVMF_RDMA_DEFAULT_RDMA_DEVICE "mlx5_2"
/* MSDBD is limited by DOCA STA. */
#define SPDK_NVMF_RDMA_MSDBD 1

static void
nvmf_rdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->msdbd =			SPDK_NVMF_RDMA_MSDBD;
	opts->transport_specific =      NULL;
	opts->data_wr_pool_size	=	SPDK_NVMF_RDMA_DEFAULT_DATA_WR_POOL_SIZE;
}

static int nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
			     spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

static inline bool
nvmf_rdma_is_rxe_device(struct spdk_nvmf_rdma_device *device)
{
	return device->attr.vendor_id == SPDK_RDMA_PROVIDER_RXE_VENDOR_ID_OLD ||
	       device->attr.vendor_id == SPDK_RDMA_PROVIDER_RXE_VENDOR_ID_NEW;
}

static int nvmf_rdma_accept(void *ctx);
static bool nvmf_rdma_retry_listen_port(struct spdk_nvmf_rdma_transport *rtransport);
static void destroy_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
			      struct spdk_nvmf_rdma_device *device);

static struct doca_dev *
open_doca_sta_dev(const char *ibdev_name)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	struct doca_devinfo *devinfo = NULL;
	char devinfo_ibvdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE];
	struct doca_dev *dev = NULL;
	doca_error_t ret;
	uint32_t i;

	ret = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (DOCA_IS_ERROR(ret)) {
		SPDK_ERRLOG("doca_devinfo_create_list(): %s\n", doca_error_get_descr(ret));
		return NULL;
	}

	for (i = 0; i < nb_devs; i++) {
		ret = doca_devinfo_get_ibdev_name(dev_list[i], devinfo_ibvdev_name,
						  DOCA_DEVINFO_IBDEV_NAME_SIZE);
		if (DOCA_IS_ERROR(ret)) {
			SPDK_ERRLOG("doca_devinfo_get_ibdev_name(): %s\n", doca_error_get_descr(ret));
			goto destroy_list_and_exit;
		}
		if (strncmp(ibdev_name, devinfo_ibvdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == 0) {
			devinfo = dev_list[i];
			break;
		}
	}
	if (!devinfo) {
		SPDK_ERRLOG("DOCA device %s is not found\n", ibdev_name);
		goto destroy_list_and_exit;
	}

	ret = doca_sta_cap_is_supported(devinfo);
	if (DOCA_IS_ERROR(ret)) {
		SPDK_NOTICELOG("DOCA device %s does not support STA capabilties\n", ibdev_name);
		goto destroy_list_and_exit;
	}

	ret = doca_dev_open(devinfo, &dev);
	if (DOCA_IS_ERROR(ret)) {
		SPDK_ERRLOG("doca_dev_open(): %s\n", doca_error_get_descr(ret));
	}

destroy_list_and_exit:
	doca_devinfo_destroy_list(dev_list);
	return dev;
}

static int
create_ib_device(struct spdk_nvmf_rdma_transport *rtransport, struct ibv_context *context,
		 struct spdk_nvmf_rdma_device **new_device)
{
	struct spdk_nvmf_rdma_device	*device;
	int				flag = 0;
	int				rc = 0;

	device = calloc(1, sizeof(*device));
	if (!device) {
		SPDK_ERRLOG("Unable to allocate memory for RDMA devices.\n");
		return -ENOMEM;
	}
	device->context = context;
	rc = ibv_query_device(device->context, &device->attr);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		free(device);
		return rc;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) == 0) {
		SPDK_WARNLOG("The libibverbs on this system supports SEND_WITH_INVALIDATE,");
		SPDK_WARNLOG("but the device with vendor ID %u does not.\n", device->attr.vendor_id);
	}

	/**
	 * The vendor ID is assigned by the IEEE and an ID of 0 implies Soft-RoCE.
	 * The Soft-RoCE RXE driver does not currently support send with invalidate,
	 * but incorrectly reports that it does. There are changes making their way
	 * through the kernel now that will enable this feature. When they are merged,
	 * we can conditionally enable this feature.
	 *
	 * TODO: enable this for versions of the kernel rxe driver that support it.
	 */
	if (nvmf_rdma_is_rxe_device(device)) {
		device->attr.device_cap_flags &= ~(IBV_DEVICE_MEM_MGT_EXTENSIONS);
	}
#endif

	device->doca_dev = open_doca_sta_dev(ibv_get_device_name(context->device));
	if (!device->doca_dev) {
		free(device);
		return -EINVAL;
	}

	/* set up device context async ev fd as NON_BLOCKING */
	flag = fcntl(device->context->async_fd, F_GETFL);
	rc = fcntl(device->context->async_fd, F_SETFL, flag | O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to set context async fd to NONBLOCK.\n");
		free(device);
		return rc;
	}

	TAILQ_INSERT_TAIL(&rtransport->devices, device, link);
	SPDK_DEBUGLOG(rdma_offload, "New device %p is added to RDMA transport\n", device);

	device->pd = spdk_rdma_utils_get_pd(device->context);

	if (!device->pd) {
		SPDK_ERRLOG("Unable to allocate protection domain.\n");
		destroy_ib_device(rtransport, device);
		return -ENOMEM;
	}

	assert(device->map == NULL);

	device->map = spdk_rdma_utils_create_mem_map(device->pd, NULL, IBV_ACCESS_LOCAL_WRITE);
	if (!device->map) {
		SPDK_ERRLOG("Unable to allocate memory map for listen address\n");
		destroy_ib_device(rtransport, device);
		return -ENOMEM;
	}

	assert(device->map != NULL);
	assert(device->pd != NULL);

	if (new_device) {
		*new_device = device;
	}
	SPDK_NOTICELOG("Create IB device %s(%p/%p) succeed.\n", ibv_get_device_name(context->device),
		       device, context);

	return 0;
}

static void
free_poll_fds(struct spdk_nvmf_rdma_transport *rtransport)
{
	if (rtransport->poll_fds) {
		free(rtransport->poll_fds);
		rtransport->poll_fds = NULL;
	}
	rtransport->npoll_fds = 0;
}

static int
generate_poll_fds(struct spdk_nvmf_rdma_transport *rtransport)
{
	/* Set up poll descriptor array to monitor events from RDMA and IB
	 * in a single poll syscall
	 */
	int device_count = 0;
	int i = 0;
	struct spdk_nvmf_rdma_device *device, *tmp;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		device_count++;
	}

	rtransport->npoll_fds = device_count + 1;

	rtransport->poll_fds = calloc(rtransport->npoll_fds, sizeof(struct pollfd));
	if (rtransport->poll_fds == NULL) {
		SPDK_ERRLOG("poll_fds allocation failed\n");
		return -ENOMEM;
	}

	rtransport->poll_fds[i].fd = rtransport->event_channel->fd;
	rtransport->poll_fds[i++].events = POLLIN;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		rtransport->poll_fds[i].fd = device->context->async_fd;
		rtransport->poll_fds[i++].events = POLLIN;
	}

	return 0;
}

static const char *
nvmf_rdma_sta_state_to_str(enum doca_ctx_states state)
{
	static const char *state_str[DOCA_CTX_STATE_STOPPING + 1] = {
		[DOCA_CTX_STATE_IDLE] = "IDLE",
		[DOCA_CTX_STATE_STARTING] = "STARTING",
		[DOCA_CTX_STATE_RUNNING] = "RUNNING",
		[DOCA_CTX_STATE_STOPPING] = "STOPPING"
	};

	return (state <= DOCA_CTX_STATE_STOPPING) ? state_str[state] : "UNKNOWN";
}

static void
nvmf_rdma_sta_state_changed_cb(const union doca_data user_data,
			       struct doca_ctx *ctx,
			       enum doca_ctx_states prev_state,
			       enum doca_ctx_states next_state)
{
	struct spdk_nvmf_rdma_transport *rtransport = user_data.ptr;

	SPDK_DEBUGLOG(rdma_offload, "DOCA STA Context state is chnaged %s -> %s\n",
		      nvmf_rdma_sta_state_to_str(prev_state),
		      nvmf_rdma_sta_state_to_str(next_state));

	rtransport->sta.state = next_state;
}


static void
sta_offload_task_detach_ns_complete(struct doca_sta_producer_task_send *task,
				    union doca_data task_user_data)
{
	struct spdk_nvmf_rdma_ns *rns = task_user_data.ptr;

	doca_task_free(doca_sta_producer_send_task_as_task(task));
	rns->delete_completed = true;
	rns->handle = 0;
}

static void
sta_offload_task_detach_ns_complete_err(struct doca_sta_producer_task_send *task,
					union doca_data task_user_data)
{
	struct spdk_nvmf_rdma_ns *rns = task_user_data.ptr;

	doca_task_free(doca_sta_producer_send_task_as_task(task));
	rns->delete_failed = true;
	rns->delete_completed = true;
}

static void
sta_offload_task_destroy_bqueue_complete(struct doca_sta_producer_task_send *task,
		union doca_data task_user_data)
{
	struct spdk_nvmf_rdma_bdev_queue_destroy_ctx *destroy_ctx = task_user_data.ptr;

	doca_task_free(doca_sta_producer_send_task_as_task(task));
	destroy_ctx->destroy_completed = true;
}

static void
sta_offload_task_destroy_bqueue_complete_err(struct doca_sta_producer_task_send *task,
		union doca_data task_user_data)
{
	struct spdk_nvmf_rdma_bdev_queue_destroy_ctx *destroy_ctx = task_user_data.ptr;

	doca_task_free(doca_sta_producer_send_task_as_task(task));
	destroy_ctx->destroy_failed = true;
	destroy_ctx->destroy_completed = true;
}

static void
nvmf_sta_event_be_timeout_cb(const struct doca_sta_event_be_timeout *event,
			     union doca_data user_data)
{
	SPDK_ERRLOG("nvmf_sta_event_be_timeout_cb\n");
}

static void
nvmf_sta_event_eu_err_cb(const struct doca_sta_event_eu_err *event, union doca_data user_data)
{
	bool is_fatal;
	doca_error_t drc;

	drc = doca_sta_event_eu_err_is_fatal_error(event, &is_fatal);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("EU error occurred\n");
		return;
	}

	if (is_fatal) {
		SPDK_ERRLOG("EU fatal error occurred\n");
	} else {
		SPDK_ERRLOG("EU non-fatal error occurred\n");
	}
}

static void *
nvmf_sta_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *addr;

	addr = spdk_zmalloc(size, align, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!addr) {
		return NULL;
	}

	*phys_addr = spdk_vtophys(addr, NULL);
	if (*phys_addr == SPDK_VTOPHYS_ERROR) {
		spdk_free(addr);
		addr = NULL;
	}

	return addr;
}

static void
nvmf_sta_free(void *buf)
{
	spdk_free(buf);
}

static uint64_t
nvmf_sta_vtophys(const void *buf, uint32_t size)
{
	uint64_t translated_size = size;
	uint64_t addr;

	addr = spdk_vtophys(buf, &translated_size);

	if (addr == SPDK_VTOPHYS_ERROR || translated_size != size) {
		return DOCA_STA_VTOPHYS_ERROR;
	}

	return addr;
}

static int
nvmf_rdma_sta_get_caps(struct doca_sta *sta, struct nvmf_rdma_sta_caps *caps)
{
	doca_error_t drc;

	drc = doca_sta_get_max_devs(&caps->max_devs);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_devs(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_num_eus_available(sta, &caps->max_eus);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_num_eus_available(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_num_connected_qp_per_eu(sta, &caps->max_connected_qps);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_num_connected_qp_per_eu(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_subsys(&caps->max_subsys);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_subsys(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_io_threads(&caps->max_io_threads);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_io_threads(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_io_size(&caps->max_io_size);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_io_size(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_io_num_per_dev(sta, &caps->max_ios);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_io_num_per_dev(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_io_queue_size(sta, &caps->max_io_queue_size);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_io_queue_size(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_min_ioccsz(&caps->min_ioccsz);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_min_ioccsz(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_ioccsz(&caps->max_ioccsz);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_ioccsz(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_min_iorcsz(&caps->min_iorcsz);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_min_iorcsz(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_iorcsz(&caps->max_iorcsz);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_iorcsz(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_icdoff(&caps->max_icdoff);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_icdoff(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_be(&caps->max_be);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_be(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	drc = doca_sta_get_max_qs_per_be(&caps->max_qs_per_be);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("doca_sta_cap_get_max_qs_per_be(): %s\n", doca_error_get_descr(drc));
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_sta_create(struct spdk_nvmf_rdma_transport *rtransport)
{
	doca_error_t rc;

	TAILQ_INIT(&rtransport->sta.bdevs);

	rtransport->sta.dev = open_doca_sta_dev(rtransport->rdma_opts.doca_device);
	if (!rtransport->sta.dev) {
		return -ENODEV;
	}

	/* Create a new DOCA STA Context */
	rc = doca_sta_create(rtransport->sta.dev, &rtransport->sta.sta);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("Unable to create DOCA STA Context for device %s: %s\n",
			    rtransport->rdma_opts.doca_device,
			    doca_error_get_descr(rc));
		return -EINVAL;
	}
	SPDK_NOTICELOG("Create DOCA STA Context for device %s\n",
		       rtransport->rdma_opts.doca_device);

	if (nvmf_rdma_sta_get_caps(rtransport->sta.sta, &rtransport->sta.caps)) {
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_sta_progress(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	uint8_t rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rc = doca_pe_progress(rtransport->sta.pe);

	return (rc == 0) ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

static doca_error_t
nvmf_rdma_sta_get_num_cq_handlers(struct spdk_nvmf_rdma_sta *sta)
{
	struct doca_sta_eu_handle **eu_handles;
	uint32_t num_eu_handles, num_cq_handles, i;
	enum dpa_sta_eu_type eu_type;
	doca_error_t drc;

	num_eu_handles = sta->caps.max_eus;
	eu_handles = calloc(num_eu_handles, sizeof(*eu_handles));
	if (!eu_handles) {
		return DOCA_ERROR_NO_MEMORY;
	}

	drc = doca_sta_get_eu_handle(sta->sta, eu_handles, &num_eu_handles);
	if (DOCA_IS_ERROR(drc)) {
		goto cleanup;
	}

	num_cq_handles = 0;
	for (i = 0; i < num_eu_handles; i++) {
		drc = doca_sta_get_eu_type(eu_handles[i], &eu_type);
		if (DOCA_IS_ERROR(drc)) {
			goto cleanup;
		}
		if (eu_type == DOCA_STA_EU_COMP) {
			num_cq_handles++;
		}
	}
	sta->num_cq_handlers = num_cq_handles;
cleanup:
	free(eu_handles);
	return drc;
}

static int
nvmf_rdma_sta_start(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_device *device;
	union doca_data udata;
	doca_error_t rc;

	rtransport->sta.ctx = doca_sta_as_ctx(rtransport->sta.sta);
	if (!rtransport->sta.ctx) {
		SPDK_ERRLOG("Unable to get context for DOCA STA\n");
		return -EINVAL;
	}

	rc = doca_pe_create(&rtransport->sta.pe);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("doca_pe_create(): %s\n", doca_error_get_descr(rc));
		return -EINVAL;
	}

	rc = doca_pe_connect_ctx(rtransport->sta.pe, rtransport->sta.ctx);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("doca_pe_connect_ctx(): %s\n", doca_error_get_descr(rc));
		return -EINVAL;
	}

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		/* Add device to the existing DOCA STA Context */
		rc = doca_sta_add_dev(rtransport->sta.sta, device->doca_dev);
		if (DOCA_IS_ERROR(rc)) {
			SPDK_ERRLOG("Unable to add RDMA device %s to DOCA STA Context: %s\n",
				    ibv_get_device_name(device->context->device),
				    doca_error_get_descr(rc));
			return -EINVAL;
		}
		SPDK_NOTICELOG("Add RDMA device %s to DOCA STA Context\n",
			       ibv_get_device_name(device->context->device));
	}

	rtransport->sta.state = DOCA_CTX_STATE_IDLE;
	udata.ptr = rtransport;

	rc = doca_ctx_set_user_data(rtransport->sta.ctx, udata);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("Unable to set user data for DOCA STA: %s\n", doca_error_get_descr(rc));
		return -EINVAL;
	}

	rc = doca_ctx_set_state_changed_cb(rtransport->sta.ctx, nvmf_rdma_sta_state_changed_cb);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("Unable to set state changed callback for DOCA STA: %s\n", doca_error_get_descr(rc));
		return -EINVAL;
	}

	rc = doca_sta_subsystem_task_rm_ns_set_conf(rtransport->sta.sta,
			sta_offload_task_detach_ns_complete,
			sta_offload_task_detach_ns_complete_err);
	if (rc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to subsystem_task_rm_ns_set_conf, err: %s", doca_error_get_name(rc));
		return -EINVAL;
	}

	rc = doca_sta_be_task_destroy_queue_set_conf(rtransport->sta.sta,
			sta_offload_task_destroy_bqueue_complete,
			sta_offload_task_destroy_bqueue_complete_err);
	if (rc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to doca_sta_be_task_destroy_queue_set_conf, err: %s", doca_error_get_name(rc));
		return -EINVAL;
	}

	rc = doca_sta_mem_allocator_register(rtransport->sta.sta, nvmf_sta_zmalloc, nvmf_sta_free,
					     nvmf_sta_vtophys);
	if (rc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to register DOCA STA memory allocator, err: %s", doca_error_get_name(rc));
		return -EINVAL;
	}

	rc = doca_sta_set_max_sta_io(rtransport->sta.sta, spdk_env_get_core_count());
	if (rc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to set max_sta_io: %s", doca_error_get_name(rc));
		return -EINVAL;
	}

	rc = doca_sta_event_be_timeout_register_cb(rtransport->sta.sta, nvmf_sta_event_be_timeout_cb,
			udata);
	if (rc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to register event_back_end_timeout_cb: %s", doca_error_get_name(rc));
		return -EINVAL;
	}

	rc = doca_sta_event_eu_err_register_cb(rtransport->sta.sta, nvmf_sta_event_eu_err_cb, udata);
	if (rc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to register event_eu_err_cb: %s", doca_error_get_name(rc));
		return -EINVAL;
	}

	rc = doca_ctx_start(rtransport->sta.ctx);
	if (DOCA_IS_ERROR(rc)) {
		if (rc != DOCA_ERROR_IN_PROGRESS) {
			SPDK_ERRLOG("Unable to start DOCA STA: %s\n", doca_error_get_descr(rc));
			return -EINVAL;
		}

		assert(rtransport->sta.state == DOCA_CTX_STATE_STARTING);
		while (rtransport->sta.state == DOCA_CTX_STATE_STARTING) {
			doca_pe_progress(rtransport->sta.pe);
		}
		if (rtransport->sta.state != DOCA_CTX_STATE_RUNNING) {
			SPDK_NOTICELOG("Wrong DOCA STA state %s\n", nvmf_rdma_sta_state_to_str(rtransport->sta.state));
			return -EINVAL;
		}
	}

	rc = nvmf_rdma_sta_get_num_cq_handlers(&rtransport->sta);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("Unable to get number of CQ EUs: %s\n", doca_error_get_descr(rc));
		return -EINVAL;
	}
	SPDK_DEBUGLOG(rdma_offload, "Number of DOCA STA CQ handlers: %u\n",
		      rtransport->sta.num_cq_handlers);

	rtransport->sta.poller = SPDK_POLLER_REGISTER(nvmf_rdma_sta_progress, &rtransport->transport,
				 NVMF_RDMA_STA_DEFAULT_POLL_RATE_US);
	if (!rtransport->sta.poller) {
		SPDK_ERRLOG("Failed to register nvmf_rdma_sta_progress poller\n");
		return -EINVAL;
	}

	return 0;
}

static int
nvmf_doca_log_level_decode(const char *name, enum doca_log_level *level)
{
	struct log_level {
		const char *name;
		const enum doca_log_level value;
	};
	static const struct log_level log_levels[] = {
		{ .name = "disable",	.value = DOCA_LOG_LEVEL_DISABLE },
		{ .name = "critical",	.value = DOCA_LOG_LEVEL_CRIT },
		{ .name = "error",	.value = DOCA_LOG_LEVEL_ERROR },
		{ .name = "warning",	.value = DOCA_LOG_LEVEL_WARNING },
		{ .name = "info",	.value = DOCA_LOG_LEVEL_INFO },
		{ .name = "debug",	.value = DOCA_LOG_LEVEL_DEBUG },
		{ .name = "trace",	.value = DOCA_LOG_LEVEL_TRACE },
		{ .name = NULL,		.value = 0 },
	};
	const struct log_level *log_level;

	for (log_level = log_levels; log_level->name != NULL; log_level++) {
		if (strcmp(name, log_level->name) == 0) {
			*level = log_level->value;
			return 0;
		}
	}

	SPDK_ERRLOG("Unknown log level %s\n", name);
	return -1;
}

static int
nvmf_enable_doca_log(const char *log_level_name)
{
	struct doca_log_backend *sta_lib_log;
	enum doca_log_level log_level;
	int rc;
	doca_error_t drc;

	if (!log_level_name) {
		log_level = DOCA_LOG_LEVEL_ERROR;
	} else {
		rc = nvmf_doca_log_level_decode(log_level_name, &log_level);
		if (rc) {
			return -1;
		}
	}
	/* Register a logger backend for internal SDK errors and warnings */
	drc = doca_log_backend_create_with_file_sdk(stderr, &sta_lib_log);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to create DOCA log backend for SDK: %s\n", doca_error_get_descr(drc));
		return -1;
	}
	drc = doca_log_backend_set_sdk_level(sta_lib_log, log_level);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to set log level for DOCA log backend: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	return 0;
}

static int
nvmf_parse_rdma_device_list(const char *rdma_devices_str, char ***rdma_devices,
			    int *num_rdma_devices)
{
	char *str, *tmp, *sp, **devices;
	int i;

	str = strdup(rdma_devices_str);
	if (!str) {
		return -ENOMEM;
	}

	i = 1;
	tmp = str;
	while ((tmp = strchr(tmp, ',')) != NULL) {
		tmp++;
		i++;
	}

	devices = calloc(i, sizeof(char *));
	if (!devices) {
		free(str);
		return -ENOMEM;
	}

	i = 0;
	tmp = strtok_r(str, ",", &sp);
	while (tmp) {
		devices[i] = strdup(tmp);
		if (!devices[i]) {
			free(devices);
			free(str);
			return -ENOMEM;
		}
		i++;
		tmp = strtok_r(NULL, ",", &sp);
	}

	free(str);
	*rdma_devices = devices;
	*num_rdma_devices = i;

	return 0;
}

static struct spdk_nvmf_transport *
nvmf_rdma_create(struct spdk_nvmf_transport_opts *opts)
{
	int rc;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct ibv_context		**contexts;
	size_t				data_wr_pool_size;
	struct ibv_context		*context;
	int				i;
	int				j;
	int				flag;
	uint32_t			sge_count;
	uint32_t			min_shared_buffers;
	uint32_t			min_in_capsule_data_size;
	int				max_device_sge = SPDK_NVMF_MAX_SGL_ENTRIES;

	rtransport = calloc(1, sizeof(*rtransport));
	if (!rtransport) {
		return NULL;
	}

	TAILQ_INIT(&rtransport->devices);
	TAILQ_INIT(&rtransport->ports);
	TAILQ_INIT(&rtransport->poll_groups);
	TAILQ_INIT(&rtransport->retry_ports);
	TAILQ_INIT(&rtransport->subsystems);
	TAILQ_INIT(&rtransport->bdevs);

	rtransport->transport.ops = &spdk_nvmf_transport_rdma_offload;
	rtransport->rdma_opts.num_cqe = DEFAULT_NVMF_RDMA_CQ_SIZE;
	rtransport->rdma_opts.max_srq_depth = SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH;
	rtransport->rdma_opts.no_srq = SPDK_NVMF_RDMA_DEFAULT_NO_SRQ;
	rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	rtransport->rdma_opts.no_wr_batching = SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, rdma_transport_opts_decoder,
					    SPDK_COUNTOF(rdma_transport_opts_decoder),
					    &rtransport->rdma_opts)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rc = nvmf_enable_doca_log(rtransport->rdma_opts.doca_log_level);
	if (rc) {
		SPDK_ERRLOG("Failed to enable DOCA log\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	if (rtransport->rdma_opts.rdma_devices_str) {
		rc = nvmf_parse_rdma_device_list(rtransport->rdma_opts.rdma_devices_str,
						 &rtransport->rdma_opts.rdma_devices,
						 &rtransport->rdma_opts.num_rdma_devices);
		if (rc) {
			SPDK_ERRLOG("Failed to parse rdma_device_list %s\n", rtransport->rdma_opts.rdma_devices_str);
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
	}

	if (!rtransport->rdma_opts.doca_device) {
		rtransport->rdma_opts.doca_device = strdup(SPDK_NVMF_RDMA_DEFAULT_DOCA_DEVICE);
		if (!rtransport->rdma_opts.doca_device) {
			SPDK_ERRLOG("Failed to allocate memory for doca_device\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
	}

	if (!rtransport->rdma_opts.rdma_devices) {
		rtransport->rdma_opts.rdma_devices = malloc(sizeof(char *));
		if (!rtransport->rdma_opts.rdma_devices) {
			SPDK_ERRLOG("Failed to allocate memory for rdma_devices\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
		rtransport->rdma_opts.rdma_devices[0] = strdup(SPDK_NVMF_RDMA_DEFAULT_RDMA_DEVICE);
		if (!rtransport->rdma_opts.rdma_devices[0]) {
			SPDK_ERRLOG("Failed to allocate memory for rdma_devices\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
		rtransport->rdma_opts.num_rdma_devices = 1;
	}

	rc = nvmf_rdma_sta_create(rtransport);
	if (DOCA_IS_ERROR(rc)) {
		SPDK_ERRLOG("Unable to create DOCA STA\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}
	opts->max_queue_depth = spdk_min(opts->max_queue_depth, rtransport->sta.caps.max_io_queue_size);
	data_wr_pool_size = opts->max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES;
	opts->max_io_size = spdk_min(opts->max_io_size, rtransport->sta.caps.max_io_size);

	SPDK_INFOLOG(rdma_offload, "*** RDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, num_cqe=%d, max_srq_depth=%d, no_srq=%d,"
		     "  acceptor_backlog=%d, no_wr_batching=%d abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     rtransport->rdma_opts.num_cqe,
		     rtransport->rdma_opts.max_srq_depth,
		     rtransport->rdma_opts.no_srq,
		     rtransport->rdma_opts.acceptor_backlog,
		     rtransport->rdma_opts.no_wr_batching,
		     opts->abort_timeout_sec);

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	if (rtransport->rdma_opts.acceptor_backlog <= 0) {
		SPDK_ERRLOG("The acceptor backlog cannot be less than 1, setting to the default value of (%d).\n",
			    SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG);
		rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	}
	if (rtransport->transport.opts.msdbd > SPDK_NVMF_RDMA_MSDBD) {
		SPDK_WARNLOG("Configured MSDBD %u exceeds max supported value, result is limited by %u\n",
			     rtransport->transport.opts.msdbd, SPDK_NVMF_RDMA_MSDBD);
		rtransport->transport.opts.msdbd = SPDK_NVMF_RDMA_MSDBD;
	} else if (!rtransport->transport.opts.msdbd) {
		rtransport->transport.opts.msdbd = SPDK_NVMF_RDMA_MSDBD;
	}

	if (opts->num_shared_buffers < (SPDK_NVMF_MAX_SGL_ENTRIES * 2)) {
		SPDK_ERRLOG("The number of shared data buffers (%d) is less than"
			    "the minimum number required to guarantee that forward progress can be made (%d)\n",
			    opts->num_shared_buffers, (SPDK_NVMF_MAX_SGL_ENTRIES * 2));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	/* If buf_cache_size == UINT32_MAX, we will dynamically pick a cache size later that we know will fit. */
	if (opts->buf_cache_size < UINT32_MAX) {
		min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
		if (min_shared_buffers > opts->num_shared_buffers) {
			SPDK_ERRLOG("There are not enough buffers to satisfy"
				    "per-poll group caches for each thread. (%" PRIu32 ")"
				    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
			SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > NVMF_DEFAULT_TX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	min_in_capsule_data_size = sizeof(struct spdk_nvme_sgl_descriptor) * SPDK_NVMF_MAX_SGL_ENTRIES;
	if (opts->in_capsule_data_size < min_in_capsule_data_size) {
		SPDK_WARNLOG("In capsule data size is set to %u, this is minimum size required to support msdbd=16\n",
			     min_in_capsule_data_size);
		opts->in_capsule_data_size = min_in_capsule_data_size;
	}

	rtransport->event_channel = rdma_create_event_channel();
	if (rtransport->event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", spdk_strerror(errno));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	flag = fcntl(rtransport->event_channel->fd, F_GETFL);
	if (fcntl(rtransport->event_channel->fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n",
			    rtransport->event_channel->fd, spdk_strerror(errno));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	data_wr_pool_size = opts->data_wr_pool_size;
	if (data_wr_pool_size < SPDK_NVMF_MAX_SGL_ENTRIES * 2 * spdk_env_get_core_count()) {
		data_wr_pool_size = SPDK_NVMF_MAX_SGL_ENTRIES * 2 * spdk_env_get_core_count();
		SPDK_NOTICELOG("data_wr_pool_size is changed to %zu to guarantee enough cache for handling "
			       "at least one IO in each core\n", data_wr_pool_size);
	}
	rtransport->data_wr_pool = spdk_mempool_create("spdk_nvmf_rdma_wr_data", data_wr_pool_size,
				   sizeof(struct spdk_nvmf_rdma_request_data), SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_NUMA_ID_ANY);
	if (!rtransport->data_wr_pool) {
		if (spdk_mempool_lookup("spdk_nvmf_rdma_wr_data") != NULL) {
			SPDK_ERRLOG("Unable to allocate work request pool for poll group: already exists\n");
			SPDK_ERRLOG("Probably running in multiprocess environment, which is "
				    "unsupported by the nvmf library\n");
		} else {
			SPDK_ERRLOG("Unable to allocate work request pool for poll group\n");
		}
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rc = 0;
	for (i = 0; i < rtransport->rdma_opts.num_rdma_devices; i++) {
		context = NULL;
		for (j = 0; contexts[j] != NULL; j++) {
			if (strcmp(rtransport->rdma_opts.rdma_devices[i],
				   ibv_get_device_name(contexts[j]->device)) == 0) {
				context = contexts[j];
				break;
			}
		}
		if (!context) {
			SPDK_ERRLOG("RDMA device %s is not found.\n", rtransport->rdma_opts.rdma_devices[i]);
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
		rc = create_ib_device(rtransport, context, &device);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to open RDMA device %s.\n", rtransport->rdma_opts.rdma_devices[i]);
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
		max_device_sge = spdk_min(max_device_sge, device->attr.max_sge);
		device->is_ready = true;
	}
	rdma_free_devices(contexts);

	rc = nvmf_rdma_sta_start(rtransport);
	if (rc) {
		SPDK_ERRLOG("Unable to start DOCA STA\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}
	if (opts->io_unit_size * max_device_sge < opts->max_io_size) {
		/* divide and round up. */
		opts->io_unit_size = (opts->max_io_size + max_device_sge - 1) / max_device_sge;

		/* round up to the nearest 4k. */
		opts->io_unit_size = (opts->io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT - 1) & ~NVMF_DATA_BUFFER_MASK;

		opts->io_unit_size = spdk_max(opts->io_unit_size, SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE);
		SPDK_NOTICELOG("Adjusting the io unit size to fit the device's maximum I/O size. New I/O unit size %u\n",
			       opts->io_unit_size);
	}

	rc = generate_poll_fds(rtransport);
	if (rc < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rtransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_rdma_accept, &rtransport->transport,
				    opts->acceptor_poll_rate);
	if (!rtransport->accept_poller) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	return &rtransport->transport;
}

static void
destroy_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
		  struct spdk_nvmf_rdma_device *device)
{
	TAILQ_REMOVE(&rtransport->devices, device, link);
	spdk_rdma_utils_free_mem_map(&device->map);
	if (device->pd) {
		spdk_rdma_utils_put_pd(device->pd);
	}
	if (device->doca_dev) {
		doca_dev_close(device->doca_dev);
	}
	SPDK_DEBUGLOG(rdma_offload, "IB device [%p] is destroyed.\n", device);
	free(device);
}

static void
nvmf_rdma_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	assert(w != NULL);

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	spdk_json_write_named_uint32(w, "max_srq_depth", rtransport->rdma_opts.max_srq_depth);
	spdk_json_write_named_bool(w, "no_srq", rtransport->rdma_opts.no_srq);
	if (rtransport->rdma_opts.no_srq == true) {
		spdk_json_write_named_int32(w, "num_cqe", rtransport->rdma_opts.num_cqe);
	}
	spdk_json_write_named_int32(w, "acceptor_backlog", rtransport->rdma_opts.acceptor_backlog);
	spdk_json_write_named_bool(w, "no_wr_batching", rtransport->rdma_opts.no_wr_batching);
	if (transport->opts.msdbd != SPDK_NVMF_RDMA_MSDBD) {
		spdk_json_write_named_bool(w, "msdbd", transport->opts.msdbd);
	}
	spdk_json_write_named_string(w, "doca_device", rtransport->rdma_opts.doca_device);
}

static int nvmf_rdma_subsystem_destroy(struct spdk_nvmf_rdma_subsystem *rsubsystem);

static void
nvmf_rdma_sta_destroy(struct spdk_nvmf_rdma_transport *rtransport)
{
	doca_error_t rc;

	if (rtransport->sta.poller) {
		spdk_poller_unregister(&rtransport->sta.poller);
	}
	if (rtransport->sta.sta) {
		if (rtransport->sta.state == DOCA_CTX_STATE_RUNNING) {
			rc = doca_ctx_stop(rtransport->sta.ctx);
			if (DOCA_IS_ERROR(rc)) {
				if (rc != DOCA_ERROR_IN_PROGRESS) {
					SPDK_ERRLOG("Unable to stop DOCA STA: %s\n", doca_error_get_descr(rc));
				}
				assert(rtransport->sta.state == DOCA_CTX_STATE_STOPPING);
				while (rtransport->sta.state == DOCA_CTX_STATE_STOPPING) {
					doca_pe_progress(rtransport->sta.pe);
				}
				assert(rtransport->sta.state == DOCA_CTX_STATE_IDLE);
			}
		}
		rc = doca_sta_destroy(rtransport->sta.sta);
		if (DOCA_IS_ERROR(rc)) {
			SPDK_ERRLOG("doca_sta_destroy: %s\n", doca_error_get_descr(rc));
		}
	}

	if (rtransport->sta.dev) {
		rc = doca_dev_close(rtransport->sta.dev);
		if (DOCA_IS_ERROR(rc)) {
			SPDK_ERRLOG("doca_pe_destroy: %s\n", doca_error_get_descr(rc));
		}
	}

	if (rtransport->sta.pe) {
		rc = doca_pe_destroy(rtransport->sta.pe);
		if (DOCA_IS_ERROR(rc)) {
			SPDK_ERRLOG("doca_pe_destroy: %s\n", doca_error_get_descr(rc));
		}
	}
}

static int
nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
		  spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;
	struct spdk_nvmf_rdma_bdev	*rbdev, *rbdev_tmp;
	struct spdk_nvmf_rdma_subsystem *rsubsystem, *rsubsystem_tmp;
	int				i;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->retry_ports, port, link);
		free(port);
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->ports, port, link);
		rdma_destroy_id(port->id);
		free(port);
	}

	free_poll_fds(rtransport);

	if (rtransport->event_channel != NULL) {
		rdma_destroy_event_channel(rtransport->event_channel);
	}

	TAILQ_FOREACH_SAFE(rbdev, &rtransport->bdevs, link, rbdev_tmp) {
		TAILQ_REMOVE(&rtransport->bdevs, rbdev, link);
		nvmf_rdma_bdev_destroy(rbdev);
	}

	TAILQ_FOREACH_SAFE(rsubsystem, &rtransport->subsystems, link, rsubsystem_tmp) {
		TAILQ_REMOVE(&rtransport->subsystems, rsubsystem, link);
		nvmf_rdma_subsystem_destroy(rsubsystem);
	}

	nvmf_rdma_sta_destroy(rtransport);

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		destroy_ib_device(rtransport, device);
	}

	if (rtransport->data_wr_pool != NULL) {
		if (spdk_mempool_count(rtransport->data_wr_pool) != transport->opts.data_wr_pool_size) {
			SPDK_ERRLOG("transport wr pool count is %zu but should be %u\n",
				    spdk_mempool_count(rtransport->data_wr_pool),
				    transport->opts.max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES);
		}
	}

	spdk_mempool_free(rtransport->data_wr_pool);
	spdk_poller_unregister(&rtransport->accept_poller);

	if (rtransport->rdma_opts.doca_log_level) {
		free(rtransport->rdma_opts.doca_log_level);
	}
	if (rtransport->rdma_opts.doca_device) {
		free(rtransport->rdma_opts.doca_device);
	}
	if (rtransport->rdma_opts.rdma_devices) {
		for (i = 0; i < rtransport->rdma_opts.num_rdma_devices; i++) {
			free(rtransport->rdma_opts.rdma_devices[i]);
		}
		free(rtransport->rdma_opts.rdma_devices);
	}
	if (rtransport->rdma_opts.rdma_devices_str) {
		free(rtransport->rdma_opts.rdma_devices_str);
	}
	free(rtransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
	return 0;
}

static int nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
				     struct spdk_nvme_transport_id *trid,
				     bool peer);

static bool nvmf_rdma_rescan_devices(struct spdk_nvmf_rdma_transport *rtransport);

static int
nvmf_rdma_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port	*port, *tmp_port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;
	long int			port_val;
	bool				is_retry = false;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -EINVAL;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	assert(rtransport->event_channel != NULL);

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		return -ENOMEM;
	}

	port->trid = trid;

	switch (trid->adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", trid->adrfam);
		free(port);
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	/* Range check the trsvcid. Fail in 3 cases:
	 * < 0: means that spdk_strtol hit an error
	 * 0: this results in ephemeral port which we don't want
	 * > 65535: port too high
	 */
	port_val = spdk_strtol(trid->trsvcid, 10);
	if (port_val <= 0 || port_val > 65535) {
		SPDK_ERRLOG("invalid trsvcid %s\n", trid->trsvcid);
		free(port);
		return -EINVAL;
	}

	rc = getaddrinfo(trid->traddr, trid->trsvcid, &hints, &res);
	if (rc) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(rc), rc);
		free(port);
		return -(abs(rc));
	}

	rc = rdma_create_id(rtransport->event_channel, &port->id, port, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		freeaddrinfo(res);
		free(port);
		return rc;
	}

	rc = rdma_bind_addr(port->id, res->ai_addr);
	freeaddrinfo(res);

	if (rc < 0) {
		TAILQ_FOREACH(tmp_port, &rtransport->retry_ports, link) {
			if (spdk_nvme_transport_id_compare(tmp_port->trid, trid) == 0) {
				is_retry = true;
				break;
			}
		}
		if (!is_retry) {
			SPDK_ERRLOG("rdma_bind_addr() failed\n");
		}
		rdma_destroy_id(port->id);
		free(port);
		return rc;
	}

	if (!port->id->verbs) {
		SPDK_ERRLOG("ibv_context is null\n");
		rdma_destroy_id(port->id);
		free(port);
		return -1;
	}

	rc = rdma_listen(port->id, rtransport->rdma_opts.acceptor_backlog);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_listen() failed\n");
		rdma_destroy_id(port->id);
		free(port);
		return rc;
	}

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->context == port->id->verbs && device->is_ready) {
			port->device = device;
			break;
		}
	}
	if (!port->device) {
		SPDK_ERRLOG("Accepted a connection with verbs %p, but unable to find a corresponding device.\n",
			    port->id->verbs);
		rdma_destroy_id(port->id);
		free(port);
		nvmf_rdma_rescan_devices(rtransport);
		return -EINVAL;
	}

	SPDK_NOTICELOG("*** NVMe/RDMA Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	return 0;
}

static void
nvmf_rdma_stop_listen_ex(struct spdk_nvmf_transport *transport,
			 const struct spdk_nvme_transport_id *trid, bool need_retry)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (!need_retry) {
		TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, tmp) {
			if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
				TAILQ_REMOVE(&rtransport->retry_ports, port, link);
				free(port);
			}
		}
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
			SPDK_DEBUGLOG(rdma_offload, "Port %s:%s removed. need retry: %d\n",
				      port->trid->traddr, port->trid->trsvcid, need_retry);
			TAILQ_REMOVE(&rtransport->ports, port, link);
			rdma_destroy_id(port->id);
			port->id = NULL;
			port->device = NULL;
			if (need_retry) {
				TAILQ_INSERT_TAIL(&rtransport->retry_ports, port, link);
			} else {
				free(port);
			}
			break;
		}
	}
}

static void
nvmf_rdma_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	nvmf_rdma_stop_listen_ex(transport, trid, false);
}

static void _nvmf_rdma_register_poller_in_group(void *c);
static void _nvmf_rdma_remove_poller_in_group(void *c);

static bool
nvmf_rdma_all_pollers_management_done(void *c)
{
	struct poller_manage_ctx	*ctx = c;
	int				counter;

	counter = __atomic_sub_fetch(ctx->inflight_op_counter, 1, __ATOMIC_SEQ_CST);
	SPDK_DEBUGLOG(rdma_offload,
		      "nvmf_rdma_all_pollers_management_done called. counter: %d, poller: %p\n",
		      counter, ctx->rpoller);

	if (counter == 0) {
		free((void *)ctx->inflight_op_counter);
	}
	free(ctx);

	return counter == 0;
}

static int
nvmf_rdma_manage_poller(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_device *device, bool *has_inflight, bool is_add)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_poll_group		*poll_group;
	struct poller_manage_ctx		*ctx;
	bool					found;
	int					*inflight_counter;
	spdk_msg_fn				do_fn;

	*has_inflight = false;
	do_fn = is_add ? _nvmf_rdma_register_poller_in_group : _nvmf_rdma_remove_poller_in_group;
	inflight_counter = calloc(1, sizeof(int));
	if (!inflight_counter) {
		SPDK_ERRLOG("Failed to allocate inflight counter when removing pollers\n");
		return -ENOMEM;
	}

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		(*inflight_counter)++;
	}

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		found = false;
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			if (rpoller->device == device) {
				found = true;
				break;
			}
		}
		if (found == is_add) {
			__atomic_fetch_sub(inflight_counter, 1, __ATOMIC_SEQ_CST);
			continue;
		}

		ctx = calloc(1, sizeof(struct poller_manage_ctx));
		if (!ctx) {
			SPDK_ERRLOG("Failed to allocate poller_manage_ctx when removing pollers\n");
			if (!*has_inflight) {
				free(inflight_counter);
			}
			return -ENOMEM;
		}

		ctx->rtransport = rtransport;
		ctx->rgroup = rgroup;
		ctx->rpoller = rpoller;
		ctx->device = device;
		ctx->thread = spdk_get_thread();
		ctx->inflight_op_counter = inflight_counter;
		*has_inflight = true;

		poll_group = rgroup->group.group;
		if (poll_group->thread != spdk_get_thread()) {
			spdk_thread_send_msg(poll_group->thread, do_fn, ctx);
		} else {
			do_fn(ctx);
		}
	}

	if (!*has_inflight) {
		free(inflight_counter);
	}

	return 0;
}

static void nvmf_rdma_handle_device_removal(struct spdk_nvmf_rdma_transport *rtransport,
		struct spdk_nvmf_rdma_device *device);

static struct spdk_nvmf_rdma_device *
nvmf_rdma_find_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
			 struct ibv_context *context)
{
	struct spdk_nvmf_rdma_device	*device, *tmp_device;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp_device) {
		if (device->need_destroy) {
			continue;
		}

		if (strcmp(device->context->device->dev_name, context->device->dev_name) == 0) {
			return device;
		}
	}

	return NULL;
}

static bool
nvmf_rdma_check_devices_context(struct spdk_nvmf_rdma_transport *rtransport,
				struct ibv_context *context)
{
	struct spdk_nvmf_rdma_device	*old_device, *new_device;
	int				rc = 0;
	bool				has_inflight;

	old_device = nvmf_rdma_find_ib_device(rtransport, context);

	if (old_device) {
		if (old_device->context != context && !old_device->need_destroy && old_device->is_ready) {
			/* context may not have time to be cleaned when rescan. exactly one context
			 * is valid for a device so this context must be invalid and just remove it. */
			SPDK_WARNLOG("Device %p has a invalid context %p\n", old_device, old_device->context);
			old_device->need_destroy = true;
			nvmf_rdma_handle_device_removal(rtransport, old_device);
		}
		return false;
	}

	rc = create_ib_device(rtransport, context, &new_device);
	/* TODO: update transport opts. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create ib device for context: %s(%p)\n",
			    ibv_get_device_name(context->device), context);
		return false;
	}

	rc = nvmf_rdma_manage_poller(rtransport, new_device, &has_inflight, true);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add poller for device context: %s(%p)\n",
			    ibv_get_device_name(context->device), context);
		return false;
	}

	if (has_inflight) {
		new_device->is_ready = true;
	}

	return true;
}

static bool
nvmf_rdma_rescan_devices(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_device	*device;
	struct ibv_device		**ibv_device_list = NULL;
	struct ibv_context		**contexts = NULL;
	int				i = 0;
	int				num_dev = 0;
	bool				new_create = false, has_new_device = false;
	struct ibv_context		*tmp_verbs = NULL;
	int				j;

	/* do not rescan when any device is destroying, or context may be freed when
	 * regenerating the poll fds.
	 */
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->need_destroy) {
			return false;
		}
	}

	ibv_device_list = ibv_get_device_list(&num_dev);

	/* There is a bug in librdmacm. If verbs init failed in rdma_get_devices, it'll be
	 * marked as dead verbs and never be init again. So we need to make sure the
	 * verbs is available before we call rdma_get_devices. */
	if (num_dev >= 0) {
		for (i = 0; i < num_dev; i++) {
			tmp_verbs = ibv_open_device(ibv_device_list[i]);
			if (!tmp_verbs) {
				SPDK_WARNLOG("Failed to init ibv device %p, err %d. Skip rescan.\n", ibv_device_list[i], errno);
				break;
			}
			if (nvmf_rdma_find_ib_device(rtransport, tmp_verbs) == NULL) {
				SPDK_DEBUGLOG(rdma_offload, "Find new verbs init ibv device %p(%s).\n", ibv_device_list[i],
					      tmp_verbs->device->dev_name);
				has_new_device = true;
			}
			ibv_close_device(tmp_verbs);
		}
		ibv_free_device_list(ibv_device_list);
		if (!tmp_verbs || !has_new_device) {
			return false;
		}
	}

	contexts = rdma_get_devices(NULL);

	for (i = 0; contexts && contexts[i] != NULL; i++) {
		for (j = 0; j < rtransport->rdma_opts.num_rdma_devices; j++) {
			if (strcmp(ibv_get_device_name(contexts[i]->device),
				   rtransport->rdma_opts.rdma_devices[j]) == 0) {
				break;
			}
		}
		if (j == rtransport->rdma_opts.num_rdma_devices) {
			SPDK_DEBUGLOG(rdma_offload, "Skip ibv device %s because it not in the allowed list\n",
				      ibv_get_device_name(contexts[i]->device));
			continue;
		}
		new_create |= nvmf_rdma_check_devices_context(rtransport, contexts[i]);
	}

	if (new_create) {
		free_poll_fds(rtransport);
		generate_poll_fds(rtransport);
	}

	if (contexts) {
		rdma_free_devices(contexts);
	}

	return new_create;
}

static bool
nvmf_rdma_retry_listen_port(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_port	*port, *tmp_port;
	int				rc = 0;
	bool				new_create = false;

	if (TAILQ_EMPTY(&rtransport->retry_ports)) {
		return false;
	}

	new_create = nvmf_rdma_rescan_devices(rtransport);

	TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, tmp_port) {
		rc = nvmf_rdma_listen(&rtransport->transport, port->trid, NULL);

		TAILQ_REMOVE(&rtransport->retry_ports, port, link);
		if (rc) {
			if (new_create) {
				SPDK_ERRLOG("Found new IB device but port %s:%s is still failed(%d) to listen.\n",
					    port->trid->traddr, port->trid->trsvcid, rc);
			}
			TAILQ_INSERT_TAIL(&rtransport->retry_ports, port, link);
			break;
		} else {
			SPDK_NOTICELOG("Port %s:%s come back\n", port->trid->traddr, port->trid->trsvcid);
			free(port);
		}
	}

	return true;
}

static void
nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_qpair *rqpair, bool drain)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;
	struct spdk_nvmf_rdma_resources *resources;

	/* First process requests which are waiting for response to be sent */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_send_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* We process I/O in the data transfer pending queue at the highest priority. */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_read_queue, state_link, req_tmp) {
		if (rdma_req->state != RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING) {
			/* Requests in this queue might be in state RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
			 * they are transmitting data over network but we keep them in the list to guarantee
			 * fair processing. */
			continue;
		}
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then RDMA writes since reads have stronger restrictions than writes */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_write_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then we handle request waiting on memory buffers. */
	STAILQ_FOREACH_SAFE(req, &rqpair->poller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = nvmf_rdma_request_get(req);
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	resources = rqpair->resources;
	while (!STAILQ_EMPTY(&resources->free_queue) && !STAILQ_EMPTY(&resources->incoming_queue)) {
		rdma_req = STAILQ_FIRST(&resources->free_queue);
		STAILQ_REMOVE_HEAD(&resources->free_queue, state_link);
		rdma_req->recv = STAILQ_FIRST(&resources->incoming_queue);
		STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);

		if (rqpair->srq != NULL) {
			rdma_req->common.req.qpair = &rdma_req->recv->qpair->common.qpair;
			rdma_req->recv->qpair->qd++;
		} else {
			rqpair->qd++;
		}

		rdma_req->receive_tsc = rdma_req->recv->receive_tsc;
		rdma_req->state = RDMA_REQUEST_STATE_NEW;
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
	if (!STAILQ_EMPTY(&resources->incoming_queue) && STAILQ_EMPTY(&resources->free_queue)) {
		rqpair->poller->stat.pending_free_request++;
	}
}

static void
nvmf_offload_qpair_process_pending(struct spdk_nvmf_offload_qpair *oqpair, bool drain)
{
	struct nvmf_non_offload_request	*non_offload_req, *non_offload_req_tmp;
	struct nvmf_sta_non_offload_resources *resources;

	/* First process requests which are waiting for response to be sent */
	STAILQ_FOREACH_SAFE(non_offload_req, &oqpair->pending_rdma_send_queue, state_link,
			    non_offload_req_tmp) {
		if (nvmf_sta_io_non_offload_request_process(non_offload_req) == false && drain == false) {
			break;
		}
	}

	/* We process I/O in the data transfer pending queue at the highest priority. */
	STAILQ_FOREACH_SAFE(non_offload_req, &oqpair->pending_rdma_read_queue, state_link,
			    non_offload_req_tmp) {
		if (nvmf_sta_io_non_offload_request_process(non_offload_req) == false && drain == false) {
			break;
		}
	}

	/* Then RDMA writes since reads have stronger restrictions than writes */
	STAILQ_FOREACH_SAFE(non_offload_req, &oqpair->pending_rdma_write_queue, state_link,
			    non_offload_req_tmp) {
		if (nvmf_sta_io_non_offload_request_process(non_offload_req) == false && drain == false) {
			break;
		}
	}

	resources = oqpair->opoller->resources;
	while (!STAILQ_EMPTY(&resources->incoming_queue)) {
		non_offload_req = STAILQ_FIRST(&resources->incoming_queue);
		STAILQ_REMOVE_HEAD(&resources->incoming_queue, state_link);

		if (nvmf_sta_io_non_offload_request_process(non_offload_req) == false) {
			break;
		}
	}
}

static void
nvmf_rdma_poller_process_pending_buf_queue(struct spdk_nvmf_rdma_transport *rtransport,
		struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request *rdma_req;

	STAILQ_FOREACH_SAFE(req, &rpoller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = nvmf_rdma_request_get(req);
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
}

static inline bool
nvmf_rdma_device_supports_last_wqe_reached(struct spdk_nvmf_rdma_device *device)
{
	/* iWARP transport and SoftRoCE driver don't support LAST_WQE_REACHED ibv async event */
	return !nvmf_rdma_is_rxe_device(device) &&
	       device->context->device->transport_type != IBV_TRANSPORT_IWARP;
}

static void
nvmf_rdma_destroy_drained_qpair(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->common.qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	if (rqpair->in_destroy) {
		return;
	}
	rqpair->in_destroy = true;

	nvmf_rdma_qpair_process_pending(rtransport, rqpair, true);

	/* nvmf_rdma_close_qpair is not called */
	if (!rqpair->to_close) {
		goto again;
	}

	/* device is already destroyed and we should force destroy this qpair. */
	if (rqpair->poller && rqpair->poller->need_destroy) {
		nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	/* In non SRQ path, we will reach rqpair->max_queue_depth. In SRQ path, we will get the last_wqe event. */
	if (rqpair->current_send_depth != 0) {
		goto again;
	}

	if (rqpair->srq == NULL && rqpair->current_recv_depth != rqpair->max_queue_depth) {
		goto again;
	}

	/* For devices that support LAST_WQE_REACHED with srq, we need to
	 * wait to destroy the qpair until that event has been received.
	 */
	if (rqpair->srq != NULL && rqpair->last_wqe_reached == false &&
	    nvmf_rdma_device_supports_last_wqe_reached(rqpair->device)) {
		goto again;
		return;
	}

	assert(rqpair->common.qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED ||
	       rqpair->common.qpair.state == SPDK_NVMF_QPAIR_ERROR);

	nvmf_rdma_qpair_destroy(rqpair);

	return;
again:
	rqpair->in_destroy = false;
}

static int
nvmf_rdma_disconnect(struct rdma_cm_event *evt, bool *event_acked)
{
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_common_qpair   *cqpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;

	if (evt->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		return -1;
	}

	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}

	rdma_ack_cm_event(evt);
	*event_acked = true;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);
	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		rqpair = nvmf_rdma_qpair_get(qpair);
		spdk_trace_record(TRACE_RDMA_OFFLOAD_QP_DISCONNECT, 0, 0, (uintptr_t)rqpair);
	}

	spdk_nvmf_qpair_disconnect(qpair);

	return 0;
}

#ifdef DEBUG
static const char *CM_EVENT_STR[] = {
	"RDMA_CM_EVENT_ADDR_RESOLVED",
	"RDMA_CM_EVENT_ADDR_ERROR",
	"RDMA_CM_EVENT_ROUTE_RESOLVED",
	"RDMA_CM_EVENT_ROUTE_ERROR",
	"RDMA_CM_EVENT_CONNECT_REQUEST",
	"RDMA_CM_EVENT_CONNECT_RESPONSE",
	"RDMA_CM_EVENT_CONNECT_ERROR",
	"RDMA_CM_EVENT_UNREACHABLE",
	"RDMA_CM_EVENT_REJECTED",
	"RDMA_CM_EVENT_ESTABLISHED",
	"RDMA_CM_EVENT_DISCONNECTED",
	"RDMA_CM_EVENT_DEVICE_REMOVAL",
	"RDMA_CM_EVENT_MULTICAST_JOIN",
	"RDMA_CM_EVENT_MULTICAST_ERROR",
	"RDMA_CM_EVENT_ADDR_CHANGE",
	"RDMA_CM_EVENT_TIMEWAIT_EXIT"
};
#endif /* DEBUG */

static void
nvmf_rdma_disconnect_qpairs_on_port(struct spdk_nvmf_rdma_transport *rtransport,
				    struct spdk_nvmf_rdma_port *port)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_qpair		*rqpair;

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
				if (rqpair->listen_id == port->id) {
					spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
				}
			}
		}
	}
}

static bool
nvmf_rdma_handle_cm_event_addr_change(struct spdk_nvmf_transport *transport,
				      struct rdma_cm_event *event)
{
	const struct spdk_nvme_transport_id	*trid;
	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_transport		*rtransport;
	bool					event_acked = false;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (port->id == event->id) {
			SPDK_ERRLOG("ADDR_CHANGE: IP %s:%s migrated\n", port->trid->traddr, port->trid->trsvcid);
			rdma_ack_cm_event(event);
			event_acked = true;
			trid = port->trid;
			break;
		}
	}

	if (event_acked) {
		nvmf_rdma_disconnect_qpairs_on_port(rtransport, port);

		nvmf_rdma_stop_listen(transport, trid);
		nvmf_rdma_listen(transport, trid, NULL);
	}

	return event_acked;
}

static void
nvmf_rdma_handle_device_removal(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	int				rc;
	bool				has_inflight;

	rc = nvmf_rdma_manage_poller(rtransport, device, &has_inflight, false);
	if (rc) {
		SPDK_ERRLOG("Failed to handle device removal, rc %d\n", rc);
		return;
	}

	if (!has_inflight) {
		/* no pollers, destroy the device */
		device->ready_to_destroy = true;
		spdk_thread_send_msg(spdk_get_thread(), _nvmf_rdma_remove_destroyed_device, rtransport);
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		if (port->device == device) {
			SPDK_NOTICELOG("Port %s:%s on device %s is being removed.\n",
				       port->trid->traddr,
				       port->trid->trsvcid,
				       ibv_get_device_name(port->device->context->device));

			/* keep NVMF listener and only destroy structures of the
			 * RDMA transport. when the device comes back we can retry listening
			 * and the application's workflow will not be interrupted.
			 */
			nvmf_rdma_stop_listen_ex(&rtransport->transport, port->trid, true);
		}
	}
}

static void
nvmf_rdma_handle_cm_event_port_removal(struct spdk_nvmf_transport *transport,
				       struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_port		*port, *tmp_port;
	struct spdk_nvmf_rdma_transport		*rtransport;

	port = event->id->context;
	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rdma_ack_cm_event(event);

	/* if device removal happens during ctrl qpair disconnecting, it's possible that we receive
	 * an DEVICE_REMOVAL event on qpair but the id->qp is just NULL. So we should make sure that
	 * we are handling a port event here.
	 */
	TAILQ_FOREACH(tmp_port, &rtransport->ports, link) {
		if (port == tmp_port && port->device && !port->device->need_destroy) {
			port->device->need_destroy = true;
			nvmf_rdma_handle_device_removal(rtransport, port->device);
		}
	}
}

static void
nvmf_process_cm_events(struct spdk_nvmf_transport *transport, uint32_t max_events)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct rdma_cm_event		*event;
	uint32_t			i;
	int				rc;
	bool				event_acked;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (rtransport->event_channel == NULL) {
		return;
	}

	for (i = 0; i < max_events; i++) {
		event_acked = false;
		rc = rdma_get_cm_event(rtransport->event_channel, &event);
		if (rc) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}

		SPDK_DEBUGLOG(rdma_offload, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

		spdk_trace_record(TRACE_RDMA_OFFLOAD_CM_ASYNC_EVENT, 0, 0, 0, event->event);

		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
		case RDMA_CM_EVENT_ROUTE_ERROR:
			/* No action required. The target never attempts to resolve routes. */
			break;
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			rc = nvmf_rdma_connect(transport, event);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process connect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			/* The target never initiates a new connection. So this will not occur. */
			break;
		case RDMA_CM_EVENT_CONNECT_ERROR:
			/* Can this happen? The docs say it can, but not sure what causes it. */
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			/* These only occur on the client side. */
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			/* TODO: Should we be waiting for this event anywhere? */
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			rc = nvmf_rdma_disconnect(event, &event_acked);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			/* In case of device removal, kernel IB part triggers IBV_EVENT_DEVICE_FATAL
			 * which triggers RDMA_CM_EVENT_DEVICE_REMOVAL on all cma_id’s.
			 * Once these events are sent to SPDK, we should release all IB resources and
			 * don't make attempts to call any ibv_query/modify/create functions. We can only call
			 * ibv_destroy* functions to release user space memory allocated by IB. All kernel
			 * resources are already cleaned. */
			if (event->id->qp) {
				/* If rdma_cm event has a valid `qp` pointer then the event refers to the
				 * corresponding qpair. Otherwise the event refers to a listening device. */
				rc = nvmf_rdma_disconnect(event, &event_acked);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
					break;
				}
			} else {
				nvmf_rdma_handle_cm_event_port_removal(transport, event);
				event_acked = true;
			}
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			/* Multicast is not used */
			break;
		case RDMA_CM_EVENT_ADDR_CHANGE:
			event_acked = nvmf_rdma_handle_cm_event_addr_change(transport, event);
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			/* For now, do nothing. The target never re-uses queue pairs. */
			break;
		default:
			SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
			break;
		}
		if (!event_acked) {
			rdma_ack_cm_event(event);
		}
	}
}

static void
nvmf_rdma_handle_last_wqe_reached(struct spdk_nvmf_rdma_qpair *rqpair)
{
	rqpair->last_wqe_reached = true;
	nvmf_rdma_destroy_drained_qpair(rqpair);
}

static void
nvmf_rdma_qpair_process_last_wqe_event(void *ctx)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *event_ctx = ctx;
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = event_ctx->rqpair;

	if (rqpair) {
		assert(event_ctx == rqpair->last_wqe_reached_ctx);
		rqpair->last_wqe_reached_ctx = NULL;
		nvmf_rdma_handle_last_wqe_reached(rqpair);
	}
	free(event_ctx);
}

static int
nvmf_rdma_send_qpair_last_wqe_event(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;
	struct spdk_thread *thr = NULL;
	int rc;

	if (rqpair->common.qpair.group) {
		thr = rqpair->common.qpair.group->thread;
	} else if (rqpair->destruct_channel) {
		thr = spdk_io_channel_get_thread(rqpair->destruct_channel);
	}

	if (!thr) {
		SPDK_DEBUGLOG(rdma_offload, "rqpair %p has no thread\n", rqpair);
		return -EINVAL;
	}

	if (rqpair->last_wqe_reached || rqpair->last_wqe_reached_ctx != NULL) {
		SPDK_ERRLOG("LAST_WQE_REACHED already received for rqpair %p\n", rqpair);
		return -EALREADY;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->rqpair = rqpair;
	rqpair->last_wqe_reached_ctx = ctx;

	rc = spdk_thread_send_msg(thr, nvmf_rdma_qpair_process_last_wqe_event, ctx);
	if (rc) {
		rqpair->last_wqe_reached_ctx = NULL;
		free(ctx);
	}

	return rc;
}

static int
nvmf_process_ib_event(struct spdk_nvmf_rdma_device *device)
{
	int				rc;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct ibv_async_event		event;

	rc = ibv_get_async_event(device->context, &event);

	if (rc) {
		/* In non-blocking mode -1 means there are no events available */
		return rc;
	}

	switch (event.event_type) {
	case IBV_EVENT_QP_FATAL:
	case IBV_EVENT_QP_LAST_WQE_REACHED:
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_PATH_MIG_ERR:
		rqpair = event.element.qp->qp_context;
		if (!rqpair) {
			/* Any QP event for NVMe-RDMA initiator may be returned. */
			SPDK_NOTICELOG("Async QP event for unknown QP: %s\n",
				       ibv_event_type_str(event.event_type));
			break;
		}

		switch (event.event_type) {
		case IBV_EVENT_QP_FATAL:
			SPDK_ERRLOG("Fatal event received for rqpair %p\n", rqpair);
			spdk_trace_record(TRACE_RDMA_OFFLOAD_IBV_ASYNC_EVENT, 0, 0,
					  (uintptr_t)rqpair, event.event_type);
			rqpair->ibv_in_error_state = true;
			spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
			break;
		case IBV_EVENT_QP_LAST_WQE_REACHED:
			/* This event only occurs for shared receive queues. */
			SPDK_DEBUGLOG(rdma_offload, "Last WQE reached event received for rqpair %p\n", rqpair);
			rc = nvmf_rdma_send_qpair_last_wqe_event(rqpair);
			if (rc) {
				SPDK_WARNLOG("Failed to send LAST_WQE_REACHED event. rqpair %p, err %d\n", rqpair, rc);
				rqpair->last_wqe_reached = true;
			}
			break;
		case IBV_EVENT_QP_REQ_ERR:
		case IBV_EVENT_QP_ACCESS_ERR:
		case IBV_EVENT_COMM_EST:
		case IBV_EVENT_PATH_MIG:
		case IBV_EVENT_PATH_MIG_ERR:
			SPDK_NOTICELOG("Async QP event: %s\n",
				       ibv_event_type_str(event.event_type));
			spdk_trace_record(TRACE_RDMA_OFFLOAD_IBV_ASYNC_EVENT, 0, 0,
					  (uintptr_t)rqpair, event.event_type);
			rqpair->ibv_in_error_state = true;
			break;
		default:
			break;
		}
		break;
	case IBV_EVENT_DEVICE_FATAL:
		SPDK_ERRLOG("Device Fatal event[%s] received on %s. device: %p\n",
			    ibv_event_type_str(event.event_type), ibv_get_device_name(device->context->device), device);
		device->need_destroy = true;
		break;
	case IBV_EVENT_CQ_ERR:
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_PORT_ERR:
	case IBV_EVENT_LID_CHANGE:
	case IBV_EVENT_PKEY_CHANGE:
	case IBV_EVENT_SM_CHANGE:
	case IBV_EVENT_SRQ_ERR:
	case IBV_EVENT_SRQ_LIMIT_REACHED:
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_GID_CHANGE:
	case IBV_EVENT_SQ_DRAINED:
	default:
		SPDK_NOTICELOG("Async event: %s\n",
			       ibv_event_type_str(event.event_type));
		spdk_trace_record(TRACE_RDMA_OFFLOAD_IBV_ASYNC_EVENT, 0, 0, 0, event.event_type);
		break;
	}
	ibv_ack_async_event(&event);

	return 0;
}

static void
nvmf_process_ib_events(struct spdk_nvmf_rdma_device *device, uint32_t max_events)
{
	int rc = 0;
	uint32_t i = 0;

	for (i = 0; i < max_events; i++) {
		rc = nvmf_process_ib_event(device);
		if (rc) {
			break;
		}
	}

	SPDK_DEBUGLOG(rdma_offload, "Device %s: %u events processed\n", device->context->device->name, i);
}

static int
nvmf_rdma_accept(void *ctx)
{
	int	nfds, i = 0;
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device *device, *tmp;
	uint32_t count;
	short revents;
	bool do_retry;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	do_retry = nvmf_rdma_retry_listen_port(rtransport);

	count = nfds = poll(rtransport->poll_fds, rtransport->npoll_fds, 0);

	if (nfds <= 0) {
		return do_retry ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	}

	/* The first poll descriptor is RDMA CM event */
	if (rtransport->poll_fds[i++].revents & POLLIN) {
		nvmf_process_cm_events(transport, NVMF_RDMA_MAX_EVENTS_PER_POLL);
		nfds--;
	}

	if (nfds == 0) {
		return SPDK_POLLER_BUSY;
	}

	/* Second and subsequent poll descriptors are IB async events */
	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		revents = rtransport->poll_fds[i++].revents;
		if (revents & POLLIN) {
			if (spdk_likely(!device->need_destroy)) {
				nvmf_process_ib_events(device, NVMF_RDMA_MAX_EVENTS_PER_POLL);
				if (spdk_unlikely(device->need_destroy)) {
					nvmf_rdma_handle_device_removal(rtransport, device);
				}
			}
			nfds--;
		} else if (revents & POLLNVAL || revents & POLLHUP) {
			SPDK_ERRLOG("Receive unknown revent %x on device %p\n", (int)revents, device);
			nfds--;
		}
	}
	/* check all flagged fd's have been served */
	assert(nfds == 0);

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_rdma_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		     struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->nvmf_specific.msdbd = transport->opts.msdbd;

	/* Disable in-capsule data transfer for RDMA controller when dif_insert_or_strip is enabled
	since in-capsule data only works with NVME drives that support SGL memory layout */
	if (transport->opts.dif_insert_or_strip) {
		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	}

	if (cdata->nvmf_specific.ioccsz > ((sizeof(struct spdk_nvme_cmd) + 0x1000) / 16)) {
		SPDK_WARNLOG("RDMA is configured to support up to 16 SGL entries while in capsule"
			     " data is greater than 4KiB.\n");
		SPDK_WARNLOG("When used in conjunction with the NVMe-oF initiator from the Linux "
			     "kernel between versions 5.4 and 5.12 data corruption may occur for "
			     "writes that are not a multiple of 4KiB in size.\n");
	}
}

static void
nvmf_rdma_discover(struct spdk_nvmf_transport *transport,
		   struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static int
nvmf_rdma_poller_interrupt(void *ctx)
{
	struct spdk_nvmf_rdma_poller *poller = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct ibv_cq *ev_cq;
	void *ev_ctx;

	if (ibv_get_cq_event(poller->comp_channel, &ev_cq, &ev_ctx)) {
		SPDK_ERRLOG("Failed to get cq_event\n");
		return 0;
	}

	SPDK_DEBUGLOG(rdma_offload, "Received CQ event\n");
	ibv_ack_cq_events(ev_cq, 1);
	if (spdk_rdma_provider_req_notify_cq(poller->cq, 0)) {
		SPDK_ERRLOG("Couldn't request CQ notification\n");
		return -1;
	}

	rtransport = SPDK_CONTAINEROF(poller->group->group.transport, struct spdk_nvmf_rdma_transport,
				      transport);
	return nvmf_rdma_poller_poll(rtransport, poller);
}

static int
nvmf_rdma_poller_create(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_poll_group *rgroup, struct spdk_nvmf_rdma_device *device,
			struct spdk_nvmf_rdma_poller **out_poller)
{
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_rdma_provider_srq_init_attr	srq_init_attr;
	struct spdk_nvmf_rdma_resource_opts	opts;
	int					num_cqe;
	struct spdk_rdma_provider_cq_init_attr	cq_init_attr;
	int					flags;
	int					rc;

	poller = calloc(1, sizeof(*poller));
	if (!poller) {
		SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
		return -1;
	}

	poller->device = device;
	poller->group = rgroup;
	*out_poller = poller;

	RB_INIT(&poller->qpairs);
	STAILQ_INIT(&poller->qpairs_pending_send);
	STAILQ_INIT(&poller->qpairs_pending_recv);

	TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
	SPDK_DEBUGLOG(rdma_offload, "Create poller %p on device %p in poll group %p.\n", poller, device,
		      rgroup);
	if (rtransport->rdma_opts.no_srq == false && device->num_srq < device->attr.max_srq) {
		if ((int)rtransport->rdma_opts.max_srq_depth > device->attr.max_srq_wr) {
			SPDK_WARNLOG("Requested SRQ depth %u, max supported by dev %s is %d\n",
				     rtransport->rdma_opts.max_srq_depth, device->context->device->name, device->attr.max_srq_wr);
		}
		poller->max_srq_depth = spdk_min((int)rtransport->rdma_opts.max_srq_depth, device->attr.max_srq_wr);

		device->num_srq++;
		memset(&srq_init_attr, 0, sizeof(srq_init_attr));
		srq_init_attr.pd = device->pd;
		srq_init_attr.stats = &poller->stat.qp_stats.recv;
		srq_init_attr.srq_init_attr.attr.max_wr = poller->max_srq_depth;
		srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
		poller->srq = spdk_rdma_provider_srq_create(&srq_init_attr);
		if (!poller->srq) {
			SPDK_ERRLOG("Unable to create shared receive queue, errno %d\n", errno);
			return -1;
		}

		opts.qp = poller->srq;
		opts.map = device->map;
		opts.qpair = NULL;
		opts.shared = true;
		opts.max_queue_depth = poller->max_srq_depth;
		opts.in_capsule_data_size = rtransport->transport.opts.in_capsule_data_size;

		poller->resources = nvmf_rdma_resources_create(&opts);
		if (!poller->resources) {
			SPDK_ERRLOG("Unable to allocate resources for shared receive queue.\n");
			return -1;
		}
	}

	if (spdk_interrupt_mode_is_enabled()) {
		poller->comp_channel = ibv_create_comp_channel(device->context);
		if (!poller->comp_channel) {
			SPDK_ERRLOG("Unable to create completion channel\n");
			return -1;
		}

		flags = fcntl(poller->comp_channel->fd, F_GETFL);
		rc = fcntl(poller->comp_channel->fd, F_SETFL, flags | O_NONBLOCK);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to change completion channel fd to NONBLOCK mode\n");
			return -1;
		}

		SPDK_NOTICELOG("Created comp channel %p, fd %d, max number of comp vectors %d\n",
			       poller->comp_channel, poller->comp_channel->fd, device->context->num_comp_vectors);
		poller->interrupt = SPDK_INTERRUPT_REGISTER(poller->comp_channel->fd,
				    nvmf_rdma_poller_interrupt, poller);
		if (!poller->interrupt) {
			SPDK_ERRLOG("Failed to regiter interrupt\n");
			return -1;
		}
	}

	/*
	 * When using an srq, we can limit the completion queue at startup.
	 * The following formula represents the calculation:
	 * num_cqe = num_recv + num_data_wr + num_send_wr.
	 * where num_recv=num_data_wr=and num_send_wr=poller->max_srq_depth
	 */
	if (poller->srq) {
		num_cqe = poller->max_srq_depth * 3;
	} else {
		num_cqe = rtransport->rdma_opts.num_cqe;
	}

	cq_init_attr.cqe		= num_cqe;
	cq_init_attr.comp_vector	= 0;
	cq_init_attr.cq_context		= poller;
	cq_init_attr.comp_channel	= poller->comp_channel;
	cq_init_attr.pd			= device->pd;

	poller->cq = spdk_rdma_provider_cq_create(&cq_init_attr);
	if (!poller->cq) {
		SPDK_ERRLOG("Unable to create completion queue\n");
		return -1;
	}
	poller->num_cqe = num_cqe;
	if (spdk_interrupt_mode_is_enabled()) {
		if (spdk_rdma_provider_req_notify_cq(poller->cq, 0)) {
			SPDK_ERRLOG("Couldn't request CQ notification\n");
			return -1;
		}
	}

	return 0;
}

static void
_nvmf_rdma_register_poller_in_group(void *c)
{
	struct spdk_nvmf_rdma_poller	*poller = NULL;
	struct poller_manage_ctx	*ctx = c;
	struct spdk_nvmf_rdma_device	*device;
	int				rc;

	rc = nvmf_rdma_poller_create(ctx->rtransport, ctx->rgroup, ctx->device, &poller);
	if (rc < 0 && poller) {
		nvmf_rdma_poller_destroy(poller);
	}

	device = ctx->device;
	if (nvmf_rdma_all_pollers_management_done(ctx)) {
		device->is_ready = true;
	}
}

static void
nvmf_sta_io_state_changed_cb(const union doca_data user_data,
			     struct doca_ctx *ctx,
			     enum doca_ctx_states prev_state,
			     enum doca_ctx_states next_state)
{
	struct spdk_nvmf_offload_poller *opoller = user_data.ptr;

	SPDK_DEBUGLOG(rdma_offload, "DOCA STA IO Context state is chnaged %s -> %s\n",
		      nvmf_rdma_sta_state_to_str(prev_state),
		      nvmf_rdma_sta_state_to_str(next_state));

	opoller->state = next_state;
}

static struct spdk_nvmf_offload_qpair *
get_offload_qpair_from_qp_handle(struct spdk_nvmf_offload_poller *opoller,
				 struct doca_sta_qp_handle *qp_handle)
{
	struct spdk_nvmf_offload_qpair find;

	find.handle = qp_handle;
	return RB_FIND(offload_qpairs_tree, &opoller->qpairs, &find);
}

static void
nvmf_rdma_offload_qpair_disconnect(struct spdk_nvmf_offload_qpair *oqpair)
{
	if (spdk_nvmf_qpair_is_active(&oqpair->common.qpair)) {
		spdk_nvmf_qpair_disconnect(&oqpair->common.qpair);
	}
}

static void
nvmf_sta_io_event_transport_err_cb(const struct doca_sta_event_transport_err *event,
				   union doca_data user_data)
{
	struct spdk_nvmf_offload_poller *opoller = user_data.ptr;

	SPDK_ERRLOG("nvmf_sta_io_event_transport_err_cb: opoller %p\n", opoller);
}

static void
nvmf_sta_io_non_offload_cb(struct doca_sta_qp_handle *qp_handle,
			   union doca_data user_data,
			   const uint8_t *nvme_cmd,
			   uint8_t *payload,
			   uint32_t payload_len,
			   bool payload_valid,
			   union doca_data non_offload_user_data)
{
	struct spdk_nvmf_offload_poller *opoller = user_data.ptr;
	struct spdk_nvmf_offload_qpair *oqpair;
	struct nvmf_non_offload_request *req;
	uint64_t receive_tsc = spdk_get_ticks();

	assert(opoller);

	oqpair = get_offload_qpair_from_qp_handle(opoller, qp_handle);
	if (!oqpair) {
		SPDK_ERRLOG("qpair is not found for qp_handle %p\n", qp_handle);
		return;
	}

	if (STAILQ_EMPTY(&opoller->resources->free_queue)) {
		SPDK_ERRLOG("No free entries for non-offload IO\n");
		nvmf_rdma_offload_qpair_disconnect(oqpair);
		return;
	}

	req = STAILQ_FIRST(&opoller->resources->free_queue);
	STAILQ_REMOVE_HEAD(&opoller->resources->free_queue, state_link);
	assert(req->state == RDMA_REQUEST_STATE_FREE);

	req->common.req.qpair = &oqpair->common.qpair;
	req->nvme_cmd = nvme_cmd;
	req->payload = payload;
	req->payload_len = payload_len;
	req->payload_valid = payload_valid;
	req->sta_context = non_offload_user_data;
	req->receive_tsc = receive_tsc;
	req->state = RDMA_REQUEST_STATE_NEW;
	STAILQ_INSERT_HEAD(&opoller->resources->incoming_queue, req, state_link);
	oqpair->qd++;

	nvmf_offload_qpair_process_pending(oqpair, false);
}

static void
nvmf_sta_io_rdma_write_send_comp_cb(struct doca_sta_producer_task_send *task,
				    union doca_data task_user_data)
{
	struct nvmf_non_offload_request *non_offload_req = task_user_data.ptr;

	SPDK_DEBUGLOG(rdma_offload, "RDMA_WRITE/SEND task comp, req %p\n", non_offload_req);
	assert(non_offload_req->task == task);
	doca_task_free(doca_sta_producer_send_task_as_task(non_offload_req->task));
	non_offload_req->task = NULL;

	non_offload_req->state = RDMA_REQUEST_STATE_COMPLETED;
	nvmf_sta_io_non_offload_request_process(non_offload_req);
}

static void
nvmf_sta_io_rdma_write_send_error_cb(struct doca_sta_producer_task_send *task,
				     union doca_data task_user_data)
{
	struct nvmf_non_offload_request *non_offload_req = task_user_data.ptr;
	struct spdk_nvmf_request *req = &non_offload_req->common.req;
	struct spdk_nvmf_offload_qpair *oqpair = nvmf_offload_qpair_get(req->qpair);;

	SPDK_ERRLOG("RDMA_WRITE/SEND task error, req %p\n", non_offload_req);
	assert(non_offload_req->task == task);
	doca_task_free(doca_sta_producer_send_task_as_task(non_offload_req->task));
	non_offload_req->task = NULL;

	non_offload_req->state = RDMA_REQUEST_STATE_COMPLETED;
	oqpair->in_error_state = 1;
	nvmf_sta_io_non_offload_request_process(non_offload_req);

	nvmf_rdma_offload_qpair_disconnect(oqpair);
}

static void
nvmf_sta_io_rdma_read_comp_cb(struct doca_sta_producer_task_send *task,
			      union doca_data task_user_data)
{
	struct nvmf_non_offload_request *non_offload_req = task_user_data.ptr;

	SPDK_DEBUGLOG(rdma_offload, "RDMA_READ task comp, req %p\n", non_offload_req);
	assert(non_offload_req->task == task);
	doca_task_free(doca_sta_producer_send_task_as_task(non_offload_req->task));
	non_offload_req->task = NULL;

	non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
	nvmf_sta_io_non_offload_request_process(non_offload_req);
}

static void
nvmf_sta_io_rdma_read_error_cb(struct doca_sta_producer_task_send *task,
			       union doca_data task_user_data)
{
	struct nvmf_non_offload_request *non_offload_req = task_user_data.ptr;
	struct spdk_nvmf_request *req = &non_offload_req->common.req;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_offload_qpair *oqpair = nvmf_offload_qpair_get(req->qpair);

	SPDK_ERRLOG("RDMA_READ task error, req %p\n", non_offload_req);
	assert(non_offload_req->task == task);
	doca_task_free(doca_sta_producer_send_task_as_task(non_offload_req->task));
	non_offload_req->task = NULL;

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	/* Submit a non-offload IO send task to let DOCA STA free the IO context. */
	STAILQ_INSERT_TAIL(&oqpair->pending_rdma_send_queue, non_offload_req, state_link);
	non_offload_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
	oqpair->in_error_state = 1;
	nvmf_sta_io_non_offload_request_process(non_offload_req);

	nvmf_rdma_offload_qpair_disconnect(oqpair);
}

static int
nvmf_offload_poller_destroy(struct spdk_nvmf_offload_poller *opoller)
{
	doca_error_t drc;

	if (opoller->resources) {
		nvmf_sta_non_offload_resources_destroy(opoller->resources);
		opoller->resources = NULL;
	}
	if (opoller->state == DOCA_CTX_STATE_RUNNING) {
		drc = doca_ctx_stop(opoller->io_ctx);
		if (DOCA_IS_ERROR(drc)) {
			if (drc != DOCA_ERROR_IN_PROGRESS) {
				SPDK_ERRLOG("Unable to stop DOCA STA IO: %s\n", doca_error_get_descr(drc));
				return -EINVAL;
			}
			assert(opoller->state == DOCA_CTX_STATE_STOPPING);
			while (opoller->state == DOCA_CTX_STATE_STOPPING) {
				doca_pe_progress(opoller->pe);
			}
			if (opoller->state != DOCA_CTX_STATE_IDLE) {
				SPDK_ERRLOG("Unexpected state %s of DOCA STA IO\n",
					    nvmf_rdma_sta_state_to_str(opoller->state));
				return -EINVAL;
			}
		}
	}
	if (opoller->sta_io) {
		drc = doca_sta_io_destroy(opoller->sta_io);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Unable to destroy doca_sta_io: %s\n", doca_error_get_descr(drc));
			return -EINVAL;
		}
		opoller->sta_io = NULL;
	}

	spdk_interrupt_unregister(&opoller->interrupt);
	if (opoller->pe) {
		drc = doca_pe_destroy(opoller->pe);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Unable to destroy doca_pe: %s\n", doca_error_get_descr(drc));
			return -EINVAL;
		}
		opoller->pe = NULL;
	}
	free(opoller);

	return 0;
}

static int
nvmf_rdma_offload_poller_interrupt(void *ctx)
{
	struct spdk_nvmf_offload_poller *opoller = ctx;
	doca_error_t drc;
	int num_events;

	drc = doca_pe_clear_notification(opoller->pe, opoller->notification_handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to clear PE event notification\n");
		return -1;
	}

	num_events = doca_pe_progress(opoller->pe);

	drc = doca_pe_request_notification(opoller->pe);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to request PE event notification\n");
		return -1;
	}

	return num_events;
}

static int
nvmf_offload_poller_create(struct spdk_nvmf_rdma_transport *rtransport,
			   struct spdk_nvmf_rdma_poll_group *rgroup,
			   struct spdk_nvmf_offload_poller **out_opoller)
{
	struct spdk_nvmf_offload_poller *opoller;
	union doca_data udata;
	doca_error_t drc;

	opoller = calloc(1, sizeof(*opoller));
	if (!opoller) {
		SPDK_ERRLOG("Cannot allocate memory for offload poller context\n");
		return -ENOMEM;
	}
	RB_INIT(&opoller->qpairs);
	opoller->notification_handle = doca_event_invalid_handle;
	opoller->state = DOCA_CTX_STATE_IDLE;

	/* The max_ios capability is specified per CQ handler. DOCA STA balances IOs among STA
	 * IOs, and this cannot be controlled outside of DOCA. Let's assume the worst case,
	 * where all IOs can be forwarded to the same STA IO.
	 */
	opoller->max_queue_depth = rtransport->sta.caps.max_ios * rtransport->sta.num_cq_handlers;
	SPDK_DEBUGLOG(rdma_offload, "max_io_num per poller: %u\n", opoller->max_queue_depth);

	drc = doca_sta_io_create(rtransport->sta.sta, &opoller->sta_io);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to create doca_sta_io: %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	opoller->io_ctx = doca_sta_io_as_ctx(opoller->sta_io);
	if (!opoller->io_ctx) {
		SPDK_ERRLOG("Unable to get doca_ctx\n");
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	drc = doca_pe_create(&opoller->pe);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to create doca_pe: %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		drc = doca_pe_set_event_mode(opoller->pe, DOCA_PE_EVENT_MODE_PROGRESS_ALL);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to set event mode: %s\n", doca_error_get_descr(drc));
			nvmf_offload_poller_destroy(opoller);
			return -EINVAL;
		}

		drc = doca_pe_get_notification_handle(opoller->pe, &opoller->notification_handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to get notification handle: %s\n", doca_error_get_descr(drc));
			nvmf_offload_poller_destroy(opoller);
			return -EINVAL;
		}

		opoller->interrupt =
			SPDK_INTERRUPT_REGISTER(opoller->notification_handle, nvmf_rdma_offload_poller_interrupt, opoller);
		if (!opoller->interrupt) {
			SPDK_ERRLOG("Failed to regiter interrupt\n");
			nvmf_offload_poller_destroy(opoller);
			return -EINVAL;
		}
	}

	drc = doca_pe_connect_ctx(opoller->pe, opoller->io_ctx);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to connect doca_pe: %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	drc = doca_sta_io_task_disconnect_set_conf(opoller->sta_io,
			nvmf_sta_io_disconnect_comp_hadler,
			nvmf_sta_io_disconnect_error_hadler);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to set disconnect handlers for doca_sta_io: %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	udata.ptr = opoller;
	drc = doca_sta_io_event_transport_err_register_cb(opoller->sta_io,
			nvmf_sta_io_event_transport_err_cb, udata);
	if (drc != DOCA_SUCCESS) {
		SPDK_ERRLOG("Failed to register event_transport_err_cb: %s", doca_error_get_name(drc));
		return -EINVAL;
	}

	drc = doca_sta_io_non_offload_register_cb(opoller->sta_io, nvmf_sta_io_non_offload_cb, udata);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to set non-offload handler doca_sta_io: %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	drc = doca_sta_io_task_non_offload_set_rdma_write_send_conf(opoller->sta_io,
			nvmf_sta_io_rdma_write_send_comp_cb,
			nvmf_sta_io_rdma_write_send_error_cb);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to set rdma_write completio handler doca_sta_io: %s\n",
			    doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	drc = doca_sta_io_task_non_offload_set_rdma_read_conf(opoller->sta_io,
			nvmf_sta_io_rdma_read_comp_cb,
			nvmf_sta_io_rdma_read_error_cb);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to set rdma_read completio handler doca_sta_io: %s\n",
			    doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	udata.ptr = opoller;
	drc = doca_ctx_set_user_data(opoller->io_ctx, udata);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Unable to set udata for doca_sta_io %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}
	drc = doca_ctx_set_state_changed_cb(opoller->io_ctx, nvmf_sta_io_state_changed_cb);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Unable to set state changed callback for doca_sta_io %s\n", doca_error_get_descr(drc));
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}
	drc = doca_ctx_start(opoller->io_ctx);
	if (DOCA_IS_ERROR(drc)) {
		if (drc != DOCA_ERROR_IN_PROGRESS) {
			SPDK_ERRLOG("Unable to start doca_sta_io: %s\n", doca_error_get_descr(drc));
			nvmf_offload_poller_destroy(opoller);
			return -EINVAL;
		}

		assert(opoller->state == DOCA_CTX_STATE_STARTING);
		while (opoller->state == DOCA_CTX_STATE_STARTING) {
			doca_pe_progress(opoller->pe);
		}

		if (opoller->state != DOCA_CTX_STATE_RUNNING) {
			SPDK_NOTICELOG("Wrong DOCA STA IO state %s\n", nvmf_rdma_sta_state_to_str(opoller->state));
			nvmf_offload_poller_destroy(opoller);
			return -EINVAL;
		}
	}
	opoller->resources = nvmf_sta_non_offload_resources_create(opoller->max_queue_depth);
	if (!opoller->resources) {
		SPDK_ERRLOG("Failed to create resources for non-offloaded IOs\n");
		nvmf_offload_poller_destroy(opoller);
		return -EINVAL;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		drc = doca_pe_request_notification(opoller->pe);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Request notification for PE failed: %s\n", doca_error_get_descr(drc));
			nvmf_offload_poller_destroy(opoller);
			return -EINVAL;
		}
	}

	*out_opoller = opoller;
	return 0;
}

static void nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport,
			    struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;
	int					rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		rc = nvmf_rdma_poller_create(rtransport, rgroup, device, &poller);
		if (rc < 0) {
			nvmf_rdma_poll_group_destroy(&rgroup->group);
			return NULL;
		}
	}

	TAILQ_INSERT_TAIL(&rtransport->poll_groups, rgroup, link);
	if (rtransport->conn_sched.next_admin_pg == NULL) {
		rtransport->conn_sched.next_admin_pg = rgroup;
		rtransport->conn_sched.next_io_pg = rgroup;
	}

	rc = nvmf_offload_poller_create(rtransport, rgroup, &rgroup->offload_poller);
	if (rc) {
		nvmf_rdma_poll_group_destroy(&rgroup->group);
		return NULL;
	}

	return &rgroup->group;
}

static uint32_t
nvmf_poll_group_get_io_qpair_count(struct spdk_nvmf_poll_group *pg)
{
	uint32_t count;

	/* Just assume that unassociated qpairs will eventually be io
	 * qpairs.  This is close enough for the use cases for this
	 * function.
	 */
	pthread_mutex_lock(&pg->mutex);
	count = pg->stat.current_io_qpairs + pg->current_unassociated_qpairs;
	pthread_mutex_unlock(&pg->mutex);

	return count;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group **pg;
	struct spdk_nvmf_transport_poll_group *result;
	uint32_t count;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);

	if (TAILQ_EMPTY(&rtransport->poll_groups)) {
		return NULL;
	}

	if (qpair->qid == 0) {
		pg = &rtransport->conn_sched.next_admin_pg;
	} else {
		struct spdk_nvmf_rdma_poll_group *pg_min, *pg_start, *pg_current;
		uint32_t min_value;

		pg = &rtransport->conn_sched.next_io_pg;
		pg_min = *pg;
		pg_start = *pg;
		pg_current = *pg;
		min_value = nvmf_poll_group_get_io_qpair_count(pg_current->group.group);

		while (1) {
			count = nvmf_poll_group_get_io_qpair_count(pg_current->group.group);

			if (count < min_value) {
				min_value = count;
				pg_min = pg_current;
			}

			pg_current = TAILQ_NEXT(pg_current, link);
			if (pg_current == NULL) {
				pg_current = TAILQ_FIRST(&rtransport->poll_groups);
			}

			if (pg_current == pg_start || min_value == 0) {
				break;
			}
		}
		*pg = pg_min;
	}

	assert(*pg != NULL);

	result = &(*pg)->group;

	*pg = TAILQ_NEXT(*pg, link);
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&rtransport->poll_groups);
	}

	return result;
}

static void
nvmf_rdma_poller_destroy(struct spdk_nvmf_rdma_poller *poller)
{
	struct spdk_nvmf_rdma_qpair	*qpair, *tmp_qpair;
	int rc;

	TAILQ_REMOVE(&poller->group->pollers, poller, link);
	RB_FOREACH_SAFE(qpair, qpairs_tree, &poller->qpairs, tmp_qpair) {
		nvmf_rdma_qpair_destroy(qpair);
	}

	if (poller->srq) {
		if (poller->resources) {
			nvmf_rdma_resources_destroy(poller->resources);
		}
		spdk_rdma_provider_srq_destroy(poller->srq);
		SPDK_DEBUGLOG(rdma_offload, "Destroyed RDMA shared queue %p\n", poller->srq);
	}

	if (poller->cq) {
		spdk_rdma_provider_cq_destroy(poller->cq);
	}

	spdk_interrupt_unregister(&poller->interrupt);
	if (poller->comp_channel) {
		rc = ibv_destroy_comp_channel(poller->comp_channel);
		if (rc != 0) {
			SPDK_ERRLOG("Destroy comp channel return %d, error: %s\n", rc, strerror(errno));
		}
	}

	if (poller->destroy_cb) {
		poller->destroy_cb(poller->destroy_cb_ctx);
		poller->destroy_cb = NULL;
	}

	free(poller);
}

static void
nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup, *next_rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;
	struct spdk_nvmf_rdma_transport		*rtransport;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	if (!rgroup) {
		return;
	}

	if (rgroup->offload_poller) {
		nvmf_offload_poller_destroy(rgroup->offload_poller);
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		nvmf_rdma_poller_destroy(poller);
	}

	if (rgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_rdma_poll_group_create()
		 * calls this function directly in a failure path. */
		free(rgroup);
		return;
	}

	rtransport = SPDK_CONTAINEROF(rgroup->group.transport, struct spdk_nvmf_rdma_transport, transport);

	next_rgroup = TAILQ_NEXT(rgroup, link);
	TAILQ_REMOVE(&rtransport->poll_groups, rgroup, link);
	if (next_rgroup == NULL) {
		next_rgroup = TAILQ_FIRST(&rtransport->poll_groups);
	}
	if (rtransport->conn_sched.next_admin_pg == rgroup) {
		rtransport->conn_sched.next_admin_pg = next_rgroup;
	}
	if (rtransport->conn_sched.next_io_pg == rgroup) {
		rtransport->conn_sched.next_io_pg = next_rgroup;
	}

	free(rgroup);
}

static void
nvmf_rdma_qpair_reject_connection(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->cm_id != NULL) {
		nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
	}
}

static int
nvmf_rdma_poll_group_add_rdma_qpair(struct spdk_nvmf_rdma_poll_group *rgroup,
				    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_poller	*poller;
	int				rc;

	rqpair = nvmf_rdma_qpair_get(qpair);
	device = rqpair->device;

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	if (poller->need_destroy) {
		SPDK_ERRLOG("Poller is destroying.\n");
		return -1;
	}

	rqpair->poller = poller;
	rqpair->srq = rqpair->poller->srq;

	rc = nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		rqpair->poller = NULL;
		rqpair->srq = NULL;
		return -1;
	}

	RB_INSERT(qpairs_tree, &poller->qpairs, rqpair);

	rc = nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	if (rc) {
		/* Try to reject, but we probably can't */
		nvmf_rdma_qpair_reject_connection(rqpair);
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_poll_group_add_offload_qpair(struct spdk_nvmf_rdma_poll_group *rgroup,
				       struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_offload_qpair	*oqpair;
	struct spdk_nvmf_offload_poller	*opoller;
	int				rc;

	oqpair = nvmf_offload_qpair_get(qpair);
	opoller = rgroup->offload_poller;

	if (opoller->need_destroy) {
		SPDK_ERRLOG("Poller is destroying.\n");
		return -1;
	}

	oqpair->opoller = opoller;

	rc = nvmf_offload_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		oqpair->opoller = NULL;
		return -1;
	}

	oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_CONNECTED;
	RB_INSERT(offload_qpairs_tree, &opoller->qpairs, oqpair);

	return 0;
}

static int
nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_common_qpair		*cqpair;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);

	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		return nvmf_rdma_poll_group_add_rdma_qpair(rgroup, qpair);
	} else if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD) {
		return nvmf_rdma_poll_group_add_offload_qpair(rgroup, qpair);
	}
	SPDK_ERRLOG("Unknown qpair type %d\n", cqpair->type);

	return -1;
}

static int
nvmf_rdma_poll_group_remove_rdma_qpair(struct spdk_nvmf_transport_poll_group *group,
				       struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = nvmf_rdma_qpair_get(qpair);
	assert(group->transport->tgt != NULL);

	rqpair->destruct_channel = spdk_get_io_channel(group->transport->tgt);

	if (!rqpair->destruct_channel) {
		SPDK_WARNLOG("failed to get io_channel, qpair %p\n", qpair);
		return 0;
	}

	/* Sanity check that we get io_channel on the correct thread */
	if (qpair->group) {
		assert(qpair->group->thread == spdk_io_channel_get_thread(rqpair->destruct_channel));
	}

	return 0;
}

static int
nvmf_rdma_poll_group_remove_offload_qpair(struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_offload_qpair *oqpair;

	oqpair = nvmf_offload_qpair_get(qpair);
	assert(group->transport->tgt != NULL);

	oqpair->destruct_channel = spdk_get_io_channel(group->transport->tgt);

	if (!oqpair->destruct_channel) {
		SPDK_WARNLOG("failed to get io_channel, qpair %p\n", oqpair);
		return 0;
	}

	/* Sanity check that we get io_channel on the correct thread */
	if (qpair->group) {
		assert(qpair->group->thread == spdk_io_channel_get_thread(oqpair->destruct_channel));
	}

	return 0;
}

static int
nvmf_rdma_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_common_qpair		*cqpair;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);

	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		return nvmf_rdma_poll_group_remove_rdma_qpair(group, qpair);
	} else if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD) {
		return nvmf_rdma_poll_group_remove_offload_qpair(group, qpair);
	}
	SPDK_ERRLOG("Unknown qpair type %d\n", cqpair->type);

	return -1;
}

static int
nvmf_non_offload_request_free(struct nvmf_non_offload_request *non_offload_req)
{
	struct spdk_nvmf_offload_qpair *oqpair;

	oqpair = nvmf_offload_qpair_get(non_offload_req->common.req.qpair);

	non_offload_req->common.req.length = 0;
	non_offload_req->common.req.iovcnt = 0;
	non_offload_req->common.req.dif_enabled = false;

	memset(&non_offload_req->common.req.dif, 0, sizeof(non_offload_req->common.req.dif));

	STAILQ_INSERT_HEAD(&oqpair->opoller->resources->free_queue, non_offload_req, state_link);
	non_offload_req->state = RDMA_REQUEST_STATE_FREE;
	oqpair->qd--;

	if (spdk_unlikely(oqpair->state == SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINING && oqpair->qd == 0)) {
		oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINED;
		nvmf_rdma_offload_qpair_close_process(oqpair);
	}

	return 0;
}

static int
nvmf_rdma_request_free(struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(
				rdma_req->common.req.qpair->transport,
				struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_qpair *rqpair = nvmf_rdma_qpair_get(rdma_req->common.req.qpair);

	/*
	 * AER requests are freed when a qpair is destroyed. The recv corresponding to that request
	 * needs to be returned to the shared receive queue or the poll group will eventually be
	 * starved of RECV structures.
	 */
	if (rqpair->srq && rdma_req->recv) {
		int rc;
		struct ibv_recv_wr *bad_recv_wr;

		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_req->recv->wr);
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		}
	}

	_nvmf_rdma_request_free(rdma_req, rtransport);
	return 0;
}

static int
nvmf_rdma_offload_request_free(struct spdk_nvmf_request *req)
{
	struct nvmf_offload_common_request *common_req;

	common_req = SPDK_CONTAINEROF(req, struct nvmf_offload_common_request, req);

	if (common_req->type == NVMF_OFFLOAD_REQUEST_TYPE_RDMA) {
		return nvmf_rdma_request_free(nvmf_rdma_request_get(req));
	}
	if (common_req->type == NVMF_OFFLOAD_REQUEST_TYPE_NON_OFFLOAD) {
		return nvmf_non_offload_request_free(nvmf_non_offload_request_get(req));
	}

	return -EINVAL;
}

static int
nvmf_rdma_request_complete(struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(
				rdma_req->common.req.qpair->transport,
				struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_qpair     *rqpair = nvmf_rdma_qpair_get(rdma_req->common.req.qpair);

	if (spdk_unlikely(rqpair->ibv_in_error_state)) {
		/* The connection is dead. Move the request directly to the completed state. */
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	} else {
		/* The connection is alive, so process the request as normal */
		rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	}

	nvmf_rdma_request_process(rtransport, rdma_req);

	return 0;
}

static int
nvmf_non_offload_request_complete(struct nvmf_non_offload_request *non_offload_req)
{
	non_offload_req->state = RDMA_REQUEST_STATE_EXECUTED;
	nvmf_sta_io_non_offload_request_process(non_offload_req);

	return 0;
}

static int
nvmf_rdma_offload_request_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_offload_common_request *common_req;

	common_req = SPDK_CONTAINEROF(req, struct nvmf_offload_common_request, req);

	if (common_req->type == NVMF_OFFLOAD_REQUEST_TYPE_RDMA) {
		return nvmf_rdma_request_complete(nvmf_rdma_request_get(req));
	}
	if (common_req->type == NVMF_OFFLOAD_REQUEST_TYPE_NON_OFFLOAD) {
		return nvmf_non_offload_request_complete(nvmf_non_offload_request_get(req));
	}

	return -EINVAL;
}

static void
nvmf_rdma_close_qpair_rdma(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair *rqpair = nvmf_rdma_qpair_get(qpair);

	rqpair->to_close = true;

	if (qpair->state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		nvmf_rdma_qpair_reject_connection(rqpair);
	}
	if (rqpair->rdma_qp) {
		spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);
	}

	nvmf_rdma_destroy_drained_qpair(rqpair);
}

static void
nvmf_rdma_offload_qpair_drain(struct spdk_nvmf_offload_qpair *oqpair)
{
	nvmf_offload_qpair_process_pending(oqpair, true);

	if (oqpair->qd == 0) {
		oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINED;
		return;
	}
}

static void
nvmf_rdma_offload_qpair_destroy(struct spdk_nvmf_offload_qpair *oqpair)
{
	doca_error_t drc;

	if (oqpair->opoller) {
		if (oqpair->handle) {
			drc = doca_sta_io_qp_destroy(oqpair->opoller->sta_io, oqpair->handle);
			if (DOCA_IS_ERROR(drc)) {
				SPDK_ERRLOG("Unable to destroy DOCA STA IO QP: %s\n",
					    doca_error_get_descr(drc));
			}
		}
		RB_REMOVE(offload_qpairs_tree, &oqpair->opoller->qpairs, oqpair);
	}
	if (oqpair->destruct_channel) {
		spdk_put_io_channel(oqpair->destruct_channel);
	}
	if (oqpair->opoller && oqpair->opoller->need_destroy && RB_EMPTY(&oqpair->opoller->qpairs)) {
		nvmf_offload_poller_destroy(oqpair->opoller);
	}
	if (oqpair->cm_id) {
		rdma_destroy_id(oqpair->cm_id);
	}
}

static void
nvmf_rdma_offload_qpair_close_process(struct spdk_nvmf_offload_qpair *oqpair)
{
	enum spdk_nvmf_offload_qpair_state prev_state;
	union doca_data task_user_data;
	doca_error_t drc;

	/* Prevent recursive calls from nvmf_rdma_offload_qpair_drain(). */
	if (oqpair->in_close) {
		return;
	}
	oqpair->in_close = 1;

	do {
		prev_state = oqpair->state;

		SPDK_DEBUGLOG(rdma_offload, "offload qpair state %d\n", oqpair->state);

		switch (oqpair->state) {
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_UNINITIALIZED:
			oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_REJECT;
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_REJECT:
			assert(oqpair->cm_id);
			nvmf_rdma_event_reject(oqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
			oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_PENDING_TO_CLOSE;
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_CONNECTED:
			task_user_data.ptr = oqpair;
			drc = doca_sta_io_task_disconnect_alloc_init(oqpair->opoller->sta_io,
					task_user_data, oqpair->handle, &oqpair->destroy_task);
			if (DOCA_IS_ERROR(drc)) {
				SPDK_ERRLOG("Failed to alloc disconnect task for offload QP %p: %s\n",
					    oqpair->handle, doca_error_get_descr(drc));
				break;
			}
			drc = submit_doca_task(doca_sta_producer_send_task_as_task(oqpair->destroy_task));
			if (DOCA_IS_ERROR(drc)) {
				SPDK_ERRLOG("Failed to submit disconnect task for IO QP %p: %s\n",
					    oqpair->handle, doca_error_get_descr(drc));
				doca_task_free(doca_sta_producer_send_task_as_task(oqpair->destroy_task));
				oqpair->destroy_task = NULL;
				break;
			}

			oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTING;
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTING:
			/* Some external code must kick a request into SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTED
			 * or SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECT_FAILED to escape this state.
			 */
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECT_FAILED:
			/* There is no good option to handle this error. Retrying to disconnect is unlikely to help.
			 * Try to close the QP anyway.
			 */
			oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINING;
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTED:
			oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINING;
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINING:
			nvmf_rdma_offload_qpair_drain(oqpair);
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_DRAINED:
			oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_PENDING_TO_CLOSE;
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_PENDING_TO_CLOSE:
			if (oqpair->to_close) {
				oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_CLOSE;
			}
			break;
		case SPDK_NVMF_OFFLOAD_QPAIR_STATE_CLOSE:
			nvmf_rdma_offload_qpair_destroy(oqpair);
			free(oqpair);
			return;
		default:
			SPDK_ERRLOG("Unknown offload qpair state %d, handle %p\n", oqpair->state, oqpair->handle);
			assert(0);
		}
	} while (prev_state != oqpair->state);

	oqpair->in_close = 0;
}

static void
nvmf_sta_io_disconnect_comp_hadler(struct doca_sta_producer_task_send *task,
				   union doca_data task_user_data)
{
	struct spdk_nvmf_offload_qpair *oqpair = task_user_data.ptr;

	assert(oqpair);
	assert(oqpair->destroy_task == task);
	assert(oqpair->state == SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTING);

	doca_task_free(doca_sta_producer_send_task_as_task(task));
	oqpair->destroy_task = NULL;

	SPDK_DEBUGLOG(rdma_offload, "Disconect task completed for IO QP %p\n", oqpair->handle);
	oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTED;
	nvmf_rdma_offload_qpair_close_process(oqpair);
}

static void
nvmf_sta_io_disconnect_error_hadler(struct doca_sta_producer_task_send *task,
				    union doca_data task_user_data)
{
	struct spdk_nvmf_offload_qpair *oqpair = task_user_data.ptr;

	assert(oqpair);
	assert(oqpair->destroy_task == task);
	assert(oqpair->state == SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECTING);

	doca_task_free(doca_sta_producer_send_task_as_task(task));
	oqpair->destroy_task = NULL;

	SPDK_ERRLOG("Disconect task failed for IO QP %p\n", oqpair->handle);
	oqpair->state = SPDK_NVMF_OFFLOAD_QPAIR_STATE_DISCONNECT_FAILED;
	nvmf_rdma_offload_qpair_close_process(oqpair);
}

static void
nvmf_rdma_close_qpair_offload(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_offload_qpair *oqpair = nvmf_offload_qpair_get(qpair);

	oqpair->to_close = 1;
	nvmf_rdma_offload_qpair_close_process(oqpair);
}

static void
nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_common_qpair *cqpair;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);

	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		nvmf_rdma_close_qpair_rdma(qpair);
	} else if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD) {
		nvmf_rdma_close_qpair_offload(qpair);
	} else {
		SPDK_ERRLOG("Unknown qpair type %d\n", cqpair->type);
	}

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static struct spdk_nvmf_rdma_qpair *
get_rdma_qpair_from_wc(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_qpair find;

	find.qp_num = wc->qp_num;

	return RB_FIND(qpairs_tree, &rpoller->qpairs, &find);
}

#ifdef DEBUG
static int
nvmf_rdma_req_is_completing(struct spdk_nvmf_rdma_request *rdma_req)
{
	return rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST ||
	       rdma_req->state == RDMA_REQUEST_STATE_COMPLETING;
}
#endif

static void
_poller_reset_failed_recvs(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_recv_wr *bad_recv_wr,
			   int rc)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;

	SPDK_ERRLOG("Failed to post a recv for the poller %p with errno %d\n", rpoller, -rc);
	while (bad_recv_wr != NULL) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_recv_wr->wr_id;
		rdma_recv = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);

		rdma_recv->qpair->current_recv_depth++;
		bad_recv_wr = bad_recv_wr->next;
		SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rdma_recv->qpair, -rc);
		spdk_nvmf_qpair_disconnect(&rdma_recv->qpair->common.qpair);
	}
}

static void
_qp_reset_failed_recvs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rqpair, -rc);
	while (bad_recv_wr != NULL) {
		bad_recv_wr = bad_recv_wr->next;
		rqpair->current_recv_depth++;
	}
	spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
}

static void
_poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_recv_wr		*bad_recv_wr;
	int				rc;

	if (rpoller->srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->srq, &bad_recv_wr);
		if (spdk_unlikely(rc)) {
			_poller_reset_failed_recvs(rpoller, bad_recv_wr, rc);
		}
	} else {
		while (!STAILQ_EMPTY(&rpoller->qpairs_pending_recv)) {
			rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_recv);
			rc = spdk_rdma_provider_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);
			if (spdk_unlikely(rc)) {
				_qp_reset_failed_recvs(rqpair, bad_recv_wr, rc);
			}
			STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_recv, recv_link);
		}
	}
}

static void
_qp_reset_failed_sends(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_send_wr *bad_wr, int rc)
{
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;
	struct spdk_nvmf_rdma_request	*prev_rdma_req = NULL, *cur_rdma_req = NULL;

	SPDK_ERRLOG("Failed to post a send for the qpair %p with errno %d\n", rqpair, -rc);
	for (; bad_wr != NULL; bad_wr = bad_wr->next) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_wr->wr_id;
		assert(rqpair->current_send_depth > 0);
		rqpair->current_send_depth--;
		switch (bad_rdma_wr->type) {
		case RDMA_WR_TYPE_DATA:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, data_wr);
			if (bad_wr->opcode == IBV_WR_RDMA_READ) {
				assert(rqpair->current_read_depth > 0);
				rqpair->current_read_depth--;
			}
			break;
		case RDMA_WR_TYPE_SEND:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, rsp_wr);
			break;
		default:
			SPDK_ERRLOG("Found a RECV in the list of pending SEND requests for qpair %p\n", rqpair);
			prev_rdma_req = cur_rdma_req;
			continue;
		}

		if (prev_rdma_req == cur_rdma_req) {
			/* this request was handled by an earlier wr. i.e. we were performing an nvme read. */
			/* We only have to check against prev_wr since each requests wrs are contiguous in this list. */
			continue;
		}

		switch (cur_rdma_req->state) {
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			cur_rdma_req->common.req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, cur_rdma_req, state_link);
			cur_rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
		case RDMA_REQUEST_STATE_COMPLETING:
			cur_rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			break;
		default:
			SPDK_ERRLOG("Found a request in a bad state %d when draining pending SEND requests for qpair %p\n",
				    cur_rdma_req->state, rqpair);
			continue;
		}

		nvmf_rdma_request_process(rtransport, cur_rdma_req);
		prev_rdma_req = cur_rdma_req;
	}

	if (spdk_nvmf_qpair_is_active(&rqpair->common.qpair)) {
		/* Disconnect the connection. */
		spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
	}

}

static void
_poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_send_wr		*bad_wr = NULL;
	int				rc;

	while (!STAILQ_EMPTY(&rpoller->qpairs_pending_send)) {
		rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_send);
		rc = spdk_rdma_provider_qp_flush_send_wrs(rqpair->rdma_qp, &bad_wr);

		/* bad wr always points to the first wr that failed. */
		if (spdk_unlikely(rc)) {
			_qp_reset_failed_sends(rtransport, rqpair, bad_wr, rc);
		}
		STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_send, send_link);
	}
}

static const char *
nvmf_rdma_wr_type_str(enum spdk_nvmf_rdma_wr_type wr_type)
{
	switch (wr_type) {
	case RDMA_WR_TYPE_RECV:
		return "RECV";
	case RDMA_WR_TYPE_SEND:
		return "SEND";
	case RDMA_WR_TYPE_DATA:
		return "DATA";
	default:
		SPDK_ERRLOG("Unknown WR type %d\n", wr_type);
		SPDK_UNREACHABLE();
	}
}

static inline void
nvmf_rdma_log_wc_status(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	enum spdk_nvmf_rdma_wr_type wr_type = ((struct spdk_nvmf_rdma_wr *)wc->wr_id)->type;

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		SPDK_DEBUGLOG(rdma_offload,
			      "Error on CQ %p, (qp state %d, in_error %d) request 0x%lu, type %s, status: (%d): %s\n",
			      rqpair->poller->cq, rqpair->common.qpair.state, rqpair->ibv_in_error_state, wc->wr_id,
			      nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	} else {
		SPDK_ERRLOG("Error on CQ %p, (qp state %d, in_error %d) request 0x%lu, type %s, status: (%d): %s\n",
			    rqpair->poller->cq, rqpair->common.qpair.state, rqpair->ibv_in_error_state, wc->wr_id,
			    nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	}
}

static int
nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
		      struct spdk_nvmf_rdma_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_rdma_wr	*rdma_wr;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_qpair	*rqpair, *tmp_rqpair;
	int reaped, i;
	int count = 0;
	int rc;
	bool error = false;
	uint64_t poll_tsc = spdk_get_ticks();

	if (spdk_unlikely(rpoller->need_destroy)) {
		/* If qpair is closed before poller destroy, nvmf_rdma_destroy_drained_qpair may not
		 * be called because we cannot poll anything from cq. So we call that here to force
		 * destroy the qpair after to_close turning true.
		 */
		RB_FOREACH_SAFE(rqpair, qpairs_tree, &rpoller->qpairs, tmp_rqpair) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
		return 0;
	}

	/* Poll for completing operations. */
	reaped = spdk_rdma_provider_cq_poll(rpoller->cq, 32, wc);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	} else if (reaped == 0) {
		rpoller->stat.idle_polls++;
	}

	rpoller->stat.polls++;
	rpoller->stat.completions += reaped;

	for (i = 0; i < reaped; i++) {

		rdma_wr = (struct spdk_nvmf_rdma_wr *)wc[i].wr_id;

		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_SEND:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, rsp_wr);
			rqpair = nvmf_rdma_qpair_get(rdma_req->common.req.qpair);

			if (spdk_likely(!wc[i].status)) {
				count++;
				assert(wc[i].opcode == IBV_WC_SEND);
				assert(nvmf_rdma_req_is_completing(rdma_req));
			}

			rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			/* RDMA_WRITE operation completed. +1 since it was chained with rsp WR */
			assert(rqpair->current_send_depth >= (uint32_t)rdma_req->num_outstanding_data_wr + 1);
			rqpair->current_send_depth -= rdma_req->num_outstanding_data_wr + 1;
			rdma_req->num_outstanding_data_wr = 0;

			nvmf_rdma_request_process(rtransport, rdma_req);
			break;
		case RDMA_WR_TYPE_RECV:
			/* rdma_recv->qpair will be invalid if using an SRQ.  In that case we have to get the qpair from the wc. */
			rdma_recv = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);
			if (rpoller->srq != NULL) {
				rdma_recv->qpair = get_rdma_qpair_from_wc(rpoller, &wc[i]);
				/* It is possible that there are still some completions for destroyed QP
				 * associated with SRQ. We just ignore these late completions and re-post
				 * receive WRs back to SRQ.
				 */
				if (spdk_unlikely(NULL == rdma_recv->qpair)) {
					struct ibv_recv_wr *bad_wr;

					rdma_recv->wr.next = NULL;
					spdk_rdma_provider_srq_queue_recv_wrs(rpoller->srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->srq, &bad_wr);
					if (rc) {
						SPDK_ERRLOG("Failed to re-post recv WR to SRQ, err %d\n", rc);
					}
					continue;
				}
			}
			rqpair = rdma_recv->qpair;

			assert(rqpair != NULL);
			if (spdk_likely(!wc[i].status)) {
				assert(wc[i].opcode == IBV_WC_RECV);
				if (rqpair->current_recv_depth >= rqpair->max_queue_depth) {
					spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
					break;
				}
			}

			rdma_recv->wr.next = NULL;
			rqpair->current_recv_depth++;
			rdma_recv->receive_tsc = poll_tsc;
			rpoller->stat.requests++;
			STAILQ_INSERT_HEAD(&rqpair->resources->incoming_queue, rdma_recv, link);
			rqpair->common.qpair.queue_depth++;
			break;
		case RDMA_WR_TYPE_DATA:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, data_wr);
			rqpair = nvmf_rdma_qpair_get(rdma_req->common.req.qpair);

			assert(rdma_req->num_outstanding_data_wr > 0);

			rqpair->current_send_depth--;
			rdma_req->num_outstanding_data_wr--;
			if (spdk_likely(!wc[i].status)) {
				assert(wc[i].opcode == IBV_WC_RDMA_READ);
				rqpair->current_read_depth--;
				/* wait for all outstanding reads associated with the same rdma_req to complete before proceeding. */
				if (rdma_req->num_outstanding_data_wr == 0) {
					if (rdma_req->num_remaining_data_wr) {
						/* Only part of RDMA_READ operations was submitted, process the rest */
						nvmf_rdma_request_reset_transfer_in(rdma_req, rtransport);
						rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
						nvmf_rdma_request_process(rtransport, rdma_req);
						break;
					}
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
					nvmf_rdma_request_process(rtransport, rdma_req);
				}
			} else {
				/* If the data transfer fails still force the queue into the error state,
				 * if we were performing an RDMA_READ, we need to force the request into a
				 * completed state since it wasn't linked to a send. However, in the RDMA_WRITE
				 * case, we should wait for the SEND to complete. */
				if (rdma_req->data.wr.opcode == IBV_WR_RDMA_READ) {
					rqpair->current_read_depth--;
					if (rdma_req->num_outstanding_data_wr == 0) {
						if (rdma_req->num_remaining_data_wr) {
							/* Partially sent request is still in the pending_rdma_read_queue,
							 * remove it now before completing */
							rdma_req->num_remaining_data_wr = 0;
							STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
						}
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						nvmf_rdma_request_process(rtransport, rdma_req);
					}
				}
			}
			break;
		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			continue;
		}

		/* Handle error conditions */
		if (spdk_unlikely(wc[i].status)) {
			rqpair->ibv_in_error_state = true;
			nvmf_rdma_log_wc_status(rqpair, &wc[i]);

			error = true;

			if (spdk_nvmf_qpair_is_active(&rqpair->common.qpair)) {
				/* Disconnect the connection. */
				spdk_nvmf_qpair_disconnect(&rqpair->common.qpair);
			} else {
				nvmf_rdma_destroy_drained_qpair(rqpair);
			}
			continue;
		}

		nvmf_rdma_qpair_process_pending(rtransport, rqpair, false);

		if (spdk_unlikely(!spdk_nvmf_qpair_is_active(&rqpair->common.qpair))) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
	}

	if (spdk_unlikely(error == true)) {
		return -1;
	}

	if (reaped == 0) {
		/* In some cases we may not receive any CQE but we still may have pending IO requests waiting for
		 * a resource (e.g. a WR from the data_wr_pool).
		 * We need to start processing of such requests if no CQE reaped */
		nvmf_rdma_poller_process_pending_buf_queue(rtransport, rpoller);
	}

	return count;
}

static void
_nvmf_rdma_remove_destroyed_device(void *c)
{
	struct spdk_nvmf_rdma_transport	*rtransport = c;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;
	int				rc;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		if (device->ready_to_destroy) {
			destroy_ib_device(rtransport, device);
		}
	}

	free_poll_fds(rtransport);
	rc = generate_poll_fds(rtransport);
	/* cannot handle fd allocation error here */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to generate poll fds after remove ib device.\n");
	}
}

static void
_nvmf_rdma_remove_poller_in_group_cb(void *c)
{
	struct poller_manage_ctx	*ctx = c;
	struct spdk_nvmf_rdma_transport	*rtransport = ctx->rtransport;
	struct spdk_nvmf_rdma_device	*device = ctx->device;
	struct spdk_thread		*thread = ctx->thread;

	if (nvmf_rdma_all_pollers_management_done(c)) {
		/* destroy device when last poller is destroyed */
		device->ready_to_destroy = true;
		spdk_thread_send_msg(thread, _nvmf_rdma_remove_destroyed_device, rtransport);
	}
}

static void
_nvmf_rdma_remove_poller_in_group(void *c)
{
	struct poller_manage_ctx		*ctx = c;

	ctx->rpoller->need_destroy = true;
	ctx->rpoller->destroy_cb_ctx = ctx;
	ctx->rpoller->destroy_cb = _nvmf_rdma_remove_poller_in_group_cb;

	/* qp will be disconnected after receiving a RDMA_CM_EVENT_DEVICE_REMOVAL event. */
	if (RB_EMPTY(&ctx->rpoller->qpairs)) {
		nvmf_rdma_poller_destroy(ctx->rpoller);
	}
}

static int
nvmf_rdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller	*rpoller, *tmp;
	int				count = 0, rc, rc2 = 0;

	rtransport = SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	TAILQ_FOREACH_SAFE(rpoller, &rgroup->pollers, link, tmp) {
		rc = nvmf_rdma_poller_poll(rtransport, rpoller);
		if (spdk_unlikely(rc < 0)) {
			if (rc2 == 0) {
				rc2 = rc;
			}
			continue;
		}
		count += rc;
	}
	doca_pe_progress(rgroup->offload_poller->pe);

	return rc2 ? rc2 : count;
}

static int
nvmf_rdma_trid_from_sockaddr(struct sockaddr *saddr,
			     uint16_t port,
			     struct spdk_nvme_transport_id *trid)
{
	trid->trtype = spdk_nvmf_transport_rdma_offload.type;
	snprintf(trid->trstring, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", spdk_nvmf_transport_rdma_offload.name);

	switch (saddr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *saddr_in = (struct sockaddr_in *)saddr;

		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
		inet_ntop(AF_INET, &saddr_in->sin_addr,
			  trid->traddr, sizeof(trid->traddr));
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *saddr_in6 = (struct sockaddr_in6 *)saddr;

		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
		inet_ntop(AF_INET6, &saddr_in6->sin6_addr,
			  trid->traddr, sizeof(trid->traddr));
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	default:
		SPDK_ERRLOG("Unsupported address family %d\n", saddr->sa_family);
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
			  struct spdk_nvme_transport_id *trid,
			  bool peer)
{
	struct sockaddr *saddr;
	uint16_t port;

	if (peer) {
		saddr = rdma_get_peer_addr(id);
		port = ntohs(rdma_get_dst_port(id));
	} else {
		saddr = rdma_get_local_addr(id);
		port = ntohs(rdma_get_src_port(id));
	}

	return nvmf_rdma_trid_from_sockaddr(saddr, port, trid);
}

static int
nvmf_rdma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_common_qpair	*cqpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_offload_qpair	*oqpair;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);

	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		rqpair = nvmf_rdma_qpair_get(qpair);
		return nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, true);
	} else if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD) {
		oqpair = nvmf_offload_qpair_get(qpair);
		return nvmf_rdma_trid_from_sockaddr((struct sockaddr *)&oqpair->peer_addr, oqpair->peer_port, trid);
	}

	SPDK_ERRLOG("Unknown qpair type %d\n", cqpair->type);
	return -1;
}

static int
nvmf_rdma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_common_qpair	*cqpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_offload_qpair	*oqpair;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);

	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		rqpair = nvmf_rdma_qpair_get(qpair);
		return nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, false);
	} else if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD) {
		oqpair = nvmf_offload_qpair_get(qpair);
		return nvmf_rdma_trid_from_sockaddr((struct sockaddr *)&oqpair->local_addr, oqpair->local_port,
						    trid);
	}

	SPDK_ERRLOG("Unknown qpair type %d\n", cqpair->type);
	return -1;
}

static int
nvmf_rdma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_common_qpair	*cqpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_offload_qpair	*oqpair;
	struct rdma_cm_id		*listen_id;

	cqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_common_qpair, qpair);

	if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_RDMA) {
		rqpair = nvmf_rdma_qpair_get(qpair);
		listen_id = rqpair->listen_id;
	} else if (cqpair->type == SPDK_NVMF_COMMON_QPAIR_OFFLOAD) {
		oqpair = nvmf_offload_qpair_get(qpair);
		listen_id = oqpair->listen_id;
	} else {
		SPDK_ERRLOG("Unknown qpair type %d\n", cqpair->type);
		return -EINVAL;
	}

	return nvmf_rdma_trid_from_cm_id(listen_id, trid, false);
}

static void
nvmf_rdma_request_set_abort_status(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_rdma_request *rdma_req_to_abort,
				   struct spdk_nvmf_rdma_qpair *rqpair)
{
	rdma_req_to_abort->common.req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	rdma_req_to_abort->common.req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;

	STAILQ_INSERT_TAIL(&rqpair->pending_rdma_send_queue, rdma_req_to_abort, state_link);
	rdma_req_to_abort->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;

	req->rsp->nvme_cpl.cdw0 &= ~1U;	/* Command was successfully aborted. */
}

static int
_nvmf_rdma_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = nvmf_rdma_request_get(req->req_to_abort);
	struct spdk_nvmf_rdma_qpair *rqpair = nvmf_rdma_qpair_get(req->req_to_abort->qpair);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (rdma_req_to_abort->state) {
	case RDMA_REQUEST_STATE_EXECUTING:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case RDMA_REQUEST_STATE_NEED_BUFFER:
		STAILQ_REMOVE(&rqpair->poller->group->group.pending_buf_queue,
			      &rdma_req_to_abort->common.req, spdk_nvmf_request, buf_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
		STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
		STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
		/* Remove req from the list here to re-use common function */
		STAILQ_REMOVE(&rqpair->pending_rdma_send_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_rdma_qpair_abort_request, req, 0);
			return SPDK_POLLER_BUSY;
		}
		break;

	default:
		break;
	}

	spdk_nvmf_request_complete(req);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_rdma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_qpair *rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i, max_req_count;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = NULL, *rdma_req;

	rqpair = nvmf_rdma_qpair_get(qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	transport = &rtransport->transport;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	max_req_count = rqpair->srq == NULL ? rqpair->max_queue_depth : rqpair->poller->max_srq_depth;

	for (i = 0; i < max_req_count; i++) {
		rdma_req = &rqpair->resources->reqs[i];
		/* When SRQ == NULL, rqpair has its own requests and req.qpair pointer always points to the qpair
		 * When SRQ != NULL all rqpairs share common requests and qpair pointer is assigned when we start to
		 * process a request. So in both cases all requests which are not in FREE state have valid qpair ptr */
		if (rdma_req->state != RDMA_REQUEST_STATE_FREE && rdma_req->common.req.cmd->nvme_cmd.cid == cid &&
		    rdma_req->common.req.qpair == qpair) {
			rdma_req_to_abort = rdma_req;
			break;
		}
	}

	if (rdma_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &rdma_req_to_abort->common.req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_rdma_qpair_abort_request(req);
}

static void
nvmf_rdma_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller *rpoller;

	assert(w != NULL);

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	spdk_json_write_named_uint64(w, "pending_data_buffer", rgroup->stat.pending_data_buffer);

	spdk_json_write_named_array_begin(w, "devices");

	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name",
					     ibv_get_device_name(rpoller->device->context->device));
		spdk_json_write_named_uint64(w, "polls",
					     rpoller->stat.polls);
		spdk_json_write_named_uint64(w, "idle_polls",
					     rpoller->stat.idle_polls);
		spdk_json_write_named_uint64(w, "completions",
					     rpoller->stat.completions);
		spdk_json_write_named_uint64(w, "requests",
					     rpoller->stat.requests);
		spdk_json_write_named_uint64(w, "request_latency",
					     rpoller->stat.request_latency);
		spdk_json_write_named_uint64(w, "pending_free_request",
					     rpoller->stat.pending_free_request);
		spdk_json_write_named_uint64(w, "pending_rdma_read",
					     rpoller->stat.pending_rdma_read);
		spdk_json_write_named_uint64(w, "pending_rdma_write",
					     rpoller->stat.pending_rdma_write);
		spdk_json_write_named_uint64(w, "pending_rdma_send",
					     rpoller->stat.pending_rdma_send);
		spdk_json_write_named_uint64(w, "total_send_wrs",
					     rpoller->stat.qp_stats.send.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "send_doorbell_updates",
					     rpoller->stat.qp_stats.send.doorbell_updates);
		spdk_json_write_named_uint64(w, "total_recv_wrs",
					     rpoller->stat.qp_stats.recv.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "recv_doorbell_updates",
					     rpoller->stat.qp_stats.recv.doorbell_updates);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

static int
nvmf_rdma_subsystem_destroy(struct spdk_nvmf_rdma_subsystem *rsubsystem)
{
	struct spdk_nvmf_rdma_transport *rtransport = rsubsystem->rtransport;
	doca_error_t drc;

	if (!TAILQ_EMPTY(&rsubsystem->namespaces)) {
		SPDK_WARNLOG("Namespace list is not empty\n");
	}

	if (rsubsystem->handle) {
		drc = doca_sta_subsystem_destroy(rsubsystem->handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy DOCA STA subsystem %p\n", rsubsystem->handle);
			return -EINVAL;
		}
	}
	TAILQ_REMOVE(&rtransport->subsystems, rsubsystem, link);
	free(rsubsystem);
	return 0;
}

static struct spdk_nvmf_rdma_subsystem *
nvmf_rdma_subsystem_create(struct spdk_nvmf_rdma_transport *rtransport,
			   const struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_rdma_subsystem *rsubsystem;
	struct spdk_nvmf_rdma_device *rdevice;
	doca_error_t drc;

	rsubsystem = calloc(1, sizeof(*rsubsystem));
	if (!rsubsystem) {
		SPDK_ERRLOG("Cannot allocate memory for DOCA STA subsystem context\n");
		return NULL;
	}

	TAILQ_INIT(&rsubsystem->namespaces);
	rsubsystem->subsystem = subsystem;
	rsubsystem->rtransport = rtransport;

	drc = doca_sta_subsystem_create(rtransport->sta.sta, spdk_nvmf_subsystem_get_nqn(subsystem),
					&rsubsystem->handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to create DOCA STA subsystem for nqn %s\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem));
		nvmf_rdma_subsystem_destroy(rsubsystem);
		return NULL;
	}

	TAILQ_FOREACH(rdevice, &rtransport->devices, link) {
		drc = doca_sta_subsystem_add_dev(rsubsystem->handle, rdevice->doca_dev);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to add %s to subsystem %s\n",
				    ibv_get_device_name(rdevice->context->device),
				    spdk_nvmf_subsystem_get_nqn(subsystem));
			nvmf_rdma_subsystem_destroy(rsubsystem);
			return NULL;
		}
	}
	SPDK_NOTICELOG("Create DOCA STA subsystem %p for nqn %s\n",
		       rsubsystem->handle, spdk_nvmf_subsystem_get_nqn(subsystem));
	TAILQ_INSERT_TAIL(&rtransport->subsystems, rsubsystem, link);

	return rsubsystem;
}

static inline bool
nvmf_rdma_subsystem_is_busy(struct spdk_nvmf_rdma_subsystem *rsubsystem)
{
	return !TAILQ_EMPTY(&rsubsystem->namespaces);
}

static int
nvmf_rdma_listen_associate(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvmf_subsystem *subsystem,
			   const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_port *port;
	struct spdk_nvmf_rdma_subsystem *rsubsystem;

	/* Skip discovery subsystems */
	if (subsystem->subtype != SPDK_NVMF_SUBTYPE_NVME) {
		return 0;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
			break;
		}
	}

	if (!port) {
		SPDK_ERRLOG("Port is not found for trid\n");
		return -EINVAL;
	}

	rsubsystem = nvmf_rdma_subsystem_find(rtransport, subsystem);
	if (!rsubsystem) {
		rsubsystem = nvmf_rdma_subsystem_create(rtransport, subsystem);
	}
	if (!rsubsystem) {
		SPDK_ERRLOG("Cannot get subsystem\n");
		return -EINVAL;
	}

	return 0;
}

static int
nvmf_sta_bdev_queue_destroy(struct spdk_nvmf_rdma_sta *sta,
			    struct doca_sta_be_q_handle *handle,
			    struct spdk_nvmf_rdma_bdev_queue_destroy_ctx *destroy_ctx)
{
	struct doca_sta_producer_task_send *destroy_task;
	struct doca_task *doca_task;
	union doca_data task_user_data;
	doca_error_t drc;

	memset(destroy_ctx, 0, sizeof(*destroy_ctx));
	task_user_data.ptr = destroy_ctx;

	drc = doca_sta_be_destroy_queue_task_alloc_init(handle, task_user_data, &destroy_task);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to alloc destroy task: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	doca_task = doca_sta_producer_send_task_as_task(destroy_task);
	if (!doca_task) {
		SPDK_ERRLOG("Failed to get doca_task\n");
		return -1;
	}

	drc = submit_doca_task(doca_task);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to submit be destroy queue task: %s\n", doca_error_get_descr(drc));
		doca_task_free(doca_task);
		return -1;
	}

	while (!destroy_ctx->destroy_completed) {
		doca_pe_progress(sta->pe);
	}

	if (destroy_ctx->destroy_failed) {
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_bdev_nvme_queue_destroy(struct spdk_nvmf_rdma_sta *sta,
				  struct spdk_nvmf_rdma_bdev_nvme_queue *queue)
{
	doca_error_t drc;
	int rc;

	if (queue->handle) {
		rc = nvmf_sta_bdev_queue_destroy(sta, queue->handle, &queue->destroy_ctx);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy nvme backend queue\n");
			return -1;
		}
		SPDK_NOTICELOG("Destroy DOCA STA nvme backend queue %p\n", queue->handle);
		queue->handle = 0;
	}

	if (queue->sq_mmap) {
		drc = doca_mmap_destroy(queue->sq_mmap);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy SQ doca_mmap: %s\n", doca_error_get_descr(drc));
			return -1;
		}
		queue->sq_mmap = NULL;
	}

	if (queue->cq_mmap) {
		drc = doca_mmap_destroy(queue->cq_mmap);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy CQ doca_mmap: %s\n", doca_error_get_descr(drc));
			return -1;
		}
		queue->cq_mmap = NULL;
	}

	if (queue->nvme_qpair) {
		/* Prevent polling completions because the offload handles them. */
		queue->nvme_qpair->no_deletion_notification_needed = 1;
		queue->nvme_qpair->no_process_completions = 1;

		spdk_nvme_ctrlr_free_io_qpair(queue->nvme_qpair);
		queue->nvme_qpair = NULL;
	}

	return 0;
}

static int
nvmf_rdma_bdev_nvme_queue_init(struct spdk_nvmf_rdma_sta *sta,
			       struct spdk_nvmf_rdma_bdev *rbdev,
			       struct spdk_nvmf_rdma_bdev_nvme_queue *queue)
{
	const struct spdk_nvme_transport_id *trid;
	struct spdk_nvme_io_qpair_opts opts;
	union spdk_nvme_cap_register nvme_ctrlr_cap;
	uint32_t max_io_queue_size;
	struct nvme_pcie_qpair *nvme_pqpair;
	size_t sq_db_mmap_offset;
	size_t cq_db_mmap_offset;
	doca_error_t drc;
	int rc;

	trid = spdk_nvme_ctrlr_get_transport_id(rbdev->nvme.ctrlr);
	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		return -EINVAL;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(rbdev->nvme.ctrlr, &opts, sizeof(opts));
	opts.create_only = true;
	nvme_ctrlr_cap = spdk_nvme_ctrlr_get_regs_cap(rbdev->nvme.ctrlr);
	/* mqes is a 0's based value, but max_io_queue_size isn't */
	max_io_queue_size = nvme_ctrlr_cap.bits.mqes + 1;
	/* DOCA STA requires aligned IO queue size */
	opts.io_queue_size = spdk_align32pow2(max_io_queue_size);
	if (opts.io_queue_size > max_io_queue_size) {
		opts.io_queue_size /= 2;
	}

	queue->nvme_qpair = spdk_nvme_ctrlr_alloc_io_qpair(rbdev->nvme.ctrlr, &opts, sizeof(opts));
	if (!queue->nvme_qpair) {
		SPDK_ERRLOG("Failed to allocate nvme IO qpair\n");
		nvmf_rdma_bdev_nvme_queue_destroy(sta, queue);
		return -1;
	}

	rc = spdk_nvme_ctrlr_connect_io_qpair(rbdev->nvme.ctrlr, queue->nvme_qpair);
	if (rc) {
		SPDK_ERRLOG("Failed to connect nvme IO qpair, rc %d\n", rc);
		nvmf_rdma_bdev_nvme_queue_destroy(sta, queue);
		return rc;
	}

	nvme_pqpair = nvme_pcie_qpair(queue->nvme_qpair);
	SPDK_DEBUGLOG(rdma_offload,
		      "PCIe qpair: sqdb %p, cqdb %p, sq %p, cq %p, num_entries %u"
		      ", sq_bus_addr %p, cq_bus_addr %p\n",
		      nvme_pqpair->sq_tdbl, nvme_pqpair->cq_hdbl, nvme_pqpair->cmd, nvme_pqpair->cpl,
		      nvme_pqpair->num_entries, (void *)nvme_pqpair->cmd_bus_addr, (void *)nvme_pqpair->cpl_bus_addr);

	queue->sq_mmap = nvmf_rdma_create_doca_mmap(sta->dev,
			 nvme_pqpair->cmd,
			 nvme_pqpair->num_entries * sizeof(struct spdk_nvme_cmd),
			 -1, 0);
	if (!queue->sq_mmap) {
		SPDK_ERRLOG("Failed to create SQ mmap\n");
		nvmf_rdma_bdev_nvme_queue_destroy(sta, queue);
		return -1;
	}

	queue->cq_mmap = nvmf_rdma_create_doca_mmap(sta->dev,
			 nvme_pqpair->cpl,
			 nvme_pqpair->num_entries * sizeof(struct spdk_nvme_cpl),
			 -1, 0);
	if (!queue->cq_mmap) {
		SPDK_ERRLOG("Failed to create CQ mmap\n");
		nvmf_rdma_bdev_nvme_queue_destroy(sta, queue);
		return -1;
	}

	sq_db_mmap_offset = (uintptr_t)nvme_pqpair->sq_tdbl - (uintptr_t)rbdev->nvme.bar0_dmabuf->addr;
	cq_db_mmap_offset = (uintptr_t)nvme_pqpair->cq_hdbl - (uintptr_t)rbdev->nvme.bar0_dmabuf->addr;
	drc = doca_sta_be_add_queue(rbdev->handle, queue->sq_mmap, rbdev->nvme.bar0_mmap, sq_db_mmap_offset,
				    queue->cq_mmap, rbdev->nvme.bar0_mmap, cq_db_mmap_offset, &queue->handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to add queue to doca_sta_be: %s\n", doca_error_get_descr(drc));
		nvmf_rdma_bdev_nvme_queue_destroy(sta, queue);
		return -1;
	}

	SPDK_DEBUGLOG(rdma_offload, "Add DOCA STA nvme backend queue %p to backend %p\n", queue->handle,
		      rbdev->handle);
	return 0;
}

static int
nvmf_rdma_bdev_null_queue_destroy(struct spdk_nvmf_rdma_sta *sta,
				  struct spdk_nvmf_rdma_bdev_null_queue *queue)
{
	int rc;
	doca_error_t drc;

	if (queue->handle) {
		rc = nvmf_sta_bdev_queue_destroy(sta, queue->handle, &queue->destroy_ctx);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy null backend queue\n");
			return -1;
		}
		SPDK_NOTICELOG("Destroy DOCA STA null backend queue %p\n", queue->handle);
		queue->handle = 0;
	}
	if (queue->sq_mmap) {
		drc = doca_mmap_destroy(queue->sq_mmap);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy SQ doca_mmap: %s\n", doca_error_get_descr(drc));
			return -1;
		}
		queue->sq_mmap = NULL;
	}

	if (queue->cq_mmap) {
		drc = doca_mmap_destroy(queue->cq_mmap);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy CQ doca_mmap: %s\n", doca_error_get_descr(drc));
			return -1;
		}
		queue->cq_mmap = NULL;
	}

	if (queue->db_mmap) {
		drc = doca_mmap_destroy(queue->db_mmap);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy SQDB doca_mmap: %s\n", doca_error_get_descr(drc));
			return -1;
		}
		queue->db_mmap = NULL;
	}

	spdk_free(queue->sq);
	spdk_free(queue->cq);
	spdk_free(queue->sqdb);
	queue->sq = NULL;
	queue->cq = NULL;
	queue->sqdb = NULL;

	return 0;
}

static int
nvmf_rdma_bdev_null_queue_init(struct spdk_nvmf_rdma_sta *sta,
			       struct spdk_nvmf_rdma_bdev *bdev,
			       struct spdk_nvmf_rdma_bdev_null_queue *queue)
{
	const size_t queue_size = 4096;
	const size_t db_buf_size = 64;
	const size_t db_buf_align = 64;
	doca_error_t drc;

	queue->sq = spdk_zmalloc(queue_size * sizeof(struct spdk_nvme_cmd),
				 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!queue->sq) {
		SPDK_ERRLOG("Failed to allocate null SQ\n");
		nvmf_rdma_bdev_null_queue_destroy(sta, queue);
		return -ENOMEM;
	}

	queue->cq = spdk_zmalloc(queue_size * sizeof(struct spdk_nvme_cmd),
				 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!queue->cq) {
		SPDK_ERRLOG("Failed to allocate null CQ\n");
		nvmf_rdma_bdev_null_queue_destroy(sta, queue);
		return -ENOMEM;
	}

	queue->sqdb = (uint32_t *)spdk_zmalloc(db_buf_size, db_buf_align, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	if (!queue->sqdb) {
		SPDK_ERRLOG("Failed to allocate null doorbells\n");
		return -ENOMEM;
	}

	queue->cqdb = queue->sqdb + 1;

	queue->sq_mmap = nvmf_rdma_create_doca_mmap(sta->dev, queue->sq,
			 queue_size * sizeof(struct spdk_nvme_cmd), -1, 0);
	if (!queue->sq_mmap) {
		SPDK_ERRLOG("Failed to create SQ mmap\n");
		nvmf_rdma_bdev_null_queue_destroy(sta, queue);
		return -1;
	}

	queue->cq_mmap = nvmf_rdma_create_doca_mmap(sta->dev, queue->cq,
			 queue_size * sizeof(struct spdk_nvme_cpl), -1, 0);
	if (!queue->cq_mmap) {
		SPDK_ERRLOG("Failed to create CQ mmap\n");
		nvmf_rdma_bdev_null_queue_destroy(sta, queue);
		return -1;
	}

	queue->db_mmap = nvmf_rdma_create_doca_mmap(sta->dev, queue->sqdb,
			 db_buf_size, -1, 0);
	if (!queue->db_mmap) {
		SPDK_ERRLOG("Failed to create SQDB mmap\n");
		nvmf_rdma_bdev_null_queue_destroy(sta, queue);
		return -1;
	}

	drc = doca_sta_be_add_queue(bdev->handle, queue->sq_mmap, queue->db_mmap, 0, queue->cq_mmap,
				    queue->db_mmap, sizeof(*queue->sqdb), &queue->handle);
	if (drc) {
		SPDK_ERRLOG("Failed to add queue to doca_sta_be: %s\n", doca_error_get_descr(drc));
		nvmf_rdma_bdev_null_queue_destroy(sta, queue);
		return -1;
	}

	SPDK_NOTICELOG("Add DOCA STA null queue %p to backend %p\n", queue->handle, bdev->handle);
	return 0;
}

static int
nvmf_rdma_bdev_destroy(struct spdk_nvmf_rdma_bdev *rbdev)
{
	uint32_t i;
	doca_error_t drc;
	int rc;

	if (rbdev->type == SPDK_NVMF_RDMA_BDEV_TYPE_NVME) {
		if (rbdev->nvme.queues) {
			for (i = 0; i < rbdev->num_queues; i++) {
				rc = nvmf_rdma_bdev_nvme_queue_destroy(rbdev->sta, &rbdev->nvme.queues[i]);
				if (rc) {
					SPDK_ERRLOG("Failed to destroy nvme backend queue %d\n", i);
					return rc;
				}
			}
			free(rbdev->nvme.queues);
			rbdev->nvme.queues = NULL;
		}
		if (rbdev->nvme.bar0_mmap) {
			drc = doca_mmap_destroy(rbdev->nvme.bar0_mmap);
			if (DOCA_IS_ERROR(drc)) {
				SPDK_ERRLOG("Failed to destroy doca_mmap for NVMe BAR 0: %s\n", doca_error_get_descr(drc));
				return -1;
			}
			rbdev->nvme.bar0_mmap = NULL;
		}

		if (rbdev->nvme.bar0_dmabuf) {
			spdk_pci_device_destroy_dmabuf(rbdev->nvme.pci_dev, rbdev->nvme.bar0_dmabuf);
			rbdev->nvme.bar0_dmabuf = NULL;
		}
	} else {
		if (rbdev->null.queues) {
			for (i = 0; i < rbdev->num_queues; i++) {
				rc = nvmf_rdma_bdev_null_queue_destroy(rbdev->sta, &rbdev->null.queues[i]);
				if (rc) {
					SPDK_ERRLOG("Failed to destroy null backend queue %d\n", i);
					return rc;
				}
			}
			free(rbdev->null.queues);
			rbdev->null.queues = NULL;
		}
	}

	if (rbdev->handle) {
		drc = doca_sta_be_destroy(rbdev->handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to destroy doca_sta_be %p: %s\n", rbdev->handle,
				    doca_error_get_descr(drc));
			return -EINVAL;
		}
	}

	if (rbdev->name) {
		free(rbdev->name);
		rbdev->name = NULL;
	}
	free(rbdev);

	return 0;
}

static struct spdk_nvmf_rdma_bdev *
nvmf_rdma_bdev_create(struct spdk_nvmf_rdma_transport *rtransport,
		      char *rbdev_name,
		      struct spdk_nvme_ctrlr *nvme_ctrlr,
		      struct spdk_bdev *bdev)
{
	struct spdk_nvmf_rdma_bdev *rbdev;
	uint32_t i;
	doca_error_t drc;
	int rc;

	rbdev = calloc(1, sizeof(*rbdev));
	if (!rbdev) {
		SPDK_ERRLOG("Cannot allocate memory for backend device context\n");
		return NULL;
	}
	rbdev->sta = &rtransport->sta;
	rbdev->name = strdup(rbdev_name);
	if (!rbdev->name) {
		SPDK_ERRLOG("Failed to allocate memory for device name\n");
		nvmf_rdma_bdev_destroy(rbdev);
		return NULL;
	}

	drc = doca_sta_be_create(rtransport->sta.sta, &rbdev->handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to create doca_sta_be: %s\n", doca_error_get_descr(drc));
		nvmf_rdma_bdev_destroy(rbdev);
		return NULL;
	}
	SPDK_DEBUGLOG(rdma_offload, "Created DOCA STA backend %p, name %s, handle %p\n",
		      rbdev, rbdev->name, rbdev->handle);

	if (nvme_ctrlr) {
		rbdev->type = SPDK_NVMF_RDMA_BDEV_TYPE_NVME;
		rbdev->nvme.ctrlr = nvme_ctrlr;
		rbdev->num_queues = spdk_min(nvme_ctrlr->opts.max_p2p_io_queues,
					     rtransport->sta.caps.max_qs_per_be);

		if (rbdev->num_queues == 0) {
			SPDK_ERRLOG("The number of P2P IO queues cannot be zero\n");
			nvmf_rdma_bdev_destroy(rbdev);
			return NULL;
		}
		if (rbdev->num_queues < nvme_ctrlr->opts.max_p2p_io_queues) {
			SPDK_NOTICELOG("The number of P2P IO queues is reduced from %u to %u\n",
				       nvme_ctrlr->opts.max_p2p_io_queues, rbdev->num_queues);
		}
		rbdev->nvme.pci_dev = spdk_nvme_ctrlr_get_pci_device(rbdev->nvme.ctrlr);
		if (!rbdev->nvme.pci_dev) {
			SPDK_ERRLOG("Failed to get PCI device for NVMe controller\n");
			nvmf_rdma_bdev_destroy(rbdev);
			return NULL;
		}

		rbdev->nvme.bar0_dmabuf = spdk_pci_device_create_dmabuf(rbdev->nvme.pci_dev, 0);
		if (!rbdev->nvme.bar0_dmabuf) {
			SPDK_ERRLOG("Failed to get DMABUF for BAR 0\n");
			nvmf_rdma_bdev_destroy(rbdev);
			return NULL;
		}
		SPDK_DEBUGLOG(rdma_offload, "Nvme %s BAR0 dmabuf: addr %p, len %lu, fd %d\n", rbdev->name,
			      rbdev->nvme.bar0_dmabuf->addr, rbdev->nvme.bar0_dmabuf->length,
			      rbdev->nvme.bar0_dmabuf->fd);

		rbdev->nvme.bar0_mmap = nvmf_rdma_create_doca_mmap(rtransport->sta.dev,
					rbdev->nvme.bar0_dmabuf->addr,
					rbdev->nvme.bar0_dmabuf->length,
					rbdev->nvme.bar0_dmabuf->fd, 0);
		if (!rbdev->nvme.bar0_mmap) {
			SPDK_ERRLOG("Failed to create doca_mmap for NVMe BAR 0\n");
			nvmf_rdma_bdev_destroy(rbdev);
			return NULL;
		}

		rbdev->nvme.queues = calloc(rbdev->num_queues, sizeof(struct spdk_nvmf_rdma_bdev_nvme_queue));
		if (!rbdev->nvme.queues) {
			SPDK_ERRLOG("Cannot allocate memory for nvme be queues\n");
			nvmf_rdma_bdev_destroy(rbdev);
			return NULL;
		}

		for (i = 0; i < rbdev->num_queues; i++) {
			rc = nvmf_rdma_bdev_nvme_queue_init(&rtransport->sta, rbdev, &rbdev->nvme.queues[i]);
			if (rc) {
				SPDK_ERRLOG("Failed to create nvme offload backend for bdev %s, rc %d\n",
					    spdk_bdev_get_name(bdev), rc);
				nvmf_rdma_bdev_destroy(rbdev);
				return NULL;
			}
		}
	} else {
		rbdev->type = SPDK_NVMF_RDMA_BDEV_TYPE_NULL;
		rbdev->null.ns_id = 1;
		rbdev->num_queues = 1;

		rbdev->null.queues = calloc(rbdev->num_queues, sizeof(struct spdk_nvmf_rdma_bdev_null_queue));
		if (!rbdev->null.queues) {
			SPDK_ERRLOG("Cannot allocate memory for null be queues\n");
			nvmf_rdma_bdev_destroy(rbdev);
			return NULL;
		}

		for (i = 0; i < rbdev->num_queues; i++) {
			rc = nvmf_rdma_bdev_null_queue_init(&rtransport->sta, rbdev, &rbdev->null.queues[i]);
			if (rc) {
				SPDK_ERRLOG("Failed to create nvme offload backend for bdev %s, rc %d\n",
					    spdk_bdev_get_name(bdev), rc);
				nvmf_rdma_bdev_destroy(rbdev);
				return NULL;
			}
		}
	}

	return rbdev;
}

static char *
get_be_dev_name(struct spdk_bdev *bdev)
{
	const char *module_name = spdk_bdev_get_module_name(bdev);
	const char *bdev_name;
	const char *tmp;
	char *be_name;

	assert(bdev);

	module_name = spdk_bdev_get_module_name(bdev);
	bdev_name = spdk_bdev_get_name(bdev);

	if (strcmp(module_name, "nvme") == 0) {
		/* The NVMe namespace name has the following format: <ctrlr_name>n<namespace_id> */
		tmp = strrchr(bdev_name, 'n');
		if (!tmp) {
			SPDK_ERRLOG("Wrong NVMe namespace name format\n");
			return NULL;
		}
		be_name = strndup(bdev_name, tmp - bdev_name);
	} else if (strcmp(module_name, "null") == 0) {
		be_name = strdup(bdev_name);
	} else {
		SPDK_ERRLOG("bdev module %s is unsupported\n", module_name);
		return NULL;
	}

	return be_name;
}

static struct spdk_nvmf_rdma_bdev *
nvmf_rdma_find_bdev(struct spdk_nvmf_rdma_transport *rtransport,
		    const char *name)
{
	struct spdk_nvmf_rdma_bdev *rbdev;

	TAILQ_FOREACH(rbdev, &rtransport->bdevs, link) {
		if (strcmp(name, rbdev->name) == 0) {
			break;
		}
	}

	return rbdev;
}

static struct spdk_nvmf_rdma_bdev *
nvmf_rdma_add_bdev(struct spdk_nvmf_rdma_transport *rtransport,
		   struct spdk_nvme_ctrlr *nvme_ctrlr,
		   struct spdk_bdev *bdev)
{
	struct spdk_nvmf_rdma_bdev *rbdev;
	char *be_name;

	be_name = get_be_dev_name(bdev);
	if (!be_name) {
		SPDK_ERRLOG("Failed to get backend device name for bdev %s\n", spdk_bdev_get_name(bdev));
		return NULL;
	}

	rbdev = nvmf_rdma_find_bdev(rtransport, be_name);
	if (rbdev) {
		rbdev->refs++;
		free(be_name);
		return rbdev;
	}

	rbdev = nvmf_rdma_bdev_create(rtransport, be_name, nvme_ctrlr, bdev);
	if (!rbdev) {
		SPDK_ERRLOG("Failed to create backend dev\n");
	} else {
		rbdev->refs = 1;
		TAILQ_INSERT_TAIL(&rtransport->bdevs, rbdev, link);
	}

	free(be_name);
	return rbdev;
}

static int
nvmf_rdma_rm_bdev(struct spdk_nvmf_rdma_transport *rtransport, struct spdk_nvmf_rdma_bdev *rbdev)
{
	int rc;

	rbdev->refs--;
	if (rbdev->refs != 0) {
		return 0;
	}

	TAILQ_REMOVE(&rtransport->bdevs, rbdev, link);
	rc = nvmf_rdma_bdev_destroy(rbdev);
	if (rc) {
		TAILQ_INSERT_TAIL(&rtransport->bdevs, rbdev, link);
	}

	return rc;
}

static int
spdk_nvmf_rdma_ns_destroy(struct spdk_nvmf_rdma_ns *rns)
{
	union doca_data task_user_data;
	int rc;
	doca_error_t drc;

	if (rns->handle) {
		rns->delete_started = true;
		task_user_data.ptr = rns;

		drc = doca_sta_subsystem_task_rm_ns_alloc_init(rns->rsubsystem->handle,
				rns->handle, task_user_data, &rns->delete_task);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to alloc DOCA task: %s\n", doca_error_get_descr(drc));
			rns->delete_started = false;
			return -1;
		}
		drc = submit_doca_task(doca_sta_producer_send_task_as_task(rns->delete_task));
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to submit DOCA task: %s\n", doca_error_get_descr(drc));
			rns->delete_started = false;
			return -1;
		}

		while (!rns->delete_completed) {
			doca_pe_progress(rns->rsubsystem->rtransport->sta.pe);
		}
		if (rns->delete_failed) {
			return -1;
		}
		rns->handle = 0;
	}

	if (rns->rbdev) {
		rc = nvmf_rdma_rm_bdev(rns->rsubsystem->rtransport, rns->rbdev);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy backend device\n");
			return rc;
		}
		rns->rbdev = NULL;
	}

	free(rns);
	return 0;
}

static struct spdk_nvmf_rdma_ns *
spdk_nvmf_rdma_ns_create(struct spdk_nvmf_rdma_subsystem *rsubsystem,
			 struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_rdma_transport *rtransport = rsubsystem->rtransport;
	struct spdk_bdev *bdev = ns->bdev;
	const char *module_name = spdk_bdev_get_module_name(bdev);
	struct spdk_nvmf_rdma_ns *rns;
	doca_error_t drc;

	rns = calloc(1, sizeof(*rns));
	if (!rns) {
		SPDK_ERRLOG("Cannot allocate memory for namespace\n");
		return NULL;
	}
	rns->ns = ns;
	rns->fe_ns_id = ns->nsid;
	rns->rsubsystem = rsubsystem;

	SPDK_NOTICELOG("NVMf namespace %u, bdev %s, module %s\n",
		       ns->nsid, spdk_bdev_get_name(bdev), module_name);
	if (strcmp(module_name, "nvme") == 0) {
		struct spdk_nvme_ns *nvme_ns = spdk_bdev_get_module_ctx(ns->desc);

		rns->be_ns_id = spdk_nvme_ns_get_id(nvme_ns);
		rns->rbdev = nvmf_rdma_add_bdev(rtransport, spdk_nvme_ns_get_ctrlr(nvme_ns), bdev);
		if (!rns->rbdev) {
			spdk_nvmf_rdma_ns_destroy(rns);
			return NULL;
		}
	} else if (strcmp(module_name, "null") == 0) {
		rns->rbdev = nvmf_rdma_add_bdev(rtransport, NULL, bdev);
		if (!rns->rbdev) {
			spdk_nvmf_rdma_ns_destroy(rns);
			return NULL;
		}
		rns->be_ns_id = rns->rbdev->null.ns_id++;
	} else {
		SPDK_ERRLOG("%s is unsupported\n", module_name);
		spdk_nvmf_rdma_ns_destroy(rns);
		return NULL;
	}

	drc = doca_sta_subsystem_add_ns(rsubsystem->handle,
					rns->fe_ns_id,
					spdk_bdev_get_block_size(ns->bdev),
					rns->be_ns_id,
					rns->rbdev->handle,
					&rns->handle);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to add namespace to DOCA STA subsystem: %s\n", doca_error_get_descr(drc));
		spdk_nvmf_rdma_ns_destroy(rns);
		return NULL;
	}

	return rns;
}

static int
nvmf_rdma_subsystem_add_ns(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvmf_subsystem *subsystem,
			   struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_subsystem *rsubsystem;
	struct spdk_nvmf_rdma_ns *rns;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rsubsystem = nvmf_rdma_subsystem_find(rtransport, subsystem);
	if (!rsubsystem) {
		rsubsystem = nvmf_rdma_subsystem_create(rtransport, subsystem);
	}
	if (!rsubsystem) {
		SPDK_ERRLOG("Cannot get subsystem\n");
		return -EINVAL;
	}

	rns = spdk_nvmf_rdma_ns_create(rsubsystem, ns);
	if (!rns) {
		SPDK_ERRLOG("Failed to create namespace\n");
		return -EINVAL;
	}
	TAILQ_INSERT_TAIL(&rsubsystem->namespaces, rns, link);

	return 0;

}

static void
nvmf_rdma_subsystem_remove_ns(struct spdk_nvmf_transport *transport,
			      const struct spdk_nvmf_subsystem *subsystem,
			      uint32_t nsid)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_subsystem *rsubsystem;
	struct spdk_nvmf_rdma_ns *rns;
	int rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rsubsystem = nvmf_rdma_subsystem_find(rtransport, subsystem);
	if (!rsubsystem) {
		SPDK_ERRLOG("Subsystem for nqn %s does not exist\n", spdk_nvmf_subsystem_get_nqn(subsystem));
		return;
	}

	TAILQ_FOREACH(rns, &rsubsystem->namespaces, link) {
		if (rns->fe_ns_id == nsid) {
			break;
		}
	}

	if (!rns) {
		SPDK_ERRLOG("Namespace %u does not exist\n", nsid);
		return;
	}

	TAILQ_REMOVE(&rsubsystem->namespaces, rns, link);
	rc = spdk_nvmf_rdma_ns_destroy(rns);
	if (rc) {
		SPDK_ERRLOG("Failed to destroy namespace %u\n", nsid);
	}

	if (!nvmf_rdma_subsystem_is_busy(rsubsystem)) {
		rc = nvmf_rdma_subsystem_destroy(rsubsystem);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy subsystem %s\n", spdk_nvmf_subsystem_get_nqn(subsystem));
		}
	}

	SPDK_NOTICELOG("Remove namespace %u from %s\n", nsid, spdk_nvmf_subsystem_get_nqn(subsystem));
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma_offload = {
	.name = "RDMA_OFFLOAD",
	.type = SPDK_NVME_TRANSPORT_CUSTOM_FABRICS,
	.opts_init = nvmf_rdma_opts_init,
	.create = nvmf_rdma_create,
	.dump_opts = nvmf_rdma_dump_opts,
	.destroy = nvmf_rdma_destroy,

	.listen = nvmf_rdma_listen,
	.stop_listen = nvmf_rdma_stop_listen,
	.cdata_init = nvmf_rdma_cdata_init,

	.listen_associate = nvmf_rdma_listen_associate,
	.subsystem_add_ns = nvmf_rdma_subsystem_add_ns,
	.subsystem_remove_ns = nvmf_rdma_subsystem_remove_ns,

	.listener_discover = nvmf_rdma_discover,

	.poll_group_create = nvmf_rdma_poll_group_create,
	.get_optimal_poll_group = nvmf_rdma_get_optimal_poll_group,
	.poll_group_destroy = nvmf_rdma_poll_group_destroy,
	.poll_group_add = nvmf_rdma_poll_group_add,
	.poll_group_remove = nvmf_rdma_poll_group_remove,
	.poll_group_poll = nvmf_rdma_poll_group_poll,

	.req_free = nvmf_rdma_offload_request_free,
	.req_complete = nvmf_rdma_offload_request_complete,

	.qpair_fini = nvmf_rdma_close_qpair,
	.qpair_get_peer_trid = nvmf_rdma_qpair_get_peer_trid,
	.qpair_get_local_trid = nvmf_rdma_qpair_get_local_trid,
	.qpair_get_listen_trid = nvmf_rdma_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_rdma_qpair_abort_request,

	.poll_group_dump_stat = nvmf_rdma_poll_group_dump_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(rdma_offload, &spdk_nvmf_transport_rdma_offload);
SPDK_LOG_REGISTER_COMPONENT(rdma_offload)

static int
rpc_tgt_ofld_get_rtransport(struct spdk_nvmf_rdma_transport **out)
{
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_rdma_transport *rtransport;

	tgt = spdk_nvmf_get_tgt(NULL);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object\n");
		return -ENODEV;
	}

	transport = spdk_nvmf_tgt_get_transport(tgt, "RDMA_OFFLOAD");
	if (!transport) {
		SPDK_ERRLOG("RDMA_OFFLOAD transport is not found\n");
		return -ENODEV;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	if (rtransport->sta.state != DOCA_CTX_STATE_RUNNING) {
		SPDK_ERRLOG("Invalid sta state - not running\n");
		return -EPERM;
	}

	*out = rtransport;
	return 0;
}

static int
rpc_tgt_ofld_get_sta(struct spdk_nvmf_rdma_sta **sta)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	int rc;

	rc = rpc_tgt_ofld_get_rtransport(&rtransport);
	if (rc) {
		return rc;
	}

	*sta = &rtransport->sta;
	return 0;
}

static doca_error_t
rpc_tgt_ofld_get_eu_handles(struct spdk_nvmf_rdma_sta *sta, struct doca_sta_eu_handle ***handles,
			    uint32_t *num_handles)
{
	struct doca_sta_eu_handle **eu_handles;
	uint32_t num_eu_handles;
	doca_error_t drc;

	num_eu_handles = sta->caps.max_eus;
	eu_handles = calloc(num_eu_handles, sizeof(*eu_handles));
	if (!eu_handles) {
		return DOCA_ERROR_NO_MEMORY;
	}

	drc = doca_sta_get_eu_handle(sta->sta, eu_handles, &num_eu_handles);
	if (DOCA_IS_ERROR(drc)) {
		free(eu_handles);
		return drc;
	}

	*handles = eu_handles;
	*num_handles = num_eu_handles;
	return 0;
}

static int
rpc_tgt_ofld_eu_cmp_type(struct doca_sta_eu_handle *eu_handle, enum dpa_sta_eu_type type)
{
	doca_error_t drc;
	enum dpa_sta_eu_type eu_type;

	assert(type < DOCA_STA_EU_MAX);
	drc = doca_sta_get_eu_type(eu_handle, &eu_type);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get handle type: %s\n", doca_error_get_descr(drc));
		return -1;
	}

	return (type == eu_type) ? 0 : -1;
}

static doca_error_t
rpc_tgt_ofld_eu_find_by_type_id(struct doca_sta_eu_handle **handles, uint32_t num_handles,
				enum dpa_sta_eu_type type, uint16_t id, struct doca_sta_eu_handle **handle)
{
	uint32_t i;
	enum dpa_sta_eu_type eu_type;
	uint16_t eu_id;
	doca_error_t drc;

	for (i = 0; i < num_handles; i++) {
		drc = doca_sta_get_eu_type(handles[i], &eu_type);
		if (DOCA_IS_ERROR(drc)) {
			return drc;
		}
		if (eu_type != type) {
			continue;
		}

		drc = doca_sta_get_eu_id(handles[i], &eu_id);
		if (DOCA_IS_ERROR(drc)) {
			return drc;
		}
		if (eu_id == id) {
			*handle = handles[i];
			return DOCA_SUCCESS;
		}
	}

	return DOCA_ERROR_NOT_FOUND;
}

static doca_error_t
rpc_tgt_ofld_eu_find_by_name(struct doca_sta_eu_handle **handles, uint32_t num_handles,
			     const char *name, struct doca_sta_eu_handle **handle)
{
	uint32_t i;
	const char *eu_name;
	doca_error_t drc;


	for (i = 0; i < num_handles; i++) {
		drc = doca_sta_get_eu_name(handles[i], &eu_name);
		if (DOCA_IS_ERROR(drc)) {
			return drc;
		}
		if (strcmp(eu_name, name) == 0) {
			*handle = handles[i];
			return DOCA_SUCCESS;
		}
	}

	return DOCA_ERROR_NOT_FOUND;
}

struct rpc_tgt_ofld_eu_data {
	const char *name;
	uint16_t eu_id;
	uint16_t port;
};

static doca_error_t
rpc_tgt_ofld_eu_get_data(struct doca_sta_eu_handle *eu_handle, struct rpc_tgt_ofld_eu_data *data)
{
	doca_error_t drc;

	drc = doca_sta_get_eu_name(eu_handle, &data->name);
	if (DOCA_IS_ERROR(drc)) {
		return drc;
	}

	drc = doca_sta_get_eu_id(eu_handle, &data->eu_id);
	if (DOCA_IS_ERROR(drc)) {
		return drc;
	}

	drc = doca_sta_get_eu_port(eu_handle, &data->port);
	if (DOCA_IS_ERROR(drc)) {
		return drc;
	}

	return DOCA_SUCCESS;
}

static void
rpc_tgt_ofld_hdlr_data_dump(struct doca_sta_eu_handle *eu_handle, struct spdk_json_write_ctx *w)
{
	struct rpc_tgt_ofld_eu_data data;
	doca_error_t drc;

	drc = rpc_tgt_ofld_eu_get_data(eu_handle, &data);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get EU handle data: %s\n", doca_error_get_descr(drc));
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "hdlr_name", data.name);
	spdk_json_write_named_uint16(w, "eu_id", data.eu_id);
	spdk_json_write_named_uint16(w, "port", data.port);

	spdk_json_write_object_end(w);
}

static int
rpc_tgt_ofld_decode_eu_type(const struct spdk_json_val *val, void *out)
{
	enum dpa_sta_eu_type *type = out;

	if (spdk_json_strequal(val, "comp") == true) {
		*type = DOCA_STA_EU_COMP;
	} else if (spdk_json_strequal(val, "tx") == true) {
		*type = DOCA_STA_EU_TX;
	} else if (spdk_json_strequal(val, "beq") == true) {
		*type = DOCA_STA_EU_BEQ;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: type\n");
		return -EINVAL;
	}

	return 0;
}

struct rpc_tgt_ofld_hdlr_list_attr {
	enum dpa_sta_eu_type type;
};

static const struct spdk_json_object_decoder rpc_tgt_ofld_rpc_hdlr_list_decoder[] = {
	{"type", offsetof(struct rpc_tgt_ofld_hdlr_list_attr, type), rpc_tgt_ofld_decode_eu_type, true},
};

static void
rpc_tgt_ofld_event_handler_list(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_hdlr_list_attr attr = {
		.type = DOCA_STA_EU_TYPE_UNKNOWN
	};
	struct spdk_nvmf_rdma_sta *sta;
	struct spdk_json_write_ctx *w;
	struct doca_sta_eu_handle **eu_handles;
	uint32_t i, num_eu_handles;
	int rc;
	doca_error_t drc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_rpc_hdlr_list_decoder,
					    SPDK_COUNTOF(rpc_tgt_ofld_rpc_hdlr_list_decoder), &attr)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	rc = rpc_tgt_ofld_get_sta(&sta);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		return;
	}

	drc = rpc_tgt_ofld_get_eu_handles(sta, &eu_handles, &num_eu_handles);
	if (DOCA_IS_ERROR(drc)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 doca_error_get_descr(drc));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	for (i = 0; i < num_eu_handles; ++i) {
		if (attr.type == DOCA_STA_EU_TYPE_UNKNOWN ||
		    !rpc_tgt_ofld_eu_cmp_type(eu_handles[i], attr.type)) {
			rpc_tgt_ofld_hdlr_data_dump(eu_handles[i], w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	free(eu_handles);
}
SPDK_RPC_REGISTER("tgt_ofld_event_handler_list", rpc_tgt_ofld_event_handler_list, SPDK_RPC_RUNTIME)

static void
rpc_tgt_ofld_ctr_entries_dump(struct spdk_json_write_ctx *w,
			      const struct doca_sta_eu_ctr_entry *entries,
			      uint16_t num_entries)
{
	unsigned int i;

	for (i = 0; i < num_entries; i++) {
		spdk_json_write_named_uint64(w, entries[i].name, *entries[i].val);
	}
}

static void
rpc_tgt_ofld_eu_dump_counters(struct doca_sta_eu_handle *eu_handle, struct spdk_json_write_ctx *w)
{
	struct rpc_tgt_ofld_eu_data eu_data;
	const struct doca_sta_eu_ctr_entry *ctr_entries;
	uint16_t num_entries;
	doca_error_t drc;

	drc = rpc_tgt_ofld_eu_get_data(eu_handle, &eu_data);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get EU data: %s\n", doca_error_get_descr(drc));
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "hdlr_name", eu_data.name);
	spdk_json_write_named_uint16(w, "eu_id", eu_data.eu_id);
	spdk_json_write_named_uint16(w, "port", eu_data.port);
	spdk_json_write_named_string(w, "state", "RUNNING");

	drc = doca_sta_get_eu_stats(eu_handle, &ctr_entries, &num_entries);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get EU stats: %s\n", doca_error_get_descr(drc));
	} else {
		rpc_tgt_ofld_ctr_entries_dump(w, ctr_entries, num_entries);
	}

	spdk_json_write_object_end(w);
}

struct rpc_tgt_ofld_hdlr_counter_attr {
	enum dpa_sta_eu_type type;
	char *name;
};

static const struct spdk_json_object_decoder rpc_tgt_ofld_rpc_hdlr_counter_decoder[] = {
	{"type", offsetof(struct rpc_tgt_ofld_hdlr_counter_attr, type), rpc_tgt_ofld_decode_eu_type, true},
	{"name", offsetof(struct rpc_tgt_ofld_hdlr_counter_attr, name), spdk_json_decode_string, true},
};

static void
rpc_tgt_ofld_event_handler_counter(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_hdlr_counter_attr attr = {
		.type = DOCA_STA_EU_TYPE_UNKNOWN
	};
	struct spdk_nvmf_rdma_sta *sta;
	struct spdk_json_write_ctx *w;
	struct doca_sta_eu_handle **eu_handles = NULL;
	struct doca_sta_eu_handle *eu_handle;
	uint32_t i, num_eu_handles;
	int rc;
	doca_error_t drc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_rpc_hdlr_counter_decoder,
					    SPDK_COUNTOF(rpc_tgt_ofld_rpc_hdlr_counter_decoder), &attr)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}
	}
	if (attr.type != DOCA_STA_EU_TYPE_UNKNOWN && attr.name != NULL) {
		SPDK_ERRLOG("Only one of type and name can be configured\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		goto cleanup;
	}

	rc = rpc_tgt_ofld_get_sta(&sta);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto cleanup;
	}

	drc = rpc_tgt_ofld_get_eu_handles(sta, &eu_handles, &num_eu_handles);
	if (DOCA_IS_ERROR(drc)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 doca_error_get_descr(drc));
		goto cleanup;
	}

	eu_handle = NULL;
	if (attr.name) {
		drc = rpc_tgt_ofld_eu_find_by_name(eu_handles, num_eu_handles, attr.name, &eu_handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("EU %s is not found\n", attr.name);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}
	}
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (eu_handle) {
		rpc_tgt_ofld_eu_dump_counters(eu_handle, w);
	} else {
		for (i = 0; i < num_eu_handles; ++i) {
			if (attr.type != DOCA_STA_EU_TYPE_UNKNOWN &&
			    rpc_tgt_ofld_eu_cmp_type(eu_handles[i], attr.type)) {
				continue;
			}
			rpc_tgt_ofld_eu_dump_counters(eu_handles[i], w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	if (eu_handles) {
		free(eu_handles);
	}
	if (attr.name) {
		free(attr.name);
	}
}
SPDK_RPC_REGISTER("tgt_ofld_event_handler_counter", rpc_tgt_ofld_event_handler_counter,
		  SPDK_RPC_RUNTIME)

static void
rpc_tgt_ofld_event_handler_counter_reset(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_hdlr_counter_attr attr = {
		.type = DOCA_STA_EU_TYPE_UNKNOWN
	};
	struct spdk_nvmf_rdma_sta *sta;
	struct doca_sta_eu_handle **eu_handles = NULL;
	struct doca_sta_eu_handle *eu_handle;
	uint32_t num_eu_handles;
	enum dpa_sta_eu_type all_eu_types[] = {
		DOCA_STA_EU_COMP,
		DOCA_STA_EU_TX,
		DOCA_STA_EU_BEQ
	};
	size_t i;
	int rc;
	doca_error_t drc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_rpc_hdlr_counter_decoder,
					    SPDK_COUNTOF(rpc_tgt_ofld_rpc_hdlr_counter_decoder), &attr)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}
	}
	if (attr.type != DOCA_STA_EU_TYPE_UNKNOWN && attr.name != NULL) {
		SPDK_ERRLOG("Only one of type and name can be configured\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		goto cleanup;
	}

	rc = rpc_tgt_ofld_get_sta(&sta);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto cleanup;
	}

	if (attr.type != DOCA_STA_EU_TYPE_UNKNOWN) {
		drc = doca_sta_eu_reset_stats_type(sta->sta, attr.type);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to reset counters by EU type (%d): %s\n",
				    attr.type, doca_error_get_descr(drc));
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}
	} else if (attr.name != NULL) {
		drc = rpc_tgt_ofld_get_eu_handles(sta, &eu_handles, &num_eu_handles);
		if (DOCA_IS_ERROR(drc)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}

		drc = rpc_tgt_ofld_eu_find_by_name(eu_handles, num_eu_handles, attr.name, &eu_handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("EU %s is not found\n", attr.name);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}
		drc = doca_sta_eu_reset_stats_handle(eu_handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to reset counters for EU %s: %s\n", attr.name,
				    doca_error_get_descr(drc));
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}
	} else {
		for (i = 0; i < SPDK_COUNTOF(all_eu_types); i++) {
			drc = doca_sta_eu_reset_stats_type(sta->sta, all_eu_types[i]);
			if (DOCA_IS_ERROR(drc)) {
				SPDK_ERRLOG("Failed to reset EU counters: %s\n", doca_error_get_descr(drc));
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
								 doca_error_get_descr(drc));
				goto cleanup;
			}
		}
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	if (eu_handles) {
		free(eu_handles);
	}
	if (attr.name) {
		free(attr.name);
	}
}
SPDK_RPC_REGISTER("tgt_ofld_event_handler_counter_reset", rpc_tgt_ofld_event_handler_counter_reset,
		  SPDK_RPC_RUNTIME)

static void
rpc_tgt_ofld_qp_dump(struct doca_sta_qp_handle *qp_handle, struct spdk_json_write_ctx *w)
{
	uint32_t qpn;
	uint16_t port_id;
	uint16_t conn_id;
	doca_error_t drc;

	drc = doca_sta_io_qp_get_id(qp_handle, &qpn);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get QPN: %s\n", doca_error_get_descr(drc));
		return;
	}

	drc = doca_sta_io_qp_get_port_id(qp_handle, &port_id);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get QP port: %s\n", doca_error_get_descr(drc));
		return;
	}

	drc = doca_sta_io_qp_get_index_in_group(qp_handle, &conn_id);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get QP conn_id: %s\n", doca_error_get_descr(drc));
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string_fmt(w, "QPN", "0x%x", qpn);
	spdk_json_write_named_uint16(w, "port", port_id);
	spdk_json_write_named_uint16(w, "conn_id", conn_id);

	spdk_json_write_object_end(w);
}

static void
rpc_tgt_ofld_eu_dump_qp_stats(struct doca_sta_eu_handle *eu_handle,
			      struct doca_sta_qp_handle **qp_handles,
			      uint16_t *num_qp_handles, struct spdk_json_write_ctx *w)
{
	char grp_name[64];
	uint16_t eu_id;
	uint16_t i;
	doca_error_t drc;

	drc = doca_sta_get_eu_id(eu_handle, &eu_id);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get EU id\n");
		*num_qp_handles = 0;
		return;
	}

	drc = doca_sta_get_eu_connect_qp_stats(eu_handle, qp_handles, num_qp_handles);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get QP stats for EU #%u: %s\n", eu_id, doca_error_get_descr(drc));
		*num_qp_handles = 0;
		return;
	}
	if (*num_qp_handles == 0) {
		return;
	}

	spdk_json_write_object_begin(w);

	snprintf(grp_name, sizeof(grp_name), "EU #%u", eu_id);
	spdk_json_write_named_array_begin(w, grp_name);

	for (i = 0; i < *num_qp_handles; i++) {
		rpc_tgt_ofld_qp_dump(qp_handles[i], w);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_named_uint32(w, "Total", *num_qp_handles);
	spdk_json_write_object_end(w);
}

#define RPC_TGT_OFLD_EU_NUM_MAX		UINT16_MAX
#define RPC_TGT_OFLD_EU_NUM_UNDEFINED	(RPC_TGT_OFLD_EU_NUM_MAX + 1)

static int
rpc_tgt_ofld_decode_eu_num(const struct spdk_json_val *val, void *out)
{
	int rc;
	uint32_t eu_num;
	uint32_t *u = out;

	rc = spdk_json_number_to_uint32(val, &eu_num);
	if (rc) {
		return -EINVAL;
	}
	if (eu_num > RPC_TGT_OFLD_EU_NUM_MAX) {
		SPDK_NOTICELOG("Invalid parameter value: too big (%u > %u)\n",
			       eu_num, RPC_TGT_OFLD_EU_NUM_MAX);
		return -EINVAL;
	}
	*u = eu_num;

	return 0;
}

struct rpc_tgt_ofld_comp_eu_num_attr {
	uint32_t group;
};

static const struct spdk_json_object_decoder rpc_tgt_ofld_comp_eu_num_decoder[] = {
	{"group", offsetof(struct rpc_tgt_ofld_comp_eu_num_attr, group), rpc_tgt_ofld_decode_eu_num, true},
};

static void
rpc_tgt_ofld_connect_qp_list(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_comp_eu_num_attr attr = {
		.group = RPC_TGT_OFLD_EU_NUM_UNDEFINED
	};
	struct spdk_nvmf_rdma_sta *sta;
	struct doca_sta_eu_handle **eu_handles;
	struct doca_sta_eu_handle *eu_handle;
	struct doca_sta_qp_handle **qp_handles;
	struct spdk_json_write_ctx *w;
	uint32_t i, num_eu_handles;
	uint16_t num_qp_handles, connected_qps;
	uint32_t total_qps = 0;
	int rc;
	doca_error_t drc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_comp_eu_num_decoder,
					    SPDK_COUNTOF(rpc_tgt_ofld_comp_eu_num_decoder), &attr)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	rc = rpc_tgt_ofld_get_sta(&sta);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		return;
	}

	drc = rpc_tgt_ofld_get_eu_handles(sta, &eu_handles, &num_eu_handles);
	if (DOCA_IS_ERROR(drc)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 doca_error_get_descr(drc));
		return;
	}

	num_qp_handles = spdk_min(sta->caps.max_connected_qps, UINT16_MAX);
	qp_handles = calloc(num_qp_handles, sizeof(*qp_handles));
	if (!qp_handles) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		goto cleanup;
	}

	eu_handle = NULL;
	if (attr.group != RPC_TGT_OFLD_EU_NUM_UNDEFINED) {
		drc = rpc_tgt_ofld_eu_find_by_type_id(eu_handles, num_eu_handles, DOCA_STA_EU_COMP,
						      attr.group, &eu_handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Group %u is not found\n", attr.group);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (eu_handle) {
		connected_qps = num_qp_handles;
		rpc_tgt_ofld_eu_dump_qp_stats(eu_handle, qp_handles, &connected_qps, w);
		total_qps += connected_qps;
	} else {
		for (i = 0; i < num_eu_handles; ++i) {
			if (rpc_tgt_ofld_eu_cmp_type(eu_handles[i], DOCA_STA_EU_COMP)) {
				continue;
			}

			connected_qps = num_qp_handles;
			rpc_tgt_ofld_eu_dump_qp_stats(eu_handles[i], qp_handles, &connected_qps, w);
			total_qps += connected_qps;
		}
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "Total", total_qps);
	spdk_json_write_object_end(w);

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	if (qp_handles) {
		free(qp_handles);
	}
	if (eu_handles) {
		free(eu_handles);
	}
}
SPDK_RPC_REGISTER("tgt_ofld_connect_qp_list", rpc_tgt_ofld_connect_qp_list, SPDK_RPC_RUNTIME)

static void
rpc_tgt_ofld_eu_dump_qp_count(struct doca_sta_eu_handle *eu_handle, uint16_t *num_qps,
			      struct spdk_json_write_ctx *w)
{
	struct rpc_tgt_ofld_eu_data eu_data;
	doca_error_t drc;

	*num_qps = 0;
	drc = rpc_tgt_ofld_eu_get_data(eu_handle, &eu_data);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed get EU data: %s\n", doca_error_get_descr(drc));
		return;
	}

	drc = doca_sta_get_eu_connect_qp_stats(eu_handle, NULL, num_qps);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to qp stats for EU #%u\n", eu_data.eu_id);
		return;
	}

	if (*num_qps) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_uint32(w, "EU #", eu_data.eu_id);
		spdk_json_write_named_uint32(w, "Port", eu_data.port);
		spdk_json_write_named_uint32(w, "Total", *num_qps);

		spdk_json_write_object_end(w);
	}
}

static void
rpc_tgt_ofld_connect_qp_count(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_comp_eu_num_attr attr = {
		.group = RPC_TGT_OFLD_EU_NUM_UNDEFINED
	};
	struct spdk_nvmf_rdma_sta *sta;
	struct spdk_json_write_ctx *w;
	struct doca_sta_eu_handle **eu_handles = NULL;
	struct doca_sta_eu_handle *eu_handle;
	uint32_t i, num_eu_handles, total_qps;
	uint16_t num_qps;
	int rc;
	doca_error_t drc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_comp_eu_num_decoder,
					    SPDK_COUNTOF(rpc_tgt_ofld_comp_eu_num_decoder), &attr)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	rc = rpc_tgt_ofld_get_sta(&sta);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		return;
	}

	drc = rpc_tgt_ofld_get_eu_handles(sta, &eu_handles, &num_eu_handles);
	if (DOCA_IS_ERROR(drc)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 doca_error_get_descr(drc));
		return;
	}

	eu_handle = NULL;
	if (attr.group != RPC_TGT_OFLD_EU_NUM_UNDEFINED) {
		drc = rpc_tgt_ofld_eu_find_by_type_id(eu_handles, num_eu_handles, DOCA_STA_EU_COMP,
						      attr.group, &eu_handle);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Group %u is not found\n", attr.group);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 doca_error_get_descr(drc));
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (eu_handle) {
		rpc_tgt_ofld_eu_dump_qp_count(eu_handle, &num_qps, w);
		total_qps = num_qps;
	} else {
		total_qps = 0;
		for (i = 0; i < num_eu_handles; ++i) {
			if (rpc_tgt_ofld_eu_cmp_type(eu_handles[i], DOCA_STA_EU_COMP)) {
				continue;
			}

			rpc_tgt_ofld_eu_dump_qp_count(eu_handles[i], &num_qps, w);
			total_qps += num_qps;
		}
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "Total", total_qps);
	spdk_json_write_object_end(w);

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free(eu_handles);
}
SPDK_RPC_REGISTER("tgt_ofld_connect_qp_count", rpc_tgt_ofld_connect_qp_count, SPDK_RPC_RUNTIME)

struct rpc_tgt_ofld_get_backend_ctrl_stat {
	char *name;
};

static void
free_rpc_tgt_ofld_get_backend_ctrl_stat(struct rpc_tgt_ofld_get_backend_ctrl_stat *s)
{
	if (s->name) {
		free(s->name);
	}
}

static const struct spdk_json_object_decoder rpc_tgt_ofld_get_backend_ctrl_stat_decoders[] = {
	{"name", offsetof(struct rpc_tgt_ofld_get_backend_ctrl_stat, name), spdk_json_decode_string, true},
};

static void
rpc_tgt_ofld_be_ctrlr_stats_dump(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_rdma_bdev *rbdev)
{
	uint32_t i;
	doca_error_t drc;
	struct doca_sta_be_q_handle *q_handle;
	const struct doca_sta_eu_ctr_entry *ctr_entries;
	uint16_t num_entries;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", rbdev->name);
	spdk_json_write_named_array_begin(w, "queues");

	for (i = 0; i < rbdev->num_queues; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string_fmt(w, "name", "queue%d", i);

		if (rbdev->type == SPDK_NVMF_RDMA_BDEV_TYPE_NVME) {
			q_handle = rbdev->nvme.queues[i].handle;
		} else {
			q_handle = rbdev->null.queues[i].handle;
		}

		drc = doca_sta_get_be_queue_stats(rbdev->handle, q_handle, &ctr_entries, &num_entries);
		if (DOCA_IS_ERROR(drc)) {
			SPDK_ERRLOG("Failed to get queue %d stats: %s\n", i, doca_error_get_descr(drc));
		} else {
			rpc_tgt_ofld_ctr_entries_dump(w, ctr_entries, num_entries);
		}
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

static void
rpc_tgt_ofld_get_backend_ctrl_stat(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_get_backend_ctrl_stat req = {};
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_bdev *rbdev = NULL;
	struct spdk_json_write_ctx *w;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_get_backend_ctrl_stat_decoders,
					    SPDK_COUNTOF(rpc_tgt_ofld_get_backend_ctrl_stat_decoders), &req)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}
	}

	rc = rpc_tgt_ofld_get_rtransport(&rtransport);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto cleanup;
	}

	if (req.name) {
		TAILQ_FOREACH(rbdev, &rtransport->bdevs, link) {
			if (strcmp(rbdev->name, req.name) == 0) {
				break;
			}
		}
		if (!rbdev) {
			SPDK_ERRLOG("Backend controller is not found (%s)\n", req.name);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 spdk_strerror(ENODEV));
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (rbdev) {
		rpc_tgt_ofld_be_ctrlr_stats_dump(w, rbdev);
	} else {
		TAILQ_FOREACH(rbdev, &rtransport->bdevs, link) {
			rpc_tgt_ofld_be_ctrlr_stats_dump(w, rbdev);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_rpc_tgt_ofld_get_backend_ctrl_stat(&req);
}
SPDK_RPC_REGISTER("tgt_ofld_get_backend_ctrl_stat", rpc_tgt_ofld_get_backend_ctrl_stat,
		  SPDK_RPC_RUNTIME)

struct rpc_tgt_ofld_get_bdev_stat {
	char *name;
};

static void
free_tgt_ofld_get_bdev_stat(struct rpc_tgt_ofld_get_bdev_stat *s)
{
	if (s->name) {
		free(s->name);
	}
}

static const struct spdk_json_object_decoder rpc_tgt_ofld_get_bdev_stat_decoders[] = {
	{"name", offsetof(struct rpc_tgt_ofld_get_bdev_stat, name), spdk_json_decode_string, true},
};

static void
rpc_tgt_ofld_get_bdev_stats_dump(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_rdma_subsystem *rsubsystem,
				 struct spdk_nvmf_rdma_ns *rns)
{
	const struct doca_sta_eu_ctr_entry *ctr_entries;
	uint16_t num_entries;
	doca_error_t drc;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(rns->ns->bdev));

	drc = doca_sta_get_ns_stats(rsubsystem->handle, rns->handle, &ctr_entries, &num_entries);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get bdev %s stats: %s\n", spdk_bdev_get_name(rns->ns->bdev),
			    doca_error_get_descr(drc));
	} else {
		rpc_tgt_ofld_ctr_entries_dump(w, ctr_entries, num_entries);
	}

	spdk_json_write_object_end(w);
}

static void
rpc_tgt_ofld_get_bdev_stat(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_get_bdev_stat req = {};
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_subsystem *rsubsystem;
	struct spdk_nvmf_rdma_ns *rns = NULL;
	struct spdk_json_write_ctx *w;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_get_bdev_stat_decoders,
					    SPDK_COUNTOF(rpc_tgt_ofld_get_bdev_stat_decoders), &req)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}
	}

	rc = rpc_tgt_ofld_get_rtransport(&rtransport);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto cleanup;
	}

	if (req.name) {
		TAILQ_FOREACH(rsubsystem, &rtransport->subsystems, link) {
			TAILQ_FOREACH(rns, &rsubsystem->namespaces, link) {
				if (strcmp(spdk_bdev_get_name(rns->ns->bdev), req.name) == 0) {
					goto bdev_is_found;
				}
			}
		}
		SPDK_ERRLOG("bdev is not found (%s)\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		goto cleanup;
	}
bdev_is_found:
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (rns) {
		assert(rsubsystem);
		rpc_tgt_ofld_get_bdev_stats_dump(w, rsubsystem, rns);
	} else {
		TAILQ_FOREACH(rsubsystem, &rtransport->subsystems, link) {
			TAILQ_FOREACH(rns, &rsubsystem->namespaces, link) {
				rpc_tgt_ofld_get_bdev_stats_dump(w, rsubsystem, rns);
			}
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_tgt_ofld_get_bdev_stat(&req);
}
SPDK_RPC_REGISTER("tgt_ofld_get_bdev_stat", rpc_tgt_ofld_get_bdev_stat, SPDK_RPC_RUNTIME)

struct rpc_tgt_ofld_get_bdev_queue_mapping {
	char *name;
};

static void
free_tgt_ofld_get_bdev_queue_mapping(struct rpc_tgt_ofld_get_bdev_queue_mapping *s)
{
	if (s->name) {
		free(s->name);
	}
}

static const struct spdk_json_object_decoder rpc_tgt_ofld_get_bdev_queue_mapping_decoders[] = {
	{"name", offsetof(struct rpc_tgt_ofld_get_bdev_queue_mapping, name), spdk_json_decode_string, true},
};

static void
rpc_tgt_ofld_be_queue_mapping_dump(struct spdk_json_write_ctx *w,
				   const struct doca_sta_be_q_handle *queue_handle)
{
	const struct doca_sta_eu_ctr_entry *map_entries;
	uint16_t num_entries;
	doca_error_t drc;

	drc = doca_sta_get_be_queue_mapping_info(queue_handle, &map_entries, &num_entries);
	if (DOCA_IS_ERROR(drc)) {
		SPDK_ERRLOG("Failed to get queue mapping: %s\n", doca_error_get_descr(drc));
	} else {
		spdk_json_write_object_begin(w);
		rpc_tgt_ofld_ctr_entries_dump(w, map_entries, num_entries);
		spdk_json_write_object_end(w);
	}
}

static void
rpc_tgt_ofld_bdev_queues_mapping_dump(struct spdk_json_write_ctx *w,
				      struct spdk_nvmf_rdma_ns *rns)
{
	struct spdk_nvmf_rdma_bdev *rbdev = rns->rbdev;
	struct doca_sta_be_q_handle *queue_handle;
	uint32_t i;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(rns->ns->bdev));
	spdk_json_write_named_array_begin(w, "queues");

	for (i = 0; i < rbdev->num_queues; i++) {
		if (rbdev->type == SPDK_NVMF_RDMA_BDEV_TYPE_NVME) {
			queue_handle = rbdev->nvme.queues[i].handle;
		} else {
			queue_handle = rbdev->null.queues[i].handle;
		}

		rpc_tgt_ofld_be_queue_mapping_dump(w, queue_handle);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

static void
rpc_tgt_ofld_get_bdev_queue_mapping(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_tgt_ofld_get_bdev_queue_mapping req = {};
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_subsystem *rsubsystem;
	struct spdk_nvmf_rdma_ns *rns = NULL;
	struct spdk_json_write_ctx *w;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_tgt_ofld_get_bdev_queue_mapping_decoders,
					    SPDK_COUNTOF(rpc_tgt_ofld_get_bdev_queue_mapping_decoders), &req)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}
	}

	rc = rpc_tgt_ofld_get_rtransport(&rtransport);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto cleanup;
	}

	if (req.name) {
		TAILQ_FOREACH(rsubsystem, &rtransport->subsystems, link) {
			TAILQ_FOREACH(rns, &rsubsystem->namespaces, link) {
				if (strcmp(spdk_bdev_get_name(rns->ns->bdev), req.name) == 0) {
					goto bdev_is_found;
				}
			}
		}
		SPDK_ERRLOG("bdev is not found (%s)\n", req.name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		goto cleanup;
	}
bdev_is_found:
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (rns) {
		rpc_tgt_ofld_bdev_queues_mapping_dump(w, rns);
	} else {
		TAILQ_FOREACH(rsubsystem, &rtransport->subsystems, link) {
			TAILQ_FOREACH(rns, &rsubsystem->namespaces, link) {
				rpc_tgt_ofld_bdev_queues_mapping_dump(w, rns);
			}
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_tgt_ofld_get_bdev_queue_mapping(&req);
}
SPDK_RPC_REGISTER("tgt_ofld_get_bdev_queue_mapping", rpc_tgt_ofld_get_bdev_queue_mapping,
		  SPDK_RPC_RUNTIME)
