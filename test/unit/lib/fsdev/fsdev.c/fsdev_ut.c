/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"

#include "spdk/log.h"
#include "spdk/fsdev.h"
#include "spdk/fsdev_module.h"

#define UT_UNIQUE 0xBEADBEAD
#define UT_FOBJECT ((struct spdk_fsdev_file_object *)0xDEADDEAD)
#define UT_FHANDLE ((struct spdk_fsdev_file_handle *)0xBEABBEAB)
#define UT_FNAME "ut_test.file"
#define UT_LNAME "ut_test.file.link"
#define UT_ANAME "xattr1.name"
#define UT_AVALUE "xattr1.val"
#define UT_NUM_LOOKUPS 11
#define UT_DATA_SIZE 22


#define UT_CALL_REC_MAX_CALLS 5
#define UT_CALL_REC_MAX_PARAMS 15
#define UT_CALL_REC_MAX_STR_SIZE 255

#define UT_SUBMIT_IO_NUM_COMMON_PARAMS 4

static uint64_t
ut_hash(const void *buf, size_t size)
{
	uint64_t hash = 5381;
	const char *p = buf;
	size_t i;

	for (i = 0; i < size; i++) {
		hash = ((hash << 5) + hash) + (*p); /* hash * 33 + c */
		p++;
	}

	return hash;
}

struct ut_call_record {
	struct {
		void *func;
		union {
			uint64_t integer;
			void *ptr;
			char str[UT_CALL_REC_MAX_STR_SIZE + 1];
			uint64_t hash;
		} params[UT_CALL_REC_MAX_PARAMS];
		size_t param_count;
	} call[UT_CALL_REC_MAX_CALLS];
	size_t count;
};

static struct ut_call_record g_call_list;

static inline void
ut_calls_reset(void)
{
	memset(&g_call_list, 0, sizeof(g_call_list));
}

static inline void
ut_call_record_begin(void *pfunc)
{
	SPDK_CU_ASSERT_FATAL(g_call_list.count < UT_CALL_REC_MAX_CALLS);
	g_call_list.call[g_call_list.count].func = pfunc;
	g_call_list.call[g_call_list.count].param_count = 0;
}

static inline void
ut_call_record_param_int(uint64_t val)
{
	SPDK_CU_ASSERT_FATAL(g_call_list.call[g_call_list.count].param_count < UT_CALL_REC_MAX_PARAMS);
	g_call_list.call[g_call_list.count].params[g_call_list.call[g_call_list.count].param_count].integer
		= val;
	g_call_list.call[g_call_list.count].param_count++;
}

static inline void
ut_call_record_param_ptr(void *ptr)
{
	SPDK_CU_ASSERT_FATAL(g_call_list.call[g_call_list.count].param_count < UT_CALL_REC_MAX_PARAMS);
	g_call_list.call[g_call_list.count].params[g_call_list.call[g_call_list.count].param_count].ptr =
		ptr;
	g_call_list.call[g_call_list.count].param_count++;
}

static inline void
ut_call_record_param_str(const char *str)
{
	SPDK_CU_ASSERT_FATAL(g_call_list.call[g_call_list.count].param_count < UT_CALL_REC_MAX_PARAMS);
	spdk_strcpy_pad(
		g_call_list.call[g_call_list.count].params[g_call_list.call[g_call_list.count].param_count].str,
		str, UT_CALL_REC_MAX_STR_SIZE, 0);
	g_call_list.call[g_call_list.count].params[g_call_list.call[g_call_list.count].param_count].str[UT_CALL_REC_MAX_STR_SIZE]
		= 0;
	g_call_list.call[g_call_list.count].param_count++;
}

static inline void
ut_call_record_param_hash(const void *buf, size_t size)
{
	SPDK_CU_ASSERT_FATAL(g_call_list.call[g_call_list.count].param_count < UT_CALL_REC_MAX_PARAMS);
	g_call_list.call[g_call_list.count].params[g_call_list.call[g_call_list.count].param_count].hash =
		ut_hash(buf, size);
	g_call_list.call[g_call_list.count].param_count++;
}

static inline size_t
ut_call_record_get_current_param_count(void)
{
	return g_call_list.call[g_call_list.count].param_count;
}
static inline void
ut_call_record_end(void)
{
	g_call_list.count++;
}

static inline void
ut_call_record_simple_param_ptr(void *pfunc, void *ptr)
{
	ut_call_record_begin(pfunc);
	ut_call_record_param_ptr(ptr);
	ut_call_record_end();
}

static inline size_t
ut_calls_get_call_count(void)
{
	return g_call_list.count;
}

static inline size_t
ut_calls_get_param_count(size_t call_idx)
{
	SPDK_CU_ASSERT_FATAL(call_idx < g_call_list.count);
	return g_call_list.call[call_idx].param_count;
}

static inline void *
ut_calls_get_func(size_t call_idx)
{
	SPDK_CU_ASSERT_FATAL(call_idx < g_call_list.count);
	return g_call_list.call[call_idx].func;
}

static inline uint64_t
ut_calls_param_get_int(size_t call_idx, size_t param_idx)
{
	SPDK_CU_ASSERT_FATAL(call_idx < g_call_list.count);
	SPDK_CU_ASSERT_FATAL(param_idx < g_call_list.call[call_idx].param_count);
	return g_call_list.call[call_idx].params[param_idx].integer;
}

static inline void *
ut_calls_param_get_ptr(size_t call_idx, size_t param_idx)
{
	SPDK_CU_ASSERT_FATAL(call_idx < g_call_list.count);
	SPDK_CU_ASSERT_FATAL(param_idx < g_call_list.call[call_idx].param_count);
	return g_call_list.call[call_idx].params[param_idx].ptr;
}

static inline const char *
ut_calls_param_get_str(size_t call_idx, size_t param_idx)
{
	SPDK_CU_ASSERT_FATAL(call_idx < g_call_list.count);
	SPDK_CU_ASSERT_FATAL(param_idx < g_call_list.call[call_idx].param_count);
	return g_call_list.call[call_idx].params[param_idx].str;
}

static inline uint64_t
ut_calls_param_get_hash(size_t call_idx, size_t param_idx)
{
	SPDK_CU_ASSERT_FATAL(call_idx < g_call_list.count);
	SPDK_CU_ASSERT_FATAL(param_idx < g_call_list.call[call_idx].param_count);
	return g_call_list.call[call_idx].params[param_idx].hash;
}

struct ut_fsdev {
	struct spdk_fsdev fsdev;
	int desired_io_status;
};

struct ut_io_channel {
	int reserved;
};

struct spdk_fsdev_file_object {
	int reserved;
};

struct spdk_fsdev_file_handle {
	int reserved;
};

static inline struct ut_fsdev *
fsdev_to_ut_fsdev(struct spdk_fsdev *fsdev)
{
	return SPDK_CONTAINEROF(fsdev, struct ut_fsdev, fsdev);
}

static struct ut_io_channel *g_ut_io_channel = NULL;

static int
ut_fsdev_io_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct ut_io_channel *ch = ctx_buf;

	g_ut_io_channel = ch;

	ut_call_record_simple_param_ptr(ut_fsdev_io_channel_create_cb, ctx_buf);

	return 0;
}

static void
ut_fsdev_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	g_ut_io_channel = NULL;

	ut_call_record_simple_param_ptr(ut_fsdev_io_channel_destroy_cb, ctx_buf);
}

static int
ut_fsdev_initialize(void)
{
	spdk_io_device_register(&g_call_list,
				ut_fsdev_io_channel_create_cb, ut_fsdev_io_channel_destroy_cb,
				sizeof(struct ut_io_channel), "ut_fsdev");

	return 0;
}

static void
ut_fsdev_io_device_unregister_done(void *io_device)
{
	SPDK_NOTICELOG("ut_fsdev_io_device unregistred\n");
}

