/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"

#include <sys/epoll.h>
#include <linux/errqueue.h>

#include <dlfcn.h>

#include <infiniband/verbs.h>

#include "spdk/log.h"
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/sock.h"
#include "spdk_internal/event.h"

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/crc32.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/bit_pool.h"
#include "spdk/dma.h"
#include "spdk/accel.h"
#include "spdk/util.h"

#include "spdk_internal/nvme_nvda_tcp.h"
#include "spdk_internal/trace_defs.h"
#include "spdk_internal/rdma.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/xlio.h"
#include "spdk/accel_module.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define XLIO_PACKETS_BUF_SIZE 128

#if !defined(SO_ZEROCOPY) && !defined(MSG_ZEROCOPY)
#error "XLIO requires zcopy"
#endif

#define NVME_TCP_MAX_R2T_DEFAULT		1

struct xlio_sock_packet {
	struct xlio_socketxtreme_packet_desc_t xlio_packet;
	int refs;
	STAILQ_ENTRY(xlio_sock_packet) link;
};

struct xlio_sock_buf {
	struct spdk_sock_buf sock_buf;
	struct xlio_sock_packet *packet;
};

struct spdk_xlio_ring_fd {
	int			ring_fd;
	int			refs;
	TAILQ_ENTRY(spdk_xlio_ring_fd)	link;
};

struct spdk_xlio_sock {
	/* TODO: do we need both link and link_send entries? */
	TAILQ_ENTRY(spdk_xlio_sock)	link;
	TAILQ_ENTRY(spdk_xlio_sock)	link_send;
	TAILQ_HEAD(, nvme_tcp_pdu)	queued_pdus;
	TAILQ_HEAD(, nvme_tcp_pdu)	pending_pdus;
	STAILQ_HEAD(, xlio_sock_packet)	received_packets;

	struct xlio_packets_pool	*xlio_packets_pool;
	struct xlio_buff_t		*cur_xlio_buf;
	struct spdk_xlio_sock_group_impl *group_impl;
	struct ibv_pd			*pd;

	int				fd;
	uint32_t			sendmsg_idx;
	uint16_t			consumed_packets;
	uint16_t			cur_offset;
	union {
		struct {
			uint8_t pending_recv: 1;
			uint8_t pending_send: 1;
			uint8_t zcopy: 1;
			uint8_t recv_zcopy: 1;
			uint8_t disconnected: 1;
			uint8_t closed: 1;
			uint8_t connect_notified: 1;
			uint8_t in_complete_cb: 1;
		} flags;
		uint8_t flags_raw;
	};
	uint8_t			queued_iovcnt;

	/* "Cold" data starts here */
	uint64_t			batch_start_tsc;
	struct spdk_xlio_ring_fd	*ring_fd;
	int				batch_nr;
};

SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct xlio_buff_t,
				      len) <= SPDK_SIZEOF_MEMBER(struct spdk_xlio_sock, cur_offset),
		   "type of xlio_buff_t::len changed, update sock::cur_offset");

SPDK_STATIC_ASSERT(IOV_BATCH_SIZE <= UINT8_MAX,
		   "Type of spdk_xlio_sock::queued_iovcnt must be extended");

struct spdk_xlio_sock_group_impl {
	TAILQ_HEAD(, spdk_xlio_ring_fd)	ring_fd;
	TAILQ_HEAD(pending_recv_head, spdk_xlio_sock)	pending_recv;
	TAILQ_HEAD(, spdk_xlio_sock)	pending_send;
	struct xlio_packets_pool	*xlio_packets_pool;
	struct spdk_sock_impl_opts	impl_opts;
};

/* xlio packets pool for each core */
struct xlio_packets_pool {
	STAILQ_HEAD(, xlio_sock_packet)	free_packets;
	struct xlio_sock_packet	*packets;
	uint32_t		num_free_packets;
	uint32_t		core_id;
	STAILQ_ENTRY(xlio_packets_pool)	link;
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
typedef TAILQ_HEAD(, nvme_tcp_req)	nvme_tcp_req_tailq_t;

struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;

	uint8_t					sock_padding[40];
	struct spdk_xlio_sock			sock;

	uint8_t					padding[40];
	/* TODO: Do we need both outstanding_reqs and send_queue queues? */
	nvme_tcp_req_tailq_t			outstanding_reqs;
	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;
	TAILQ_ENTRY(nvme_tcp_qpair)		link;
	struct nvme_tcp_pdu			*recv_pdu;

	struct spdk_bit_pool			*cid_pool;
	struct nvme_tcp_req			**tcp_reqs_lookup;
	struct spdk_rdma_utils_mem_map		*mem_map;
	struct spdk_rdma_utils_memory_domain	*memory_domain;
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

	struct {
		uint16_t host_hdgst_enable: 1;
		uint16_t host_ddgst_enable: 1;
		uint16_t icreq_send_ack: 1;
		uint16_t in_connect_poll: 1;
		uint16_t use_poll_group_req_pool: 1;
		uint16_t needs_poll: 1;
		uint16_t needs_resubmit: 1;
		uint16_t shared_stats: 1;
		uint16_t has_accel_nomem_pdus : 1;
		uint16_t reserved : 7;
	} flags;

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

SPDK_STATIC_ASSERT(offsetof(struct nvme_tcp_qpair, sock) % 64 == 0,
		   "sock is not cache line aligned");
SPDK_STATIC_ASSERT(offsetof(struct nvme_tcp_qpair, outstanding_reqs) % 64 == 0,
		   "nvme_tcp_qpair own data is not cache line aligned");

enum nvme_tcp_req_state {
	NVME_TCP_REQ_FREE,
	NVME_TCP_REQ_ACTIVE,
	NVME_TCP_REQ_ACTIVE_R2T,
};

struct nvme_tcp_req {
	struct nvme_request			req;
	struct spdk_iobuf_entry			iobuf_entry;
	struct iovec				iobuf_iov;
	TAILQ_ENTRY(nvme_tcp_req)		link;
	struct spdk_sock_buf			*sock_buf;
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
			uint16_t			state : 3;
			uint16_t			c2h_accel_copy : 1;
		} bits;
	} ordering;
	uint8_t					active_r2ts;
	/* XXX 7 bytes hole, try to pack */
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C ack */
	struct nvme_tcp_pdu			pdu;
	struct iovec				iovs[NVME_TCP_MAX_SGL_DESCRIPTORS];
};

SPDK_STATIC_ASSERT(NVME_TCP_MAX_R2T_DEFAULT <= UINT8_MAX,
		   "type of active_r2ts needs to be extended");

static pthread_mutex_t g_xlio_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static STAILQ_HEAD(, xlio_packets_pool) g_xlio_packets_pools = STAILQ_HEAD_INITIALIZER(
			g_xlio_packets_pools);
static struct spdk_mempool *g_xlio_buffers_pool;

static int xlio_sock_poll_fd(int fd, uint32_t max_events_per_poll);
static inline int xlio_sock_flush_ext(struct spdk_xlio_sock *vsock);
static void xlio_sock_close(struct spdk_xlio_sock *sock);
static void _pdu_write_done(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu, int err);
static inline int nvme_tcp_fill_data_mkeys(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_req *tcp_req, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_qpair_sock_cb(struct nvme_tcp_qpair *tqpair);

static void
xlio_sock_free_pools(void)
{
	struct xlio_packets_pool *pool, *tmp;
	STAILQ_FOREACH_SAFE(pool, &g_xlio_packets_pools, link, tmp) {
		STAILQ_REMOVE_HEAD(&g_xlio_packets_pools, link);
		free(pool->packets);
		free(pool);
	}

	spdk_mempool_free(g_xlio_buffers_pool);
}

static int
get_addr_str(struct sockaddr *sa, char *host, size_t hlen)
{
	const char *result = NULL;

	if (sa == NULL || host == NULL) {
		return -1;
	}

	switch (sa->sa_family) {
	case AF_INET:
		result = inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				   host, hlen);
		break;
	case AF_INET6:
		result = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				   host, hlen);
		break;
	default:
		break;
	}

	if (result != NULL) {
		return 0;
	} else {
		return -1;
	}
}

static int
xlio_sock_set_recvbuf(struct spdk_xlio_sock *sock, int sz)
{
	struct spdk_sock_impl_opts impl_opts;
	size_t opts_len = sizeof(impl_opts);
	int min_size;
	int rc;

	assert(sock != NULL);

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

	rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static inline struct ibv_pd *
xlio_get_pd(int fd)
{
	struct xlio_pd_attr pd_attr_ptr = {};
	socklen_t len = sizeof(pd_attr_ptr);

	int err = xlio_getsockopt(fd, SOL_SOCKET, SO_XLIO_PD, &pd_attr_ptr, &len);
	if (err < 0) {
		return NULL;
	}
	return pd_attr_ptr.ib_pd;
}

static inline void
xlio_sock_req_reset(struct nvme_tcp_pdu *pdu)
{
	pdu->rw_offset = 0;
	pdu->is_zcopy = false;
}

static inline void
xlio_sock_request_complete(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *pdu, int err)
{
	struct nvme_tcp_qpair *tqpair;
	bool in_complete_cb;

	tqpair = SPDK_CONTAINEROF(sock, struct nvme_tcp_qpair, sock);

	xlio_sock_req_reset(pdu);

	in_complete_cb = sock->flags.in_complete_cb;
	sock->flags.in_complete_cb = 1;
	_pdu_write_done(tqpair, pdu, err);
	assert(sock->flags.in_complete_cb == 1);
	sock->flags.in_complete_cb = in_complete_cb;
}


static inline void
xlio_sock_request_put(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *pdu, int err)
{
	assert(pdu->curr_list == &sock->pending_pdus);
	TAILQ_REMOVE(&sock->pending_pdus, pdu, link);
#ifdef DEBUG
	pdu->curr_list = NULL;
#endif
	xlio_sock_request_complete(sock, pdu, err);
}

static inline void
xlio_sock_abort_requests(struct spdk_xlio_sock *sock)
{
	struct nvme_tcp_pdu *pdu;
	struct nvme_tcp_qpair *tqpair;
	bool in_complete_cb;

	in_complete_cb = sock->flags.in_complete_cb;
	sock->flags.in_complete_cb = 1;
	tqpair = SPDK_CONTAINEROF(sock, struct nvme_tcp_qpair, sock);

	pdu = TAILQ_FIRST(&sock->pending_pdus);
	while (pdu) {
		assert(pdu->curr_list == &sock->pending_pdus);
		TAILQ_REMOVE(&sock->pending_pdus, pdu, link);
#ifdef DEBUG
		pdu->curr_list = NULL;
#endif

		xlio_sock_req_reset(pdu);
		_pdu_write_done(tqpair, pdu, -ECANCELED);
		pdu = TAILQ_FIRST(&sock->pending_pdus);
	}

	pdu = TAILQ_FIRST(&sock->queued_pdus);
	while (pdu) {
		assert(pdu->curr_list == &sock->queued_pdus);
		TAILQ_REMOVE(&sock->queued_pdus, pdu, link);
#ifdef DEBUG
		pdu->curr_list = NULL;
#endif

		assert(sock->queued_iovcnt >= pdu->iovcnt);
		sock->queued_iovcnt -= pdu->iovcnt;

		xlio_sock_req_reset(pdu);
		_pdu_write_done(tqpair, pdu, -ECANCELED);
		pdu = TAILQ_FIRST(&sock->queued_pdus);
	}

	assert(sock->flags.in_complete_cb == 1);
	sock->flags.in_complete_cb = in_complete_cb;

	assert(TAILQ_EMPTY(&sock->queued_pdus));
	assert(TAILQ_EMPTY(&sock->pending_pdus));
}

static inline void
xlio_sock_request_pend(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *pdu)
{
	assert(pdu->curr_list == &sock->queued_pdus);
	TAILQ_REMOVE(&sock->queued_pdus, pdu, link);
	assert(sock->queued_iovcnt >= pdu->iovcnt);
	sock->queued_iovcnt -= pdu->iovcnt;
	TAILQ_INSERT_TAIL(&sock->pending_pdus, pdu, link);
#ifdef DEBUG
	pdu->curr_list = &sock->pending_pdus;
#endif
}


static inline void
xlio_sock_request_queue(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *pdu)
{
	assert(pdu->curr_list == NULL);
	TAILQ_INSERT_TAIL(&sock->queued_pdus, pdu, link);
#ifdef DEBUG
	pdu->curr_list = &sock->queued_pdus;
#endif
	SPDK_DEBUGLOG(nvme_xlio, "sock %p, pdu %p, iovs %u\n", sock, pdu, pdu->iovcnt);
	sock->queued_iovcnt += pdu->iovcnt;
}

static int
xlio_sock_alloc_buffers_pool(uint32_t buffers_pool_size)
{
	pthread_mutex_lock(&g_xlio_pool_mutex);
	if (g_xlio_buffers_pool) {
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		return 0;
	}

	g_xlio_buffers_pool = spdk_mempool_create("xlio_buffers_pool",
			      buffers_pool_size,
			      sizeof(struct xlio_sock_buf),
			      SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			      SPDK_ENV_SOCKET_ID_ANY);
	if (!g_xlio_buffers_pool) {
		SPDK_ERRLOG("Failed to create xlio buffers pool\n");
		pthread_mutex_unlock(&g_xlio_pool_mutex);
		return -ENOMEM;
	}

	pthread_mutex_unlock(&g_xlio_pool_mutex);
	SPDK_NOTICELOG("Create xlio buffers pool, buffers_pool_size %u\n", buffers_pool_size);

	return 0;
}

static struct xlio_packets_pool *
xlio_sock_get_packets_pool(uint32_t packets_pool_size)
{
	struct xlio_packets_pool *pool;
	uint32_t i, current_core = spdk_env_get_current_core();

	pthread_mutex_lock(&g_xlio_pool_mutex);
	STAILQ_FOREACH(pool, &g_xlio_packets_pools, link) {
		if (pool->core_id == current_core) {
			pthread_mutex_unlock(&g_xlio_pool_mutex);
			return pool;
		}
	}

	pool = calloc(1, sizeof(*pool));
	if (!pool) {
		SPDK_ERRLOG("Failed to allocate pool\n");
		goto fail;
	}

	pool->packets = calloc(packets_pool_size,
			       sizeof(struct xlio_sock_packet));
	if (!pool->packets) {
		SPDK_ERRLOG("Failed to allocate packets\n");
		free(pool);
		goto fail;
	}

	STAILQ_INIT(&pool->free_packets);
	for (i = 0; i < packets_pool_size; ++i) {
		STAILQ_INSERT_TAIL(&pool->free_packets, &pool->packets[i], link);
	}

	STAILQ_INSERT_HEAD(&g_xlio_packets_pools, pool, link);
	pool->num_free_packets = packets_pool_size;
	pool->core_id = current_core;
	pthread_mutex_unlock(&g_xlio_pool_mutex);
	SPDK_NOTICELOG("Create xlio pool, packets_pool_size %u on core %u\n",
		       packets_pool_size, current_core);

	return pool;

fail:
	pthread_mutex_unlock(&g_xlio_pool_mutex);
	return NULL;
}

static int
xlio_sock_alloc(struct spdk_xlio_sock *sock, int fd, bool enable_zero_copy,
		struct spdk_sock_impl_opts *xlio_opts)
{
	int flag = 1;
	int rc;

	sock->fd = fd;

	if (enable_zero_copy) {
		/* Try to turn on zero copy sends */
		rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
		if (rc == 0) {
			sock->flags.zcopy = true;
		} else {
			SPDK_WARNLOG("Zcopy send is not supported\n");
		}
	}

	sock->pd = xlio_get_pd(fd);
	if (!sock->pd) {
		SPDK_ERRLOG("Failed to get pd\n");
		return -ENODEV;
	}

	sock->xlio_packets_pool = xlio_sock_get_packets_pool(xlio_opts->packets_pool_size);
	if (!sock->xlio_packets_pool) {
		SPDK_ERRLOG("Failed to allocated packets pool for socket %d\n", fd);
		return -ENOMEM;
	}

	if (xlio_opts->enable_zerocopy_recv) {
		uint64_t user_data = (uintptr_t)sock;

		sock->flags.recv_zcopy = true;
		STAILQ_INIT(&sock->received_packets);

		if (xlio_sock_alloc_buffers_pool(xlio_opts->buffers_pool_size)) {
			return -ENOMEM;
		}

		rc = xlio_setsockopt(sock->fd, SOL_SOCKET, SO_XLIO_USER_DATA,
				     &user_data, sizeof(user_data));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to set socket user data for sock %p: rc %d, errno %d\n",
				    sock, rc, errno);
			return rc;
		}
	}

