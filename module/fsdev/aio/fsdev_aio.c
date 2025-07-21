/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/config.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/lut.h"

#include "fsdev_aio.h"
#include <libaio.h>
#ifdef SPDK_CONFIG_URING
#include <liburing.h>
#endif
#include <sys/ioctl.h>
#include <linux/fs.h>

#define SPDK_URING_QUEUE_DEPTH 512

#define FILE_PTR_LUT_INIT_SIZE 1000
#define FILE_PTR_LUT_BITS 63
#define FILE_PTR_LUT_BASE (((uint64_t)1) << FILE_PTR_LUT_BITS)
#define FILE_PTR_LUT_MAX_SIZE (1024 * 1024 * 1024) /* 1 billion files maximum */
#define FILE_PTR_LUT_GROWTH_STEP 1000

#define IO_STATUS_ASYNC INT_MIN

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/* See https://libfuse.github.io/doxygen/structfuse__conn__info.html */
#define MAX_BACKGROUND (100)
#define TIME_GRAN (1)
#define DEFAULT_WRITEBACK_CACHE false
#define DEFAULT_MAX_XFER_SIZE 0x00020000
#define DEFAULT_MAX_READAHEAD 0x00020000
#define DEFAULT_XATTR_ENABLED false
#define DEFAULT_SKIP_RW false
#define DEFAULT_ATTR_VALID_MS 0 /* to prevent the attribute caching */
#define DEFAULT_NOTIFY_MAX_DATA_SIZE 4096
#define DEFAULT_ENABLE_NOTIFICATIONS false
#define FANOTIFY_POLLER_PERIOD_US 1000
#ifdef SPDK_CONFIG_COPY_FILE_RANGE
#define DEFAULT_DISABLE_COPY_FILE_RANGE false
#else
#define DEFAULT_DISABLE_COPY_FILE_RANGE true
#endif

#ifdef SPDK_CONFIG_HAVE_STRUCT_STAT_ST_ATIM
/* Linux */
#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atim.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctim.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtim.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atim.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctim.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtim.tv_nsec = (val)
#elif defined(SPDK_CONFIG_HAVE_STRUCT_STAT_ST_ATIMESPEC)
/* FreeBSD */
#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atimespec.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctimespec.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtimespec.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atimespec.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctimespec.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtimespec.tv_nsec = (val)
#else
#define ST_ATIM_NSEC(stbuf) 0
#define ST_CTIM_NSEC(stbuf) 0
#define ST_MTIM_NSEC(stbuf) 0
#define ST_ATIM_NSEC_SET(stbuf, val) do { } while (0)
#define ST_CTIM_NSEC_SET(stbuf, val) do { } while (0)
#define ST_MTIM_NSEC_SET(stbuf, val) do { } while (0)
#endif

#define FANOTIFY_MASK (FAN_ATTRIB | FAN_ONDIR | FAN_EVENT_ON_CHILD)

/*
 * Example of "unrestricted" variant of the data that can be get or
 * set by AIO ioctl() implementation.
 *
 * The "buf" and "size" should be handled in a special way with using
 * SPDK_FSDEVB_IOCTL_RETRY protocol.
 */
struct aio_ioctl_unrest {
	char *buf;
	uint32_t size;
};

/*
 * Example of restricted (traditional) variant of the data that can be
 * get or set by AIO ioctl() implementation, when the structure size
 * is well known in advance.
 */
struct aio_ioctl_rest {
	uint32_t width;
	uint32_t height;
};

/*
 * Reading data, output buffer must be poulated by internal module data.
 * The input is zero. This command may request RETRY.
 *
 * The meaning of values:
 * - 'E' - means example.
 * - 42  - cmd number.
 * - data type for the output data (root structure).
 */
#define AIO_IOCTL_GET_UNREST_DATA_CMD _IOR('E', 42, struct aio_ioctl_unrest)

/*
 * Setting data, input buffer must be used for poulating internal module data.
 * The output is zero. This command may request RETRY.
 *
 * The meaning of values:
 * - 'E' - means example.
 * - 43  - cmd number.
 * - data type for the output data (root structure).
 */
#define AIO_IOCTL_SET_UNREST_DATA_CMD _IOW('E', 43, struct aio_ioctl_unrest)

/*
 * Same as AIO_IOCTL_GET_UNREST_DATA_CMD for restricted ioctl() variant.
 * No RETRY is allowed.
 */
#define AIO_IOCTL_GET_REST_DATA_CMD _IOR('E', 44, struct aio_ioctl_rest)

/*
 * Same as AIO_IOCTL_SET_UNREST_DATA_CMD for restricted ioctl() variant.
 * No RETRY is allowed.
 */
#define AIO_IOCTL_SET_REST_DATA_CMD _IOW('E', 45, struct aio_ioctl_rest)

/*
 * Setting and getting data in one blow. The input buffer must be used for poulating
 * internal module data. The output - for returning the old value (before changing).
 *
 * The meaning of values:
 * - 'E' - means example.
 * - 46  - cmd number.
 * - size of the output data (root structure) is sizeof(struct aio_ioctl_unrest)
 */
#define AIO_IOCTL_REST_DATA_CMD _IOWR('E', 46, struct aio_ioctl_rest)

/*
 * No data exchange (action) command. Input and output buffers are zero.
 */
#define AIO_IOCTL_ACT_CMD _IO('E', 47)

static struct fsdev_aio_module_opts g_opts = {
	.enable_io_uring = false,
	.max_io_depth = 256,
};

#ifdef SPDK_CONFIG_URING
static int g_io_uring_supported_ops[IORING_OP_LAST] = {0};

static void
fsdev_init_supported_uring_ops(void)
{
	size_t i = 0;
	struct io_uring_probe *probe;

	probe = io_uring_get_probe();
	if (!probe) {
		SPDK_WARNLOG("Failed to get io_uring probe\n");
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(g_io_uring_supported_ops); i++) {
		g_io_uring_supported_ops[i] = io_uring_opcode_supported(probe, i);
	}

	assert(g_io_uring_supported_ops[IORING_OP_WRITEV] == g_io_uring_supported_ops[IORING_OP_READV]);
}

static inline bool
aio_fsdev_use_io_uring_rdwr(void)
{
	return g_opts.enable_io_uring &&
	       g_io_uring_supported_ops[IORING_OP_READV] && g_io_uring_supported_ops[IORING_OP_WRITEV];
}
#else
#define aio_fsdev_use_io_uring_rdwr() false
#endif

struct fsdev_aio_cred {
	uid_t euid;
	gid_t egid;
};

/** Inode number type */
typedef uint64_t spdk_ino_t;

struct fsdev_aio_key {
	ino_t ino;
	dev_t dev;
	RB_ENTRY(fsdev_aio_key) link;
};

struct aio_fsdev_fhdr {
	uint64_t is_fobject : 1;
	uint64_t lut_key : 63;
	uint64_t refcount;
};

/* The sizeof(struct aio_fsdev_fhdr) a multiple of sizeof(uint64_t) */
SPDK_STATIC_ASSERT(sizeof(struct aio_fsdev_fhdr) == 16, "Incorrect size");

struct aio_fsdev_file_handle {
	struct aio_fsdev_fhdr hdr;
	int fd;
	struct {
		DIR *dp;
		struct dirent *entry;
		off_t offset;
	} dir;
	struct aio_fsdev *vfsdev;
	uint64_t fobject_lut_key;
	TAILQ_ENTRY(aio_fsdev_file_handle) link;
};

struct aio_fsdev;

struct aio_fsdev_linux_fh {
	const struct file_handle *fh;
	RB_ENTRY(aio_fsdev_linux_fh) node;
};

static int
aio_fsdev_linux_fh_cmp(struct aio_fsdev_linux_fh *fh1, struct aio_fsdev_linux_fh *fh2)
{
	return memcmp(fh1->fh, fh2->fh, sizeof(*fh1->fh) + fh1->fh->handle_bytes);
}

RB_HEAD(aio_fsdev_linux_fh_tree, aio_fsdev_linux_fh);
RB_GENERATE_STATIC(aio_fsdev_linux_fh_tree, aio_fsdev_linux_fh, node, aio_fsdev_linux_fh_cmp);

#define FOBJECT_FMT "fobj=%p (lut=0x%" PRIx64 " ino=%" PRIu64 " dev=%" PRIu64 ")"
#define FOBJECT_ARGS(fo) (fo), ((uint64_t)(fo)->hdr.lut_key), ((uint64_t)(fo)->key.ino), ((uint64_t)(fo)->key.dev)
struct aio_fsdev_file_object {
	struct aio_fsdev_fhdr hdr;
	mode_t mode;
	int fd;
	char *fd_str;
	struct fsdev_aio_key key;
	union {
		struct file_handle linux_fh;
		char fh_buf[sizeof(struct file_handle) + MAX_HANDLE_SZ];
	};
	struct aio_fsdev_linux_fh linux_fh_entry;
	struct aio_fsdev_file_object *parent_fobject;
	RB_HEAD(aio_fsdev_file_object_tree, fsdev_aio_key) leafs;
	TAILQ_HEAD(, aio_fsdev_file_handle) handles;
	struct aio_fsdev *vfsdev;
};

static inline bool
fsdev_aio_fobject_is_symlink(struct aio_fsdev_file_object *fobject)
{
	return S_ISLNK(fobject->mode);
}

static inline bool
fsdev_aio_fobject_is_dir(struct aio_fsdev_file_object *fobject)
{
	return S_ISDIR(fobject->mode);
}

static int
aio_fsdev_file_object_cmp(struct fsdev_aio_key *fo1, struct fsdev_aio_key *fo2)
{
	if (fo1->dev != fo2->dev) {
		assert(false);
		return -1;
	}

	if (fo1->ino < fo2->ino) {
		return -1;
	} else if (fo1->ino > fo2->ino) {
		return 1;
	} else {
		return 0;
	}
}

RB_GENERATE_STATIC(aio_fsdev_file_object_tree, fsdev_aio_key, link, aio_fsdev_file_object_cmp);

struct aio_fsdev {
	struct spdk_fsdev fsdev;
	struct spdk_fsdev_mount_opts mount_opts;
	struct spdk_fsdev_aio_opts opts;
	char *root_path;
	int proc_self_fd;
	struct aio_fsdev_file_object *root;
	TAILQ_ENTRY(aio_fsdev) tailq;
	struct spdk_lut *lut;
	struct spdk_spinlock lock;
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	int fanotify_fd;
	struct spdk_poller *fanotify_poller;
	pid_t pid;
	struct aio_fsdev_linux_fh_tree linux_fhs;
#endif
};

struct aio_fsdev_io;

struct aio_fsdev_io {
	struct iocb io;
	uint32_t data_size;
	int status;
	TAILQ_ENTRY(aio_fsdev_io) link;
};

struct aio_io_channel {
	struct spdk_poller *poller;
	TAILQ_HEAD(, aio_fsdev_io) ios_for_submit;
	TAILQ_HEAD(, aio_fsdev_io) ios_in_progress;
	TAILQ_HEAD(, aio_fsdev_io) ios_to_complete;
	union {
		io_context_t io_ctx;
#ifdef SPDK_CONFIG_URING
		struct {
			/* NOTE: once get_sqe fails for the 1st time, we must queue all the following IOs to
			 * preserve the order until the queue is empty
			 */
			uint32_t queue_ios: 1;
			uint32_t reserved: 31;
			uint32_t io_count;
			struct io_uring io_ring;
		} uring;
#endif
	} u;
};

static TAILQ_HEAD(, aio_fsdev) g_aio_fsdev_head = TAILQ_HEAD_INITIALIZER(
			g_aio_fsdev_head);


static inline struct aio_fsdev *
fsdev_to_aio_fsdev(struct spdk_fsdev *fsdev)
{
	return SPDK_CONTAINEROF(fsdev, struct aio_fsdev, fsdev);
}

static inline struct spdk_fsdev_io *
aio_to_fsdev_io(const struct aio_fsdev_io *aio_io)
{
	return SPDK_CONTAINEROF(aio_io, struct spdk_fsdev_io, driver_ctx);
}

static inline struct aio_fsdev_io *
fsdev_to_aio_io(const struct spdk_fsdev_io *fsdev_io)
{
	return (struct aio_fsdev_io *)fsdev_io->driver_ctx;
}

static int clear_suid_sgid(struct aio_fsdev_io *vfsdev_io);

static void
fsdev_aio_cb(struct aio_fsdev_io *aio, long res, long res2)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(aio);
	struct spdk_io_channel *ioch = spdk_fsdev_io_get_io_channel(fsdev_io);
	struct aio_io_channel *aioch = spdk_io_channel_get_ctx(ioch);

	TAILQ_REMOVE(&aioch->ios_in_progress, aio, link);

	if (res >= 0) {
		switch (spdk_fsdev_io_get_type(fsdev_io)) {
		case SPDK_FSDEV_IO_READ:
			fsdev_io->u_out.read.data_size = res;
			break;
		case SPDK_FSDEV_IO_WRITE:
			fsdev_io->u_out.write.data_size = res;

			/* Even though we don't report support for handling this flag, the kernel
			 * will send us on files opened with O_DIRECT, so we still handle it.
			 */
			if (fsdev_io->u_in.write.flags & SPDK_FSDEV_WRITE_KILL_SUIDGID) {
				/* We do not check the return value because
				* failure is not fatal. */
				clear_suid_sgid(aio);
			}
			break;
		default:
			break;
		}
		aio->status = 0;
	} else {
		SPDK_ERRLOG("aio operation failed: %ld\n", res);
		aio->status = res;
	}

	TAILQ_INSERT_TAIL(&aioch->ios_to_complete, aio, link);
}

static inline struct aio_fsdev_file_object *
fsdev_aio_get_fobject(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_object *_fobject)
{
	uint64_t n = (uint64_t)(uintptr_t)_fobject;
	struct aio_fsdev_file_object *fobject;

	if (n < FILE_PTR_LUT_BASE) {
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fobject (< 0x%" PRIx64 ")\n", n, FILE_PTR_LUT_BASE);
		return NULL;
	}

	spdk_spin_lock(&vfsdev->lock);
	fobject = spdk_lut_get(vfsdev->lut, n - FILE_PTR_LUT_BASE);
	if (fobject == SPDK_LUT_INVALID_VALUE) {
		spdk_spin_unlock(&vfsdev->lock);
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fobject\n", n);
		return NULL;
	}

	assert(fobject); /* There shouldn't be NULL fobject in the LUT */

	if (spdk_likely(fobject->hdr.is_fobject)) {
		__atomic_add_fetch(&fobject->hdr.refcount, 1, __ATOMIC_RELAXED); /* ref by caller */
	} else {
		/* Error: the key rather belongs to a fhandle */
		SPDK_WARNLOG("0x%" PRIx64 " is not a fobject\n", n);
		fobject = NULL;
	}
	spdk_spin_unlock(&vfsdev->lock);

	return fobject;
}

static inline struct spdk_fsdev_file_object *
fsdev_aio_get_spdk_fobject(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject)
{
	assert(fobject);

	return (struct spdk_fsdev_file_object *)(uintptr_t)(fobject->hdr.lut_key + FILE_PTR_LUT_BASE);
}

/* The returned fhandle pointer is only valid while the stack frame this pointer is returned to
 * remains valid. You cannot store tihs pointer to be used later. Instead, look it back up again. */
static inline struct aio_fsdev_file_handle *
fsdev_aio_get_fhandle(struct aio_fsdev *vfsdev, struct spdk_fsdev_file_handle *_fhandle)
{
	uint64_t n = (uint64_t)(uintptr_t)_fhandle;
	struct aio_fsdev_file_handle *fhandle;

	if (n < FILE_PTR_LUT_BASE) {
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fhandle (< 0x%" PRIx64 ")\n", n, FILE_PTR_LUT_BASE);
		return NULL;
	}

	fhandle = spdk_lut_get(vfsdev->lut, n - FILE_PTR_LUT_BASE);
	if (fhandle == SPDK_LUT_INVALID_VALUE) {
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fhandle\n", n);
		return NULL;
	}

	assert(fhandle); /* There shouldn't be NULL fhandle in the LUT */

	if (!fhandle->hdr.is_fobject) {
		/* fhandles either have a refcount of 0 or 1. They do not take extra references. */
		assert(fhandle->hdr.refcount <= 1);
	} else {
		/* Error: the key rather belongs to a fobject */
		SPDK_WARNLOG("0x%" PRIx64 " is not a fhandle\n", n);
		fhandle = NULL;
	}

	return fhandle;
}

static inline struct spdk_fsdev_file_handle *
fsdev_aio_get_spdk_fhandle(struct aio_fsdev *vfsdev, struct aio_fsdev_file_handle *fhandle)
{
	return (struct spdk_fsdev_file_handle *)(uintptr_t)(fhandle->hdr.lut_key + FILE_PTR_LUT_BASE);
}

static int
is_dot_or_dotdot(const char *name)
{
	return name[0] == '.' && (name[1] == '\0' ||
				  (name[1] == '.' && name[2] == '\0'));
}

/* Is `path` a single path component that is not "." or ".."? */
static int
is_safe_path_component(const char *path)
{
	if (strchr(path, '/')) {
		return 0;
	}

	return !is_dot_or_dotdot(path);
}

static struct aio_fsdev_file_object *
find_leaf_unsafe(struct aio_fsdev_file_object *fobject, ino_t ino, dev_t dev)
{
	struct fsdev_aio_key tmp, *key;

	tmp.ino = ino;
	tmp.dev = dev;
	key = RB_FIND(aio_fsdev_file_object_tree, &fobject->leafs, &tmp);

	if (key != NULL) {
		return SPDK_CONTAINEROF(key, struct aio_fsdev_file_object, key);
	}

	return NULL;
}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
static int
fsdev_aio_fanotify_add(struct aio_fsdev_file_object *fobject, int parent_fd, const char *name)
{
	struct aio_fsdev *vfsdev = fobject->vfsdev;
	int mount_id;
	int rc;

	fobject->linux_fh.handle_bytes = MAX_HANDLE_SZ;
	rc = name_to_handle_at(parent_fd, name, &fobject->linux_fh, &mount_id, 0);
	if (rc) {
		SPDK_ERRLOG("Failed to get file handle: errno %d, parent fd %d, name %s\n",
			    errno, parent_fd, name);
		return rc;
	}

	rc = fanotify_mark(vfsdev->fanotify_fd, FAN_MARK_ADD | FAN_MARK_ONLYDIR, FANOTIFY_MASK,
			   parent_fd, name);
	if (rc) {
		SPDK_ERRLOG("Failed to add fobject to fanotify: errno %d, fd %d, "
			    "parent fd %d, name %s\n",
			    errno, fobject->fd, parent_fd, name);
		return rc;
	}

	fobject->linux_fh_entry.fh = &fobject->linux_fh;
	RB_INSERT(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, &fobject->linux_fh_entry);

	SPDK_DEBUGLOG(fsdev_aio, "Added fobject to fanotify: fd %d, name %s\n",
		      fobject->fd, name);

	return 0;
}

static void
fsdev_aio_fanotify_remove(struct aio_fsdev_file_object *fobject)
{
	struct aio_fsdev *vfsdev = fobject->vfsdev;
	const char *name;
	int fd;
	int rc;

	if (fobject == vfsdev->root) {
		fd = AT_FDCWD;
		name = vfsdev->root_path;
	} else {
		fd = fobject->fd;
		name = ".";
	}

	rc = fanotify_mark(vfsdev->fanotify_fd, FAN_MARK_REMOVE | FAN_MARK_ONLYDIR, FANOTIFY_MASK,
			   fd, name);
	if (rc) {
		SPDK_ERRLOG("Failed to remove fobject from fanotify: errno %d, fd %d, name %s\n",
			    errno, fd, name);
	} else {
		SPDK_DEBUGLOG(fsdev_aio, "Removed fobject from fanotify: fd %d, name %s\n", fd, name);
	}

	spdk_spin_lock(&vfsdev->lock);
	RB_REMOVE(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, &fobject->linux_fh_entry);
	spdk_spin_unlock(&vfsdev->lock);
}
#endif