static void
ut_fsdev_finish(void)
{
	spdk_io_device_unregister(&g_call_list, ut_fsdev_io_device_unregister_done);
}

static int
ut_fsdev_get_ctx_size(void)
{
	return 0;
}

static struct spdk_fsdev_module ut_fsdev_module = {
	.name = "ut_fsdev",
	.module_init = ut_fsdev_initialize,
	.module_fini = ut_fsdev_finish,
	.get_ctx_size = ut_fsdev_get_ctx_size,
};

SPDK_FSDEV_MODULE_REGISTER(ut_fsdev, &ut_fsdev_module);

static int
ut_fsdev_destruct(void *ctx)
{
	ut_call_record_simple_param_ptr(ut_fsdev_destruct, ctx);

	return 0;
}

static struct spdk_fsdev_file_attr ut_fsdev_attr;
static struct spdk_fsdev_file_object ut_fsdev_fobject;
static struct iovec ut_iov[5];
static struct spdk_fsdev_file_statfs ut_statfs;
static char ut_buff[1024];
static bool ut_listxattr_size_only;
static uint64_t ut_readdir_offset;
static uint64_t ut_readdir_num_entries;
static uint64_t ut_readdir_num_entry_cb_calls;

static void
ut_fsdev_submit_request(struct spdk_io_channel *_ch, struct spdk_fsdev_io *fsdev_io)
{
	enum spdk_fsdev_op op = spdk_fsdev_io_get_op(fsdev_io);
	struct ut_fsdev *utfsdev = fsdev_to_ut_fsdev(fsdev_io->fsdev);
	struct ut_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	uint64_t unique = spdk_fsdev_io_get_unuqie(fsdev_io);
	int res, i = 0;

	CU_ASSERT(op >= 0 && op < __SPDK_FSDEV_OP_LAST);

	ut_call_record_begin(ut_fsdev_submit_request);

	/* Common params */
	ut_call_record_param_int(op);
	/* There's no unique for abort so we just add UT_UNIQUE to pass the test */
	ut_call_record_param_int((op != SPDK_FSDEV_OP_ABORT) ? unique : UT_UNIQUE);
	ut_call_record_param_ptr(ch);
	ut_call_record_param_ptr(utfsdev);

	CU_ASSERT(ut_call_record_get_current_param_count() == UT_SUBMIT_IO_NUM_COMMON_PARAMS);

	switch (op) {
	case SPDK_FSDEV_OP_LOOKUP:
		ut_call_record_param_str(fsdev_io->u_in.lookup.name);
		ut_call_record_param_ptr(fsdev_io->u_in.lookup.parent_fobject);
		fsdev_io->u_out.lookup.fobject = &ut_fsdev_fobject;
		break;
	case SPDK_FSDEV_OP_FORGET:
		ut_call_record_param_ptr(fsdev_io->u_in.forget.fobject);
		ut_call_record_param_int(fsdev_io->u_in.forget.nlookup);
		break;
	case SPDK_FSDEV_OP_GETATTR:
		ut_call_record_param_ptr(fsdev_io->u_in.getattr.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.getattr.fhandle);
		fsdev_io->u_out.getattr.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_SETATTR:
		ut_call_record_param_ptr(fsdev_io->u_in.setattr.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.setattr.fhandle);
		ut_call_record_param_hash(&fsdev_io->u_in.setattr.attr, sizeof(fsdev_io->u_in.setattr.attr));
		ut_call_record_param_int(fsdev_io->u_in.setattr.to_set);
		fsdev_io->u_out.getattr.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_READLINK:
		ut_call_record_param_ptr(fsdev_io->u_in.readlink.fobject);
		fsdev_io->u_out.readlink.linkname = strdup(UT_FNAME);
		SPDK_CU_ASSERT_FATAL(fsdev_io->u_out.readlink.linkname != NULL);
		break;
	case SPDK_FSDEV_OP_SYMLINK:
		ut_call_record_param_ptr(fsdev_io->u_in.symlink.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.symlink.target);
		ut_call_record_param_str(fsdev_io->u_in.symlink.linkpath);
		ut_call_record_param_int(fsdev_io->u_in.symlink.euid);
		ut_call_record_param_int(fsdev_io->u_in.symlink.egid);
		fsdev_io->u_out.symlink.fobject = UT_FOBJECT + 1;
		fsdev_io->u_out.symlink.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_MKNOD:
		ut_call_record_param_ptr(fsdev_io->u_in.mknod.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.mknod.name);
		ut_call_record_param_int(fsdev_io->u_in.mknod.mode);
		ut_call_record_param_int(fsdev_io->u_in.mknod.rdev);
		ut_call_record_param_int(fsdev_io->u_in.mknod.euid);
		ut_call_record_param_int(fsdev_io->u_in.mknod.egid);
		fsdev_io->u_out.mknod.fobject = UT_FOBJECT + 1;
		fsdev_io->u_out.mknod.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_MKDIR:
		ut_call_record_param_ptr(fsdev_io->u_in.mkdir.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.mkdir.name);
		ut_call_record_param_int(fsdev_io->u_in.mkdir.mode);
		ut_call_record_param_int(fsdev_io->u_in.mkdir.euid);
		ut_call_record_param_int(fsdev_io->u_in.mkdir.egid);
		fsdev_io->u_out.mkdir.fobject = UT_FOBJECT + 1;
		fsdev_io->u_out.mkdir.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_UNLINK:
		ut_call_record_param_ptr(fsdev_io->u_in.unlink.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.unlink.name);
		break;
	case SPDK_FSDEV_OP_RMDIR:
		ut_call_record_param_ptr(fsdev_io->u_in.rmdir.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.rmdir.name);
		break;
	case SPDK_FSDEV_OP_RENAME:
		ut_call_record_param_ptr(fsdev_io->u_in.rename.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.rename.name);
		ut_call_record_param_ptr(fsdev_io->u_in.rename.new_parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.rename.new_name);
		ut_call_record_param_int(fsdev_io->u_in.rename.flags);
		break;
	case SPDK_FSDEV_OP_LINK:
		ut_call_record_param_ptr(fsdev_io->u_in.link.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.link.new_parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.link.name);
		fsdev_io->u_out.link.fobject = UT_FOBJECT + 1;
		fsdev_io->u_out.link.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_OPEN:
		ut_call_record_param_ptr(fsdev_io->u_in.open.fobject);
		ut_call_record_param_int(fsdev_io->u_in.open.flags);
		fsdev_io->u_out.open.fhandle = UT_FHANDLE;
		break;
	case SPDK_FSDEV_OP_READ:
		ut_call_record_param_ptr(fsdev_io->u_in.read.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.read.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.read.size);
		ut_call_record_param_int(fsdev_io->u_in.read.offs);
		ut_call_record_param_int(fsdev_io->u_in.read.flags);
		ut_call_record_param_hash(fsdev_io->u_in.read.iov,
					  fsdev_io->u_in.read.iovcnt * sizeof(fsdev_io->u_in.read.iov[0]));
		ut_call_record_param_int(fsdev_io->u_in.read.iovcnt);
		ut_call_record_param_ptr(fsdev_io->u_in.read.opts);
		fsdev_io->u_out.read.data_size = UT_DATA_SIZE;
		break;
	case SPDK_FSDEV_OP_WRITE:
		ut_call_record_param_ptr(fsdev_io->u_in.write.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.write.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.write.size);
		ut_call_record_param_int(fsdev_io->u_in.write.offs);
		ut_call_record_param_int(fsdev_io->u_in.write.flags);
		ut_call_record_param_hash(fsdev_io->u_in.write.iov,
					  fsdev_io->u_in.write.iovcnt * sizeof(fsdev_io->u_in.write.iov[0]));
		ut_call_record_param_int(fsdev_io->u_in.write.iovcnt);
		ut_call_record_param_ptr(fsdev_io->u_in.write.opts);
		fsdev_io->u_out.write.data_size = UT_DATA_SIZE;
		break;
	case SPDK_FSDEV_OP_STATFS:
		ut_call_record_param_ptr(fsdev_io->u_in.statfs.fobject);
		fsdev_io->u_out.statfs.statfs = ut_statfs;
		break;
	case SPDK_FSDEV_OP_RELEASE:
		ut_call_record_param_ptr(fsdev_io->u_in.release.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.release.fhandle);
		break;
	case SPDK_FSDEV_OP_FSYNC:
		ut_call_record_param_ptr(fsdev_io->u_in.fsync.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.fsync.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.fsync.datasync);
		break;
	case SPDK_FSDEV_OP_SETXATTR:
		ut_call_record_param_ptr(fsdev_io->u_in.setxattr.fobject);
		ut_call_record_param_str(fsdev_io->u_in.setxattr.name);
		ut_call_record_param_str(fsdev_io->u_in.setxattr.value);
		ut_call_record_param_int(fsdev_io->u_in.setxattr.size);
		ut_call_record_param_int(fsdev_io->u_in.setxattr.flags);
		break;
	case SPDK_FSDEV_OP_GETXATTR:
		ut_call_record_param_ptr(fsdev_io->u_in.getxattr.fobject);
		ut_call_record_param_str(fsdev_io->u_in.getxattr.name);
		ut_call_record_param_ptr(fsdev_io->u_in.getxattr.buffer);
		ut_call_record_param_int(fsdev_io->u_in.getxattr.size);
		spdk_strcpy_pad(fsdev_io->u_in.getxattr.buffer, UT_AVALUE,
				fsdev_io->u_in.getxattr.size - 1, 0);
		fsdev_io->u_out.getxattr.value_size = sizeof(UT_AVALUE);
		break;
	case SPDK_FSDEV_OP_LISTXATTR:
		ut_call_record_param_ptr(fsdev_io->u_in.listxattr.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.listxattr.buffer);
		ut_call_record_param_int(fsdev_io->u_in.listxattr.size);

		fsdev_io->u_out.listxattr.size_only = fsdev_io->u_in.listxattr.buffer == NULL;
		fsdev_io->u_out.listxattr.data_size = (sizeof(ut_buff) / sizeof(UT_ANAME)) * sizeof(UT_ANAME);

		if (!fsdev_io->u_out.listxattr.size_only) {
			size_t size = fsdev_io->u_in.listxattr.size;
			char *p = fsdev_io->u_in.listxattr.buffer;

			while (size >= sizeof(UT_ANAME)) {
				memcpy(p, UT_ANAME, sizeof(UT_ANAME));
				p += sizeof(UT_ANAME);
				size -= sizeof(UT_ANAME);
			}
		}
		break;
	case SPDK_FSDEV_OP_REMOVEXATTR:
		ut_call_record_param_ptr(fsdev_io->u_in.removexattr.fobject);
		ut_call_record_param_str(fsdev_io->u_in.removexattr.name);
		break;
	case SPDK_FSDEV_OP_FLUSH:
		ut_call_record_param_ptr(fsdev_io->u_in.flush.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.flush.fhandle);
		break;
	case SPDK_FSDEV_OP_OPENDIR:
		ut_call_record_param_ptr(fsdev_io->u_in.opendir.fobject);
		ut_call_record_param_int(fsdev_io->u_in.opendir.flags);
		fsdev_io->u_out.opendir.fhandle = UT_FHANDLE;
		break;
	case SPDK_FSDEV_OP_READDIR:
		ut_call_record_param_ptr(fsdev_io->u_in.readdir.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.readdir.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.readdir.offset);
		ut_call_record_param_ptr(fsdev_io->u_in.readdir.usr_entry_cb_fn);

		do {
			fsdev_io->u_out.readdir.fobject = UT_FOBJECT + i;
			fsdev_io->u_out.readdir.attr = ut_fsdev_attr;
			fsdev_io->u_out.readdir.name = UT_FNAME;
			fsdev_io->u_out.readdir.offset = ut_readdir_offset + i;
			res = fsdev_io->u_in.readdir.entry_cb_fn(fsdev_io, fsdev_io->internal.cb_arg);
			i++;
		} while (!res);

		break;
	case SPDK_FSDEV_OP_RELEASEDIR:
		ut_call_record_param_ptr(fsdev_io->u_in.releasedir.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.releasedir.fhandle);
		break;
	case SPDK_FSDEV_OP_FSYNCDIR:
		ut_call_record_param_ptr(fsdev_io->u_in.fsyncdir.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.fsyncdir.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.fsyncdir.datasync);
		break;
	case SPDK_FSDEV_OP_FLOCK:
		ut_call_record_param_ptr(fsdev_io->u_in.flock.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.flock.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.flock.operation);
		break;
	case SPDK_FSDEV_OP_CREATE:
		ut_call_record_param_ptr(fsdev_io->u_in.create.parent_fobject);
		ut_call_record_param_str(fsdev_io->u_in.create.name);
		ut_call_record_param_int(fsdev_io->u_in.create.mode);
		ut_call_record_param_int(fsdev_io->u_in.create.flags);
		ut_call_record_param_int(fsdev_io->u_in.create.umask);
		ut_call_record_param_int(fsdev_io->u_in.create.euid);
		ut_call_record_param_int(fsdev_io->u_in.create.egid);
		fsdev_io->u_out.create.fobject = UT_FOBJECT + 1;
		fsdev_io->u_out.create.fhandle = UT_FHANDLE;
		fsdev_io->u_out.create.attr = ut_fsdev_attr;
		break;
	case SPDK_FSDEV_OP_ABORT:
		ut_call_record_param_int(fsdev_io->u_in.abort.unique_to_abort);
		break;
	case SPDK_FSDEV_OP_FALLOCATE:
		ut_call_record_param_ptr(fsdev_io->u_in.fallocate.fobject);
		ut_call_record_param_ptr(fsdev_io->u_in.fallocate.fhandle);
		ut_call_record_param_int(fsdev_io->u_in.fallocate.mode);
		ut_call_record_param_int(fsdev_io->u_in.fallocate.offset);
		ut_call_record_param_int(fsdev_io->u_in.fallocate.length);
		break;
	case SPDK_FSDEV_OP_COPY_FILE_RANGE:
		ut_call_record_param_ptr(fsdev_io->u_in.copy_file_range.fobject_in);
		ut_call_record_param_ptr(fsdev_io->u_in.copy_file_range.fhandle_in);
		ut_call_record_param_int(fsdev_io->u_in.copy_file_range.off_in);
		ut_call_record_param_ptr(fsdev_io->u_in.copy_file_range.fobject_out);
		ut_call_record_param_ptr(fsdev_io->u_in.copy_file_range.fhandle_out);
		ut_call_record_param_int(fsdev_io->u_in.copy_file_range.off_out);
		ut_call_record_param_int(fsdev_io->u_in.copy_file_range.len);
		ut_call_record_param_int(fsdev_io->u_in.copy_file_range.flags);
		fsdev_io->u_out.copy_file_range.data_size = UT_DATA_SIZE;
		break;
	case __SPDK_FSDEV_OP_LAST:
	default:
		break;
	}

	ut_call_record_end();

	spdk_fsdev_io_complete(fsdev_io, utfsdev->desired_io_status);
}

