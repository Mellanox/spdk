/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/stdinc.h"
#include "spdk/env.h"

#include <sys/epoll.h>
#include <linux/errqueue.h>

#include <dlfcn.h>

#include <infiniband/verbs.h>
#include <mellanox/xlio_extra.h>

#include "spdk/log.h"
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/sock.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define XLIO_PACKETS_BUF_SIZE 128
/* @todo: make pool sizes configurable */
#define RECV_ZCOPY_PACKETS_POOL_SIZE 1024
#define RECV_ZCOPY_BUFFERS_POOL_SIZE 4096
#define DEFAULT_XLIO_PATH "libxlio.so"

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

#ifndef SPDK_ZEROCOPY
#error "XLIO requires zcopy"
#endif

enum {
	IOCTL_USER_ALLOC_TX = (1 << 0),
	IOCTL_USER_ALLOC_RX = (1 << 1),
	IOCTL_USER_ALLOC_TX_ZC = (1 << 2)
};

struct xlio_sock_packet {
	struct xlio_recvfrom_zcopy_packet_t *xlio_packet;
	int refs;
	void *xlio_packet_id;
	STAILQ_ENTRY(xlio_sock_packet) link;
};

struct xlio_sock_buf {
	struct spdk_sock_buf sock_buf;
	struct xlio_sock_packet *packet;
};

struct spdk_xlio_sock {
	struct spdk_sock	base;
	int			fd;
	uint32_t		sendmsg_idx;
	struct ibv_pd *pd;
	bool			pending_recv;
	bool			pending_send;
	bool			zcopy;
	bool			recv_zcopy;
	int			so_priority;

	char			xlio_packets_buf[XLIO_PACKETS_BUF_SIZE];
	struct xlio_sock_packet	*packets;
	STAILQ_HEAD(, xlio_sock_packet)	free_packets;
	STAILQ_HEAD(, xlio_sock_packet)	received_packets;
	size_t			cur_iov_idx;
	size_t			cur_offset;
	struct xlio_sock_buf	*buffers;
	struct spdk_sock_buf	*free_buffers;

	TAILQ_ENTRY(spdk_xlio_sock)	link;
	TAILQ_ENTRY(spdk_xlio_sock)	link_send;
};

struct spdk_xlio_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
	TAILQ_HEAD(, spdk_xlio_sock)	pending_recv;
	TAILQ_HEAD(, spdk_xlio_sock)	pending_send;
	struct xlio_sock_packet	*packets;
	STAILQ_HEAD(, xlio_sock_packet)	free_packets;
	struct xlio_sock_buf	*buffers;
	struct spdk_sock_buf	*free_buffers;
};

static struct spdk_sock_impl_opts g_spdk_xlio_sock_impl_opts = {
	.recv_buf_size = MIN_SO_RCVBUF_SIZE,
	.send_buf_size = MIN_SO_SNDBUF_SIZE,
	.enable_recv_pipe = false,
	.enable_zerocopy_send = true,
	.enable_quickack = false,
	.enable_placement_id = false,
	.enable_zerocopy_send_server = true,
	.enable_zerocopy_send_client = true,
	.enable_zerocopy_recv = true,
	.zerocopy_threshold = 4096,
	.enable_tcp_nodelay = false,
	.buffers_pool_size = 1024,
	.packets_pool_size = 1024
};

static int _sock_flush_ext(struct spdk_sock *sock);
static struct xlio_api_t *g_xlio_api;
static void *g_xlio_handle;