static void
file_object_destroy(struct aio_fsdev_file_object *fobject)
{
	assert(!fobject->hdr.refcount);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	/* root is handled on umount */
	if (fobject->vfsdev->fanotify_fd != -1 &&
	    fsdev_aio_fobject_is_dir(fobject) && fobject->parent_fobject) {
		fsdev_aio_fanotify_remove(fobject);
	}
#endif

	close(fobject->fd);
	free(fobject->fd_str);
	free(fobject);
}

/* This function returns the result reference counter */
static uint64_t
file_object_unref(struct aio_fsdev_file_object *fobject, uint32_t count)
{
	struct aio_fsdev *vfsdev = fobject->vfsdev;
	struct aio_fsdev_file_object *parent_fobject = fobject->parent_fobject;
	uint64_t refcount;

	assert(fobject->hdr.refcount >= count);

	/* IMPORTANT NOTE:
	 * We want keep this function as lightweight and lockless as possible, so we decrease the reference counter
	 * before we take the lock and destroy the object. This is fine in the wast majority of cases, as usually
	 * file operations are performed on a fobject while it's being referenced by the app.
	 * However, there's a race here in cases when the last reference is being removed. The fobject can be
	 * obtained by fsdev_aio_do_lookup after the reference counter has been decreased and checked and before we take
	 * the lock to remove the fobject from its parent's leafs list.
	 * Thus we have to check the value of the reference counter once again to avoid deleting the fobject while
	 * it's in use.
	 */
	refcount = __atomic_sub_fetch(&fobject->hdr.refcount, count, __ATOMIC_RELAXED);
	if (refcount) {
		SPDK_DEBUGLOG(fsdev_aio, "%p urefed (cnt=%" PRIu32 " refcnt=%" PRIu64 ")\n",
			      fobject, count, refcount);
		return refcount;
	}

	if (spdk_unlikely(!parent_fobject)) {
		assert(fobject == fobject->vfsdev->root);

		spdk_spin_lock(&vfsdev->lock);
		refcount = __atomic_load_n(&fobject->hdr.refcount, __ATOMIC_RELAXED);
		if (!refcount) {
			spdk_lut_remove(fobject->vfsdev->lut, fobject->hdr.lut_key);
		}
		spdk_spin_unlock(&vfsdev->lock);

		if (!refcount) {
			SPDK_DEBUGLOG(fsdev_aio, "root fobject removed %p\n", fobject);
			file_object_destroy(fobject);
		}
		return 0;
	}

	spdk_spin_lock(&vfsdev->lock);

	refcount = __atomic_load_n(&fobject->hdr.refcount, __ATOMIC_RELAXED);
	if (!refcount) {
		spdk_lut_remove(fobject->vfsdev->lut, fobject->hdr.lut_key);
		RB_REMOVE(aio_fsdev_file_object_tree, &parent_fobject->leafs, &fobject->key);
	}

	spdk_spin_unlock(&vfsdev->lock);

	if (refcount) {
		return refcount;
	}

	SPDK_DEBUGLOG(fsdev_aio, "%p finally urefed (cnt=%" PRIu32 ")\n",
		      fobject, count);

	file_object_destroy(fobject);

	file_object_unref(parent_fobject, 1); /* unref by the leaf */

	return 0;
}

static inline void
file_object_ref(struct aio_fsdev_file_object *fobject)
{
	/* The fobject is referenced by a caller, so it's' safe just increase the ref count */
	__atomic_add_fetch(&fobject->hdr.refcount, 1, __ATOMIC_RELAXED);
}

static struct aio_fsdev_file_object *
file_object_create_unsafe(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *parent_fobject,
			  int fd, ino_t ino, dev_t dev, mode_t mode, const char *name)
{
	struct aio_fsdev_file_object *fobject;
	uint64_t lut_key = SPDK_LUT_INVALID_KEY;

	fobject = calloc(1, sizeof(*fobject));
	if (!fobject) {
		SPDK_ERRLOG("Cannot alloc fobject\n");
		return NULL;
	}

	fobject->fd_str = spdk_sprintf_alloc("%d", fd);
	if (!fobject->fd_str) {
		SPDK_ERRLOG("Cannot alloc fd_str\n");
		goto err;
	}

	lut_key = spdk_lut_insert(vfsdev->lut, fobject);
	if (lut_key == SPDK_LUT_INVALID_KEY) {
		SPDK_ERRLOG("Cannot insert fobject into lookup table\n");
		goto err;
	}

	fobject->hdr.is_fobject = true;
	fobject->hdr.lut_key = lut_key;
	fobject->hdr.refcount = 1; /* ref by caller */

	fobject->fd = fd;
	fobject->key.ino = ino;
	fobject->key.dev = dev;
	fobject->vfsdev = vfsdev;
	fobject->mode = mode;

	TAILQ_INIT(&fobject->handles);
	RB_INIT(&fobject->leafs);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	/* Root is marked on mount */
	if (vfsdev->fanotify_fd != -1 && fsdev_aio_fobject_is_dir(fobject) && parent_fobject) {
		int rc = fsdev_aio_fanotify_add(fobject, parent_fobject->fd, name);
		if (rc) {
			goto err;
		}
	}
#endif

	if (parent_fobject) {
		fobject->parent_fobject = parent_fobject;
		RB_INSERT(aio_fsdev_file_object_tree, &parent_fobject->leafs, &fobject->key);
		file_object_ref(parent_fobject); /* ref by leaf */
	}

	SPDK_DEBUGLOG(fsdev_aio, "fobject created %p (lut=0x%" PRIx64 ")\n", fobject,
		      (uint64_t)fobject->hdr.lut_key);

	return fobject;

err:
	if (lut_key != SPDK_LUT_INVALID_KEY) {
		spdk_lut_remove(vfsdev->lut, lut_key);
	}

	free(fobject->fd_str);
	free(fobject);
	return NULL;
}

/* Allocate and prepare a new fhandle. This does not commit it to the table, so it is not
 * yet visible to other operations. */
static struct aio_fsdev_file_handle *
file_handle_alloc(struct aio_fsdev_file_object *fobject, int fd)
{
	struct aio_fsdev_file_handle *fhandle;
	struct aio_fsdev *vfsdev;
	uint64_t lut_key;

	assert(fobject != NULL);
	vfsdev = fobject->vfsdev;

	fhandle = calloc(1, sizeof(*fhandle));
	if (!fhandle) {
		SPDK_ERRLOG("Cannot alloc fhandle\n");
		return NULL;
	}

	fhandle->hdr.refcount = 0;
	fhandle->fobject_lut_key = fobject->hdr.lut_key;
	fhandle->vfsdev = vfsdev;
	fhandle->fd = fd;

	spdk_spin_lock(&vfsdev->lock);
	lut_key = spdk_lut_insert(vfsdev->lut, fhandle);
	if (lut_key != SPDK_LUT_INVALID_KEY) {
		fhandle->hdr.lut_key = lut_key;
		fhandle->hdr.refcount = 1;
		__atomic_add_fetch(&fobject->hdr.refcount, 1, __ATOMIC_RELAXED); /* ref by fhandle */
		TAILQ_INSERT_TAIL(&fobject->handles, fhandle, link);
	} else {
		spdk_spin_unlock(&vfsdev->lock);
		SPDK_ERRLOG("Cannot insert fhandle into lookup table\n");
		free(fhandle);
		return NULL;
	}
	spdk_spin_unlock(&vfsdev->lock);

	return fhandle;
}

/* Mark a valid file handle as invalid so future lookups will correctly fail. */
static void
file_handle_invalidate(struct aio_fsdev_file_handle *fhandle)
{
	struct aio_fsdev *vfsdev = fhandle->vfsdev;

	spdk_spin_lock(&vfsdev->lock);
	assert(fhandle->hdr.refcount == 1);
	/* A valid entry in the spdk_lut with a refcount of 0 indicates that it is invalid (but hasn't been destroyed) */
	fhandle->hdr.refcount = 0;

	/* We now immediately remove the handle from the table so future lookups do not find it. */
	spdk_lut_remove(vfsdev->lut, fhandle->hdr.lut_key);

	spdk_spin_unlock(&vfsdev->lock);
}

static void
file_handle_destroy(struct aio_fsdev_file_handle *fhandle)
{
	struct aio_fsdev_file_object *fobject = NULL;
	struct aio_fsdev *vfsdev = fhandle->vfsdev;

	assert(fhandle->hdr.refcount == 0);

	spdk_spin_lock(&vfsdev->lock);

	fobject = spdk_lut_get(vfsdev->lut, fhandle->fobject_lut_key);
	if (fobject == SPDK_LUT_INVALID_VALUE) {
		/* This handle refers to an invalid file object. This should not happen. */
		SPDK_WARNLOG("0x%" PRIx64 " is not a valid fobject\n", fhandle->fobject_lut_key);
		fobject = NULL;
	}

	assert(fobject);

	if (spdk_likely(fobject->hdr.is_fobject)) {
		file_object_ref(fobject);
	} else {
		/* Error: the key rather belongs to a fhandle */
		SPDK_WARNLOG("0x%" PRIx64 " is not a fobject\n", fhandle->fobject_lut_key);
		fobject = NULL;
	}

	assert(fobject); /* There shouldn't be NULL fobject in the LUT and neither of the error conditions above should ever hit */

	if (fobject) {
		TAILQ_REMOVE(&fobject->handles, fhandle, link);
	}

	spdk_spin_unlock(&vfsdev->lock);

	if (fobject) {
		file_object_unref(fobject, 1); /* unref the ref we took above */
		file_object_unref(fobject, 1); /* unref for the fhandle */
	}

	if (fhandle->dir.dp) {
		closedir(fhandle->dir.dp);
	} else {
		close(fhandle->fd);
	}

	free(fhandle);
}

static int
file_object_fill_attr(struct aio_fsdev_file_object *fobject, struct spdk_fsdev_file_attr *attr)
{
	struct stat stbuf;
	int res;

	res = fstatat(fobject->fd, "", &stbuf, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatat() failed with %d\n", res);
		return res;
	}

	fobject->mode = stbuf.st_mode;
	memset(attr, 0, sizeof(*attr));

	attr->ino = stbuf.st_ino;
	attr->size = stbuf.st_size;
	attr->blocks = stbuf.st_blocks;
	attr->atime = stbuf.st_atime;
	attr->mtime = stbuf.st_mtime;
	attr->ctime = stbuf.st_ctime;
	attr->atimensec = ST_ATIM_NSEC(&stbuf);
	attr->mtimensec = ST_MTIM_NSEC(&stbuf);
	attr->ctimensec = ST_CTIM_NSEC(&stbuf);
	attr->mode = stbuf.st_mode;
	attr->nlink = stbuf.st_nlink;
	attr->uid = stbuf.st_uid;
	attr->gid = stbuf.st_gid;
	attr->rdev = stbuf.st_rdev;
	attr->blksize = stbuf.st_blksize;
	attr->valid_ms = fobject->vfsdev->opts.attr_valid_ms;

	return 0;
}

static int
utimensat_empty(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject,
		const struct timespec *tv)
{
	int res;

	if (fsdev_aio_fobject_is_symlink(fobject)) {
		res = utimensat(fobject->fd, "", tv, AT_EMPTY_PATH);
		if (res == -1 && errno == EINVAL) {
			/* Sorry, no race free way to set times on symlink. */
			errno = EPERM;
		}
	} else {
		res = utimensat(vfsdev->proc_self_fd, fobject->fd_str, tv, 0);
	}

	return res;
}

static void
fsdev_free_leafs(struct aio_fsdev_file_object *fobject, bool unref_fobject)
{
	uint64_t refcount;
	struct fsdev_aio_key *key, *tmp;
	/* ref to make sure it's not deleted when the last reference by a handle or a leaf removed */
	file_object_ref(fobject);

	while (!TAILQ_EMPTY(&fobject->handles)) {
		struct aio_fsdev_file_handle *fhandle = TAILQ_FIRST(&fobject->handles);
		file_handle_invalidate(fhandle);
		file_handle_destroy(fhandle);
#ifdef __clang_analyzer__
		/*
		 * scan-build fails to comprehend that file_handle_destroy() removes the fhandle
		 * from the queue, so it thinks it's remained accessible and throws the "Use of
		 * memory after it is freed" error here.
		 * The loop below "teaches" the scan-build that the freed fhandle is not on the
		 * list anymore and suppresses the error in this way.
		 */
		struct aio_fsdev_file_handle *tmp;
		TAILQ_FOREACH(tmp, &fobject->handles, link) {
			assert(tmp != fhandle);
		}
#endif
	}

	RB_FOREACH_SAFE(key, aio_fsdev_file_object_tree, &fobject->leafs, tmp) {
		struct aio_fsdev_file_object *leaf_fobject = SPDK_CONTAINEROF(key, struct aio_fsdev_file_object,
				key);
		/* We free (unref) the fobject's leafs in any case as the unref_fobject is only related to the fobject */
		fsdev_free_leafs(leaf_fobject, true);
	}

	refcount = file_object_unref(fobject, 1); /* a ref that we took at the beginning of this function */
	if (refcount && unref_fobject) {
		/* if still referenced - unref by refcount */
		refcount = file_object_unref(fobject, refcount);
		assert(refcount == 0);
		UNUSED(refcount);
	}
}