#if defined(__linux__)
	flag = 1;

	if (xlio_opts->enable_quickack) {
		rc = xlio_setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}
#endif

	TAILQ_INIT(&sock->queued_pdus);
	TAILQ_INIT(&sock->pending_pdus);

	return 0;
}

static bool
sock_is_loopback(int fd)
{
	struct ifaddrs *addrs, *tmp;
	struct sockaddr_storage sa = {};
	socklen_t salen;
	struct ifreq ifr = {};
	char ip_addr[256], ip_addr_tmp[256];
	int rc;
	bool is_loopback = false;

	salen = sizeof(sa);
	rc = xlio_getsockname(fd, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		return is_loopback;
	}

	memset(ip_addr, 0, sizeof(ip_addr));
	rc = get_addr_str((struct sockaddr *)&sa, ip_addr, sizeof(ip_addr));
	if (rc != 0) {
		return is_loopback;
	}

	getifaddrs(&addrs);
	for (tmp = addrs; tmp != NULL; tmp = tmp->ifa_next) {
		if (tmp->ifa_addr && (tmp->ifa_flags & IFF_UP) &&
		    (tmp->ifa_addr->sa_family == sa.ss_family)) {
			memset(ip_addr_tmp, 0, sizeof(ip_addr_tmp));
			rc = get_addr_str(tmp->ifa_addr, ip_addr_tmp, sizeof(ip_addr_tmp));
			if (rc != 0) {
				continue;
			}

			if (strncmp(ip_addr, ip_addr_tmp, sizeof(ip_addr)) == 0) {
				memcpy(ifr.ifr_name, tmp->ifa_name, sizeof(ifr.ifr_name));
				xlio_ioctl(fd, SIOCGIFFLAGS, &ifr);
				if (ifr.ifr_flags & IFF_LOOPBACK) {
					is_loopback = true;
				}
				goto end;
			}
		}
	}

end:
	freeifaddrs(addrs);
	return is_loopback;
}

static int
xlio_sock_set_nonblock(int fd)
{
	int flag;

	flag = xlio_fcntl(fd, F_GETFL);
	if (xlio_fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n",
			    fd, errno);
		return -1;
	}

	return 0;
}

static int
xlio_sock_init(struct spdk_xlio_sock *sock, const char *ip, int port, struct spdk_sock_opts *opts,
	       bool isolate, int vlan_tag)
{
	struct spdk_sock_impl_opts *xlio_opts;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd;
	int val = 1;
	int rc, sz;
	bool enable_zcopy_user_opts = true;
	bool enable_zcopy_impl_opts = true;

	if (!opts && !opts->impl_opts) {
		return -EINVAL;
	}
	xlio_opts = opts->impl_opts;

	if (ip == NULL) {
		return -EINVAL;
	}
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		ip = (const char *) &buf[0];
	}

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
	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
		fd = xlio_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0) {
			/* error */
			continue;
		}

		if (isolate) {
			sz = SO_XLIO_ISOLATE_SAFE;
			rc = xlio_setsockopt(fd, SOL_SOCKET, SO_XLIO_ISOLATE, &sz, sizeof(sz));
			if (rc) {
				SPDK_WARNLOG("Fail to set isolation: sock %p rc %d errno %d\n", sock, rc, errno);
			}
		}

		sz = xlio_opts->recv_buf_size;
		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		sz = xlio_opts->send_buf_size;
		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			xlio_close(fd);
			/* error */
			continue;
		}

		if (xlio_opts->enable_tcp_nodelay) {
			rc = xlio_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}

#if defined(SO_PRIORITY)
		if (opts->priority) {
			rc = xlio_setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}
#endif

		if (res->ai_family == AF_INET6) {
			rc = xlio_setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}

		if (opts->ack_timeout) {
#if defined(__linux__)
			int to;

			to = opts->ack_timeout;
			rc = xlio_setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to));
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
#else
			SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
		}

		uint64_t user_data = (uintptr_t)NULL;
		rc = xlio_setsockopt(fd, SOL_SOCKET, SO_XLIO_USER_DATA,
				     &user_data, sizeof(user_data));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to set socket user data for sock %d: rc %d, errno %d\n",
				    fd, rc, errno);
			xlio_close(fd);
			fd = -1;
			break;
		}

		if (vlan_tag != 0) {
			rc = xlio_setsockopt(fd, SOL_SOCKET, SO_XLIO_EXT_VLAN_TAG, &vlan_tag,
					     sizeof vlan_tag);
			if (rc != 0) {
				xlio_close(fd);
				/* error */
				continue;
			}
		}

		if (xlio_sock_set_nonblock(fd)) {
			xlio_close(fd);
			fd = -1;
			break;
		}

		rc = xlio_connect(fd, res->ai_addr, res->ai_addrlen);
		if (rc != 0) {
			if (rc != EAGAIN && rc != EWOULDBLOCK && errno != EINPROGRESS) {
				SPDK_ERRLOG("connect() failed, rc %d, errno = %d\n", rc, errno);
				/* try next family */
				xlio_close(fd);
				fd = -1;
				continue;
			}
		}

		enable_zcopy_impl_opts = xlio_opts->enable_zerocopy_send_client;

		break;
	}
	xlio_freeaddrinfo(res0);

	if (fd < 0) {
		return -EINVAL;
	}

	/* Only enable zero copy for non-loopback sockets. */
	enable_zcopy_user_opts = opts->zcopy && !sock_is_loopback(fd);

	rc = xlio_sock_alloc(sock, fd, enable_zcopy_user_opts && enable_zcopy_impl_opts, xlio_opts);
	if (rc) {
		SPDK_ERRLOG("sock allocation failed\n");
		xlio_close(fd);
		return rc;
	}

	SPDK_NOTICELOG("sock %p %d: send zcopy %d, recv zcopy %d, pd %p, context %p, dev %s, handle %u isolate %d\n",
		       sock, fd, sock->flags.zcopy, sock->flags.recv_zcopy, sock->pd,
		       sock->pd ? sock->pd->context : NULL,
		       sock->pd ? sock->pd->context->device->name : "unknown",
		       sock->pd ? sock->pd->handle : 0, isolate);

	return 0;
}


static void xlio_sock_free_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet);

static void
xlio_sock_release_packets(struct spdk_xlio_sock *sock)
{
	while (!STAILQ_EMPTY(&sock->received_packets)) {
		struct xlio_sock_packet *packet = STAILQ_FIRST(&sock->received_packets);

		STAILQ_REMOVE_HEAD(&sock->received_packets, link);
		if (--packet->refs == 0) {
			xlio_sock_free_packet(sock, packet);
		} else {
			SPDK_ERRLOG("Socket close: received packet with non zero refs %u, fd %d\n",
				    packet->refs, sock->fd);
		}
	}
}

static void
xlio_sock_close(struct spdk_xlio_sock *sock)
{
	assert(TAILQ_EMPTY(&sock->pending_pdus));
	assert(sock->consumed_packets == 0);
	assert(!sock->flags.pending_recv);
	assert(!sock->flags.pending_send);

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	xlio_close(sock->fd);

	if (sock->ring_fd && --sock->ring_fd->refs == 0) {
		free(sock->ring_fd);
	}

	sock->ring_fd = NULL;
	sock->fd = 0;
	sock->flags_raw = 0;
	sock->flags.closed = 1;
}

static int
_sock_check_zcopy(struct spdk_xlio_sock *vsock)
{
	struct spdk_xlio_sock_group_impl *group = vsock->group_impl;
	struct msghdr msgh = {};
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct nvme_tcp_pdu *pdu, *treq;
	bool found;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	while (true) {
		rc = xlio_recvmsg(vsock->fd, &msgh, MSG_ERRQUEUE);

		if (rc < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;
			}

			if (!TAILQ_EMPTY(&vsock->pending_pdus)) {
				SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries\n");
			} else {
				SPDK_WARNLOG("Recvmsg yielded an error!\n");
			}
			return 0;
		}

		cm = CMSG_FIRSTHDR(&msgh);
		if (!cm || cm->cmsg_level != SOL_IP || cm->cmsg_type != IP_RECVERR) {
			SPDK_WARNLOG("Unexpected cmsg level or type!\n");
			return 0;
		}

		serr = (struct sock_extended_err *)CMSG_DATA(cm);
		if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
			SPDK_WARNLOG("Unexpected extended error origin\n");
			return 0;
		}

		/* Most of the time, the pending_reqs array is in the exact
		 * order we need such that all of the requests to complete are
		 * in order, in the front. It is guaranteed that all requests
		 * belonging to the same sendmsg call are sequential, so once
		 * we encounter one match we can stop looping as soon as a
		 * non-match is found.
		 */
		for (idx = serr->ee_info; idx <= serr->ee_data; idx++) {
			found = false;

			TAILQ_FOREACH_SAFE(pdu, &vsock->pending_pdus, link, treq) {
				if (!pdu->is_zcopy) {
					/* This wasn't a zcopy request. It was just waiting in line to complete */
					xlio_sock_request_put(vsock, pdu, 0);
				} else if (pdu->rw_offset == idx) {
					found = true;

					xlio_sock_request_put(vsock, pdu, 0);
				} else if (found) {
					break;
				}
			}

			/* If we reaped buffer reclaim notification and sock is not in pending_recv list yet,
			 * add it now. It allows to call socket callback and process completions */
			if (found && !vsock->flags.pending_recv && group) {
				assert(vsock->ring_fd);
				vsock->flags.pending_recv = true;
				TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
			}
		}
	}

	return 0;
}

static int
xlio_sock_flush(struct spdk_xlio_sock *sock)
{
	if (sock->flags.zcopy && !TAILQ_EMPTY(&sock->pending_pdus)) {
		_sock_check_zcopy(sock);
	}

	return xlio_sock_flush_ext(sock);
}

static inline struct xlio_recvfrom_zcopy_packet_t *
next_packet(struct xlio_recvfrom_zcopy_packet_t *packet)
{
	return (struct xlio_recvfrom_zcopy_packet_t *)((char *)packet +
			sizeof(struct xlio_recvfrom_zcopy_packet_t) +
			packet->sz_iov * sizeof(struct iovec));
}