static struct spdk_io_channel *
ut_fsdev_get_io_channel(void *ctx)
{
	ut_call_record_simple_param_ptr(ut_fsdev_get_io_channel, ctx);

	return spdk_get_io_channel(&g_call_list);
}

static int
ut_fsdev_negotiate_opts(void *ctx, struct spdk_fsdev_device_opts *opts)
{
	ut_call_record_simple_param_ptr(ut_fsdev_negotiate_opts, ctx);

	return 0;
}

static void
ut_fsdev_write_config_json(struct spdk_fsdev *fsdev, struct spdk_json_write_ctx *w)
{

}

static int
ut_fsdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains,
			    int array_size)
{
	return 0;
}

static const struct spdk_fsdev_fn_table ut_fdev_fn_table = {
	.destruct		= ut_fsdev_destruct,
	.submit_request		= ut_fsdev_submit_request,
	.get_io_channel		= ut_fsdev_get_io_channel,
	.negotiate_opts		= ut_fsdev_negotiate_opts,
	.write_config_json	= ut_fsdev_write_config_json,
	.get_memory_domains	= ut_fsdev_get_memory_domains,
};

static void
ut_fsdev_free(struct ut_fsdev *ufsdev)
{
	free(ufsdev->fsdev.name);
	free(ufsdev);
}