static int
fsdev_aio_op_getattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	int res;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.getattr.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = file_object_fill_attr(fobject, &fsdev_io->u_out.getattr.attr);
	if (res) {
		SPDK_ERRLOG("Cannot fill attr for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "GETATTR succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_opendir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int error = 0;
	int fd;
	DIR *dp = NULL;
	struct aio_fsdev_file_object *fobject;
	uint32_t flags = fsdev_io->u_in.opendir.flags;
	struct aio_fsdev_file_handle *fhandle = NULL;

	UNUSED(flags);

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.opendir.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(fobject->fd, ".", O_RDONLY);
	if (fd == -1) {
		error = -errno;
		SPDK_ERRLOG("openat failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), error);
		goto do_return;
	}

	dp = fdopendir(fd);
	if (dp == NULL) {
		error = -errno;
		SPDK_ERRLOG("fdopendir failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), error);
		close(fd);
		goto do_return;
	}

	fhandle = file_handle_alloc(fobject, fd);
	if (fhandle == NULL) {
		error = -ENOMEM;
		SPDK_ERRLOG("file_handle_alloc failed for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject),
			    error);
		closedir(dp);
		goto do_return;
	}

	/* The fd is no longer valid once fdopendir succeeds, so
	 * we just set it to -1 here. It is automatically closed
	 * by closedir in the future. */
	fhandle->fd = -1;
	fhandle->dir.dp = dp;
	fhandle->dir.offset = 0;
	fhandle->dir.entry = NULL;

	SPDK_DEBUGLOG(fsdev_aio, "OPENDIR succeeded for " FOBJECT_FMT " (fh=%p)\n",
		      FOBJECT_ARGS(fobject), fhandle);

	fsdev_io->u_out.opendir.fhandle = fsdev_aio_get_spdk_fhandle(vfsdev, fhandle);

do_return:
	file_object_unref(fobject, 1);
	return error;
}

struct fsdev_aio_release_ctx {
	struct aio_fsdev_file_handle *fhandle;
	struct spdk_fsdev_io *fsdev_io;
};

static void
fsdev_aio_thread_barrier(void *ctx)
{
	/* Do nothing. */
}

static void
fsdev_aio_destroy_fhandle_cpl(void *_ctx)
{
	struct fsdev_aio_release_ctx *ctx = _ctx;
	struct spdk_fsdev_io *fsdev_io = ctx->fsdev_io;
	struct aio_fsdev_file_handle *fhandle = ctx->fhandle;

	file_handle_destroy(fhandle);
	spdk_fsdev_io_complete(fsdev_io, 0);

	free(ctx);
}

static int
fsdev_aio_do_release(struct spdk_fsdev_file_handle *fhandle, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct fsdev_aio_release_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Cannot allocate release context\n");
		return -ENOMEM;
	}
	ctx->fhandle = fsdev_aio_get_fhandle(vfsdev, fhandle);
	if (!ctx->fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", ctx->fhandle);
		free(ctx);
		return -EINVAL;
	}
	ctx->fsdev_io = fsdev_io;

	file_handle_invalidate(ctx->fhandle);
	spdk_for_each_thread(fsdev_aio_thread_barrier, ctx, fsdev_aio_destroy_fhandle_cpl);

	return IO_STATUS_ASYNC;
}

static int
fsdev_aio_op_releasedir(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_aio_do_release(fsdev_io->u_in.releasedir.fhandle, fsdev_io);
}

static int
fsdev_aio_set_mount_opts(struct aio_fsdev *vfsdev, struct spdk_fsdev_mount_opts *opts)
{
	bool writeback_cache_enabled = !!(opts->flags & SPDK_FSDEV_MOUNT_WRITEBACK_CACHE);
	uint64_t flags = 0;

	assert(opts != NULL);
	assert(opts->opts_size != 0);

	UNUSED(vfsdev);

	if (opts->opts_size > offsetof(struct spdk_fsdev_mount_opts, max_xfer_size)) {
		/* Set the value the aio fsdev was created with */
		opts->max_xfer_size = vfsdev->mount_opts.max_xfer_size;
	}

	if (opts->opts_size > offsetof(struct spdk_fsdev_mount_opts, max_readahead)) {
		/* Set the value the aio fsdev was created with */
		opts->max_readahead = vfsdev->mount_opts.max_readahead;
	}

	if (vfsdev->opts.writeback_cache_enabled) {
		/* The writeback_cache_enabled was enabled upon creation => we follow the opts */
		vfsdev->opts.writeback_cache_enabled = writeback_cache_enabled;
		SPDK_WARNLOG("Enabling writeback cache is unsafe and requires additional "
			     "synchronization from the applications\n");
	} else {
		/* The writeback_cache_enabled was disabled upon creation => we reflect it in the opts */
		writeback_cache_enabled = false;
	}

#define AIO_SET_MOUNT_FLAG(cond, store, flag) \
	if ((cond) && (opts->flags & (SPDK_FSDEV_MOUNT_##flag))) { \
		store |= (SPDK_FSDEV_MOUNT_##flag);                \
	}

	AIO_SET_MOUNT_FLAG(true, flags, DOT_PATH_LOOKUP);
	AIO_SET_MOUNT_FLAG(true, flags, AUTO_INVAL_DATA);
	AIO_SET_MOUNT_FLAG(true, flags, EXPLICIT_INVAL_DATA);
	AIO_SET_MOUNT_FLAG(true, flags, POSIX_ACL);
	AIO_SET_MOUNT_FLAG(true, flags, POSIX_LOCKS);
	AIO_SET_MOUNT_FLAG(true, flags, FLOCK_LOCKS);
	AIO_SET_MOUNT_FLAG(true, flags, O_TRUNC);
	AIO_SET_MOUNT_FLAG(true, flags, NO_EXPORT_SUPPORT);
	AIO_SET_MOUNT_FLAG(true, flags, DIRECT_IO_ALLOW_MMAP);

	/* Based on the setting above. */
	AIO_SET_MOUNT_FLAG(writeback_cache_enabled, flags, WRITEBACK_CACHE);

	/* Updating negotiated flags. */
	opts->flags = vfsdev->mount_opts.flags = flags;

#undef AIO_SET_MOUNT_FLAG

	/* The AIO doesn't apply any additional restrictions, so we just accept the requested opts */
	SPDK_DEBUGLOG(fsdev_aio,
		      "aio filesystem %s: opts updated: max_xfer_size=%" PRIu32 ", max_readahead=%" PRIu32
		      ", writeback_cache=%" PRIu8 ", mount_flags=%" PRIu64 "\n", vfsdev->fsdev.name,
		      opts->max_xfer_size, opts->max_readahead, writeback_cache_enabled, flags);

	return 0;
}

static void
fsdev_aio_fanotify_close(struct aio_fsdev *vfsdev)
{
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	struct aio_fsdev_linux_fh *entry, *tmp_entry;

	RB_FOREACH_SAFE(entry, aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, tmp_entry) {
		RB_REMOVE(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, entry);
	}

	spdk_poller_unregister(&vfsdev->fanotify_poller);
	if (vfsdev->fanotify_fd != -1) {
		close(vfsdev->fanotify_fd);
		vfsdev->fanotify_fd = -1;
	}
#endif
}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY

static struct aio_fsdev_file_object *
fsdev_aio_get_fobject_by_linux_fh(struct aio_fsdev *vfsdev, const struct file_handle *file_handle)
{
	struct aio_fsdev_linux_fh find = { .fh = file_handle };
	struct aio_fsdev_linux_fh *res;
	struct aio_fsdev_file_object *fobject = NULL;

	spdk_spin_lock(&vfsdev->lock);
	res = RB_FIND(aio_fsdev_linux_fh_tree, &vfsdev->linux_fhs, &find);
	if (res) {
		fobject = SPDK_CONTAINEROF(res, struct aio_fsdev_file_object, linux_fh_entry);
		file_object_ref(fobject);
	}
	spdk_spin_unlock(&vfsdev->lock);

	return fobject;
}

static void
fsdev_aio_notify_reply_cb(const struct spdk_fsdev_notify_reply_data *notify_reply_data,
			  void *reply_ctx)
{
	struct aio_fsdev *vfsdev = reply_ctx;

	SPDK_INFOLOG(fsdev_aio, "Notify reply: status %d, ctx %p\n",
		     notify_reply_data->status, reply_ctx);
	spdk_fsdev_notify_reply_add_stat(&vfsdev->fsdev, SPDK_FSDEV_NOTIFY_INVAL_ENTRY);
}

static void
fsdev_aio_fanotify_attrib_event_handle(struct aio_fsdev *vfsdev, struct file_handle *file_handle,
				       const char *file_name)
{
	struct aio_fsdev_file_object *fobject;

	fobject = fsdev_aio_get_fobject_by_linux_fh(vfsdev, file_handle);
	if (fobject) {
		SPDK_INFOLOG(fsdev_aio, "Notify inval entry: parent " FOBJECT_FMT
			     ", parent fd %d, name %s\n",
			     FOBJECT_ARGS(fobject), fobject->fd, file_name);
		spdk_fsdev_notify_inval_entry(&vfsdev->fsdev,
					      fsdev_aio_get_spdk_fobject(vfsdev, fobject),
					      file_name, fsdev_aio_notify_reply_cb, vfsdev);
		file_object_unref(fobject, 1);
	} else {
		SPDK_INFOLOG(fsdev_aio, "Fobject not found for parent of %s\n", file_name);
	}
}

static void
fsdev_aio_fanotify_event_handle(struct aio_fsdev *vfsdev,
				const struct fanotify_event_metadata *metadata)
{
	struct fanotify_event_info_header *hdr;
	struct file_handle *file_handle = NULL;
	const char *file_name = NULL;
	uint32_t md_len;

	SPDK_DEBUGLOG(fsdev_aio, "Got fanotify event: fd %d, pid %d, mask %016llX\n",
		      metadata->fd, metadata->pid, metadata->mask);

	md_len = metadata->event_len;
	md_len -= sizeof(*metadata);
	hdr = (struct fanotify_event_info_header *)(metadata + 1);
	while (md_len) {
		if (md_len < sizeof(*hdr)) {
			break;
		}

		SPDK_DEBUGLOG(fsdev_aio, "Extra event info of type %u, len %u\n", hdr->info_type, hdr->len);
		assert(md_len >= hdr->len);
		if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
			struct fanotify_event_info_fid *dfid_name = (struct fanotify_event_info_fid *)hdr;
			file_handle = (struct file_handle *)dfid_name->handle;
			file_name = file_handle->f_handle + file_handle->handle_bytes;
		}

		md_len -= hdr->len;
		hdr = (struct fanotify_event_info_header *)((char *)hdr + hdr->len);
	}

	if ((metadata->mask & FAN_ATTRIB) && file_name && file_handle) {
		fsdev_aio_fanotify_attrib_event_handle(vfsdev, file_handle, file_name);
	}

	if (metadata->fd != FAN_NOFD) {
		close(metadata->fd);
	}
}

static int
fsdev_aio_fanotify_poller(void *ctx)
{
	struct aio_fsdev *vfsdev = ctx;
	const struct fanotify_event_metadata *metadata;
	struct fanotify_event_metadata buf[256];
	ssize_t len;

	len = read(vfsdev->fanotify_fd, buf, sizeof(buf));
	if (len == -1 && errno == EAGAIN) {
		return SPDK_POLLER_IDLE;
	} else if (len <= 0) {
		SPDK_ERRLOG("Read fanotify_fd failed: len %ld, errno %d\n", len, errno);
		assert(false);
		fsdev_aio_fanotify_close(vfsdev);
		return SPDK_POLLER_IDLE;
	}

	for (metadata = buf; FAN_EVENT_OK(metadata, len); metadata = FAN_EVENT_NEXT(metadata, len)) {
		if (metadata->vers != FANOTIFY_METADATA_VERSION) {
			SPDK_ERRLOG("Mismatch of fanotify metadata version: expected %d, got %d\n",
				    FANOTIFY_METADATA_VERSION, metadata->vers);
			fsdev_aio_fanotify_close(vfsdev);
			break;
		}

		/* Ignore events from our process */
		if (metadata->pid == vfsdev->pid) {
			continue;
		}

		fsdev_aio_fanotify_event_handle(vfsdev, metadata);
	}

	return SPDK_POLLER_BUSY;
}

#endif /* SPDK_CONFIG_HAVE_FANOTIFY */

static int
fsdev_aio_op_mount(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct spdk_fsdev_mount_opts *in_opts = &fsdev_io->u_in.mount.opts;

	fsdev_io->u_out.mount.opts = *in_opts;
	fsdev_aio_set_mount_opts(vfsdev, &fsdev_io->u_out.mount.opts);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	if (vfsdev->fanotify_fd != -1) {
		int rc;
		spdk_spin_lock(&vfsdev->lock);
		rc = fsdev_aio_fanotify_add(vfsdev->root, AT_FDCWD, vfsdev->root_path);
		spdk_spin_unlock(&vfsdev->lock);
		if (rc) {
			return rc;
		}
	}
#endif

	file_object_ref(vfsdev->root);
	fsdev_io->u_out.mount.root_fobject = fsdev_aio_get_spdk_fobject(vfsdev, vfsdev->root);

	return 0;
}

static int
fsdev_aio_op_umount(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	if (vfsdev->fanotify_fd != -1) {
		fsdev_aio_fanotify_remove(vfsdev->root);
	}
#endif

	fsdev_free_leafs(vfsdev->root, false);
	file_object_unref(vfsdev->root, 1);

	return 0;
}

static int
fsdev_aio_do_lookup(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *parent_fobject,
		    const char *name, struct aio_fsdev_file_object **pfobject,
		    struct spdk_fsdev_file_attr *attr)
{
	int newfd;
	int res;
	bool is_new;
	struct stat stat;
	struct aio_fsdev_file_object *fobject;

	/* Do not allow escaping root directory */
	if (parent_fobject == vfsdev->root && strcmp(name, "..") == 0) {
		name = ".";
	}

	newfd = openat(parent_fobject->fd, name, O_PATH | O_NOFOLLOW);
	if (newfd == -1) {
		res = -errno;
		SPDK_DEBUGLOG(fsdev_aio, "openat( " FOBJECT_FMT " %s) failed with %d\n",
			      FOBJECT_ARGS(parent_fobject), name, res);
		return res;
	}

	res = fstatat(newfd, "", &stat, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatat(%s) failed with %d\n", name, res);
		close(newfd);
		return res;
	}

	spdk_spin_lock(&vfsdev->lock);
	fobject = find_leaf_unsafe(parent_fobject, stat.st_ino, stat.st_dev);
	is_new = (fobject == NULL);
	if (fobject) {
		file_object_ref(fobject); /* reference by a fsdev_aio_do_lookup caller */
	} else {
		fobject = file_object_create_unsafe(vfsdev, parent_fobject, newfd, stat.st_ino, stat.st_dev,
						    stat.st_mode, name);
	}
	spdk_spin_unlock(&vfsdev->lock);

	/* Just in case close() can block, let's keep it out of spinlock. */
	if (!fobject) {
		SPDK_ERRLOG("Cannot create file object\n");
		close(newfd);
		return -ENOMEM;
	}
	if (!is_new) {
		close(newfd);
	}

	if (attr) {
		res = file_object_fill_attr(fobject, attr);
		if (res) {
			SPDK_ERRLOG("fill_attr(%s) failed with %d\n", name, res);
			file_object_unref(fobject, 1);
			return res;
		}
	}

	*pfobject = fobject;

	SPDK_DEBUGLOG(fsdev_aio, "lookup(%s) in dir " FOBJECT_FMT ": "  FOBJECT_FMT " fd=%d\n",
		      name, FOBJECT_ARGS(parent_fobject), FOBJECT_ARGS(fobject), fobject->fd);
	return 0;
}

static int
fsdev_aio_op_lookup(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int err;
	struct aio_fsdev_file_object *parent_fobject;
	struct aio_fsdev_file_object *fobject = NULL;
	const char *name = fsdev_io->u_in.lookup.name;

	/* Don't use is_safe_path_component(), allow "." and ".." for NFS export
	 * support.
	 */
	if (strchr(name, '/')) {
		SPDK_ERRLOG("Invalid name: %s\n", name);
		return -EINVAL;
	}

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.lookup.parent_fobject);
	if (!parent_fobject) {
		err = file_object_fill_attr(vfsdev->root, &fsdev_io->u_out.lookup.attr);
		if (err) {
			SPDK_DEBUGLOG(fsdev_aio, "file_object_fill_attr(root) failed with err=%d\n", err);
			return err;
		}

		file_object_ref(vfsdev->root);
		fsdev_io->u_out.lookup.fobject = fsdev_aio_get_spdk_fobject(vfsdev, vfsdev->root);
		return 0;
	}

	SPDK_DEBUGLOG(fsdev_aio, "  name %s\n", name);

	err = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, &fsdev_io->u_out.lookup.attr);
	if (err) {
		SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_do_lookup(%s) failed with err=%d\n", name, err);
		goto fop_failed;
	}

	fsdev_io->u_out.lookup.fobject = fsdev_aio_get_spdk_fobject(vfsdev, fobject);
	err = 0;

fop_failed:
	file_object_unref(parent_fobject, 1);
	return err;
}

static int
fsdev_aio_op_syncfs(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	int fd, res;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.syncfs.fobject);
	if (fobject != vfsdev->root) {
		SPDK_ERRLOG("Syncfs expected root file object but received " FOBJECT_FMT
			    "\n", FOBJECT_ARGS(fobject));
		return -EINVAL;
	}

	/*
	 * We cannot use root's fd that was open with open(O_PATH) because syncfs()
	 * requires any defined permission and O_PATH has none.
	 */
	fd = open(vfsdev->root_path, O_RDONLY);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot open root %s (err=%d)\n", vfsdev->root_path, res);
		goto fop_failed;
	}

	res = syncfs(fd);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot syncfs for " FOBJECT_FMT " (err=%d)\n", FOBJECT_ARGS(fobject), res);
		close(fd);
		goto fop_failed;
	}
	close(fd);

	SPDK_DEBUGLOG(fsdev_aio, "SYNCFS succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_lseek(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	off_t offset = fsdev_io->u_in.lseek.offset;
	enum spdk_fsdev_seek_whence whence = fsdev_io->u_in.lseek.whence;
	int awhence;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.lseek.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.lseek.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	switch (whence) {
	case SPDK_FSDEV_SEEK_SET:
		awhence = SEEK_SET;
		break;
	case SPDK_FSDEV_SEEK_CUR:
		awhence = SEEK_CUR;
		break;
	case SPDK_FSDEV_SEEK_END:
		awhence = SEEK_END;
		break;
	case SPDK_FSDEV_SEEK_HOLE:
		awhence = SEEK_HOLE;
		break;
	case SPDK_FSDEV_SEEK_DATA:
		awhence = SEEK_DATA;
		break;
	default:
		/* Inducing error from lseek() with invalid whence. */
		awhence = -1;
	}

	offset = lseek(fhandle->fd, offset, awhence);
	fsdev_io->u_out.lseek.offset = offset;
	fsdev_io->u_out.lseek.whence = whence;
	if (offset == (off_t) -1) {
		res = -errno;
		SPDK_ERRLOG("Failed to change read/write offset for " FOBJECT_FMT " (err=%d)\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "LSEEK succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static short
fsdev_events_to_posix(uint32_t spdk_events)
{
	short result = 0;

	if (spdk_events & SPDK_FSDEV_POLLIN) {
		result |= POLLIN;
	}
	if (spdk_events & SPDK_FSDEV_POLLOUT) {
		result |= POLLOUT;
	}
	if (spdk_events & SPDK_FSDEV_POLLPRI) {
		result |= POLLPRI;
	}
	if (spdk_events & SPDK_FSDEV_POLLERR) {
		result |= POLLERR;
	}
	if (spdk_events & SPDK_FSDEV_POLLHUP) {
		result |= POLLHUP;
	}
	if (spdk_events & SPDK_FSDEV_POLLNVAL) {
		result |= POLLNVAL;
	}
	if (spdk_events & SPDK_FSDEV_POLLRDNORM) {
		result |= POLLRDNORM;
	}
	if (spdk_events & SPDK_FSDEV_POLLRDBAND) {
		result |= POLLRDBAND;
	}
	if (spdk_events & SPDK_FSDEV_POLLWRNORM) {
		result |= POLLWRNORM;
	}
	if (spdk_events & SPDK_FSDEV_POLLWRBAND) {
		result |= POLLWRBAND;
	}

	return result;
}

static uint32_t
posix_events_to_fsdev(short events)
{
	uint32_t result = 0;

	if (events & POLLIN) {
		result |= SPDK_FSDEV_POLLIN;
	}
	if (events & POLLOUT) {
		result |= SPDK_FSDEV_POLLOUT;
	}
	if (events & POLLPRI) {
		result |= SPDK_FSDEV_POLLPRI;
	}
	if (events & POLLERR) {
		result |= SPDK_FSDEV_POLLERR;
	}
	if (events & POLLHUP) {
		result |= SPDK_FSDEV_POLLHUP;
	}
	if (events & POLLNVAL) {
		result |= SPDK_FSDEV_POLLNVAL;
	}
	if (events & POLLRDNORM) {
		result |= SPDK_FSDEV_POLLRDNORM;
	}
	if (events & POLLRDBAND) {
		result |= SPDK_FSDEV_POLLRDBAND;
	}
	if (events & POLLWRNORM) {
		result |= SPDK_FSDEV_POLLWRNORM;
	}
	if (events & POLLWRBAND) {
		result |= SPDK_FSDEV_POLLWRBAND;
	}

	return result;
}


static int
fsdev_aio_do_poll(struct aio_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	short posix_events = fsdev_events_to_posix(fsdev_io->u_in.poll.events);
	struct pollfd fds;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.poll.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.poll.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	fds.fd = fhandle->fd;
	fds.events = posix_events;
	fds.revents = 0;

	/* Zero timeout - return immediately even if no events available. */
	res = poll(&fds, 1, 0);
	fsdev_io->u_out.poll.revents = posix_events_to_fsdev(fds.revents);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Failed poll for " FOBJECT_FMT " (err=%d)\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	/*
	 * Wait is set and there are no events -> wait for the fhandle to
	 * become ready to perform I/O
	 */
	if (res == 0 && fsdev_io->u_in.poll.wait) {
		TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
		res = IO_STATUS_ASYNC;
		goto fop_failed;
	}

	/*
	 * The fsdev API expects -EAGAIN for no-events case and 0 for
	 * the case any events available.
	 */
	if (res == 0) {
		res = -EAGAIN;
	} else if (res > 0) {
		res = 0;
	}

	SPDK_DEBUGLOG(fsdev_aio, "POLL succeeded for " FOBJECT_FMT "\n", FOBJECT_ARGS(fobject));

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_poll(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	return fsdev_aio_do_poll(ch, fsdev_io);
}

static struct aio_ioctl_unrest aio_unrest;
static struct aio_ioctl_rest aio_rest;

static void
fsdev_aio_ioctl_io_free(void *cb_arg)
{
	free(cb_arg);
}

static int
fsdev_aio_ioctl_retry(struct spdk_fsdev_io *fsdev_io,
		      const struct iovec *in_iov, uint32_t in_iovcnt,
		      const struct iovec *out_iov, uint32_t out_iovcnt)
{
	struct iovec *iov;
	uint32_t iovcnt = in_iovcnt + out_iovcnt;
	size_t size = iovcnt * sizeof(struct iovec);

	fsdev_io->u_out.ioctl.in_iov = NULL;
	fsdev_io->u_out.ioctl.in_iovcnt = 0;
	fsdev_io->u_out.ioctl.out_iov = NULL;
	fsdev_io->u_out.ioctl.out_iovcnt = 0;

	if (!iovcnt)  {
		return 0;
	}

	iov = calloc(1, size);
	if (!iov) {
		return -ENOMEM;
	}

	if (in_iovcnt && in_iov) {
		fsdev_io->u_out.ioctl.in_iov = iov;
		fsdev_io->u_out.ioctl.in_iovcnt = in_iovcnt;
		memcpy(fsdev_io->u_out.ioctl.in_iov, in_iov, in_iovcnt * sizeof(struct iovec));
	}

	if (out_iovcnt && out_iov) {
		fsdev_io->u_out.ioctl.out_iov = iov + in_iovcnt;
		fsdev_io->u_out.ioctl.out_iovcnt = out_iovcnt;
		memcpy(fsdev_io->u_out.ioctl.out_iov, out_iov, out_iovcnt * sizeof(struct iovec));
	}

	spdk_fsdev_io_set_cleanup_callback(fsdev_io, fsdev_aio_ioctl_io_free, iov);

	return -EAGAIN;
}

/**
 * Example implemenatation of ioctl with SPDK_FSDEV_IOCTL_RETRY protocol
 * support.
 *
 * It handles unrestricted and the "traditional" ioctl cmds that we created
 * to show how to do that properly.
 * - AIO_IOCTL_GET_UNREST_DATA_CMD - unrestricted GET of the local data with
 *   embedded buffer pointer,
 * - AIO_IOCTL_SET_UNREST_DATA_CMD - same as the previous but for setting the
 *   local data.
 * - AIO_IOCTL_GET_REST_DATA_CMD - traditional get for some internal struct
 *   which size is known.
 * - AIO_IOCTL_SET_REST_DATA_CMD - same as the previous for setting the local
 *   data.
 * - AIO_IOCTL_REST_DATA_CMD - getting and setting data in same cmd.
 * - AIO_IOCTL_ACT_CMD - no data, just a command.
 */
static int
fsdev_aio_do_aio_ioctl(struct spdk_fsdev_io *fsdev_io)
{
	uint32_t request = fsdev_io->u_in.ioctl.request;
	struct iovec *in_iovec = fsdev_io->u_in.ioctl.in_iov;
	struct iovec *out_iovec = fsdev_io->u_in.ioctl.out_iov;
	uint32_t in_iovcnt = fsdev_io->u_in.ioctl.in_iovcnt;
	uint32_t out_iovcnt = fsdev_io->u_in.ioctl.out_iovcnt;
	void *arg = (void *)(uintptr_t)fsdev_io->u_in.ioctl.arg;
	struct iovec in_iov[2], out_iov[1];
	struct aio_ioctl_unrest *ur_data;
	struct aio_ioctl_rest *rt_data;
	struct aio_ioctl_rest saved;
	uint32_t in_bufsz, out_bufsz;
	void *in_buf, *out_buf;

	bool unrestricted = (request == AIO_IOCTL_GET_UNREST_DATA_CMD ||
			     request == AIO_IOCTL_SET_UNREST_DATA_CMD);

	in_iov[0].iov_base = arg;
	in_iov[0].iov_len = unrestricted ? sizeof(*ur_data) : sizeof(*rt_data);

	in_buf = in_iovcnt && in_iovec ? in_iovec[0].iov_base : NULL;
	in_bufsz = in_iovcnt && in_iovec ? in_iovec[0].iov_len : 0;

	out_buf = out_iovcnt && out_iovec ? out_iovec[0].iov_base : NULL;
	out_bufsz = out_iovcnt && out_iovec ? out_iovec[0].iov_len : 0;

	switch (request) {
	case AIO_IOCTL_GET_UNREST_DATA_CMD:
		/* No input data available - requesting RETRY. */
		if (!in_bufsz) {
			return fsdev_aio_ioctl_retry(fsdev_io, in_iov, 1, NULL, 0);
		}

		ur_data = (struct aio_ioctl_unrest *)in_buf;

		/*
		 * No output info available - sending back information regarding the arg internal
		 * buffer and requesting RETRY.
		 */
		if (!out_bufsz) {
			out_iov[0].iov_base = ur_data->buf;
			out_iov[0].iov_len = ur_data->size;
			return fsdev_aio_ioctl_retry(fsdev_io, in_iov, 1, out_iov, 1);
		}

		/* Have got all we needed - populate the data with internal structure data. */
		memcpy(out_buf, aio_unrest.buf, spdk_min(out_bufsz, aio_unrest.size));
		break;
	case AIO_IOCTL_SET_UNREST_DATA_CMD:
		/* No input data available - requesting RETRY. */
		if (!in_bufsz) {
			return fsdev_aio_ioctl_retry(fsdev_io, in_iov, 1, NULL, 0);
		}

		ur_data = (struct aio_ioctl_unrest *)in_buf;

		if (in_bufsz < sizeof(*ur_data)) {
			SPDK_ERRLOG("Invalid input buffer size=%u\n", in_bufsz);
			return -EINVAL;
		}

		/* Consumed the size of the root structure. */
		in_bufsz -= sizeof(*ur_data);
		in_buf += sizeof(*ur_data);

		/*
		 * Input iovec has only info about root structure. Sending back internal buffer info and
		 * requesting RETRY.
		 */
		if (ur_data->size && !in_bufsz) {
			in_iov[1].iov_base = ur_data->buf;
			in_iov[1].iov_len = ur_data->size;
			return fsdev_aio_ioctl_retry(fsdev_io, in_iov, 2, NULL, 0);
		}

		/* Got all we needed. Populate the local data. No data in response. */
		if (aio_unrest.size < in_bufsz) {
			aio_unrest.buf = realloc(aio_unrest.buf, in_bufsz);
			if (!aio_unrest.buf) {
				return -ENOMEM;
			}
			aio_unrest.size = in_bufsz;
		}
		memcpy(aio_unrest.buf, in_buf, in_bufsz);
		break;
	case AIO_IOCTL_GET_REST_DATA_CMD:
		/*
		 * Invalid out size. Requesting RETRY with the correct size. For restricted variant of ioctl()
		 * this results into -EIO, which is expected.
		 */
		if (out_bufsz != sizeof(*rt_data)) {
			out_iov[0].iov_base = arg;
			out_iov[0].iov_len = sizeof(*rt_data);
			return fsdev_aio_ioctl_retry(fsdev_io, NULL, 0, out_iov, 1);
		}

		rt_data = (struct aio_ioctl_rest *)out_buf;
		*rt_data = aio_rest;
		break;
	case AIO_IOCTL_SET_REST_DATA_CMD:
		/*
		 * Invalid input size. Requesting RETRY with the correct size. For restricted variant of ioctl()
		 * this results into -EIO, which is expected.
		 */
		if (in_bufsz != sizeof(*rt_data)) {
			return fsdev_aio_ioctl_retry(fsdev_io, in_iov, 1, NULL, 0);
		}

		rt_data = (struct aio_ioctl_rest *)in_buf;
		aio_rest = *rt_data;
		break;
	case AIO_IOCTL_REST_DATA_CMD:
		/*
		 * Invalid input or output size. Requesting RETRY with the correct sizes. For restricted variant
		 * of ioctl() this results into -EIO, which is expected.
		 */
		if (in_bufsz != sizeof(*rt_data) || out_bufsz != sizeof(*rt_data)) {
			out_iov[0].iov_base = arg;
			out_iov[0].iov_len = sizeof(*rt_data);
			return fsdev_aio_ioctl_retry(fsdev_io, in_iov, 1, out_iov, 0);
		}

		/*
		 * Input and output buffers can point to the same region of memory. Saving the input.
		 */
		rt_data = (struct aio_ioctl_rest *)in_buf;
		saved = *rt_data;

		/*
		 * Populating the data and sending _old_ data back (we decided we want this kind of behavior
		 * for this particular custom ioctl cmd) like a normal get.
		 */
		rt_data = (struct aio_ioctl_rest *)out_buf;
		*rt_data = aio_rest;
		aio_rest = saved;
		break;
	case AIO_IOCTL_ACT_CMD:
		SPDK_DEBUGLOG(fsdev_aio, "Zero-sized ioctl() has been successfully handled.\n");
		break;
	default:
		SPDK_INFOLOG(fsdev_aio, "Unknown ioctl cmd: %u\n", request);
		return -ENOTTY;
	}

	return 0;
}

static int
fsdev_aio_do_fsioc_ioctl(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject,
			 uint32_t request, void *ioctl_arg)
{
	int fd;
	int res;

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Failed to open fd %d\n", fobject->fd);
		return res;
	}

	res = ioctl(fd, request, ioctl_arg);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("ioctl failed for fd %d\n", fd);
	}
	close(fd);
	return res;
}

static int
fsdev_aio_op_ioctl(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	uint32_t request =  fsdev_io->u_in.ioctl.request;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.ioctl.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fsdev_io->u_out.ioctl.in_iovcnt = 0;
	fsdev_io->u_out.ioctl.out_iovcnt = 0;

	/*
	 * Zero for now and in case of forwarding ioctl to the local filesystem this
	 * can hold the return code of the ioctl() function.
	 */
	fsdev_io->u_out.ioctl.result = 0;
	switch (request) {
	case AIO_IOCTL_GET_UNREST_DATA_CMD:
	case AIO_IOCTL_SET_UNREST_DATA_CMD:
	case AIO_IOCTL_GET_REST_DATA_CMD:
	case AIO_IOCTL_SET_REST_DATA_CMD:
	case AIO_IOCTL_REST_DATA_CMD:
	case AIO_IOCTL_ACT_CMD:
		res = fsdev_aio_do_aio_ioctl(fsdev_io);
		break;
	case FS_IOC_GETFLAGS:
	case FS_IOC_FSGETXATTR:
		res = fsdev_aio_do_fsioc_ioctl(vfsdev, fobject, request, fsdev_io->u_in.ioctl.out_iov->iov_base);
		break;
	case FS_IOC_SETFLAGS:
		res = fsdev_aio_do_fsioc_ioctl(vfsdev, fobject, request, fsdev_io->u_in.ioctl.in_iov->iov_base);
		break;
	default:
		SPDK_INFOLOG(fsdev_aio, "Unknown ioctl cmd: %u\n", request);
		res = -ENOTTY;
		break;
	}

	SPDK_DEBUGLOG(fsdev_aio, "IOCTL(%u) for " FOBJECT_FMT " handled with result=%d\n",
		      request, FOBJECT_ARGS(fobject), res);

	file_object_unref(fobject, 1);
	return res;
}

#if DEBUG
static const char *
posix_lock_type_to_str(uint32_t posix_lock_type)
{
	if (posix_lock_type == F_RDLCK) {
		return "F_RDLCK";
	} else if (posix_lock_type == F_WRLCK) {
		return "F_WRLCK";
	} else if (posix_lock_type == F_UNLCK) {
		return "F_UNLCK";
	} else {
		return "UNKNOWN";
	}
}
#endif

static int
fsdev_file_lock_to_flock(struct aio_fsdev_file_handle *fhandle,
			 struct spdk_fsdev_file_lock *fsdev_lock,
			 struct flock *posix_lock)
{
	switch (fsdev_lock->type) {
	case SPDK_FSDEV_RDLCK:
		posix_lock->l_type = F_RDLCK;
		break;
	case SPDK_FSDEV_WRLCK:
		posix_lock->l_type = F_WRLCK;
		break;
	case SPDK_FSDEV_UNLCK:
		posix_lock->l_type = F_UNLCK;
		break;
	default:
		SPDK_ERRLOG("Invalid lock type %d encountered during fsdev to flock conversion.\n",
			    fsdev_lock->type);
		return -EINVAL;
	}

	posix_lock->l_whence = SEEK_SET;

	posix_lock->l_start = fsdev_lock->start;
	if (fsdev_lock->end == SPDK_FSDEV_FILE_LOCK_END_OF_FILE) {
		/* 0 means lock to the end of the file in POSIX */
		posix_lock->l_len = 0;
	} else {
		posix_lock->l_len = fsdev_lock->end - fsdev_lock->start + 1;
	}

	posix_lock->l_pid = fsdev_lock->pid;

	SPDK_DEBUGLOG(fsdev_aio, "fsdev -> flock type=%s, start=%lu, len=%lu, pid=%u\n",
		      posix_lock_type_to_str(posix_lock->l_type), posix_lock->l_start,
		      posix_lock->l_len, posix_lock->l_pid);

	return 0;
}

static int
flock_to_fsdev_file_lock(struct aio_fsdev_file_handle *fhandle,
			 struct flock *posix_lock,
			 struct spdk_fsdev_file_lock *fsdev_lock)
{
	off_t current_pos;

	switch (posix_lock->l_type) {
	case F_RDLCK:
		fsdev_lock->type = SPDK_FSDEV_RDLCK;
		break;
	case F_WRLCK:
		fsdev_lock->type = SPDK_FSDEV_WRLCK;
		break;
	case F_UNLCK:
		fsdev_lock->type = SPDK_FSDEV_UNLCK;
		break;
	default:
		SPDK_ERRLOG("Invalid lock type %d encountered during flock to fsdev conversion.\n",
			    posix_lock->l_type);
		return -EINVAL;
	}

	switch (posix_lock->l_whence) {
	case SEEK_SET:
		fsdev_lock->start = posix_lock->l_start;
		break;
	case SEEK_CUR:
		current_pos = lseek(fhandle->fd, 0, SEEK_CUR);
		if (current_pos == (off_t) -1) {
			SPDK_ERRLOG("Failed to get current file pos for fh=%p during "
				    "posix lock conversion with whence=%d!\n", fhandle,
				    posix_lock->l_whence);
			return -EINVAL;
		}
		fsdev_lock->start = current_pos + posix_lock->l_start;
		break;
	case SEEK_END:
		current_pos = lseek(fhandle->fd, 0, SEEK_END);
		if (current_pos == (off_t) -1) {
			SPDK_ERRLOG("Failed to get current file pos for fh=%p during "
				    "posix lock conversion with whence=%d!\n", fhandle,
				    posix_lock->l_whence);
			return -EINVAL;
		}
		fsdev_lock->start = current_pos + posix_lock->l_start;
		break;
	default:
		SPDK_ERRLOG("Invalid whence=%d for fh=%p during "
			    "posix lock conversion!\n", posix_lock->l_whence, fhandle);
		return -EINVAL;
	}
	if (posix_lock->l_len == 0) {
		/* Lock to the end of the file. */
		fsdev_lock->end = LONG_MAX;
	} else {
		fsdev_lock->end = posix_lock->l_start + posix_lock->l_len - 1;
	}

	fsdev_lock->pid = posix_lock->l_pid;

	SPDK_DEBUGLOG(fsdev_aio, "flock -> fsdev lock type=%x, start=%lu, end=%lu, pid=%u\n",
		      fsdev_lock->type, fsdev_lock->start, fsdev_lock->end, fsdev_lock->pid);
	return 0;
}

/*
 * This function is not fully functional implementation of getlk() operation.
 * In the enviroment where virtiofs is used the lock pid is usually wrong or 0
 * which needs to be specially handled. Thimnk of it as of an example or
 * tutorial of how it can be implemented.
 */
static int
fsdev_aio_op_getlk(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct flock posix_lock;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	struct spdk_fsdev_file_lock *fsdev_lock = &fsdev_io->u_in.getlk.lock;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.getlk.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.getlk.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	/*
	 * We're using the input lock and passing it to fcntl(F_GETLK).
	 * This technique is used for checking if a lock of particular
	 * type and the file region can be obtained.
	 */
	res = fsdev_file_lock_to_flock(fhandle, fsdev_lock, &posix_lock);
	if (res) {
		goto fop_failed;
	}

	res = fcntl(fhandle->fd, F_GETLK, &posix_lock);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Getlk failed for " FOBJECT_FMT " (err=%d)\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	res = flock_to_fsdev_file_lock(fhandle, &posix_lock, &fsdev_io->u_out.getlk.lock);
	if (res) {
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "GETLK succeeded for " FOBJECT_FMT " lock=(type:%d,start:%lu,len:%lu)\n",
		      FOBJECT_ARGS(fobject), posix_lock.l_type, posix_lock.l_start, posix_lock.l_len);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

/*
 * This function is not fully functional implementation of setlk() operation.
 * In the environment where fsdev is used the lock pid is usually wrong or 0
 * which needs to be specially handled. Think of it as of an example or
 * tutorial of how it can be implemented.
 */
static int
fsdev_aio_do_setlk(struct aio_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int res;
	struct flock posix_lock;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	struct spdk_fsdev_file_lock *fsdev_lock = &fsdev_io->u_in.setlk.lock;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.setlk.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.setlk.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	res = fsdev_file_lock_to_flock(fhandle, fsdev_lock, &posix_lock);
	if (res) {
		goto fop_failed;
	}

	res = fcntl(fhandle->fd, F_SETLK, &posix_lock);
	if (res == -1) {
		res = -errno;

		/*
		 * Some implementations return -EACCES for conflicting locks. We show
		 * error for the other error codes.
		 */
		if (res != -EACCES && res != -EAGAIN) {
			SPDK_ERRLOG("Fcntl failed for " FOBJECT_FMT " (err=%d)\n",
				    FOBJECT_ARGS(fobject), res);
		} else if (res == -EACCES) {
			res = -EAGAIN;
			goto fop_failed;
		}

		if (res == -EAGAIN && fsdev_io->u_in.setlk.wait) {
			TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
			res = IO_STATUS_ASYNC;
		}

		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "SETLK succeeded for " FOBJECT_FMT " lock=(type:%d,start:%lu,len:%lu)\n",
		      FOBJECT_ARGS(fobject), posix_lock.l_type, posix_lock.l_start, posix_lock.l_len);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_setlk(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);

	return fsdev_aio_do_setlk(ch, fsdev_io);
}

/*
 * Change to uid/gid of caller so that file is created with ownership of caller.
 */
static int
fsdev_aio_change_cred(const struct fsdev_aio_cred *new, struct fsdev_aio_cred *old)
{
	int res;

	old->euid = geteuid();
	old->egid = getegid();

	res = syscall(SYS_setresgid, -1, new->egid, -1);
	if (res == -1) {
		return -errno;
	}

	res = syscall(SYS_setresuid, -1, new->euid, -1);
	if (res == -1) {
		int errno_save = -errno;

		syscall(SYS_setresgid, -1, old->egid, -1);
		return errno_save;
	}

	return 0;
}

/* Regain Privileges */
static void
fsdev_aio_restore_cred(struct fsdev_aio_cred *old)
{
	int res;

	res = syscall(SYS_setresuid, -1, old->euid, -1);
	if (res == -1) {
		SPDK_ERRLOG("seteuid(%u)", old->euid);
	}

	res = syscall(SYS_setresgid, -1, old->egid, -1);
	if (res == -1) {
		SPDK_ERRLOG("setegid(%u)", old->egid);
	}
}

static int
fsdev_aio_op_readdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	off_t offset = (off_t)fsdev_io->u_in.readdir.offset;
	struct aio_fsdev_file_object *entry_fobject;
	int res;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.readdir.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.readdir.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	if (((off_t)offset) != fhandle->dir.offset) {
		seekdir(fhandle->dir.dp, offset);
		fhandle->dir.entry = NULL;
		fhandle->dir.offset = offset;
	}

	while (1) {
		const char *name;
		struct spdk_fsdev_file_attr attr = {0};
		bool forget = false;

		if (!fhandle->dir.entry) {
			errno = 0;
			fhandle->dir.entry = readdir(fhandle->dir.dp);
			if (!fhandle->dir.entry) {
				if (errno) {  /* Error */
					res = -errno;
					SPDK_ERRLOG("readdir failed with err=%d", res);
					goto fop_failed;
				} else {  /* End of stream */
					break;
				}
			}
		}

		offset = fhandle->dir.entry->d_off;
		name = fhandle->dir.entry->d_name;
		entry_fobject = NULL;

		/* Hide root's parent directory */
		if (fobject == vfsdev->root && strcmp(name, "..") == 0) {
			goto skip_entry;
		}

		if (is_dot_or_dotdot(name)) {
			attr.ino = fhandle->dir.entry->d_ino;
			attr.mode = DT_DIR << 12;
			goto skip_lookup;
		}

		res = fsdev_aio_do_lookup(vfsdev, fobject, name, &entry_fobject, &attr);
		if (res) {
			SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_do_lookup(%s) failed with err=%d\n", name, res);
			goto fop_failed;
		}

skip_lookup:
		res = fsdev_io->u_in.readdir.entry_cb_fn(fsdev_io->internal.usr_cb_arg, fsdev_io, name,
				entry_fobject ? fsdev_aio_get_spdk_fobject(vfsdev, entry_fobject) : NULL,
				&attr, offset, &forget);
		if ((forget || res) && entry_fobject) {
			file_object_unref(entry_fobject, 1);
		}
		if (res) {
			break;
		}

skip_entry:
		fhandle->dir.entry = NULL;
		fhandle->dir.offset = offset;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio,
		      "READDIR succeeded for " FOBJECT_FMT " (fh=%p, offset=%" PRIu64 " -> %" PRIu64 ")\n",
		      FOBJECT_ARGS(fobject), fhandle, offset, offset);
fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_forget(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	uint64_t nlookup = fsdev_io->u_in.forget.nlookup;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.readdir.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FORGET for " FOBJECT_FMT " nlookup=%" PRIu64 "\n",
		      FOBJECT_ARGS(fobject), nlookup);
	file_object_unref(fobject, nlookup + 1 /* + 1 for the fsdev_aio_get_fobject */);

	return 0;
}

static uint32_t
update_open_flags(struct aio_fsdev *vfsdev, uint32_t flags)
{
	/*
	 * With writeback cache, kernel may send read requests even
	 * when userspace opened write-only
	 */
	if (vfsdev->opts.writeback_cache_enabled && (flags & O_ACCMODE) == O_WRONLY) {
		flags &= ~O_ACCMODE;
		flags |= O_RDWR;
	}

	/*
	 * With writeback cache, O_APPEND is handled by the kernel.
	 * This breaks atomicity (since the file may change in the
	 * underlying filesystem, so that the kernel's idea of the
	 * end of the file isn't accurate anymore). In this example,
	 * we just accept that. A more rigorous filesystem may want
	 * to return an error here
	 */
	if (vfsdev->opts.writeback_cache_enabled && (flags & O_APPEND)) {
		flags &= ~O_APPEND;
	}

	/*
	 * O_DIRECT in guest should not necessarily mean bypassing page
	 * cache on host as well. If somebody needs that behavior, it
	 * probably should be a configuration knob in daemon.
	 */
	flags &= ~O_DIRECT;

	return flags;
}

static int
fsdev_aio_op_open(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int fd, res;
	struct aio_fsdev_file_object *fobject;
	uint32_t flags = fsdev_io->u_in.open.flags;
	struct aio_fsdev_file_handle *fhandle;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.open.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	flags = update_open_flags(vfsdev, flags);

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, flags & ~O_NOFOLLOW);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat(%d, %s, 0x%08" PRIx32 ") failed with err=%d\n",
			    vfsdev->proc_self_fd, fobject->fd_str, flags, res);
		goto fop_failed;
	}

	fhandle = file_handle_alloc(fobject, fd);
	if (!fhandle) {
		res = -ENOMEM;
		SPDK_ERRLOG("cannot create a file handle (fd=%d)\n", fd);
		close(fd);
		goto fop_failed;
	}

	fsdev_io->u_out.open.fhandle = fsdev_aio_get_spdk_fhandle(vfsdev, fhandle);

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "OPEN succeeded for " FOBJECT_FMT " (fh=%p, fd=%d)\n",
		      FOBJECT_ARGS(fobject), fhandle, fd);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_flush(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	int res, dup_fd;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.flush.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.flush.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	dup_fd = dup(fhandle->fd);
	if (dup_fd == -1) {
		res = -errno;
		SPDK_ERRLOG("dup(%d) failed for " FOBJECT_FMT " (fh=%p, err=%d)\n",
			    fhandle->fd, FOBJECT_ARGS(fobject), fhandle, res);
		goto fop_failed;
	}
	res = close(dup_fd);
	if (res) {
		res = -errno;
		SPDK_ERRLOG("close(%d) failed for " FOBJECT_FMT " (fh=%p, err=%d)\n",
			    dup_fd, FOBJECT_ARGS(fobject), fhandle, res);
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "FLUSH succeeded for " FOBJECT_FMT " (fh=%p)\n", FOBJECT_ARGS(fobject),
		      fhandle);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_fchmodat(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *fobject, uint32_t mode)
{
	int res;

	res = fchmodat(fobject->fd, "", mode, AT_EMPTY_PATH);
	if (res == -1) {
		if (errno == EINVAL) {
			/* Linux only gained support for AT_EMPTY_PATH in fchmodat recently. We'll use a fallback option. */
			res = fchmodat(vfsdev->proc_self_fd, fobject->fd_str, mode, 0);
		}
	}

	return res;
}

static int
fsdev_aio_op_setattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	uint32_t to_set = fsdev_io->u_in.setattr.to_set;
	struct spdk_fsdev_file_attr *attr = &fsdev_io->u_in.setattr.attr;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.setattr.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	/* fhandle is optional here */
	fhandle = fsdev_io->u_in.setattr.fhandle ?
		  fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.setattr.fhandle) : NULL;

	if (to_set & SPDK_FSDEV_ATTR_MODE) {
		if (fhandle) {
			res = fchmod(fhandle->fd, attr->mode);
		} else {
			res = fsdev_fchmodat(vfsdev, fobject, attr->mode);
		}
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("fchmod failed for " FOBJECT_FMT " with %d\n", FOBJECT_ARGS(fobject), res);
			goto fop_failed;
		}
		fobject->mode = attr->mode;
	}

	if (to_set & (SPDK_FSDEV_ATTR_UID | SPDK_FSDEV_ATTR_GID)) {
		uid_t uid = (to_set & SPDK_FSDEV_ATTR_UID) ? attr->uid : (uid_t) -1;
		gid_t gid = (to_set & SPDK_FSDEV_ATTR_GID) ? attr->gid : (gid_t) -1;

		res = fchownat(fobject->fd, "", uid, gid, AT_EMPTY_PATH);
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("fchownat failed for " FOBJECT_FMT " with %d\n", FOBJECT_ARGS(fobject), res);
			goto fop_failed;
		}
	}

	if (to_set & SPDK_FSDEV_ATTR_SIZE) {
		int truncfd;

		if (fhandle) {
			truncfd = fhandle->fd;
		} else {
			truncfd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
			if (truncfd < 0) {
				res = -errno;
				SPDK_ERRLOG("openat failed for " FOBJECT_FMT " with %d\n", FOBJECT_ARGS(fobject), res);
				goto fop_failed;
			}
		}

		res = ftruncate(truncfd, attr->size);
		if (!fhandle) {
			int saverr = errno;
			close(truncfd);
			errno = saverr;
		}
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("ftruncate failed for " FOBJECT_FMT " (size=%" PRIu64 ")\n", FOBJECT_ARGS(fobject),
				    attr->size);
			goto fop_failed;
		}
	}

	if (to_set & (SPDK_FSDEV_ATTR_ATIME | SPDK_FSDEV_ATTR_MTIME)) {
		struct timespec tv[2];

		tv[0].tv_sec = 0;
		tv[1].tv_sec = 0;
		tv[0].tv_nsec = UTIME_OMIT;
		tv[1].tv_nsec = UTIME_OMIT;

		if (to_set & SPDK_FSDEV_ATTR_ATIME_NOW) {
			tv[0].tv_nsec = UTIME_NOW;
		} else if (to_set & SPDK_FSDEV_ATTR_ATIME) {
			tv[0].tv_sec = attr->atime;
			tv[0].tv_nsec = attr->atimensec;
		}

		if (to_set & SPDK_FSDEV_ATTR_MTIME_NOW) {
			tv[1].tv_nsec = UTIME_NOW;
		} else if (to_set & SPDK_FSDEV_ATTR_MTIME) {
			tv[1].tv_sec = attr->mtime;
			tv[1].tv_nsec = attr->mtimensec;
		}

		if (fhandle) {
			res = futimens(fhandle->fd, tv);
		} else {
			res = utimensat_empty(vfsdev, fobject, tv);
		}
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("futimens/utimensat_empty failed for " FOBJECT_FMT " with %d\n",
				    FOBJECT_ARGS(fobject), res);
			goto fop_failed;
		}
	}

	res = file_object_fill_attr(fobject, &fsdev_io->u_out.setattr.attr);
	if (res) {
		SPDK_ERRLOG("file_object_fill_attr failed for " FOBJECT_FMT "\n",
			    FOBJECT_ARGS(fobject));
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "SETATTR succeeded for " FOBJECT_FMT "\n",
		      FOBJECT_ARGS(fobject));

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_create(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int fd = -1;
	int err;
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_io->u_in.create.name;
	uint32_t mode = fsdev_io->u_in.create.mode;
	uint32_t flags = fsdev_io->u_in.create.flags;
	uint32_t umask = fsdev_io->u_in.create.umask;
	struct fsdev_aio_cred old_cred, new_cred = {
		.euid = fsdev_io->u_in.create.euid,
		.egid = fsdev_io->u_in.create.egid,
	};
	struct aio_fsdev_file_handle *fhandle = NULL;
	struct aio_fsdev_file_object *fobject = NULL;
	struct spdk_fsdev_file_attr *attr = &fsdev_io->u_out.create.attr;

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("CREATE: %s not a safe component\n", name);
		return -EINVAL;
	}

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.create.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	err = fsdev_aio_change_cred(&new_cred, &old_cred);
	if (err) {
		SPDK_ERRLOG("CREATE: cannot change credentials\n");
		goto fop_failed;
	}

	flags = update_open_flags(vfsdev, flags);

	fd = openat(parent_fobject->fd, name, (flags | O_CREAT) & ~O_NOFOLLOW, (mode & ~umask));
	err = fd == -1 ? -errno : 0;
	fsdev_aio_restore_cred(&old_cred);

	if (err) {
		SPDK_ERRLOG("CREATE: openat failed with %d\n", err);
		goto fop_failed;
	}

	/* Fixup mode, openat() ignores some bits important for POSIX compliance. */
	err = fchmod(fd, (mode & ~umask));
	if (err == -1) {
		err = -errno;
		SPDK_ERRLOG("CREATE: lookup failed with %d\n", err);
		goto fop_failed;
	}

	err = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, attr);
	if (err) {
		SPDK_ERRLOG("CREATE: lookup failed with %d\n", err);
		goto fop_failed;
	}
	assert(fobject != NULL);
	attr->mode = (mode & ~umask);

	fhandle = file_handle_alloc(fobject, fd);
	if (!fhandle) {
		file_object_unref(fobject, 1);
		err = -ENOMEM;
		SPDK_ERRLOG("CREATE: failed to create a file handle (fd=%d) with %d\n",
			    fd, err);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "CREATE: succeeded (name=%s " FOBJECT_FMT " fh=%p)\n",
		      name, FOBJECT_ARGS(fobject), fhandle);

	fsdev_io->u_out.create.fobject = fsdev_aio_get_spdk_fobject(vfsdev, fobject);
	fsdev_io->u_out.create.fhandle = fsdev_aio_get_spdk_fhandle(vfsdev, fhandle);

	err = 0;

