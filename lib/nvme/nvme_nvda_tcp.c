/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"

#include "nvme_internal.h"

#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/crc32.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/bit_pool.h"
#include "spdk/accel_module.h"

#include "spdk_internal/nvme_nvda_tcp.h"
#include "spdk_internal/trace_defs.h"
#include "spdk_internal/rdma_provider.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/xlio.h"
#include "spdk_internal/sock_module.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define XLIO_PACKETS_BUF_SIZE 128

#if !defined(SO_ZEROCOPY) && !defined(MSG_ZEROCOPY)
#error "XLIO requires zcopy"
#endif

#define NVME_TCP_MAX_R2T_DEFAULT		1
#define DEFAULT_ZCOPY_THRESHOLD			512
#define NVDA_TCP_PACKET_POOL_SHARED		UINT32_MAX

struct xlio_sock_packet {
	struct xlio_buf *xlio_buf;
	struct iovec iov;
	int refs;
	bool from_malloc;
	STAILQ_ENTRY(xlio_sock_packet) link;
};

struct xlio_sock_buf {
	struct iovec iov;
	struct xlio_sock_buf *next;
	struct xlio_sock_packet *packet;
};

/* xlio packets pool for each core */
struct xlio_packets_pool {
	STAILQ_HEAD(, xlio_sock_packet)	free_packets;
	struct xlio_sock_packet	*packets;
	uint32_t		core_id;
	uint32_t		refcnt;
	STAILQ_ENTRY(xlio_packets_pool)	link;
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
typedef TAILQ_HEAD(, nvme_tcp_req)	nvme_tcp_req_tailq_t;

struct nvme_tcp_poll_group {
	struct spdk_nvme_transport_poll_group		group;
	xlio_poll_group_t				xgroup;

	TAILQ_HEAD(pending_recv_head, nvme_tcp_qpair)	pending_events;
	struct spdk_sock_impl_opts			impl_opts;
	union {
		struct {
			uint8_t				pp_handler_registered: 1;
			uint8_t				reserved: 7;
		} flags;
		uint8_t					raw;
	};

	uint32_t					completions_per_qpair;
	int64_t						num_completions;

	void						*tcp_reqs;
	TAILQ_HEAD(, nvme_tcp_pdu)			free_pdus;
	void						*recv_pdus;
	struct spdk_nvme_tcp_stat			stats;
};

struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;

	TAILQ_ENTRY(nvme_tcp_qpair)		link;
	STAILQ_HEAD(, xlio_sock_packet)		received_packets;

	struct xlio_packets_pool		*xlio_packets_pool;
	struct nvme_tcp_poll_group		*group;
	struct ibv_pd				*pd;

	xlio_socket_t				xlio_sock;
	uint16_t				consumed_packets;
	uint16_t				zcopy_tx_pdus;

	union {
		struct {
			uint16_t host_hdgst_enable: 1;
			uint16_t host_ddgst_enable: 1;
			uint16_t icreq_send_ack: 1;
			uint16_t use_poll_group_req_pool: 1;
			uint16_t needs_resubmit: 1;
			uint16_t shared_stats: 1;
			uint16_t has_accel_nomem_pdus : 1;
			uint16_t pending_events: 1;
			uint16_t pending_send: 1;
			uint16_t disconnected: 1;
			uint16_t closed: 1;
			uint16_t stop_receiving: 1;
			uint16_t connect_notified: 1;
			uint16_t reserved : 3;
		} flags;
		uint16_t flags_raw;
	};

	size_t					cur_offset;

	nvme_tcp_req_tailq_t			outstanding_reqs;
	struct nvme_tcp_pdu			*recv_pdu;

	struct spdk_bit_pool			*cid_pool;
	struct nvme_tcp_req			**tcp_reqs_lookup;
	struct spdk_rdma_utils_mem_map		*mem_map;
	struct spdk_memory_domain		*memory_domain;
	struct spdk_nvme_tcp_stat		*stats;

	uint32_t				pdus_mkey;
	uint32_t				maxh2cdata;

	uint16_t				num_entries;
	uint16_t				async_complete;

	/* enum nvme_tcp_qpair_state */
	uint8_t					state : 4;
	/* enum nvme_tcp_pdu_recv_state */
	uint8_t					recv_state : 4;
	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint8_t					maxr2t;

	/* 0 based value, which is used to guide the padding */
	uint8_t					cpda;

	/* XXX 7 bytes hole, try to pack */
	/* "Cold" data starts here */
	TAILQ_HEAD(, nvme_tcp_pdu)		accel_nomem_queue;
	uint64_t				icreq_timeout_tsc;
	struct nvme_tcp_pdu			*send_pdu; /* only for error pdu and init pdu */
	void					*_recv_pdu;
	struct nvme_tcp_req			*tcp_reqs;
	struct nvme_tcp_req			*reserved_tcp_req;
	struct iovec				ctrlr_hdr_iov;
	union {
		/* to hold error pdu data */
		uint8_t					raw[SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE];
		struct spdk_nvme_tcp_common_pdu_hdr	common;
		struct spdk_nvme_tcp_ic_req		ic_req;
		struct spdk_nvme_tcp_term_req_hdr	term_req;
	} ctrl_hdr;
};

enum nvme_tcp_req_state {
	NVME_TCP_REQ_FREE,
	NVME_TCP_REQ_ACTIVE,
	NVME_TCP_REQ_ACTIVE_R2T,
};

#define MAX_RX_IOVS 128

struct nvme_tcp_req {
	struct nvme_request			req;
	struct spdk_iobuf_entry			iobuf_entry;
	struct iovec				iobuf_iov;
	TAILQ_ENTRY(nvme_tcp_req)		link;
	struct xlio_sock_buf			*sock_buf;
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C ack */
	uint32_t				r2tl_remain_next;
	uint32_t				datao;
	uint32_t				expected_datao;
	uint32_t				r2tl_remain;
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C complete */
	uint16_t				ttag_r2t_next;
	uint16_t				cid;
	uint16_t				ttag;
	/* It is used to track whether the req can be safely freed */
	union {
		uint16_t raw;
		struct {
			/* The last send operation completed - kernel released send buffer */
			uint16_t			send_ack : 1;
			/* Data transfer completed - target send resp or last data bit */
			uint16_t			data_recv : 1;
			/* tcp_req is waiting for completion of the previous send operation (buffer reclaim notification
			 * from kernel) to send H2C */
			uint16_t			h2c_send_waiting_ack : 1;
			/* tcp_req received subsequent r2t while it is still waiting for send_ack.
			 * Rare case, actual when dealing with target that can send several R2T requests.
			 * SPDK TCP target sends 1 R2T for the whole data buffer */
			uint16_t			r2t_waiting_h2c_complete : 1;
			uint16_t			in_progress_accel : 1;
			uint16_t			digest_offloaded : 1;
			uint16_t			has_memory_domain : 1;
			uint16_t			needs_accel_seq : 1;
			uint16_t			in_capsule_data : 1;
			uint16_t			recv_iovs_malloced : 1;
			uint16_t			state : 3;
		} bits;
	} ordering;
	uint8_t					active_r2ts;
	/* XXX 7 bytes hole, try to pack */
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C ack */
	struct nvme_tcp_pdu			pdu;

	/* For writes, this describes the source of the data. For reads, this describes the
	 * destination. */
	struct iovec				iovs[NVME_TCP_MAX_SGL_DESCRIPTORS];

	/* For reads we need another set of iovs to describe the segments of the tcp
	 * stream. */
	uint32_t				recv_iovcnt;
	struct iovec				*recv_iovs;
	struct iovec				_recv_iovs[NVME_TCP_MAX_SGL_DESCRIPTORS];
};

struct xlio_shared_group {
	xlio_poll_group_t xlio_group;
	int refs;
};

SPDK_STATIC_ASSERT(NVME_TCP_MAX_R2T_DEFAULT <= UINT8_MAX,
		   "type of active_r2ts needs to be extended");

static pthread_mutex_t g_xlio_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static STAILQ_HEAD(, xlio_packets_pool) g_xlio_packets_pools = STAILQ_HEAD_INITIALIZER(
			g_xlio_packets_pools);
static struct spdk_mempool *g_xlio_nvme_buffers_pool;
static struct spdk_mempool *g_iovec_pool = NULL;
static pthread_mutex_t g_xlio_admin_group_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct spdk_spinlock g_xlio_admin_group_lock;
static bool g_xlio_admin_group_lock_initialized = false;
static struct xlio_shared_group g_xlio_admin_group;
static uint32_t g_xlio_buffers_pool_refs;