static struct {
	int (*socket)(int domain, int type, int protocol);
	int (*bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	int (*listen)(int sockfd, int backlog);
	int (*connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	int (*accept)(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen);
	int (*close)(int fd);
	ssize_t (*readv)(int fd, const struct iovec *iov, int iovcnt);
	ssize_t (*writev)(int fd, const struct iovec *iov, int iovcnt);
	ssize_t (*recv)(int sockfd, void *buf, size_t len, int flags);
	ssize_t (*recvmsg)(int sockfd, struct msghdr *msg, int flags);
	ssize_t (*sendmsg)(int sockfd, const struct msghdr *msg, int flags);
	int (*epoll_create1)(int flags);
	int (*epoll_ctl)(int epfd, int op, int fd, struct epoll_event *event);
	int (*epoll_wait)(int epfd, struct epoll_event *events, int maxevents, int timeout);
	int (*fcntl)(int fd, int cmd, ... /* arg */);
	int (*ioctl)(int fd, unsigned long request, ...);
	int (*getsockopt)(int sockfd, int level, int optname, void *restrict optval, socklen_t *restrict optlen);
	int (*setsockopt)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
	int (*getsockname)(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen);
	int (*getpeername)(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen);
	int (*getaddrinfo)(const char *restrict node,
			   const char *restrict service,
			   const struct addrinfo *restrict hints,
			   struct addrinfo **restrict res);
	void (*freeaddrinfo)(struct addrinfo *res);
	const char *(*gai_strerror)(int errcode);
} g_xlio_ops;

static int
xlio_load(void)
{
	char *xlio_path;

	xlio_path = getenv("SPDK_XLIO_PATH");
	if (!xlio_path){
		printf("SPDK_XLIO_PATH is not defined. XLIO socket implementation is disabled.\n");
		return -1;
	} else if (strnlen(xlio_path, 1) == 0) {
		xlio_path = NULL;
		printf("SPDK_XLIO_PATH is defined but empty. Using default: %s\n",
		       DEFAULT_XLIO_PATH);
	}

	g_xlio_handle = dlopen(xlio_path ? xlio_path : DEFAULT_XLIO_PATH, RTLD_NOW);
	if (!g_xlio_handle) {
		SPDK_ERRLOG("Failed to load XLIO library: path %s, error %s\n",
			    xlio_path ? xlio_path : DEFAULT_XLIO_PATH, dlerror());
		return -1;
	}

#define GET_SYM(sym) \
	g_xlio_ops.sym = dlsym(g_xlio_handle, #sym); \
	if (!g_xlio_ops.sym) { \
		SPDK_ERRLOG("Failed to find symbol '%s'in XLIO library\n", #sym); \
		dlclose(g_xlio_handle); \
		g_xlio_handle = NULL; \
		return -1; \
	}

	GET_SYM(socket);
	GET_SYM(bind);
	GET_SYM(listen);
	GET_SYM(connect);
	GET_SYM(accept);
	GET_SYM(close);
	GET_SYM(readv);
	GET_SYM(writev);
	GET_SYM(recv);
	GET_SYM(recvmsg);
	GET_SYM(sendmsg);
	GET_SYM(epoll_create1);
	GET_SYM(epoll_ctl);
	GET_SYM(epoll_wait);
	GET_SYM(fcntl);
	GET_SYM(ioctl);
	GET_SYM(getsockopt);
	GET_SYM(setsockopt);
	GET_SYM(getsockname);
	GET_SYM(getpeername);
	GET_SYM(getaddrinfo);
	GET_SYM(freeaddrinfo);
	GET_SYM(gai_strerror);
#undef GET_SYM

	return 0;
}

static void
xlio_unload(void)
{
	int rc;

	if (g_xlio_handle) {
		memset(&g_xlio_ops, 0, sizeof(g_xlio_ops));
		rc = dlclose(g_xlio_handle);
		if (rc) {
			SPDK_ERRLOG("Closing libxlio failed: rc %d %s\n",
				    rc, dlerror());
		}

		SPDK_NOTICELOG("Unloaded libxlio\n");
		g_xlio_handle = NULL;
	}
}

static void *
spdk_xlio_alloc(size_t size)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
}

static void
spdk_xlio_free(void *buf) {
	/* For some reason XLIO destructor is not called from dlclose
	 * but is called later after DPDK cleanup. And this leads to a
	 * crash since all DPDK structures are already freed.
	 *
	 * The check below works this around. If XLIO didn't free the
	 * memory while in dlcose(), don't try to free it. DPDK will
	 * do or has already done the cleanup.
	 */
	if (g_xlio_handle) {
		spdk_free(buf);
	}
}

static struct xlio_api_t*
spdk_xlio_get_api(void)
{
	struct xlio_api_t *api_ptr = NULL;
	socklen_t len = sizeof(api_ptr);

	int err = g_xlio_ops.getsockopt(-2, SOL_SOCKET, SO_XLIO_GET_API, &api_ptr, &len);
	if (err < 0) {
		return NULL;
	}

	return api_ptr;
}

static int
xlio_init(void)
{
	int rc;
#pragma pack(push, 1)
	struct {
		uint8_t flags;
		void *(*alloc_func)(size_t);
		void (*free_func)(void *);
	} data;
#pragma pack(pop)
	struct cmsghdr *cmsg;
	char cbuf[CMSG_SPACE(sizeof(data))];

	static_assert((sizeof(uint8_t) + sizeof(uintptr_t) +
		       sizeof(uintptr_t)) == sizeof(data),
		      "wrong xlio ioctl data size.");

	/* Before init, g_xlio_api must be NULL */
	assert(g_xlio_api == NULL);

	g_xlio_api = spdk_xlio_get_api();
	if (!g_xlio_api) {
		SPDK_ERRLOG("Failed to get XLIO API\n");
		return -1;
	}
	SPDK_NOTICELOG("Got XLIO API %p\n", g_xlio_api);

	cmsg = (struct cmsghdr *)cbuf;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = CMSG_XLIO_IOCTL_USER_ALLOC;
	cmsg->cmsg_len = CMSG_LEN(sizeof(data));
	data.flags = IOCTL_USER_ALLOC_RX;
	data.alloc_func = spdk_xlio_alloc;
	data.free_func = spdk_xlio_free;
	memcpy(CMSG_DATA(cmsg), &data, sizeof(data));

	rc = g_xlio_api->ioctl(cmsg, cmsg->cmsg_len);
	if (rc < 0) {
		SPDK_ERRLOG("xlio_int rc %d (errno=%d)\n", rc, errno);
	}

	return rc;
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

#define __xlio_sock(sock) (struct spdk_xlio_sock *)sock
#define __xlio_group_impl(group) (struct spdk_xlio_sock_group_impl *)group

static int
xlio_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		 char *caddr, int clen, uint16_t *cport)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_xlio_ops.getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return -1;
	}

	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		break;
	default:
		/* Unsupported socket family */
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, saddr, slen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	if (sport) {
		if (sa.ss_family == AF_INET) {
			*sport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*sport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_xlio_ops.getpeername(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, caddr, clen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	if (cport) {
		if (sa.ss_family == AF_INET) {
			*cport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*cport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	return 0;
}

enum xlio_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
xlio_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, g_spdk_xlio_sock_impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = g_xlio_ops.setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static int
xlio_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (sz < MIN_SO_SNDBUF_SIZE) {
		sz = MIN_SO_SNDBUF_SIZE;
	}

	rc = g_xlio_ops.setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static inline struct ibv_pd *xlio_get_pd(int fd)
{
	struct xlio_pd_attr pd_attr_ptr = {};
	socklen_t len = sizeof(pd_attr_ptr);

	int err = g_xlio_ops.getsockopt(fd, SOL_SOCKET, SO_XLIO_PD, &pd_attr_ptr, &len);
	if (err < 0) {
		return NULL;
	}
	return pd_attr_ptr.ib_pd;
}

static struct spdk_xlio_sock *
xlio_sock_alloc(int fd, bool enable_zero_copy, enum xlio_sock_create_type type)
{
	struct spdk_xlio_sock *sock;
#if defined(SPDK_ZEROCOPY) || defined(__linux__)
	int flag;
	int rc;
#endif

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;

#if defined(SPDK_ZEROCOPY)
	flag = 1;

	if (enable_zero_copy) {

		/* Try to turn on zero copy sends */
		rc = g_xlio_ops.setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
		if (rc == 0) {
			sock->zcopy = true;
		} else {
			SPDK_WARNLOG("Zcopy send is not supported\n");
		}
	}

	if (type != SPDK_SOCK_CREATE_LISTEN) {
		sock->pd = xlio_get_pd(fd);
		if (!sock->pd) {
			SPDK_WARNLOG("Failed to get pd\n");
		}
	}

#endif

	if (g_spdk_xlio_sock_impl_opts.enable_zerocopy_recv) {
		int i;

		sock->recv_zcopy = true;

		sock->packets = calloc(RECV_ZCOPY_PACKETS_POOL_SIZE,
				       sizeof(struct xlio_sock_packet));
		if (!sock->packets) {
			SPDK_ERRLOG("Failed to allocated packets pool for socket %d\n", fd);
			goto err_free_sock;
		}

		STAILQ_INIT(&sock->free_packets);
		for (i = 0; i < RECV_ZCOPY_PACKETS_POOL_SIZE; ++i) {
			STAILQ_INSERT_TAIL(&sock->free_packets, &sock->packets[i], link);
		}

		STAILQ_INIT(&sock->received_packets);

		sock->buffers = calloc(RECV_ZCOPY_BUFFERS_POOL_SIZE,
				       sizeof(struct xlio_sock_buf));
		if (!sock->buffers) {
			SPDK_ERRLOG("Failed to allocated buffers pool for socket %d\n", fd);
			goto err_free_packets;
		}

		sock->free_buffers = &sock->buffers[0].sock_buf;
		for (i = 1; i < RECV_ZCOPY_BUFFERS_POOL_SIZE; ++i) {
			sock->buffers[i - 1].sock_buf.next = &sock->buffers[i].sock_buf;
		}
	}

#if defined(__linux__)
	flag = 1;

	if (g_spdk_xlio_sock_impl_opts.enable_quickack) {
		rc = g_xlio_ops.setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}
#endif

	return sock;

 err_free_packets:
	free(sock->packets);
 err_free_sock:
	free(sock);
	return NULL;
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
	rc = g_xlio_ops.getsockname(fd, (struct sockaddr *)&sa, &salen);
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
				g_xlio_ops.ioctl(fd, SIOCGIFFLAGS, &ifr);
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

static struct spdk_sock *
xlio_sock_create(const char *ip, int port,
		enum xlio_sock_create_type type,
		struct spdk_sock_opts *opts)
{
	struct spdk_xlio_sock *sock;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd, flag;
	int val = 1;
	int rc, sz;
	bool enable_zcopy_user_opts = true;
	bool enable_zcopy_impl_opts = true;

	assert(opts != NULL);

	if (ip == NULL) {
		return NULL;
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
	rc = g_xlio_ops.getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", g_xlio_ops.gai_strerror(rc), rc);
		return NULL;
	}

	/* try listen */
	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		fd = g_xlio_ops.socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0) {
			/* error */
			continue;
		}

		sz = g_spdk_xlio_sock_impl_opts.recv_buf_size;
		rc = g_xlio_ops.setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		sz = g_spdk_xlio_sock_impl_opts.send_buf_size;
		rc = g_xlio_ops.setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		rc = g_xlio_ops.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			g_xlio_ops.close(fd);
			/* error */
			continue;
		}

		if (g_spdk_xlio_sock_impl_opts.enable_tcp_nodelay) {
			rc = g_xlio_ops.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
			if (rc != 0) {
				g_xlio_ops.close(fd);
				/* error */
				continue;
			}
		}

#if defined(SO_PRIORITY)
		if (opts->priority) {
			rc = g_xlio_ops.setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
			if (rc != 0) {
				g_xlio_ops.close(fd);
				/* error */
				continue;
			}
		}
#endif

		if (res->ai_family == AF_INET6) {
			rc = g_xlio_ops.setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				g_xlio_ops.close(fd);
				/* error */
				continue;
			}
		}

		if (opts->ack_timeout) {
#if defined(__linux__)
			int to;

			to = opts->ack_timeout;
			rc = g_xlio_ops.setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to));
			if (rc != 0) {
				g_xlio_ops.close(fd);
				/* error */
				continue;
			}
#else
			SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
		}

		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = g_xlio_ops.bind(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed at port %d, errno = %d\n", port, errno);
				switch (errno) {
				case EINTR:
					/* interrupted? */
					g_xlio_ops.close(fd);
					goto retry;
				case EADDRNOTAVAIL:
					SPDK_ERRLOG("IP address %s not available. "
						    "Verify IP address in config file "
						    "and make sure setup script is "
						    "run before starting spdk app.\n", ip);
				/* FALLTHROUGH */
				default:
					/* try next family */
					g_xlio_ops.close(fd);
					fd = -1;
					continue;
				}
			}
			/* bind OK */
			rc = g_xlio_ops.listen(fd, 512);
			if (rc != 0) {
				SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
				g_xlio_ops.close(fd);
				fd = -1;
				break;
			}

			enable_zcopy_impl_opts = g_spdk_xlio_sock_impl_opts.enable_zerocopy_send_server;
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			rc = g_xlio_ops.connect(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
				/* try next family */
				g_xlio_ops.close(fd);
				fd = -1;
				continue;
			}

			enable_zcopy_impl_opts = g_spdk_xlio_sock_impl_opts.enable_zerocopy_send_client;
		}

		flag = g_xlio_ops.fcntl(fd, F_GETFL);
		if (g_xlio_ops.fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
			SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
			g_xlio_ops.close(fd);
			fd = -1;
			break;
		}
		break;
	}
	g_xlio_ops.freeaddrinfo(res0);