fop_failed:
	if (err && fd != -1) {
		close(fd);
	}
	file_object_unref(parent_fobject, 1);
	return err;
}

static int
fsdev_aio_op_release(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_aio_do_release(fsdev_io->u_in.release.fhandle, fsdev_io);
}

static inline void
fsdev_aio_prep_read_aio(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	io_prep_preadv(&vfsdev_io->io, fd, iovs, iovcnt, offs);
	vfsdev_io->io.data = vfsdev_io;
	TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
}

#ifdef SPDK_CONFIG_URING
static inline int
fsdev_aio_prep_read_io_uring(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			     int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	struct io_uring_sqe *sqe;

	sqe = ch->u.uring.queue_ios ? NULL : io_uring_get_sqe(&ch->u.uring.io_ring);
	if (!sqe) {
		SPDK_DEBUGLOG(fsdev_aio, "No SQE available, IO queued for later\n");
		ch->u.uring.queue_ios = 1;
		return -EAGAIN;
	}

	io_uring_prep_readv(sqe, fd, iovs, iovcnt, offs);
	io_uring_sqe_set_data(sqe, vfsdev_io);

	TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);
	ch->u.uring.io_count++;

	return 0;
}
#else
#define fsdev_aio_prep_read_io_uring(...)  ({ assert(0); 0; })
#endif