static int xlio_sock_close(struct nvme_tcp_qpair *qpair);
static void _pdu_write_done(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu, int err);
static inline int nvme_tcp_fill_data_mkeys(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_req *tcp_req, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_qpair_process_pending_events(struct nvme_tcp_qpair *tqpair);
static void nvme_tcp_qpair_connect_sock_done(struct nvme_tcp_qpair *qpair, int err);
static int xlio_sock_group_create(xlio_poll_group_t *group, unsigned int flags);
static void xlio_sock_put_buffers_pool_unsafe(void);
static void xlio_sock_put_packets_pool_unsafe(struct xlio_packets_pool *pool);

static void
xlio_sock_put_xlio_pools(struct nvme_tcp_qpair *tqpair)
{
	if (!tqpair->xlio_packets_pool) {
		return;
	}

	pthread_mutex_lock(&g_xlio_pool_mutex);
	xlio_sock_put_packets_pool_unsafe(tqpair->xlio_packets_pool);
	tqpair->xlio_packets_pool = NULL;
	xlio_sock_put_buffers_pool_unsafe();
	pthread_mutex_unlock(&g_xlio_pool_mutex);
}

static int
xlio_sock_set_recvbuf(struct nvme_tcp_qpair *tqpair, int sz)
{
	struct spdk_sock_impl_opts impl_opts;
	size_t opts_len = sizeof(impl_opts);
	int min_size;
	int rc;

	assert(tqpair != NULL);

	rc = spdk_sock_impl_get_opts("xlio", &impl_opts, &opts_len);
	if (rc) {
		return rc;
	}
	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = xlio_socket_setsockopt(tqpair->xlio_sock, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

/* g_xlio_pool_mutex must be held. */
static int
xlio_sock_get_buffers_pool_unsafe(uint32_t buffers_pool_size)
{
	if (g_xlio_nvme_buffers_pool) {
		g_xlio_buffers_pool_refs++;
		return 0;
	}

	g_xlio_nvme_buffers_pool = spdk_mempool_create("xlio_nvme_buffers_pool",
				   buffers_pool_size,
				   sizeof(struct xlio_sock_buf),
				   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_SOCKET_ID_ANY);
	if (!g_xlio_nvme_buffers_pool) {
		SPDK_ERRLOG("Failed to create xlio buffers pool\n");
		return -ENOMEM;
	}

	g_iovec_pool = spdk_mempool_create("nvda_iovec_pool",
					   buffers_pool_size,
					   sizeof(struct iovec) * MAX_RX_IOVS,
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);
	if (!g_iovec_pool) {
		SPDK_ERRLOG("Failed to create iovec pool\n");
		spdk_mempool_free(g_xlio_nvme_buffers_pool);
		g_xlio_nvme_buffers_pool = NULL;
		return -ENOMEM;
	}

	SPDK_NOTICELOG("Create xlio buffers pool, buffers_pool_size %u\n", buffers_pool_size);

	g_xlio_buffers_pool_refs = 1;
	return 0;
}

/* g_xlio_pool_mutex must be held. */
static void
xlio_sock_put_buffers_pool_unsafe(void)
{
	assert(g_xlio_buffers_pool_refs > 0);

	g_xlio_buffers_pool_refs--;
	if (g_xlio_buffers_pool_refs == 0) {
		if (g_xlio_nvme_buffers_pool != NULL) {
			spdk_mempool_free(g_xlio_nvme_buffers_pool);
			g_xlio_nvme_buffers_pool = NULL;
		}
		if (g_iovec_pool != NULL) {
			spdk_mempool_free(g_iovec_pool);
			g_iovec_pool = NULL;
		}
	}
}

/* g_xlio_pool_mutex must be held. */
static void
xlio_sock_put_packets_pool_unsafe(struct xlio_packets_pool *pool)
{
	assert(pool != NULL);
	assert(pool->refcnt > 0);

	pool->refcnt--;
	if (pool->refcnt == 0) {
		STAILQ_REMOVE(&g_xlio_packets_pools, pool, xlio_packets_pool, link);
		free(pool->packets);
		free(pool);
	}
}

/* g_xlio_pool_mutex must be held. */
static struct xlio_packets_pool *
xlio_sock_get_packets_pool_unsafe(uint32_t packets_pool_size, bool shared)
{
	struct xlio_packets_pool *pool;
	uint32_t i, current_core = spdk_env_get_current_core();

	assert(current_core != NVDA_TCP_PACKET_POOL_SHARED);

	STAILQ_FOREACH(pool, &g_xlio_packets_pools, link) {
		if ((!shared && pool->core_id == current_core) || (shared &&
				pool->core_id == NVDA_TCP_PACKET_POOL_SHARED)) {
			pool->refcnt++;
			return pool;
		}
	}

	pool = calloc(1, sizeof(*pool));
	if (!pool) {
		SPDK_ERRLOG("Failed to allocate pool\n");
		return NULL;
	}

	pool->packets = calloc(packets_pool_size,
			       sizeof(struct xlio_sock_packet));
	if (!pool->packets) {
		SPDK_ERRLOG("Failed to allocate packets\n");
		free(pool);
		return NULL;
	}

	STAILQ_INIT(&pool->free_packets);
	for (i = 0; i < packets_pool_size; ++i) {
		STAILQ_INSERT_TAIL(&pool->free_packets, &pool->packets[i], link);
	}

	STAILQ_INSERT_HEAD(&g_xlio_packets_pools, pool, link);
	pool->core_id = shared ? NVDA_TCP_PACKET_POOL_SHARED : current_core;
	pool->refcnt = 1;
	SPDK_NOTICELOG("Create xlio pool, packets_pool_size %u on core %u\n",
		       packets_pool_size, current_core);

	return pool;
}

static int
xlio_sock_alloc(struct nvme_tcp_qpair *tqpair, struct spdk_sock_impl_opts *xlio_opts)
{
	int flag = 1;
	int rc;
	struct xlio_packets_pool *pool;

	tqpair->pd = xlio_socket_get_pd(tqpair->xlio_sock);
	if (!tqpair->pd) {
		SPDK_ERRLOG("Failed to get pd\n");
		return -ENODEV;
	}

	pthread_mutex_lock(&g_xlio_pool_mutex);
	pool = xlio_sock_get_packets_pool_unsafe(xlio_opts->packets_pool_size,
			nvme_qpair_is_admin_queue(&tqpair->qpair));
	if (!pool) {
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		SPDK_ERRLOG("Failed to allocated packets pool for tqpair %p\n", tqpair);
		return -ENOMEM;
	}

	if (xlio_sock_get_buffers_pool_unsafe(xlio_opts->buffers_pool_size)) {
		xlio_sock_put_packets_pool_unsafe(pool);
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		return -ENOMEM;
	}

	tqpair->xlio_packets_pool = pool;
	pthread_mutex_unlock(&g_xlio_pool_mutex);

	STAILQ_INIT(&tqpair->received_packets);

#if defined(__linux__)
	flag = 1;

	if (xlio_opts->enable_quickack) {
		rc = xlio_socket_setsockopt(tqpair->xlio_sock, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}
#endif

	return 0;
}

static inline const char *
strip_ip(const char *ip, char *buf, size_t buf_size)
{
	char *p;

	if (ip[0] == '[') {
		snprintf(buf, buf_size, "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		return buf;
	}

	return ip;
}

static inline int
xlio_bind_client_socket(struct nvme_tcp_qpair *tqpair, const char *addr, int port)
{
	char buf[256];
	char portnum[32];
	struct addrinfo hints, *res;
	int rc;

	assert(addr || port);

	if (addr) {
		addr = strip_ip(addr, buf, sizeof(buf));
	}

	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_NUMERICHOST;
	rc = xlio_getaddrinfo(addr, portnum, &hints, &res);
	if (rc != 0) {
		SPDK_ERRLOG("Source getaddrinfo() failed %s (%d), address %s, port %d\n",
			    xlio_gai_strerror(rc), rc, addr ? addr : "null", port);
		return -1;
	}

	rc = xlio_socket_bind(tqpair->xlio_sock, res->ai_addr, res->ai_addrlen);
	if (rc != 0) {
		SPDK_ERRLOG("bind() failed at address %s port %d, errno = %d\n",
			    addr ? addr : "null", port, errno);
		xlio_freeaddrinfo(res);
		return -1;
	}

	xlio_freeaddrinfo(res);
	return 0;
}

static int
xlio_sock_init(struct nvme_tcp_qpair *tqpair, const char *ip, int port, struct spdk_sock_opts *opts,
	       xlio_poll_group_t group, int vlan_tag)
{
	struct spdk_sock_impl_opts *xlio_opts;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	const char *src_addr;
	uint16_t src_port;
	struct addrinfo hints, *res, *res0;
	int val = 1;
	int rc, sz;

	if (!opts && !opts->impl_opts) {
		return -EINVAL;
	}
	xlio_opts = opts->impl_opts;

	if (ip == NULL) {
		return -EINVAL;
	}

	ip = strip_ip(ip, buf, sizeof(buf));
	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = xlio_getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", xlio_gai_strerror(rc), rc);
		return rc;
	}

	/* try listen */
	for (res = res0; res != NULL; res = res->ai_next) {
		struct xlio_socket_attr sattr = {
			.domain = res->ai_family,
			.group = group,
			.userdata_sq = (uintptr_t)tqpair,
		};

		rc = xlio_socket_create(&sattr, &tqpair->xlio_sock);
		if (rc != 0) {
			/* error */
			continue;
		}

		sz = xlio_opts->recv_buf_size;
		xlio_socket_setsockopt(tqpair->xlio_sock, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));

		sz = xlio_opts->send_buf_size;
		xlio_socket_setsockopt(tqpair->xlio_sock, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));

		rc = xlio_socket_setsockopt(tqpair->xlio_sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			/* error */
			if (!xlio_socket_destroy(tqpair->xlio_sock)) {
				assert(false);
			}
			continue;
		}

		if (xlio_opts->enable_tcp_nodelay) {
			rc = xlio_socket_setsockopt(tqpair->xlio_sock, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
			if (rc != 0) {
				/* error */
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
		}

#if defined(SO_PRIORITY)
		if (opts->priority) {
			rc = xlio_socket_setsockopt(tqpair->xlio_sock, SOL_SOCKET, SO_PRIORITY, &opts->priority,
						    sizeof val);
			if (rc != 0) {
				/* error */
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
		}
#endif

		if (res->ai_family == AF_INET6) {
			rc = xlio_socket_setsockopt(tqpair->xlio_sock, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				/* error */
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
		}

		if (opts->ack_timeout) {
#if defined(__linux__)
			int to;

			to = opts->ack_timeout;
			rc = xlio_socket_setsockopt(tqpair->xlio_sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to));
			if (rc != 0) {
				/* error */
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
#else
			SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
		}

		if (vlan_tag != 0) {
			rc = xlio_socket_setsockopt(tqpair->xlio_sock, SOL_SOCKET, SO_XLIO_EXT_VLAN_TAG, &vlan_tag,
						    sizeof vlan_tag);
			if (rc != 0) {
				/* error */
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
		}

		src_addr = SPDK_GET_FIELD(opts, src_addr, NULL, opts->opts_size);
		src_port = SPDK_GET_FIELD(opts, src_port, 0, opts->opts_size);
		if (src_addr != NULL || src_port != 0) {
			rc = xlio_bind_client_socket(tqpair, src_addr, src_port);
			if (rc != 0) {
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
		}

		rc = xlio_socket_connect(tqpair->xlio_sock, res->ai_addr, res->ai_addrlen);
		if (rc != 0) {
			if (rc != EAGAIN && rc != EWOULDBLOCK && errno != EINPROGRESS) {
				SPDK_ERRLOG("connect() failed, rc %d, errno = %d\n", rc, errno);
				/* try next family */
				if (!xlio_socket_destroy(tqpair->xlio_sock)) {
					assert(false);
				}
				continue;
			}
			rc = 0;
		}

		break;
	}
	xlio_freeaddrinfo(res0);

	if (rc) {
		tqpair->xlio_sock = 0;
		return -EINVAL;
	}

	rc = xlio_sock_alloc(tqpair, xlio_opts);
	if (rc) {
		SPDK_ERRLOG("tqpair allocation failed\n");
		if (!xlio_socket_destroy(tqpair->xlio_sock)) {
			assert(false);
		}
		tqpair->xlio_sock = 0;
		return rc;
	}

	tqpair->flags.closed = 0;
	SPDK_NOTICELOG("Created socket: tqpair %p, qid %u, xlio_sock %lx, pd %p, context %p, dev %s, handle %u\n",
		       tqpair, tqpair->qpair.id, tqpair->xlio_sock, tqpair->pd,
		       tqpair->pd ? tqpair->pd->context : NULL,
		       tqpair->pd ? tqpair->pd->context->device->name : "unknown",
		       tqpair->pd ? tqpair->pd->handle : 0);

	return 0;
}


static void xlio_sock_free_packet(struct nvme_tcp_qpair *tqpair, struct xlio_sock_packet *packet);

/* Do not call this function, if part of data in received_packets list is using (i.e., is consumed),
 * that is, the data has been read and not freed yet.
 */
static void
nvme_qpair_disconnect_release_xlio_packets(struct nvme_tcp_qpair *tqpair)
{
	while (!STAILQ_EMPTY(&tqpair->received_packets)) {
		struct xlio_sock_packet *packet = STAILQ_FIRST(&tqpair->received_packets);

		STAILQ_REMOVE_HEAD(&tqpair->received_packets, link);
		if (--packet->refs == 0) {
			xlio_sock_free_packet(tqpair, packet);
		} else {
			SPDK_ERRLOG("Release packet with non zero refs %u, tqpair %p\n",
				    packet->refs, tqpair);
		}
	}
}

static xlio_poll_group_t
xlio_sock_get_admin_group(void)
{
	pthread_mutex_lock(&g_xlio_admin_group_mutex);
	if (!g_xlio_admin_group.refs) {
		int rc = xlio_sock_group_create(&g_xlio_admin_group.xlio_group,
						XLIO_GROUP_FLAG_SAFE);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to create admin group, rc %d errno %d: %s\n",
				    rc, errno, spdk_strerror(errno));
			pthread_mutex_unlock(&g_xlio_admin_group_mutex);
			return 0;
		}
	}
	g_xlio_admin_group.refs++;
	pthread_mutex_unlock(&g_xlio_admin_group_mutex);

	return g_xlio_admin_group.xlio_group;
}

static void
xlio_sock_put_admin_group(void)
{
	pthread_mutex_lock(&g_xlio_admin_group_mutex);
	if (--g_xlio_admin_group.refs == 0) {
		xlio_poll_group_destroy(g_xlio_admin_group.xlio_group);
	}
	pthread_mutex_unlock(&g_xlio_admin_group_mutex);
}

static int
xlio_sock_close(struct nvme_tcp_qpair *tqpair)
{
	int rc, shared_stats;
	assert(tqpair->consumed_packets == 0);
	assert(tqpair->zcopy_tx_pdus == 0);
	assert(tqpair->flags.closed == 0);
	assert(!nvme_qpair_is_admin_queue(&tqpair->qpair) || spdk_spin_held(&g_xlio_admin_group_lock));

	/* Mark socket as closed because xlio_socket_destroy() may generate CLOSED event.
	 * We don't want to start another disconnect procedure in xlio_sock_event_cb().
	 */
	tqpair->flags.closed = 1;

	if (tqpair->xlio_sock != 0) {
		rc = xlio_socket_destroy(tqpair->xlio_sock);
		if (rc) {
			SPDK_WARNLOG("Fail to destroy socket 0x%lx, rc %d\n", tqpair->xlio_sock, rc);
			tqpair->flags.closed = 0;
			return rc;
		}
		SPDK_INFOLOG(nvme_xlio, "tqpair %p socket 0x%lx is destroyed\n",
			     tqpair, tqpair->xlio_sock);

		/* Destroy may be asynchronous — events may still fire until
		 * XLIO_SOCKET_EVENT_TERMINATED.  If TERMINATED was already
		 * delivered synchronously from destroy, the callback cleared
		 * xlio_sock and the socket object may be gone.  Otherwise
		 * clear userdata so late callbacks see tqpair==NULL and
		 * return early.
		 */
		if (tqpair->xlio_sock != 0) {
			rc = xlio_socket_update(tqpair->xlio_sock, 0, (uintptr_t)NULL);
			if (rc) {
				SPDK_WARNLOG("Fail to update socket 0x%lx, rc %d\n",
					     tqpair->xlio_sock, rc);
			}
		}
	}

	if (nvme_qpair_is_admin_queue(&tqpair->qpair)) {
		xlio_sock_put_admin_group();
	}

	shared_stats = tqpair->flags.shared_stats;
	tqpair->flags_raw = 0;
	tqpair->flags.shared_stats = shared_stats;
	tqpair->flags.closed = 1;
	tqpair->xlio_sock = 0;

	xlio_sock_put_xlio_pools(tqpair);

	return 0;
}

static struct xlio_sock_packet *
xlio_sock_get_packet(struct nvme_tcp_qpair *tqpair)
{
	struct xlio_sock_packet *packet = STAILQ_FIRST(&tqpair->xlio_packets_pool->free_packets);
	static bool not_enough_packets = false;

	assert(!nvme_qpair_is_admin_queue(&tqpair->qpair) || spdk_spin_held(&g_xlio_admin_group_lock));

	if (spdk_likely(packet)) {
		STAILQ_REMOVE_HEAD(&tqpair->xlio_packets_pool->free_packets, link);
	} else {
		packet = (struct xlio_sock_packet *)malloc(sizeof(struct xlio_sock_packet));
		assert(packet);
		packet->from_malloc = true;
		if (spdk_unlikely(!not_enough_packets)) {
			SPDK_WARNLOG("Not enough xlio packets, using dynamic allocation."
				     " Performance may be degraded\n");
			not_enough_packets = true;
		}
	}

	return packet;
}

static void
xlio_sock_free_packet(struct nvme_tcp_qpair *tqpair, struct xlio_sock_packet *packet)
{
	SPDK_DEBUGLOG(nvme_xlio, "tqpair %p xlio_sock 0x%lx: free xlio buf %p\n",
		      tqpair, tqpair->xlio_sock, packet->xlio_buf);
	assert(!nvme_qpair_is_admin_queue(&tqpair->qpair) || spdk_spin_held(&g_xlio_admin_group_lock));
	assert(packet->refs == 0);
	xlio_socket_buf_free(tqpair->xlio_sock, packet->xlio_buf);

	if (spdk_likely(!packet->from_malloc)) {
		STAILQ_INSERT_HEAD(&tqpair->xlio_packets_pool->free_packets, packet, link);
	} else {
		free(packet);
	}
}

static void
xlio_sock_free_consumed_packet(struct nvme_tcp_qpair *tqpair, struct xlio_sock_packet *packet)
{
	xlio_sock_free_packet(tqpair, packet);

	assert(tqpair->consumed_packets > 0);
	tqpair->consumed_packets--;
}

static void
packets_advance(struct nvme_tcp_qpair *tqpair, size_t len)
{
	SPDK_DEBUGLOG(nvme_xlio, "tqpair %p xlio_sock 0x%lx: advance packets by %lu bytes\n",
		      tqpair, tqpair->xlio_sock, len);
	while (len > 0) {
		struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&tqpair->received_packets);
		/* We don't allow to advance by more than we have data in packets */
		assert(cur_packet != NULL);
		size_t remaining_buf_len = cur_packet->iov.iov_len - tqpair->cur_offset;

		if (len < remaining_buf_len) {
			tqpair->cur_offset += len;
			len = 0;
		} else {
			len -= remaining_buf_len;

			/* Next packet */
			tqpair->cur_offset = 0;
			STAILQ_REMOVE_HEAD(&tqpair->received_packets, link);
			tqpair->consumed_packets++;
			if (--cur_packet->refs == 0) {
				xlio_sock_free_consumed_packet(tqpair, cur_packet);
			}
		}
	}

	assert(len == 0);
}

static size_t
packets_next_chunk(struct nvme_tcp_qpair *tqpair,
		   void **buf,
		   struct xlio_sock_packet **packet,
		   size_t max_len)
{
	struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&tqpair->received_packets);

	while (cur_packet) {
		size_t len = cur_packet->iov.iov_len - tqpair->cur_offset;

		assert(max_len > 0);
		assert(len > 0);
		len = spdk_min(len, max_len);
		*buf = (uint8_t *)cur_packet->iov.iov_base + tqpair->cur_offset;
		*packet = cur_packet;
		return len;
	}

	return 0;
}

static int
poll_admin_socket(struct nvme_tcp_qpair *tqpair)
{
	assert(nvme_qpair_is_admin_queue(&tqpair->qpair));

	if (!spdk_spin_held(&g_xlio_admin_group_lock)) {
		SPDK_ERRLOG("qpair %p sock %"PRIx64" cb out of lock\n", tqpair, (uint64_t)tqpair->xlio_sock);
		/* Function below may print just addresses in shared build, but still better than nothing */
		spdk_print_backtrace(SPDK_LOG_ERROR);
		assert(0);
	}

	xlio_poll_group_poll(g_xlio_admin_group.xlio_group);

	if (STAILQ_EMPTY(&tqpair->received_packets)) {
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

static ssize_t
xlio_sock_readv(struct nvme_tcp_qpair *tqpair, struct iovec *iovs, int iovcnt)
{
	int ret, i;
	size_t offset = 0;

	if (STAILQ_EMPTY(&tqpair->received_packets)) {
		if (spdk_unlikely(nvme_qpair_is_admin_queue(&tqpair->qpair))) {
			ret = poll_admin_socket(tqpair);
			if (ret < 0) {
				if (tqpair->flags.disconnected) {
					return 0;
				}
				return ret;
			}
		} else {
			/* @todo: should we try to poll here? */
			if (tqpair->flags.disconnected) {
				return 0;
			}
			errno = EAGAIN;
			return -1;
		}
	}

	assert(!STAILQ_EMPTY(&tqpair->received_packets));
	ret = 0;
	i = 0;
	while (i < iovcnt) {
		void *buf;
		size_t len;
		struct iovec *iov = &iovs[i];
		size_t iov_len = iov->iov_len - offset;
		struct xlio_sock_packet *packet;

		len = packets_next_chunk(tqpair, &buf, &packet, iov_len);
		if (len == 0) {
			/* No more data */
			SPDK_DEBUGLOG(nvme_xlio, "tqpair %p: readv_wrapper ret %d\n", tqpair, ret);
			return ret;
		}

		memcpy((uint8_t *)iov->iov_base + offset, buf, len);
		packets_advance(tqpair, len);
		ret += len;
		offset += len;
		assert(offset <= iov->iov_len);
		if (offset == iov->iov_len) {
			offset = 0;
			i++;
		}
	}

	SPDK_DEBUGLOG(nvme_xlio, "tqpair %p: readv_wrapper ret %d\n", tqpair, ret);

	return ret;

}

static void
xlio_socket_event_cb(xlio_socket_t sock, uintptr_t userdata_sq, int event, int value)
{
	struct nvme_tcp_qpair *tqpair = (struct nvme_tcp_qpair *)userdata_sq;

	if (spdk_unlikely(!tqpair)) {
		SPDK_INFOLOG(nvme_xlio, "tqpair is NULL: sock=0x%lx event=%d value=%d\n",
			     sock, event, value);
		return;
	}

	switch (event) {
	case XLIO_SOCKET_EVENT_ESTABLISHED:
		SPDK_INFOLOG(nvme_xlio, "Connection established: sock=0x%lx tqpair=%p qid=%u event=%d value=%d\n",
			     sock, tqpair, tqpair->qpair.id, event, value);
		if (!tqpair->flags.connect_notified) {
			nvme_tcp_qpair_connect_sock_done(tqpair, 0);
			tqpair->flags.connect_notified = 1;
		}
		break;
	case XLIO_SOCKET_EVENT_TERMINATED:
		tqpair->xlio_sock = 0;
	/* fallthrough */
	case XLIO_SOCKET_EVENT_CLOSED:
	case XLIO_SOCKET_EVENT_ERROR:
		SPDK_INFOLOG(nvme_xlio,
			     "Connection closed passively: sock=0x%lx tqpair=%p qid=%u event=%d value=%d\n",
			     sock, tqpair, tqpair->qpair.id, event, value);
		if (!tqpair->flags.closed) {
			tqpair->flags.disconnected = true;
		}

		if (!tqpair->flags.connect_notified) {
			nvme_tcp_qpair_connect_sock_done(tqpair, -1);
			tqpair->flags.connect_notified = 1;
		}

		if (spdk_likely(tqpair->group) && !tqpair->flags.closed && !tqpair->flags.pending_events) {
			/* "disconnected" flag is only checked when reading pdu. We may have nothing to read.
			 * So, add qpair to pending events list to quickly detect disconnect.
			 */
			struct nvme_tcp_poll_group *group = tqpair->group;

			tqpair->flags.pending_events = true;
			TAILQ_INSERT_TAIL(&group->pending_events, tqpair, link);
		}
		break;
	default:
		SPDK_ERRLOG("Unknown Event callback: sock=0x%lx tqpair=%p qid=%u event=%d value=%d\n",
			    sock, tqpair, tqpair->qpair.id, event, value);
		break;
	}
}

static void
xlio_socket_comp_cb(xlio_socket_t sock, uintptr_t userdata_sq, uintptr_t userdata_op)
{
	struct nvme_tcp_qpair *tqpair = (struct nvme_tcp_qpair *)userdata_sq;
	struct nvme_tcp_pdu *pdu = (struct nvme_tcp_pdu *)userdata_op;

	assert(userdata_sq != 0);
	assert(userdata_op != 0);

	SPDK_DEBUGLOG(nvme_xlio, "Completed zcopy buffer userdata_sq=%lx userdata_op=%lx.\n",
		      userdata_sq, userdata_op);

	assert(tqpair->zcopy_tx_pdus > 0);
	tqpair->zcopy_tx_pdus--;
	_pdu_write_done(tqpair, pdu, 0);
}

static void
xlio_socket_rx_cb(xlio_socket_t sock, uintptr_t userdata_sq, void *data, size_t len,
		  struct xlio_buf *buf)
{
	assert(userdata_sq != 0);
	struct nvme_tcp_qpair *tqpair = (struct nvme_tcp_qpair *)userdata_sq;
	struct xlio_sock_packet *packet = xlio_sock_get_packet(tqpair);

	SPDK_DEBUGLOG(nvme_xlio, "zcopy buffer userdata_sq=%lx data %p len %lu buf %p\n",
		      userdata_sq, data, len, buf);
	packet->xlio_buf = buf;
	packet->iov.iov_base = data;
	assert(len != 0);
	packet->iov.iov_len = len;
	if (spdk_unlikely(tqpair->flags.stop_receiving)) {
		xlio_sock_free_packet(tqpair, packet);
	} else {
		packet->refs = 1;
		STAILQ_INSERT_TAIL(&tqpair->received_packets, packet, link);

		/* If the socket does not already have recv pending, add it now */
		if (spdk_likely(tqpair->group) && !tqpair->flags.pending_events) {
			struct nvme_tcp_poll_group *group = tqpair->group;

			tqpair->flags.pending_events = true;
			TAILQ_INSERT_TAIL(&group->pending_events, tqpair, link);
		}
	}
}

static void
xlio_socket_event_cb_safe(xlio_socket_t sock, uintptr_t userdata_sq, int event, int value)
{
	spdk_spin_lock(&g_xlio_admin_group_lock);
	xlio_socket_event_cb(sock, userdata_sq, event, value);
	spdk_spin_unlock(&g_xlio_admin_group_lock);
}

static void
xlio_socket_comp_cb_safe(xlio_socket_t sock, uintptr_t userdata_sq, uintptr_t userdata_op)
{
	spdk_spin_lock(&g_xlio_admin_group_lock);
	xlio_socket_comp_cb(sock, userdata_sq, userdata_op);
	spdk_spin_unlock(&g_xlio_admin_group_lock);
}

static void
xlio_socket_rx_cb_safe(xlio_socket_t sock, uintptr_t userdata_sq, void *data, size_t len,
		       struct xlio_buf *buf)
{
	spdk_spin_lock(&g_xlio_admin_group_lock);
	xlio_socket_rx_cb(sock, userdata_sq, data, len, buf);
	spdk_spin_unlock(&g_xlio_admin_group_lock);
}

static void
xlio_batch_pp_handler(void *ctx)
{
	struct nvme_tcp_poll_group *group = (struct nvme_tcp_poll_group *)ctx;

	xlio_poll_group_flush(group->xgroup);

	group->flags.pp_handler_registered = false;
}

static inline struct nvme_tcp_poll_group *
nvme_tcp_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return SPDK_CONTAINEROF(group, struct nvme_tcp_poll_group, group);
}

static int
nvme_tcp_qpair_send_pdu(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	struct xlio_socket_send_attr attr = {};
	int i, rc;

	if (pdu->data_len < DEFAULT_ZCOPY_THRESHOLD || !pdu->has_mkeys) {
		attr.flags |= XLIO_SOCKET_SEND_FLAG_INLINE;
	}

	if (pdu->has_capsule) {
		attr.mkey = tqpair->pdus_mkey;
		attr.userdata_op = pdu->data_len > 0 ? 0 : (uintptr_t)pdu;
		rc = xlio_socket_send(tqpair->xlio_sock,
				      pdu->hdr.raw,
				      pdu->capsule_len,
				      &attr);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to submit %d, errno %d, tqpair %p, sock closed %d\n",
				    rc, errno, tqpair, tqpair->flags.closed);
			return rc;
		}
	}

	if (pdu->data_len) {
		if (attr.flags & XLIO_SOCKET_SEND_FLAG_INLINE) {
			rc = xlio_socket_sendv(tqpair->xlio_sock, pdu->iovs, pdu->data_iovcnt, &attr);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to submit %d, errno %d, tqpair %p, sock closed %d\n",
					    rc, errno, tqpair, tqpair->flags.closed);
				return rc;
			}
		} else {
			/* TODO: If mkeys are same, xlio_socket_sendv can be used */
			for (i = 0; i < pdu->data_iovcnt; i++) {
				attr.mkey = pdu->mkeys[i];
				attr.userdata_op = !pdu->ddgst_enable && (i == pdu->data_iovcnt - 1) ? (uintptr_t)pdu : 0;

				rc = xlio_socket_send(tqpair->xlio_sock,
						      pdu->iovs[i].iov_base,
						      pdu->iovs[i].iov_len,
						      &attr);
				if (rc < 0) {
					SPDK_ERRLOG("Failed to submit %d, errno %d, tqpair %p, sock closed %d\n",
						    rc, errno, tqpair, tqpair->flags.closed);
					return rc;
				}
			}
		}

		if (pdu->ddgst_enable) {
			attr.mkey = tqpair->pdus_mkey;
			attr.userdata_op = (uintptr_t)pdu;
			rc = xlio_socket_send(tqpair->xlio_sock,
					      pdu->data_digest,
					      sizeof(pdu->data_digest),
					      &attr);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to submit %d, errno %d, tqpair %p, sock closed %d\n",
					    rc, errno, tqpair, tqpair->flags.closed);
				return rc;
			}
		}
	}

	if (attr.flags & XLIO_SOCKET_SEND_FLAG_INLINE) {
		_pdu_write_done(tqpair, pdu, 0);
	} else {
		tqpair->zcopy_tx_pdus++;
	}

	if (group && !group->flags.pp_handler_registered) {
		spdk_thread_register_post_poller_handler(xlio_batch_pp_handler, group);
		group->flags.pp_handler_registered = true;
	} else if (!group) {
		tqpair->flags.pending_send = true;
	}

	return 0;
}

static int
xlio_sock_group_create(xlio_poll_group_t *group, unsigned int flags)
{
	int rc;
	struct xlio_poll_group_attr gattr = {};

	gattr.flags = flags;

	if (flags & XLIO_GROUP_FLAG_SAFE) {
		gattr.socket_event_cb = xlio_socket_event_cb_safe;
		gattr.socket_comp_cb = xlio_socket_comp_cb_safe;
		gattr.socket_rx_cb = xlio_socket_rx_cb_safe;
		if (!g_xlio_admin_group_lock_initialized) {
			spdk_spin_init_recursive(&g_xlio_admin_group_lock);
			g_xlio_admin_group_lock_initialized = true;
		}
	} else {
		gattr.socket_event_cb = xlio_socket_event_cb;
		gattr.socket_comp_cb = xlio_socket_comp_cb;
		gattr.socket_rx_cb = xlio_socket_rx_cb;
	}

	rc = xlio_poll_group_create(&gattr, group);
	if (rc) {
		SPDK_ERRLOG("Failed to create group.\n");
	}

	return rc;
}