#ifdef DEBUG
static void
dump_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet)
{
	size_t i;
	struct xlio_buff_t *xlio_buf;

	SPDK_DEBUGLOG(nvme_xlio, "sock %p packet %p: num_bufs %lu, total_len %u, first buf %p\n",
		      sock,
		      packet, packet->xlio_packet.num_bufs,
		      packet->xlio_packet.total_len,
		      packet->xlio_packet.buff_lst);

	for (i = 0, xlio_buf = packet->xlio_packet.buff_lst;
	     xlio_buf;
	     ++i, xlio_buf = xlio_buf->next) {
		SPDK_DEBUGLOG(nvme_xlio, "Packet %p[%lu]: payload %p, len %u\n",
			      packet, i, xlio_buf->payload, xlio_buf->len);
	}
}
#endif

static struct xlio_sock_packet *
xlio_sock_get_packet(struct spdk_xlio_sock *sock)
{
	struct xlio_sock_packet *packet = STAILQ_FIRST(&sock->xlio_packets_pool->free_packets);

	assert(packet);
	STAILQ_REMOVE_HEAD(&sock->xlio_packets_pool->free_packets, link);
	assert(sock->xlio_packets_pool->num_free_packets > 0);
	sock->xlio_packets_pool->num_free_packets--;

	return packet;
}

static void
xlio_sock_free_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet)
{
	int ret;

	SPDK_DEBUGLOG(nvme_xlio, "sock %p: free xlio packet, first buf %p\n",
		      sock, packet->xlio_packet.buff_lst);
	assert(packet->refs == 0);
	/* @todo: How heavy is free_packets()? Maybe batch packets to free? */
	ret = xlio_socketxtreme_free_packets(&packet->xlio_packet, 1);
	if (ret < 0) {
		SPDK_ERRLOG("Free xlio packets failed, ret %d, errno %d\n",
			    ret, errno);
	}

	STAILQ_INSERT_HEAD(&sock->xlio_packets_pool->free_packets, packet, link);
	sock->xlio_packets_pool->num_free_packets++;
	assert(sock->consumed_packets > 0);
	sock->consumed_packets--;
}

static void
packets_advance(struct spdk_xlio_sock *sock, size_t len)
{
	SPDK_DEBUGLOG(nvme_xlio, "sock %p: advance packets by %lu bytes\n", sock, len);
	while (len > 0) {
		struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);
		/* We don't allow to advance by more than we have data in packets */
		assert(cur_packet != NULL);
		struct xlio_buff_t *cur_xlio_buf = sock->cur_xlio_buf;
		assert(cur_xlio_buf != NULL);
		/* both  xlio_buff_t::len and sock::cur_offset are uint16_t,
		 * here we can't get value bigger than UINT16_MAX */
		size_t remaining_buf_len = cur_xlio_buf->len - sock->cur_offset;
		assert(remaining_buf_len <= UINT16_MAX);

		if (len < remaining_buf_len) {
			assert(sock->cur_offset + len <= UINT16_MAX);
			sock->cur_offset += len;
			len = 0;
		} else {
			len -= remaining_buf_len;

			/* Next iov */
			sock->cur_offset = 0;
			sock->cur_xlio_buf = cur_xlio_buf->next;
			if (!sock->cur_xlio_buf) {
				/* Next packet */
				STAILQ_REMOVE_HEAD(&sock->received_packets, link);
				if (--cur_packet->refs == 0) {
					xlio_sock_free_packet(sock, cur_packet);
				}

				cur_packet = STAILQ_FIRST(&sock->received_packets);
				sock->cur_xlio_buf = cur_packet ? cur_packet->xlio_packet.buff_lst : NULL;
			}
		}
	}

	assert(len == 0);
}

static size_t
packets_next_chunk(struct spdk_xlio_sock *sock,
		   void **buf,
		   struct xlio_sock_packet **packet,
		   size_t max_len)
{
	struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);

	if (!sock->cur_xlio_buf && cur_packet) {
		sock->cur_xlio_buf = cur_packet->xlio_packet.buff_lst;
	}

	while (cur_packet) {
		struct xlio_buff_t *cur_xlio_buf = sock->cur_xlio_buf;
		assert(cur_xlio_buf);
		size_t len = cur_xlio_buf->len - sock->cur_offset;

		if (len == 0) {
			/* xlio may return zero length iov. Skip to next in this case */
			SPDK_DEBUGLOG(nvme_xlio, "Zero length buffer: len %d, offset %u\n",
				      cur_xlio_buf->len, sock->cur_offset);
			sock->cur_offset = 0;
			sock->cur_xlio_buf = cur_xlio_buf->next;
			if (!sock->cur_xlio_buf) {
				/* Next packet */
				cur_packet = STAILQ_NEXT(cur_packet, link);
				sock->cur_xlio_buf = cur_packet ? cur_packet->xlio_packet.buff_lst : NULL;
			}
			continue;
		}

		assert(max_len > 0);
		assert(len > 0);
		len = spdk_min(len, max_len);
		*buf = (uint8_t *)cur_xlio_buf->payload + sock->cur_offset;
		*packet = cur_packet;
		return len;
	}

	return 0;
}

static int
poll_no_group_socket(struct spdk_xlio_sock *sock)
{
	int ret;
	uint32_t max_events_per_poll;

	/* For sockets not bound to group we have to poll here.
	 * Polling may find events for other sockets but not for this one.
	 * So, we need to check if new packets were added for this socket.
	 */
	if (!sock->ring_fd) {
		int ring_fds[2];

		ret = xlio_get_socket_rings_fds(sock->fd, ring_fds, 2);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to get ring FDs for socket %d: rc %d, errno %d\n",
				    sock->fd, ret, errno);
			return ret;
		}

		/* @todo: add support for multiple rings */
		assert(ret == 1);
		sock->ring_fd = calloc(1, sizeof(struct spdk_xlio_ring_fd));
		if (!sock->ring_fd) {
			SPDK_ERRLOG("Failed to allocate ring_fd\n");
			return -1;
		}
		sock->ring_fd->ring_fd = ring_fds[0];
		sock->ring_fd->refs = 1;
		SPDK_DEBUGLOG(nvme, "Discovered ring fd %d for socket %p %d, num_rings %d\n",
			      sock->ring_fd->ring_fd, sock, sock->fd, ret);
	}

	if (sock->xlio_packets_pool->num_free_packets) {
		max_events_per_poll = spdk_min(sock->xlio_packets_pool->num_free_packets, MAX_EVENTS_PER_POLL);

		ret = xlio_sock_poll_fd(sock->ring_fd->ring_fd, max_events_per_poll);
		if (ret < 0) {
			return -1;
		}
	} else {
		SPDK_DEBUGLOG(nvme_xlio, "no free packets\n");
	}

	if (STAILQ_EMPTY(&sock->received_packets)) {
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

static ssize_t
xlio_sock_readv(struct spdk_xlio_sock *sock, struct iovec *iovs, int iovcnt)
{
	int ret;

	if (sock->flags.recv_zcopy) {
		int i;
		size_t offset = 0;

		if (STAILQ_EMPTY(&sock->received_packets)) {
			if (spdk_unlikely(!sock->group_impl)) {
				ret = poll_no_group_socket(sock);
				if (ret < 0) {
					if (sock->flags.disconnected) {
						return 0;
					}
					return ret;
				}
			} else {
				/* @todo: should we try to poll here? */
				if (sock->flags.disconnected) {
					return 0;
				}
				errno = EAGAIN;
				return -1;
			}
		}

		assert(!STAILQ_EMPTY(&sock->received_packets));
		ret = 0;
		i = 0;
		while (i < iovcnt) {
			void *buf;
			size_t len;
			struct iovec *iov = &iovs[i];
			size_t iov_len = iov->iov_len - offset;
			struct xlio_sock_packet *packet;

			len = packets_next_chunk(sock, &buf, &packet, iov_len);
			if (len == 0) {
				/* No more data */
				SPDK_DEBUGLOG(nvme_xlio, "sock %p: readv_wrapper ret %d\n", sock, ret);
				return ret;
			}

			memcpy((uint8_t *)iov->iov_base + offset, buf, len);
			packets_advance(sock, len);
			ret += len;
			offset += len;
			assert(offset <= iov->iov_len);
			if (offset == iov->iov_len) {
				offset = 0;
				i++;
			}
		}

		SPDK_DEBUGLOG(nvme_xlio, "sock %p: readv_wrapper ret %d\n", sock, ret);
	} else {
		ret = xlio_readv(sock->fd, iovs, iovcnt);
		SPDK_DEBUGLOG(nvme_xlio, "Sock %d: readv_wrapper ret %d, errno %d\n", sock->fd, ret, errno);
	}

	return ret;

}

union _mkeys_container {
	char buf[CMSG_SPACE(sizeof(struct xlio_pd_key) * IOV_BATCH_SIZE)];
	struct cmsghdr align;
};

static inline size_t
xlio_sock_prep_reqs(struct spdk_xlio_sock *vsock, struct iovec *iovs, struct msghdr *msg,
		    union _mkeys_container *mkeys_container, uint32_t *_total)
{
	size_t iovcnt = 0;
	int i;
	struct nvme_tcp_pdu *pdu;
	struct nvme_tcp_qpair *tqpair;
	unsigned int offset;
	struct cmsghdr *cmsg;
	struct xlio_pd_key *mkeys = NULL;
	uint32_t total = 0;
	uint32_t pdu_data_len_remaining;
	bool first_req_mkey;

	pdu = TAILQ_FIRST(&vsock->queued_pdus);
	assert(pdu);
	first_req_mkey = pdu->has_mkeys;

	msg->msg_control = mkeys_container->buf;
	msg->msg_controllen = sizeof(mkeys_container->buf);
	cmsg = CMSG_FIRSTHDR(msg);

	cmsg->cmsg_len = CMSG_LEN(sizeof(struct xlio_pd_key) * IOV_BATCH_SIZE);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_XLIO_PD;

	mkeys = (struct xlio_pd_key *)CMSG_DATA(cmsg);
	tqpair = SPDK_CONTAINEROF(vsock, struct nvme_tcp_qpair, sock);

	while (pdu && iovcnt < IOV_BATCH_SIZE) {
		offset = pdu->rw_offset;

		if (spdk_unlikely(first_req_mkey != pdu->has_mkeys)) {
			/* mkey setting or zcopy threshold is different with the first req */
			break;
		}

		if (pdu->has_capsule) {
			iovs[iovcnt].iov_base = pdu->hdr.raw + pdu->capsule_offset;
			iovs[iovcnt].iov_len = pdu->capsule_len - pdu->capsule_offset;
			if (spdk_likely(first_req_mkey)) {
				mkeys[iovcnt].mkey = tqpair->pdus_mkey;
				mkeys[iovcnt].flags = 0;
			}
			total += (uint32_t)iovs[iovcnt].iov_len;
			SPDK_DEBUGLOG(nvme_xlio, "\tpdu %p, capsule len %zu\n", pdu, iovs[iovcnt].iov_len);
			iovcnt++;
		}

		assert(pdu->data_len >= offset);
		pdu_data_len_remaining = pdu->data_len - offset;
		for (i = 0; i < pdu->data_iovcnt && iovcnt < IOV_BATCH_SIZE && pdu_data_len_remaining; i++) {
			/* Consume any offset first */
			if (offset >= pdu->iovs[i].iov_len) {
				SPDK_DEBUGLOG(nvme_xlio, "\tpdu %p, skip iov[%u] len %zu\n", pdu, i, iovs[i].iov_len);
				offset -= pdu->iovs[i].iov_len;
				continue;
			}
			if (spdk_likely(first_req_mkey)) {
				mkeys[iovcnt].mkey = pdu->mkeys[i];
				mkeys[iovcnt].flags = 0;
			}
			iovs[iovcnt].iov_base = (uint8_t *)pdu->iovs[i].iov_base + offset;
			iovs[iovcnt].iov_len = pdu->iovs[i].iov_len - offset;
			iovs[iovcnt].iov_len = spdk_min(pdu_data_len_remaining, iovs[iovcnt].iov_len);
			pdu_data_len_remaining -= iovs[iovcnt].iov_len;
			offset = 0;
			total += (uint32_t)iovs[iovcnt].iov_len;
			SPDK_DEBUGLOG(nvme_xlio, "\tpdu %p, iovs[%zu].iov_len = %zu\n", pdu, iovcnt, iovs[iovcnt].iov_len);
			iovcnt++;
		}

		if (pdu->ddgst_enable && iovcnt < IOV_BATCH_SIZE) {
			iovs[iovcnt].iov_base = pdu->data_digest;
			iovs[iovcnt].iov_len = sizeof(pdu->data_digest);
			if (spdk_likely(first_req_mkey)) {
				mkeys[iovcnt].mkey = tqpair->pdus_mkey;
				mkeys[iovcnt].flags = 0;
			}
			SPDK_DEBUGLOG(nvme_xlio, "\tpdu %p, dgst len %zu\n", pdu, iovs[iovcnt].iov_len);
			total += (uint32_t)iovs[iovcnt].iov_len;
			iovcnt++;
		}

		pdu = TAILQ_NEXT(pdu, link);
	}

	if (first_req_mkey) {
		msg->msg_controllen = CMSG_SPACE(sizeof(struct xlio_pd_key) * iovcnt);
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct xlio_pd_key) * iovcnt);
	} else {
		msg->msg_control = NULL;
		msg->msg_controllen = 0;
	}

	*_total = total;

	return iovcnt;
}