static int
fsdev_aio_op_read(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.read.size;
	uint64_t offs = fsdev_io->u_in.read.offs;
	uint32_t flags = fsdev_io->u_in.read.flags;
	struct iovec *iovs = fsdev_io->u_in.read.iov;
	uint32_t iovcnt = fsdev_io->u_in.read.iovcnt;

	/* we don't suport the memory domains at the moment */
	assert(!fsdev_io->u_in.read.opts || !fsdev_io->u_in.read.opts->memory_domain);

	UNUSED(size);
	UNUSED(flags);
	UNUSED(size);

	if (!iovs || !iovcnt) {
		SPDK_ERRLOG("bad outvec: iov=%p outcnt=%" PRIu32 "\n", iovs, iovcnt);
		return -EINVAL;
	}

	if (vfsdev->opts.skip_rw) {
		uint32_t i;

		fsdev_io->u_out.read.data_size = 0;
		vfsdev_io->status = 0;

		for (i = 0; i < iovcnt; i++, iovs++) {
			fsdev_io->u_out.read.data_size += iovs->iov_len;
		}

		TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);

		return IO_STATUS_ASYNC;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.read.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "read: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	vfsdev_io->data_size = 0;

	if (aio_fsdev_use_io_uring_rdwr()) {
		if (fsdev_aio_prep_read_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs) == -EAGAIN) {
			TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
		}
	} else {
		fsdev_aio_prep_read_aio(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
	}

	return IO_STATUS_ASYNC;
}

static int
clear_suid_sgid(struct aio_fsdev_io *vfsdev_io)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	mode_t new_mode;
	int fd, error;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.write.fobject);
	if (!fobject) {
		return 0;
	}

	new_mode = fobject->mode & ~S_ISUID;
	if (fobject->mode & S_IXGRP) {
		new_mode &= ~S_ISGID;
	}

	if (fobject->mode == new_mode) {
		error = 0;
		goto out;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		error = -errno;
		goto out;
	}

	error = fchmod(fd, new_mode);
	if (error == -1) {
		error = -errno;
		SPDK_ERRLOG("Failed to fchmod(%d, %o) with err=%d\n", fd, new_mode, error);
	}
	close(fd);

out:
	file_object_unref(fobject, 1);
	if (error != 0) {
		SPDK_ERRLOG("Failed to clear suid/sgid on successfull "
			    "write with err=%d - ignoriing\n", error);
	}
	return error;
}

static inline void
fsdev_aio_prep_write_aio(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			 int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	io_prep_pwritev(&vfsdev_io->io, fd, iovs, iovcnt, offs);
	vfsdev_io->io.data = vfsdev_io;
	TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
}

#ifdef SPDK_CONFIG_URING
static inline int
fsdev_aio_prep_write_io_uring(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io,
			      int fd, const struct iovec *iovs, uint32_t iovcnt, uint64_t offs)
{
	struct io_uring_sqe *sqe;

	sqe = ch->u.uring.queue_ios ? NULL : io_uring_get_sqe(&ch->u.uring.io_ring);
	if (!sqe) {
		SPDK_DEBUGLOG(fsdev_aio, "No SQE available, IO queued for later\n");
		ch->u.uring.queue_ios = 1;
		return -EAGAIN;
	}

	io_uring_prep_writev(sqe, fd, iovs, iovcnt, offs);
	io_uring_sqe_set_data(sqe, vfsdev_io);

	TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);
	ch->u.uring.io_count++;

	return 0;
}
#else
#define fsdev_aio_prep_write_io_uring(...) ({ assert(0); 0; })
#endif

static int
fsdev_aio_op_write(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io = fsdev_to_aio_io(fsdev_io);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.write.size;
	uint64_t offs = fsdev_io->u_in.write.offs;
	uint32_t flags = fsdev_io->u_in.write.flags;
	const struct iovec *iovs = fsdev_io->u_in.write.iov;
	uint32_t iovcnt =  fsdev_io->u_in.write.iovcnt;

	/* we don't suport the memory domains at the moment */
	assert(!fsdev_io->u_in.write.opts || !fsdev_io->u_in.write.opts->memory_domain);

	UNUSED(size);
	UNUSED(flags);
	UNUSED(size);

	if (!iovcnt || !iovs) { /* there should be at least one iovec with data */
		SPDK_ERRLOG("bad invec: iov=%p cnt=%" PRIu32 "\n", iovs, iovcnt);
		return -EINVAL;
	}

	if (vfsdev->opts.skip_rw) {
		uint32_t i;

		fsdev_io->u_out.write.data_size = 0;
		vfsdev_io->status = 0;

		for (i = 0; i < iovcnt; i++, iovs++) {
			fsdev_io->u_out.write.data_size += iovs->iov_len;
		}

		TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);

		return IO_STATUS_ASYNC;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.write.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "write: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	vfsdev_io->data_size = 0;

	if (aio_fsdev_use_io_uring_rdwr()) {
		if (fsdev_aio_prep_write_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs) == -EAGAIN) {
			TAILQ_INSERT_TAIL(&ch->ios_for_submit, vfsdev_io, link);
		}
	} else {
		fsdev_aio_prep_write_aio(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
	}

	return IO_STATUS_ASYNC;
}

static void
fsdev_aio_op_readlink_free(void *cb_arg)
{
	free(cb_arg);
}

static int
fsdev_aio_op_readlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	char *buf;
	struct aio_fsdev_file_object *fobject;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.readlink.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	buf = malloc(PATH_MAX + 1);
	if (!buf) {
		SPDK_ERRLOG("malloc(%zu) failed\n", (size_t)(PATH_MAX + 1));
		res = -ENOMEM;
		goto alloc_failed;
	}

	res = readlinkat(fobject->fd, "", buf, PATH_MAX + 1);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("readlinkat failed for " FOBJECT_FMT " with %d\n",
			    FOBJECT_ARGS(fobject), res);
		goto fop_failed;
	}

	if (((uint32_t)res) == PATH_MAX + 1) {
		SPDK_ERRLOG("buffer is too short\n");
		res = -ENAMETOOLONG;
		goto fop_failed;
	}

	buf[res] = 0;
	fsdev_io->u_out.readlink.linkname = buf;
	spdk_fsdev_io_set_cleanup_callback(fsdev_io, fsdev_aio_op_readlink_free, buf);
	buf = NULL;
	res = 0;

fop_failed:
	free(buf);
alloc_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_statfs(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct statvfs stbuf;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.statfs.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	res = fstatvfs(fobject->fd, &stbuf);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("fstatvfs failed with %d\n", res);
		goto fop_failed;
	}

	fsdev_io->u_out.statfs.statfs.blocks = stbuf.f_blocks;
	fsdev_io->u_out.statfs.statfs.bfree = stbuf.f_bfree;
	fsdev_io->u_out.statfs.statfs.bavail = stbuf.f_bavail;
	fsdev_io->u_out.statfs.statfs.files = stbuf.f_files;
	fsdev_io->u_out.statfs.statfs.ffree = stbuf.f_ffree;
	fsdev_io->u_out.statfs.statfs.bsize = stbuf.f_bsize;
	fsdev_io->u_out.statfs.statfs.namelen = stbuf.f_namemax;
	fsdev_io->u_out.statfs.statfs.frsize = stbuf.f_frsize;

	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_mknod_symlink(struct spdk_fsdev_io *fsdev_io,
			struct aio_fsdev_file_object *parent_fobject,
			const char *name, mode_t mode, dev_t rdev, const char *link, uid_t euid, gid_t egid,
			uint32_t umask, struct aio_fsdev_file_object **pfobject, struct spdk_fsdev_file_attr *attr)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	int saverr;
	struct fsdev_aio_cred old_cred, new_cred = {
		.euid = euid,
		.egid = egid,
	};

	assert(parent_fobject != NULL);

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s isn'h safe\n", name);
		return -EINVAL;
	}

	res = fsdev_aio_change_cred(&new_cred, &old_cred);
	if (res) {
		SPDK_ERRLOG("cannot change cred (err=%d)\n", res);
		return res;
	}

	if (S_ISDIR(mode)) {
		res = mkdirat(parent_fobject->fd, name, (mode & ~umask));
	} else if (S_ISLNK(mode)) {
		if (link) {
			res = symlinkat(link, parent_fobject->fd, name);
		} else {
			SPDK_ERRLOG("NULL link pointer\n");
			errno = EINVAL;
		}
	} else if (S_ISFIFO(mode)) {
		res = mkfifoat(parent_fobject->fd, name, (mode & ~umask));
	} else {
		res = mknodat(parent_fobject->fd, name, (mode & ~umask), rdev);
	}
	saverr = -errno;

	fsdev_aio_restore_cred(&old_cred);

	if (res == -1) {
		SPDK_ERRLOG("cannot mkdirat/symlinkat/mknodat (err=%d)\n", saverr);
		return saverr;
	}

	res = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, pfobject, attr);
	if (res) {
		SPDK_ERRLOG("lookup failed (err=%d)\n", res);
		return res;
	}
	assert(*pfobject != NULL);
	/*
	 * Fixup the mode, functions creating files above ignore some bits important
	 * for POSIX compliance.
	 */
	if (!S_ISLNK(mode)) {
		res = fsdev_fchmodat(vfsdev, *pfobject, (mode & ~umask));
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("fsdev_aio_mknod_symlink mode fixup failed with %d\n", res);
			file_object_unref(*pfobject, 1);
			return res;
		}
		attr->mode = (mode & ~umask);
	}

	SPDK_DEBUGLOG(fsdev_aio, "fsdev_aio_mknod_symlink(%s " FOBJECT_FMT ") -> " FOBJECT_FMT ")\n",
		      name, FOBJECT_ARGS(parent_fobject), FOBJECT_ARGS(*pfobject));

	return 0;
}

static int
fsdev_aio_op_mknod(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_io->u_in.mknod.name;
	mode_t mode = fsdev_io->u_in.mknod.mode;
	uint32_t umask = fsdev_io->u_in.mknod.umask;
	dev_t rdev = fsdev_io->u_in.mknod.rdev;
	uid_t euid = fsdev_io->u_in.mknod.euid;
	gid_t egid = fsdev_io->u_in.mknod.egid;
	struct aio_fsdev_file_object *fobject = NULL;
	int rc;

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.mknod.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	rc = fsdev_aio_mknod_symlink(fsdev_io, parent_fobject, name, mode, rdev, NULL, euid, egid,
				     umask, &fobject, &fsdev_io->u_out.mknod.attr);
	if (!rc) {
		assert(fobject);
		fsdev_io->u_out.mknod.fobject = fsdev_aio_get_spdk_fobject(vfsdev, fobject);
	}

	file_object_unref(parent_fobject, 1);
	return rc;
}