static inline int
xlio_sock_group_impl_close(struct nvme_tcp_poll_group *group)
{
	int rc = xlio_poll_group_destroy(group->xgroup);
	if (rc) {
		SPDK_ERRLOG("Failed to destroy group: rc %d errno %d (%s)\n",
			    rc, errno, spdk_strerror(errno));
		assert(false);
		return -1;
	}

	return 0;
}

static int
xlio_sock_poll_group_create(struct nvme_tcp_poll_group *group)
{
	size_t impl_opts_size = sizeof(group->impl_opts);
	uint32_t num_buffers;
	int rc;

	assert(group);
	rc = xlio_sock_group_create(&group->xgroup, XLIO_GROUP_FLAG_DIRTY);
	if (rc) {
		SPDK_ERRLOG("Failed to create group.\n");
		return rc;
	}

	rc = spdk_sock_impl_get_opts("xlio", &group->impl_opts, &impl_opts_size);
	if (rc) {
		goto err_close_group;
	}

	num_buffers = group->impl_opts.buffers_pool_size;

	TAILQ_INIT(&group->pending_events);

	if (num_buffers != 0) {
		pthread_mutex_lock(&g_xlio_pool_mutex);
		rc = xlio_sock_get_buffers_pool_unsafe(num_buffers);
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to allocated buffers pool for group %p\n", group);
			goto err_close_group;
		}
	}

	return 0;

err_close_group:
	xlio_sock_group_impl_close(group);
	return rc;
}

static int
xlio_sock_group_impl_poll(struct nvme_tcp_poll_group *group, int max_events)
{
	int num_events;
	struct nvme_tcp_qpair *tqpair, *tmp_tqpair;

	xlio_poll_group_poll(group->xgroup);

	num_events = 0;
	tmp_tqpair = TAILQ_LAST(&group->pending_events, pending_recv_head);
	tqpair = NULL;
	while (tqpair != tmp_tqpair && num_events < MAX_EVENTS_PER_POLL) {
		tqpair = TAILQ_FIRST(&group->pending_events);

		TAILQ_REMOVE(&group->pending_events, tqpair, link);
		tqpair->flags.pending_events = false;

		if (spdk_unlikely(tqpair->flags.closed)) {
			continue;
		}

		nvme_tcp_qpair_process_pending_events(tqpair);
		num_events++;
	}

	return num_events;
}

static ssize_t
xlio_sock_recv_zcopy(struct nvme_tcp_qpair *tqpair, size_t len, struct xlio_sock_buf **sock_buf)
{
	struct nvme_tcp_poll_group *group = tqpair->group;
	struct xlio_sock_buf *prev_buf = NULL;
	int ret;

	SPDK_DEBUGLOG(nvme_xlio, "tqpair %p: zcopy recv %lu bytes\n", tqpair, len);
	*sock_buf = NULL;

	if (STAILQ_EMPTY(&tqpair->received_packets)) {
		if (spdk_unlikely(nvme_qpair_is_admin_queue(&tqpair->qpair))) {
			ret = poll_admin_socket(tqpair);
			if (ret < 0) {
				if (tqpair->flags.disconnected) {
					return 0;
				}
				return ret;
			}
		} else {
			if (tqpair->flags.disconnected) {
				return 0;
			}
			errno = EAGAIN;
			return -1;
		}
	}

	assert(!STAILQ_EMPTY(&tqpair->received_packets));
	ret = 0;
	while (len > 0) {
		void *data;
		size_t chunk_len;
		struct xlio_sock_buf *buf;
		struct xlio_sock_packet *packet;

		chunk_len = packets_next_chunk(tqpair, &data, &packet, len);
		if (chunk_len == 0) {
			/* No more data */
			break;
		}

		assert(chunk_len <= len);
		buf = spdk_mempool_get(g_xlio_nvme_buffers_pool);
		if (spdk_unlikely(!buf)) {
			SPDK_DEBUGLOG(nvme_xlio, "tqpair %p: no more buffers, total_len %d\n", tqpair, ret);
			if (spdk_unlikely(group && !tqpair->flags.pending_events)) {
				tqpair->flags.pending_events = true;
				SPDK_DEBUGLOG(nvme_xlio, "tqpair %p, insert to pending_recv\n", tqpair);
				TAILQ_INSERT_TAIL(&group->pending_events, tqpair, link);
			}
			if (ret == 0) {
				ret = -1;
				errno = EAGAIN;
			}
			break;
		}

		buf->iov.iov_base = data;
		buf->iov.iov_len = chunk_len;
		buf->next = NULL;
		buf->packet = packet;
		packet->refs++;
		if (prev_buf) {
			prev_buf->next = buf;
		} else {
			*sock_buf = buf;
		}

		packets_advance(tqpair, chunk_len);
		len -= chunk_len;
		ret += chunk_len;
		prev_buf = buf;
		SPDK_DEBUGLOG(nvme_xlio, "tqpair %p: add buffer %p, len %lu, total_len %d\n",
			      tqpair, buf, buf->iov.iov_len, ret);
	}

	SPDK_DEBUGLOG(nvme_xlio, "tqpair %p: recv_zcopy ret %d\n", tqpair, ret);
	return ret;
}

#define BUFS_BULK_SIZE 32

static int
xlio_sock_free_bufs(struct nvme_tcp_qpair *tqpair, struct xlio_sock_buf *sock_buf)
{
	void *bufs_bulk[BUFS_BULK_SIZE];
	uint32_t bufs_count = 0;

	while (sock_buf) {
		struct xlio_sock_packet *packet = sock_buf->packet;
		struct xlio_sock_buf *next = sock_buf->next;

		bufs_bulk[bufs_count++] = (void *)sock_buf;
		if (bufs_count == BUFS_BULK_SIZE) {
			spdk_mempool_put_bulk(g_xlio_nvme_buffers_pool, bufs_bulk, bufs_count);
			bufs_count = 0;
		}
		if (--packet->refs == 0) {
			xlio_sock_free_consumed_packet(tqpair, packet);
		}

		sock_buf = next;
	}

	if (bufs_count) {
		spdk_mempool_put_bulk(g_xlio_nvme_buffers_pool, bufs_bulk, bufs_count);
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(nvme_xlio)


/*
 * NVMe/NVDA_TCP transport
 */

#define NVME_TCP_RW_BUFFER_SIZE 131072
#define NVME_TCP_TIME_OUT_IN_SECONDS 2

#define NVME_TCP_HPDA_DEFAULT			0
#define NVME_TCP_PDU_H2C_MIN_DATA_SIZE		4096

/*
 * Maximum value of transport_ack_timeout used by TCP controller
 */
#define NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT	31

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
};

static struct spdk_nvme_tcp_stat g_dummy_stats = {};

static void nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req);
static int64_t nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group
		*tgroup, uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
static void nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				  struct spdk_nvme_cpl *rsp, bool print_on_error);
static struct nvme_tcp_req *get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid);
static int nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_req *tcp_req);
static bool nvme_tcp_memory_domain_enabled(void);

static inline bool
nvme_tcp_req_with_memory_domain(struct nvme_tcp_req *tcp_req)
{
	return tcp_req && (tcp_req->req.accel_sequence || tcp_req->ordering.bits.has_memory_domain);
}

static inline struct nvme_tcp_req *
nvme_tcp_req(struct nvme_request *req)
{
	return SPDK_CONTAINEROF(req, struct nvme_tcp_req, req);
}

static inline struct nvme_tcp_qpair *
nvme_tcp_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_CUSTOM_FABRICS);
	return SPDK_CONTAINEROF(qpair, struct nvme_tcp_qpair, qpair);
}

static inline struct nvme_tcp_ctrlr *
nvme_tcp_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_CUSTOM_FABRICS);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
}

static int
nvme_tcp_req_get(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	uint32_t cid;

	if (tqpair->flags.use_poll_group_req_pool) {
		cid = spdk_bit_pool_allocate_bit(tqpair->cid_pool);
		if (cid == UINT32_MAX) {
			return -EAGAIN;
		}

		tcp_req->cid = cid;
	}

	tqpair->tcp_reqs_lookup[tcp_req->cid] = tcp_req;

	tcp_req->datao = 0;
	tcp_req->expected_datao = 0;
	tcp_req->r2tl_remain = 0;
	tcp_req->r2tl_remain_next = 0;
	tcp_req->active_r2ts = 0;
	memset(&tcp_req->req.cpl, 0, sizeof(tcp_req->req.cpl));
	tcp_req->pdu.iovcnt = 0;
	tcp_req->pdu.data_iovcnt = 0;
	assert(tcp_req->ordering.bits.state == NVME_TCP_REQ_FREE);
	tcp_req->ordering.raw = 0;
	tcp_req->ordering.bits.state = NVME_TCP_REQ_ACTIVE;
	tcp_req->pdu.data_len = 0;
	tcp_req->pdu.has_mkeys = 0;
	tcp_req->pdu.has_capsule = 0;
	tcp_req->pdu.ddgst_enable = 0;
	tcp_req->iobuf_iov.iov_base = NULL;
	tcp_req->sock_buf = NULL;
	tcp_req->recv_iovs = tcp_req->_recv_iovs;
	tcp_req->recv_iovcnt = 0;

	return 0;
}

static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;

	assert(tcp_req->ordering.bits.state != NVME_TCP_REQ_FREE);
	tcp_req->ordering.bits.state = NVME_TCP_REQ_FREE;

	tqpair->tcp_reqs_lookup[tcp_req->cid] = NULL;

	if (group && tcp_req->iobuf_iov.iov_base) {
		spdk_iobuf_put(group->group->accel_fn_table.get_iobuf_channel(group->group->ctx),
			       tcp_req->iobuf_iov.iov_base,
			       tcp_req->iobuf_iov.iov_len);

		tcp_req->iobuf_iov.iov_base = NULL;
	}

	if (tcp_req->recv_iovs != tcp_req->_recv_iovs &&
	    tcp_req->recv_iovs != NULL) {
		if (tcp_req->ordering.bits.recv_iovs_malloced) {
			free(tcp_req->recv_iovs);
			tcp_req->ordering.bits.recv_iovs_malloced = 0;
		} else {
			spdk_mempool_put(g_iovec_pool, tcp_req->recv_iovs);
		}
		tcp_req->recv_iovs = NULL;
	}

	if (tqpair->flags.use_poll_group_req_pool) {
		spdk_bit_pool_free_bit(tqpair->cid_pool, tcp_req->cid);
		tcp_req->cid = UINT16_MAX;
	}
}

static struct nvme_tcp_pdu *
nvme_tcp_recv_pdu_get(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(group);

	if (spdk_likely(group && tgroup->recv_pdus)) {
		pdu = TAILQ_FIRST(&tgroup->free_pdus);
		if (!pdu) {
			return NULL;
		}

		TAILQ_REMOVE(&tgroup->free_pdus, pdu, tailq);
	} else {
		pdu = tqpair->_recv_pdu;
	}

	return pdu;
}

static void
nvme_tcp_recv_pdu_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(group);

	if (spdk_likely(group && tgroup->recv_pdus)) {
		TAILQ_INSERT_HEAD(&tgroup->free_pdus, pdu, tailq);
		tqpair->recv_pdu = NULL;
	}
}

static inline void
nvme_tcp_qpair_set_recv_state(struct nvme_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (spdk_unlikely(tqpair->recv_state == state)) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	if (state == NVME_TCP_PDU_RECV_STATE_ERROR) {
		assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));
	}

	tqpair->recv_state = state;
	if ((state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY ||
	     state == NVME_TCP_PDU_RECV_STATE_ERROR) && tqpair->recv_pdu) {
		nvme_tcp_recv_pdu_put(tqpair, tqpair->recv_pdu);
	}
}

static inline void
nvme_tcp_qpair_set_recv_quiescing(struct nvme_tcp_qpair *tqpair)
{
	if (tqpair->recv_state != NVME_TCP_PDU_RECV_STATE_QUIESCING) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	} else {
		SPDK_DEBUGLOG(nvme, "tqpair %p recv state is already QUIESCING\n", tqpair);
	}
}

static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	spdk_free(tqpair->tcp_reqs);
	tqpair->tcp_reqs = NULL;
	spdk_free(tqpair->reserved_tcp_req);
	tqpair->reserved_tcp_req = NULL;

	spdk_free(tqpair->send_pdu);
	tqpair->send_pdu = NULL;
	spdk_free(tqpair->_recv_pdu);
	tqpair->_recv_pdu = NULL;

	free(tqpair->tcp_reqs_lookup);
	spdk_bit_pool_free(&tqpair->cid_pool);
}

static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	size_t req_size_padded;
	uint16_t i;
	struct nvme_tcp_req	*tcp_req;

	req_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_req), 64);

	tqpair->tcp_reqs = spdk_zmalloc(tqpair->num_entries * req_size_padded, 64, NULL,
					SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->tcp_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate tcp_reqs on tqpair=%p\n", tqpair);
		goto fail;
	}

	tqpair->reserved_tcp_req = spdk_zmalloc(req_size_padded, 64, NULL,
						SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->reserved_tcp_req == NULL) {
		goto fail;
	}

	tqpair->send_pdu = spdk_zmalloc(sizeof(struct nvme_tcp_pdu), 0x1000, NULL,
					SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->send_pdu == NULL) {
		goto fail;
	}

	tqpair->_recv_pdu = spdk_zmalloc(sizeof(struct nvme_tcp_pdu), 0x1000, NULL,
					 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (tqpair->_recv_pdu == NULL) {
		goto fail;
	}

	tqpair->cid_pool = spdk_bit_pool_create(tqpair->num_entries);
	tqpair->tcp_reqs_lookup = calloc(tqpair->num_entries, sizeof(*tqpair->tcp_reqs_lookup));
	TAILQ_INIT(&tqpair->outstanding_reqs);
	TAILQ_INIT(&tqpair->accel_nomem_queue);
	for (i = 0; i < tqpair->num_entries; i++) {
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;
		tcp_req->req.qpair = &tqpair->qpair;
		STAILQ_INSERT_HEAD(&tqpair->qpair.free_req, &tcp_req->req, stailq);
	}

	tcp_req = tqpair->reserved_tcp_req;
	tcp_req->req.qpair = &tqpair->qpair;

	tqpair->qpair.reserved_req = &tcp_req->req;
	tqpair->qpair.active_free_req = &tqpair->qpair.free_req;

	return 0;
fail:
	nvme_tcp_free_reqs(tqpair);
	return -ENOMEM;
}

static void nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static void
nvme_tcp_qpair_print_reqs_info(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_req *tcp_req;
	uint32_t reqs = 0;
	uint32_t in_accel = 0;
	uint64_t now = spdk_get_ticks();
	uint64_t ticks_hz = spdk_get_ticks_hz();
	uint64_t max_age_tsc = 0;

	TAILQ_FOREACH(tcp_req, &tqpair->outstanding_reqs, link) {
		/* Submit tick is only set if timeout callback is registered for controller.
		 * If timeout is not enabled, submit_tick is zero.
		 */
		uint64_t age_tsc = now - (tcp_req->req.submit_tick ? tcp_req->req.submit_tick : now);

		SPDK_INFOLOG(nvme, "Outstanding req: tqpair %p, qid %u, sock 0x%lx, tid %lu, req %p, "
			     "ordering 0x%x, cb_arg %p, age_ms %lu\n",
			     tqpair, tqpair->qpair.id, tqpair->xlio_sock,
			     spdk_thread_get_id(spdk_get_thread()), tcp_req, tcp_req->ordering.raw,
			     tcp_req->req.cb_arg, age_tsc * 1000 / ticks_hz);
		max_age_tsc = spdk_max(max_age_tsc, age_tsc);
		reqs++;
		if (tcp_req->ordering.bits.in_progress_accel) {
			in_accel++;
		}
	}

	SPDK_NOTICELOG("tqpair %p, qid %u, sock 0x%lx, tid %lu, outstanding_reqs %u, in_accel %u, max_age_ms %lu\n",
		       tqpair, tqpair->qpair.id, tqpair->xlio_sock, spdk_thread_get_id(spdk_get_thread()),
		       reqs, in_accel, max_age_tsc * 1000 / ticks_hz);
}

static void
nvme_tcp_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_poll_group *group;
	bool need_lock = nvme_qpair_is_admin_queue(qpair);

	if (need_lock) {
		spdk_spin_lock(&g_xlio_admin_group_lock);
	}

	if (tqpair->flags.pending_events) {
		group = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE(&group->pending_events, tqpair, link);
		tqpair->flags.pending_events = false;
	}

	if (tqpair->flags.shared_stats) {
		tqpair->stats = &g_dummy_stats;
	}

	nvme_tcp_qpair_print_reqs_info(tqpair);
	tqpair->flags.stop_receiving = 1;

	if (tqpair->zcopy_tx_pdus == 0) {
		nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);
		if (tqpair->consumed_packets == 0) {
			nvme_qpair_disconnect_release_xlio_packets(tqpair);
			xlio_sock_close(tqpair);
		}
	} else {
		SPDK_DEBUGLOG(nvme,
			      "qpair %p %u: defer close, consumed_packets %u, zcopy_tx_pdus %u\n",
			      tqpair, qpair->id, tqpair->consumed_packets,
			      tqpair->zcopy_tx_pdus);
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);

	if (need_lock) {
		spdk_spin_unlock(&g_xlio_admin_group_lock);
	}
}

static int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	bool need_lock;

	assert(qpair != NULL);
	need_lock = nvme_qpair_is_admin_queue(qpair);
	if (need_lock) {
		spdk_spin_lock(&g_xlio_admin_group_lock);
	}
	tqpair = nvme_tcp_qpair(qpair);
	nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);
	assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));
	assert(tqpair->qpair.num_outstanding_reqs == 0);
	qpair->reserved_req = NULL;
	nvme_qpair_deinit(qpair);
	nvme_tcp_free_reqs(tqpair);
	if (!tqpair->flags.shared_stats) {
		free(tqpair->stats);
	}
	spdk_rdma_utils_free_mem_map(&tqpair->mem_map);
	spdk_rdma_utils_put_memory_domain(tqpair->memory_domain);
	spdk_free(tqpair);

	if (need_lock) {
		spdk_spin_unlock(&g_xlio_admin_group_lock);
	}

	return 0;
}

static int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static int
nvme_tcp_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tcp_ctrlr *tctrlr = nvme_tcp_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(tctrlr);

	return 0;
}

static void
_pdu_write_done(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu, int err)
{
	struct nvme_tcp_poll_group *pgroup;

	/* If there are queued requests, we assume they are queued because they are waiting
	 * for resources to be released. Those resources are almost certainly released in
	 * response to a PDU completing here. However, to attempt to make forward progress
	 * the qpair needs to be polled and we can't rely on another network event to make
	 * that happen. Add it to a list of qpairs to poll regardless of network activity
	 * here.
	 * Besides, when tqpair state is NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL or
	 * NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED, need to add it to pending_events list too to make
	 * forward progress in case that the resources are released after icreq's or CONNECT's
	 * resp is processed. */
	if (tqpair->qpair.poll_group && !tqpair->flags.pending_events &&
	    (!STAILQ_EMPTY(&tqpair->qpair.queued_req) ||
	     tqpair->state == NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL ||
	     tqpair->state == NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED)) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

		TAILQ_INSERT_TAIL(&pgroup->pending_events, tqpair, link);
		tqpair->flags.pending_events = true;
	}

	if (spdk_unlikely(err != 0)) {
		nvme_transport_ctrlr_disconnect_qpair(tqpair->qpair.ctrlr, &tqpair->qpair);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(tqpair, SPDK_CONTAINEROF(pdu, struct nvme_tcp_req, pdu));
}