static void
ut_fsdev_unregister_done(void *cb_arg, int rc)
{
	struct ut_fsdev *ufsdev = cb_arg;

	ut_call_record_simple_param_ptr(ut_fsdev_unregister_done, cb_arg);

	ut_fsdev_free(ufsdev);
}

static void
ut_fsdev_destroy(struct ut_fsdev *utfsdev)
{
	ut_calls_reset();
	spdk_fsdev_unregister(&utfsdev->fsdev, ut_fsdev_unregister_done, utfsdev);
	poll_thread(0);

	CU_ASSERT(ut_calls_get_call_count() == 2);

	CU_ASSERT(ut_calls_get_func(0) == ut_fsdev_destruct);
	CU_ASSERT(ut_calls_get_param_count(0) == 1);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == utfsdev);

	CU_ASSERT(ut_calls_get_func(1) == ut_fsdev_unregister_done);
	CU_ASSERT(ut_calls_get_param_count(1) == 1);
	CU_ASSERT(ut_calls_param_get_ptr(1, 0) == utfsdev);
}

static struct ut_fsdev *
ut_fsdev_create(const char *name)
{
	struct ut_fsdev *ufsdev;
	int rc;

	ufsdev = calloc(1, sizeof(*ufsdev));
	if (!ufsdev) {
		SPDK_ERRLOG("Could not allocate ut_fsdev\n");
		return NULL;
	}

	ufsdev->fsdev.name = strdup(name);
	if (!ufsdev->fsdev.name) {
		SPDK_ERRLOG("Could not strdup name %s\n", name);
		free(ufsdev);
		return NULL;
	}

	ufsdev->fsdev.ctxt = ufsdev;
	ufsdev->fsdev.fn_table = &ut_fdev_fn_table;
	ufsdev->fsdev.module = &ut_fsdev_module;

	rc = spdk_fsdev_register(&ufsdev->fsdev);
	if (rc) {
		ut_fsdev_free(ufsdev);
		return NULL;
	}

	return ufsdev;
}

static void
ut_fsdev_initialize_complete(void *cb_arg, int rc)
{
	bool *completed  = cb_arg;

	*completed = true;
}

static int
ut_fsdev_setup(void)
{
	bool completed = false;

	spdk_fsdev_initialize(ut_fsdev_initialize_complete, &completed);

	poll_thread(0);

	if (!completed) {
		SPDK_ERRLOG("No spdk_fsdev_initialize callback arrived\n");
		return EINVAL;
	}

	return 0;
}

static void
ut_fsdev_teardown_complete(void *cb_arg)
{
	bool *completed  = cb_arg;

	*completed = true;
}

static int
ut_fsdev_teardown(void)
{
	bool completed = false;
	spdk_fsdev_finish(ut_fsdev_teardown_complete, &completed);

	poll_thread(0);

	if (!completed) {
		SPDK_ERRLOG("No spdk_fsdev_finish callback arrived\n");
		return EINVAL;
	}

	return 0;
}

static void
fsdev_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev,
	       void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static void
ut_fsdev_test_open_close(void)
{
	struct ut_fsdev *utfsdev;
	struct spdk_fsdev_desc *fsdev_desc;
	int rc;

	utfsdev = ut_fsdev_create("utfsdev0");
	CU_ASSERT(utfsdev != NULL);

	CU_ASSERT(!strcmp(spdk_fsdev_get_module_name(&utfsdev->fsdev), ut_fsdev_module.name));
	CU_ASSERT(!strcmp(spdk_fsdev_get_name(&utfsdev->fsdev), "utfsdev0"));

	rc = spdk_fsdev_open("utfsdev0", fsdev_event_cb, NULL, &fsdev_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(fsdev_desc != NULL);
	CU_ASSERT(spdk_fsdev_desc_get_fsdev(fsdev_desc) == &utfsdev->fsdev);

	spdk_fsdev_close(fsdev_desc);

	ut_fsdev_destroy(utfsdev);
}

static void
ut_fsdev_test_get_by_name(void)
{
	struct ut_fsdev *utfsdev;
	struct spdk_fsdev *fsdev;

	utfsdev = ut_fsdev_create("utfsdev0");
	CU_ASSERT(utfsdev != NULL);

	fsdev = spdk_fsdev_get_by_name("utfsdev0");
	CU_ASSERT(fsdev == &utfsdev->fsdev);

	ut_fsdev_destroy(utfsdev);
}

static void
ut_fsdev_test_set_opts(void)
{
	struct spdk_fsdev_library_opts old_opts;
	struct spdk_fsdev_library_opts new_opts;
	int rc;

	rc = spdk_fsdev_set_opts(NULL);
	CU_ASSERT(rc == -EINVAL);

	new_opts.opts_size = 0;
	rc = spdk_fsdev_set_opts(&new_opts);
	CU_ASSERT(rc == -EINVAL);

	old_opts.opts_size = sizeof(old_opts);
	rc = spdk_fsdev_get_opts(&old_opts, sizeof(old_opts));
	CU_ASSERT(rc == 0);

	new_opts.opts_size = sizeof(new_opts);
	new_opts.fsdev_io_pool_size = old_opts.fsdev_io_pool_size * 2;
	new_opts.fsdev_io_cache_size = old_opts.fsdev_io_cache_size * 2;
	rc = spdk_fsdev_set_opts(&new_opts);
	CU_ASSERT(rc == 0);

	rc = spdk_fsdev_get_opts(&new_opts, sizeof(new_opts));
	CU_ASSERT(rc == 0);
	CU_ASSERT(old_opts.fsdev_io_pool_size * 2 == new_opts.fsdev_io_pool_size);
	CU_ASSERT(old_opts.fsdev_io_cache_size * 2 == new_opts.fsdev_io_cache_size);
}

static void
ut_fsdev_test_set_device_opts(void)
{
	struct ut_fsdev *utfsdev;
	struct spdk_fsdev_device_opts old_opts;
	struct spdk_fsdev_device_opts new_opts;
	int rc;

	utfsdev = ut_fsdev_create("utfsdev0");
	CU_ASSERT(utfsdev != NULL);

	rc = spdk_fsdev_set_device_opts(&utfsdev->fsdev, NULL);
	CU_ASSERT(rc == -EINVAL);

	new_opts.opts_size = 0;
	rc = spdk_fsdev_set_device_opts(&utfsdev->fsdev, &new_opts);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_fsdev_get_device_opts(&utfsdev->fsdev, &old_opts, sizeof(old_opts));
	CU_ASSERT(rc == 0);

	new_opts.opts_size = sizeof(new_opts);
	new_opts.max_write = old_opts.max_write * 2;
	new_opts.writeback_cache_enabled = !old_opts.writeback_cache_enabled;
	ut_calls_reset();
	rc = spdk_fsdev_set_device_opts(&utfsdev->fsdev, &new_opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ut_calls_get_call_count() == 1);

	CU_ASSERT(ut_calls_get_func(0) == ut_fsdev_negotiate_opts);
	CU_ASSERT(ut_calls_get_param_count(0) == 1);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == utfsdev);

	rc = spdk_fsdev_get_device_opts(&utfsdev->fsdev, &new_opts, sizeof(new_opts));
	CU_ASSERT(rc == 0);
	CU_ASSERT(old_opts.max_write * 2 == new_opts.max_write);
	CU_ASSERT(old_opts.writeback_cache_enabled != new_opts.writeback_cache_enabled);

	ut_fsdev_destroy(utfsdev);
}