	if (fd < 0) {
		return NULL;
	}

	/* Only enable zero copy for non-loopback sockets. */
	enable_zcopy_user_opts = opts->zcopy && !sock_is_loopback(fd);

	sock = xlio_sock_alloc(fd, enable_zcopy_user_opts && enable_zcopy_impl_opts, type);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		g_xlio_ops.close(fd);
		return NULL;
	}

	if (opts != NULL) {
		sock->so_priority = opts->priority;
	}

	SPDK_NOTICELOG("Created xlio sock: send zcopy %d, recv zcopy %d, pd %p, context %p, dev %s, handle %u\n",
		       sock->zcopy, sock->recv_zcopy,
		       sock->pd,
		       sock->pd ? sock->pd->context : NULL,
		       sock->pd ? sock->pd->context->device->name : "unknown",
		       sock->pd ? sock->pd->handle : 0);
	return &sock->base;
}

static struct spdk_sock *
xlio_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return xlio_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts);
}

static struct spdk_sock *
xlio_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return xlio_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts);
}

static struct spdk_sock *
xlio_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock		*sock = __xlio_sock(_sock);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc, fd;
	struct spdk_xlio_sock		*new_sock;
	int				flag;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	rc = g_xlio_ops.accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	fd = rc;

	flag = g_xlio_ops.fcntl(fd, F_GETFL);
	if ((!(flag & O_NONBLOCK)) && (g_xlio_ops.fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
		g_xlio_ops.close(fd);
		return NULL;
	}

#if defined(SO_PRIORITY)
	/* The priority is not inherited, so call this function again */
	if (sock->base.opts.priority) {
		rc = g_xlio_ops.setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sock->base.opts.priority, sizeof(int));
		if (rc != 0) {
			g_xlio_ops.close(fd);
			return NULL;
		}
	}