static uint32_t
nvme_tcp_pdu_calc_data_digest_with_iov(struct nvme_tcp_pdu *pdu, struct iovec *iovs, int iovcnt)
{
	uint32_t crc32c = SPDK_CRC32C_XOR;
	uint32_t mod;

	assert(pdu->data_len != 0);

	crc32c = spdk_crc32c_iov_update(iovs, iovcnt, crc32c);
	mod = pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = SPDK_NVME_TCP_DIGEST_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
nvme_tcp_pdu_calc_data_digest_with_sock_buf(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct xlio_sock_buf *sock_buf = tcp_req->sock_buf;
	uint32_t crc32c = SPDK_CRC32C_XOR;
	uint32_t mod;

	assert(pdu->data_len != 0);
	while (sock_buf) {
		crc32c = spdk_crc32c_iov_update(&sock_buf->iov, 1, crc32c);
		sock_buf = sock_buf->next;
	}

	mod = pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = SPDK_NVME_TCP_DIGEST_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}

	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static int
nvme_tcp_qpair_write_control_pdu(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 nvme_tcp_qpair_xfer_complete_cb cb_fn)
{
	int rc;

	pdu->cb_fn = cb_fn;
	pdu->iovs = &tqpair->ctrlr_hdr_iov;
	pdu->iovs[0].iov_base = &tqpair->ctrl_hdr.raw;
	pdu->iovs[0].iov_len = tqpair->ctrl_hdr.common.plen;
	pdu->data_iovcnt = 1;
	pdu->data_len = tqpair->ctrl_hdr.common.plen;
	pdu->iovcnt = 1;
	pdu->has_capsule = 0;
	pdu->capsule_offset = 0;
	pdu->ddgst_enable = 0;
	tqpair->stats->submitted_requests++;
	rc = nvme_tcp_qpair_send_pdu(tqpair, pdu);

	return rc;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_tcp_build_contig_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;

	pdu->iovs = tcp_req->iovs;
	pdu->iovs[0].iov_base = (uint8_t *)req->payload.contig_or_cb_arg + req->payload.offset;
	pdu->iovs[0].iov_len = req->payload.size;
	pdu->data_iovcnt = 1;

	assert(nvme_req_payload_type(req) == NVME_PAYLOAD_TYPE_CONTIG);

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_tcp_build_sgl_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	int rc;
	uint32_t length, remaining_size, iovcnt = 0, max_num_sgl;
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;

	assert(req->payload.size != 0);
	assert(nvme_req_payload_type(req) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload.offset);

	max_num_sgl = spdk_min(req->qpair->ctrlr->max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS);
	remaining_size = req->payload.size;

	pdu->iovs = tcp_req->iovs;
	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &pdu->iovs[iovcnt].iov_base,
					      &length);
		if (spdk_unlikely(rc)) {
			return -1;
		}

		length = spdk_min(length, remaining_size);
		pdu->iovs[iovcnt].iov_len = length;
		remaining_size -= length;
		iovcnt++;
	} while (remaining_size > 0 && iovcnt < max_num_sgl);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (spdk_unlikely(remaining_size > 0)) {
		SPDK_ERRLOG("Failed to construct tcp_req=%p, and the iovcnt=%u, remaining_size=%u\n",
			    tcp_req, iovcnt, remaining_size);
		return -1;
	}

	pdu->data_iovcnt = iovcnt;

	return 0;
}

static inline int
nvme_tcp_build_iov_request_fill_with_offset(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_req *tcp_req, uint32_t max_num_sgl)
{
	struct spdk_iov_sgl sgl;
	uint32_t remaining_size = tcp_req->req.payload.size;
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;

	pdu->iovs = tcp_req->iovs;
	spdk_iov_sgl_init(&sgl, tcp_req->req.payload.iov, tcp_req->req.payload.iov_count, 0);
	spdk_iov_sgl_advance(&sgl, tcp_req->req.payload.offset);

	for (pdu->data_iovcnt = 0; pdu->data_iovcnt < max_num_sgl &&
	     remaining_size > 0; pdu->data_iovcnt++) {
		void *addr;
		uint32_t length;

		length = spdk_min(sgl.iov->iov_len - sgl.iov_offset, remaining_size);
		addr = (uint8_t *)sgl.iov->iov_base + sgl.iov_offset;

		pdu->iovs[pdu->data_iovcnt].iov_base = addr;
		pdu->iovs[pdu->data_iovcnt].iov_len = length;
		remaining_size -= length;
		spdk_iov_sgl_advance(&sgl, length);
	}

	if (spdk_unlikely(remaining_size > 0)) {
		return -E2BIG;
	}

	return 0;
}

static inline int
nvme_tcp_build_iov_request_fill(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req, uint32_t max_num_sgl)
{
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;

	if (spdk_unlikely(max_num_sgl > NVME_TCP_MAX_SGL_DESCRIPTORS)) {
		return -E2BIG;
	}

	pdu->iovs = tcp_req->req.payload.iov;
	pdu->data_iovcnt = tcp_req->req.payload.iov_count;

	return 0;
}

static inline int
nvme_tcp_build_iov_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	uint32_t max_num_sgl;
	struct nvme_request *req = &tcp_req->req;

	assert(req->payload.size != 0);
	assert(nvme_req_payload_type(req) == NVME_PAYLOAD_TYPE_IOV);

	max_num_sgl = spdk_min(req->qpair->ctrlr->max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS);
	max_num_sgl = spdk_min(req->payload.iov_count, max_num_sgl);

	if (req->payload.offset != 0 || req->parent != NULL) {
		return nvme_tcp_build_iov_request_fill_with_offset(tqpair, tcp_req, max_num_sgl);
	} else {
		return nvme_tcp_build_iov_request_fill(tqpair, tcp_req, max_num_sgl);
	}
}

static int
nvme_tcp_req_build(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);;
	int rc;
	enum nvme_payload_type payload_type = nvme_req_payload_type(req);
	enum spdk_nvme_data_transfer xfer;
	uint32_t max_in_capsule_data_size;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload.size;

	SPDK_DEBUGLOG(nvme, "tqpair %p %u, xlio_sock 0x%lx, tcp_req %p pdu %p, p_type %d\n",
		      tqpair,
		      tqpair->qpair.id, tqpair->xlio_sock, tcp_req, &tcp_req->pdu, payload_type);

	switch (payload_type) {
	case NVME_PAYLOAD_TYPE_CONTIG:
		rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
		break;
	case NVME_PAYLOAD_TYPE_SGL:
		rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
		break;
	case NVME_PAYLOAD_TYPE_IOV:
		rc = nvme_tcp_build_iov_request(tqpair, tcp_req);
		break;
	default:
		rc = -1;
	}

	if (spdk_unlikely(rc)) {
		return rc;
	}

	if (spdk_unlikely(req->cmd.opc == SPDK_NVME_OPC_FABRIC) ||
	    nvme_qpair_is_admin_queue(&tqpair->qpair)) {
		max_in_capsule_data_size = SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE;
		if (req->cmd.opc == SPDK_NVME_OPC_FABRIC) {
			xfer = spdk_nvme_opc_get_data_transfer(((struct spdk_nvmf_capsule_cmd *)&req->cmd)->fctype);
		} else {
			xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
		}
	} else {
		struct spdk_nvme_ctrlr *ctrlr = tqpair->qpair.ctrlr;

		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
		max_in_capsule_data_size = ctrlr->ioccsz_bytes;
	}
	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
	    req->payload.size <= max_in_capsule_data_size) {
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
		req->cmd.dptr.sgl1.address = 0;
		tcp_req->ordering.bits.in_capsule_data = true;
	}

	return 0;
}

static void
_nvme_tcp_accel_finished_in_capsule(void *cb_arg, int status)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	uint32_t *ddgst;
	struct spdk_nvme_cpl cpl;
	enum spdk_nvme_generic_command_status_code sc;
	uint16_t dnr = 0;

	SPDK_DEBUGLOG(nvme, "accel cpl, req %p, status %d\n", tcp_req, status);
	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;
	assert(tcp_req->ordering.bits.needs_accel_seq);
	tcp_req->ordering.bits.needs_accel_seq = 0;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqpair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		dnr = 1;
		goto fail_req;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
			  !spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			      tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		goto fail_req;
	}

	if (tqpair->flags.host_ddgst_enable) {
		uint32_t ddgst_tmp;

		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + tcp_req->req.payload.size);
		ddgst_tmp = *ddgst;

		ddgst_tmp ^= SPDK_CRC32C_XOR;
		MAKE_DIGEST_WORD((uint8_t *)ddgst, ddgst_tmp);
	}

	pdu->has_mkeys = 1;
	if (spdk_unlikely(nvme_tcp_fill_data_mkeys(tqpair, tcp_req, pdu) != 0)) {
		SPDK_ERRLOG("Failed to fill mkeys\n");
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}

	tqpair->stats->submitted_requests++;
	if (spdk_unlikely(nvme_tcp_qpair_send_pdu(tqpair, pdu))) {
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}

	return;

fail_req:
	memset(&cpl, 0, sizeof(cpl));
	cpl.status.sc = sc;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;
	nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
}

static inline int
nvme_tcp_handle_accel_sequence_in_capsule(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;
	struct spdk_accel_sequence *accel_seq;
	struct spdk_accel_task *task;
	uint32_t *ddgst;
	bool skip_copy = false;
	int rc;

	SPDK_DEBUGLOG(nvme, "Write request with accel sequence: tcp_req %p\n", tcp_req);
	assert(req->payload.opts);
	accel_seq = req->accel_sequence;
	if (accel_seq) {
		task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == SPDK_ACCEL_OPC_ENCRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			task->dst_domain = NULL;
			task->dst_domain_ctx = NULL;
			task->d.iovs = &tcp_req->iobuf_iov;
			task->d.iovcnt = 1;
			skip_copy = true;
		}
	}

	if (tqpair->flags.host_ddgst_enable) {
		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + req->payload.size);

		if (!skip_copy) {
			rc = group->accel_fn_table.append_copy_crc32c(group->ctx, (void **)&accel_seq, ddgst,
					&tcp_req->iobuf_iov, 1, NULL, NULL,
					pdu->iovs, pdu->data_iovcnt,
					req->payload.opts->memory_domain,
					req->payload.opts->memory_domain_ctx,
					0, NULL, NULL);
			skip_copy = true;
		} else {
			rc = group->accel_fn_table.append_crc32c(group->ctx, (void **)&accel_seq, ddgst,
					&tcp_req->iobuf_iov, 1, NULL, NULL, 0, NULL, NULL);
		}
		if (spdk_unlikely(rc)) {
			if (rc != -ENOMEM) {
				SPDK_ERRLOG("Failed to append crc32 accel task, rc %d\n", rc);
			} else {
				SPDK_DEBUGLOG(nvme, "Failed to append crc32 accel task, rc %d\n", rc);
			}
			return rc;
		}
		tcp_req->ordering.bits.digest_offloaded = 1;
	}

	if (!skip_copy) {
		rc = group->accel_fn_table.append_copy(group->ctx, (void **)&accel_seq, &tcp_req->iobuf_iov, 1,
						       NULL, NULL, pdu->iovs, pdu->data_iovcnt,
						       req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx, NULL, NULL);
		if (spdk_unlikely(rc)) {
			return rc;
		}
	}

	/* Staging buffer will contain result of accel operations and will be written to the socket
	 * We can update iovs to be passed to the socket with this staging buffer */
	pdu->iovs = &tcp_req->iobuf_iov;
	pdu->data_iovcnt = 1;
	/* Buffer is in local memory, clear memory domain flag */
	assert(tcp_req->ordering.bits.has_memory_domain);
	tcp_req->ordering.bits.has_memory_domain = 0;
	/* We need to execute accel sequence before writing data to the socket */
	tcp_req->ordering.bits.needs_accel_seq = 1;
	req->accel_sequence = accel_seq;

	return 0;
}

static void
nvme_tcp_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct nvme_tcp_req *tcp_req = SPDK_CONTAINEROF(entry, struct nvme_tcp_req, iobuf_entry);
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct spdk_nvme_cpl cpl;
	int rc;

	tcp_req->iobuf_iov.iov_base = buf;
	TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);
	tqpair->stats->outstanding_reqs++;
	rc = nvme_tcp_handle_accel_sequence_in_capsule(tcp_req);

	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("failed to apply sequence, rc %d\n", rc);
		goto err;
	}

	rc = nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to send capsule cmd, rc %d\n", rc);
		goto err;
	}

	spdk_trace_record(TRACE_NVME_TCP_SUBMIT, tqpair->qpair.id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12);
	return;

err:
	cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = 1;
	nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
}

static int
nvme_tcp_req_init(struct nvme_tcp_qpair *tqpair, struct nvme_request *req,
		  struct nvme_tcp_req *tcp_req)
{
	req->cmd.cid = tcp_req->cid;
	tcp_req->ordering.bits.has_memory_domain = req->payload.opts && req->payload.opts->memory_domain;

	return nvme_tcp_req_build(tcp_req);
}

static inline bool
nvme_tcp_req_complete_safe(struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_qpair *tqpair;

	if (!(tcp_req->ordering.bits.send_ack && tcp_req->ordering.bits.data_recv)) {
		return false;
	}

	tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	assert(tcp_req->ordering.bits.state == NVME_TCP_REQ_ACTIVE);
	assert(tqpair != NULL);

	SPDK_DEBUGLOG(nvme, "tqpair %p %u, xlio_sock 0x%lx, tcp_req %p pdu %p, ordering %x\n", tqpair,
		      tqpair->qpair.id, tqpair->xlio_sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw);

	if (!tqpair->qpair.in_completion_context) {
		tqpair->async_complete++;
	}

	nvme_tcp_req_complete(tcp_req, tqpair, &tcp_req->req.cpl, true);
	return true;
}

static void
nvme_tcp_qpair_cmd_send_complete(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;

	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING)) {
		struct spdk_nvme_cpl cpl = {
			.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION,
			.status.sct = SPDK_NVME_SCT_GENERIC,
			.status.dnr = 1
		};

		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p cpl in disconnecting\n", tqpair, tcp_req);

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
		return;
	}

	tcp_req->ordering.bits.send_ack = 1;
	SPDK_DEBUGLOG(nvme, "tqpair %p %u, xlio_sock 0x%lx, tcp_req %p pdu %p, ordering %x cid %u\n",
		      tqpair,
		      tqpair->qpair.id, tqpair->xlio_sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw, tcp_req->cid);
	/* Handle the r2t case */
	if (spdk_unlikely(tcp_req->ordering.bits.h2c_send_waiting_ack)) {
		SPDK_DEBUGLOG(nvme, "tcp req %p, send H2C data\n", tcp_req);
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		if (group && tcp_req->ordering.bits.in_capsule_data && tcp_req->iobuf_iov.iov_base) {
			spdk_iobuf_put(group->group->accel_fn_table.get_iobuf_channel(group->group->ctx),
				       tcp_req->iobuf_iov.iov_base,
				       tcp_req->iobuf_iov.iov_len);
			tcp_req->iobuf_iov.iov_base = NULL;
		}

		nvme_tcp_req_complete_safe(tcp_req);
	}
}

static inline int
nvme_tcp_get_memory_translation(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				struct spdk_rdma_provider_memory_translation_ctx *_ctx)
{
	struct nvme_request *req = &tcp_req->req;
	struct spdk_memory_domain_translation_result dma_translation;
	struct spdk_rdma_utils_memory_translation rdma_translation;
	int rc;

	if (tcp_req->ordering.bits.has_memory_domain) {
		assert(_ctx);
		struct ibv_qp dst_qp = {
			.pd = tqpair->pd
		};
		struct spdk_memory_domain_translation_ctx dst_domain_ctx = {
			.size = sizeof(struct spdk_memory_domain_translation_ctx),
			.rdma.ibv_qp = &dst_qp
		};
		dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

		rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx,
						       tqpair->memory_domain, &dst_domain_ctx,
						       _ctx->addr, _ctx->length, &dma_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("DMA memory translation failed, rc %d\n", rc);
			return rc;
		} else if (spdk_unlikely(dma_translation.iov_count != 1)) {
			SPDK_ERRLOG("Translation to multiple iovs is not supported, iov count %u\n",
				    dma_translation.iov_count);
			return -ENOTSUP;
		}

		_ctx->lkey = dma_translation.rdma.lkey;
		_ctx->rkey = dma_translation.rdma.rkey;
		_ctx->addr = dma_translation.iov.iov_base;
		_ctx->length = dma_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_utils_get_translation(tqpair->mem_map, _ctx->addr, _ctx->length, &rdma_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA memory translation failed, rc %d\n", rc);
			return rc;
		}
		if (spdk_likely(rdma_translation.translation_type == SPDK_RDMA_UTILS_TRANSLATION_MR)) {
			_ctx->lkey = rdma_translation.mr_or_key.mr->lkey;
			_ctx->rkey = rdma_translation.mr_or_key.mr->rkey;
		} else {
			_ctx->lkey = _ctx->rkey = (uint32_t)rdma_translation.mr_or_key.key;
		}
	}

	return 0;
}

static inline int
nvme_tcp_fill_data_mkeys(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req,
			 struct nvme_tcp_pdu *pdu)
{
	uint8_t iovcnt;
	int rc;

	for (iovcnt = 0; iovcnt < pdu->data_iovcnt; iovcnt++) {
		struct spdk_rdma_provider_memory_translation_ctx ctx = {
			. addr = pdu->iovs[iovcnt].iov_base,
			.length = pdu->iovs[iovcnt].iov_len
		};

		assert(tcp_req != NULL);
		rc = nvme_tcp_get_memory_translation(tcp_req, tqpair, &ctx);

		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Memory translation failed, rc %d", rc);
			return rc;
		}

		pdu->mkeys[iovcnt] = ctx.lkey;
		assert(ctx.lkey);
	}

	return 0;
}

static int
nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t plen = 0, alignment;
	uint8_t pdo;

	pdu = &tcp_req->pdu;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	capsule_cmd->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	plen = capsule_cmd->common.hlen = sizeof(*capsule_cmd);
	capsule_cmd->ccsqe = tcp_req->req.cmd;

	/* Capsule header with hdsgt and possible alignment */
	pdu->has_capsule = 1;
	if (tqpair->flags.host_hdgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	pdo = plen;
	pdu->padding_len = 0;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			pdu->padding_len = alignment - plen;
			pdo = alignment;
			plen = alignment;
		}
	}

	pdu->capsule_len = plen;
	pdu->capsule_offset = 0;

	if (tcp_req->req.payload.size == 0 || !tcp_req->ordering.bits.in_capsule_data) {
		pdu->data_len = 0;
		goto end;
	}

	/* data + digest */
	pdu->data_len = tcp_req->req.payload.size;

	capsule_cmd->common.pdo = pdo;
	plen += tcp_req->req.payload.size;
	if (pdu->data_len > 0 && tqpair->flags.host_ddgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
		if (!tcp_req->ordering.bits.digest_offloaded) {
			uint32_t crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(pdu, pdu->iovs, pdu->data_iovcnt);
			MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
			pdu->ddgst_enable = 1;
			pdu->ddigest_offset = 0;
		} else {
			/* digest is sent as part of data buffer */
			pdu->data_len += SPDK_NVME_TCP_DIGEST_LEN;
		}
		tqpair->stats->send_ddgsts++;
	}

	if (tcp_req->ordering.bits.in_capsule_data) {
		if (spdk_unlikely(nvme_tcp_fill_data_mkeys(tqpair, tcp_req, pdu) != 0)) {
			return -1;
		}
	}

	pdu->iovcnt = pdu->data_iovcnt + pdu->has_capsule + pdu->ddgst_enable;
	tcp_req->datao = 0;

end:
	capsule_cmd->common.plen = plen;

	if (tqpair->flags.host_hdgst_enable) {
		uint32_t crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
	}

	pdu->cb_fn = nvme_tcp_qpair_cmd_send_complete;

	SPDK_DEBUGLOG(nvme, "tqpair %p %u, xlio_sock 0x%lx, tcp_req %p pdu %p, ordering 0x%x cid %u\n",
		      tqpair, tqpair->qpair.id, tqpair->xlio_sock, tcp_req, &tcp_req->pdu,
		      tcp_req->ordering.raw, tcp_req->cid);

	if (tcp_req->ordering.bits.needs_accel_seq) {
		assert(tcp_req->req.accel_sequence);
		tcp_req->ordering.bits.in_progress_accel = 1;
		tqpair->qpair.poll_group->group->accel_fn_table.finish_sequence(
			tcp_req->req.accel_sequence,
			_nvme_tcp_accel_finished_in_capsule,
			tcp_req);
		return 0;
	}

	pdu->has_mkeys = !nvme_qpair_is_admin_queue(&tqpair->qpair) &&
			 &tcp_req->req != tqpair->qpair.reserved_req && pdu->data_len;

	tqpair->stats->submitted_requests++;
	if (spdk_unlikely(nvme_tcp_qpair_send_pdu(tqpair, pdu))) {
		return -1;
	}

	return 0;
}