static void
ut_fsdev_test_get_io_channel(void)
{
	struct ut_fsdev *utfsdev;
	struct spdk_io_channel *ch;
	struct spdk_fsdev_desc *fsdev_desc;
	struct ut_io_channel *ut_ch;
	int rc;

	utfsdev = ut_fsdev_create("utfsdev0");
	CU_ASSERT(utfsdev != NULL);

	rc = spdk_fsdev_open("utfsdev0", fsdev_event_cb, NULL, &fsdev_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(fsdev_desc != NULL);
	CU_ASSERT(spdk_fsdev_desc_get_fsdev(fsdev_desc) == &utfsdev->fsdev);

	ut_calls_reset();
	ch = spdk_fsdev_get_io_channel(fsdev_desc);
	CU_ASSERT(ch != NULL);
	CU_ASSERT(ut_calls_get_call_count() == 2);

	CU_ASSERT(ut_calls_get_func(0) == ut_fsdev_get_io_channel);
	CU_ASSERT(ut_calls_get_param_count(0) == 1);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == utfsdev);

	CU_ASSERT(ut_calls_get_func(1) == ut_fsdev_io_channel_create_cb);
	CU_ASSERT(ut_calls_get_param_count(1) == 1);
	ut_ch = (struct ut_io_channel *)ut_calls_param_get_ptr(1, 0);

	ut_calls_reset();
	spdk_put_io_channel(ch);
	poll_thread(0);
	CU_ASSERT(ut_calls_get_call_count() == 1);

	CU_ASSERT(ut_calls_get_func(0) == ut_fsdev_io_channel_destroy_cb);
	CU_ASSERT(ut_calls_get_param_count(0) == 1);
	CU_ASSERT(ut_calls_param_get_ptr(0, 0) == ut_ch);

	spdk_fsdev_close(fsdev_desc);

	ut_fsdev_destroy(utfsdev);
}

typedef int (*execute_op_clb)(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
			      struct spdk_fsdev_desc *fsdev_desc, int *status);
typedef void (*check_op_clb)(void);

static void
ut_fsdev_test_op(enum spdk_fsdev_op op, int desired_io_status, size_t num_priv_params,
		 execute_op_clb execute_cb, check_op_clb check_cb)
{
	struct ut_fsdev *utfsdev;
	struct spdk_io_channel *ch;
	struct spdk_fsdev_desc *fsdev_desc;
	int rc;
	int status = -1;

	utfsdev = ut_fsdev_create("utfsdev0");
	CU_ASSERT(utfsdev != NULL);

	rc = spdk_fsdev_open("utfsdev0", fsdev_event_cb, NULL, &fsdev_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(fsdev_desc != NULL);

	ch = spdk_fsdev_get_io_channel(fsdev_desc);
	CU_ASSERT(ch != NULL);

	ut_calls_reset();
	utfsdev->desired_io_status = desired_io_status;
	rc = execute_cb(utfsdev, ch, fsdev_desc, &status);
	CU_ASSERT(rc == 0);

	poll_thread(0);
	CU_ASSERT(status == desired_io_status);
	CU_ASSERT(ut_calls_get_call_count() == 1);
	CU_ASSERT(ut_calls_get_func(0) == ut_fsdev_submit_request);
	CU_ASSERT(ut_calls_get_param_count(0) == UT_SUBMIT_IO_NUM_COMMON_PARAMS + num_priv_params);

	/* Common params */
	CU_ASSERT(ut_calls_param_get_int(0, 0) == op);
	CU_ASSERT(ut_calls_param_get_int(0, 1) == UT_UNIQUE);
	CU_ASSERT(ut_calls_param_get_ptr(0, 2) == g_ut_io_channel);
	CU_ASSERT(ut_calls_param_get_ptr(0, 3) == utfsdev);

	SPDK_CU_ASSERT_FATAL(UT_SUBMIT_IO_NUM_COMMON_PARAMS == 4);

	/* Op-specific params */
	check_cb();

	ut_calls_reset();
	spdk_put_io_channel(ch);
	poll_thread(0);

	spdk_fsdev_close(fsdev_desc);

	ut_fsdev_destroy(utfsdev);
}

static void
ut_fsdev_op_lookup_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			  struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	*clb_status = status;
	if (!status) {
		CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
		CU_ASSERT(&ut_fsdev_fobject == fobject);
	}
}

static int
ut_fsdev_op_lookup_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				  struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_lookup(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME,
				    ut_fsdev_op_lookup_cpl_cb,
				    status);
}

static void
ut_fsdev_op_lookup_check_op_clb(void)
{
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FOBJECT);
}

static void
ut_fsdev_test_op_lookup_ok(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_LOOKUP, 0, 2, ut_fsdev_op_lookup_execute_op_clb,
			 ut_fsdev_op_lookup_check_op_clb);
}

static void
ut_fsdev_test_op_lookup_err(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_LOOKUP, -EBUSY, 2, ut_fsdev_op_lookup_execute_op_clb,
			 ut_fsdev_op_lookup_check_op_clb);
}

static void
ut_fsdev_op_forget_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_forget_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				  struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_forget(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_NUM_LOOKUPS,
				    ut_fsdev_op_forget_cpl_cb, status);
}

static void
ut_fsdev_op_forget_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_NUM_LOOKUPS);
}

static void
ut_fsdev_test_op_forget(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_FORGET, 0, 2, ut_fsdev_op_forget_execute_op_clb,
			 ut_fsdev_op_forget_check_op_clb);
}

static void
ut_fsdev_op_getattr_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			   const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_getattr_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				   struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_getattr(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE,
				     ut_fsdev_op_getattr_cpl_cb, status);
}

static void
ut_fsdev_op_getattr_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
}

static void
ut_fsdev_test_op_getattr(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_GETATTR, 0, 2, ut_fsdev_op_getattr_execute_op_clb,
			 ut_fsdev_op_getattr_check_op_clb);
}

static void
ut_fsdev_op_setattr_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			   const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	*clb_status = status;
}

static int
ut_fsdev_op_setattr_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				   struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_fsdev_attr, rand(), sizeof(ut_fsdev_attr));
	return spdk_fsdev_op_setattr(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE,
				     &ut_fsdev_attr, 0x11111111, ut_fsdev_op_setattr_cpl_cb, status);
}

static void
ut_fsdev_op_setattr_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_hash(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) ==
		  ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 0x11111111);
}

static void
ut_fsdev_test_op_setattr(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_SETATTR, 0, 4, ut_fsdev_op_setattr_execute_op_clb,
			 ut_fsdev_op_setattr_check_op_clb);
}

static void
ut_fsdev_op_readlink_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			    const char *linkname)
{
	int *clb_status = cb_arg;
	CU_ASSERT(!strcmp(linkname, UT_FNAME));
	*clb_status = status;
}

static int
ut_fsdev_op_readlink_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				    struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_readlink(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, ut_fsdev_op_readlink_cpl_cb,
				      status);
}

static void
ut_fsdev_op_readlink_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
}

static void
ut_fsdev_test_op_readlink(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_READLINK, 0, 1, ut_fsdev_op_readlink_execute_op_clb,
			 ut_fsdev_op_readlink_check_op_clb);
}

static void
ut_fsdev_op_symlink_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			   struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fobject == UT_FOBJECT + 1);
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	*clb_status = status;
}

static int
ut_fsdev_op_symlink_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				   struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_fsdev_attr, rand(), sizeof(ut_fsdev_attr));
	return spdk_fsdev_op_symlink(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME, UT_LNAME, 100, 200,
				     ut_fsdev_op_symlink_cpl_cb, status);
}