#endif

	/* Inherit the zero copy feature from the listen socket */
	new_sock = xlio_sock_alloc(fd, sock->zcopy, SPDK_SOCK_CREATE_CONNECT);
	if (new_sock == NULL) {
		g_xlio_ops.close(fd);
		return NULL;
	}
	new_sock->so_priority = sock->base.opts.priority;

	return &new_sock->base;
}

static void xlio_sock_free_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet);

static int
xlio_sock_close(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

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

	assert(TAILQ_EMPTY(&_sock->pending_reqs));

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	g_xlio_ops.close(sock->fd);

	free(sock->packets);
	free(sock->buffers);
	free(sock);

	return 0;
}

#ifdef SPDK_ZEROCOPY
static int
_sock_check_zcopy(struct spdk_sock *sock)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(sock->group_impl);
	struct msghdr msgh = {};
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct spdk_sock_request *req, *treq;
	bool found;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	while (true) {
		rc = g_xlio_ops.recvmsg(vsock->fd, &msgh, MSG_ERRQUEUE);

		if (rc < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;
			}

			if (!TAILQ_EMPTY(&sock->pending_reqs)) {
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

			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) {
				if (!req->internal.is_zcopy) {
					/* This wasn't a zcopy request. It was just waiting in line to complete */
					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}
				} else if (req->internal.offset == idx) {
					found = true;

					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}

				} else if (found) {
					break;
				}
			}

			/* If we reaped buffer reclaim notification and sock is not in pending_recv list yet,
			 * add it now. It allows to call socket callback and process completions */
			if (found && !vsock->pending_recv && group) {
				vsock->pending_recv = true;
				TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
			}
		}
	}

	return 0;
}
#endif


static int
xlio_sock_flush(struct spdk_sock *sock)
{
#ifdef SPDK_ZEROCOPY
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);

	if (vsock->zcopy && !TAILQ_EMPTY(&sock->pending_reqs)) {
		_sock_check_zcopy(sock);
	}
#endif

	return _sock_flush_ext(sock);
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
dump_packet(struct xlio_sock_packet *packet)
{
	size_t i;

	for (i = 0; i < packet->xlio_packet->sz_iov; ++i) {
		SPDK_DEBUGLOG(xlio, "Packet %p: id %p, iov[%lu].len %lu\n",
			      packet, packet->xlio_packet_id, i,
			      packet->xlio_packet->iov[i].iov_len);
	}
}
#endif

static struct xlio_sock_packet *
xlio_sock_get_packet(struct spdk_xlio_sock *sock) {
	struct spdk_sock_group_impl *group = sock->base.group_impl;
	struct spdk_xlio_sock_group_impl *xlio_group = __xlio_group_impl(group);
	struct xlio_sock_packet *packet = NULL;

	if (spdk_likely(group && xlio_group->packets)) {
		packet = STAILQ_FIRST(&xlio_group->free_packets);
		STAILQ_REMOVE_HEAD(&xlio_group->free_packets, link);
	} else {
		packet = STAILQ_FIRST(&sock->free_packets);
		STAILQ_REMOVE_HEAD(&sock->free_packets, link);
	}

	/* @todo: handle lack of free packets */
	assert(packet);
	return packet;
}

static ssize_t
xlio_sock_recvfrom_zcopy(struct spdk_xlio_sock *sock)
{
	struct xlio_recvfrom_zcopy_packets_t *xlio_packets;
	struct xlio_recvfrom_zcopy_packet_t *xlio_packet;
	int flags = 0;
	int ret;
	size_t i;

	ret = g_xlio_api->recvfrom_zcopy(sock->fd, sock->xlio_packets_buf,
					sizeof(sock->xlio_packets_buf), &flags, NULL, NULL);
	if (ret <= 0) {
		if (spdk_unlikely(ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))) {
			SPDK_ERRLOG("recvfrom_zcopy failed, ret %d, errno %d\n", ret, errno);
		}

		return ret;
	}

	if (!(flags & MSG_XLIO_ZCOPY)) {
		SPDK_WARNLOG("Zcopy receive was not performed. Got %d bytes.\n", ret);
		return -1;
	}

	xlio_packets = (struct xlio_recvfrom_zcopy_packets_t *)sock->xlio_packets_buf;
	SPDK_DEBUGLOG(xlio, "Sock %d: got %lu packets, total %d bytes\n",
		      sock->fd, xlio_packets->n_packet_num, ret);

	/* Wrap all xlio packets and link to received packets list */
	xlio_packet = &xlio_packets->pkts[0];
	for (i = 0; i < xlio_packets->n_packet_num; ++i) {
		struct xlio_sock_packet *packet;
		size_t j, len = 0;

		/* @todo: Filter out zero length packets.
		 * Check with xlio team why it happens.
		 */
		for (j = 0; j < xlio_packet->sz_iov; ++j) {
			len += xlio_packet->iov[j].iov_len;
		}

		if (len == 0) {
			int rc;

			SPDK_DEBUGLOG(xlio, "Dropping zero length packet: id %p\n", xlio_packet->packet_id);
			rc = g_xlio_api->recvfrom_zcopy_free_packets(sock->fd, xlio_packet, 1);
			if (rc < 0) {
				SPDK_ERRLOG("Free XLIO packets failed, ret %d, errno %d\n",
					    rc, errno);
			}
			xlio_packet = next_packet(xlio_packet);
			continue;
		}

		packet = xlio_sock_get_packet(sock);
		/*
		 * @todo: XLIO packet pointer is only valid till next
		 * recvfrom_zcopy. We should be done with iovs by that
		 * time, but we need packet_id to free the packet
		 * later. Need to save it somewhere.It is not clear if
		 * iovs are required to free the packet.
		 */
		packet->xlio_packet = xlio_packet;
		packet->xlio_packet_id = xlio_packet->packet_id;
		/*
		 * While the packet is in received list there is data
		 * to read from it.  To avoid free of packets with
		 * unread data we intialize reference counter to 1.
		 */
		packet->refs = 1;
		STAILQ_INSERT_TAIL(&sock->received_packets, packet, link);
#ifdef DEBUG
		SPDK_DEBUGLOG(xlio, "Sock %d: packet %lu\n", sock->fd, i);
		dump_packet(packet);
#endif
		xlio_packet = next_packet(xlio_packet);
	}

	sock->cur_iov_idx = 0;
	sock->cur_offset = 0;

	return ret;
}