static inline bool
xlio_sock_flush_now(struct spdk_xlio_sock *vsock, uint32_t qlen_bytes)
{
	if (vsock->group_impl && vsock->group_impl->impl_opts.flush_batch_timeout) {
		uint64_t now = spdk_get_ticks();
		struct spdk_sock_impl_opts *impl_opts = &vsock->group_impl->impl_opts;

		if (qlen_bytes >= impl_opts->flush_batch_bytes_threshold) {
			/* Flush now */
		} else if (vsock->batch_start_tsc &&
			   (now - vsock->batch_start_tsc) * SPDK_SEC_TO_USEC / spdk_get_ticks_hz() >
			   impl_opts->flush_batch_timeout) {
			/* batch timeout */
			if (vsock->queued_iovcnt < vsock->batch_nr) {
				vsock->batch_nr = spdk_max(vsock->batch_nr >> 1, 1);
			}
		} else if (vsock->queued_iovcnt >= vsock->batch_nr) {
			/* Try to flush socket before timeout, so could batch more */
			vsock->batch_nr = spdk_min(vsock->batch_nr + 1,
						   impl_opts->flush_batch_iovcnt_threshold);
		} else {
			/* Not flush socket, try to batch more requests */
			if (!vsock->batch_start_tsc) {
				vsock->batch_start_tsc = now;
			}
			return false;
		}
		vsock->batch_start_tsc = 0;
	}

	return true;
}

static int
xlio_sock_flush_ext(struct spdk_xlio_sock *vsock)
{
	struct iovec iovs[IOV_BATCH_SIZE];
	struct msghdr msg = {};
	union _mkeys_container mkeys_container;
	size_t iovcnt;
	struct nvme_tcp_pdu *pdu;
	ssize_t rc;
	size_t len;
	int flags = 0;
	int i;
	unsigned int offset;
	uint32_t total;
	uint32_t pdu_remaining;
	bool is_zcopy = false;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (vsock->flags.in_complete_cb != 0) {
		return 0;
	}

	if (spdk_unlikely(TAILQ_EMPTY(&vsock->queued_pdus))) {
		return 0;
	}

	iovcnt = xlio_sock_prep_reqs(vsock, iovs, &msg, &mkeys_container, &total);
	if (spdk_unlikely(iovcnt == 0)) {
		return 0;
	}

	assert(!(!vsock->flags.zcopy && msg.msg_controllen > 0));
	if (!xlio_sock_flush_now(vsock, total)) {
		return 0;
	}

	/* Allow zcopy if enabled on socket and the data needs to be sent,
	 * which is reported by xlio_sock_prep_reqs() with setting msg.msg_controllen */
	if (vsock->flags.zcopy && msg.msg_controllen) {
		flags = MSG_ZEROCOPY;
		is_zcopy = true;
	}
	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;

	rc = xlio_sendmsg(vsock->fd, &msg, flags);
	SPDK_DEBUGLOG(nvme_xlio, "sock %p, iovs %zu, len %u, zcopy %d, rc %zd\n", vsock, iovcnt, total,
		      is_zcopy, rc);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && vsock->flags.zcopy)) {
			return 0;
		}

		SPDK_ERRLOG("sendmsg error %zd\n", rc);
		return rc;
	}

	if (is_zcopy) {
		/* Handling overflow case, because we use vsock->sendmsg_idx - 1 for the
		 * req->internal.offset, so sendmsg_idx should not be zero  */
		if (spdk_unlikely(vsock->sendmsg_idx == UINT32_MAX)) {
			vsock->sendmsg_idx = 1;
		} else {
			vsock->sendmsg_idx++;
		}
	}

	/* Consume the requests that were actually written */
	pdu = TAILQ_FIRST(&vsock->queued_pdus);
	while (pdu) {
		offset = pdu->rw_offset;

		/* req->internal.is_zcopy is true when the whole req or part of it is
		 * sent with zerocopy */
		pdu->is_zcopy = is_zcopy;
		pdu_remaining = ((pdu->capsule_len - pdu->capsule_offset) * pdu->has_capsule) +
				(pdu->data_len - offset) + ((SPDK_NVME_TCP_DIGEST_LEN - pdu->ddigest_offset) * pdu->ddgst_enable);
		SPDK_DEBUGLOG(nvme_xlio, "\tsock %p, pdu %p, offset %u, remaining %u\n", vsock, pdu, offset,
			      pdu_remaining);
		if (rc >= pdu_remaining) {
			/* PDU was fully consumed, no need to check each part */
			pdu->has_capsule = 0;
			pdu->ddgst_enable = 0;
			pdu->rw_offset = pdu->data_len;
			rc -= pdu_remaining;
			goto full_req;
		}

		if (pdu->has_capsule) {
			if (spdk_likely(rc >= pdu->capsule_len - pdu->capsule_offset)) {
				/* capsule header was fully sent */
				rc -= pdu->capsule_len - pdu->capsule_offset;
				pdu->has_capsule = 0;
			} else {
				pdu->capsule_offset += rc;
				/* Only part of capsule was sent */
				return 0;
			}
		}

		for (i = 0; i < pdu->data_iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= pdu->iovs[i].iov_len) {
				offset -= pdu->iovs[i].iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = pdu->iovs[i].iov_len - offset;

			if (len > (size_t)rc) {
				/* This element was partially sent. */
				pdu->rw_offset += rc;
				return 0;
			}

			offset = 0;
			pdu->rw_offset += len;
			rc -= len;
		}

		if (pdu->ddgst_enable) {
			if (spdk_likely(rc >= SPDK_NVME_TCP_DIGEST_LEN - pdu->ddigest_offset)) {
				rc -= SPDK_NVME_TCP_DIGEST_LEN - pdu->ddigest_offset;
				pdu->ddgst_enable = 0;
			} else {
				pdu->ddigest_offset += rc;
				/* Only part of data digest was sent */
				return 0;
			}
		}

full_req:
		/* Handled a full request. */
		xlio_sock_request_pend(vsock, pdu);

		/* Ordering control. */
		if (!pdu->is_zcopy && pdu == TAILQ_FIRST(&vsock->pending_pdus)) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			xlio_sock_request_put(vsock, pdu, 0);
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			pdu->rw_offset = vsock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		pdu = TAILQ_FIRST(&vsock->queued_pdus);
	}

	return 0;
}


static void
xlio_sock_writev_async(struct spdk_xlio_sock *vsock, struct nvme_tcp_pdu *pdu)
{
	struct spdk_xlio_sock_group_impl *group = vsock->group_impl;
	int rc;

	xlio_sock_request_queue(vsock, pdu);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (vsock->queued_iovcnt >= IOV_BATCH_SIZE) {
		rc = xlio_sock_flush_ext(vsock);
		if (spdk_likely(rc == 0)) {
			if (TAILQ_EMPTY(&vsock->queued_pdus) && vsock->flags.pending_send && vsock->group_impl) {
				TAILQ_REMOVE(&group->pending_send, vsock, link_send);
				vsock->flags.pending_send = false;
			}
		} else {
			xlio_sock_abort_requests(vsock);
		}
	} else if (!vsock->flags.pending_send && vsock->group_impl) {
		TAILQ_INSERT_TAIL(&group->pending_send, vsock, link_send);
		vsock->flags.pending_send = true;
	}
}

static int
xlio_sock_group_impl_create(struct spdk_xlio_sock_group_impl *group_impl)
{
	size_t impl_opts_size = sizeof(group_impl->impl_opts);
	uint32_t num_packets;
	uint32_t num_buffers;
	int rc;

	assert(group_impl);

	rc = spdk_sock_impl_get_opts("xlio", &group_impl->impl_opts, &impl_opts_size);
	if (rc) {
		return rc;
	}
	num_packets = group_impl->impl_opts.packets_pool_size;
	num_buffers = group_impl->impl_opts.buffers_pool_size;

	TAILQ_INIT(&group_impl->ring_fd);
	TAILQ_INIT(&group_impl->pending_recv);
	TAILQ_INIT(&group_impl->pending_send);

	if (num_packets) {
		group_impl->xlio_packets_pool = xlio_sock_get_packets_pool(num_packets);
		if (!group_impl->xlio_packets_pool) {
			return -ENOMEM;
		}
	}

	if (num_buffers && xlio_sock_alloc_buffers_pool(num_buffers)) {
		SPDK_ERRLOG("Failed to allocated buffers pool for group %p\n", group_impl);
		return -ENOMEM;
	}

	return 0;
}

static int
xlio_sock_group_impl_add_sock(struct spdk_xlio_sock_group_impl *group, struct spdk_xlio_sock *vsock)
{
	struct spdk_xlio_ring_fd *ring_fd;
	int ring_fds[2];
	int rc;

	rc = xlio_get_socket_rings_fds(vsock->fd, ring_fds, 2);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to get ring FDs for socket %d\n", vsock->fd);
		return rc;
	}

	/* @todo: add support for multiple rings */
	assert(rc == 1);
	SPDK_DEBUGLOG(nvme_xlio, "sock %p ring %d\n", vsock, ring_fds[0]);
	vsock->group_impl = group;

	TAILQ_FOREACH(ring_fd, &group->ring_fd, link) {
		if (ring_fd->ring_fd == ring_fds[0]) {
			vsock->ring_fd = ring_fd;
			ring_fd->refs++;
			return 0;
		}
	}

	if (!vsock->ring_fd) {
		vsock->ring_fd = calloc(1, sizeof(struct spdk_xlio_ring_fd));
		if (!vsock->ring_fd) {
			SPDK_ERRLOG("Failed to allocate ring_fd\n");
			return -1;
		}
		vsock->ring_fd->ring_fd = ring_fds[0];
	}
	vsock->ring_fd->refs = 1;
	TAILQ_INSERT_TAIL(&group->ring_fd, vsock->ring_fd, link);

	return 0;
}

static int
xlio_sock_group_impl_remove_sock(struct spdk_xlio_sock_group_impl *group,
				 struct spdk_xlio_sock *vsock)
{

	xlio_sock_abort_requests(vsock);
	if (vsock->flags.pending_send) {
		TAILQ_REMOVE(&group->pending_send, vsock, link_send);
		vsock->flags.pending_send = false;
	}
	if (vsock->flags.pending_recv) {
		TAILQ_REMOVE(&group->pending_recv, vsock, link);
		vsock->flags.pending_recv = false;
	}
	if (--vsock->ring_fd->refs == 0) {
		TAILQ_REMOVE(&group->ring_fd, vsock->ring_fd, link);
		free(vsock->ring_fd);
	}
	vsock->ring_fd = NULL;

	return 0;
}

static void nvme_tcp_qpair_connect_sock_done(struct spdk_xlio_sock *vsock, int err);

static int
xlio_sock_poll_fd(int fd, uint32_t max_events_per_poll)
{
	struct spdk_xlio_sock *vsock;
	int num_events, i, rc;
	struct xlio_socketxtreme_completion_t comps[MAX_EVENTS_PER_POLL];

	num_events = xlio_socketxtreme_poll(fd, comps, max_events_per_poll, SOCKETXTREME_POLL_TX);
	if (num_events < 0) {
		SPDK_ERRLOG("Socket extreme poll failed for fd %d: fd, result %d, errno %d\n", fd, num_events,
			    errno);
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		struct xlio_socketxtreme_completion_t *comp = &comps[i];
		struct xlio_sock_packet *packet;

		vsock = (struct spdk_xlio_sock *)comp->user_data;
		if (!vsock) {
			continue;
		}

		SPDK_DEBUGLOG(nvme_xlio, "XLIO completion[%d]: ring fd %d, events %" PRIx64
			      ", user_data %p, listen_fd %d\n",
			      i, fd, comp->events, (void *)comp->user_data, comp->listen_fd);

		if (comp->events & EPOLLHUP) {
			SPDK_ERRLOG("Got EPOLLHUP event on socket %d, events %" PRIx64 "\n",
				    vsock->fd, comp->events);
			vsock->flags.disconnected = true;
			if (!vsock->flags.connect_notified) {
				nvme_tcp_qpair_connect_sock_done(vsock, ETIMEDOUT);
				vsock->flags.connect_notified = 1;
			}
		}

		if (comp->events & EPOLLOUT && !vsock->flags.connect_notified) {
			nvme_tcp_qpair_connect_sock_done(vsock, 0);
			vsock->flags.connect_notified = 1;
		}

		if (comp->events & EPOLLERR) {
			rc = _sock_check_zcopy(vsock);
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || vsock->flags.closed) {
				continue;
			}
		}
		if (comp->events & XLIO_SOCKETXTREME_PACKET) {
			packet = xlio_sock_get_packet(vsock);
			packet->xlio_packet = comp->packet;
			/*
			 * While the packet is in received list there is data
			 * to read from it.  To avoid free of packets with
			 * unread data we intialize reference counter to 1.
			 */
			packet->refs = 1;
			STAILQ_INSERT_TAIL(&vsock->received_packets, packet, link);
			vsock->consumed_packets++;
#ifdef DEBUG
			dump_packet(vsock, packet);
#endif
		}

		/* If the socket does not already have recv pending, add it now */
		if ((comp->events & (XLIO_SOCKETXTREME_PACKET | EPOLLHUP)) &&
		    spdk_likely(vsock->group_impl) && !vsock->flags.pending_recv) {
			struct spdk_xlio_sock_group_impl *group = vsock->group_impl;

			assert(vsock->ring_fd);
			vsock->flags.pending_recv = true;
			TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
		}
	}

	return num_events;
}