static int
nvme_tcp_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			      struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;
	int rc;
	enum spdk_nvme_data_transfer xfer;

	tqpair = nvme_tcp_qpair(qpair);
	assert(tqpair != NULL);

	tcp_req = nvme_tcp_req(req);
	assert(tcp_req != NULL);

	rc = nvme_tcp_req_get(tqpair, tcp_req);
	if (spdk_unlikely(rc != 0)) {
		tqpair->stats->queued_requests++;
		/* Inform the upper layer to try again later. */
		return rc;
	}

	rc = nvme_tcp_req_init(tqpair, req, tcp_req);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("nvme_tcp_req_init() failed, rc %d\n", rc);
		nvme_tcp_req_put(tqpair, tcp_req);
		return -1;
	}

	xfer = spdk_nvmf_cmd_get_data_transfer(&req->cmd);
	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && req->payload.opts &&
	    ((req->accel_sequence && (!tqpair->flags.host_ddgst_enable ||
				      tcp_req->ordering.bits.in_capsule_data)) ||
	     (tqpair->flags.host_ddgst_enable && tcp_req->ordering.bits.in_capsule_data))) {
		struct spdk_iobuf_channel *iobuf_ch;
		struct spdk_nvme_poll_group *group;

		/* Request contains accel sequence, we need to finish the sequence before
		 * continue to build the request */
		group = tqpair->qpair.poll_group->group;
		if (spdk_unlikely(!group)) {
			SPDK_ERRLOG("accel_seq is only supported with poll groups\n");
			nvme_tcp_req_put(tqpair, tcp_req);
			return -ENOTSUP;
		}
		iobuf_ch = group->accel_fn_table.get_iobuf_channel(group->ctx);
		assert(iobuf_ch);
		tcp_req->iobuf_iov.iov_len = req->payload.size + SPDK_NVME_TCP_DIGEST_LEN *
					     tqpair->flags.host_ddgst_enable;
		tcp_req->iobuf_iov.iov_base = spdk_iobuf_get(iobuf_ch, tcp_req->iobuf_iov.iov_len,
					      &tcp_req->iobuf_entry, nvme_tcp_iobuf_get_cb);
		if (spdk_unlikely(!tcp_req->iobuf_iov.iov_base)) {
			/* Finish accel sequence once buffer is allocated */
			SPDK_DEBUGLOG(nvme, "no buffer, in progress\n");
			return 0;
		}
		rc = nvme_tcp_handle_accel_sequence_in_capsule(tcp_req);
		if (spdk_unlikely(rc)) {
			nvme_tcp_req_put(tqpair, tcp_req);
			return rc;
		}
	}

	TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);
	tqpair->stats->outstanding_reqs++;
	rc = nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to send capsule cmd, rc %d\n", rc);
		TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);
		tqpair->stats->outstanding_reqs--;
		nvme_tcp_req_put(tqpair, tcp_req);
		return -1;
	}

	spdk_trace_record(TRACE_NVME_TCP_SUBMIT, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12);
	return 0;
}

static int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static inline void
_nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req,
		       struct nvme_tcp_qpair *tqpair,
		       struct spdk_nvme_cpl *rsp)
{
	struct spdk_nvme_cpl	cpl;
	spdk_nvme_cmd_cb	user_cb;
	void			*user_cb_arg;
	struct nvme_request	*req;
	struct spdk_nvme_qpair	*qpair;

	req = &tcp_req->req;
	qpair = req->qpair;

	xlio_sock_free_bufs(tqpair, tcp_req->sock_buf);
	tcp_req->sock_buf = NULL;

	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);
	assert(tqpair->stats->outstanding_reqs > 0);
	tqpair->stats->outstanding_reqs--;
	SPDK_DEBUGLOG(nvme,
		      "tqpair %p %u (out %"PRIu64"), xlio_sock 0x%lx, tcp_req %p pdu %p, ordering %x, cid %u\n", tqpair,
		      tqpair->qpair.id, tqpair->stats->outstanding_reqs, tqpair->xlio_sock, tcp_req, &tcp_req->pdu,
		      tcp_req->ordering.raw, rsp->cid);

	/* Cache arguments to be passed to nvme_complete_request since tcp_req can be zeroed when released */
	memcpy(&cpl, rsp, sizeof(cpl));
	user_cb		= req->cb_fn;
	user_cb_arg	= req->cb_arg;

	/* Accel sequence is either executed or aborted. Clean pointer since nvme transport expects it
	 * to be NULL */
	req->accel_sequence = NULL;
	nvme_tcp_req_put(tqpair, tcp_req);
	nvme_complete_request(user_cb, user_cb_arg, qpair, req, &cpl);
}

static void
nvme_tcp_req_accel_seq_complete_cb(void *arg, int status)
{
	struct nvme_tcp_req	*tcp_req = arg;
	struct nvme_tcp_qpair	*tqpair = nvme_tcp_qpair(tcp_req->req.qpair);

	SPDK_DEBUGLOG(nvme, "Accel sequence completed: tcp_req %p, status %d\n", tcp_req, status);

	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;
	tcp_req->pdu.iovcnt = 0;
	tcp_req->pdu.data_iovcnt = 0;
	tcp_req->pdu.data_len = 0;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqpair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
		tcp_req->req.cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		tcp_req->req.cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		tcp_req->req.cpl.status.dnr = 1;
		goto complete;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
			  !spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			      tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		tcp_req->req.cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		tcp_req->req.cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		tcp_req->req.cpl.status.dnr = 0;
	}

complete:
	/* Requests may be queued before accel sequence completed,
	 * so need to poll again or resubmit them.
	 */
	if (tqpair->qpair.poll_group && !STAILQ_EMPTY(&tqpair->qpair.queued_req)) {
		if (!tqpair->flags.pending_events) {
			struct nvme_tcp_poll_group *pgroup;

			pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
			TAILQ_INSERT_TAIL(&pgroup->pending_events, tqpair, link);
			tqpair->flags.pending_events = true;
		}
		tqpair->flags.needs_resubmit = true;
	}
	_nvme_tcp_req_complete(tcp_req, tqpair, &tcp_req->req.cpl);
}

static inline int
nvme_tcp_calc_and_set_recv_iovs(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				struct spdk_nvme_cpl *rsp)
{
	struct xlio_sock_buf *sock_buf = tcp_req->sock_buf;
	uint32_t iovcnt = 0;

	while (sock_buf) {
		iovcnt++;
		sock_buf = sock_buf->next;
	}

	if (iovcnt > NVME_TCP_MAX_SGL_DESCRIPTORS) {
		assert(tcp_req->recv_iovs != NULL);
		if (iovcnt <= MAX_RX_IOVS) {
			tcp_req->recv_iovs = spdk_mempool_get(g_iovec_pool);
		}

		if (iovcnt > MAX_RX_IOVS || tcp_req->recv_iovs == NULL) {
			tcp_req->recv_iovs = malloc(sizeof(*tcp_req->recv_iovs) * iovcnt);
			if (spdk_unlikely(tcp_req->recv_iovs == NULL)) {
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				SPDK_ERRLOG("Failed to allocate RX iovec array, iovcnt %u\n", iovcnt);
				return -ENOMEM;
			}
			tcp_req->ordering.bits.recv_iovs_malloced = 1;
			tqpair->stats->rx_iov_fallbacks++;
		}
	}

	tqpair->stats->received_data_pdus++;
	tqpair->stats->received_data_iovs += iovcnt;
	if (iovcnt > tqpair->stats->max_data_iovs_per_pdu) {
		tqpair->stats->max_data_iovs_per_pdu = iovcnt;
	}

	tcp_req->recv_iovcnt = 0;
	sock_buf = tcp_req->sock_buf;
	while (sock_buf) {
		tcp_req->recv_iovs[tcp_req->recv_iovcnt++] = sock_buf->iov;
		sock_buf = sock_buf->next;
	}

	return 0;
}

static void
nvme_tcp_req_complete_memory_domain(struct nvme_tcp_req *tcp_req,
				    struct nvme_tcp_qpair *tqpair,
				    struct spdk_nvme_cpl *rsp)
{
	struct nvme_request	*req;
	enum spdk_nvme_data_transfer xfer;
	bool			error;
	int			rc = 0;
	struct spdk_accel_task	*task;
	struct spdk_accel_sequence *accel_seq;
	bool skip_copy = false;

	req = &tcp_req->req;

	assert(req->cmd.opc != SPDK_NVME_OPC_FABRIC);
	xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
	error = spdk_nvme_cpl_is_error(rsp);

	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;

		assert(group != 0);

		/* @todo: check if we ever need to deliver data for error completion, e.g. with data digest error */
		if (spdk_unlikely(error)) {
			goto out;
		}

		if (tcp_req->ordering.bits.digest_offloaded) {
			goto out;
		}

		rc = nvme_tcp_calc_and_set_recv_iovs(tcp_req, tqpair, rsp);
		if (spdk_unlikely(rc)) {
			goto out;
		}

		accel_seq = req->accel_sequence;
		if (accel_seq) {
			task = spdk_accel_sequence_first_task(accel_seq);
			if (task->op_code == SPDK_ACCEL_OPC_DECRYPT && spdk_accel_sequence_next_task(task) == NULL) {
				skip_copy = true;
				task->src_domain = NULL;
				task->src_domain_ctx = NULL;
				task->s.iovs = tcp_req->recv_iovs;
				task->s.iovcnt = tcp_req->recv_iovcnt;
			}
		}
		if (!skip_copy) {
			rc = group->accel_fn_table.append_copy(group->ctx, (void **)&accel_seq,
							       tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
							       req->payload.opts->memory_domain,
							       req->payload.opts->memory_domain_ctx,
							       tcp_req->recv_iovs, tcp_req->recv_iovcnt,
							       NULL, NULL, NULL, NULL);
			if (spdk_unlikely(rc)) {
				SPDK_ERRLOG("Failed to append copy accel task, rc %d\n", rc);
				goto out;
			}

		}
		group->accel_fn_table.reverse_sequence(accel_seq);
		tcp_req->ordering.bits.in_progress_accel = 1;
		group->accel_fn_table.finish_sequence(accel_seq, nvme_tcp_req_accel_seq_complete_cb, tcp_req);
		return;
	}

out:
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		tcp_req->pdu.data_iovcnt = 0;
		tcp_req->pdu.iovcnt = 0;
		tcp_req->pdu.data_len = 0;
	}

	_nvme_tcp_req_complete(tcp_req, tqpair, rsp);
}

static inline void
nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req,
		      struct nvme_tcp_qpair *tqpair,
		      struct spdk_nvme_cpl *rsp,
		      bool print_on_error)
{
	struct spdk_nvme_qpair	*qpair;
	struct nvme_request	*req;
	bool			error, print_error;

	req = &tcp_req->req;
	qpair = req->qpair;

	error = spdk_nvme_cpl_is_error(rsp);
	spdk_trace_record(TRACE_NVME_TCP_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)rsp->status_raw);

	if (spdk_unlikely(error)) {
		print_error = print_on_error && !qpair->ctrlr->opts.disable_error_logging;
		if (spdk_unlikely(print_error)) {
			spdk_nvme_qpair_print_command(qpair, &req->cmd);
		}
		if (spdk_unlikely(print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme"))) {
			spdk_nvme_qpair_print_completion(qpair, rsp);
		}
	}

	if (spdk_likely(nvme_tcp_req_with_memory_domain(tcp_req))) {
		nvme_tcp_req_complete_memory_domain(tcp_req, tqpair, rsp);
		return;
	}

	_nvme_tcp_req_complete(tcp_req, tqpair, rsp);
}

static void
nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock 0x%lx, req %p, ordering 0x%x\n",
			      tqpair, qpair->id, tqpair->xlio_sock, tcp_req, tcp_req->ordering.raw);
		if (tcp_req->ordering.bits.in_progress_accel) {
			continue;
		}
		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
	}
}

static void
nvme_tcp_qpair_send_h2c_term_req_complete(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_req *tcp_req)
{
	tqpair->state = NVME_TCP_QPAIR_STATE_EXITING;
}

static void
nvme_tcp_qpair_send_h2c_term_req(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req;
	uint32_t h2c_term_req_hdr_len = sizeof(*h2c_term_req);
	uint8_t copy_len;

	rsp_pdu = tqpair->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	h2c_term_req = &tqpair->ctrl_hdr.term_req;
	h2c_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;
	h2c_term_req->common.hlen = h2c_term_req_hdr_len;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&h2c_term_req->fei, error_offset);
	}

	copy_len = tqpair->ctrl_hdr.common.hlen;
	if (copy_len > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE) {
		copy_len = SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE;
	}

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)tqpair->ctrl_hdr.raw + h2c_term_req_hdr_len, tqpair->ctrl_hdr.raw, copy_len);

	/* Contain the header len of the wrong received pdu */
	h2c_term_req->common.plen = h2c_term_req->common.hlen + copy_len;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	nvme_tcp_qpair_write_control_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_send_h2c_term_req_complete);
}

static bool
nvme_tcp_qpair_recv_state_valid(struct nvme_tcp_qpair *tqpair)
{
	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
	case NVME_TCP_QPAIR_STATE_RUNNING:
		return true;
	default:
		return false;
	}
}

static void
nvme_tcp_pdu_ch_handle(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen, hd_len = 0;
	bool plen_error = false;

	pdu = tqpair->recv_pdu;

	SPDK_DEBUGLOG(nvme, "pdu type = %d\n", pdu->hdr.common.pdu_type);
	if (spdk_unlikely(pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP)) {
		if (tqpair->state >= NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED) {
			SPDK_ERRLOG("Already received IC_RESP PDU, and we should reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (spdk_unlikely(!nvme_tcp_qpair_recv_state_valid(tqpair))) {
			SPDK_ERRLOG("The TCP/IP tqpair connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
			expected_hlen = sizeof(struct spdk_nvme_tcp_rsp);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (spdk_unlikely(pdu->hdr.common.plen != (expected_hlen + hd_len))) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
			if (spdk_unlikely(pdu->hdr.common.plen < pdu->hdr.common.pdo)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if (spdk_unlikely((pdu->hdr.common.plen < expected_hlen) ||
					  (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE))) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_R2T:
			expected_hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (spdk_unlikely(pdu->hdr.common.plen != (expected_hlen + hd_len))) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (spdk_unlikely(pdu->hdr.common.hlen != expected_hlen)) {
		SPDK_ERRLOG("Expected PDU header length %u, got %u\n",
			    expected_hlen, pdu->hdr.common.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (spdk_unlikely(plen_error)) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(tqpair->recv_pdu, tqpair->flags.host_hdgst_enable);
		return;
	}
err:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static struct nvme_tcp_req *
get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid)
{
	assert(tqpair != NULL);
	if (spdk_unlikely(cid >= tqpair->num_entries)) {
		return NULL;
	}

	return tqpair->tcp_reqs_lookup[cid];
}

static void
nvme_tcp_c2h_data_payload_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu, uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint8_t flags;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvme, "enter\n");
	c2h_data = &pdu->hdr.c2h_data;
	tcp_req->datao += pdu->data_len;
	flags = c2h_data->common.flags;

	if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU) {
		rsp = &tcp_req->req.cpl;
		rsp->status.p = tcp_req->datao != tcp_req->req.payload.size;
		rsp->cid = tcp_req->cid;
		rsp->sqid = tqpair->qpair.id;
		if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
			tcp_req->ordering.bits.data_recv = 1;
			if (nvme_tcp_req_complete_safe(tcp_req)) {
				(*reaped)++;
			}
		}
	}
}

static const char *spdk_nvme_tcp_term_req_fes_str[] = {
	"Reserved",
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digest Error",
	"Data Transfer Out of Range",
	"Data Transfer Limit Exceeded",
	"Unsupported parameter",
};

static void
nvme_tcp_c2h_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *c2h_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", c2h_term_req,
		    spdk_nvme_tcp_term_req_fes_str[c2h_term_req->fes]);
	if ((c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(nvme, "The offset from the start of the PDU header is %u\n",
			      DGET32(c2h_term_req->fei));
	}
	/* we may also need to dump some other info here */
}

static void
nvme_tcp_c2h_term_req_payload_handle(struct nvme_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	bool need_lock = nvme_qpair_is_admin_queue(&tqpair->qpair);

	if (need_lock) {
		spdk_spin_lock(&g_xlio_admin_group_lock);
	}
	nvme_tcp_c2h_term_req_dump(&tqpair->ctrl_hdr.term_req);
	nvme_tcp_qpair_abort_reqs(&tqpair->qpair, 0);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	if (need_lock) {
		spdk_spin_unlock(&g_xlio_admin_group_lock);
	}
}

static void
_nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;

	assert(tqpair != NULL);
	pdu = tqpair->recv_pdu;

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_payload_handle(tqpair, pdu, reaped);
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_payload_handle(tqpair, pdu);
		break;
	default:
		/* The code should not go to here */
		SPDK_ERRLOG("The code should not go to here\n");
		break;
	}
}

static void
nvme_tcp_req_accel_seq_complete_crc_c2h_cb(void *arg, int status)
{
	struct nvme_tcp_req *tcp_req = arg;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *pgroup;
	int dummy_reaped = 0;

	SPDK_DEBUGLOG(nvme, "Accel sequence completed: tcp_req %p, status %d\n", tcp_req, status);
	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;

	tqpair = nvme_tcp_qpair(tcp_req->req.qpair);

	if (tqpair->qpair.poll_group && !STAILQ_EMPTY(&tqpair->qpair.queued_req) &&
	    !tqpair->flags.pending_events) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		TAILQ_INSERT_TAIL(&pgroup->pending_events, tqpair, link);
		tqpair->flags.pending_events = true;
	}

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", &tcp_req->pdu);
		tcp_req->req.cpl.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		tcp_req->req.cpl.status.dnr = 1;

		/* Prevent aborting this sequence in nvme_tcp_req_complete_memory_domain(). */
		tcp_req->req.accel_sequence = NULL;
		goto complete;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
			  !spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			      tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		tcp_req->req.cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		tcp_req->req.cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		tcp_req->req.cpl.status.dnr = 0;
	}
complete:
	nvme_tcp_c2h_data_payload_handle(tqpair, &tcp_req->pdu, &dummy_reaped);
}

static inline int
nvme_tcp_apply_accel_sequence_c2h(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct nvme_request *req = &tcp_req->req;
	struct spdk_accel_sequence *accel_seq;
	bool skip_copy = false;
	int rc;

	accel_seq = req->accel_sequence;
	if (accel_seq) {
		struct spdk_accel_task *task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == SPDK_ACCEL_OPC_DECRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			skip_copy = true;
			task->src_domain = NULL;
			task->src_domain_ctx = NULL;
			task->s.iovs = tcp_req->recv_iovs;
			task->s.iovcnt = tcp_req->recv_iovcnt;
		}
	}

	if (skip_copy) {
		rc = group->accel_fn_table.append_check_crc32c(group->ctx, (void **)&accel_seq,
				&pdu->data_digest_crc32, tcp_req->recv_iovs,
				tcp_req->recv_iovcnt, NULL, NULL, 0, NULL, NULL);
	} else {
		rc = group->accel_fn_table.append_copy_check_crc32c(group->ctx, (void **)&accel_seq,
				&pdu->data_digest_crc32,
				tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
				req->payload.opts->memory_domain,
				req->payload.opts->memory_domain_ctx,
				tcp_req->recv_iovs, tcp_req->recv_iovcnt, NULL, NULL,
				0, NULL, NULL);
	}

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(nvme, "pdu %p, tqpair %p: no task for check_crc32c\n", pdu, tqpair);
			TAILQ_INSERT_TAIL(&tqpair->accel_nomem_queue, pdu, tailq);
			tqpair->flags.has_accel_nomem_pdus = 1;

			if (!tqpair->flags.pending_events) {
				struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

				TAILQ_INSERT_TAIL(&tgroup->pending_events, tqpair, link);
				tqpair->flags.pending_events = 1;
			}

			return rc;
		}
		SPDK_ERRLOG("Failed to append check crc accel task, rc %d\n", rc);
		goto abort_sequence;
	}

	group->accel_fn_table.reverse_sequence(accel_seq);
	tcp_req->ordering.bits.in_progress_accel = 1;
	group->accel_fn_table.finish_sequence(accel_seq, nvme_tcp_req_accel_seq_complete_crc_c2h_cb,
					      tcp_req);

	return 0;