static void
ut_fsdev_op_symlink_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2), UT_LNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 100);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 200);
}

static void
ut_fsdev_test_op_symlink(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_SYMLINK, 0, 5, ut_fsdev_op_symlink_execute_op_clb,
			 ut_fsdev_op_symlink_check_op_clb);
}

static void
ut_fsdev_op_mknod_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			 struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fobject == UT_FOBJECT + 1);
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	*clb_status = status;
}

static int
ut_fsdev_op_mknod_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_fsdev_attr, rand(), sizeof(ut_fsdev_attr));
	return spdk_fsdev_op_mknod(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME, 0x1111, 50, 100, 200,
				   ut_fsdev_op_mknod_cpl_cb, status);
}

static void
ut_fsdev_op_mknod_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 0x1111);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 50);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 100);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 5) == 200);
}

static void
ut_fsdev_test_op_mknod(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_MKNOD, 0, 6, ut_fsdev_op_mknod_execute_op_clb,
			 ut_fsdev_op_mknod_check_op_clb);
}

static void
ut_fsdev_op_mkdir_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			 struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fobject == UT_FOBJECT + 1);
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	*clb_status = status;
}

static int
ut_fsdev_op_mkdir_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_fsdev_attr, rand(), sizeof(ut_fsdev_attr));
	return spdk_fsdev_op_mkdir(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME, 0x1111, 100, 200,
				   ut_fsdev_op_mkdir_cpl_cb, status);
}

static void
ut_fsdev_op_mkdir_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 0x1111);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 100);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 200);
}

static void
ut_fsdev_test_op_mkdir(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_MKDIR, 0, 5, ut_fsdev_op_mkdir_execute_op_clb,
			 ut_fsdev_op_mkdir_check_op_clb);
}

static void
ut_fsdev_op_unlink_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_unlink_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				  struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_unlink(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME,
				    ut_fsdev_op_unlink_cpl_cb, status);
}

static void
ut_fsdev_op_unlink_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
}

static void
ut_fsdev_test_op_unlink(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_UNLINK, 0, 2, ut_fsdev_op_unlink_execute_op_clb,
			 ut_fsdev_op_unlink_check_op_clb);
}

static void
ut_fsdev_op_rmdir_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_rmdir_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_rmdir(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME,
				   ut_fsdev_op_rmdir_cpl_cb, status);
}

static void
ut_fsdev_op_rmdir_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
}

static void
ut_fsdev_test_op_rmdir(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_RMDIR, 0, 2, ut_fsdev_op_rmdir_execute_op_clb,
			 ut_fsdev_op_rmdir_check_op_clb);
}

static void
ut_fsdev_op_rename_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_rename_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				  struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_rename(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FNAME, UT_FOBJECT + 2,
				    UT_LNAME, 0xFFFF, ut_fsdev_op_rename_cpl_cb, status);
}

static void
ut_fsdev_op_rename_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_FNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == UT_FOBJECT + 2);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3), UT_LNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 0xFFFF);
}

static void
ut_fsdev_test_op_rename(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_RENAME, 0, 5, ut_fsdev_op_rename_execute_op_clb,
			 ut_fsdev_op_rename_check_op_clb);
}

static void
ut_fsdev_op_link_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fobject == UT_FOBJECT + 1);
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	*clb_status = status;
}

static int
ut_fsdev_op_link_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_link(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FOBJECT + 2, UT_LNAME,
				  ut_fsdev_op_link_cpl_cb, status);
}

static void
ut_fsdev_op_link_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FOBJECT + 2);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2), UT_LNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
}

static void
ut_fsdev_test_op_link(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_LINK, 0, 3, ut_fsdev_op_link_execute_op_clb,
			 ut_fsdev_op_link_check_op_clb);
}

static void
ut_fsdev_op_open_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			struct spdk_fsdev_file_handle *fhandle)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fhandle == UT_FHANDLE);
	*clb_status = status;
}

static int
ut_fsdev_op_open_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_open(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, 0xFEAD,
				  ut_fsdev_op_open_cpl_cb, status);
}

static void
ut_fsdev_op_open_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == 0xFEAD);
}

static void
ut_fsdev_test_op_open(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_OPEN, 0, 2, ut_fsdev_op_open_execute_op_clb,
			 ut_fsdev_op_open_check_op_clb);
}

static void
ut_fsdev_op_read_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			uint32_t data_size)
{
	int *clb_status = cb_arg;
	CU_ASSERT(data_size == UT_DATA_SIZE);
	*clb_status = status;
}

static int
ut_fsdev_op_read_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_iov, rand(), sizeof(ut_iov));
	return spdk_fsdev_op_read(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, 100, 200, 0x1111,
				  ut_iov, SPDK_COUNTOF(ut_iov), (struct spdk_fsdev_ext_op_opts *)0xAAAAAAAA,
				  ut_fsdev_op_read_cpl_cb, status);
}

static void
ut_fsdev_op_read_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 100);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 200);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 0x1111);
	CU_ASSERT(ut_calls_param_get_hash(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 5) == ut_hash(ut_iov,
			sizeof(ut_iov)));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 6) == SPDK_COUNTOF(ut_iov));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 7) == 0xAAAAAAAA);
}

static void
ut_fsdev_test_op_read(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_READ, 0, 8, ut_fsdev_op_read_execute_op_clb,
			 ut_fsdev_op_read_check_op_clb);
}

static void
ut_fsdev_op_write_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			 uint32_t data_size)
{
	int *clb_status = cb_arg;
	CU_ASSERT(data_size == UT_DATA_SIZE);
	*clb_status = status;
}

static int
ut_fsdev_op_write_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_iov, rand(), sizeof(ut_iov));
	return spdk_fsdev_op_write(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, 100, 200, 0x1111,
				   ut_iov, SPDK_COUNTOF(ut_iov), (struct spdk_fsdev_ext_op_opts *)0xAAAAAAAA,
				   ut_fsdev_op_write_cpl_cb, status);
}

static void
ut_fsdev_op_write_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 100);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 200);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 0x1111);
	CU_ASSERT(ut_calls_param_get_hash(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 5) ==
		  ut_hash(ut_iov, sizeof(ut_iov)));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 6) == SPDK_COUNTOF(ut_iov));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 7) == 0xAAAAAAAA);
}

static void
ut_fsdev_test_op_write(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_WRITE, 0, 8, ut_fsdev_op_write_execute_op_clb,
			 ut_fsdev_op_write_check_op_clb);
}

static void
ut_fsdev_op_statfs_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			  const struct spdk_fsdev_file_statfs *statfs)
{
	int *clb_status = cb_arg;
	CU_ASSERT(ut_hash(&ut_statfs, sizeof(ut_statfs)) == ut_hash(statfs, sizeof(*statfs)));
	*clb_status = status;
}

static int
ut_fsdev_op_statfs_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				  struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_statfs, rand(), sizeof(ut_statfs));
	return spdk_fsdev_op_statfs(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT,
				    ut_fsdev_op_statfs_cpl_cb, status);
}

static void
ut_fsdev_op_statfs_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
}

static void
ut_fsdev_test_op_statfs(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_STATFS, 0, 1, ut_fsdev_op_statfs_execute_op_clb,
			 ut_fsdev_op_statfs_check_op_clb);
}

static void
ut_fsdev_op_release_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_release_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				   struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_release(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE,
				     ut_fsdev_op_release_cpl_cb, status);
}

static void
ut_fsdev_op_release_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
}

static void
ut_fsdev_test_op_release(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_RELEASE, 0, 2, ut_fsdev_op_release_execute_op_clb,
			 ut_fsdev_op_release_check_op_clb);
}

static void
ut_fsdev_op_fsync_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_fsync_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_fsync(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, false,
				   ut_fsdev_op_fsync_cpl_cb, status);
}