static int
xlio_sock_group_impl_poll(struct spdk_xlio_sock_group_impl *group, int max_events)
{
	int num_events, rc;
	struct spdk_xlio_sock *vsock, *ptmp;
	uint32_t max_events_per_poll;
	struct spdk_xlio_ring_fd *ring_fd;

	/*
	 * Important to use TAILQ_FOREACH_SAFE() here bacause of the following:
	 * - spdk_sock_abort_requests() can lead to removing from ->pending_send
	 *   in xlio_sock_group_impl_remove_sock()
	 * - we remove from ->pending_send if no more ->queued_reqs left.
	 */
	TAILQ_FOREACH_SAFE(vsock, &group->pending_send, link_send, ptmp) {
		assert(vsock->flags.pending_send);

		rc = xlio_sock_flush_ext(vsock);
		if (spdk_likely(rc == 0)) {
			/*
			 * Removing from pendings only in no-error case because
			 * spdk_sock_abort_requests() can cause removing from group,
			 * removing from pendign and kill vsock itself underneath.
			 * In any case we will crash in that case.
			 */
			if (TAILQ_EMPTY(&vsock->queued_pdus)) {
				TAILQ_REMOVE(&group->pending_send, vsock, link_send);
				vsock->flags.pending_send = false;
			}
		} else {
			/*
			 * Aborting requests leads to removing from group
			 * and socket close. Removing from group also
			 * removes @vsock from all group pending lists
			 * in xlio_sock_group_impl_remove_sock().
			 */
			xlio_sock_abort_requests(vsock);
		}
	}

	TAILQ_FOREACH(ring_fd, &group->ring_fd, link) {
		if (group->xlio_packets_pool->num_free_packets) {
			max_events_per_poll = spdk_min(group->xlio_packets_pool->num_free_packets, MAX_EVENTS_PER_POLL);
			num_events = xlio_sock_poll_fd(ring_fd->ring_fd, max_events_per_poll);
			if (num_events < 0) {
				/* @todo: what if we have a problem with just one ring and another one is good? */
				return -1;
			}
		} else {
			SPDK_DEBUGLOG(nvme_xlio, "no free packets\n");
			break;
		}
	}

	num_events = 0;
	ptmp = TAILQ_LAST(&group->pending_recv, pending_recv_head);
	vsock = NULL;
	while (vsock != ptmp && num_events < max_events) {
		vsock = TAILQ_FIRST(&group->pending_recv);
		/* If the socket is closed, just remove it from the
		 * list and do not add it to socks array */
		if (spdk_unlikely(vsock->flags.closed)) {
			vsock->flags.pending_recv = false;
			TAILQ_REMOVE(&group->pending_recv, vsock, link);
			continue;
		}

		TAILQ_REMOVE(&group->pending_recv, vsock, link);
		vsock->flags.pending_recv = false;
		nvme_tcp_qpair_sock_cb(SPDK_CONTAINEROF(vsock, struct nvme_tcp_qpair, sock));
		num_events++;
	}

	return num_events;
}

static int
xlio_sock_group_impl_close(struct spdk_xlio_sock_group_impl *group)
{
	struct spdk_xlio_ring_fd *ring_fd, *ptmp;

	/* All ring_fds should have already been removed while removing sockets from group */
	assert(TAILQ_EMPTY(&group->ring_fd));
	TAILQ_FOREACH_SAFE(ring_fd, &group->ring_fd, link, ptmp) {
		TAILQ_REMOVE(&group->ring_fd, ring_fd, link);
		free(ring_fd);
	}

	return 0;
}

static ssize_t
xlio_sock_recv_zcopy(struct spdk_xlio_sock *vsock, size_t len, struct spdk_sock_buf **sock_buf)
{
	struct spdk_xlio_sock_group_impl *group = vsock->group_impl;
	struct xlio_sock_buf *prev_buf = NULL;
	int ret;

	SPDK_DEBUGLOG(nvme_xlio, "sock %p: zcopy recv %lu bytes\n", vsock, len);
	assert(vsock->flags.recv_zcopy);
	*sock_buf = NULL;

	if (STAILQ_EMPTY(&vsock->received_packets)) {
		if (spdk_unlikely(!vsock->group_impl)) {
			ret = poll_no_group_socket(vsock);
			if (ret < 0) {
				if (vsock->flags.disconnected) {
					return 0;
				}
				return ret;
			}
		} else {
			if (vsock->flags.disconnected) {
				return 0;
			}
			errno = EAGAIN;
			return -1;
		}
	}

	assert(!STAILQ_EMPTY(&vsock->received_packets));
	ret = 0;
	while (len > 0) {
		void *data;
		size_t chunk_len;
		struct xlio_sock_buf *buf;
		struct xlio_sock_packet *packet;

		chunk_len = packets_next_chunk(vsock, &data, &packet, len);
		if (chunk_len == 0) {
			/* No more data */
			break;
		}

		assert(chunk_len <= len);
		buf = spdk_mempool_get(g_xlio_buffers_pool);
		if (spdk_unlikely(!buf)) {
			SPDK_DEBUGLOG(nvme_xlio, "sock %p: no more buffers, total_len %d\n", vsock, ret);
			if (spdk_unlikely(group && !vsock->flags.pending_recv)) {
				assert(vsock->ring_fd);
				vsock->flags.pending_recv = true;
				SPDK_DEBUGLOG(nvme_xlio, "sock %p, insert to pending_recv\n", vsock);
				TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
			}
			if (ret == 0) {
				ret = -1;
				errno = EAGAIN;
			}
			break;
		}

		buf->sock_buf.iov.iov_base = data;
		buf->sock_buf.iov.iov_len = chunk_len;
		buf->sock_buf.next = NULL;
		buf->packet = packet;
		packet->refs++;
		if (prev_buf) {
			prev_buf->sock_buf.next = &buf->sock_buf;
		} else {
			*sock_buf = &buf->sock_buf;
		}

		packets_advance(vsock, chunk_len);
		len -= chunk_len;
		ret += chunk_len;
		prev_buf = buf;
		SPDK_DEBUGLOG(nvme_xlio, "sock %p: add buffer %p, len %lu, total_len %d\n",
			      vsock, buf, buf->sock_buf.iov.iov_len, ret);
	}

	SPDK_DEBUGLOG(nvme_xlio, "sock %p: recv_zcopy ret %d\n", vsock, ret);
	return ret;
}

#define BUFS_BULK_SIZE 32

static int
xlio_sock_free_bufs(struct spdk_xlio_sock *sock, struct spdk_sock_buf *sock_buf)
{
	void *bufs_bulk[BUFS_BULK_SIZE];
	uint32_t bufs_count = 0;

	while (sock_buf) {
		struct xlio_sock_buf *buf = SPDK_CONTAINEROF(sock_buf,
					    struct xlio_sock_buf,
					    sock_buf);
		struct xlio_sock_packet *packet = buf->packet;
		struct spdk_sock_buf *next = buf->sock_buf.next;

		bufs_bulk[bufs_count++] = (void *)buf;
		if (bufs_count == BUFS_BULK_SIZE) {
			spdk_mempool_put_bulk(g_xlio_buffers_pool, bufs_bulk, bufs_count);
			bufs_count = 0;
		}
		if (--packet->refs == 0) {
			xlio_sock_free_packet(sock, packet);
		}

		sock_buf = next;
	}

	if (bufs_count) {
		spdk_mempool_put_bulk(g_xlio_buffers_pool, bufs_bulk, bufs_count);
	}

	return 0;
}

static void
__attribute__((destructor))
nvme_tcp_cleanup(void)
{
	xlio_sock_free_pools();
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

struct nvme_tcp_poll_group {
	struct spdk_nvme_transport_poll_group group;
	struct spdk_xlio_sock_group_impl xlio_group;
	uint32_t completions_per_qpair;
	int64_t num_completions;

	void *tcp_reqs;
	TAILQ_HEAD(, nvme_tcp_pdu) free_pdus;
	void *recv_pdus;
	TAILQ_HEAD(, nvme_tcp_qpair) needs_poll;
	struct spdk_nvme_tcp_stat stats;
};

static struct spdk_nvme_tcp_stat g_dummy_stats = {};

static void nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req);
static int64_t nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group
		*tgroup, uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
static void nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				  struct spdk_nvme_cpl *rsp, bool print_on_error);
static struct nvme_tcp_req *get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid);
static int nvme_tcp_qpair_free_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
static int nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
		struct nvme_tcp_req *tcp_req);
static bool nvme_tcp_memory_domain_enabled(void);

static inline bool
nvme_tcp_pdu_is_zcopy(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	return (tcp_req &&
		nvme_payload_type(&tcp_req->req.payload) == NVME_PAYLOAD_TYPE_ZCOPY);
}

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

static inline struct nvme_tcp_poll_group *
nvme_tcp_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return SPDK_CONTAINEROF(group, struct nvme_tcp_poll_group, group);
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

static int
nvme_tcp_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service)
{
	struct addrinfo *res;
	struct addrinfo hints;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	ret = getaddrinfo(addr, service, &hints, &res);
	if (ret) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(ret), ret);
		return ret;
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n", (size_t)res->ai_addrlen);
		ret = -EINVAL;
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	return ret;
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
	TAILQ_INIT(&tqpair->send_queue);
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
nvme_tcp_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_xlio_sock *sock = &tqpair->sock;
	struct nvme_tcp_poll_group *group;

	if (tqpair->flags.needs_poll) {
		group = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE(&group->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = false;
	}

	nvme_tcp_qpair_abort_reqs(qpair, 0);
	xlio_sock_release_packets(sock);

	if (qpair->outstanding_zcopy_reqs == 0 && sock->consumed_packets == 0) {
		xlio_sock_close(sock);
	} else {
		SPDK_WARNLOG("qpair %p %u, sock %p: can't close, %u zcopy reqs, consumed_packets %u\n",
			     tqpair, qpair->id, sock, qpair->outstanding_zcopy_reqs, sock->consumed_packets);
	}

	/* clear the send_queue */
	TAILQ_INIT(&tqpair->send_queue);

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
}

static int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;

	assert(qpair != NULL);
	tqpair = nvme_tcp_qpair(qpair);
	nvme_tcp_qpair_abort_reqs(qpair, 0);
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
	free(tqpair);

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
	 * NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED, need to add it to needs_poll list too to make
	 * forward progress in case that the resources are released after icreq's or CONNECT's
	 * resp is processed. */
	if (tqpair->qpair.poll_group && !tqpair->flags.needs_poll &&
	    (!STAILQ_EMPTY(&tqpair->qpair.queued_req) ||
	     tqpair->state == NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL ||
	     tqpair->state == NVME_TCP_QPAIR_STATE_ICRESP_RECEIVED)) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = true;
	}

	TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);

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
	struct spdk_sock_buf *sock_buf = tcp_req->sock_buf;
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
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	tqpair->stats->submitted_requests++;
	xlio_sock_writev_async(&tqpair->sock, pdu);

	return 0;
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
	pdu->iovs[0].iov_base = (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset;
	pdu->iovs[0].iov_len = req->payload_size;
	pdu->data_iovcnt = 1;

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

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

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	max_num_sgl = spdk_min(req->qpair->ctrlr->max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS);
	remaining_size = req->payload_size;

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

static int
nvme_tcp_build_sgl_passthru_request(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.opts != NULL);
	assert(req->payload.opts->iov != NULL);
	assert(req->payload.opts->iovcnt != 0);

	pdu->iovs = req->payload.opts->iov;
	pdu->data_iovcnt = req->payload.opts->iovcnt;

	return 0;
}

static inline int
nvme_tcp_build_zcopy_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu = &tcp_req->pdu;
#ifdef DEBUG
	struct nvme_request *req = &tcp_req->req;
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_ZCOPY);
#endif
	pdu->iovs = tcp_req->iovs;
	pdu->iovcnt = 0;
	pdu->data_iovcnt = 0;
	return 0;
}

static int
nvme_tcp_req_build(struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = &tcp_req->req;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req.qpair);;
	int rc;
	enum nvme_payload_type payload_type = nvme_payload_type(&req->payload);
	enum spdk_nvme_data_transfer xfer;
	uint32_t max_in_capsule_data_size;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;

	SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock %p, tcp_req %p pdu %p, p_type %d, passthru %d\n", tqpair,
		      tqpair->qpair.id, &tqpair->sock, tcp_req, &tcp_req->pdu, payload_type,
		      req->payload.opts != NULL && req->payload.opts->iov != NULL);

	switch (payload_type) {
	case NVME_PAYLOAD_TYPE_CONTIG:
		rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
		break;
	case NVME_PAYLOAD_TYPE_SGL:
		if (req->payload.opts != NULL && req->payload.opts->iov != NULL) {
			rc = nvme_tcp_build_sgl_passthru_request(tcp_req);
		} else {
			rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
		}
		break;
	case NVME_PAYLOAD_TYPE_ZCOPY:
		rc = nvme_tcp_build_zcopy_request(tqpair, tcp_req);
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
	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && req->payload_size <= max_in_capsule_data_size) {
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
		SPDK_ERRLOG("tqair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
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

		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + tcp_req->req.payload_size);
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
	xlio_sock_writev_async(&tqpair->sock, pdu);

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
	struct spdk_io_channel *accel_ch;
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

	accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
	assert(accel_ch);
	if (tqpair->flags.host_ddgst_enable) {
		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + req->payload_size);

		if (!skip_copy) {
			rc = spdk_accel_append_copy_crc32c(&accel_seq, accel_ch,
							   ddgst,
							   &tcp_req->iobuf_iov, 1, NULL, NULL,
							   pdu->iovs, pdu->data_iovcnt,
							   req->payload.opts->memory_domain,
							   req->payload.opts->memory_domain_ctx,
							   0, NULL, NULL);
			skip_copy = true;
		} else {
			rc = spdk_accel_append_crc32c(&accel_seq, accel_ch, ddgst,
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
		rc = spdk_accel_append_copy(&accel_seq, accel_ch, &tcp_req->iobuf_iov, 1, NULL, NULL, pdu->iovs,
					    pdu->data_iovcnt, req->payload.opts->memory_domain,
					    req->payload.opts->memory_domain_ctx, 0, NULL, NULL);
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

	SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock %p, tcp_req %p pdu %p, ordering %x\n", tqpair,
		      tqpair->qpair.id, &tqpair->sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw);

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

	tcp_req->ordering.bits.send_ack = 1;
	SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock %p, tcp_req %p pdu %p, ordering %x cid %u\n", tqpair,
		      tqpair->qpair.id, &tqpair->sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw, tcp_req->cid);
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
				struct spdk_rdma_memory_translation_ctx *_ctx)
{
	struct nvme_request *req = &tcp_req->req;
	struct spdk_memory_domain_translation_result dma_translation;
	struct spdk_rdma_utils_memory_translation rdma_translation;
	int rc;