static void
xlio_sock_free_packet(struct spdk_xlio_sock *sock, struct xlio_sock_packet *packet) {
	int ret;
	struct xlio_recvfrom_zcopy_packet_t xlio_packet;
	struct spdk_sock_group_impl *group = sock->base.group_impl;
	struct spdk_xlio_sock_group_impl *xlio_group = __xlio_group_impl(group);

	SPDK_DEBUGLOG(xlio, "Sock %d: free xlio packet %p\n",
		      sock->fd, packet->xlio_packet->packet_id);
	assert(packet->refs == 0);
	/* @todo: How heavy is free_packets()? Maybe batch packets to free? */
	xlio_packet.packet_id = packet->xlio_packet_id;
	xlio_packet.sz_iov = 0;
	ret = g_xlio_api->recvfrom_zcopy_free_packets(sock->fd, &xlio_packet, 1);
	if (ret < 0) {
		SPDK_ERRLOG("Free xlio packets failed, ret %d, errno %d\n",
			    ret, errno);
	}

	if (spdk_likely(group && xlio_group->packets)) {
		STAILQ_INSERT_HEAD(&xlio_group->free_packets, packet, link);
	} else {
		STAILQ_INSERT_HEAD(&sock->free_packets, packet, link);
	}
}

static void
packets_advance(struct spdk_xlio_sock *sock, size_t len)
{
	SPDK_DEBUGLOG(xlio, "Sock %d: advance packets by %lu bytes\n", sock->fd, len);
	while (len > 0) {
		struct xlio_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);
		/* We don't allow to advance by more than we have data in packets */
		assert(cur_packet != NULL);
		struct iovec *iov = &cur_packet->xlio_packet->iov[sock->cur_iov_idx];
		int iov_len = iov->iov_len - sock->cur_offset;

		if ((int)len < iov_len) {
			sock->cur_offset += len;
			len = 0;
		} else {
			len -= iov_len;

			/* Next iov */
			sock->cur_offset = 0;
			sock->cur_iov_idx++;
			if (sock->cur_iov_idx >= cur_packet->xlio_packet->sz_iov) {
				/* Next packet */
				sock->cur_iov_idx = 0;
				STAILQ_REMOVE_HEAD(&sock->received_packets, link);
				if (--cur_packet->refs == 0) {
					xlio_sock_free_packet(sock, cur_packet);
				}
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

	while (cur_packet) {
		struct iovec *iov = &cur_packet->xlio_packet->iov[sock->cur_iov_idx];
		size_t len = iov->iov_len - sock->cur_offset;

		if (len == 0) {
			/* xlio may return zero length iov. Skip to next in this case */
			sock->cur_offset = 0;
			sock->cur_iov_idx++;
			assert(sock->cur_iov_idx <= cur_packet->xlio_packet->sz_iov);
			if (sock->cur_iov_idx >= cur_packet->xlio_packet->sz_iov) {
				/* Next packet */
				sock->cur_iov_idx = 0;
				cur_packet = STAILQ_NEXT(cur_packet, link);
			}
			continue;
		}

		assert(max_len > 0);
		assert(len > 0);
		len = spdk_min(len, max_len);
		*buf = iov->iov_base + sock->cur_offset;
		*packet = cur_packet;
		return len;
	}

	return 0;
}

static int
readv_wrapper(struct spdk_xlio_sock *sock, struct iovec *iovs, int iovcnt)
{
	int ret;

	if (sock->recv_zcopy) {
		int i;
		size_t offset = 0;

		if (STAILQ_EMPTY(&sock->received_packets)) {
			ret = xlio_sock_recvfrom_zcopy(sock);
			if (ret <= 0) {
				SPDK_DEBUGLOG(xlio, "Sock %d: readv_wrapper ret %d, errno %d\n",
					      sock->fd, ret, errno);
				return ret;
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
				SPDK_DEBUGLOG(xlio, "Sock %d: readv_wrapper ret %d\n", sock->fd, ret);
				return ret;
			}

			memcpy(iov->iov_base + offset, buf, len);
			packets_advance(sock, len);
			ret += len;
			offset += len;
			assert(offset <= iov->iov_len);
			if (offset == iov->iov_len) {
				offset = 0;
				i++;
			}
		}

		SPDK_DEBUGLOG(xlio, "Sock %d: readv_wrapper ret %d\n", sock->fd, ret);
	} else {
		ret = g_xlio_ops.readv(sock->fd, iovs, iovcnt);
	}

	return ret;
}

static ssize_t
xlio_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	return readv_wrapper(sock, iov, iovcnt);
}

static ssize_t
xlio_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return xlio_sock_readv(sock, iov, 1);
}

static ssize_t
xlio_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;

	/* In order to process a writev, we need to flush any asynchronous writes
	 * first. */
	rc = _sock_flush_ext(_sock);
	if (rc < 0) {
		return rc;
	}

	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {
		/* We weren't able to flush all requests */
		errno = EAGAIN;
		return -1;
	}

	return g_xlio_ops.writev(sock->fd, iov, iovcnt);
}

union _mkeys_container {
	char buf[CMSG_SPACE(sizeof(struct xlio_pd_key) * IOV_BATCH_SIZE)];
	struct cmsghdr align;
};

static inline size_t
xlio_sock_prep_reqs(struct spdk_sock *_sock, struct iovec *iovs, struct msghdr *msg,
		    union _mkeys_container *mkeys_container, uint32_t *total)
{
	size_t iovcnt = 0;
	int i;
	struct spdk_sock_request *req;
	unsigned int offset;
	struct cmsghdr *cmsg;
	struct xlio_pd_key *mkeys = NULL;
	struct spdk_xlio_sock *vsock = __xlio_sock(_sock);
	bool first_req_mkey;
	bool has_data = false;

	assert(total != NULL);
	*total = 0;

	req = TAILQ_FIRST(&_sock->queued_reqs);

	first_req_mkey = (req && req->mkeys)? true : false;