abort_sequence:
	if (!req->accel_sequence) {
		group->accel_fn_table.abort_sequence(accel_seq);
	}
	return rc;
}

static inline int
nvme_tcp_prepare_accel_sequence_c2h(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	int rc;

	rc = nvme_tcp_calc_and_set_recv_iovs(tcp_req, tqpair, &tcp_req->req.cpl);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	rc = nvme_tcp_apply_accel_sequence_c2h(tqpair, pdu);
	if (spdk_unlikely(rc == -ENOMEM)) {
		return 0;
	}

	return rc;
}

static void
nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair,
			    uint32_t *reaped)
{
	struct nvme_tcp_pdu *recv_pdu = tqpair->recv_pdu;
	struct nvme_tcp_poll_group *tgroup;
	struct nvme_tcp_req *tcp_req = recv_pdu->req;
	uint32_t crc32c;
	int rc = 0;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);

	SPDK_DEBUGLOG(nvme, "tqpair %p %u xlio_sock 0x%lx, tcp_req %p pdu %p, ordering %x, cid %u\n",
		      tqpair,
		      tqpair->qpair.id, tqpair->xlio_sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw, tcp_req->cid);

	/* The request can be NULL, e.g. in case of C2HTermReq */
	if (spdk_likely(tcp_req != NULL)) {
		tcp_req->expected_datao += recv_pdu->data_len;
	}

	/* check data digest if need */
	if (recv_pdu->ddgst_enable) {
		/* But if the data digest is enabled, tcp_req cannot be NULL */
		assert(tcp_req != NULL);
		tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		/* Only support this limitated case that the request has only one c2h pdu */
		if ((nvme_qpair_get_state(&tqpair->qpair) >= NVME_QPAIR_CONNECTED) &&
		    tgroup != NULL &&
		    recv_pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0 &&
		    tcp_req->req.payload.size == recv_pdu->data_len) {
			tcp_req->pdu.hdr = recv_pdu->hdr;
			tcp_req->pdu.req = tcp_req;
			memcpy(tcp_req->pdu.data_digest, recv_pdu->data_digest, sizeof(recv_pdu->data_digest));
			tcp_req->pdu.data_len = recv_pdu->data_len;

			if (nvme_tcp_req_with_memory_domain(tcp_req)) {
				tqpair->stats->recv_ddgsts++;
				tcp_req->ordering.bits.digest_offloaded = 1;
				tcp_req->pdu.data_digest_crc32 = DGET32(tcp_req->pdu.data_digest);
				tcp_req->pdu.data_digest_crc32 ^= SPDK_CRC32C_XOR;

				rc = nvme_tcp_prepare_accel_sequence_c2h(tqpair, &tcp_req->pdu);
				if (spdk_unlikely(rc)) {
					SPDK_ERRLOG("accel_seq failed with rc %d\n", rc);
					goto transient_error;
				}
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				return;
			}
		}

		if (nvme_tcp_req_with_memory_domain(recv_pdu->req)) {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_sock_buf(recv_pdu);
		} else {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(recv_pdu, recv_pdu->iovs, recv_pdu->data_iovcnt);
		}

		tqpair->stats->recv_ddgsts++;
		rc = MATCH_DIGEST_WORD(recv_pdu->data_digest, crc32c);
		if (spdk_unlikely(rc == 0)) {
transient_error:
			SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, recv_pdu);
			tcp_req = recv_pdu->req;
			assert(tcp_req != NULL);
			tcp_req->req.cpl.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		}
	}

	_nvme_tcp_pdu_payload_handle(tqpair, reaped);
}

static void
nvme_tcp_send_icreq_complete(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	SPDK_DEBUGLOG(nvme, "Complete the icreq send for tqpair=%p %u\n", tqpair, tqpair->qpair.id);

	tqpair->flags.icreq_send_ack = true;

	if (tqpair->state == NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED) {
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, finalize icresp\n", tqpair, tqpair->qpair.id);
		tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND;
	}
}

static void
nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair,
		       struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = &pdu->hdr.ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int recv_buf_size;

	/* Only PFV 0 is defined currently */
	if (ic_resp->pfv != 0) {
		SPDK_ERRLOG("Expected ICResp PFV %u, got %u\n", 0u, ic_resp->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, pfv);
		goto end;
	}

	if (ic_resp->maxh2cdata < NVME_TCP_PDU_H2C_MIN_DATA_SIZE) {
		SPDK_ERRLOG("Expected ICResp maxh2cdata >=%u, got %u\n", NVME_TCP_PDU_H2C_MIN_DATA_SIZE,
			    ic_resp->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, maxh2cdata);
		goto end;
	}
	tqpair->maxh2cdata = ic_resp->maxh2cdata;

	if (ic_resp->cpda > SPDK_NVME_TCP_CPDA_MAX) {
		SPDK_ERRLOG("Expected ICResp cpda <=%u, got %u\n", SPDK_NVME_TCP_CPDA_MAX, ic_resp->cpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, cpda);
		goto end;
	}
	tqpair->cpda = ic_resp->cpda;

	tqpair->flags.host_hdgst_enable = ic_resp->dgst.bits.hdgst_enable ? true : false;
	tqpair->flags.host_ddgst_enable = ic_resp->dgst.bits.ddgst_enable ? true : false;
	SPDK_DEBUGLOG(nvme, "host_hdgst_enable: %u\n", tqpair->flags.host_hdgst_enable);
	SPDK_DEBUGLOG(nvme, "host_ddgst_enable: %u\n", tqpair->flags.host_ddgst_enable);

	/* Now that we know whether digests are enabled, properly size the receive buffer to
	 * handle several incoming 4K read commands according to SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR
	 * parameter. */
	recv_buf_size = 0x1000 + sizeof(struct spdk_nvme_tcp_c2h_data_hdr);

	if (tqpair->flags.host_hdgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (tqpair->flags.host_ddgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (xlio_sock_set_recvbuf(tqpair, recv_buf_size * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR) < 0) {
		SPDK_WARNLOG("Unable to allocate enough memory for receive buffer on tqpair=%p with size=%d\n",
			     tqpair,
			     recv_buf_size);
		/* Not fatal. */
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (!tqpair->flags.icreq_send_ack) {
		tqpair->state = NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED;
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, waiting icreq ack\n", tqpair, tqpair->qpair.id);
		return;
	}

	tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND;
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_capsule_resp_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_rsp *capsule_resp = &pdu->hdr.capsule_resp;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(nvme, "enter\n");
	cid = capsule_resp->rccqe.cid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);

	if (spdk_unlikely(!tcp_req)) {
		SPDK_ERRLOG("no tcp_req is found with cid=%u for tqpair=%p\n", cid, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_rsp, rccqe);
		goto end;
	}

	tcp_req->req.cpl = capsule_resp->rccqe;
	tcp_req->ordering.bits.data_recv = 1;

	/* Recv the pdu again */
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (nvme_tcp_req_complete_safe(tcp_req)) {
		(*reaped)++;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_c2h_term_req_hdr_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct nvme_tcp_req *tcp_req;

	if (c2h_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Status(FES) is unknown for c2h_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	memcpy(&tqpair->ctrl_hdr.raw, c2h_term_req, c2h_term_req->common.hlen);
	c2h_term_req = &tqpair->ctrl_hdr.term_req;
	/* set the data buffer */
	tcp_req = SPDK_CONTAINEROF(pdu, struct nvme_tcp_req, pdu);
	pdu->iovs = tcp_req->iovs;
	pdu->data_iovcnt = 1;
	pdu->iovs[0].iov_base = (uint8_t *)tqpair->ctrl_hdr.raw + c2h_term_req->common.hlen;
	pdu->iovs[0].iov_len = c2h_term_req->common.plen - c2h_term_req->common.hlen;
	pdu->data_len = pdu->iovs[0].iov_len;
	pdu->iovcnt = pdu->data_iovcnt;

	if (pdu->data_len) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	} else {
		nvme_tcp_c2h_term_req_payload_handle(tqpair, pdu);
	}

	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_c2h_data_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *recv_pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data = &recv_pdu->hdr.c2h_data;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int flags = c2h_data->common.flags;

	tcp_req = get_nvme_active_req_by_cid(tqpair, c2h_data->cccid);
	SPDK_DEBUGLOG(nvme,
		      "tqpair %p %u, xlio_sock 0x%lx, tcp_req %p pdu %p, ordering 0x%x: datao=%u, datal=%u, cccid=%d\n",
		      tqpair, tqpair->qpair.id, tqpair->xlio_sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw,
		      c2h_data->datao, c2h_data->datal, c2h_data->cccid);
	if (spdk_unlikely(!tcp_req)) {
		SPDK_ERRLOG("no tcp_req found for c2hdata cid=%d\n", c2h_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid);
		goto end;
	}

	if (spdk_unlikely((flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) &&
			  !(flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU))) {
		SPDK_ERRLOG("Invalid flag flags=%d in c2h_data=%p\n", flags, c2h_data);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, common);
		goto end;
	}

	if (spdk_unlikely(c2h_data->datal > tcp_req->req.payload.size)) {
		SPDK_ERRLOG("Invalid datal for tcp_req(%p), datal(%u) exceeds payload_size(%u)\n",
			    tcp_req, c2h_data->datal, tcp_req->req.payload.size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto end;
	}

	if (spdk_unlikely(tcp_req->expected_datao != c2h_data->datao)) {
		SPDK_ERRLOG("Invalid datao for tcp_req(%p), received datal(%u) != expected datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, tcp_req->expected_datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datao);
		goto end;
	}

	if (spdk_unlikely((c2h_data->datao + c2h_data->datal) > tcp_req->req.payload.size)) {
		SPDK_ERRLOG("Invalid data range for tcp_req(%p), received (datao(%u) + datal(%u)) > datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, c2h_data->datal, tcp_req->req.payload.size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datal);
		goto end;
	}

	/* recv_pdu reads data into upper layer's iovs */
	recv_pdu->iovs = tcp_req->pdu.iovs;
	recv_pdu->data_iovcnt = tcp_req->pdu.data_iovcnt;
	recv_pdu->req = tcp_req;
	recv_pdu->data_len = c2h_data->datal;
	/* data digest */
	recv_pdu->ddgst_enable = tqpair->flags.host_ddgst_enable;


	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, recv_pdu, fes, error_offset);
}

static void
nvme_tcp_qpair_h2c_data_send_complete(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_transport_poll_group *group = tqpair->qpair.poll_group;
	assert(tcp_req != NULL);

	tcp_req->ordering.bits.send_ack = 1;
	if (tcp_req->r2tl_remain) {
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		assert(tcp_req->active_r2ts > 0);
		tcp_req->active_r2ts--;
		tcp_req->ordering.bits.state = NVME_TCP_REQ_ACTIVE;

		if (tcp_req->ordering.bits.r2t_waiting_h2c_complete) {
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 0;
			SPDK_DEBUGLOG(nvme, "tcp_req %p: continue r2t\n", tcp_req);
			assert(tcp_req->active_r2ts > 0);
			tcp_req->ttag = tcp_req->ttag_r2t_next;
			tcp_req->r2tl_remain = tcp_req->r2tl_remain_next;
			tcp_req->ordering.bits.state = NVME_TCP_REQ_ACTIVE_R2T;
			nvme_tcp_send_h2c_data(tcp_req);
			return;
		}

		if (group && tcp_req->iobuf_iov.iov_base) {
			spdk_iobuf_put(group->group->accel_fn_table.get_iobuf_channel(group->group->ctx),
				       tcp_req->iobuf_iov.iov_base,
				       tcp_req->iobuf_iov.iov_len);
			tcp_req->iobuf_iov.iov_base = NULL;
		}

		/* Need also call this function to free the resource */
		nvme_tcp_req_complete_safe(tcp_req);
	}
}

static void
nvme_tcp_accel_seq_finished_h2c_cb(void *cb_arg, int status)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct nvme_tcp_pdu *rsp_pdu;
	enum spdk_nvme_generic_command_status_code sc;
	struct spdk_nvme_cpl cpl;
	uint32_t *ddgst;
	uint16_t dnr = 0;

	SPDK_DEBUGLOG(nvme, "accel cpl, req %p, status %d\n", tcp_req, status);
	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqpair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		dnr = 1;
		goto fail_req;
	}
	if (spdk_unlikely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING ||
			  !spdk_nvme_qpair_is_connected(&tqpair->qpair))) {
		SPDK_DEBUGLOG(nvme, "tqpair %p, req %p accel cpl in disconnecting, outstanding %u\n",
			      tqpair, tcp_req, tqpair->qpair.num_outstanding_reqs);
		sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
		goto fail_req;
	}
	rsp_pdu = &tcp_req->pdu;
	if (tqpair->flags.host_ddgst_enable) {
		uint32_t ddgst_tmp;

		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + tcp_req->req.payload.size);
		ddgst_tmp = *ddgst;

		ddgst_tmp ^= SPDK_CRC32C_XOR;
		MAKE_DIGEST_WORD((uint8_t *)ddgst, ddgst_tmp);
	}

	rsp_pdu->iovs = &tcp_req->iobuf_iov;
	rsp_pdu->data_iovcnt = 1;
	rsp_pdu->iovcnt = rsp_pdu->data_iovcnt + rsp_pdu->has_capsule + rsp_pdu->ddgst_enable;
	rsp_pdu->has_mkeys = 1;
	/* Buffer is in local memory, clear memory domain pointer */
	assert(tcp_req->ordering.bits.has_memory_domain);
	tcp_req->ordering.bits.has_memory_domain = 0;

	if (spdk_unlikely(nvme_tcp_fill_data_mkeys(tqpair, tcp_req, rsp_pdu) != 0)) {
		SPDK_ERRLOG("Failed to fill mkeys\n");
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}
	tqpair->stats->submitted_requests++;
	if (spdk_unlikely(nvme_tcp_qpair_send_pdu(tqpair, rsp_pdu))) {
		sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto fail_req;
	}

	return;

fail_req:
	memset(&cpl, 0, sizeof(cpl));
	cpl.status.sc = sc;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;
	nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
}

static inline int
nvme_tcp_apply_accel_sequence_h2c(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;
	struct spdk_accel_sequence *accel_seq;
	struct spdk_accel_task *task;
	uint32_t *ddgst;
	bool skip_copy = false;
	int rc;

	SPDK_DEBUGLOG(nvme, "Write request with accel sequence h2c: tcp_req %p\n", tcp_req);

	accel_seq = req->accel_sequence;
	if (accel_seq) {
		task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == SPDK_ACCEL_OPC_ENCRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			task->dst_domain = NULL;
			task->dst_domain_ctx = NULL;
			task->d.iovs = &tcp_req->iobuf_iov;
			task->d.iovcnt = 1;
			skip_copy = true;
		}
	}

	/*
	 * Ddigest offload is not supported when the data are split into two and more PDUs. The SW will
	 * handle ddigest later.
	 */
	if (tqpair->flags.host_ddgst_enable && !tcp_req->r2tl_remain) {
		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + tcp_req->req.payload.size);

		if (!skip_copy) {
			rc = group->accel_fn_table.append_copy_crc32c(group->ctx, (void **)&accel_seq,
					ddgst,
					&tcp_req->iobuf_iov, 1, NULL, NULL,
					tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
					req->payload.opts->memory_domain,
					req->payload.opts->memory_domain_ctx,
					0, NULL, NULL);
			skip_copy = true;
		} else {
			rc = group->accel_fn_table.append_crc32c(group->ctx, (void **)&accel_seq, ddgst,
					&tcp_req->iobuf_iov, 1, NULL, NULL, 0, NULL, NULL);
		}
		if (spdk_unlikely(rc)) {
			if (rc != -ENOMEM) {
				SPDK_ERRLOG("Failed to append crc32 accel task, rc %d\n", rc);
			} else {
				SPDK_DEBUGLOG(nvme, "Failed to append crc32 accel task, rc %d\n", rc);
			}
			return rc;
		}
		tcp_req->ordering.bits.digest_offloaded = 1;
	}

	if (!skip_copy) {
		rc = group->accel_fn_table.append_copy(group->ctx, (void **)&accel_seq, &tcp_req->iobuf_iov, 1,
						       NULL, NULL, tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
						       req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx, NULL, NULL);
		if (spdk_unlikely(rc)) {
			return rc;
		}
	}

	tcp_req->ordering.bits.in_progress_accel = 1;
	group->accel_fn_table.finish_sequence(accel_seq, nvme_tcp_accel_seq_finished_h2c_cb, tcp_req);

	return rc;
}

static void
nvme_tcp_h2c_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct nvme_tcp_req *tcp_req = SPDK_CONTAINEROF(entry, struct nvme_tcp_req, iobuf_entry);
	int rc;

	tcp_req->iobuf_iov.iov_base = buf;

	rc = nvme_tcp_apply_accel_sequence_h2c(tcp_req);
	if (spdk_unlikely(rc)) {
		struct spdk_nvme_cpl cpl;
		struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);

		SPDK_ERRLOG("failed to apply sequence, rc %d\n", rc);
		cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		cpl.status.dnr = 1;
		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
	}
}

static void
nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t plen, pdo, alignment;
	struct spdk_nvme_cpl cpl;

	/* Reinit the send_ack and h2c_send_waiting_ack bits */
	tcp_req->ordering.bits.send_ack = 0;
	tcp_req->ordering.bits.h2c_send_waiting_ack = 0;
	rsp_pdu = &tcp_req->pdu;
	rsp_pdu->u_raw = 0;
	h2c_data = &rsp_pdu->hdr.h2c_data;

	h2c_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	plen = h2c_data->common.hlen = sizeof(*h2c_data);
	h2c_data->cccid = tcp_req->cid;
	h2c_data->ttag = tcp_req->ttag;
	h2c_data->datao = tcp_req->datao;
	h2c_data->datal = spdk_min(tcp_req->r2tl_remain, tqpair->maxh2cdata);
	tcp_req->r2tl_remain -= h2c_data->datal;


	/* Capsule header with hdsgt and possible alignment */
	rsp_pdu->has_capsule = 1;
	if (tqpair->flags.host_hdgst_enable) {
		SPDK_DEBUGLOG(nvme, "Header digest is enabled for capsule command on tcp_req=%p\n", tcp_req);
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			rsp_pdu->padding_len = alignment - plen;
			pdo = plen = alignment;
		}
	}

	rsp_pdu->capsule_len = plen;
	rsp_pdu->capsule_offset = 0;

	/* data + digest */
	rsp_pdu->data_len = h2c_data->datal;
	rsp_pdu->rw_offset = h2c_data->datao;
	rsp_pdu->iovcnt = rsp_pdu->data_iovcnt + 1;
	rsp_pdu->cb_fn = nvme_tcp_qpair_h2c_data_send_complete;

	h2c_data->common.pdo = pdo;
	plen += h2c_data->datal;

	h2c_data->common.plen = plen;
	tcp_req->datao += h2c_data->datal;
	if (!tcp_req->r2tl_remain) {
		h2c_data->common.flags |= SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU;
	}

	SPDK_DEBUGLOG(nvme, "h2c_data info: datao=%u, datal=%u, pdu_len=%u for tqpair=%p\n",
		      h2c_data->datao, h2c_data->datal, h2c_data->common.plen, tqpair);

	if (tqpair->flags.host_ddgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		h2c_data->common.plen += SPDK_NVME_TCP_DIGEST_LEN;
		rsp_pdu->ddigest_offset = 0;
		tqpair->stats->send_ddgsts++;

		/* Allocate an IO buffer and copy data to it if this H2CData PDU is the first. */
		if (h2c_data->datao == 0 && tcp_req->r2tl_remain == 0 && nvme_tcp_req_with_memory_domain(tcp_req)) {
			struct spdk_nvme_poll_group *group;
			struct spdk_iobuf_channel *iobuf_ch;
			int rc;

			group = tqpair->qpair.poll_group->group;
			if (spdk_unlikely(!group)) {
				SPDK_ERRLOG("accel_seq is only supported with poll groups\n");
				goto fail_req;
			}

			if (tqpair->flags.host_hdgst_enable) {
				uint32_t crc32c = nvme_tcp_pdu_calc_header_digest(rsp_pdu);
				MAKE_DIGEST_WORD((uint8_t *)rsp_pdu->hdr.raw + rsp_pdu->hdr.common.hlen, crc32c);
			}

			iobuf_ch = group->accel_fn_table.get_iobuf_channel(group->ctx);
			assert(iobuf_ch);
			rsp_pdu->data_len += SPDK_NVME_TCP_DIGEST_LEN;
			tcp_req->iobuf_iov.iov_len = rsp_pdu->data_len;
			tcp_req->iobuf_iov.iov_base = spdk_iobuf_get(iobuf_ch, tcp_req->iobuf_iov.iov_len,
						      &tcp_req->iobuf_entry, nvme_tcp_h2c_iobuf_get_cb);
			if (spdk_unlikely(!tcp_req->iobuf_iov.iov_base)) {
				/* Finish accel sequence once buffer is allocated */
				SPDK_DEBUGLOG(nvme, "no buffer, in progress\n");
				return;
			}

			rc = nvme_tcp_apply_accel_sequence_h2c(tcp_req);
			if (spdk_unlikely(rc)) {
				SPDK_ERRLOG("Failed to apply sequence\n");
				goto fail_req;
			}
			return;
		} else {
			rsp_pdu->ddgst_enable = 1;
			rsp_pdu->iovcnt++;
			uint32_t crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(rsp_pdu, rsp_pdu->iovs,
					  rsp_pdu->data_iovcnt);
			MAKE_DIGEST_WORD(rsp_pdu->data_digest, crc32c);
		}
	}

	if (tqpair->flags.host_hdgst_enable) {
		uint32_t crc32c = nvme_tcp_pdu_calc_header_digest(rsp_pdu);
		MAKE_DIGEST_WORD((uint8_t *)rsp_pdu->hdr.raw + rsp_pdu->hdr.common.hlen, crc32c);
	}

	rsp_pdu->has_mkeys = 1;
	if (spdk_unlikely(nvme_tcp_fill_data_mkeys(tqpair, tcp_req, rsp_pdu) != 0)) {
		SPDK_ERRLOG("Failed to fill mkeys\n");
		goto fail_req;
	}

	tqpair->stats->submitted_requests++;
	if (spdk_unlikely(nvme_tcp_qpair_send_pdu(tqpair, rsp_pdu))) {
		goto fail_req;
	}

	return;