	if (tcp_req->ordering.bits.has_memory_domain) {
		assert(_ctx);
		struct ibv_qp dst_qp = {
			.pd = tqpair->sock.pd
		};
		struct spdk_memory_domain_translation_ctx dst_domain_ctx = {
			.size = sizeof(struct spdk_memory_domain_translation_ctx),
			.rdma.ibv_qp = &dst_qp
		};
		dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

		rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx,
						       tqpair->memory_domain->domain, &dst_domain_ctx,
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
		struct spdk_rdma_memory_translation_ctx ctx = {
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

	if (tcp_req->req.payload_size == 0 || !tcp_req->ordering.bits.in_capsule_data) {
		pdu->data_len = 0;
		goto end;
	}

	/* data + digest */
	pdu->data_len = tcp_req->req.payload_size;

	capsule_cmd->common.pdo = pdo;
	plen += tcp_req->req.payload_size;
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

	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);

	SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock %p, tcp_req %p pdu %p, ordering 0x%x cid %u\n", tqpair,
		      tqpair->qpair.id, &tqpair->sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw, tcp_req->cid);

	if (tcp_req->ordering.bits.needs_accel_seq) {
		assert(tcp_req->req.accel_sequence);
		tcp_req->ordering.bits.in_progress_accel = 1;
		/* TODO: finish accel sequence and send capsule simultaneously */
		spdk_accel_sequence_finish(tcp_req->req.accel_sequence, _nvme_tcp_accel_finished_in_capsule,
					   tcp_req);
		return 0;
	}

	pdu->has_mkeys = !nvme_qpair_is_admin_queue(&tqpair->qpair) &&
			 &tcp_req->req != tqpair->qpair.reserved_req && pdu->data_len;

	tqpair->stats->submitted_requests++;
	xlio_sock_writev_async(&tqpair->sock, pdu);

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
		tcp_req->iobuf_iov.iov_len = req->payload_size + SPDK_NVME_TCP_DIGEST_LEN *
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
nvme_tcp_qpair_free_request(struct spdk_nvme_qpair *qpair,
			    struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;
	int rc = 0;

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_ZCOPY);

	assert(qpair != NULL);
	tqpair = nvme_tcp_qpair(qpair);
	tcp_req = get_nvme_active_req_by_cid(tqpair, req->cmd.cid);
	if (spdk_likely(tcp_req)) {
		xlio_sock_free_bufs(&tqpair->sock, tcp_req->sock_buf);
		tcp_req->pdu.iovcnt = 0;
		tcp_req->pdu.data_iovcnt = 0;
		tcp_req->pdu.data_len = 0;
		tcp_req->sock_buf = NULL;
		nvme_tcp_req_put(tqpair, tcp_req);
	} else {
		rc = -EINVAL;
		SPDK_ERRLOG("Failed to find request to free: cid %u\n", req->cmd.cid);
	}

	req->zcopy.iovs = NULL;
	req->zcopy.iovcnt = 0;
	nvme_free_request(req);

	/* Zcopy requests may be queued for waiting resources, so set needs_poll
	 * and increase async_complete to trigger a resubmission of queued requests.
	 */
	if (tqpair->qpair.poll_group && !STAILQ_EMPTY(&tqpair->qpair.queued_req) &&
	    !tqpair->flags.needs_poll) {
		struct nvme_tcp_poll_group *pgroup;
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = true;
	}
	tqpair->async_complete++;

	return rc;
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

	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);
	assert(tqpair->stats->outstanding_reqs > 0);
	tqpair->stats->outstanding_reqs--;
	SPDK_DEBUGLOG(nvme,
		      "tqpair %p %u (out %"PRIu64"), sock %p, tcp_req %p pdu %p, ordering %x, cid %u\n", tqpair,
		      tqpair->qpair.id, tqpair->stats->outstanding_reqs, &tqpair->sock, tcp_req, &tcp_req->pdu,
		      tcp_req->ordering.raw, rsp->cid);

	/* Cache arguments to be passed to nvme_complete_request since tcp_req can be zeroed when released */
	memcpy(&cpl, rsp, sizeof(cpl));
	user_cb		= req->cb_fn;
	user_cb_arg	= req->cb_arg;

	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_ZCOPY) {
		nvme_complete_request_zcopy(req->zcopy.zcopy_cb_fn, user_cb_arg, qpair, req, &cpl);
	} else {
		/* Accel sequence is either executed or aborted. Clean pointer since nvme transport expects it
		 * to be NULL */
		req->accel_sequence = NULL;
		nvme_tcp_req_put(tqpair, tcp_req);
		nvme_complete_request(user_cb, user_cb_arg, qpair, req, &cpl);
	}
}

static void
nvme_tcp_req_accel_seq_complete_cb(void *arg, int status)
{
	struct nvme_tcp_req	*tcp_req = arg;
	struct nvme_tcp_qpair	*tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	struct nvme_request	*req;

	SPDK_DEBUGLOG(nvme, "Accel sequence completed: tcp_req %p, status %d\n", tcp_req, status);

	req = &tcp_req->req;

	assert(tcp_req->ordering.bits.in_progress_accel);
	tcp_req->ordering.bits.in_progress_accel = 0;
	spdk_nvme_request_put_zcopy_iovs(&req->zcopy);
	xlio_sock_free_bufs(&tqpair->sock, tcp_req->sock_buf);
	tcp_req->pdu.iovcnt = 0;
	tcp_req->pdu.data_iovcnt = 0;
	tcp_req->pdu.data_len = 0;
	tcp_req->sock_buf = NULL;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("tqair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
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
		if (!tqpair->flags.needs_poll) {
			struct nvme_tcp_poll_group *pgroup;

			pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
			TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
			tqpair->flags.needs_poll = true;
		}
		tqpair->flags.needs_resubmit = true;
	}
	_nvme_tcp_req_complete(tcp_req, tqpair, &tcp_req->req.cpl);
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
		struct spdk_io_channel	*accel_ch;
		struct spdk_nvme_poll_group *group = tqpair->qpair.poll_group->group;
		struct spdk_sock_buf *sock_buf;

		assert(group != 0);

		/* @todo: check if we ever need to deliver data for error completion, e.g. with data digest error */
		if (spdk_unlikely(error)) {
			goto out;
		}

		if (tcp_req->ordering.bits.digest_offloaded) {
			goto out;
		}

		assert(req->zcopy.iovcnt == 0);
		sock_buf = tcp_req->sock_buf;
		while (sock_buf) {
			req->zcopy.iovcnt++;
			sock_buf = sock_buf->next;
		}

		rc = spdk_nvme_request_get_zcopy_iovs(&req->zcopy);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to allocate zcopy iovs count\n");
			goto out;
		}

		req->zcopy.iovcnt = 0;
		sock_buf = tcp_req->sock_buf;
		while (sock_buf) {
			req->zcopy.iovs[req->zcopy.iovcnt++] = sock_buf->iov;
			sock_buf = sock_buf->next;
		}

		tqpair->stats->received_data_pdus++;
		tqpair->stats->received_data_iovs += req->zcopy.iovcnt;
		if (req->zcopy.iovcnt > (int)tqpair->stats->max_data_iovs_per_pdu) {
			tqpair->stats->max_data_iovs_per_pdu = req->zcopy.iovcnt;
		}

		accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
		if (spdk_unlikely(!accel_ch)) {
			SPDK_ERRLOG("Failed to get accel io channel\n");
		}

		accel_seq = req->accel_sequence;
		if (accel_seq) {
			task = spdk_accel_sequence_first_task(accel_seq);
			if (task->op_code == SPDK_ACCEL_OPC_DECRYPT && spdk_accel_sequence_next_task(task) == NULL) {
				skip_copy = true;
				task->src_domain = NULL;
				task->src_domain_ctx = NULL;
				task->s.iovs = req->zcopy.iovs;
				task->s.iovcnt = req->zcopy.iovcnt;
			}
		}
		if (!skip_copy) {
			rc = spdk_accel_append_copy(&accel_seq, accel_ch,
						    tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
						    req->payload.opts->memory_domain,
						    req->payload.opts->memory_domain_ctx,
						    req->zcopy.iovs, req->zcopy.iovcnt,
						    NULL, NULL, 0, NULL, NULL);
			if (spdk_unlikely(rc)) {
				SPDK_ERRLOG("Failed to append copy accel task, rc %d\n", rc);
				spdk_nvme_request_put_zcopy_iovs(&req->zcopy);
				goto out;
			}

		}
		spdk_accel_sequence_reverse(accel_seq);
		tcp_req->ordering.bits.in_progress_accel = 1;
		spdk_accel_sequence_finish(accel_seq, nvme_tcp_req_accel_seq_complete_cb, tcp_req);
		return;
	}

out:
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		spdk_nvme_request_put_zcopy_iovs(&req->zcopy);
		xlio_sock_free_bufs(&tqpair->sock, tcp_req->sock_buf);
		tcp_req->pdu.data_iovcnt = 0;
		tcp_req->pdu.iovcnt = 0;
		tcp_req->pdu.data_len = 0;
		tcp_req->sock_buf = NULL;
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
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock %p, req %p, ordering 0x%x\n", tqpair, tqpair->qpair.id,
			      &tqpair->sock, tcp_req, tcp_req->ordering.raw);
		if (tcp_req->ordering.bits.in_progress_accel) {
			continue;
		}
		spdk_nvme_request_put_zcopy_iovs(&tcp_req->req.zcopy);
		if (tcp_req->sock_buf) {
			xlio_sock_free_bufs(&tqpair->sock, tcp_req->sock_buf);
			tcp_req->sock_buf = NULL;
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
		rsp->status.p = tcp_req->datao != tcp_req->req.payload_size;
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
	nvme_tcp_c2h_term_req_dump(&tqpair->ctrl_hdr.term_req);
	nvme_tcp_qpair_abort_reqs(&tqpair->qpair, 0);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
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
tcp_data_recv_crc32_done(void *cb_arg, int status)
{
	struct nvme_tcp_req *tcp_req = cb_arg;
	struct nvme_tcp_pdu *pdu;
	struct nvme_tcp_qpair *tqpair;
	int rc;
	struct nvme_tcp_poll_group *pgroup;
	int dummy_reaped = 0;

	pdu = &tcp_req->pdu;

	tqpair = nvme_tcp_qpair(tcp_req->req.qpair);
	assert(tqpair != NULL);

	if (tqpair->qpair.poll_group && !STAILQ_EMPTY(&tqpair->qpair.queued_req) &&
	    !tqpair->flags.needs_poll) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = true;
	}

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", pdu);
		tcp_req->req.cpl.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		goto end;
	}

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	tqpair->stats->recv_ddgsts++;
	rc = MATCH_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);
	if (spdk_unlikely(rc == 0)) {
		SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
		tcp_req->req.cpl.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
	}