static int
fsdev_aio_op_mkdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_io->u_in.mkdir.name;
	mode_t mode = fsdev_io->u_in.mkdir.mode;
	uint32_t umask = fsdev_io->u_in.mkdir.umask;
	uid_t euid = fsdev_io->u_in.mkdir.euid;
	gid_t egid = fsdev_io->u_in.mkdir.egid;
	struct aio_fsdev_file_object *fobject = NULL;
	int rc;

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.mkdir.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	rc = fsdev_aio_mknod_symlink(fsdev_io, parent_fobject, name, S_IFDIR | mode, 0, NULL, euid, egid,
				     umask, &fobject, &fsdev_io->u_out.mkdir.attr);
	if (!rc) {
		fsdev_io->u_out.mkdir.fobject = fsdev_aio_get_spdk_fobject(vfsdev, fobject);
	}
	file_object_unref(parent_fobject, 1);
	return rc;
}

static int
fsdev_aio_op_symlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *target = fsdev_io->u_in.symlink.target;
	const char *linkpath = fsdev_io->u_in.symlink.linkpath;
	uid_t euid = fsdev_io->u_in.symlink.euid;
	gid_t egid = fsdev_io->u_in.symlink.egid;
	struct aio_fsdev_file_object *fobject = NULL;
	int rc;

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.symlink.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	rc = fsdev_aio_mknod_symlink(fsdev_io, parent_fobject, target, S_IFLNK, 0, linkpath, euid, egid,
				     0, &fobject, &fsdev_io->u_out.symlink.attr);
	if (!rc) {
		fsdev_io->u_out.symlink.fobject = fsdev_aio_get_spdk_fobject(vfsdev, fobject);
	}
	file_object_unref(parent_fobject, 1);
	return rc;
}

static int
fsdev_aio_do_unlink(struct aio_fsdev *vfsdev, struct aio_fsdev_file_object *parent_fobject,
		    const char *name, bool is_dir)
{
	/* fobject must be initialized to avoid a scan-build false positive */
	struct aio_fsdev_file_object *fobject = NULL;
	int res;

	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s isn't safe\n", name);
		return -EINVAL;
	}

	res = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &fobject, NULL);
	if (res) {
		SPDK_ERRLOG("can't find '%s' under " FOBJECT_FMT "\n", name, FOBJECT_ARGS(parent_fobject));
		return -EIO;
	}

	res = unlinkat(parent_fobject->fd, name, is_dir ? AT_REMOVEDIR : 0);
	if (res) {
		res = -errno;
		SPDK_WARNLOG("unlinkat(" FOBJECT_FMT " %s) failed (err=%d)\n",
			     FOBJECT_ARGS(parent_fobject), name, res);
	}

	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_unlink(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_io->u_in.unlink.name;
	int res;

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.unlink.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	res = fsdev_aio_do_unlink(vfsdev, parent_fobject, name, false);
	file_object_unref(parent_fobject, 1);
	return res;
}

static int
fsdev_aio_op_rmdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_io->u_in.rmdir.name;
	int res;

	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.rmdir.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", parent_fobject);
		return -EINVAL;
	}

	res = fsdev_aio_do_unlink(vfsdev, parent_fobject, name, true);
	file_object_unref(parent_fobject, 1);
	return res;
}

#define RENAME2_FLAGS_MAP \
	RENAME2_FLAG(EXCHANGE)  \
	RENAME2_FLAG(NOREPLACE) \
	RENAME2_FLAG(WHITEOUT)

static uint32_t
fsdev_rename2_flags_to_posix(uint32_t flags)
{
	uint32_t result = 0;

#define RENAME2_FLAG(name) \
	if (flags & SPDK_FSDEV_RENAME_##name) { \
		result |= RENAME_##name;        \
	}

	RENAME2_FLAGS_MAP;

#undef RENAME2_FLAG

	return result;
}
static int
fsdev_aio_op_rename(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	/* old_fobject must be initialized to avoid a scan-build false positive */
	struct aio_fsdev_file_object *old_fobject = NULL;
	struct aio_fsdev_file_object *parent_fobject;
	const char *name = fsdev_io->u_in.rename.name;
	struct aio_fsdev_file_object *new_parent_fobject;
	const char *new_name = fsdev_io->u_in.rename.new_name;
	uint32_t flags = fsdev_io->u_in.rename.flags;

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("name '%s' isn't safe\n", name);
		return -EINVAL;
	}

	if (!is_safe_path_component(new_name)) {
		SPDK_ERRLOG("newname '%s' isn't safe\n", new_name);
		return -EINVAL;
	}


	parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.rename.parent_fobject);
	if (!parent_fobject) {
		SPDK_ERRLOG("Invalid parent_fobject\n");
		return -EINVAL;
	}

	new_parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.rename.new_parent_fobject);
	if (!new_parent_fobject) {
		SPDK_ERRLOG("Invalid new_parent_fobject\n");
		res = -EINVAL;
		goto bad_new_parent_fobject;
	}

	res = fsdev_aio_do_lookup(vfsdev, parent_fobject, name, &old_fobject, NULL);
	if (res) {
		SPDK_ERRLOG("can't find '%s' under " FOBJECT_FMT "\n", name, FOBJECT_ARGS(parent_fobject));
		res = -EIO;
		goto fop_failed;
	}

	if (flags) {
#ifndef SYS_renameat2
		SPDK_ERRLOG("flags are not supported\n");
		res = -ENOTSUP;
		goto fop_failed;
#else
		res = syscall(SYS_renameat2, parent_fobject->fd, name, new_parent_fobject->fd,
			      new_name, fsdev_rename2_flags_to_posix(flags));
		if (res == -1 && errno == ENOSYS) {
			SPDK_ERRLOG("SYS_renameat2 returned ENOSYS\n");
			res = -ENOSYS;
			goto fop_failed;
		} else if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("SYS_renameat2 failed (err=%d))\n", res);
			goto fop_failed;
		}
#endif
	} else {
		res = renameat(parent_fobject->fd, name, new_parent_fobject->fd, new_name);
		if (res == -1) {
			res = -errno;
			SPDK_ERRLOG("renameat failed (err=%d)\n", res);
			goto fop_failed;
		}
	}

	file_object_unref(old_fobject, 1);
	res = 0;

fop_failed:
	file_object_unref(new_parent_fobject, 1);
bad_new_parent_fobject:
	file_object_unref(parent_fobject, 1);
	return res;
}

static int
fsdev_aio_op_link(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_object *new_parent_fobject;
	const char *name = fsdev_io->u_in.link.name;
	struct aio_fsdev_file_object *link_fobject = NULL;

	if (!is_safe_path_component(name)) {
		SPDK_ERRLOG("%s is not a safe component\n", name);
		return -EINVAL;
	}

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.link.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	new_parent_fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.link.new_parent_fobject);
	if (!new_parent_fobject) {
		SPDK_ERRLOG("Invalid new_parent_fobject: %p\n", new_parent_fobject);
		res = -EINVAL;
		goto bad_new_parent_fobject;
	}

	res = linkat(fobject->fd, "", new_parent_fobject->fd, name, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("linkat failed " FOBJECT_FMT " -> " FOBJECT_FMT " name=%s (err=%d)\n",
			    FOBJECT_ARGS(fobject), FOBJECT_ARGS(new_parent_fobject), name, res);
		goto fop_failed;
	}

	res = fsdev_aio_do_lookup(vfsdev, new_parent_fobject, name, &link_fobject,
				  &fsdev_io->u_out.link.attr);
	if (res) {
		SPDK_ERRLOG("lookup failed (err=%d)\n", res);
		goto fop_failed;
	}

	assert(link_fobject);
	fsdev_io->u_out.link.fobject = fsdev_aio_get_spdk_fobject(vfsdev, link_fobject);

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "LINK succeeded for " FOBJECT_FMT " -> " FOBJECT_FMT " name=%s\n",
		      FOBJECT_ARGS(fobject), FOBJECT_ARGS(link_fobject), name);

fop_failed:
	file_object_unref(new_parent_fobject, 1);
bad_new_parent_fobject:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_fsync(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, saverr, fd;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	bool datasync = fsdev_io->u_in.fsync.datasync;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.fsync.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.fsync.fhandle);

	if (!fhandle) {
		fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
		res = -errno;
		if (fd == -1) {
			SPDK_ERRLOG("openat failed (errno=%d)\n", res);
			goto fop_failed;
		}
	} else {
		fd = fhandle->fd;
	}

	if (datasync) {
		res = fdatasync(fd);
	} else {
		res = fsync(fd);
	}

	saverr = -errno;
	if (!fhandle) {
		close(fd);
	}

	if (res == -1) {
		res = saverr;
		SPDK_ERRLOG("fdatasync/fsync failed for " FOBJECT_FMT " fh=%p (err=%d)\n",
			    FOBJECT_ARGS(fobject), fhandle, res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio, "FSYNC succeeded for " FOBJECT_FMT " fh=%p\n",
		      FOBJECT_ARGS(fobject), fhandle);

	res = 0;

fop_failed:
	file_object_unref(fobject, 1);

	return res;
}

#define XATTR_FLAGS_MAP \
	XATTR_FLAG(XATTR_CREATE) \
	XATTR_FLAG(XATTR_REPLACE)

static uint32_t
fsdev_xattr_flags_to_posix(uint64_t flags)
{
	uint32_t result = 0;

#define XATTR_FLAG(name) \
	if (flags & SPDK_FSDEV_##name) { \
		result |= name;          \
	}

	XATTR_FLAGS_MAP;

#undef XATTR_FLAG

	return result;
}

static int
fsdev_aio_op_setxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	const char *name = fsdev_io->u_in.setxattr.name;
	const char *value = fsdev_io->u_in.setxattr.value;
	uint32_t size = fsdev_io->u_in.setxattr.size;
	uint64_t flags = fsdev_io->u_in.setxattr.flags;
	static const char *acl_access_name = "system.posix_acl_access";

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.setxattr.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	res = fsetxattr(fd, name, value, size, fsdev_xattr_flags_to_posix(flags));
	if (res == -1) {
		res = -errno;
		if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "fsetxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("fsetxattr failed with errno=%d\n", res);
		}
		goto fop_failed;
	}

	/* Clear SGID when system.posix_acl_access is set. */
	if ((flags & SPDK_FSDEV_SETXATTR_ACL_KILL_SGID) && !strcmp(name, acl_access_name)) {
		struct spdk_fsdev_file_attr st = {};
		mode_t new_mode;

		res = file_object_fill_attr(fobject, &st);
		if (res) {
			SPDK_ERRLOG("Failed to get file attrs for cleaning SGID on behalf of changed "
				    "\"%s\" with error=%d - ignoring.\n", acl_access_name, res);
			goto fop_failed;
		}

		new_mode = st.mode & ~S_ISGID;
		res = fchmod(fobject->fd, new_mode);
		if (res == -1) {
			SPDK_WARNLOG("Failed to clean SGID on behalf of changed '%s' with errno=%d - ignoring.\n",
				     acl_access_name, -errno);
		}
	}

	res = 0;

	SPDK_DEBUGLOG(fsdev_aio,
		      "SETXATTR succeeded for " FOBJECT_FMT " name=%s value=%s size=%" PRIu32 " flags=0x%lx" PRIx64 "\n",
		      FOBJECT_ARGS(fobject), name, value, size, flags);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_getxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	const char *name = fsdev_io->u_in.getxattr.name;
	void *buffer = fsdev_io->u_in.getxattr.buffer;
	size_t size = fsdev_io->u_in.getxattr.size;
	ssize_t value_size;

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.getxattr.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	value_size = fgetxattr(fd, name, buffer, size);
	if (value_size == -1) {
		res = -errno;
		if (res == -ENODATA) {
			SPDK_INFOLOG(fsdev_aio, "getxattr: no extended attribute '%s' found\n", name);
		} else if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "getxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("getxattr failed with errno=%d\n", res);
		}

		goto fop_failed;
	}

	fsdev_io->u_out.getxattr.value_size = value_size;

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio,
		      "GETXATTR succeeded for " FOBJECT_FMT " name=%s value=%s value_size=%zd\n",
		      FOBJECT_ARGS(fobject), name, (char *)buffer, value_size);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_listxattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t data_size;
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	char *buffer = fsdev_io->u_in.listxattr.buffer;
	size_t size = fsdev_io->u_in.listxattr.size;

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.listxattr.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	data_size = flistxattr(fd, buffer, size);
	if (data_size == -1) {
		res = -errno;
		if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "listxattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("listxattr failed with errno=%d\n", res);
		}
		goto fop_failed;
	}

	fsdev_io->u_out.listxattr.data_size = data_size;
	fsdev_io->u_out.listxattr.size_only = (size == 0);

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "LISTXATTR succeeded for " FOBJECT_FMT " data_size=%zu\n",
		      FOBJECT_ARGS(fobject), data_size);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_removexattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res, fd;
	struct aio_fsdev_file_object *fobject;
	const char *name = fsdev_io->u_in.removexattr.name;

	if (!vfsdev->opts.xattr_enabled) {
		SPDK_INFOLOG(fsdev_aio, "xattr is disabled by config\n");
		return -ENOSYS;
	}

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.removexattr.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fd = openat(vfsdev->proc_self_fd, fobject->fd_str, O_RDWR);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("openat failed (errno=%d)\n", -res);
		goto fop_failed;
	}

	res = fremovexattr(fd, name);
	if (res == -1) {
		res = -errno;
		if (res == -ENODATA) {
			SPDK_INFOLOG(fsdev_aio, "removexattr: no extended attribute '%s' found\n", name);
		} else if (res == -ENOTSUP) {
			SPDK_INFOLOG(fsdev_aio, "removexattr: extended attributes are not supported or disabled\n");
		} else {
			SPDK_ERRLOG("removexattr failed with errno=%d\n", res);
		}
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "REMOVEXATTR succeeded for " FOBJECT_FMT " name=%s\n",
		      FOBJECT_ARGS(fobject), name);

fop_failed:
	if (fd != -1) {
		close(fd);
	}
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_fsyncdir(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	bool datasync = fsdev_io->u_in.fsyncdir.datasync;

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.fsyncdir.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.fsyncdir.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	if (datasync) {
		res = fdatasync(fhandle->fd);
	} else {
		res = fsync(fhandle->fd);
	}

	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("%s failed for fh=%p with err=%d\n",
			    datasync ? "fdatasync" : "fsync", fhandle, res);
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "FSYNCDIR succeeded for " FOBJECT_FMT " fh=%p datasync=%d\n",
		      FOBJECT_ARGS(fobject), fhandle, datasync);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_flock(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	int operation;

	switch (fsdev_io->u_in.flock.operation) {
	case SPDK_FSDEV_LOCK_SH:
		operation = LOCK_SH;
		break;
	case SPDK_FSDEV_LOCK_EX:
		operation = LOCK_EX;
		break;
	case SPDK_FSDEV_LOCK_UN:
		operation = LOCK_UN;
		break;
	default:
		SPDK_ERRLOG("Invalid flock operation type %d\n",
			    fsdev_io->u_in.flock.operation);
		return -EINVAL;
	}

	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.flock.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.flock.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	res = flock(fhandle->fd, operation | LOCK_NB);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("flock failed for fh=%p with err=%d\n", fhandle, res);
		goto fop_failed;
	}

	res = 0;
	SPDK_DEBUGLOG(fsdev_aio, "FLOCK succeeded for " FOBJECT_FMT " fh=%p operation=%d\n",
		      FOBJECT_ARGS(fobject), fhandle, operation);

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

#define FALLOC_FLAGS_MAP \
	FALLOC_FLAG(FL_KEEP_SIZE)      \
	FALLOC_FLAG(FL_PUNCH_HOLE)     \
	FALLOC_FLAG(FL_NO_HIDE_STALE)  \
	FALLOC_FLAG(FL_COLLAPSE_RANGE) \
	FALLOC_FLAG(FL_ZERO_RANGE)     \
	FALLOC_FLAG(FL_INSERT_RANGE)   \
	FALLOC_FLAG(FL_UNSHARE_RANGE)

static uint32_t
fsdev_falloc_flags_to_posix(uint32_t flags)
{
	uint32_t result = 0;

#define FALLOC_FLAG(name) \
	if (flags & SPDK_FSDEV_FALLOC_##name) { \
		result |= FALLOC_##name;        \
	}

	FALLOC_FLAGS_MAP;

#undef FALLOC_FLAG

	return result;
}

static int
fsdev_aio_do_fallocate(struct aio_fsdev_file_handle *fhandle, uint32_t mode,
		       uint64_t offset, uint64_t length)
{
	int res;

#ifdef __linux__
	res = fallocate(fhandle->fd, mode, offset, length);

	/* Standard errno-based error handling. */
	if (res == -1) {
		res = -errno;
	}
#else
	res = posix_fallocate(fhandle->fd, offset, length);

	/*
	 * posix_fallocate() returns positive error without
	 * setting errno.
	 */
	if (res) {
		res = -res;
	}
#endif
	return res;
}

static int
fsdev_aio_op_fallocate(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	int res;
	struct aio_fsdev_file_object *fobject;
	struct aio_fsdev_file_handle *fhandle;
	uint32_t mode = fsdev_io->u_in.fallocate.mode;
	uint64_t offset  = fsdev_io->u_in.fallocate.offset;
	uint64_t length = fsdev_io->u_in.fallocate.length;

#ifndef __linux__
	if (mode) {
		SPDK_ERRLOG("non-zero mode is not suppored\n");
		return -EINVAL;
	}
#endif
	fobject = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.fallocate.fobject);
	if (!fobject) {
		SPDK_ERRLOG("Invalid fobject: %p\n", fobject);
		return -EINVAL;
	}

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.fallocate.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		res = -EINVAL;
		goto fop_failed;
	}

	mode = fsdev_falloc_flags_to_posix(mode);
	res = fsdev_aio_do_fallocate(fhandle, mode, offset, length);
	if (res) {
		SPDK_ERRLOG("fallocate failed for fh=%p with err=%d\n",
			    fhandle, res);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "FALLOCATE returns %d for " FOBJECT_FMT " fh=%p offset=%" PRIu64 " length=%" PRIu64 "\n",
		      res, FOBJECT_ARGS(fobject), fhandle, offset, length);
	res = 0;

fop_failed:
	file_object_unref(fobject, 1);
	return res;
}

static int
fsdev_aio_op_copy_file_range(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
#ifdef SPDK_CONFIG_COPY_FILE_RANGE
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	ssize_t res;
	int saverr = 0;
	struct aio_fsdev_file_object *fobject_in;
	struct aio_fsdev_file_handle *fhandle_in;
	off_t off_in = fsdev_io->u_in.copy_file_range.off_in;
	struct aio_fsdev_file_object *fobject_out;
	struct aio_fsdev_file_handle *fhandle_out;
	off_t off_out = fsdev_io->u_in.copy_file_range.off_out;
	size_t len = fsdev_io->u_in.copy_file_range.len;
	uint32_t flags = fsdev_io->u_in.copy_file_range.flags;

	if (vfsdev->opts.disable_copy_file_range) {
		SPDK_ERRLOG("copy_file_range is disabled by config\n");
		return -ENOSYS;
	}

	fobject_in = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.copy_file_range.fobject_in);
	if (!fobject_in) {
		SPDK_ERRLOG("Invalid fobject_in\n");
		return -EINVAL;
	}

	fhandle_in = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.copy_file_range.fhandle_in);
	if (!fhandle_in) {
		SPDK_ERRLOG("Invalid fhandle_in: %p\n", fhandle_in);
		res = -EINVAL;
		goto bad_fobject_in;
	}

	fobject_out = fsdev_aio_get_fobject(vfsdev, fsdev_io->u_in.copy_file_range.fobject_out);
	if (!fobject_out) {
		SPDK_ERRLOG("Invalid fobject_out\n");
		res = -EINVAL;
		goto bad_fobject_in;
	}

	fhandle_out = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.copy_file_range.fhandle_out);
	if (!fhandle_out) {
		SPDK_ERRLOG("Invalid fhandle_out: %p\n", fhandle_out);
		res = -EINVAL;
		goto fop_failed;
	}

	res = copy_file_range(fhandle_in->fd, &off_in, fhandle_out->fd, &off_out, len, flags);
	if (res < 0) {
		res = -errno;
		SPDK_ERRLOG("copy_file_range failed with err=%d\n", saverr);
		goto fop_failed;
	}

	SPDK_DEBUGLOG(fsdev_aio,
		      "COPY_FILE_RANGE returned %zd for " FOBJECT_FMT " fh=%p offset=%" PRIu64 " -> " FOBJECT_FMT
		      " fh=%p offset=%" PRIu64 " (len=%zu flags=0x%" PRIx32 ")\n",
		      res, FOBJECT_ARGS(fobject_in), fhandle_in, (uint64_t)off_in, FOBJECT_ARGS(fobject_out), fhandle_out,
		      (uint64_t)off_out, len, flags);

	fsdev_io->u_out.copy_file_range.data_size = res;
	res = 0;