fail_req:
	cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = 1;
	nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
}

static void
nvme_tcp_r2t_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_r2t_hdr *r2t = &pdu->hdr.r2t;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(nvme, "enter\n");
	cid = r2t->cccid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (spdk_unlikely(!tcp_req)) {
		SPDK_ERRLOG("Cannot find tcp_req for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid);
		goto end;
	}

	SPDK_DEBUGLOG(nvme, "r2t info: r2to=%u, r2tl=%u for tqpair=%p\n", r2t->r2to, r2t->r2tl,
		      tqpair);

	if (tcp_req->ordering.bits.state == NVME_TCP_REQ_ACTIVE) {
		assert(tcp_req->active_r2ts == 0);
		tcp_req->ordering.bits.state = NVME_TCP_REQ_ACTIVE_R2T;
	}

	if (spdk_unlikely(tcp_req->datao != r2t->r2to)) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2to);
		goto end;
	}

	if (spdk_unlikely((r2t->r2tl + r2t->r2to) > tcp_req->req.payload.size)) {
		SPDK_ERRLOG("Invalid R2T info for tcp_req=%p: (r2to(%u) + r2tl(%u)) exceeds payload_size(%u)\n",
			    tcp_req, r2t->r2to, r2t->r2tl, tqpair->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl);
		goto end;
	}

	tcp_req->active_r2ts++;
	if (spdk_unlikely(tcp_req->active_r2ts > tqpair->maxr2t)) {
		if (tcp_req->ordering.bits.state == NVME_TCP_REQ_ACTIVE_R2T && !tcp_req->ordering.bits.send_ack) {
			/* We receive a subsequent R2T while we are waiting for H2C transfer to complete */
			SPDK_DEBUGLOG(nvme, "received a subsequent R2T\n");
			assert(tcp_req->active_r2ts == tqpair->maxr2t + 1);
			tcp_req->ttag_r2t_next = r2t->ttag;
			tcp_req->r2tl_remain_next = r2t->r2tl;
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 1;
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			return;
		} else {
			fes = SPDK_NVME_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED;
			SPDK_ERRLOG("Invalid R2T: Maximum number of R2T exceeded! Max: %u for tqpair=%p\n", tqpair->maxr2t,
				    tqpair);
			goto end;
		}
	}

	tcp_req->ttag = r2t->ttag;
	tcp_req->r2tl_remain = r2t->r2tl;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (spdk_likely(tcp_req->ordering.bits.send_ack)) {
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		tcp_req->ordering.bits.h2c_send_waiting_ack = 1;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);

}

static void
nvme_tcp_pdu_psh_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = tqpair->recv_pdu;

	SPDK_DEBUGLOG(nvme, "enter: pdu type =%u\n", pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (spdk_unlikely(rc == 0)) {
			SPDK_ERRLOG("header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;
		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_RESP:
		nvme_tcp_icresp_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
		nvme_tcp_capsule_resp_hdr_handle(tqpair, pdu, reaped);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_hdr_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_hdr_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_R2T:
		nvme_tcp_r2t_hdr_handle(tqpair, pdu);
		break;
	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
		break;
	}
}

static inline int
nvme_nvda_tcp_readv_data(struct nvme_tcp_qpair *tqpair, struct iovec *iov, int iovcnt)
{
	int ret;

	assert(tqpair != NULL);
	assert(iov);
	assert(iovcnt);

	ret = xlio_sock_readv(tqpair, iov, iovcnt);
	if (ret > 0) {
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno != ECONNRESET) {
			SPDK_ERRLOG("xlio_sock_readv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return NVME_TCP_CONNECTION_FATAL;
}

static int
nvme_tcp_read_digest(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu, uint32_t len)
{
	struct iovec iov;

	assert(len <= SPDK_NVME_TCP_DIGEST_LEN);

	iov.iov_base = pdu->data_digest + SPDK_NVME_TCP_DIGEST_LEN - len;
	iov.iov_len = len;

	return nvme_nvda_tcp_readv_data(tqpair, &iov, 1);
}

static int
nvme_tcp_read_payload_data_memory_domain(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_pdu *recv_pdu)
{
	struct nvme_tcp_req *tcp_req = recv_pdu->req;
	struct xlio_sock_buf *sock_buf;
	int ret = 0;

	if (recv_pdu->data_len > recv_pdu->rw_offset) {
		size_t len = recv_pdu->data_len - recv_pdu->rw_offset;

		ret = xlio_sock_recv_zcopy(tqpair, len, &sock_buf);
		if (spdk_unlikely(ret <= 0)) {
			goto fail;
		}
		SPDK_DEBUGLOG(nvme, "tqpair %p, pdu %p: requested %zu, got %d bytes\n", tqpair, recv_pdu, len, ret);

		if (tcp_req->sock_buf) {
			struct xlio_sock_buf *cur_buf = tcp_req->sock_buf;

			while (cur_buf->next) {
				cur_buf = cur_buf->next;
			}
			cur_buf->next = sock_buf;
		} else {
			tcp_req->sock_buf = sock_buf;
		}

		if ((size_t)ret != len) {
			/* Part of data is not received, so return directly */
			return ret;
		}
	}

	if (recv_pdu->ddgst_enable) {
		int ret_dgst = nvme_tcp_read_digest(tqpair, recv_pdu, SPDK_NVME_TCP_DIGEST_LEN +
						    recv_pdu->data_len - recv_pdu->rw_offset - ret);
		if (spdk_unlikely(ret_dgst < 0)) {
			ret = ret_dgst;
			goto fail;
		}

		ret += ret_dgst;
	}
	return ret;

fail:
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno != ECONNRESET) {
			SPDK_ERRLOG("xlio_sock_readv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return NVME_TCP_CONNECTION_FATAL;
}

static inline bool
_nvme_tcp_sgl_append_multi(struct spdk_iov_sgl *s, struct iovec *iov, int iovcnt)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (!spdk_iov_sgl_append(s, iov[i].iov_base, iov[i].iov_len)) {
			return false;
		}
	}

	return true;
}

static inline int
nvme_tcp_build_payload_iovs(struct iovec *iov, int iovcnt, struct nvme_tcp_pdu *pdu,
			    bool ddgst_enable)
{
	struct spdk_iov_sgl sgl;

	if (iovcnt == 0) {
		return 0;
	}

	spdk_iov_sgl_init(&sgl, iov, iovcnt, pdu->rw_offset);

	if (!_nvme_tcp_sgl_append_multi(&sgl, pdu->iovs, pdu->data_iovcnt)) {
		goto end;
	}

	/* Data Digest */
	if (ddgst_enable) {
		spdk_iov_sgl_append(&sgl, pdu->data_digest, SPDK_NVME_TCP_DIGEST_LEN);
	}

end:
	return iovcnt - sgl.iovcnt;
}

static inline int
nvme_nvda_tcp_read_payload_data(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct iovec iov[NVME_TCP_MAX_SGL_DESCRIPTORS + 1];
	int iovcnt;

	/* TODO: find better way, without copying to iovs on the stack */
	iovcnt = nvme_tcp_build_payload_iovs(iov, NVME_TCP_MAX_SGL_DESCRIPTORS + 1, pdu, pdu->ddgst_enable);
	assert(iovcnt >= 0);

	return nvme_nvda_tcp_readv_data(tqpair, iov, iovcnt);
}

static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair, uint32_t *reaped, uint32_t max_completions)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t data_len;
	enum nvme_tcp_pdu_recv_state prev_state;
	struct iovec iov;

	*reaped = tqpair->async_complete;
	tqpair->async_complete = 0;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		if (*reaped >= max_completions) {
			break;
		}

		prev_state = tqpair->recv_state;
		pdu = tqpair->recv_pdu;
		switch (tqpair->recv_state) {
		/* If in a new state */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			if (pdu) {
				pdu->ch_valid_bytes = 0;
				pdu->psh_valid_bytes = 0;
				pdu->has_hdgst = 0;
				pdu->rw_offset = 0;
				pdu->ddgst_enable = 0;
			}

			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			break;
		/* Wait for the pdu common header */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			if (!pdu) {
				struct spdk_nvme_tcp_common_pdu_hdr common_hdr;
				iov.iov_base = &common_hdr;
				iov.iov_len = sizeof(struct spdk_nvme_tcp_common_pdu_hdr);

				rc = nvme_nvda_tcp_readv_data(tqpair, &iov, 1);
				if (spdk_unlikely(rc < 0)) {
					nvme_tcp_qpair_set_recv_quiescing(tqpair);
					break;
				} else if (rc == 0) {
					return NVME_TCP_PDU_IN_PROGRESS;
				}

				pdu = tqpair->recv_pdu = nvme_tcp_recv_pdu_get(tqpair);
				if (spdk_unlikely(!pdu)) {
					SPDK_ERRLOG("Failed to get recv pdu\n");
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
					break;
				}

				pdu->ch_valid_bytes = 0;
				pdu->psh_valid_bytes = 0;
				pdu->has_hdgst = 0;
				pdu->rw_offset = 0;
				pdu->ddgst_enable = 0;

				memcpy(&pdu->hdr.common, &common_hdr, rc);
				pdu->ch_valid_bytes = rc;
			} else {
				iov.iov_base = (uint8_t *)&pdu->hdr.common + pdu->ch_valid_bytes;
				iov.iov_len = sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes;

				assert(pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr));
				rc = nvme_nvda_tcp_readv_data(tqpair, &iov, 1);
				if (spdk_unlikely(rc < 0)) {
					nvme_tcp_qpair_set_recv_quiescing(tqpair);
					break;
				}
				pdu->ch_valid_bytes += rc;
			}

			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* The command header of this PDU has now been read from the socket. */
			nvme_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			assert(pdu->psh_valid_bytes < pdu->psh_len);
			iov.iov_base = (uint8_t *)&pdu->hdr.raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) +
				       pdu->psh_valid_bytes;
			iov.iov_len = pdu->psh_len - pdu->psh_valid_bytes;
			rc = nvme_nvda_tcp_readv_data(tqpair, &iov, 1);
			if (spdk_unlikely(rc < 0)) {
				nvme_tcp_qpair_set_recv_quiescing(tqpair);
				break;
			}

			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			nvme_tcp_pdu_psh_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			data_len += pdu->ddgst_enable * SPDK_NVME_TCP_DIGEST_LEN;

			if (nvme_tcp_req_with_memory_domain(pdu->req)) {
				rc = nvme_tcp_read_payload_data_memory_domain(tqpair, pdu);
			} else {
				rc = nvme_nvda_tcp_read_payload_data(tqpair, pdu);
			}
			if (spdk_unlikely(rc < 0)) {
				nvme_tcp_qpair_set_recv_quiescing(tqpair);
				break;
			}

			pdu->rw_offset += rc;
			if (pdu->rw_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			assert(pdu->rw_offset == data_len);
			/* All of this PDU has now been read from the socket. */
			nvme_tcp_pdu_payload_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_QUIESCING:
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				if (nvme_qpair_get_state(&tqpair->qpair) == NVME_QPAIR_DISCONNECTING) {
					nvme_transport_ctrlr_disconnect_qpair_done(&tqpair->qpair);
				}
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
			}
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			if (pdu) {
				memset(pdu, 0, sizeof(struct nvme_tcp_pdu));
			}
			return NVME_TCP_PDU_FATAL;
		default:
			assert(0);
			break;
		}
	} while (prev_state != tqpair->recv_state);

	return rc > 0 ? 0 : rc;
}

static void
nvme_tcp_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct nvme_tcp_req *tcp_req, *tmp;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (nvme_request_check_timeout(&tcp_req->req, tcp_req->cid, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

static int nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

static inline int
nvme_tcp_qpair_process_completions_common(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	if (max_completions == 0) {
		max_completions = spdk_max(tqpair->num_entries, 1);
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);
	}

	reaped = 0;
	rc = nvme_tcp_read_pdu(tqpair, &reaped, max_completions);
	if (spdk_unlikely(rc < 0)) {
		SPDK_DEBUGLOG(nvme, "Error polling CQ! qpair %d, rc %d(%d): %s\n",
			      tqpair->qpair.id, rc, errno, spdk_strerror(errno));
		goto fail;
	}

	if (tqpair->qpair.ctrlr->timeout_enabled) {
		nvme_tcp_qpair_check_timeout(qpair);
	}

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		rc = nvme_tcp_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc != 0 && rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to connect tqpair=%p\n", tqpair);
			goto fail;
		} else if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			nvme_qpair_resubmit_requests(qpair, tqpair->num_entries);
		}
	}

	return reaped;
fail:

	/*
	 * Since admin queues take the ctrlr_lock before entering this function,
	 * we can call nvme_transport_ctrlr_disconnect_qpair. For other qpairs we need
	 * to call the generic function which will take the lock for us.
	 */
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_transport_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
	} else {
		nvme_ctrlr_disconnect_qpair(qpair);
	}
	return -ENXIO;
}

static int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair;
	int rc = 0;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
		return nvme_tcp_qpair_process_completions_common(qpair, max_completions);
	}

	tqpair = nvme_tcp_qpair(qpair);

	spdk_spin_lock(&g_xlio_admin_group_lock);

	if (tqpair->state >= NVME_TCP_QPAIR_STATE_SOCK_CONNECTED) {
		if (tqpair->flags.closed) {
			if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTING) {
				SPDK_ERRLOG("tqpair=%p sock 0x%lx is closed: errno %d(%s)\n",
					    tqpair, tqpair->xlio_sock, errno, spdk_strerror(errno));
			}
			if (tqpair->qpair.ctrlr->timeout_enabled) {
				nvme_tcp_qpair_check_timeout(qpair);
			}
			if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
				if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
					nvme_transport_ctrlr_disconnect_qpair_done(qpair);
				}
				goto out;
			}
			qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
			nvme_transport_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
			rc = -ENXIO;
			goto out;
		} else if (tqpair->flags.pending_send) {
			xlio_socket_flush(tqpair->xlio_sock);
			tqpair->flags.pending_send = false;
		}
	}

	rc = nvme_tcp_qpair_process_completions_common(qpair, max_completions);

out:
	spdk_spin_unlock(&g_xlio_admin_group_lock);

	return rc;
}

static inline void
nvme_tcp_qpair_resubmit_accel_nomem(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu, *tmp;
	int rc;

	TAILQ_FOREACH_SAFE(pdu, &tqpair->accel_nomem_queue, tailq, tmp) {
		TAILQ_REMOVE(&tqpair->accel_nomem_queue, pdu, tailq);
		SPDK_DEBUGLOG(nvme, "tqpair %p, resubmit pdu %p\n", tqpair, pdu);
		rc = nvme_tcp_apply_accel_sequence_c2h(tqpair, pdu);
		if (rc) {
			break;
		}
	}
}

static void
nvme_tcp_qpair_process_pending_events(struct nvme_tcp_qpair *tqpair)
{
	int32_t num_completions;
	uint32_t num_free_entries;
	struct nvme_tcp_poll_group *group = tqpair->group;

	if (tqpair->flags.needs_resubmit) {
		tqpair->flags.needs_resubmit = false;
		num_free_entries = tqpair->num_entries - tqpair->stats->outstanding_reqs;
		SPDK_DEBUGLOG(nvme, "tqpair %p %u\n", tqpair, tqpair->qpair.id);
		if (num_free_entries > 0) {
			nvme_qpair_resubmit_requests(&tqpair->qpair, num_free_entries);
		}
	}

	if (spdk_unlikely(tqpair->flags.has_accel_nomem_pdus)) {
		/* For now it only works for C2H payload */
		tqpair->flags.has_accel_nomem_pdus = 0;
		nvme_tcp_qpair_resubmit_accel_nomem(tqpair);
	}

	num_completions = spdk_nvme_qpair_process_completions(&tqpair->qpair, group->completions_per_qpair);

	if (group->num_completions >= 0 && num_completions >= 0) {
		group->num_completions += num_completions;
		group->stats.nvme_completions += num_completions;
	} else {
		group->num_completions = -ENXIO;
	}
}

static int
nvme_tcp_qpair_icreq_send(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_ic_req *ic_req;
	struct nvme_tcp_pdu *pdu;

	pdu = tqpair->send_pdu;
	memset(tqpair->send_pdu, 0, sizeof(*tqpair->send_pdu));
	memset(&tqpair->ctrl_hdr, 0, sizeof(tqpair->ctrl_hdr));
	ic_req = &tqpair->ctrl_hdr.ic_req;

	ic_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	ic_req->common.hlen = ic_req->common.plen = sizeof(*ic_req);
	ic_req->pfv = 0;
	ic_req->maxr2t = NVME_TCP_MAX_R2T_DEFAULT - 1;
	ic_req->hpda = NVME_TCP_HPDA_DEFAULT;

	ic_req->dgst.bits.hdgst_enable = tqpair->qpair.ctrlr->opts.header_digest;
	ic_req->dgst.bits.ddgst_enable = tqpair->qpair.ctrlr->opts.data_digest;

	nvme_tcp_qpair_write_control_pdu(tqpair, pdu, nvme_tcp_send_icreq_complete);

	tqpair->icreq_timeout_tsc = spdk_get_ticks() + (NVME_TCP_TIME_OUT_IN_SECONDS * spdk_get_ticks_hz());
	return 0;
}