	while (req) {
		offset = req->internal.offset;

		if (first_req_mkey == !req->mkeys) {
			/* mkey setting or zcopy threshold is different with the first req */
			break;
		}

		for (i = 0; i < req->iovcnt; i++) {
			/* Consume any offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			if (vsock->zcopy && vsock->pd && req->mkeys) {
				if (!mkeys) {
					msg->msg_control = mkeys_container->buf;
					msg->msg_controllen = sizeof(mkeys_container->buf);
					cmsg = CMSG_FIRSTHDR(msg);

					cmsg->cmsg_len = CMSG_LEN(sizeof(struct xlio_pd_key) * IOV_BATCH_SIZE);
					cmsg->cmsg_level = SOL_SOCKET;
					cmsg->cmsg_type = SCM_XLIO_PD;

					mkeys = (struct xlio_pd_key *)CMSG_DATA(cmsg);
				}

				mkeys[iovcnt].mkey = req->mkeys[i];
				mkeys[iovcnt].flags = 0;
			}

			iovs[iovcnt].iov_base = SPDK_SOCK_REQUEST_IOV(req, i)->iov_base + offset;
			iovs[iovcnt].iov_len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;
			*total += (uint32_t)iovs[iovcnt].iov_len;
			iovcnt++;

			offset = 0;

			if (iovcnt >= IOV_BATCH_SIZE) {
				break;
			}
		}

		if (!has_data) {
			has_data = req->has_memory_domain_data;
		}
		if (iovcnt >= IOV_BATCH_SIZE) {
			break;
		}

		req = TAILQ_NEXT(req, internal.link);
	}

	if (mkeys && has_data) {
		msg->msg_controllen = CMSG_SPACE(sizeof(struct xlio_pd_key) * iovcnt);
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct xlio_pd_key) * iovcnt);
	} else {
		msg->msg_control = NULL;
		msg->msg_controllen = 0;
	}

	return iovcnt;
}

static int
_sock_flush_ext(struct spdk_sock *sock)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);
	struct msghdr msg = {};
	int flags;
	struct iovec iovs[IOV_BATCH_SIZE];
	size_t iovcnt;
	int retval;
	struct spdk_sock_request *req;
	int i;
	ssize_t rc;
	unsigned int offset;
	size_t len;
	union _mkeys_container mkeys_container;
	bool is_zcopy;
	uint32_t zerocopy_threshold;
	uint32_t total;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (sock->cb_cnt > 0) {
		return 0;
	}

	if (spdk_unlikely(TAILQ_EMPTY(&sock->queued_reqs))) {
		return 0;
	}

	iovcnt = xlio_sock_prep_reqs(sock, iovs, &msg, &mkeys_container, &total);
	if (spdk_unlikely(iovcnt == 0)) {
		return 0;
	}

	assert(!(!vsock->zcopy && msg.msg_controllen > 0));

	zerocopy_threshold = g_spdk_xlio_sock_impl_opts.zerocopy_threshold;

	/* Allow zcopy if enabled on socket and either the data needs to be sent,
	 * which is reported by xlio_sock_prep_reqs() with setting msg.msg_controllen
	 * or the msg size is bigger than the threshold configured. */
	if (vsock->zcopy && (msg.msg_controllen || total >= zerocopy_threshold)) {
		flags = MSG_ZEROCOPY;
	} else {
		flags = 0;
	}

	is_zcopy = (flags & MSG_ZEROCOPY);

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;

	rc = g_xlio_ops.sendmsg(vsock->fd, &msg, flags);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && vsock->zcopy)) {
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
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->internal.offset;

		/* req->internal.is_zcopy is true when the whole req or part of it is
		 * sent with zerocopy */
		req->internal.is_zcopy = is_zcopy;

		for (i = 0; i < req->iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;

			if (len > (size_t)rc) {
				/* This element was partially sent. */
				req->internal.offset += rc;
				return 0;
			}

			offset = 0;
			req->internal.offset += len;
			rc -= len;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);

		/* Ordering control. */
		if (!req->internal.is_zcopy && req == TAILQ_FIRST(&sock->pending_reqs)) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0);
			if (retval) {
				break;
			}
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			req->internal.offset = vsock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	return 0;
}


static void
xlio_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(sock->group_impl);
	int rc;

	spdk_sock_request_queue(sock, req);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {
		rc = _sock_flush_ext(sock);
		if (rc) {
			spdk_sock_abort_requests(sock);
		}

		if (TAILQ_EMPTY(&sock->queued_reqs) && vsock->pending_send && sock->group_impl) {
			TAILQ_REMOVE(&group->pending_send, vsock, link_send);
			vsock->pending_send = false;
		}
	} else if (!vsock->pending_send && sock->group_impl) {
		TAILQ_INSERT_TAIL(&group->pending_send, vsock, link_send);
		vsock->pending_send = true;
	}
}

static int
xlio_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int val;
	int rc;

	assert(sock != NULL);

	val = nbytes;
	rc = g_xlio_ops.setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0) {
		SPDK_DEBUGLOG(xlio, "Set SO_RECVLOWAT failed: rc %d\n", rc);
	}
	return 0;
}

static bool
xlio_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_xlio_ops.getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

static bool
xlio_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_xlio_ops.getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}

static bool
xlio_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	uint8_t byte;
	int rc;

	rc = g_xlio_ops.recv(sock->fd, &byte, 1, MSG_PEEK);
	if (rc == 0) {
		return false;
	}

	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return true;
		}

		return false;
	}

	return true;
}