fop_failed:
	file_object_unref(fobject_out, 1);
bad_fobject_in:
	file_object_unref(fobject_in, 1);
	return res;
#else
	return -ENOSYS;
#endif
}

static void
fsdev_aio_op_abort_aio(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	int res;
	struct io_event result;

	res = io_cancel(ch->u.io_ctx, &vfsdev_io->io, &result);
	if (res) {
		TAILQ_REMOVE(&ch->ios_in_progress, vfsdev_io, link);
		SPDK_DEBUGLOG(fsdev_aio, "aio=%p cancelled\n", vfsdev_io);
		fsdev_aio_cb(vfsdev_io, ECANCELED, 0);
	} else {
		SPDK_WARNLOG("aio=%p cancellation failed with err=%d\n", vfsdev_io, res);
	}
}

#ifdef SPDK_CONFIG_URING
static void
fsdev_aio_op_abort_io_uring(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	/* I/O abortion is optional - we do our best to abort the I/O when possible. */
#ifdef IORING_ASYNC_CANCEL_USERDATA
	int res;
	struct io_uring_sqe *sqe;

	if (!g_io_uring_supported_ops[IORING_OP_ASYNC_CANCEL]) {
		SPDK_DEBUGLOG(fsdev_aio, "IORING_OP_ASYNC_CANCEL is not supported by kernel\n");
		return;
	}

	sqe = io_uring_get_sqe(&ch->u.uring.io_ring);
	if (sqe == NULL) {
		SPDK_WARNLOG("No SQE available for cancellation\n");
		return;
	}

	io_uring_prep_cancel(sqe, vfsdev_io, IORING_ASYNC_CANCEL_USERDATA);
	io_uring_sqe_set_data(sqe, NULL);
	res = io_uring_submit(&ch->u.uring.io_ring);
	if (res > 0) {
		SPDK_DEBUGLOG(fsdev_aio, "Cancellation submitted\n");
	} else {
		SPDK_WARNLOG("Cancellation submission failed with err=%d\n", res);
	}
#else
	SPDK_INFOLOG(fsdev_aio, "IORING_OP_ASYNC_CANCEL is not supported\n");
#endif
}
#else
#define fsdev_aio_op_abort_io_uring(ch, vfsdev_io) assert(0)
#endif

static void
fsdev_aio_op_abort_io(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	if (aio_fsdev_use_io_uring_rdwr()) {
		fsdev_aio_op_abort_io_uring(ch, vfsdev_io);
	} else {
		fsdev_aio_op_abort_aio(ch, vfsdev_io);
	}
}


static int
fsdev_aio_op_abort(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct aio_fsdev_io *vfsdev_io, *tmp;
	uint64_t unique_to_abort = fsdev_io->u_in.abort.unique_to_abort;

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_for_submit, link, tmp) {
		struct spdk_fsdev_io *_fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev != _fsdev_io->fsdev) {
			continue;
		}

		if (spdk_fsdev_io_get_unique(_fsdev_io) == unique_to_abort) {
			TAILQ_REMOVE(&ch->ios_for_submit, vfsdev_io, link);
			spdk_fsdev_io_complete(fsdev_io, -ECANCELED);
			return 0; /* we found the IO to abort, no need to continue */
		}
	}

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_in_progress, link, tmp) {
		struct spdk_fsdev_io *_fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev != _fsdev_io->fsdev) {
			continue;
		}

		if (spdk_fsdev_io_get_unique(_fsdev_io) == unique_to_abort) {
			fsdev_aio_op_abort_io(ch, vfsdev_io);
			return 0; /* we found the IO to abort, no need to continue */
		}
	}

	return 0;
}

static int
aio_io_poll_common(struct aio_io_channel *ch)
{
	int res = SPDK_POLLER_IDLE;
	struct aio_fsdev_io *vfsdev_io, *tmp;
	TAILQ_HEAD(aio_tmp_list, aio_fsdev_io) ios = TAILQ_HEAD_INITIALIZER(ios);

	TAILQ_SWAP(&ch->ios_to_complete, &ios, aio_fsdev_io, link);
	TAILQ_FOREACH_SAFE(vfsdev_io, &ios, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		TAILQ_REMOVE(&ios, vfsdev_io, link);
		spdk_fsdev_io_complete(fsdev_io, vfsdev_io->status);
		res = SPDK_POLLER_BUSY;
	}

	return res;
}

static int
aio_io_poll_aio(void *arg)
{
	struct aio_io_channel *ch = arg;
	struct aio_fsdev_io *vfsdev_io, *tmp;
	TAILQ_HEAD(aio_tmp_list, aio_fsdev_io) ios = TAILQ_HEAD_INITIALIZER(ios);
	struct io_event events[32];
	struct iocb *iocbs[32];
	long to_submit = 0;
	int i, rc, res;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	rc = io_getevents(ch->u.io_ctx, 0, 32, events, &timeout);
	if (rc < 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		rc = 0;
	}

	for (i = 0; i < rc; i++) {
		fsdev_aio_cb(events[i].data, events[i].res, events[i].res2);
	}

	res = rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;

	TAILQ_SWAP(&ch->ios_for_submit, &ios, aio_fsdev_io, link);
	TAILQ_FOREACH_SAFE(vfsdev_io, &ios, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
		enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);

		rc = -EOPNOTSUPP;
		res = SPDK_POLLER_BUSY;

		switch (type) {
		case SPDK_FSDEV_IO_READ:
		case SPDK_FSDEV_IO_WRITE:
			iocbs[to_submit++] = &vfsdev_io->io;
			rc = IO_STATUS_ASYNC;
			break;
		case SPDK_FSDEV_IO_POLL:
			TAILQ_REMOVE(&ios, vfsdev_io, link);
			rc = fsdev_aio_do_poll(ch, fsdev_io);
			break;
		case SPDK_FSDEV_IO_SETLK:
			TAILQ_REMOVE(&ios, vfsdev_io, link);
			rc = fsdev_aio_do_setlk(ch, fsdev_io);
			break;
		default:
			TAILQ_REMOVE(&ios, vfsdev_io, link);
			break;
		}

		if (rc != IO_STATUS_ASYNC) {
			vfsdev_io->status = rc;
			TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);
		}

		if (to_submit == SPDK_COUNTOF(iocbs)) {
			break;
		}
	}

	if (to_submit > 0) {
		rc = io_submit(ch->u.io_ctx, to_submit, iocbs);
		if (rc < 0) {
			res = rc;
			rc = 0; /* 0 were submitted. This will get them all queued back up. */

			if (res != -EAGAIN) {
				/* The failures typically apply to the first iocb. Fail that one, but let the others
				 * queue back up to be resubmitted. */
				SPDK_ERRLOG("Failed io_submit: %s (%d)\n", spdk_strerror(-res), res);
				vfsdev_io = TAILQ_FIRST(&ios);
				TAILQ_REMOVE(&ios, vfsdev_io, link);
				vfsdev_io->status = res;
				TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);
			}
		}

		/* For each request actually submitted, shift it into the in progress list. */
		i = 0;
		TAILQ_FOREACH_SAFE(vfsdev_io, &ios, link, tmp) {
			if (i == rc) {
				break;
			}

			assert(&vfsdev_io->io == iocbs[i]);

			TAILQ_REMOVE(&ios, vfsdev_io, link);
			TAILQ_INSERT_TAIL(&ch->ios_in_progress, vfsdev_io, link);

			i++;
		}

		/* For all remaining requests, put them back into the ios_for_submit list */
		vfsdev_io = TAILQ_LAST(&ios, aio_tmp_list);
		while (vfsdev_io != NULL) {
			TAILQ_REMOVE(&ios, vfsdev_io, link);
			TAILQ_INSERT_HEAD(&ch->ios_for_submit, vfsdev_io, link);
			vfsdev_io = TAILQ_LAST(&ios, aio_tmp_list);
		}
	}

	if (aio_io_poll_common(ch) == SPDK_POLLER_BUSY) {
		res = SPDK_POLLER_BUSY;
	}

	return res;
}

#ifdef SPDK_CONFIG_URING

static int
aio_retry_io_uring_read(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.read.size;
	uint64_t offs = fsdev_io->u_in.read.offs;
	struct iovec *iovs = fsdev_io->u_in.read.iov;
	uint32_t iovcnt = fsdev_io->u_in.read.iovcnt;

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.read.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	UNUSED(size);

	SPDK_DEBUGLOG(fsdev_aio, "read: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	return fsdev_aio_prep_read_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
}

static int
aio_retry_io_uring_write(struct aio_io_channel *ch, struct aio_fsdev_io *vfsdev_io)
{
	struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev_io->fsdev);
	struct aio_fsdev_file_handle *fhandle;
	size_t size = fsdev_io->u_in.write.size;
	uint64_t offs = fsdev_io->u_in.write.offs;
	const struct iovec *iovs = fsdev_io->u_in.write.iov;
	uint32_t iovcnt =  fsdev_io->u_in.write.iovcnt;

	UNUSED(size);

	fhandle = fsdev_aio_get_fhandle(vfsdev, fsdev_io->u_in.write.fhandle);
	if (!fhandle) {
		SPDK_ERRLOG("Invalid fhandle: %p\n", fhandle);
		return -EINVAL;
	}

	SPDK_DEBUGLOG(fsdev_aio, "write: fd=%d offs=%" PRIu64 " size=%" PRIu64 " iovcnt=%" PRIu32 "\n",
		      fhandle->fd, offs, size, iovcnt);

	return fsdev_aio_prep_write_io_uring(ch, vfsdev_io, fhandle->fd, iovs, iovcnt, offs);
}

static int
aio_io_poll_io_uring(void *arg)
{
	struct aio_io_channel *ch = arg;
	int res = SPDK_POLLER_IDLE;
	unsigned cqes_count = 0;
	struct io_uring_cqe *cqe;
	unsigned head;
	TAILQ_HEAD(aio_tmp_list, aio_fsdev_io) ios = TAILQ_HEAD_INITIALIZER(ios);

	/* Handle completions */
	io_uring_for_each_cqe(&ch->u.uring.io_ring, head, cqe) {
		struct aio_fsdev_io *vfsdev_io = io_uring_cqe_get_data(cqe);

#ifdef IORING_ASYNC_CANCEL_USERDATA
		if (!vfsdev_io) {
			/* This is a cancellation request */
			continue;
		}
#endif

		fsdev_aio_cb(vfsdev_io, cqe->res, (cqe->res < 0) ? cqe->res : 0);
		res = SPDK_POLLER_BUSY;

		cqes_count++;
	}

	io_uring_cq_advance(&ch->u.uring.io_ring, cqes_count);

	/* Submit more I/O */
	if (ch->u.uring.io_count) {
		res = io_uring_submit(&ch->u.uring.io_ring);
		if (res < 0) {
			SPDK_ERRLOG("io_uring_submit failed with err=%d\n", res);
		} else {
			assert((uint32_t)res <= ch->u.uring.io_count);
			ch->u.uring.io_count -= res;
		}
	}

	/* Handle submit queue */
	ch->u.uring.queue_ios = 0; /* reset the flag */
	while (!TAILQ_EMPTY(&ch->ios_for_submit)) {
		struct aio_fsdev_io *vfsdev_io = TAILQ_FIRST(&ch->ios_for_submit);
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
		enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);
		int rc = IO_STATUS_ASYNC;

		TAILQ_REMOVE(&ch->ios_for_submit, vfsdev_io, link);

		res = SPDK_POLLER_BUSY;

		switch (type) {
		case SPDK_FSDEV_IO_READ:
			/* If this or one of the previous IOs failed due to no SQE available, we need to retry it */
			if (!TAILQ_EMPTY(&ios) || aio_retry_io_uring_read(ch, vfsdev_io) == -EAGAIN) {
				TAILQ_INSERT_TAIL(&ios, vfsdev_io, link);
			}
			break;
		case SPDK_FSDEV_IO_WRITE:
			/* If this or one of the previous IOs failed due to no SQE available, we need to retry it */
			if (!TAILQ_EMPTY(&ios) || aio_retry_io_uring_write(ch, vfsdev_io) == -EAGAIN) {
				TAILQ_INSERT_TAIL(&ios, vfsdev_io, link);
			}
			break;
		case SPDK_FSDEV_IO_POLL:
			rc = fsdev_aio_do_poll(ch, fsdev_io);
			break;
		case SPDK_FSDEV_IO_SETLK:
			rc = fsdev_aio_do_setlk(ch, fsdev_io);
			break;
		default:
			SPDK_ERRLOG("Unsupported IO type: %d\n", type);
			assert(0);
			rc = -EINVAL;
			break;
		}

		if (rc != IO_STATUS_ASYNC) {
			vfsdev_io->status = rc;
			TAILQ_INSERT_TAIL(&ch->ios_to_complete, vfsdev_io, link);
		}
	}

	if (!TAILQ_EMPTY(&ios)) {
		/* The queue is not empty, so add the remaining IOs to the submit queue */
		assert(ch->u.uring.queue_ios); /* If this is the case, the flag has been set by the retry logic */
		TAILQ_CONCAT(&ch->ios_for_submit, &ios, link);
	} else {
		/* The queue is empty, so we've successfully submitted all queued IOs */
		assert(!ch->u.uring.queue_ios);
	}

	/* Handle common part of the poll */
	if (aio_io_poll_common(ch) == SPDK_POLLER_BUSY) {
		res = SPDK_POLLER_BUSY;
	}

	return res;
}
#endif

static int
aio_fsdev_create_cb(void *io_device, void *ctx_buf)
{
	struct aio_io_channel *ch = ctx_buf;
	struct spdk_thread *thread = spdk_get_thread();
	spdk_poller_fn poller_fn;
	int res;

	UNUSED(thread);

	if (!aio_fsdev_use_io_uring_rdwr()) {
		res = io_queue_init(g_opts.max_io_depth, &ch->u.io_ctx);
		if (res) {
			SPDK_ERRLOG("io_setup(%" PRIu32 ") failed with %d\n", g_opts.max_io_depth, res);
			return -ENOMEM;
		}

		poller_fn = aio_io_poll_aio;
	}
#ifdef SPDK_CONFIG_URING
	else {
		res = io_uring_queue_init(g_opts.max_io_depth, &ch->u.uring.io_ring,
					  IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER);
		if (res) {
			SPDK_ERRLOG("io_uring_queue_init(%" PRIu32 ") failed with %d\n", g_opts.max_io_depth, res);
			io_queue_release(ch->u.io_ctx);
			return -res;
		}

		poller_fn = aio_io_poll_io_uring;
	}
#endif

	TAILQ_INIT(&ch->ios_for_submit);
	TAILQ_INIT(&ch->ios_in_progress);
	TAILQ_INIT(&ch->ios_to_complete);

	ch->poller = SPDK_POLLER_REGISTER(poller_fn, ch, 0);

	UNUSED(thread);

	SPDK_DEBUGLOG(fsdev_aio,
		      "Created aio fsdev IO channel: thread %s, thread id %" PRIu64 " (io_uring=%d)\n",
		      spdk_thread_get_name(thread), spdk_thread_get_id(thread), aio_fsdev_use_io_uring_rdwr());
	return 0;
}

static void
aio_fsdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct aio_io_channel *ch = ctx_buf;
	struct spdk_thread *thread = spdk_get_thread();

	UNUSED(thread);

	spdk_poller_unregister(&ch->poller);

	assert(TAILQ_EMPTY(&ch->ios_in_progress));

	if (!aio_fsdev_use_io_uring_rdwr()) {
		io_queue_release(ch->u.io_ctx);
	}
#ifdef SPDK_CONFIG_URING
	else {
		io_uring_queue_exit(&ch->u.uring.io_ring);
	}
#endif

	SPDK_DEBUGLOG(fsdev_aio,
		      "Destroyed aio fsdev IO channel: thread %s, thread id %" PRIu64 "\n",
		      spdk_thread_get_name(thread), spdk_thread_get_id(thread));
}

static int
fsdev_aio_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fsdev_aio_set_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "max_io_depth", g_opts.max_io_depth);
	spdk_json_write_named_bool(w, "enable_io_uring", g_opts.enable_io_uring);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static int
fsdev_aio_initialize(void)
{
	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_aio_fsdev_head,
				aio_fsdev_create_cb, aio_fsdev_destroy_cb,
				sizeof(struct aio_io_channel), "aio_fsdev");

#ifdef SPDK_CONFIG_URING
	fsdev_init_supported_uring_ops();
#endif

	return 0;
}

static void
fsdev_aio_finish(void)
{
	spdk_io_device_unregister(&g_aio_fsdev_head, NULL);
}

static int
fsdev_aio_get_ctx_size(void)
{
	return sizeof(struct aio_fsdev_io);
}

static struct spdk_fsdev_module aio_fsdev_module = {
	.name = "aio",
	.config_json = fsdev_aio_config_json,
	.module_init = fsdev_aio_initialize,
	.module_fini = fsdev_aio_finish,
	.get_ctx_size	= fsdev_aio_get_ctx_size,
};

SPDK_FSDEV_MODULE_REGISTER(aio, &aio_fsdev_module);

static void
fsdev_aio_free(struct aio_fsdev *vfsdev)
{
	if (vfsdev->proc_self_fd != -1) {
		close(vfsdev->proc_self_fd);
	}

	if (vfsdev->root) {
		uint64_t refcount = file_object_unref(vfsdev->root, 1);
		assert(refcount == 0);
		UNUSED(refcount);

	}

	fsdev_aio_fanotify_close(vfsdev);

	if (vfsdev->lut) {
		spdk_lut_free(vfsdev->lut);
		spdk_spin_destroy(&vfsdev->lock);
	}

	free(vfsdev->fsdev.name);
	free(vfsdev->root_path);

	free(vfsdev);
}

static int
fsdev_aio_destruct(void *ctx)
{
	struct aio_fsdev *vfsdev = ctx;

	TAILQ_REMOVE(&g_aio_fsdev_head, vfsdev, tailq);

	fsdev_free_leafs(vfsdev->root, true);
	vfsdev->root = NULL;

	fsdev_aio_free(vfsdev);
	return 0;
}