static void
ut_fsdev_op_fsync_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == false);
}

static void
ut_fsdev_test_op_fsync(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_FSYNC, 0, 3, ut_fsdev_op_fsync_execute_op_clb,
			 ut_fsdev_op_fsync_check_op_clb);
}

static void
ut_fsdev_op_getxattr_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			    size_t value_size)
{
	int *clb_status = cb_arg;
	CU_ASSERT(value_size == sizeof(UT_AVALUE));
	CU_ASSERT(!strcmp(ut_buff, UT_AVALUE));
	*clb_status = status;
}

static int
ut_fsdev_op_getxattr_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				    struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(ut_buff, 0, sizeof(ut_buff));
	return spdk_fsdev_op_getxattr(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_ANAME, ut_buff,
				      sizeof(ut_buff), ut_fsdev_op_getxattr_cpl_cb, status);
}

static void
ut_fsdev_op_getxattr_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_ANAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == ut_buff);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == sizeof(ut_buff));
}

static void
ut_fsdev_test_op_getxattr(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_GETXATTR, 0, 4, ut_fsdev_op_getxattr_execute_op_clb,
			 ut_fsdev_op_getxattr_check_op_clb);
}

static void
ut_fsdev_op_setxattr_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_setxattr_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				    struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_setxattr(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_ANAME, UT_AVALUE,
				      sizeof(UT_AVALUE), 0xFF, ut_fsdev_op_setxattr_cpl_cb, status);
}

static void
ut_fsdev_op_setxattr_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_ANAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2), UT_AVALUE,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == sizeof(UT_AVALUE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 0xFF);
}

static void
ut_fsdev_test_op_setxattr(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_SETXATTR, 0, 5, ut_fsdev_op_setxattr_execute_op_clb,
			 ut_fsdev_op_setxattr_check_op_clb);
}

static void
ut_fsdev_op_listxattr_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status, size_t size,
			     bool size_only)
{
	int *clb_status = cb_arg;
	if (ut_listxattr_size_only) {
		CU_ASSERT(size_only);
		CU_ASSERT(size == (sizeof(ut_buff) / sizeof(UT_ANAME)) * sizeof(UT_ANAME));
	} else {
		char *p = ut_buff;

		CU_ASSERT(!size_only);
		CU_ASSERT(size != 0);

		for (; p + sizeof(UT_ANAME) <= ut_buff + size; p += sizeof(UT_ANAME)) {
			CU_ASSERT(!strcmp(p, UT_ANAME));
		}

		CU_ASSERT(size + sizeof(UT_ANAME) > sizeof(ut_buff));
	}
	*clb_status = status;
}

static int
ut_fsdev_op_listxattr_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				     struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_listxattr(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT,
				       ut_listxattr_size_only ? NULL : ut_buff, ut_listxattr_size_only ? 0 : sizeof(ut_buff),
				       ut_fsdev_op_listxattr_cpl_cb, status);
}

static void
ut_fsdev_op_listxattr_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	if (ut_listxattr_size_only) {
		CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == NULL);
		CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 0);
	} else {
		CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == ut_buff);
		CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == sizeof(ut_buff));
	}
}

static void
ut_fsdev_test_op_listxattr(void)
{
	ut_listxattr_size_only = false;
	ut_fsdev_test_op(SPDK_FSDEV_OP_LISTXATTR, 0, 3, ut_fsdev_op_listxattr_execute_op_clb,
			 ut_fsdev_op_listxattr_check_op_clb);
}

static void
ut_fsdev_test_op_listxattr_get_size(void)
{
	ut_listxattr_size_only = true;
	ut_fsdev_test_op(SPDK_FSDEV_OP_LISTXATTR, 0, 3, ut_fsdev_op_listxattr_execute_op_clb,
			 ut_fsdev_op_listxattr_check_op_clb);
}

static void
ut_fsdev_op_removexattr_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_removexattr_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				       struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_removexattr(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_ANAME,
					 ut_fsdev_op_removexattr_cpl_cb, status);
}

static void
ut_fsdev_op_removexattr_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_ANAME,
			   UT_CALL_REC_MAX_STR_SIZE));
}

static void
ut_fsdev_test_op_removexattr(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_REMOVEXATTR, 0, 2, ut_fsdev_op_removexattr_execute_op_clb,
			 ut_fsdev_op_removexattr_check_op_clb);
}

static void
ut_fsdev_op_flush_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_flush_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_flush(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE,
				   ut_fsdev_op_flush_cpl_cb, status);
}

static void
ut_fsdev_op_flush_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
}

static void
ut_fsdev_test_op_flush(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_FLUSH, 0, 2, ut_fsdev_op_flush_execute_op_clb,
			 ut_fsdev_op_flush_check_op_clb);
}

static void
ut_fsdev_op_opendir_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			   struct spdk_fsdev_file_handle *fhandle)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fhandle == UT_FHANDLE);
	*clb_status = status;
}

static int
ut_fsdev_op_opendir_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				   struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_opendir(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, 0x1111,
				     ut_fsdev_op_opendir_cpl_cb, status);
}

static void
ut_fsdev_op_opendir_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == 0x1111);
}

static void
ut_fsdev_test_op_opendir(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_OPENDIR, 0, 2, ut_fsdev_op_opendir_execute_op_clb,
			 ut_fsdev_op_opendir_check_op_clb);
}

static int
ut_fsdev_op_readdir_entry_cb(void *cb_arg, struct spdk_io_channel *ch, const char *name,
			     struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
			     off_t offset)
{
	CU_ASSERT(!strcmp(name, UT_FNAME));
	CU_ASSERT(fobject == UT_FOBJECT + ut_readdir_num_entry_cb_calls);
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	CU_ASSERT(offset == (off_t)(ut_readdir_offset + ut_readdir_num_entry_cb_calls));

	ut_readdir_num_entry_cb_calls++;
	return (ut_readdir_num_entry_cb_calls == ut_readdir_num_entries) ? -1 : 0;
}

static void
ut_fsdev_op_readdir_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_readdir_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				   struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	ut_readdir_num_entries = 20;
	ut_readdir_num_entry_cb_calls = 0;
	ut_readdir_offset = (uint64_t)rand();
	memset(&ut_fsdev_attr, rand(), sizeof(ut_fsdev_attr));
	return spdk_fsdev_op_readdir(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, 10000,
				     ut_fsdev_op_readdir_entry_cb, ut_fsdev_op_readdir_cpl_cb, status);
}

static void
ut_fsdev_op_readdir_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 10000);
	CU_ASSERT(ut_calls_param_get_ptr(0,
					 UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == ut_fsdev_op_readdir_entry_cb);
	CU_ASSERT(ut_readdir_num_entry_cb_calls == ut_readdir_num_entries);
}

static void
ut_fsdev_test_op_readdir(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_READDIR, 0, 4, ut_fsdev_op_readdir_execute_op_clb,
			 ut_fsdev_op_readdir_check_op_clb);
}

static void
ut_fsdev_op_releasedir_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_releasedir_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				      struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_releasedir(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE,
					ut_fsdev_op_releasedir_cpl_cb, status);
}

static void
ut_fsdev_op_releasedir_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
}

static void
ut_fsdev_test_op_releasedir(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_RELEASEDIR, 0, 2, ut_fsdev_op_releasedir_execute_op_clb,
			 ut_fsdev_op_releasedir_check_op_clb);
}

static void
ut_fsdev_op_fsyncdir_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_fsyncdir_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				    struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_fsyncdir(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, true,
				      ut_fsdev_op_fsyncdir_cpl_cb, status);
}

static void
ut_fsdev_op_fsyncdir_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == true);
}

static void
ut_fsdev_test_op_fsyncdir(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_FSYNCDIR, 0, 3, ut_fsdev_op_fsyncdir_execute_op_clb,
			 ut_fsdev_op_fsyncdir_check_op_clb);
}