static struct spdk_sock_group_impl *
xlio_sock_group_impl_create(void)
{
	struct spdk_xlio_sock_group_impl *group_impl;
	int fd;
	uint32_t num_packets = g_spdk_xlio_sock_impl_opts.packets_pool_size;
	uint32_t num_buffers = g_spdk_xlio_sock_impl_opts.buffers_pool_size;
	uint32_t i;

	fd = g_xlio_ops.epoll_create1(0);
	if (fd == -1) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		g_xlio_ops.close(fd);
		return NULL;
	}

	group_impl->fd = fd;
	TAILQ_INIT(&group_impl->pending_recv);
	TAILQ_INIT(&group_impl->pending_send);

	if (num_packets) {
		group_impl->packets = calloc(num_packets, sizeof(struct xlio_sock_packet));
		if (!group_impl->packets) {
			SPDK_ERRLOG("Failed to allocated packets pool for group %p\n", group_impl);
			goto fail;
		}

		STAILQ_INIT(&group_impl->free_packets);
		for (i = 0; i < num_packets; ++i) {
			STAILQ_INSERT_TAIL(&group_impl->free_packets, &group_impl->packets[i], link);
		}
	}

	if (num_buffers) {
		group_impl->buffers = calloc(num_buffers, sizeof(struct xlio_sock_buf));
		if (!group_impl->buffers) {
			SPDK_ERRLOG("Failed to allocated buffers pool for group %p\n", group_impl);
			goto fail;
		}

		group_impl->free_buffers = &group_impl->buffers[0].sock_buf;
		for (i = 1; i < num_buffers; ++i) {
			group_impl->buffers[i - 1].sock_buf.next = &group_impl->buffers[i].sock_buf;
		}
	}

	return &group_impl->base;

 fail:
	free(group_impl->buffers);
	free(group_impl->packets);
	free(group_impl);
	g_xlio_ops.close(fd);
	return NULL;
}

static int
xlio_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	/* EPOLLERR is always on even if we don't set it, but be explicit for clarity */
	event.events = EPOLLIN | EPOLLERR;
	event.data.ptr = sock;

	rc = g_xlio_ops.epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);

	return rc;
}

static int
xlio_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	int rc;
	struct epoll_event event;

	/* Event parameter is ignored but some old kernel version still require it. */
	rc = g_xlio_ops.epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
	spdk_sock_abort_requests(_sock);

	return rc;
}

static int
xlio_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			 struct spdk_sock **socks)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	struct spdk_sock *sock;
	int num_events, i, rc;
	struct spdk_xlio_sock *vsock, *ptmp;
	struct epoll_event events[MAX_EVENTS_PER_POLL];

	TAILQ_FOREACH_SAFE(vsock, &group->pending_send, link_send, ptmp) {
		rc = _sock_flush_ext(&vsock->base);
		if (rc) {
			spdk_sock_abort_requests(&vsock->base);
		}

		if (TAILQ_EMPTY(&vsock->base.queued_reqs)) {
			TAILQ_REMOVE(&group->pending_send, vsock, link_send);
			vsock->pending_send = false;
		}
	}

	num_events = g_xlio_ops.epoll_wait(group->fd, events, max_events, 0);
	if (num_events == -1) {
		return -1;
	} else if (num_events == 0 && !TAILQ_EMPTY(&_group->socks)) {
		uint8_t byte;

		sock = TAILQ_FIRST(&_group->socks);
		vsock = __xlio_sock(sock);
		/* a recv is done here to busy poll the queue associated with
		 * first socket in list and potentially reap incoming data.
		 */
		if (vsock->so_priority) {
			g_xlio_ops.recv(vsock->fd, &byte, 1, MSG_PEEK);
		}
	}

	for (i = 0; i < num_events; i++) {
		sock = events[i].data.ptr;
		vsock = __xlio_sock(sock);

#ifdef SPDK_ZEROCOPY
		if (events[i].events & EPOLLERR) {
			rc = _sock_check_zcopy(sock);
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || sock->cb_fn == NULL) {
				continue;
			}
		}
#endif
		if ((events[i].events & EPOLLIN) == 0) {
			continue;
		}

		/* If the socket does not already have recv pending, add it now */
		if (!vsock->pending_recv) {
			vsock->pending_recv = true;
			TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
		}
	}

	num_events = 0;

	TAILQ_FOREACH_SAFE(vsock, &group->pending_recv, link, ptmp) {
		if (num_events == max_events) {
			break;
		}

		/* If the socket's cb_fn is NULL, just remove it from the
		 * list and do not add it to socks array */
		if (spdk_unlikely(vsock->base.cb_fn == NULL)) {
			vsock->pending_recv = false;
			TAILQ_REMOVE(&group->pending_recv, vsock, link);
			continue;
		}

		socks[num_events++] = &vsock->base;
	}

	/* Cycle the pending_recv list so that each time we poll things aren't
	 * in the same order. */
	for (i = 0; i < num_events; i++) {
		vsock = __xlio_sock(socks[i]);

		TAILQ_REMOVE(&group->pending_recv, vsock, link);
		vsock->pending_recv = false;
	}

	return num_events;
}

static int
xlio_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_xlio_sock_group_impl *group = __xlio_group_impl(_group);
	int rc;

	rc = g_xlio_ops.close(group->fd);
	free(group->buffers);
	free(group->packets);
	free(group);
	return rc;
}

static int
xlio_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}
	memset(opts, 0, *len);

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= *len

#define GET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = g_spdk_xlio_sock_impl_opts.field; \
	}

	GET_FIELD(recv_buf_size);
	GET_FIELD(send_buf_size);
	GET_FIELD(enable_recv_pipe);
	GET_FIELD(enable_zerocopy_send);
	GET_FIELD(enable_quickack);
	GET_FIELD(enable_placement_id);
	GET_FIELD(enable_zerocopy_send_server);
	GET_FIELD(enable_zerocopy_send_client);
	GET_FIELD(enable_zerocopy_recv);
	GET_FIELD(zerocopy_threshold);
	GET_FIELD(enable_tcp_nodelay);
	GET_FIELD(buffers_pool_size);

#undef GET_FIELD
#undef FIELD_OK

	*len = spdk_min(*len, sizeof(g_spdk_xlio_sock_impl_opts));
	return 0;
}

static int
xlio_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		g_spdk_xlio_sock_impl_opts.field = opts->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_send_server);
	SET_FIELD(enable_zerocopy_send_client);
	SET_FIELD(enable_zerocopy_recv);
	SET_FIELD(zerocopy_threshold);
	SET_FIELD(enable_tcp_nodelay);
	SET_FIELD(buffers_pool_size);

#undef SET_FIELD
#undef FIELD_OK

	return 0;
}

static int
xlio_sock_get_caps(struct spdk_sock *sock, struct spdk_sock_caps *caps)
{
	struct spdk_xlio_sock *vsock = __xlio_sock(sock);

	caps->zcopy_send = vsock->zcopy;
	caps->ibv_pd = vsock->pd;
	caps->zcopy_recv = vsock->recv_zcopy;

	return 0;
}