end:
	nvme_tcp_c2h_data_payload_handle(tqpair, pdu, &dummy_reaped);
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
	    !tqpair->flags.needs_poll) {
		pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = true;
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
	struct spdk_io_channel *accel_ch;
	struct spdk_accel_sequence *accel_seq;
	bool skip_copy = false;
	int rc;

	accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
	if (spdk_unlikely(!accel_ch)) {
		SPDK_ERRLOG("Failed to get accel io channel\n");
		return -EIO;
	}

	accel_seq = req->accel_sequence;
	if (accel_seq) {
		struct spdk_accel_task *task = spdk_accel_sequence_first_task(accel_seq);
		if (task->op_code == SPDK_ACCEL_OPC_DECRYPT && spdk_accel_sequence_next_task(task) == NULL) {
			skip_copy = true;
			task->src_domain = NULL;
			task->src_domain_ctx = NULL;
			task->s.iovs = req->zcopy.iovs;
			task->s.iovcnt = req->zcopy.iovcnt;
		}
	}
	if (!skip_copy && !tcp_req->ordering.bits.c2h_accel_copy) {
		rc = spdk_accel_append_copy(&accel_seq, accel_ch, tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
					    req->payload.opts->memory_domain, req->payload.opts->memory_domain_ctx,
					    req->zcopy.iovs, req->zcopy.iovcnt, NULL, NULL, 0, NULL, NULL);
		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				SPDK_WARNLOG("no task for copy\n");
				TAILQ_INSERT_TAIL(&tqpair->accel_nomem_queue, pdu, tailq);
				tqpair->flags.has_accel_nomem_pdus = 1;

				if (!tqpair->flags.needs_poll) {
					struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

					TAILQ_INSERT_TAIL(&tgroup->needs_poll, tqpair, link);
					tqpair->flags.needs_poll = 1;
				}

				return rc;
			}
			SPDK_ERRLOG("Failed to append copy accel task, rc %d\n", rc);
			return rc;
		}
		tcp_req->ordering.bits.c2h_accel_copy = 1;
	}

	rc = spdk_accel_append_check_crc32c(&accel_seq, accel_ch, &pdu->data_digest_crc32, req->zcopy.iovs,
					    req->zcopy.iovcnt, NULL, NULL, 0, NULL, NULL);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(nvme, "pdu %p, tqpair %p: no task for check_crc32c\n", pdu, tqpair);
			TAILQ_INSERT_TAIL(&tqpair->accel_nomem_queue, pdu, tailq);
			tqpair->flags.has_accel_nomem_pdus = 1;

			if (!tqpair->flags.needs_poll) {
				struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);

				TAILQ_INSERT_TAIL(&tgroup->needs_poll, tqpair, link);
				tqpair->flags.needs_poll = 1;
			}

			return rc;
		}
		SPDK_ERRLOG("Failed to append check crc accel task, rc %d\n", rc);
		goto abort_sequence;
	}

	spdk_accel_sequence_reverse(accel_seq);
	tcp_req->ordering.bits.in_progress_accel = 1;
	spdk_accel_sequence_finish(accel_seq, nvme_tcp_req_accel_seq_complete_crc_c2h_cb, tcp_req);

	return 0;
abort_sequence:
	if (!req->accel_sequence) {
		spdk_accel_sequence_abort(accel_seq);
	}
	return rc;
}