static void
nvme_tcp_qpair_connect_sock_done(struct nvme_tcp_qpair *tqpair, int err)
{
	struct spdk_nvme_qpair *qpair = &tqpair->qpair;
	struct nvme_tcp_poll_group *tgroup = NULL;
	void *tcp_reqs;
	struct spdk_rdma_utils_memory_translation mem_translation = {};
	char *tcp_mem_domain = getenv("SPDK_NVDA_TCP_USE_TCP_MEM_DOMAIN");
	int rc;

	SPDK_NOTICELOG("Socket connect done: tqpair %p, qid %u, xlio_sock %lx, err %d\n",
		       tqpair, tqpair->qpair.id, tqpair->xlio_sock, err);

	assert(tqpair->state == NVME_TCP_QPAIR_STATE_CONNECTING ||
	       tqpair->state == NVME_TCP_QPAIR_STATE_INVALID);

	if (err) {
		goto fail;
	}

	if (tcp_mem_domain) {
		tqpair->memory_domain = spdk_rdma_utils_get_memory_domain(tqpair->pd,
					SPDK_DMA_DEVICE_TYPE_RDMA_TCP);
	} else {
		tqpair->memory_domain = spdk_rdma_utils_get_memory_domain(tqpair->pd,
					SPDK_DMA_DEVICE_TYPE_RDMA);
	}

	if (!tqpair->memory_domain) {
		SPDK_ERRLOG("Failed to get memory domain\n");
		goto fail;
	}

	SPDK_DEBUGLOG(nvme, "TCP qpair %p %u, PD %p\n", tqpair, tqpair->qpair.id, tqpair->pd);

	tqpair->mem_map = spdk_rdma_utils_create_mem_map(tqpair->pd, NULL,
			  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!tqpair->mem_map) {
		SPDK_ERRLOG("Failed to create memory map\n");
		goto fail;
	}

	tgroup = qpair->poll_group ? nvme_tcp_poll_group(qpair->poll_group) : NULL;
	tcp_reqs = (qpair->poll_group && tgroup->tcp_reqs) ?
		   tgroup->tcp_reqs : tqpair->tcp_reqs;
	rc = spdk_rdma_utils_get_translation(tqpair->mem_map, tcp_reqs,
					     tqpair->num_entries * sizeof(struct nvme_tcp_req),
					     &mem_translation);
	if (rc) {
		SPDK_ERRLOG("Failed to get mkey for PDUs\n");
		goto fail;
	}
	tqpair->pdus_mkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);

	if (nvme_qpair_is_admin_queue(&tqpair->qpair)) {
		if (nvme_tcp_memory_domain_enabled() &&
		    getenv("SPDK_NVDA_TCP_DISABLE_ACCEL_SEQ") == NULL) {
			qpair->ctrlr->flags |= SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;
		} else {
			SPDK_NOTICELOG("Accel sequence support disabled\n");
		}
	}

	/* We can send icreq only when user asked to connect qpair. If it didn't happen yet, just wait. */
	if (tqpair->state == NVME_TCP_QPAIR_STATE_CONNECTING) {
		tqpair->state = NVME_TCP_QPAIR_STATE_ICREQ_SEND;
		if (tqpair->qpair.poll_group && !tqpair->flags.pending_events) {
			tgroup = nvme_tcp_poll_group(qpair->poll_group);
			TAILQ_INSERT_TAIL(&tgroup->pending_events, tqpair, link);
			tqpair->flags.pending_events = true;
		}
	} else {
		tqpair->state = NVME_TCP_QPAIR_STATE_SOCK_CONNECTED;
	}

	return;

fail:
	tqpair->state = NVME_TCP_QPAIR_STATE_SOCK_CONNECT_FAIL;
}

static int
nvme_tcp_qpair_connect_sock(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct nvme_tcp_qpair *tqpair;
	int family;
	long int port, src_port = 0;
	struct spdk_sock_impl_opts impl_opts;
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;
	xlio_poll_group_t xgroup;

	tqpair = nvme_tcp_qpair(qpair);

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		rc = -1;
		return rc;
	}

	SPDK_DEBUGLOG(nvme, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	SPDK_DEBUGLOG(nvme, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid, &port);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_parse_addr() failed\n");
		return rc;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_parse_addr(&src_addr, family,
				     ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL,
				     ctrlr->opts.src_svcid[0] ? ctrlr->opts.src_svcid : NULL,
				     &src_port);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_parse_addr() failed\n");
			return rc;
		}
	}

	rc = spdk_sock_impl_get_opts("xlio", &impl_opts, &impl_opts_size);
	if (rc) {
		SPDK_ERRLOG("Failed to get xlio options\n");
		return rc;
	}

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.priority = ctrlr->trid.priority;
	opts.zcopy = !nvme_qpair_is_admin_queue(qpair);
	opts.src_addr = ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL;
	opts.src_port = src_port;
	if (ctrlr->opts.transport_ack_timeout) {
		opts.ack_timeout = 1ULL << ctrlr->opts.transport_ack_timeout;
	}
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = sizeof(impl_opts);
	if (nvme_qpair_is_admin_queue(qpair)) {
		xgroup = xlio_sock_get_admin_group();
	} else {
		struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
		assert(group != NULL);
		xgroup = group->xgroup;
	}
	assert(xgroup != 0);

	rc = xlio_sock_init(tqpair, ctrlr->trid.traddr, port, &opts,
			    xgroup, ctrlr->opts.vlan_tag);
	if (rc) {
		SPDK_ERRLOG("tqpair connection error of tqpair=%p with addr=%s, port=%ld, rc %d\n",
			    tqpair, ctrlr->trid.traddr, port, rc);
		rc = -1;
		return rc;
	}

	return 0;
}

static int
nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	int rc;

	tqpair = nvme_tcp_qpair(qpair);

	/* Prevent this function from being called recursively, as it could lead to issues with
	 * nvme_fabric_qpair_connect_poll() if the connect response is received in the recursive
	 * call.
	 */
	if (qpair->in_connect_poll) {
		return -EAGAIN;
	}

	qpair->in_connect_poll = 1;

	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_INVALID:
	case NVME_TCP_QPAIR_STATE_CONNECTING:
	case NVME_TCP_QPAIR_STATE_SOCK_CONNECTED:
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_SOCK_CONNECT_FAIL:
		SPDK_ERRLOG("Failed to connect socket for tqpair=%p\n", tqpair);
		rc = -ENETDOWN;
		break;
	case NVME_TCP_QPAIR_STATE_ICREQ_SEND:
		tqpair->maxr2t = NVME_TCP_MAX_R2T_DEFAULT;
		rc = nvme_tcp_qpair_icreq_send(tqpair);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to send ic_req, rc %d\n", rc);
			break;
		}
		tqpair->state = NVME_TCP_QPAIR_STATE_ICRESP_WAIT;
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_ICRESP_WAIT:
	case NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED:
		if (spdk_get_ticks() > tqpair->icreq_timeout_tsc) {
			SPDK_ERRLOG("Failed to construct the tqpair=%p via correct icresp\n", tqpair);
			rc = -ETIMEDOUT;
			break;
		}
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
		if (ctrlr->lazy_fabric_connect) {
			/*
			 * Inform the caller that the ctrlr is fully constructed, added
			 * to the init list and connected up to the stage before FABRIC CONNECT.
			 */
			if (ctrlr->construct_cb) {
				/*
				 * Increase the ref count before calling construct_cb() as the user may
				 * call nvme_detach() immediately.
				 *
				 * We will not do this before attach_cb() in case lazy_fabric_connect
				 * is true.
				 */
				nvme_ctrlr_proc_get_ref(ctrlr);
				ctrlr->construct_cb(ctrlr->cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
				ctrlr->construct_cb = NULL;
			}
		} else {
			/* Proceed with FABRIC CONNECT as usually. */
			rc = nvme_fabric_qpair_connect_async(&tqpair->qpair, tqpair->num_entries + 1);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
				break;
			}
			tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL;
		}
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
		rc = nvme_fabric_qpair_connect_poll(&tqpair->qpair);
		if (rc == 0) {
			tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_TCP_QPAIR_STATE_RUNNING:
		rc = 0;
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	qpair->in_connect_poll = 0;
	return rc;
}

static int
nvme_tcp_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;
	struct nvme_tcp_qpair *tqpair;

	tqpair = nvme_tcp_qpair(qpair);

	if (qpair->poll_group) {
		struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(qpair->poll_group);

		tqpair->recv_pdu = NULL;
		tqpair->stats = &tgroup->stats;
		tqpair->flags.shared_stats = true;

		if (tgroup->tcp_reqs != NULL) {
			tqpair->flags.use_poll_group_req_pool = 1;
			qpair->active_free_req = &qpair->poll_group->free_req;
		}
	}

	if (!tqpair->xlio_sock) {
		rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
		if (rc < 0) {
			return rc;
		}
	}

	if (qpair->poll_group) {
		tqpair->group = nvme_tcp_poll_group(qpair->poll_group);
	} else if (!tqpair->stats) {
		tqpair->stats = calloc(1, sizeof(*tqpair->stats));
		if (!tqpair->stats) {
			SPDK_ERRLOG("tcp stats memory allocation failed\n");
			return -ENOMEM;
		}
		tqpair->flags.shared_stats = false;
	}

	/* Explicitly set the recv_state of tqpair */
	if (tqpair->recv_state != NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	}

	/* We can send icreq only when socket is connected. If it didn't happen yet, just wait. */
	if (tqpair->state == NVME_TCP_QPAIR_STATE_SOCK_CONNECTED) {
		tqpair->state = NVME_TCP_QPAIR_STATE_ICREQ_SEND;
		if (tqpair->qpair.poll_group && !tqpair->flags.pending_events) {
			struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(qpair->poll_group);
			TAILQ_INSERT_TAIL(&tgroup->pending_events, tqpair, link);
			tqpair->flags.pending_events = true;
		}
	} else {
		tqpair->state = NVME_TCP_QPAIR_STATE_CONNECTING;
	}

	return rc;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests, bool async)
{
	struct nvme_tcp_qpair *tqpair;
	struct spdk_nvme_qpair *qpair;
	size_t tqpair_size_padded;
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		SPDK_ERRLOG("Failed to create qpair with size %u. Minimum queue size is %d.\n",
			    qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	tqpair_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_qpair), 64);
	tqpair = spdk_zmalloc(tqpair_size_padded, 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!tqpair) {
		SPDK_ERRLOG("failed to create tqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	tqpair->num_entries = qsize - 1;
	qpair = &tqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, 1, num_requests, async);
	if (rc != 0) {
		spdk_free(tqpair);
		return NULL;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);
		return NULL;
	}

	return qpair;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests, opts->async_mode);
}

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

static spdk_nvme_ctrlr_t *
nvme_tcp_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			 const struct spdk_nvme_ctrlr_opts *opts,
			 void *devhandle)
{
	struct nvme_tcp_ctrlr *tctrlr;
	int rc;

	if (!spdk_xlio_is_initialized()) {
		SPDK_ERRLOG("XLIO is not initialized\n");
		return NULL;
	}

	tctrlr = calloc(1, sizeof(*tctrlr));
	if (tctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	tctrlr->ctrlr.opts = *opts;
	tctrlr->ctrlr.trid = *trid;

	if (opts->transport_ack_timeout > NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		SPDK_NOTICELOG("transport_ack_timeout exceeds max value %d, use max value\n",
			       NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		tctrlr->ctrlr.opts.transport_ack_timeout = NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;
	}

	rc = nvme_ctrlr_construct(&tctrlr->ctrlr);
	if (rc != 0) {
		free(tctrlr);
		return NULL;
	}

	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       tctrlr->ctrlr.opts.admin_queue_size, 0,
			       tctrlr->ctrlr.opts.admin_queue_size + 1, true);
	if (!tctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		nvme_tcp_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	if (nvme_ctrlr_add_process(&tctrlr->ctrlr, 0) != 0) {
		SPDK_ERRLOG("nvme_ctrlr_add_process() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	return &tctrlr->ctrlr;
}

static uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* We can handle only IO that fits into iobuf if data digest is enabled. */
	if (ctrlr->opts.data_digest) {
		struct spdk_iobuf_opts iobuf_opts;

		spdk_iobuf_get_opts(&iobuf_opts, sizeof(struct spdk_iobuf_opts));
		/* Reserve 4 bytes to store ddigest immediately after data buffer */
		return iobuf_opts.large_bufsize - SPDK_NVME_TCP_DIGEST_LEN;
	}
	/* In other cases, TCP transport doesn't limit maximum IO transfer size. */
	return UINT32_MAX;
}

static uint16_t
nvme_tcp_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * We do not support >1 SGE in the initiator currently,
	 *  so we can only return 1 here.  Once that support is
	 *  added, this should return ctrlr->cdata.nvmf_specific.msdbd
	 *  instead.
	 */
	return NVME_TCP_MAX_SGL_DESCRIPTORS;
}

static int
nvme_tcp_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				int (*iter_fn)(struct nvme_request *req, void *arg),
				void *arg)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_req *tcp_req, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		rc = iter_fn(&tcp_req->req, arg);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static void
nvme_tcp_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (tcp_req->req.cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, false);
	}
}

static struct spdk_nvme_transport_poll_group *
nvme_tcp_poll_group_create(void)
{
	struct nvme_tcp_poll_group *group;
	struct nvme_tcp_req	*tcp_req;
	struct nvme_tcp_pdu	*pdu;
	size_t req_size_padded, pdu_size_padded;
	uint16_t num_requests, i;
	int rc;

	if (!spdk_xlio_is_initialized()) {
		SPDK_ERRLOG("XLIO is not initialized\n");
		return NULL;
	}

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	TAILQ_INIT(&group->pending_events);

	rc = nvme_transport_poll_group_init(&group->group, 0);
	if (rc != 0) {
		free(group);
		return NULL;
	}

	num_requests = g_spdk_nvme_transport_opts.poll_group_requests;

	if (num_requests != 0) {
		req_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_req), 64);
		group->tcp_reqs = spdk_zmalloc(num_requests * req_size_padded, 64, NULL,
					       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (group->tcp_reqs == NULL) {
			SPDK_ERRLOG("Failed to allocate tcp_reqs on poll group %p\n", group);
			goto fail;
		}

		for (i = 0; i < num_requests; i++) {
			tcp_req = (struct nvme_tcp_req *)((uint8_t *)group->tcp_reqs + i * req_size_padded);
			tcp_req->cid = UINT16_MAX;
			STAILQ_INSERT_HEAD(&group->group.free_req, &tcp_req->req, stailq);
		}

		pdu_size_padded = SPDK_ALIGN_CEIL(sizeof(struct nvme_tcp_pdu), 64);

		/* @todo: what should be the size of recv pdus pool? */
		group->recv_pdus = spdk_zmalloc(num_requests * pdu_size_padded, 0x1000, NULL,
						SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

		if (group->recv_pdus == NULL) {
			SPDK_ERRLOG("Failed to allocate recv_pdus on poll group %p\n", group);
			goto fail;
		}

		TAILQ_INIT(&group->free_pdus);
		for (i = 0; i < num_requests; i++) {
			pdu = (struct nvme_tcp_pdu *)((uint8_t *)group->recv_pdus + i * pdu_size_padded);
			TAILQ_INSERT_TAIL(&group->free_pdus, pdu, tailq);
		}
	}

	rc = xlio_sock_poll_group_create(group);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate sock group.\n");
		goto fail;
	}

	return &group->group;

fail:
	nvme_transport_poll_group_deinit(&group->group);
	spdk_free(group->tcp_reqs);
	spdk_free(group->recv_pdus);
	free(group);
	return NULL;
}

static int64_t
nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
					uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	struct nvme_tcp_qpair *tqpair;
	int num_events;

	group->completions_per_qpair = completions_per_qpair;
	group->num_completions = 0;
	group->stats.polls++;

	num_events = xlio_sock_group_impl_poll(group, MAX_EVENTS_PER_POLL);

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		tqpair = nvme_tcp_qpair(qpair);
		if (tqpair->zcopy_tx_pdus > 0) {
			SPDK_DEBUGLOG(nvme,
				      "qpair %p %u, sock 0x%lx: defer close, zcopy_tx_pdus %u\n",
				      tqpair, qpair->id, tqpair->xlio_sock, tqpair->zcopy_tx_pdus);
			continue;
		}

		nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);

		if (tqpair->consumed_packets > 0) {
			SPDK_DEBUGLOG(nvme,
				      "qpair %p %u, sock 0x%lx: defer close, consumed_packets %u\n",
				      tqpair, qpair->id, tqpair->xlio_sock,
				      tqpair->consumed_packets);
			continue;
		}

		nvme_qpair_disconnect_release_xlio_packets(tqpair);

		if (!tqpair->flags.closed) {
			int rc = xlio_sock_close(tqpair);
			if (rc) {
				SPDK_DEBUGLOG(nvme, "qpair %p %u: fail to close sock 0x%lx\n",
					      tqpair, qpair->id, tqpair->xlio_sock);
				continue;
			}
		}

		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				nvme_transport_ctrlr_disconnect_qpair_done(qpair);
			}
		}

		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTED) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
		}
	}

	if (spdk_unlikely(num_events < 0)) {
		return num_events;
	}

	group->stats.idle_polls += !num_events;
	group->stats.socket_completions += num_events;

	return group->num_completions;
}

static int
nvme_tcp_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	int rc;
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	rc = xlio_sock_group_impl_close(group);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to close the sock group for a tcp poll group.\n");
		assert(false);
	}

	if (group->impl_opts.buffers_pool_size != 0) {
		pthread_mutex_lock(&g_xlio_pool_mutex);
		xlio_sock_put_buffers_pool_unsafe();
		pthread_mutex_unlock(&g_xlio_pool_mutex);
	}

	nvme_transport_poll_group_deinit(&group->group);
	spdk_free(group->tcp_reqs);
	spdk_free(group->recv_pdus);
	free(tgroup);

	return 0;
}

static int
nvme_tcp_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			      struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_tcp_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = nvme_tcp_poll_group(tgroup);

	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for TCP stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_CUSTOM_FABRICS;
	snprintf(stats->trname, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", "NVDA_TCP");
	memcpy(&stats->tcp, &group->stats, sizeof(group->stats));

	*_stats = stats;

	return 0;
}

static void
nvme_tcp_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
}

static bool
nvme_tcp_memory_domain_enabled(void)
{
	const char *module_name;

	return getenv("SPDK_NVDA_TCP_DISABLE_MEM_DOMAIN") == NULL &&
	       spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_COPY, &module_name) == 0 &&
	       strcmp(module_name, "mlx5") == 0;
}

static int
nvme_tcp_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_memory_domain **domains, int array_size)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(ctrlr->adminq);

	if (!tqpair->memory_domain || !nvme_tcp_memory_domain_enabled()) {
		SPDK_NOTICELOG("Memory domain support disabled\n");
		return 0;
	} else if (domains && array_size > 0) {
		domains[0] = tqpair->memory_domain;
	}

	return 1;
}

static const struct spdk_nvme_transport_ops tcp_ops = {
	.name = "NVDA_TCP",
	.type = SPDK_NVME_TRANSPORT_CUSTOM_FABRICS,
	.ctrlr_construct = nvme_tcp_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_tcp_ctrlr_destruct,
	.ctrlr_enable = nvme_tcp_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_tcp_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_tcp_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_tcp_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_tcp_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_tcp_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_tcp_ctrlr_disconnect_qpair,

	.ctrlr_get_memory_domains = nvme_tcp_ctrlr_get_memory_domains,

	.qpair_abort_reqs = nvme_tcp_qpair_abort_reqs,
	.qpair_reset = nvme_tcp_qpair_reset,
	.qpair_submit_request = nvme_tcp_qpair_submit_request,
	.qpair_process_completions = nvme_tcp_qpair_process_completions,
	.qpair_iterate_requests = nvme_tcp_qpair_iterate_requests,
	.admin_qpair_abort_aers = nvme_tcp_admin_qpair_abort_aers,

	.poll_group_create = nvme_tcp_poll_group_create,
	.poll_group_process_completions = nvme_tcp_poll_group_process_completions,
	.poll_group_destroy = nvme_tcp_poll_group_destroy,
	.poll_group_get_stats = nvme_tcp_poll_group_get_stats,
	.poll_group_free_stats = nvme_tcp_poll_group_free_stats,
};

SPDK_NVME_TRANSPORT_REGISTER(tcp, &tcp_ops);

static void
nvme_nvda_tcp(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"NVME_NVDA_TCP_SUBMIT", TRACE_NVME_NVDA_TCP_SUBMIT,
			OWNER_NVME_NVDA_TCP_QP, OBJECT_NVME_NVDA_TCP_REQ, 1,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 4 }
			}
		},
		{
			"NVME_NVDA_TCP_COMPLETE", TRACE_NVME_NVDA_TCP_COMPLETE,
			OWNER_NVME_NVDA_TCP_QP, OBJECT_NVME_NVDA_TCP_REQ, 0,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "cpl", SPDK_TRACE_ARG_TYPE_PTR, 4 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_NVDA_TCP_REQ, 'p');
	spdk_trace_register_owner(OWNER_NVME_NVDA_TCP_QP, "q");
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
SPDK_TRACE_REGISTER_FN(nvme_nvda_tcp, "nvme_nvda_tcp", TRACE_GROUP_NVME_NVDA_TCP)