static struct xlio_sock_buf *
xlio_sock_get_buf(struct spdk_xlio_sock *sock)
{
	struct spdk_sock_group_impl *group = sock->base.group_impl;
	struct spdk_xlio_sock_group_impl *xlio_group = __xlio_group_impl(group);
	struct spdk_sock_buf *sock_buf = NULL;

	if (spdk_likely(group && xlio_group->buffers)) {
		sock_buf = xlio_group->free_buffers;
		if (spdk_unlikely(!sock_buf)) {
			return NULL;
		}
		xlio_group->free_buffers = sock_buf->next;
	} else {
		sock_buf = sock->free_buffers;
		if (spdk_unlikely(!sock_buf)) {
			return NULL;
		}
		sock->free_buffers = sock_buf->next;
	}

	return SPDK_CONTAINEROF(sock_buf, struct xlio_sock_buf, sock_buf);
}

static void
xlio_sock_free_buf(struct spdk_xlio_sock *sock, struct xlio_sock_buf *buf)
{
	struct spdk_sock_group_impl *group = sock->base.group_impl;
	struct spdk_xlio_sock_group_impl *xlio_group = __xlio_group_impl(group);

	if (spdk_likely(group && xlio_group->buffers)) {
		buf->sock_buf.next = xlio_group->free_buffers;
		xlio_group->free_buffers = &buf->sock_buf;
	} else {
		buf->sock_buf.next = sock->free_buffers;
		sock->free_buffers = &buf->sock_buf;
	}
}

static ssize_t
xlio_sock_recv_zcopy(struct spdk_sock *_sock, size_t len, struct spdk_sock_buf **sock_buf)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);
	struct xlio_sock_buf *prev_buf = NULL;
	int ret;

	SPDK_DEBUGLOG(xlio, "Sock %d: zcopy recv %lu bytes\n", sock->fd, len);
	assert(sock->recv_zcopy);
	*sock_buf = NULL;

	if (STAILQ_EMPTY(&sock->received_packets)) {
		ret = xlio_sock_recvfrom_zcopy(sock);
		if (ret <= 0) {
			SPDK_DEBUGLOG(xlio, "Sock %d: recv_zcopy ret %d, errno %d\n",
				      sock->fd, ret, errno);
			return ret;
		}
	}

	assert(!STAILQ_EMPTY(&sock->received_packets));
	ret = 0;
	while (len > 0) {
		void *data;
		size_t chunk_len;
		struct xlio_sock_buf *buf;
		struct xlio_sock_packet *packet;

		chunk_len = packets_next_chunk(sock, &data, &packet, len);
		if (chunk_len == 0) {
			/* No more data */
			break;
		}

		assert(chunk_len <= len);
		buf = xlio_sock_get_buf(sock);
		if (spdk_unlikely(!buf)) {
			SPDK_DEBUGLOG(xlio, "Sock %d: no more buffers, total_len %d\n", sock->fd, ret);
			if (ret == 0) {
				if (!sock->pending_recv) {
					struct spdk_xlio_sock_group_impl *group =
						__xlio_group_impl(sock->base.group_impl);
					sock->pending_recv = true;
					TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
				}
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

		packets_advance(sock, chunk_len);
		len -= chunk_len;
		ret += chunk_len;
		prev_buf = buf;
		SPDK_DEBUGLOG(xlio, "Sock %d: add buffer %p, len %lu, total_len %d\n",
			      sock->fd, buf, buf->sock_buf.iov.iov_len, ret);
	}

	SPDK_DEBUGLOG(xlio, "Sock %d: recv_zcopy ret %d\n", sock->fd, ret);
	return ret;
}

static int
xlio_sock_free_bufs(struct spdk_sock *_sock, struct spdk_sock_buf *sock_buf)
{
	struct spdk_xlio_sock *sock = __xlio_sock(_sock);

	while (sock_buf) {
		struct xlio_sock_buf *buf = SPDK_CONTAINEROF(sock_buf,
							    struct xlio_sock_buf,
							    sock_buf);
		struct xlio_sock_packet *packet = buf->packet;
		struct spdk_sock_buf *next = buf->sock_buf.next;

		xlio_sock_free_buf(sock, buf);
		if (--packet->refs == 0) {
			xlio_sock_free_packet(sock, packet);
		}

		sock_buf = next;
	}

	return 0;
}

static struct spdk_sock_group_impl *
xlio_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	return NULL;
}

static struct spdk_net_impl g_xlio_net_impl = {
	.name		= "xlio",
	.getaddr	= xlio_sock_getaddr,
	.connect	= xlio_sock_connect,
	.listen		= xlio_sock_listen,
	.accept		= xlio_sock_accept,
	.close		= xlio_sock_close,
	.recv		= xlio_sock_recv,
	.readv		= xlio_sock_readv,
	.writev		= xlio_sock_writev,
	.writev_async	= xlio_sock_writev_async,
	.flush		= xlio_sock_flush,
	.set_recvlowat	= xlio_sock_set_recvlowat,
	.set_recvbuf	= xlio_sock_set_recvbuf,
	.set_sendbuf	= xlio_sock_set_sendbuf,
	.is_ipv6	= xlio_sock_is_ipv6,
	.is_ipv4	= xlio_sock_is_ipv4,
	.is_connected	= xlio_sock_is_connected,
	.group_impl_get_optimal	= xlio_sock_group_impl_get_optimal,
	.group_impl_create	= xlio_sock_group_impl_create,
	.group_impl_add_sock	= xlio_sock_group_impl_add_sock,
	.group_impl_remove_sock = xlio_sock_group_impl_remove_sock,
	.group_impl_poll	= xlio_sock_group_impl_poll,
	.group_impl_close	= xlio_sock_group_impl_close,
	.get_opts	= xlio_sock_impl_get_opts,
	.set_opts	= xlio_sock_impl_set_opts,
	.get_caps	= xlio_sock_get_caps,
	.recv_zcopy	= xlio_sock_recv_zcopy,
	.free_bufs	= xlio_sock_free_bufs
};

static void __attribute__((constructor))
spdk_net_impl_register_xlio(void)
{
	if (xlio_load() == 0 &&
	    xlio_init() == 0) {
		spdk_net_impl_register(&g_xlio_net_impl, DEFAULT_SOCK_PRIORITY - 1);
	}
}

static void __attribute__((destructor))
spdk_net_impl_unregister_xlio(void)
{
	xlio_unload();
}

SPDK_LOG_REGISTER_COMPONENT(xlio)