static inline int
nvme_tcp_prepare_accel_sequence_c2h(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct nvme_request *req;
	struct spdk_sock_buf *sock_buf;
	int rc;

	req = &tcp_req->req;

	assert(req->zcopy.iovcnt == 0);
	sock_buf = tcp_req->sock_buf;
	while (sock_buf) {
		req->zcopy.iovcnt++;
		sock_buf = sock_buf->next;
	}

	rc = spdk_nvme_request_get_zcopy_iovs(&req->zcopy);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to allocate zcopy iovs count\n");
		return rc;
	}

	req->zcopy.iovcnt = 0;
	sock_buf = tcp_req->sock_buf;
	while (sock_buf) {
		req->zcopy.iovs[req->zcopy.iovcnt++] = sock_buf->iov;
		sock_buf = sock_buf->next;
	}

	tqpair->stats->received_data_pdus++;
	tqpair->stats->received_data_iovs += req->zcopy.iovcnt;
	if (req->zcopy.iovcnt > (int)tqpair->stats->max_data_iovs_per_pdu) {
		tqpair->stats->max_data_iovs_per_pdu = req->zcopy.iovcnt;
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

	SPDK_DEBUGLOG(nvme, "tqpair %p %u sock %p, tcp_req %p pdu %p, ordering %x, cid %u\n", tqpair,
		      tqpair->qpair.id, &tqpair->sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw, tcp_req->cid);

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
		if ((nvme_qpair_get_state(&tqpair->qpair) >= NVME_QPAIR_CONNECTED) && tgroup != NULL &&
		    spdk_likely(recv_pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0
				&& tcp_req->req.payload_size == recv_pdu->data_len) &&
		    !nvme_tcp_pdu_is_zcopy(recv_pdu)) {
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
			} else if (tgroup->group.group->accel_fn_table.submit_accel_crc32c) {

				tgroup->group.group->accel_fn_table.submit_accel_crc32c(
					tgroup->group.group->ctx, &tcp_req->pdu.data_digest_crc32,
					tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt, 0, tcp_data_recv_crc32_done,
					tcp_req);
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

				return;
			}
		}

		if (nvme_tcp_pdu_is_zcopy(recv_pdu)) {
			crc32c = nvme_tcp_pdu_calc_data_digest_with_iov(recv_pdu, tcp_req->req.zcopy.iovs,
					tcp_req->pdu.data_iovcnt);
		} else if (nvme_tcp_req_with_memory_domain(recv_pdu->req)) {
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

	if (xlio_sock_set_recvbuf(&tqpair->sock, recv_buf_size * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR) < 0) {
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
		      "tqpair %p %u, sock %p, tcp_req %p pdu %p, ordering 0x%x: datao=%u, datal=%u, cccid=%d\n",
		      tqpair, tqpair->qpair.id, &tqpair->sock, tcp_req, &tcp_req->pdu, tcp_req->ordering.raw,
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

	if (spdk_unlikely(c2h_data->datal > tcp_req->req.payload_size)) {
		SPDK_ERRLOG("Invalid datal for tcp_req(%p), datal(%u) exceeds payload_size(%u)\n",
			    tcp_req, c2h_data->datal, tcp_req->req.payload_size);
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

	if (spdk_unlikely((c2h_data->datao + c2h_data->datal) > tcp_req->req.payload_size)) {
		SPDK_ERRLOG("Invalid data range for tcp_req(%p), received (datao(%u) + datal(%u)) > datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, c2h_data->datal, tcp_req->req.payload_size);
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
		SPDK_ERRLOG("tqair %p, req %p, accel sequence status %d\n", tqpair, tcp_req, status);
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

		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + tcp_req->req.payload_size);
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
	TAILQ_INSERT_TAIL(&tqpair->send_queue, rsp_pdu, tailq);
	tqpair->stats->submitted_requests++;
	xlio_sock_writev_async(&tqpair->sock, rsp_pdu);

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
	struct spdk_io_channel *accel_ch;
	struct spdk_accel_task *task;
	uint32_t *ddgst;
	bool skip_copy = false;
	int rc;

	SPDK_DEBUGLOG(nvme, "Write request with accel sequence h2c: tcp_req %p\n", tcp_req);

	accel_seq = req->accel_sequence;
	accel_ch = group->accel_fn_table.get_accel_channel(group->ctx);
	assert(accel_ch);

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
		ddgst = (uint32_t *)((uint8_t *)tcp_req->iobuf_iov.iov_base + tcp_req->req.payload_size);

		if (!skip_copy) {
			rc = spdk_accel_append_copy_crc32c(&accel_seq, accel_ch,
							   ddgst,
							   &tcp_req->iobuf_iov, 1, NULL, NULL,
							   tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt,
							   req->payload.opts->memory_domain,
							   req->payload.opts->memory_domain_ctx,
							   0, NULL, NULL);
			skip_copy = true;
		} else {
			rc = spdk_accel_append_crc32c(&accel_seq, accel_ch, ddgst,
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
		rc = spdk_accel_append_copy(&accel_seq, accel_ch, &tcp_req->iobuf_iov, 1, NULL, NULL,
					    tcp_req->pdu.iovs, tcp_req->pdu.data_iovcnt, req->payload.opts->memory_domain,
					    req->payload.opts->memory_domain_ctx, 0, NULL, NULL);
		if (spdk_unlikely(rc)) {
			return rc;
		}
	}

	tcp_req->ordering.bits.in_progress_accel = 1;
	spdk_accel_sequence_finish(accel_seq, nvme_tcp_accel_seq_finished_h2c_cb, tcp_req);

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

	TAILQ_INSERT_TAIL(&tqpair->send_queue, rsp_pdu, tailq);
	rsp_pdu->has_mkeys = 1;
	if (spdk_unlikely(nvme_tcp_fill_data_mkeys(tqpair, tcp_req, rsp_pdu) != 0)) {
		SPDK_ERRLOG("Failed to fill mkeys\n");
		goto fail_req;
	}

	tqpair->stats->submitted_requests++;
	xlio_sock_writev_async(&tqpair->sock, rsp_pdu);

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

	if (spdk_unlikely((r2t->r2tl + r2t->r2to) > tcp_req->req.payload_size)) {
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
nvme_nvda_tcp_readv_data(struct spdk_xlio_sock *sock, struct iovec *iov, int iovcnt)
{
	int ret;

	assert(sock != NULL);
	assert(iov);
	assert(iovcnt);

	ret = xlio_sock_readv(sock, iov, iovcnt);
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
nvme_tcp_read_digest(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *pdu, uint32_t len)
{
	struct iovec iov;

	assert(len <= SPDK_NVME_TCP_DIGEST_LEN);

	iov.iov_base = pdu->data_digest + SPDK_NVME_TCP_DIGEST_LEN - len;
	iov.iov_len = len;

	return nvme_nvda_tcp_readv_data(sock, &iov, 1);
}

static int
nvme_tcp_read_payload_data_zcopy(struct spdk_xlio_sock *vsock, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req = pdu->req;
	struct spdk_sock_buf *sock_buf;
	int ret = 0;

	if (pdu->data_len > pdu->rw_offset) {
		int rc = 0;
		size_t len = pdu->data_len - pdu->rw_offset;

		ret = xlio_sock_recv_zcopy(vsock, len, &sock_buf);
		if (spdk_unlikely(ret <= 0)) {
			goto fail;
		}

		SPDK_DEBUGLOG(nvme, "sock %p, pdu %p: requested %zu, got %d bytes\n", vsock, pdu, len, ret);
		if (tcp_req->sock_buf) {
			struct spdk_sock_buf *cur_buf = tcp_req->sock_buf;

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

		/* We got all the data. Setup iovs */
		sock_buf = tcp_req->sock_buf;

		assert(tcp_req->req.zcopy.iovcnt == 0);
		while (sock_buf) {
			tcp_req->req.zcopy.iovcnt++;
			sock_buf = sock_buf->next;
		}

		if (spdk_unlikely(tcp_req->req.zcopy.iovcnt > NVME_MAX_ZCOPY_IOVS)) {
			/* fallback memcopy */
			tcp_req->req.zcopy.iovcnt = 0;
			rc = spdk_nvme_request_get_zcopy_buffers(&tcp_req->req, pdu->data_len);
			if (rc == 0) {
				size_t dst_offset = 0;

				tcp_req->pdu.data_iovcnt = tcp_req->req.zcopy.iovcnt;
				sock_buf = tcp_req->sock_buf;
				while (sock_buf) {
					dst_offset += spdk_copy_iov_with_offset(&sock_buf->iov, 1,
										tcp_req->req.zcopy.iovs,
										tcp_req->req.zcopy.iovcnt,
										dst_offset);

					sock_buf = sock_buf->next;
				}

				xlio_sock_free_bufs(vsock, tcp_req->sock_buf);
				tcp_req->sock_buf = NULL;

				SPDK_DEBUGLOG(nvme, "Payload is split into %d iovs\n", tcp_req->pdu.data_iovcnt);
			}
		} else {
			if (tcp_req->req.zcopy.iovcnt <= NVME_TCP_MAX_SGL_DESCRIPTORS) {
				tcp_req->req.zcopy.iovs = tcp_req->pdu.iovs;
			} else {
				rc = spdk_nvme_request_get_zcopy_iovs(&tcp_req->req.zcopy);
			}
			if (rc == 0) {
				assert(tcp_req->pdu.data_iovcnt == 0);
				sock_buf = tcp_req->sock_buf;
				while (sock_buf) {
					tcp_req->req.zcopy.iovs[tcp_req->pdu.data_iovcnt++] = sock_buf->iov;
					sock_buf = sock_buf->next;
				}
				SPDK_DEBUGLOG(nvme, "Payload is split into %d iovs\n", tcp_req->pdu.data_iovcnt);
			}
		}

		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to set zcopy iov\n");
			xlio_sock_free_bufs(vsock, tcp_req->sock_buf);
			tcp_req->sock_buf = NULL;
			tcp_req->req.cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (pdu->ddgst_enable) {
		int ret_dgst = nvme_tcp_read_digest(vsock, pdu, SPDK_NVME_TCP_DIGEST_LEN +
						    pdu->data_len - pdu->rw_offset - ret);
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

static int
nvme_tcp_read_payload_data_memory_domain(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *recv_pdu)
{
	struct nvme_tcp_req *tcp_req = recv_pdu->req;
	struct spdk_sock_buf *sock_buf;
	int ret = 0;

	if (recv_pdu->data_len > recv_pdu->rw_offset) {
		size_t len = recv_pdu->data_len - recv_pdu->rw_offset;

		ret = xlio_sock_recv_zcopy(sock, len, &sock_buf);
		if (spdk_unlikely(ret <= 0)) {
			goto fail;
		}
		SPDK_DEBUGLOG(nvme, "sock %p, pdu %p: requested %zu, got %d bytes\n", sock, recv_pdu, len, ret);

		if (tcp_req->sock_buf) {
			struct spdk_sock_buf *cur_buf = tcp_req->sock_buf;

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
		int ret_dgst = nvme_tcp_read_digest(sock, recv_pdu, SPDK_NVME_TCP_DIGEST_LEN +
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
nvme_nvda_tcp_read_payload_data(struct spdk_xlio_sock *sock, struct nvme_tcp_pdu *pdu)
{
	struct iovec iov[NVME_TCP_MAX_SGL_DESCRIPTORS + 1];
	int iovcnt;

	/* TODO: find better way, without copying to iovs on the stack */
	iovcnt = nvme_tcp_build_payload_iovs(iov, NVME_TCP_MAX_SGL_DESCRIPTORS + 1, pdu, pdu->ddgst_enable);
	assert(iovcnt >= 0);

	return nvme_nvda_tcp_readv_data(sock, iov, iovcnt);
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

				rc = nvme_nvda_tcp_readv_data(&tqpair->sock, &iov, 1);
				if (spdk_unlikely(rc < 0)) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
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
				rc = nvme_nvda_tcp_readv_data(&tqpair->sock, &iov, 1);
				if (spdk_unlikely(rc < 0)) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
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
			rc = nvme_nvda_tcp_readv_data(&tqpair->sock, &iov, 1);
			if (spdk_unlikely(rc < 0)) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
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

			if (nvme_tcp_pdu_is_zcopy(pdu)) {
				rc = nvme_tcp_read_payload_data_zcopy(&tqpair->sock, pdu);
			} else if (nvme_tcp_req_with_memory_domain(pdu->req)) {
				rc = nvme_tcp_read_payload_data_memory_domain(&tqpair->sock, pdu);
			} else {
				rc = nvme_nvda_tcp_read_payload_data(&tqpair->sock, pdu);
			}
			if (spdk_unlikely(rc < 0)) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
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

static int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	if (spdk_unlikely(qpair->poll_group == NULL &&
			  tqpair->state >= NVME_TCP_QPAIR_STATE_SOCK_CONNECTED)) {
		rc = xlio_sock_flush(&tqpair->sock);
		if (spdk_unlikely(rc < 0)) {
			SPDK_ERRLOG("Failed to flush tqpair=%p (%d): %s\n", tqpair,
				    errno, spdk_strerror(errno));
			if (spdk_unlikely(tqpair->qpair.ctrlr->timeout_enabled)) {
				nvme_tcp_qpair_check_timeout(qpair);
			}
			if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
				if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
					nvme_transport_ctrlr_disconnect_qpair_done(qpair);
				}
				return 0;
			}
			goto fail;
		}
	}

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

	if (spdk_unlikely(tqpair->qpair.ctrlr->timeout_enabled)) {
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
nvme_tcp_qpair_sock_cb(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_qpair *qpair = &tqpair->qpair;
	struct nvme_tcp_poll_group *pgroup = nvme_tcp_poll_group(qpair->poll_group);
	int32_t num_completions;

	if (tqpair->flags.needs_poll) {
		TAILQ_REMOVE(&pgroup->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = false;
		if (tqpair->flags.needs_resubmit) {
			tqpair->flags.needs_resubmit = false;
			SPDK_DEBUGLOG(nvme, "tqpair %p %u, sock %p\n", tqpair, tqpair->qpair.id, &tqpair->sock);
			nvme_qpair_resubmit_requests(qpair, tqpair->num_entries - tqpair->stats->outstanding_reqs);
		}
		if (spdk_unlikely(tqpair->flags.has_accel_nomem_pdus)) {
			/* For now it only works for C2H payload */
			tqpair->flags.has_accel_nomem_pdus = 0;
			nvme_tcp_qpair_resubmit_accel_nomem(tqpair);
		}
	}

	num_completions = spdk_nvme_qpair_process_completions(qpair, pgroup->completions_per_qpair);

	if (pgroup->num_completions >= 0 && num_completions >= 0) {
		pgroup->num_completions += num_completions;
		pgroup->stats.nvme_completions += num_completions;
	} else {
		pgroup->num_completions = -ENXIO;
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
nvme_tcp_qpair_connect_sock_done(struct spdk_xlio_sock *vsock, int err)
{
	struct nvme_tcp_qpair *tqpair = SPDK_CONTAINEROF(vsock, struct nvme_tcp_qpair, sock);
	struct spdk_nvme_qpair *qpair = &tqpair->qpair;
	struct nvme_tcp_poll_group *tgroup = NULL;
	void *tcp_reqs;
	struct spdk_rdma_utils_memory_translation mem_translation = {};
	char *tcp_mem_domain = getenv("SPDK_NVDA_TCP_USE_TCP_MEM_DOMAIN");
	int rc;

	assert(tqpair->state == NVME_TCP_QPAIR_STATE_CONNECTING ||
	       tqpair->state == NVME_TCP_QPAIR_STATE_INVALID);

	if (err) {
		goto fail;
	}

	if (tcp_mem_domain) {
		tqpair->memory_domain = spdk_rdma_utils_get_memory_domain(vsock->pd,
					SPDK_DMA_DEVICE_TYPE_RDMA_TCP);
	} else {
		tqpair->memory_domain = spdk_rdma_utils_get_memory_domain(vsock->pd,
					SPDK_DMA_DEVICE_TYPE_RDMA);
	}

	if (!tqpair->memory_domain) {
		SPDK_ERRLOG("Failed to get memory domain\n");
		goto fail;
	}

	SPDK_DEBUGLOG(nvme, "TCP qpair %p %u, PD %p\n", tqpair, tqpair->qpair.id, vsock->pd);

	tqpair->mem_map = spdk_rdma_utils_create_mem_map(vsock->pd, NULL,
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
		if (tqpair->qpair.poll_group && !tqpair->flags.needs_poll) {
			tgroup = nvme_tcp_poll_group(qpair->poll_group);
			TAILQ_INSERT_TAIL(&tgroup->needs_poll, tqpair, link);
			tqpair->flags.needs_poll = true;
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
	long int port;
	struct spdk_sock_impl_opts impl_opts;
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;

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
	rc = nvme_tcp_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_tcp_parse_addr() failed\n");
		return rc;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_tcp_parse_addr(&src_addr, family, ctrlr->opts.src_addr, ctrlr->opts.src_svcid);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_tcp_parse_addr() failed\n");
			return rc;
		}
	}

	port = spdk_strtol(ctrlr->trid.trsvcid, 10);
	if (port <= 0 || port >= INT_MAX) {
		SPDK_ERRLOG("Invalid port: %s\n", ctrlr->trid.trsvcid);
		rc = -1;
		return rc;
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
	if (ctrlr->opts.transport_ack_timeout) {
		opts.ack_timeout = 1ULL << ctrlr->opts.transport_ack_timeout;
	}
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = sizeof(impl_opts);
	rc = xlio_sock_init(&tqpair->sock, ctrlr->trid.traddr, port, &opts,
			    nvme_qpair_is_admin_queue(qpair),
			    ctrlr->opts.vlan_tag);
	if (rc) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with addr=%s, port=%ld, rc %d\n",
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
	if (tqpair->flags.in_connect_poll) {
		return -EAGAIN;
	}

	tqpair->flags.in_connect_poll = 1;

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

	tqpair->flags.in_connect_poll = 0;
	return rc;
}

static int
nvme_tcp_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;
	struct nvme_tcp_qpair *tqpair;

	tqpair = nvme_tcp_qpair(qpair);

	if (!tqpair->sock.fd) {
		rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
		if (rc < 0) {
			return rc;
		}
	}

	if (qpair->poll_group) {
		rc = nvme_poll_group_connect_qpair(qpair);
		if (rc) {
			SPDK_ERRLOG("Unable to activate the tcp qpair.\n");
			return rc;
		}
	} else if (!tqpair->stats) {
		tqpair->stats = calloc(1, sizeof(*tqpair->stats));
		if (!tqpair->stats) {
			SPDK_ERRLOG("tcp stats memory allocation failed\n");
			return -ENOMEM;
		}
	}

	/* Explicitly set the recv_state of tqpair */
	if (tqpair->recv_state != NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	}

	/* We can send icreq only when socket is connected. If it didn't happen yet, just wait. */
	if (tqpair->state == NVME_TCP_QPAIR_STATE_SOCK_CONNECTED) {
		tqpair->state = NVME_TCP_QPAIR_STATE_ICREQ_SEND;
		if (tqpair->qpair.poll_group && !tqpair->flags.needs_poll) {
			struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(qpair->poll_group);
			TAILQ_INSERT_TAIL(&tgroup->needs_poll, tqpair, link);
			tqpair->flags.needs_poll = true;
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
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		SPDK_ERRLOG("Failed to create qpair with size %u. Minimum queue size is %d.\n",
			    qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}
	rc = posix_memalign((void **)&tqpair, 64, sizeof(struct nvme_tcp_qpair));
	if (rc) {
		SPDK_ERRLOG("failed to create tqpair\n");
		return NULL;
	}
	memset(tqpair, 0, sizeof(struct nvme_tcp_qpair));

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	tqpair->num_entries = qsize - 1;
	qpair = &tqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, 1, num_requests, async);
	if (rc != 0) {
		free(tqpair);
		return NULL;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);
		return NULL;
	}

	/* spdk_nvme_qpair_get_optimal_poll_group needs socket information.
	 * So create the socket first when creating a qpair. */
	rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
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
	struct nvme_tcp_qpair *tqpair;
	int rc;

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

	tqpair = nvme_tcp_qpair(tctrlr->ctrlr.adminq);
	if (tqpair->sock.flags.recv_zcopy) {
		tctrlr->ctrlr.flags |= SPDK_NVME_CTRLR_ZCOPY_SUPPORTED;
		SPDK_NOTICELOG("Controller supports zero copy API\n");
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

		spdk_iobuf_get_opts(&iobuf_opts);
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
	struct nvme_tcp_poll_group *group = calloc(1, sizeof(*group));
	struct nvme_tcp_req	*tcp_req;
	struct nvme_tcp_pdu	*pdu;
	size_t req_size_padded, pdu_size_padded;
	uint16_t num_requests, i;
	int rc;

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	TAILQ_INIT(&group->needs_poll);

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

	rc = xlio_sock_group_impl_create(&group->xlio_group);
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

static int
nvme_tcp_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_xlio_sock *vsock = &tqpair->sock;

	if (xlio_sock_group_impl_add_sock(&group->xlio_group, vsock)) {
		return -EPROTO;
	}
	return 0;
}

static int
nvme_tcp_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (tqpair->flags.needs_poll) {
		TAILQ_REMOVE(&group->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = false;
	}

	if (xlio_sock_group_impl_remove_sock(&group->xlio_group, &tqpair->sock)) {
		return -EPROTO;
	}
	return 0;
}

static int
nvme_tcp_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);
	struct spdk_xlio_sock *vsock = &tqpair->sock;

	/* disconnected qpairs won't have a sock to add. */
	if (nvme_qpair_get_state(qpair) >= NVME_QPAIR_CONNECTED) {
		if (xlio_sock_group_impl_add_sock(&group->xlio_group, vsock)) {
			return -EPROTO;
		}
	}

	tqpair->recv_pdu = NULL;
	tqpair->stats = &group->stats;
	tqpair->flags.shared_stats = true;

	if (group->tcp_reqs != NULL) {
		tqpair->flags.use_poll_group_req_pool = 1;
		qpair->active_free_req = &tgroup->free_req;
	}

	return 0;
}

static int
nvme_tcp_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			   struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *group;

	assert(qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs);

	tqpair = nvme_tcp_qpair(qpair);
	group = nvme_tcp_poll_group(tgroup);

	assert(tqpair->flags.shared_stats == true);
	tqpair->stats = &g_dummy_stats;

	if (tqpair->flags.needs_poll) {
		TAILQ_REMOVE(&group->needs_poll, tqpair, link);
		tqpair->flags.needs_poll = false;
	}

	assert(tqpair->sock.flags.closed);

	return 0;
}

static int64_t
nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
					uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	struct nvme_tcp_qpair *tqpair, *tmp_tqpair;
	int num_events;

	group->completions_per_qpair = completions_per_qpair;
	group->num_completions = 0;
	group->stats.polls++;

	num_events = xlio_sock_group_impl_poll(&group->xlio_group, MAX_EVENTS_PER_POLL);

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		tqpair = nvme_tcp_qpair(qpair);
		if (qpair->outstanding_zcopy_reqs > 0 || tqpair->sock.consumed_packets > 0) {
			SPDK_DEBUGLOG(nvme, "qpair %p %u, sock %p: can't close, zcopy_reqs %u, packets %u\n",
				      tqpair, qpair->id, &tqpair->sock, qpair->outstanding_zcopy_reqs, tqpair->sock.consumed_packets);
			continue;
		}

		if (!tqpair->sock.flags.closed) {
			xlio_sock_close(&tqpair->sock);
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

	/* If any qpairs were marked as needing to be polled due to an asynchronous write completion
	 * and they weren't polled as a consequence of calling spdk_sock_group_poll above, poll them now. */
	TAILQ_FOREACH_SAFE(tqpair, &group->needs_poll, link, tmp_tqpair) {
		nvme_tcp_qpair_sock_cb(tqpair);
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

	rc = xlio_sock_group_impl_close(&group->xlio_group);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to close the sock group for a tcp poll group.\n");
		assert(false);
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
		domains[0] = tqpair->memory_domain->domain;
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
	.poll_group_connect_qpair = nvme_tcp_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_tcp_poll_group_disconnect_qpair,
	.poll_group_add = nvme_tcp_poll_group_add,
	.poll_group_remove = nvme_tcp_poll_group_remove,
	.poll_group_process_completions = nvme_tcp_poll_group_process_completions,
	.poll_group_destroy = nvme_tcp_poll_group_destroy,
	.poll_group_get_stats = nvme_tcp_poll_group_get_stats,
	.poll_group_free_stats = nvme_tcp_poll_group_free_stats,

	.qpair_free_request = nvme_tcp_qpair_free_request,
};

SPDK_NVME_TRANSPORT_REGISTER(tcp, &tcp_ops);

SPDK_TRACE_REGISTER_FN(nvme_nvda_tcp, "nvme_nvda_tcp", TRACE_GROUP_NVME_NVDA_TCP)
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
	spdk_trace_register_owner(OWNER_NVME_NVDA_TCP_QP, 'q');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