static void
fsdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	int status;
	enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);

	assert(type >= 0 && type < __SPDK_FSDEV_IO_LAST);

	switch (type) {
	case SPDK_FSDEV_IO_MOUNT:
		status = fsdev_aio_op_mount(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_UMOUNT:
		status = fsdev_aio_op_umount(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_LOOKUP:
		status = fsdev_aio_op_lookup(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_FORGET:
		status = fsdev_aio_op_forget(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_GETATTR:
		status = fsdev_aio_op_getattr(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_SETATTR:
		status = fsdev_aio_op_setattr(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_READLINK:
		status = fsdev_aio_op_readlink(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_SYMLINK:
		status = fsdev_aio_op_symlink(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_MKNOD:
		status = fsdev_aio_op_mknod(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_MKDIR:
		status = fsdev_aio_op_mkdir(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_UNLINK:
		status = fsdev_aio_op_unlink(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_RMDIR:
		status = fsdev_aio_op_rmdir(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_RENAME:
		status = fsdev_aio_op_rename(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_LINK:
		status = fsdev_aio_op_link(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_OPEN:
		status = fsdev_aio_op_open(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_READ:
		status = fsdev_aio_op_read(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_WRITE:
		status = fsdev_aio_op_write(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_STATFS:
		status = fsdev_aio_op_statfs(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_RELEASE:
		status = fsdev_aio_op_release(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_FSYNC:
		status = fsdev_aio_op_fsync(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_SETXATTR:
		status = fsdev_aio_op_setxattr(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_GETXATTR:
		status = fsdev_aio_op_getxattr(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_LISTXATTR:
		status = fsdev_aio_op_listxattr(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_REMOVEXATTR:
		status = fsdev_aio_op_removexattr(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_FLUSH:
		status = fsdev_aio_op_flush(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_OPENDIR:
		status = fsdev_aio_op_opendir(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_READDIR:
		status = fsdev_aio_op_readdir(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_RELEASEDIR:
		status = fsdev_aio_op_releasedir(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_FSYNCDIR:
		status = fsdev_aio_op_fsyncdir(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_FLOCK:
		status = fsdev_aio_op_flock(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_CREATE:
		status = fsdev_aio_op_create(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_ABORT:
		status = fsdev_aio_op_abort(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_FALLOCATE:
		status = fsdev_aio_op_fallocate(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_COPY_FILE_RANGE:
		status = fsdev_aio_op_copy_file_range(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_SYNCFS:
		status = fsdev_aio_op_syncfs(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_LSEEK:
		status = fsdev_aio_op_lseek(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_POLL:
		status = fsdev_aio_op_poll(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_GETLK:
		status = fsdev_aio_op_getlk(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_SETLK:
		status = fsdev_aio_op_setlk(ch, fsdev_io);
		break;
	case SPDK_FSDEV_IO_IOCTL:
		status = fsdev_aio_op_ioctl(ch, fsdev_io);
		break;
	default:
		SPDK_DEBUGLOG(fsdev_aio, "Operation type %d is not implemented!\n", (int)type);
		spdk_fsdev_io_complete(fsdev_io, -ENOSYS);
		return;
	}

	if (status != IO_STATUS_ASYNC) {
		spdk_fsdev_io_complete(fsdev_io, status);
	}
}

static struct spdk_io_channel *
fsdev_aio_get_io_channel(void *ctx)
{
	/* We don't create an spdk_io_channel per aio_fsdev. Rather we share it among all the aio fsdevs. */
	return spdk_get_io_channel(&g_aio_fsdev_head);
}

static void
fsdev_aio_write_config_json(struct spdk_fsdev *fsdev, struct spdk_json_write_ctx *w)
{
	struct aio_fsdev *vfsdev = fsdev_to_aio_fsdev(fsdev);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fsdev_aio_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", spdk_fsdev_get_name(&vfsdev->fsdev));
	spdk_json_write_named_string(w, "root_path", vfsdev->root_path);
	spdk_json_write_named_bool(w, "enable_xattr", vfsdev->opts.xattr_enabled);
	spdk_json_write_named_bool(w, "enable_writeback_cache",
				   vfsdev->opts.writeback_cache_enabled);
	spdk_json_write_named_uint32(w, "max_xfer_size", vfsdev->opts.max_xfer_size);
	spdk_json_write_named_uint32(w, "max_readahead", vfsdev->opts.max_readahead);

	spdk_json_write_named_bool(w, "skip_rw", vfsdev->opts.skip_rw);
	spdk_json_write_named_bool(w, "enable_notifications", vfsdev->opts.enable_notifications);
	spdk_json_write_named_uint32(w, "attr_valid_ms", vfsdev->opts.attr_valid_ms);
	spdk_json_write_named_bool(w, "disable_copy_file_range", vfsdev->opts.disable_copy_file_range);
	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w);
}

static int
fsdev_aio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct aio_fsdev *vfsdev = ctx;

	spdk_json_write_named_string(w, "root_path", vfsdev->root_path);
	spdk_json_write_named_bool(w, "enable_xattr", vfsdev->opts.xattr_enabled);
	spdk_json_write_named_bool(w, "enable_writeback_cache",
				   vfsdev->opts.writeback_cache_enabled);
	spdk_json_write_named_uint32(w, "max_xfer_size", vfsdev->opts.max_xfer_size);
	spdk_json_write_named_uint32(w, "max_readahead", vfsdev->opts.max_readahead);
	spdk_json_write_named_bool(w, "skip_rw", vfsdev->opts.skip_rw);
	spdk_json_write_named_bool(w, "enable_notifications", vfsdev->opts.enable_notifications);
	spdk_json_write_named_uint32(w, "attr_valid_ms", vfsdev->opts.attr_valid_ms);
	spdk_json_write_named_bool(w, "disable_copy_file_range", vfsdev->opts.disable_copy_file_range);
	spdk_json_write_named_object_begin(w, "io_uring");
	spdk_json_write_named_bool(w, "rdwr", aio_fsdev_use_io_uring_rdwr());
	spdk_json_write_object_end(w);

	return 0;
}

struct fsdev_aio_reset_ctx {
	struct aio_fsdev *vfsdev;
	spdk_fsdev_reset_done_cb cb;
	void *cb_arg;
	struct spdk_poller *poller;
	bool has_outstanding_ios;
};

static void
fsdev_aio_reset_done(struct fsdev_aio_reset_ctx *ctx, int status)
{
	fsdev_free_leafs(ctx->vfsdev->root, false);

	ctx->cb(ctx->cb_arg, status);

	spdk_poller_unregister(&ctx->poller);

	free(ctx);
}

static void
fsdev_aio_reset_check_outstanding_io_msg_cb(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct aio_fsdev_io *vfsdev_io;
	bool ios_in_progress = false;

	/* Check whether some IOs remained in progress */
	TAILQ_FOREACH(vfsdev_io, &ch->ios_in_progress, link) {
		/* We only check the IOs which belong to our aio_fsdev. */
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);
		if (fsdev_io->fsdev == &ctx->vfsdev->fsdev) {
			ios_in_progress = true;
			break;
		}
	}

	if (ios_in_progress) {
		__atomic_test_and_set(&ctx->has_outstanding_ios, __ATOMIC_RELAXED);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
fsdev_aio_reset_check_outstanding_io_done_cb(struct spdk_io_channel_iter *i, int status)
{
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fsdev *fsdev = &ctx->vfsdev->fsdev;
	bool has_outstanding_ios;

	if (status) {
		SPDK_ERRLOG("%s: outstanding IOs check failed with %d\n", spdk_fsdev_get_name(fsdev), status);
		fsdev_aio_reset_done(ctx, status);
		return;
	}

	/* Get the has_outstanding_ios and reset it so the poller can set it again if resumed */
	has_outstanding_ios = __atomic_exchange_n(&ctx->has_outstanding_ios, 0, __ATOMIC_RELAXED);
	if (has_outstanding_ios) {
		/* We still have uncompleted IOs, so resume the poller */
		SPDK_DEBUGLOG(fsdev_aio, "%s: some IOs are still uncompleted\n", spdk_fsdev_get_name(fsdev));
		spdk_poller_resume(ctx->poller);
		return;
	}

	/* All IOs have been completed -> finish the reset */
	SPDK_DEBUGLOG(fsdev_aio, "%s: all IOs have been completed. Reset is done!\n",
		      spdk_fsdev_get_name(fsdev));

	fsdev_aio_reset_done(ctx, 0);
}

static int
fsdev_aio_reset_poller_cb(void *_ctx)
{
	struct fsdev_aio_reset_ctx *ctx = _ctx;

	spdk_poller_pause(ctx->poller); /* We'll pause the poller until the current check is done */

	/* Check whether all the IOs has been completed */
	spdk_for_each_channel(&g_aio_fsdev_head, fsdev_aio_reset_check_outstanding_io_msg_cb, ctx,
			      fsdev_aio_reset_check_outstanding_io_done_cb);

	return SPDK_POLLER_BUSY;
}

static void
fsdev_aio_reset_msg_cb(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct aio_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct aio_fsdev_io *vfsdev_io, *tmp;

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_for_submit, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev == &ctx->vfsdev->fsdev) {
			TAILQ_REMOVE(&ch->ios_for_submit, vfsdev_io, link);
			spdk_fsdev_io_complete(fsdev_io, -ECANCELED);
		}
	}

	TAILQ_FOREACH_SAFE(vfsdev_io, &ch->ios_in_progress, link, tmp) {
		struct spdk_fsdev_io *fsdev_io = aio_to_fsdev_io(vfsdev_io);

		if (fsdev_io->fsdev == &ctx->vfsdev->fsdev) {
			fsdev_aio_op_abort_io(ch, vfsdev_io);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
fsdev_aio_reset_done_cb(struct spdk_io_channel_iter *i, int status)
{
	struct fsdev_aio_reset_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_fsdev *fsdev = &ctx->vfsdev->fsdev;

	if (status) {
		SPDK_ERRLOG("%s: IO cancellation failed with %d\n", spdk_fsdev_get_name(fsdev), status);
		fsdev_aio_reset_done(ctx, status);
		return;
	}

	SPDK_DEBUGLOG(fsdev_aio, "%s: all the outstanding IOs have been cancelled\n",
		      spdk_fsdev_get_name(fsdev));

	/* Resume the poller, so it'll wait until the completion of all the IOs */
	spdk_poller_resume(ctx->poller);
}

static int
fsdev_aio_reset(void *_ctx, spdk_fsdev_reset_done_cb cb, void *cb_arg)
{
	struct aio_fsdev *vfsdev = _ctx;
	struct fsdev_aio_reset_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate the reset object\n");
		return -ENOMEM;
	}

	ctx->vfsdev = vfsdev;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->poller = SPDK_POLLER_REGISTER(fsdev_aio_reset_poller_cb, ctx, 0);
	if (!ctx->poller) {
		free(ctx);
		SPDK_ERRLOG("Cannot register reset poller\n");
		return -ENOMEM;
	}

	spdk_poller_pause(ctx->poller); /* We'll start it once the IOs are cancelled */

	/* First, we'll cancel all the async IOs */
	spdk_for_each_channel(&g_aio_fsdev_head, fsdev_aio_reset_msg_cb, ctx, fsdev_aio_reset_done_cb);
	return 0;
}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
static int
fsdev_aio_enable_notifications(struct aio_fsdev *vfsdev)
{
	int rc;

	vfsdev->fanotify_fd = fanotify_init(FAN_NONBLOCK | FAN_REPORT_FID | FAN_REPORT_DFID_NAME,
					    O_RDONLY | O_LARGEFILE);
	if (vfsdev->fanotify_fd == -1) {
		SPDK_ERRLOG("Failed to create fanotify, errno %d\n", errno);
		rc = -errno;
		goto err;
	}

	vfsdev->pid = getpid();
	vfsdev->fanotify_poller = SPDK_POLLER_REGISTER(fsdev_aio_fanotify_poller, vfsdev,
				  FANOTIFY_POLLER_PERIOD_US);
	if (!vfsdev->fanotify_poller) {
		SPDK_ERRLOG("Failed to create fanotify poller\n");
		rc = -ENOMEM;
		goto err;
	}

	SPDK_NOTICELOG("Started fanotify poller: fanotify fd %d\n", vfsdev->fanotify_fd);
	return 0;

err:
	fsdev_aio_fanotify_close(vfsdev);
	return rc;
}

static int
fsdev_aio_disable_notifications(struct aio_fsdev *vfsdev)
{
	fsdev_aio_fanotify_close(vfsdev);
	return 0;
}

static int
fsdev_aio_set_notifications(void *ctx, bool enabled)
{
	struct aio_fsdev *vfsdev = ctx;

	if (enabled && vfsdev->opts.enable_notifications && vfsdev->fanotify_fd == -1) {
		return fsdev_aio_enable_notifications(vfsdev);
	} else if (enabled && !vfsdev->opts.enable_notifications) {
		SPDK_ERRLOG("Notifications are disabled in fsdev_aio\n");
		return -EOPNOTSUPP;
	} else if (!enabled && vfsdev->fanotify_fd != -1) {
		return fsdev_aio_disable_notifications(vfsdev);
	}

	return 0;
}
#else
static int
fsdev_aio_set_notifications(void *ctx, bool enabled)
{
	struct aio_fsdev *vfsdev = ctx;

	if (enabled && !vfsdev->opts.enable_notifications) {
		SPDK_ERRLOG("Notifications are disabled in fsdev_aio\n");
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif

static const struct spdk_fsdev_fn_table aio_fn_table = {
	.destruct		= fsdev_aio_destruct,
	.submit_request		= fsdev_aio_submit_request,
	.get_io_channel		= fsdev_aio_get_io_channel,
	.write_config_json	= fsdev_aio_write_config_json,
	.reset			= fsdev_aio_reset,
	.dump_info_json		= fsdev_aio_dump_info_json,
	.set_notifications	= fsdev_aio_set_notifications
};

static int
setup_root(struct aio_fsdev *vfsdev)
{
	int fd, res;
	struct stat stat;

	fd = open(vfsdev->root_path, O_PATH);
	if (fd == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot open root %s (err=%d)\n", vfsdev->root_path, res);
		return res;
	}

	res = fstatat(fd, "", &stat, AT_EMPTY_PATH);
	if (res == -1) {
		res = -errno;
		SPDK_ERRLOG("Cannot get root fstatat of %s (err=%d)\n", vfsdev->root_path, res);
		close(fd);
		return res;
	}

	vfsdev->root = file_object_create_unsafe(vfsdev, NULL, fd, stat.st_ino, stat.st_dev, stat.st_mode,
			"/");
	if (!vfsdev->root) {
		SPDK_ERRLOG("Cannot alloc root\n");
		close(fd);
		return -ENOMEM;
	}

	SPDK_INFOLOG(fsdev_aio, "root (%s) fd=%d\n", vfsdev->root_path, fd);
	return 0;
}

static int
setup_proc_self_fd(struct aio_fsdev *vfsdev)
{
	vfsdev->proc_self_fd = open("/proc/self/fd", O_PATH);
	if (vfsdev->proc_self_fd == -1) {
		int saverr = -errno;
		SPDK_ERRLOG("Failed to open procfs fd dir with %d\n", saverr);
		return saverr;
	}

	SPDK_DEBUGLOG(fsdev_aio, "procfs fd dir opened (fd=%d)\n", vfsdev->proc_self_fd);
	return 0;
}

void
spdk_fsdev_aio_get_default_opts(struct spdk_fsdev_aio_opts *opts)
{
	assert(opts);

	memset(opts, 0, sizeof(*opts));

	opts->xattr_enabled = DEFAULT_XATTR_ENABLED;
	opts->writeback_cache_enabled = DEFAULT_WRITEBACK_CACHE;
	opts->max_xfer_size = DEFAULT_MAX_XFER_SIZE;
	opts->max_readahead = DEFAULT_MAX_READAHEAD;
	opts->skip_rw = DEFAULT_SKIP_RW;
	opts->enable_notifications = DEFAULT_ENABLE_NOTIFICATIONS;
	opts->attr_valid_ms = DEFAULT_ATTR_VALID_MS;
	opts->disable_copy_file_range = DEFAULT_DISABLE_COPY_FILE_RANGE;
}

int
spdk_fsdev_aio_create(struct spdk_fsdev **fsdev, const char *name, const char *root_path,
		      const struct spdk_fsdev_aio_opts *opts)
{
	struct aio_fsdev *vfsdev;
	int rc;

	vfsdev = calloc(1, sizeof(*vfsdev));
	if (!vfsdev) {
		SPDK_ERRLOG("Could not allocate aio_fsdev\n");
		return -ENOMEM;
	}

	vfsdev->proc_self_fd = -1;

	vfsdev->fsdev.name = strdup(name);
	if (!vfsdev->fsdev.name) {
		SPDK_ERRLOG("Could not strdup fsdev name: %s\n", name);
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

	vfsdev->root_path = strdup(root_path);
	if (!vfsdev->root_path) {
		SPDK_ERRLOG("Could not strdup root path: %s\n", root_path);
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

	vfsdev->lut = spdk_lut_create(FILE_PTR_LUT_INIT_SIZE, FILE_PTR_LUT_GROWTH_STEP,
				      FILE_PTR_LUT_MAX_SIZE);
	if (!vfsdev->lut) {
		SPDK_ERRLOG("Could not create lookup table\n");
		fsdev_aio_free(vfsdev);
		return -ENOMEM;
	}

#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	vfsdev->fanotify_fd = -1;
	RB_INIT(&vfsdev->linux_fhs);
#endif

	spdk_spin_init(&vfsdev->lock);

	rc = setup_root(vfsdev);
	if (rc) {
		SPDK_ERRLOG("Could not setup root: %s (err=%d)\n", root_path, rc);
		fsdev_aio_free(vfsdev);
		return rc;
	}

	rc = setup_proc_self_fd(vfsdev);
	if (rc) {
		SPDK_ERRLOG("Could not setup proc_self_fd (err=%d)\n", rc);
		fsdev_aio_free(vfsdev);
		return rc;
	}

	vfsdev->opts = *opts;
	vfsdev->fsdev.ctxt = vfsdev;
	vfsdev->fsdev.fn_table = &aio_fn_table;
	vfsdev->fsdev.module = &aio_fsdev_module;
#ifdef SPDK_CONFIG_HAVE_FANOTIFY
	if (vfsdev->opts.enable_notifications) {
		struct statvfs stat = {};

		/* Check if filesystem supports fanotify. According to fanotify_mark(2)
		 * it doesn't work for filesystems that report fsid 0.
		 */
		rc = fstatvfs(vfsdev->root->fd, &stat);
		if (rc) {
			rc = -errno;
			SPDK_ERRLOG("Failed to get filesystem statistics, fsdev %s, root_fd %d, errno %d\n",
				    vfsdev->fsdev.name, vfsdev->root->fd, errno);
			fsdev_aio_free(vfsdev);
			return rc;
		}

		if (stat.f_fsid == 0) {
			SPDK_ERRLOG("Fsdev %s does not support fanotify\n", vfsdev->fsdev.name);
			fsdev_aio_free(vfsdev);
			return -EINVAL;
		}
		vfsdev->fsdev.notify_max_data_size = DEFAULT_NOTIFY_MAX_DATA_SIZE;
	}
#endif

	rc = spdk_fsdev_register(&vfsdev->fsdev);
	if (rc) {
		fsdev_aio_free(vfsdev);
		return rc;
	}

	vfsdev->mount_opts.max_xfer_size = opts->max_xfer_size;
	vfsdev->mount_opts.max_readahead = opts->max_readahead;

	*fsdev = &(vfsdev->fsdev);
	TAILQ_INSERT_TAIL(&g_aio_fsdev_head, vfsdev, tailq);
	SPDK_DEBUGLOG(fsdev_aio, "Created aio filesystem %s: (xattr_enabled=%" PRIu8 " writeback_cache=%"
		      PRIu8 " max_xfer_size=%" PRIu32 " max_readahead=%" PRIu32 " skip_rw=%" PRIu8 " io_uring=%d)\n",
		      vfsdev->fsdev.name, vfsdev->opts.xattr_enabled, vfsdev->opts.writeback_cache_enabled,
		      vfsdev->opts.max_xfer_size, vfsdev->opts.max_readahead, vfsdev->opts.skip_rw,
		      aio_fsdev_use_io_uring_rdwr());
	return rc;
}
void
spdk_fsdev_aio_delete(const char *name,
		      spdk_delete_aio_fsdev_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_fsdev_unregister_by_name(name, &aio_fsdev_module, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}

	SPDK_DEBUGLOG(fsdev_aio, "Deleted aio filesystem %s\n", name);
}

void
fsdev_aio_get_opts(struct fsdev_aio_module_opts *opts)
{
	*opts = g_opts;
}

int
fsdev_aio_set_opts(const struct fsdev_aio_module_opts *opts)
{
	if (opts->max_io_depth == 0) {
		return -EINVAL;
	}

	g_opts = *opts;
	return 0;
}
SPDK_LOG_REGISTER_COMPONENT(fsdev_aio)