static void
ut_fsdev_op_flock_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_flock_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_flock(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, 111,
				   ut_fsdev_op_flock_cpl_cb, status);
}

static void
ut_fsdev_op_flock_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 111);
}

static void
ut_fsdev_test_op_flock(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_FLOCK, 0, 3, ut_fsdev_op_flock_execute_op_clb,
			 ut_fsdev_op_flock_check_op_clb);
}

static void
ut_fsdev_op_create_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
			  struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
			  struct spdk_fsdev_file_handle *fhandle)
{
	int *clb_status = cb_arg;
	CU_ASSERT(fobject == UT_FOBJECT + 1);
	CU_ASSERT(fhandle == UT_FHANDLE);
	CU_ASSERT(ut_hash(&ut_fsdev_attr, sizeof(ut_fsdev_attr)) == ut_hash(attr, sizeof(*attr)));
	*clb_status = status;
}

static int
ut_fsdev_op_create_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				  struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	memset(&ut_fsdev_attr, rand(), sizeof(ut_fsdev_attr));
	return spdk_fsdev_op_create(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_LNAME, 100, 0x2222, 0x666,
				    200, 300, ut_fsdev_op_create_cpl_cb, status);
}

static void
ut_fsdev_op_create_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(!strncmp(ut_calls_param_get_str(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1), UT_LNAME,
			   UT_CALL_REC_MAX_STR_SIZE));
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 100);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 0x2222);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 0x666);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 5) == 200);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 6) == 300);
}

static void
ut_fsdev_test_op_create(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_CREATE, 0, 7, ut_fsdev_op_create_execute_op_clb,
			 ut_fsdev_op_create_check_op_clb);
}

static void
ut_fsdev_op_abort_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_abort_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				 struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_abort(fsdev_desc, ch, UT_UNIQUE,
				   ut_fsdev_op_abort_cpl_cb, status);
}

static void
ut_fsdev_op_abort_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_UNIQUE);
}

static void
ut_fsdev_test_op_abort(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_ABORT, 0, 1, ut_fsdev_op_abort_execute_op_clb,
			 ut_fsdev_op_abort_check_op_clb);
}

static void
ut_fsdev_op_fallocate_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	int *clb_status = cb_arg;
	*clb_status = status;
}

static int
ut_fsdev_op_fallocate_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
				     struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_fallocate(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, 0x1111, 2000,
				       1002, ut_fsdev_op_fallocate_cpl_cb, status);
}

static void
ut_fsdev_op_fallocate_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 0x1111);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == 2000);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == 1002);
}

static void
ut_fsdev_test_op_fallocate(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_FALLOCATE, 0, 5, ut_fsdev_op_fallocate_execute_op_clb,
			 ut_fsdev_op_fallocate_check_op_clb);
}

static void
ut_fsdev_op_copy_file_range_cpl_cb(void *cb_arg, struct spdk_io_channel *ch, int status,
				   uint32_t data_size)
{
	int *clb_status = cb_arg;
	CU_ASSERT(data_size == UT_DATA_SIZE);
	*clb_status = status;
}

static int
ut_fsdev_op_copy_file_range_execute_op_clb(struct ut_fsdev *utfsdev, struct spdk_io_channel *ch,
		struct spdk_fsdev_desc *fsdev_desc, int *status)
{
	return spdk_fsdev_op_copy_file_range(fsdev_desc, ch, UT_UNIQUE, UT_FOBJECT, UT_FHANDLE, 1000,
					     UT_FOBJECT + 2, UT_FHANDLE + 2, 3000, 50000, 0x77777777,
					     ut_fsdev_op_copy_file_range_cpl_cb, status);
}

static void
ut_fsdev_op_copy_file_range_check_op_clb(void)
{
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS) == UT_FOBJECT);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 1) == UT_FHANDLE);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 2) == 1000);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 3) == UT_FOBJECT + 2);
	CU_ASSERT(ut_calls_param_get_ptr(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 4) == UT_FHANDLE + 2);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 5) == 3000);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 6) == 50000);
	CU_ASSERT(ut_calls_param_get_int(0, UT_SUBMIT_IO_NUM_COMMON_PARAMS + 7) == 0x77777777);
}

static void
ut_fsdev_test_op_copy_file_range(void)
{
	ut_fsdev_test_op(SPDK_FSDEV_OP_COPY_FILE_RANGE, 0, 8, ut_fsdev_op_copy_file_range_execute_op_clb,
			 ut_fsdev_op_copy_file_range_check_op_clb);
}

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("fsdev", ut_fsdev_setup, ut_fsdev_teardown);

	CU_ADD_TEST(suite, ut_fsdev_test_open_close);
	CU_ADD_TEST(suite, ut_fsdev_test_get_by_name);
	CU_ADD_TEST(suite, ut_fsdev_test_set_opts);
	CU_ADD_TEST(suite, ut_fsdev_test_set_device_opts);
	CU_ADD_TEST(suite, ut_fsdev_test_get_io_channel);
	CU_ADD_TEST(suite, ut_fsdev_test_op_lookup_ok);
	CU_ADD_TEST(suite, ut_fsdev_test_op_lookup_err);
	CU_ADD_TEST(suite, ut_fsdev_test_op_forget);
	CU_ADD_TEST(suite, ut_fsdev_test_op_getattr);
	CU_ADD_TEST(suite, ut_fsdev_test_op_setattr);
	CU_ADD_TEST(suite, ut_fsdev_test_op_readlink);
	CU_ADD_TEST(suite, ut_fsdev_test_op_symlink);
	CU_ADD_TEST(suite, ut_fsdev_test_op_mknod);
	CU_ADD_TEST(suite, ut_fsdev_test_op_mkdir);
	CU_ADD_TEST(suite, ut_fsdev_test_op_unlink);
	CU_ADD_TEST(suite, ut_fsdev_test_op_rmdir);
	CU_ADD_TEST(suite, ut_fsdev_test_op_rename);
	CU_ADD_TEST(suite, ut_fsdev_test_op_link);
	CU_ADD_TEST(suite, ut_fsdev_test_op_open);
	CU_ADD_TEST(suite, ut_fsdev_test_op_read);
	CU_ADD_TEST(suite, ut_fsdev_test_op_write);
	CU_ADD_TEST(suite, ut_fsdev_test_op_statfs);
	CU_ADD_TEST(suite, ut_fsdev_test_op_release);
	CU_ADD_TEST(suite, ut_fsdev_test_op_fsync);
	CU_ADD_TEST(suite, ut_fsdev_test_op_getxattr);
	CU_ADD_TEST(suite, ut_fsdev_test_op_setxattr);
	CU_ADD_TEST(suite, ut_fsdev_test_op_listxattr);
	CU_ADD_TEST(suite, ut_fsdev_test_op_listxattr_get_size);
	CU_ADD_TEST(suite, ut_fsdev_test_op_removexattr);
	CU_ADD_TEST(suite, ut_fsdev_test_op_flush);
	CU_ADD_TEST(suite, ut_fsdev_test_op_opendir);
	CU_ADD_TEST(suite, ut_fsdev_test_op_readdir);
	CU_ADD_TEST(suite, ut_fsdev_test_op_releasedir);
	CU_ADD_TEST(suite, ut_fsdev_test_op_fsyncdir);
	CU_ADD_TEST(suite, ut_fsdev_test_op_flock);
	CU_ADD_TEST(suite, ut_fsdev_test_op_create);
	CU_ADD_TEST(suite, ut_fsdev_test_op_abort);
	CU_ADD_TEST(suite, ut_fsdev_test_op_fallocate);
	CU_ADD_TEST(suite, ut_fsdev_test_op_copy_file_range);

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	poll_thread(0);

	free_threads();
	free_cores();

	return num_failures;
}
