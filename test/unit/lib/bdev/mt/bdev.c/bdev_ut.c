/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

#include "common/lib/bdev/common_stubs.h"

#define BDEV_UT_NUM_THREADS 3

DEFINE_STUB_V(spdk_accel_sequence_finish,
	      (struct spdk_accel_sequence *seq, spdk_accel_completion_cb cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_accel_sequence_abort, (struct spdk_accel_sequence *seq));
DEFINE_STUB_V(spdk_accel_sequence_reverse, (struct spdk_accel_sequence *seq));
DEFINE_STUB(spdk_accel_append_copy, int,
	    (struct spdk_accel_sequence **seq, struct spdk_io_channel *ch, struct iovec *dst_iovs,
	     uint32_t dst_iovcnt, struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, uint32_t src_iovcnt, struct spdk_memory_domain *src_domain,
	     void *src_domain_ctx, spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_append_dif_verify_copy, int,
	    (struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
	     struct iovec *dst_iovs, size_t dst_iovcnt,
	     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, size_t src_iovcnt,
	     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
	     uint32_t num_blocks,
	     const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
	     spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_append_dif_generate_copy, int,
	    (struct spdk_accel_sequence **seq,
	     struct spdk_io_channel *ch,
	     struct iovec *dst_iovs, size_t dst_iovcnt,
	     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, size_t src_iovcnt,
	     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
	     uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
	     spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_get_memory_domain, struct spdk_memory_domain *, (void), NULL);
DEFINE_STUB(spdk_accel_get_buf, int, (struct spdk_io_channel *ch, uint64_t len, void **buf,
				      struct spdk_memory_domain **domain, void **domain_ctx), 0);
DEFINE_STUB_V(spdk_accel_put_buf, (struct spdk_io_channel *ch, void *buf,
				   struct spdk_memory_domain *domain, void *domain_ctx));

DEFINE_RETURN_MOCK(spdk_memory_domain_pull_data, int);
int
spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_memory_domain_pull_data);

	cpl_cb(cpl_cb_arg, 0);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_memory_domain_push_data, int);
int
spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_memory_domain_push_data);

	cpl_cb(cpl_cb_arg, 0);
	return 0;
}

static int g_accel_io_device;

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&g_accel_io_device);
}

struct ut_bdev {
	struct spdk_bdev	bdev;
	void			*io_target;
};

struct ut_bdev_io {
	TAILQ_ENTRY(ut_bdev_io)		link;
};

struct ut_bdev_channel {
	TAILQ_HEAD(, ut_bdev_io)	outstanding_io;
	uint32_t			outstanding_cnt;
	uint32_t			avail_cnt;
	struct spdk_thread		*thread;
	TAILQ_ENTRY(ut_bdev_channel)	link;
};

struct ut_qos {
	struct spdk_bdev_qos_impl base;
	TAILQ_ENTRY(ut_qos) link;
};

struct ut_qos_channel {
	struct spdk_bdev_qos_channel_impl base;
	bdev_io_tailq_t queued_io;
	bool throttled;
	TAILQ_ENTRY(ut_qos_channel) link;
};

struct ut_qos_mgr {
	TAILQ_HEAD(, ut_qos) ut_qos_list;
};

int g_io_device;
struct ut_bdev g_bdev;
struct spdk_bdev_desc *g_desc;
struct ut_bdev *g_bdev2;
struct spdk_bdev_desc *g_desc2;
bool g_teardown_done = false;
bool g_get_io_channel = true;
bool g_create_ch = true;
bool g_init_complete_called = false;
bool g_fini_start_called = true;
int g_status = 0;
int g_count = 0;
struct spdk_histogram_data *g_histogram = NULL;
TAILQ_HEAD(, ut_bdev_channel) g_ut_channels;

struct ut_qos_mgr g_ut_qos_mgr = {
	.ut_qos_list = TAILQ_HEAD_INITIALIZER(g_ut_qos_mgr.ut_qos_list),
};

static int
ut_accel_ch_create_cb(void *io_device, void *ctx)
{
	return 0;
}

static void
ut_accel_ch_destroy_cb(void *io_device, void *ctx)
{
}

static int
stub_create_ch(void *io_device, void *ctx_buf)
{
	struct ut_bdev_channel *ch = ctx_buf;

	if (g_create_ch == false) {
		return -1;
	}

	TAILQ_INIT(&ch->outstanding_io);
	ch->outstanding_cnt = 0;
	/*
	 * When avail gets to 0, the submit_request function will return ENOMEM.
	 *  Most tests to not want ENOMEM to occur, so by default set this to a
	 *  big value that won't get hit.  The ENOMEM tests can then override this
	 *  value to something much smaller to induce ENOMEM conditions.
	 */
	ch->avail_cnt = 2048;
	ch->thread = spdk_get_thread();

	TAILQ_INSERT_TAIL(&g_ut_channels, ch, link);

	return 0;
}

static void
stub_destroy_ch(void *io_device, void *ctx_buf)
{
	struct ut_bdev_channel *ch = ctx_buf;

	TAILQ_REMOVE(&g_ut_channels, ch, link);
}

static struct spdk_io_channel *
stub_get_io_channel(void *ctx)
{
	struct ut_bdev *ut_bdev = ctx;

	if (g_get_io_channel == true) {
		return spdk_get_io_channel(ut_bdev->io_target);
	} else {
		return NULL;
	}
}

static int
stub_destruct(void *ctx)
{
	return 0;
}

static void
stub_reset_channel(void *ctx)
{
	struct ut_bdev_channel *ch = ctx;
	struct ut_bdev_io *bio;

	while (!TAILQ_EMPTY(&ch->outstanding_io)) {
		bio = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, bio, link);
		ch->outstanding_cnt--;
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_ABORTED);
		ch->avail_cnt++;
	}
}

static void
stub_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch), *tmp_ch;
	struct spdk_bdev_io *io;
	struct ut_bdev_io *bio;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		TAILQ_FOREACH(tmp_ch, &g_ut_channels, link) {
			if (spdk_get_thread() == tmp_ch->thread) {
				stub_reset_channel(tmp_ch);
			} else {
				spdk_thread_send_msg(tmp_ch->thread, stub_reset_channel, tmp_ch);
			}
		}
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT) {
		TAILQ_FOREACH(bio, &ch->outstanding_io, link) {
			io = spdk_bdev_io_from_ctx(bio);
			if (io == bdev_io->u.abort.bio_to_abort) {
				TAILQ_REMOVE(&ch->outstanding_io, bio, link);
				ch->outstanding_cnt--;
				spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_ABORTED);
				ch->avail_cnt++;

				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
				return;
			}
		}

		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (ch->avail_cnt > 0) {
		TAILQ_INSERT_TAIL(&ch->outstanding_io, (struct ut_bdev_io *)bdev_io->driver_ctx, link);
		ch->outstanding_cnt++;
		ch->avail_cnt--;
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	}
}

static uint32_t
stub_complete_io(void *io_target, uint32_t num_to_complete)
{
	struct spdk_io_channel *_ch = spdk_get_io_channel(io_target);
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct ut_bdev_io *bio;
	struct spdk_bdev_io *io;
	bool complete_all = (num_to_complete == 0);
	uint32_t num_completed = 0;

	while (complete_all || num_completed < num_to_complete) {
		if (TAILQ_EMPTY(&ch->outstanding_io)) {
			break;
		}
		bio = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, bio, link);
		io = spdk_bdev_io_from_ctx(bio);
		ch->outstanding_cnt--;
		spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_SUCCESS);
		ch->avail_cnt++;
		num_completed++;
	}
	spdk_put_io_channel(_ch);
	return num_completed;
}

static bool
stub_io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	return true;
}

static struct spdk_bdev_fn_table fn_table = {
	.get_io_channel =	stub_get_io_channel,
	.destruct =		stub_destruct,
	.submit_request =	stub_submit_request,
	.io_type_supported =	stub_io_type_supported,
};

struct spdk_bdev_module bdev_ut_if;

static int
module_init(void)
{
	spdk_bdev_module_init_done(&bdev_ut_if);
	return 0;
}

static void
module_fini(void)
{
}

static void
init_complete(void)
{
	g_init_complete_called = true;
}

static void
fini_start(void)
{
	g_fini_start_called = true;
}

static int
get_ctx_size(void)
{
	return sizeof(struct ut_bdev_io);
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = module_init,
	.module_fini = module_fini,
	.async_init = true,
	.init_complete = init_complete,
	.fini_start = fini_start,
	.get_ctx_size = get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(bdev_ut, &bdev_ut_if)

static void
register_bdev(struct ut_bdev *ut_bdev, char *name, void *io_target)
{
	memset(ut_bdev, 0, sizeof(*ut_bdev));

	ut_bdev->io_target = io_target;
	ut_bdev->bdev.ctxt = ut_bdev;
	ut_bdev->bdev.name = name;
	ut_bdev->bdev.fn_table = &fn_table;
	ut_bdev->bdev.module = &bdev_ut_if;
	ut_bdev->bdev.blocklen = 4096;
	ut_bdev->bdev.blockcnt = 1024;

	spdk_bdev_register(&ut_bdev->bdev);
}

static void
unregister_bdev(struct ut_bdev *ut_bdev)
{
	/* Handle any deferred messages. */
	poll_threads();
	spdk_bdev_unregister(&ut_bdev->bdev, NULL, NULL);
	/* Handle the async bdev unregister. */
	poll_threads();
}

static void
bdev_init_cb(void *done, int rc)
{
	CU_ASSERT(rc == 0);
	*(bool *)done = true;
}

static void
_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	       void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		if (event_ctx != NULL) {
			*(bool *)event_ctx = true;
		}
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		if (event_ctx != NULL) {
			*(int *)event_ctx += 1;
		}
		break;
	default:
		CU_ASSERT(false);
		break;
	}
}

static void
setup_bdev2(void *io_device)
{
	int rc;

	g_bdev2 = calloc(1, sizeof(*g_bdev2));
	SPDK_CU_ASSERT_FATAL(g_bdev2 != NULL);

	register_bdev(g_bdev2, "ut_bdev2", io_device);
	rc = spdk_bdev_open_ext("ut_bdev2", true, _bdev_event_cb, NULL, &g_desc2);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_desc2 != NULL);
}

static void
teardown_bdev2(void)
{
	spdk_bdev_close(g_desc2);
	g_desc2 = NULL;
	unregister_bdev(g_bdev2);
	free(g_bdev2);
	g_bdev2 = NULL;
}

static struct spdk_bdev_qos_module ut_qos_if;
static struct spdk_bdev_qos_module ut_qos2_if;

static int
ut_qos_library_init(void)
{
	return 0;
}

static void
ut_qos_library_fini(void)
{
	spdk_bdev_qos_module_fini_done();
}

static void
ut_qos_library_config_json(struct spdk_json_write_ctx *w)
{
}

static struct spdk_bdev_qos_impl *
ut_qos_get(void)
{
	struct ut_qos *ut_qos;

	ut_qos = calloc(1, sizeof(*ut_qos));
	SPDK_CU_ASSERT_FATAL(ut_qos != NULL);

	ut_qos->base.module = &ut_qos_if;

	TAILQ_INSERT_TAIL(&g_ut_qos_mgr.ut_qos_list, ut_qos, link);

	return &ut_qos->base;
}

static struct spdk_bdev_qos_impl *
ut_qos2_get(void)
{
	struct ut_qos *ut_qos;

	ut_qos = calloc(1, sizeof(*ut_qos));
	SPDK_CU_ASSERT_FATAL(ut_qos != NULL);

	ut_qos->base.module = &ut_qos2_if;

	TAILQ_INSERT_TAIL(&g_ut_qos_mgr.ut_qos_list, ut_qos, link);

	return &ut_qos->base;
}

static void
ut_qos_put(struct spdk_bdev_qos_impl *qos_impl)
{
	struct ut_qos *ut_qos = SPDK_CONTAINEROF(qos_impl, struct ut_qos, base);

	TAILQ_REMOVE(&g_ut_qos_mgr.ut_qos_list, ut_qos, link);

	free(ut_qos);
}

static void
ut_qos_config_json(struct spdk_bdev_qos_impl *qos_impl, struct spdk_json_write_ctx *w)
{
}

static void
ut_qos_info_json(struct spdk_bdev_qos_impl *qos_impl, struct spdk_json_write_ctx *w)
{
}

static struct spdk_bdev_qos_channel_impl *
ut_qos_channel_get(struct spdk_bdev_qos_impl *qos_impl)
{
	struct ut_qos_channel *ut_qos_ch;

	CU_ASSERT(qos_impl->module == &ut_qos_if);

	ut_qos_ch = calloc(1, sizeof(*ut_qos_ch));
	SPDK_CU_ASSERT_FATAL(ut_qos_ch != NULL);

	ut_qos_ch->base.qos_impl = qos_impl;

	TAILQ_INIT(&ut_qos_ch->queued_io);
	ut_qos_ch->throttled = false;

	return &ut_qos_ch->base;
}

static struct spdk_bdev_qos_channel_impl *
ut_qos2_channel_get(struct spdk_bdev_qos_impl *qos_impl)
{
	struct ut_qos_channel *ut_qos_ch;

	CU_ASSERT(qos_impl->module == &ut_qos2_if);

	ut_qos_ch = calloc(1, sizeof(*ut_qos_ch));
	SPDK_CU_ASSERT_FATAL(ut_qos_ch != NULL);

	ut_qos_ch->base.qos_impl = qos_impl;

	TAILQ_INIT(&ut_qos_ch->queued_io);
	ut_qos_ch->throttled = false;

	return &ut_qos_ch->base;
}

static void
ut_qos_channel_put(struct spdk_bdev_qos_channel_impl *qos_ch_impl)
{
	struct ut_qos_channel *ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	free(ut_qos_ch);
}

static void
ut_qos_channel_queue_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
			struct spdk_bdev_io *bdev_io)
{
	struct ut_qos_channel *ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	if (!ut_qos_ch->throttled) {
		spdk_bdev_qos_channel_impl_queue_io_done(bdev_io);
	} else {
		TAILQ_INSERT_TAIL(&ut_qos_ch->queued_io, bdev_io, internal.link);
	}
}

static bool
ut_qos_channel_abort_queued_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
			       struct spdk_bdev_io *bio_to_abort)
{
	struct ut_qos_channel *ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	return spdk_bdev_abort_queued_io(&ut_qos_ch->queued_io, bio_to_abort);
}

static void
ut_qos_channel_abort_all_queued_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
				   struct spdk_bdev_channel *bdev_ch)
{
	struct ut_qos_channel *ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	spdk_bdev_abort_all_queued_io(&ut_qos_ch->queued_io, bdev_ch);
}

static void
ut_qos_channel_unblock_all_queued_io(struct spdk_bdev_qos_channel_impl *qos_ch_impl,
				     struct spdk_bdev_channel *bdev_ch)
{
	struct ut_qos_channel *ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	return spdk_bdev_unblock_all_queued_io(&ut_qos_ch->queued_io, bdev_ch);
}

static bool
ut_qos_channel_is_throttled(struct spdk_bdev_qos_channel_impl *qos_ch_impl)
{
	struct ut_qos_channel *ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	return ut_qos_ch->throttled;
}

static struct spdk_bdev_qos_module ut_qos_if = {
	.name = "ut_qos",
	.module_init = ut_qos_library_init,
	.module_fini = ut_qos_library_fini,
	.module_config_json = ut_qos_library_config_json,
	.get_impl = ut_qos_get,
	.put_impl = ut_qos_put,
	.impl_config_json = ut_qos_config_json,
	.impl_info_json = ut_qos_info_json,
	.get_channel_impl = ut_qos_channel_get,
	.put_channel_impl = ut_qos_channel_put,
	.queue_io = ut_qos_channel_queue_io,
	.abort_queued_io = ut_qos_channel_abort_queued_io,
	.abort_all_queued_io = ut_qos_channel_abort_all_queued_io,
	.unblock_all_queued_io = ut_qos_channel_unblock_all_queued_io,
	.is_throttled = ut_qos_channel_is_throttled,
	.async_fini = true,
};

static struct spdk_bdev_qos_module ut_qos2_if = {
	.name = "ut_qos2",
	.module_init = ut_qos_library_init,
	.module_fini = ut_qos_library_fini,
	.module_config_json = ut_qos_library_config_json,
	.get_impl = ut_qos2_get,
	.put_impl = ut_qos_put,
	.impl_config_json = ut_qos_config_json,
	.impl_info_json = ut_qos_info_json,
	.get_channel_impl = ut_qos2_channel_get,
	.put_channel_impl = ut_qos_channel_put,
	.queue_io = ut_qos_channel_queue_io,
	.abort_queued_io = ut_qos_channel_abort_queued_io,
	.abort_all_queued_io = ut_qos_channel_abort_all_queued_io,
	.unblock_all_queued_io = ut_qos_channel_unblock_all_queued_io,
	.is_throttled = ut_qos_channel_is_throttled,
	.async_fini = true,
};

SPDK_BDEV_QOS_MODULE_REGISTER(ut_qos, &ut_qos_if)
SPDK_BDEV_QOS_MODULE_REGISTER(ut_qos2, &ut_qos2_if)

static void
setup_test(void)
{
	bool done = false;
	int rc;

	TAILQ_INIT(&g_ut_channels);

	allocate_cores(BDEV_UT_NUM_THREADS);
	allocate_threads(BDEV_UT_NUM_THREADS);
	set_thread(0);

	rc = spdk_iobuf_initialize();
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, &done);
	spdk_io_device_register(&g_io_device, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);
	spdk_io_device_register(&g_accel_io_device, ut_accel_ch_create_cb,
				ut_accel_ch_destroy_cb, 0, NULL);
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
}

static void
finish_cb(void *cb_arg)
{
	g_teardown_done = true;
}

static void
teardown_test(void)
{
	set_thread(0);
	g_teardown_done = false;
	spdk_bdev_close(g_desc);
	g_desc = NULL;
	unregister_bdev(&g_bdev);
	spdk_io_device_unregister(&g_io_device, NULL);
	spdk_bdev_finish(finish_cb, NULL);
	spdk_io_device_unregister(&g_accel_io_device, NULL);
	spdk_iobuf_finish(finish_cb, NULL);
	poll_threads();
	memset(&g_bdev, 0, sizeof(g_bdev));
	CU_ASSERT(g_teardown_done == true);
	g_teardown_done = false;
	free_threads();
	free_cores();
	CU_ASSERT(TAILQ_EMPTY(&g_ut_channels))
}

static uint32_t
bdev_io_tailq_cnt(bdev_io_tailq_t *tailq)
{
	struct spdk_bdev_io *io;
	uint32_t cnt = 0;

	TAILQ_FOREACH(io, tailq, internal.link) {
		cnt++;
	}

	return cnt;
}

static void
basic(void)
{
	g_init_complete_called = false;
	setup_test();
	CU_ASSERT(g_init_complete_called == true);

	set_thread(0);

	g_get_io_channel = false;
	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(g_ut_threads[0].ch == NULL);

	g_get_io_channel = true;
	g_create_ch = false;
	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(g_ut_threads[0].ch == NULL);

	g_get_io_channel = true;
	g_create_ch = true;
	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(g_ut_threads[0].ch != NULL);
	spdk_put_io_channel(g_ut_threads[0].ch);

	g_fini_start_called = false;
	teardown_test();
	CU_ASSERT(g_fini_start_called == true);
}

static void
_bdev_unregistered(void *done, int rc)
{
	CU_ASSERT(rc == 0);
	*(bool *)done = true;
}

static void
unregister_and_close(void)
{
	bool done, remove_notify;
	struct spdk_bdev_desc *desc = NULL;

	setup_test();
	set_thread(0);

	/* setup_test() automatically opens the bdev,
	 * but this test needs to do that in a different
	 * way. */
	spdk_bdev_close(g_desc);
	poll_threads();

	/* Try hotremoving a bdev with descriptors which don't provide
	 * any context to the notification callback */
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &desc);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	/* There is an open descriptor on the device. Unregister it,
	 * which can't proceed until the descriptor is closed. */
	done = false;
	spdk_bdev_unregister(&g_bdev.bdev, _bdev_unregistered, &done);

	/* Poll the threads to allow all events to be processed */
	poll_threads();

	/* Make sure the bdev was not unregistered. We still have a
	 * descriptor open */
	CU_ASSERT(done == false);

	spdk_bdev_close(desc);
	poll_threads();
	desc = NULL;

	/* The unregister should have completed */
	CU_ASSERT(done == true);


	/* Register the bdev again */
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);

	remove_notify = false;
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, &remove_notify, &desc);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(remove_notify == false);

	/* There is an open descriptor on the device. Unregister it,
	 * which can't proceed until the descriptor is closed. */
	done = false;
	spdk_bdev_unregister(&g_bdev.bdev, _bdev_unregistered, &done);
	/* No polling has occurred, so neither of these should execute */
	CU_ASSERT(remove_notify == false);
	CU_ASSERT(done == false);

	/* Prior to the unregister completing, close the descriptor */
	spdk_bdev_close(desc);

	/* Poll the threads to allow all events to be processed */
	poll_threads();

	/* Remove notify should not have been called because the
	 * descriptor is already closed. */
	CU_ASSERT(remove_notify == false);

	/* The unregister should have completed */
	CU_ASSERT(done == true);

	/* Restore the original g_bdev so that we can use teardown_test(). */
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
	teardown_test();
}

static void
unregister_and_close_different_threads(void)
{
	bool done;
	struct spdk_bdev_desc *desc = NULL;

	setup_test();
	set_thread(0);

	/* setup_test() automatically opens the bdev,
	 * but this test needs to do that in a different
	 * way. */
	spdk_bdev_close(g_desc);
	poll_threads();

	set_thread(1);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &desc);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	done = false;

	set_thread(0);
	spdk_bdev_unregister(&g_bdev.bdev, _bdev_unregistered, &done);

	/* Poll the threads to allow all events to be processed */
	poll_threads();

	/* Make sure the bdev was not unregistered. We still have a
	 * descriptor open */
	CU_ASSERT(done == false);

	/* Close the descriptor on thread 1.  Poll the thread and confirm the
	 * unregister did not complete, since it was unregistered on thread 0.
	 */
	set_thread(1);
	spdk_bdev_close(desc);
	poll_thread(1);
	CU_ASSERT(done == false);

	/* Now poll thread 0 and confirm the unregister completed. */
	set_thread(0);
	poll_thread(0);
	CU_ASSERT(done == true);

	/* Restore the original g_bdev so that we can use teardown_test(). */
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
	teardown_test();
}

static void
reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	bool *done = cb_arg;

	CU_ASSERT(success == true);
	*done = true;
	spdk_bdev_free_io(bdev_io);
}

static void
put_channel_during_reset(void)
{
	struct spdk_io_channel *io_ch;
	bool done = false;
	uint32_t num_completed;

	setup_test();

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch != NULL);

	/*
	 * Start a reset, but then put the I/O channel before
	 *  the deferred messages for the reset get a chance to
	 *  execute.
	 */
	spdk_bdev_reset(g_desc, io_ch, reset_done, &done);
	spdk_put_io_channel(io_ch);
	poll_threads();

	/* Complete the reset. */
	num_completed = stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(num_completed == 1);
	poll_threads();
	CU_ASSERT(done == true);

	teardown_test();
}

static void
aborted_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_free_io(bdev_io);
}

static void io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void
aborted_reset(void)
{
	struct spdk_io_channel *io_ch[2];
	enum spdk_bdev_io_status status1 = SPDK_BDEV_IO_STATUS_PENDING,
				 status2 = SPDK_BDEV_IO_STATUS_PENDING;

	setup_test();

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch[0] != NULL);
	spdk_bdev_reset(g_desc, io_ch[0], aborted_reset_done, &status1);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress != NULL);

	/*
	 * First reset has been submitted on ch0.  Now submit a second
	 *  reset on ch1 which will get queued since there is already a
	 *  reset in progress.
	 */
	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch[1] != NULL);
	spdk_bdev_reset(g_desc, io_ch[1], aborted_reset_done, &status2);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress != NULL);

	/*
	 * Now destroy ch1.  Nothing would really happen because the pending second reset
	 *  is still holding a reference of ch1.
	 */
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress != NULL);

	/*
	 * Now complete the first reset, verify that both resets completed with SUCCESS
	 *  status and that bdev->internal.reset_in_progress is also set back to NULL.
	 */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);

	/*
	 * Teardown should succeed.
	 */
	teardown_test();
}

static void
aborted_reset_no_outstanding_io(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	struct spdk_bdev *bdev[2];
	enum spdk_bdev_io_status status1 = SPDK_BDEV_IO_STATUS_PENDING,
				 status2 = SPDK_BDEV_IO_STATUS_PENDING;

	setup_test();

	/*
	 * This time we test the reset without any outstanding IO
	 * present on the bdev channel, so both resets should finish
	 * immediately.
	 */

	set_thread(0);
	/* Set reset_io_drain_timeout to allow bdev
	 * reset to stay pending until we call abort. */
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	bdev[0] = bdev_ch[0]->bdev;
	bdev[0]->reset_io_drain_timeout = SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE;
	CU_ASSERT(io_ch[0] != NULL);
	spdk_bdev_reset(g_desc, io_ch[0], aborted_reset_done, &status1);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);
	spdk_put_io_channel(io_ch[0]);

	set_thread(1);
	/* Set reset_io_drain_timeout to allow bdev
	 * reset to stay pending until we call abort. */
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	bdev[1] = bdev_ch[1]->bdev;
	bdev[1]->reset_io_drain_timeout = SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE;
	CU_ASSERT(io_ch[1] != NULL);
	spdk_bdev_reset(g_desc, io_ch[1], aborted_reset_done, &status2);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_SUCCESS);
	spdk_put_io_channel(io_ch[1]);

	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();

	teardown_test();
}


static void
io_during_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = bdev_io->internal.status;
	spdk_bdev_free_io(bdev_io);
}

static void
io_during_reset(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	enum spdk_bdev_io_status status0, status1, status_reset;
	int rc;

	setup_test();

	/*
	 * First test normal case - submit an I/O on each of two channels (with no resets)
	 *  and verify they complete successfully.
	 */
	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == 0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == 0);
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status1);
	CU_ASSERT(rc == 0);

	poll_threads();
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_PENDING);

	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_SUCCESS);

	set_thread(1);
	stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Now submit a reset, and leave it pending while we submit I/O on two different
	 *  channels.  These I/O should be failed by the bdev layer since the reset is in
	 *  progress.
	 */
	set_thread(0);
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch[0], io_during_io_done, &status_reset);
	CU_ASSERT(rc == 0);

	CU_ASSERT(bdev_ch[0]->flags == 0);
	CU_ASSERT(bdev_ch[1]->flags == 0);
	poll_threads();
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_RESET_IN_PROGRESS);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_RESET_IN_PROGRESS);

	set_thread(0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	set_thread(1);
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status1);
	CU_ASSERT(rc == 0);

	/*
	 * A reset is in progress so these read I/O should complete with aborted.  Note that we
	 *  need to poll_threads() since I/O completed inline have their completion deferred.
	 */
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_ABORTED);

	/*
	 * Complete the reset
	 */
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);

	/*
	 * Only poll thread 0. We should not get a completion.
	 */
	poll_thread(0);
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);

	/*
	 * Poll both thread 0 and 1 so the messages can propagate and we
	 * get a completion.
	 */
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);

	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();

	teardown_test();
}

static uint32_t
count_queued_resets(void *io_target)
{
	struct spdk_io_channel *_ch = spdk_get_io_channel(io_target);
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct ut_bdev_io *bio;
	struct spdk_bdev_io *io;
	uint32_t submitted_resets = 0;

	TAILQ_FOREACH(bio, &ch->outstanding_io, link) {
		io = spdk_bdev_io_from_ctx(bio);
		if (io->type == SPDK_BDEV_IO_TYPE_RESET) {
			submitted_resets++;
		}
	}

	spdk_put_io_channel(_ch);

	return submitted_resets;
}

static void
reset_completions(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev *bdev;
	enum spdk_bdev_io_status status0, status_reset;
	int rc, iter;

	setup_test();

	/* This test covers four test cases:
	 * 1) reset_io_drain_timeout of a bdev is greater than 0
	 * 2) No outstandind IO are present on any bdev channel
	 * 3) Outstanding IO finish during bdev reset
	 * 4) Outstanding IO do not finish before reset is done waiting
	 *    for them.
	 *
	 * Above conditions mainly affect the timing of bdev reset completion
	 * and whether a reset should be skipped via spdk_bdev_io_complete()
	 * or sent down to the underlying bdev module via bdev_io_submit_reset(). */

	/* Test preparation */
	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	CU_ASSERT(bdev_ch->flags == 0);


	/* Test case 1) reset_io_drain_timeout set to 0. Reset should be sent down immediately. */
	bdev = &g_bdev.bdev;
	bdev->reset_io_drain_timeout = 0;

	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch, io_during_io_done, &status_reset);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 1);

	/* Call reset completion inside bdev module. */
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);


	/* Test case 2) no outstanding IO are present. Reset should perform one iteration over
	* channels and then be skipped. */
	bdev->reset_io_drain_timeout = SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE;
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;

	rc = spdk_bdev_reset(g_desc, io_ch, io_during_io_done, &status_reset);
	CU_ASSERT(rc == 0);
	poll_threads();
	/* Reset was never submitted to the bdev module. */
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);


	/* Test case 3) outstanding IO finish during bdev reset procedure. Reset should initiate
	* wait poller to check for IO completions every second, until reset_io_drain_timeout is
	* reached, but finish earlier than this threshold. */
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	rc = spdk_bdev_reset(g_desc, io_ch, io_during_io_done, &status_reset);
	CU_ASSERT(rc == 0);
	poll_threads();
	/* The reset just started and should not have been submitted yet. */
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);

	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);
	/* Let the poller wait for about half the time then complete outstanding IO. */
	for (iter = 0; iter < 2; iter++) {
		/* Reset is still processing and not submitted at this point. */
		CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);
		spdk_delay_us(1000 * 1000);
		poll_threads();
		poll_threads();
	}
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	spdk_delay_us(BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD_IN_USEC);
	poll_threads();
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* Sending reset to the bdev module has been skipped. */
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);


	/* Test case 4) outstanding IO are still present after reset_io_drain_timeout
	* seconds have passed. */
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	rc = spdk_bdev_reset(g_desc, io_ch, io_during_io_done, &status_reset);
	CU_ASSERT(rc == 0);
	poll_threads();
	/* The reset just started and should not have been submitted yet. */
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);

	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);
	/* Let the poller wait for reset_io_drain_timeout seconds. */
	for (iter = 0; iter < bdev->reset_io_drain_timeout; iter++) {
		CU_ASSERT(count_queued_resets(g_bdev.io_target) == 0);
		spdk_delay_us(BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD_IN_USEC);
		poll_threads();
		poll_threads();
	}

	/* After timing out, the reset should have been sent to the module. */
	CU_ASSERT(count_queued_resets(g_bdev.io_target) == 1);
	/* Complete reset submitted to the module and the read IO. */
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);


	/* Destroy the channel and end the test. */
	spdk_put_io_channel(io_ch);
	poll_threads();

	teardown_test();
}

static void
enomem_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_free_io(bdev_io);
}

static void
enomem(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE], status_reset;
	uint32_t nomem_cnt, i;
	struct spdk_bdev_io *first_io;
	int rc;

	setup_test();

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	shared_resource = bdev_ch->shared_resource;
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	/* First submit a number of IOs equal to what the channel can support. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&shared_resource->nomem_io));

	/*
	 * Next, submit one additional I/O.  This one should fail with ENOMEM and then go onto
	 *  the enomem_io list.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&shared_resource->nomem_io));
	first_io = TAILQ_FIRST(&shared_resource->nomem_io);

	/*
	 * Now submit a bunch more I/O.  These should all fail with ENOMEM and get queued behind
	 *  the first_io above.
	 */
	for (i = AVAIL + 1; i < IO_ARRAY_SIZE; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}

	/* Assert that first_io is still at the head of the list. */
	CU_ASSERT(TAILQ_FIRST(&shared_resource->nomem_io) == first_io);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == (IO_ARRAY_SIZE - AVAIL));
	nomem_cnt = bdev_io_tailq_cnt(&shared_resource->nomem_io);
	CU_ASSERT(shared_resource->nomem_threshold == (AVAIL - NOMEM_THRESHOLD_COUNT));

	/*
	 * Complete 1 I/O only.  The key check here is bdev_io_tailq_cnt - this should not have
	 *  changed since completing just 1 I/O should not trigger retrying the queued nomem_io
	 *  list.
	 */
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == nomem_cnt);

	/*
	 * Complete enough I/O to hit the nomem_threshold.  This should trigger retrying nomem_io,
	 *  and we should see I/O get resubmitted to the test bdev module.
	 */
	stub_complete_io(g_bdev.io_target, NOMEM_THRESHOLD_COUNT - 1);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) < nomem_cnt);
	nomem_cnt = bdev_io_tailq_cnt(&shared_resource->nomem_io);

	/* Complete 1 I/O only.  This should not trigger retrying the queued nomem_io. */
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == nomem_cnt);

	/*
	 * Send a reset and confirm that all I/O are completed, including the ones that
	 *  were queued on the nomem_io list.
	 */
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch, enomem_done, &status_reset);
	poll_threads();
	CU_ASSERT(rc == 0);
	/* This will complete the reset. */
	stub_complete_io(g_bdev.io_target, 0);

	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == 0);
	CU_ASSERT(shared_resource->io_outstanding == 0);

	spdk_put_io_channel(io_ch);
	poll_threads();
	teardown_test();
}

static void
enomem_multi_bdev(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE];
	uint32_t i;
	struct spdk_bdev_channel *bdev_ch2;
	struct spdk_io_channel *io_ch2;
	int rc;

	setup_test();

	/* Register second bdev with the same io_target  */
	setup_bdev2(g_bdev.io_target);

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	shared_resource = bdev_ch->shared_resource;
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	io_ch2 = spdk_bdev_get_io_channel(g_desc2);
	bdev_ch2 = spdk_io_channel_get_ctx(io_ch2);
	SPDK_CU_ASSERT_FATAL(shared_resource == bdev_ch2->shared_resource);

	/* Saturate io_target through bdev A. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&shared_resource->nomem_io));

	/*
	 * Now submit I/O through the second bdev. This should fail with ENOMEM
	 * and then go onto the nomem_io list.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc2, io_ch2, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&shared_resource->nomem_io));

	/* Complete first bdev's I/O. This should retry sending second bdev's nomem_io */
	stub_complete_io(g_bdev.io_target, AVAIL);

	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&shared_resource->nomem_io));
	CU_ASSERT(shared_resource->io_outstanding == 1);

	/* Now complete our retried I/O  */
	stub_complete_io(g_bdev.io_target, 1);
	SPDK_CU_ASSERT_FATAL(shared_resource->io_outstanding == 0);

	spdk_put_io_channel(io_ch);
	spdk_put_io_channel(io_ch2);
	teardown_bdev2();
	poll_threads();
	teardown_test();
}

static void
enomem_multi_bdev_unregister(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE];
	uint32_t i;
	int rc;

	setup_test();

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	shared_resource = bdev_ch->shared_resource;
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	/* Saturate io_target through the bdev. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&shared_resource->nomem_io));

	/*
	 * Now submit I/O through the bdev. This should fail with ENOMEM
	 * and then go onto the nomem_io list.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&shared_resource->nomem_io));

	/* Unregister the bdev to abort the IOs from nomem_io queue. */
	unregister_bdev(&g_bdev);
	CU_ASSERT(status[AVAIL] == SPDK_BDEV_IO_STATUS_FAILED);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&shared_resource->nomem_io));
	SPDK_CU_ASSERT_FATAL(shared_resource->io_outstanding == AVAIL);

	/* Complete the bdev's I/O. */
	stub_complete_io(g_bdev.io_target, AVAIL);
	SPDK_CU_ASSERT_FATAL(shared_resource->io_outstanding == 0);

	spdk_put_io_channel(io_ch);
	poll_threads();
	teardown_test();
}

static void
enomem_multi_io_target(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE];
	uint32_t i;
	int new_io_device;
	struct spdk_bdev_channel *bdev_ch2;
	struct spdk_io_channel *io_ch2;
	int rc;

	setup_test();

	/* Create new io_target and a second bdev using it */
	spdk_io_device_register(&new_io_device, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);
	setup_bdev2(&new_io_device);

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	/* Different io_target will share per-thread shared_resource */
	io_ch2 = spdk_bdev_get_io_channel(g_desc2);
	bdev_ch2 = spdk_io_channel_get_ctx(io_ch2);
	SPDK_CU_ASSERT_FATAL(bdev_ch->shared_resource == bdev_ch2->shared_resource);

	/* Saturate io_target through bdev A. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));

	/* Issue one more I/O to fill ENOMEM list. */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));

	/*
	 * Now submit I/O through the second bdev. The IO should be queued due to
	 * per-thread shared_resource is used.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc2, io_ch2, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&bdev_ch2->shared_resource->nomem_io));

	/* Cleanup; Complete outstanding I/O. */
	stub_complete_io(g_bdev.io_target, AVAIL);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));
	/* Complete the ENOMEM I/O */
	stub_complete_io(g_bdev.io_target, 1);
	stub_complete_io(g_bdev2->io_target, 1);
	CU_ASSERT(bdev_ch->shared_resource->io_outstanding == 0);

	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));
	CU_ASSERT(bdev_ch->shared_resource->io_outstanding == 0);
	spdk_put_io_channel(io_ch);
	spdk_put_io_channel(io_ch2);
	teardown_bdev2();
	spdk_io_device_unregister(&new_io_device, NULL);
	poll_threads();
	teardown_test();
}

static void
enomem_retry_during_abort(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 16;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE], status_reset;
	uint32_t i;
	int rc;

	setup_test();

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	shared_resource = bdev_ch->shared_resource;
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = 0;

	/**
	 * Submit a number of IOs.
	 * All of these I/Os will queue in nomem_io list due to ut_ch->avail_cnt == 0.
	 */
	for (i = 0; i < IO_ARRAY_SIZE; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == IO_ARRAY_SIZE);
	CU_ASSERT(shared_resource->io_outstanding == 0);

	/* Allow some I/Os to be submitted. */
	ut_ch->avail_cnt = IO_ARRAY_SIZE / 2;

	/* Submit a reset to abort the I/Os. */
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch, enomem_done, &status_reset);
	poll_threads();
	CU_ASSERT(rc == 0);

	/* Complete the reset. */
	stub_complete_io(g_bdev.io_target, 1);
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* All I/Os are aborted. */
	for (i = 0; i < IO_ARRAY_SIZE; i++) {
		CU_ASSERT(status[i] == SPDK_BDEV_IO_STATUS_FAILED);
	}

	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == 0);
	CU_ASSERT(shared_resource->io_outstanding == 0);

	spdk_put_io_channel(io_ch);
	poll_threads();
	teardown_test();
}

static void
histogram_status_cb(void *cb_arg, int status)
{
	g_status = status;
}

static void
histogram_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	g_status = status;
	g_histogram = histogram;
}

static void
histogram_io_count(void *ctx, uint64_t start, uint64_t end, uint64_t count,
		   uint64_t total, uint64_t so_far)
{
	g_count += count;
}

static void
bdev_histograms_mt(void)
{
	struct spdk_io_channel *ch[2];
	struct spdk_histogram_data *histogram;
	uint8_t buf[4096];
	int status = false;
	int rc;


	setup_test();

	set_thread(0);
	ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[0] != NULL);

	set_thread(1);
	ch[1] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[1] != NULL);


	/* Enable histogram */
	spdk_bdev_histogram_enable(&g_bdev.bdev, histogram_status_cb, NULL, true);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(g_bdev.bdev.internal.histogram_enabled == true);

	/* Allocate histogram */
	histogram = spdk_histogram_data_alloc();

	/* Check if histogram is zeroed */
	spdk_bdev_histogram_get(&g_bdev.bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == 0);
	SPDK_CU_ASSERT_FATAL(g_histogram != NULL);

	g_count = 0;
	spdk_histogram_data_iterate(g_histogram, histogram_io_count, NULL);

	CU_ASSERT(g_count == 0);

	set_thread(0);
	rc = spdk_bdev_write_blocks(g_desc, ch[0], &buf, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);

	spdk_delay_us(10);
	stub_complete_io(g_bdev.io_target, 1);
	poll_threads();
	CU_ASSERT(status == true);


	set_thread(1);
	rc = spdk_bdev_read_blocks(g_desc, ch[1], &buf, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);

	spdk_delay_us(10);
	stub_complete_io(g_bdev.io_target, 1);
	poll_threads();
	CU_ASSERT(status == true);

	set_thread(0);

	/* Check if histogram gathered data from all I/O channels */
	spdk_bdev_histogram_get(&g_bdev.bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(g_bdev.bdev.internal.histogram_enabled == true);
	SPDK_CU_ASSERT_FATAL(g_histogram != NULL);

	g_count = 0;
	spdk_histogram_data_iterate(g_histogram, histogram_io_count, NULL);
	CU_ASSERT(g_count == 2);

	/* Disable histogram */
	spdk_bdev_histogram_enable(&g_bdev.bdev, histogram_status_cb, NULL, false);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(g_bdev.bdev.internal.histogram_enabled == false);

	spdk_histogram_data_free(histogram);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(ch[0]);
	set_thread(1);
	spdk_put_io_channel(ch[1]);
	poll_threads();
	set_thread(0);
	teardown_test();

}

struct timeout_io_cb_arg {
	struct iovec iov;
	uint8_t type;
};

static int
bdev_channel_count_submitted_io(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io;
	int n = 0;

	if (!ch) {
		return -1;
	}

	TAILQ_FOREACH(bdev_io, &ch->io_submitted, internal.ch_link) {
		n++;
	}

	return n;
}

static void
bdev_channel_io_timeout_cb(void *cb_arg, struct spdk_bdev_io *bdev_io)
{
	struct timeout_io_cb_arg *ctx = cb_arg;

	ctx->type = bdev_io->type;
	ctx->iov.iov_base = bdev_io->iov.iov_base;
	ctx->iov.iov_len = bdev_io->iov.iov_len;
}

static bool g_io_done;

static void
io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_io_done = true;
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_set_io_timeout_mt(void)
{
	struct spdk_io_channel *ch[3];
	struct spdk_bdev_channel *bdev_ch[3];
	struct timeout_io_cb_arg cb_arg;

	setup_test();

	g_bdev.bdev.optimal_io_boundary = 16;
	g_bdev.bdev.split_on_optimal_io_boundary = true;

	set_thread(0);
	ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[0] != NULL);

	set_thread(1);
	ch[1] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[1] != NULL);

	set_thread(2);
	ch[2] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[2] != NULL);

	/* Multi-thread mode
	 * 1, Check the poller was registered successfully
	 * 2, Check the timeout IO and ensure the IO was the submitted by user
	 * 3, Check the link int the bdev_ch works right.
	 * 4, Close desc and put io channel during the timeout poller is polling
	 */

	/* In desc thread set the timeout */
	set_thread(0);
	CU_ASSERT(spdk_bdev_set_timeout(g_desc, 5, bdev_channel_io_timeout_cb, &cb_arg) == 0);
	CU_ASSERT(g_desc->io_timeout_poller != NULL);
	CU_ASSERT(g_desc->cb_fn == bdev_channel_io_timeout_cb);
	CU_ASSERT(g_desc->cb_arg == &cb_arg);

	/* check the IO submitted list and timeout handler */
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[0], (void *)0x2000, 0, 1, io_done, NULL) == 0);
	bdev_ch[0] = spdk_io_channel_get_ctx(ch[0]);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[0]) == 1);

	set_thread(1);
	CU_ASSERT(spdk_bdev_write_blocks(g_desc, ch[1], (void *)0x1000, 0, 1, io_done, NULL) == 0);
	bdev_ch[1] = spdk_io_channel_get_ctx(ch[1]);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[1]) == 1);

	/* Now test that a single-vector command is split correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 *
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	set_thread(2);
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[2], (void *)0xF000, 14, 8, io_done, NULL) == 0);
	bdev_ch[2] = spdk_io_channel_get_ctx(ch[2]);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[2]) == 3);

	set_thread(0);
	memset(&cb_arg, 0, sizeof(cb_arg));
	spdk_delay_us(3 * spdk_get_ticks_hz());
	poll_threads();
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	/* Now the time reach the limit */
	spdk_delay_us(3 * spdk_get_ticks_hz());
	poll_thread(0);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x2000);
	CU_ASSERT(cb_arg.iov.iov_len == 1 * g_bdev.bdev.blocklen);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[0]) == 0);

	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(1);
	poll_thread(1);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x1000);
	CU_ASSERT(cb_arg.iov.iov_len == 1 * g_bdev.bdev.blocklen);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[1]) == 0);

	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(2);
	poll_thread(2);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0xF000);
	CU_ASSERT(cb_arg.iov.iov_len == 8 * g_bdev.bdev.blocklen);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[2]) == 2);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[2]) == 0);

	/* Run poll_timeout_done() it means complete the timeout poller */
	set_thread(0);
	poll_thread(0);
	CU_ASSERT(g_desc->refs == 0);
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[0], (void *)0x1000, 0, 1, io_done, NULL) == 0);
	set_thread(1);
	CU_ASSERT(spdk_bdev_write_blocks(g_desc, ch[1], (void *)0x2000, 0, 2, io_done, NULL) == 0);
	set_thread(2);
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[2], (void *)0x3000, 0, 3, io_done, NULL) == 0);

	/* Trigger timeout poller to run again, desc->refs is incremented.
	 * In thread 0 we destroy the io channel before timeout poller runs.
	 * Timeout callback is not called on thread 0.
	 */
	spdk_delay_us(6 * spdk_get_ticks_hz());
	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 1);
	spdk_put_io_channel(ch[0]);
	poll_thread(0);
	CU_ASSERT(g_desc->refs == 1)
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	/* In thread 1 timeout poller runs then we destroy the io channel
	 * Timeout callback is called on thread 1.
	 */
	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(1);
	poll_thread(1);
	stub_complete_io(g_bdev.io_target, 1);
	spdk_put_io_channel(ch[1]);
	poll_thread(1);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x2000);
	CU_ASSERT(cb_arg.iov.iov_len == 2 * g_bdev.bdev.blocklen);

	/* Close the desc.
	 * Unregister the timeout poller first.
	 * Then decrement desc->refs but it's not zero yet so desc is not freed.
	 */
	set_thread(0);
	spdk_bdev_close(g_desc);
	CU_ASSERT(g_desc->refs == 1);
	CU_ASSERT(g_desc->io_timeout_poller == NULL);

	/* Timeout poller runs on thread 2 then we destroy the io channel.
	 * Desc is closed so we would exit the timeout poller directly.
	 * timeout callback is not called on thread 2.
	 */
	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(2);
	poll_thread(2);
	stub_complete_io(g_bdev.io_target, 1);
	spdk_put_io_channel(ch[2]);
	poll_thread(2);
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	set_thread(0);
	poll_thread(0);
	g_teardown_done = false;
	unregister_bdev(&g_bdev);
	spdk_io_device_unregister(&g_io_device, NULL);
	spdk_bdev_finish(finish_cb, NULL);
	spdk_iobuf_finish(finish_cb, NULL);
	poll_threads();
	memset(&g_bdev, 0, sizeof(g_bdev));
	CU_ASSERT(g_teardown_done == true);
	g_teardown_done = false;
	free_threads();
	free_cores();
}

static bool g_io_done2;
static bool g_lock_lba_range_done;
static bool g_unlock_lba_range_done;

static void
io_done2(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_io_done2 = true;
	spdk_bdev_free_io(bdev_io);
}

static void
lock_lba_range_done(struct lba_range *range, void *ctx, int status)
{
	g_lock_lba_range_done = true;
}

static void
unlock_lba_range_done(struct lba_range *range, void *ctx, int status)
{
	g_unlock_lba_range_done = true;
}

static uint32_t
stub_channel_outstanding_cnt(void *io_target)
{
	struct spdk_io_channel *_ch = spdk_get_io_channel(io_target);
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);
	uint32_t outstanding_cnt;

	outstanding_cnt = ch->outstanding_cnt;

	spdk_put_io_channel(_ch);
	return outstanding_cnt;
}

static void
lock_lba_range_then_submit_io(void)
{
	struct spdk_bdev_desc *desc = NULL;
	void *io_target;
	struct spdk_io_channel *io_ch[3];
	struct spdk_bdev_channel *bdev_ch[3];
	struct lba_range *range;
	char buf[4096];
	int ctx0, ctx1, ctx2;
	int rc;

	setup_test();

	io_target = g_bdev.io_target;
	desc = g_desc;

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(io_ch[0] != NULL);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(io_ch[1] != NULL);

	set_thread(0);
	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch[0], 20, 10, lock_lba_range_done, &ctx0);
	CU_ASSERT(rc == 0);
	poll_threads();

	/* The lock should immediately become valid, since there are no outstanding
	 * write I/O.
	 */
	CU_ASSERT(g_lock_lba_range_done == true);
	range = TAILQ_FIRST(&bdev_ch[0]->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 20);
	CU_ASSERT(range->length == 10);
	CU_ASSERT(range->owner_ch == bdev_ch[0]);

	g_io_done = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));
	rc = spdk_bdev_read_blocks(desc, io_ch[0], buf, 20, 1, io_done, &ctx0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);

	stub_complete_io(io_target, 1);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));

	/* Try a write I/O.  This should actually be allowed to execute, since the channel
	 * holding the lock is submitting the write I/O.
	 */
	g_io_done = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));
	rc = spdk_bdev_write_blocks(desc, io_ch[0], buf, 20, 1, io_done, &ctx0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);

	stub_complete_io(io_target, 1);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));

	/* Try a write I/O.  This should get queued in the io_locked tailq. */
	set_thread(1);
	g_io_done = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[1]->io_locked));
	rc = spdk_bdev_write_blocks(desc, io_ch[1], buf, 20, 1, io_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 0);
	CU_ASSERT(!TAILQ_EMPTY(&bdev_ch[1]->io_locked));
	CU_ASSERT(g_io_done == false);

	/* Try to unlock the lba range using thread 1's io_ch.  This should fail. */
	rc = bdev_unlock_lba_range(desc, io_ch[1], 20, 10, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == -EINVAL);

	/* Now create a new channel and submit a write I/O with it.  This should also be queued.
	 * The new channel should inherit the active locks from the bdev's internal list.
	 */
	set_thread(2);
	io_ch[2] = spdk_bdev_get_io_channel(desc);
	bdev_ch[2] = spdk_io_channel_get_ctx(io_ch[2]);
	CU_ASSERT(io_ch[2] != NULL);

	g_io_done2 = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[2]->io_locked));
	rc = spdk_bdev_write_blocks(desc, io_ch[2], buf, 22, 2, io_done2, &ctx2);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 0);
	CU_ASSERT(!TAILQ_EMPTY(&bdev_ch[2]->io_locked));
	CU_ASSERT(g_io_done2 == false);

	set_thread(0);
	rc = bdev_unlock_lba_range(desc, io_ch[0], 20, 10, unlock_lba_range_done, &ctx0);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->locked_ranges));

	/* The LBA range is unlocked, so the write IOs should now have started execution. */
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[1]->io_locked));
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[2]->io_locked));

	set_thread(1);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);
	stub_complete_io(io_target, 1);
	set_thread(2);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);
	stub_complete_io(io_target, 1);

	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_done2 == true);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	set_thread(2);
	spdk_put_io_channel(io_ch[2]);
	poll_threads();
	set_thread(0);
	teardown_test();
}

/* spdk_bdev_reset() freezes and unfreezes I/O channels by using spdk_for_each_channel().
 * spdk_bdev_unregister() calls spdk_io_device_unregister() in the end. However
 * spdk_io_device_unregister() fails if it is called while executing spdk_for_each_channel().
 * Hence, in this case, spdk_io_device_unregister() is deferred until spdk_bdev_reset()
 * completes. Test this behavior.
 */
static void
unregister_during_reset(void)
{
	struct spdk_io_channel *io_ch[2];
	bool done_reset = false, done_unregister = false;
	int rc;

	setup_test();
	set_thread(0);

	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	SPDK_CU_ASSERT_FATAL(io_ch[0] != NULL);

	set_thread(1);

	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	SPDK_CU_ASSERT_FATAL(io_ch[1] != NULL);

	set_thread(0);

	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);

	rc = spdk_bdev_reset(g_desc, io_ch[0], reset_done, &done_reset);
	CU_ASSERT(rc == 0);

	set_thread(0);

	poll_thread_times(0, 1);

	spdk_bdev_close(g_desc);
	spdk_bdev_unregister(&g_bdev.bdev, _bdev_unregistered, &done_unregister);

	CU_ASSERT(done_reset == false);
	CU_ASSERT(done_unregister == false);

	poll_threads();

	stub_complete_io(g_bdev.io_target, 0);

	poll_threads();

	CU_ASSERT(done_reset == true);
	CU_ASSERT(done_unregister == false);

	spdk_put_io_channel(io_ch[0]);

	set_thread(1);

	spdk_put_io_channel(io_ch[1]);

	poll_threads();

	CU_ASSERT(done_unregister == true);

	/* Restore the original g_bdev so that we can use teardown_test(). */
	set_thread(0);
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
	teardown_test();
}

static void
bdev_init_wt_cb(void *done, int rc)
{
}

static int
wrong_thread_setup(void)
{
	allocate_cores(1);
	allocate_threads(2);
	set_thread(0);

	spdk_io_device_register(&g_accel_io_device, ut_accel_ch_create_cb,
				ut_accel_ch_destroy_cb, 0, NULL);
	spdk_bdev_initialize(bdev_init_wt_cb, NULL);
	spdk_io_device_register(&g_io_device, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);

	set_thread(1);

	return 0;
}

static int
wrong_thread_teardown(void)
{
	int rc = 0;

	set_thread(0);

	g_teardown_done = false;
	spdk_io_device_unregister(&g_io_device, NULL);
	spdk_bdev_finish(finish_cb, NULL);
	poll_threads();
	memset(&g_bdev, 0, sizeof(g_bdev));
	if (!g_teardown_done) {
		fprintf(stderr, "%s:%d %s: teardown not done\n", __FILE__, __LINE__, __func__);
		rc = -1;
	}
	g_teardown_done = false;

	spdk_io_device_unregister(&g_accel_io_device, NULL);
	free_threads();
	free_cores();

	return rc;
}

static void
_bdev_unregistered_wt(void *ctx, int rc)
{
	struct spdk_thread **threadp = ctx;

	*threadp = spdk_get_thread();
}

static void
spdk_bdev_register_wt(void)
{
	struct spdk_bdev bdev = { 0 };
	int rc;
	struct spdk_thread *unreg_thread;

	bdev.name = "wt_bdev";
	bdev.fn_table = &fn_table;
	bdev.module = &bdev_ut_if;
	bdev.blocklen = 4096;
	bdev.blockcnt = 1024;

	/* Can register only on app thread */
	rc = spdk_bdev_register(&bdev);
	CU_ASSERT(rc == -EINVAL);

	/* Can unregister on any thread */
	set_thread(0);
	rc = spdk_bdev_register(&bdev);
	CU_ASSERT(rc == 0);
	set_thread(1);
	unreg_thread = NULL;
	spdk_bdev_unregister(&bdev, _bdev_unregistered_wt, &unreg_thread);
	poll_threads();
	CU_ASSERT(unreg_thread == spdk_get_thread());

	/* Can unregister by name on any thread */
	set_thread(0);
	rc = spdk_bdev_register(&bdev);
	CU_ASSERT(rc == 0);
	set_thread(1);
	unreg_thread = NULL;
	rc = spdk_bdev_unregister_by_name(bdev.name, bdev.module, _bdev_unregistered_wt,
					  &unreg_thread);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(unreg_thread == spdk_get_thread());
}

static void
wait_for_examine_cb(void *arg)
{
	struct spdk_thread **thread = arg;

	*thread = spdk_get_thread();
}

static void
spdk_bdev_examine_wt(void)
{
	int rc;
	bool save_auto_examine = g_bdev_opts.bdev_auto_examine;
	struct spdk_thread *thread;

	g_bdev_opts.bdev_auto_examine = false;

	set_thread(0);
	register_bdev(&g_bdev, "ut_bdev_wt", &g_io_device);
	CU_ASSERT(spdk_bdev_get_by_name("ut_bdev_wt") != NULL);
	set_thread(1);

	/* Can examine only on the app thread */
	rc = spdk_bdev_examine("ut_bdev_wt");
	CU_ASSERT(rc == -EINVAL);
	unregister_bdev(&g_bdev);
	CU_ASSERT(spdk_bdev_get_by_name("ut_bdev_wt") == NULL);

	/* Can wait for examine on app thread, callback called on app thread. */
	set_thread(0);
	register_bdev(&g_bdev, "ut_bdev_wt", &g_io_device);
	CU_ASSERT(spdk_bdev_get_by_name("ut_bdev_wt") != NULL);
	thread = NULL;
	rc = spdk_bdev_wait_for_examine(wait_for_examine_cb, &thread);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(thread == spdk_get_thread());
	unregister_bdev(&g_bdev);
	CU_ASSERT(spdk_bdev_get_by_name("ut_bdev_wt") == NULL);

	/* Can wait for examine on non-app thread, callback called on same thread. */
	set_thread(0);
	register_bdev(&g_bdev, "ut_bdev_wt", &g_io_device);
	CU_ASSERT(spdk_bdev_get_by_name("ut_bdev_wt") != NULL);
	thread = NULL;
	rc = spdk_bdev_wait_for_examine(wait_for_examine_cb, &thread);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(thread == spdk_get_thread());
	unregister_bdev(&g_bdev);
	CU_ASSERT(spdk_bdev_get_by_name("ut_bdev_wt") == NULL);

	unregister_bdev(&g_bdev);
	g_bdev_opts.bdev_auto_examine = save_auto_examine;
}

static void
event_notify_and_close(void)
{
	int resize_notify_count = 0;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_bdev *bdev;
	int rc;

	setup_test();
	set_thread(0);

	/* setup_test() automatically opens the bdev, but this test needs to do
	 * that in a different way. */
	spdk_bdev_close(g_desc);
	poll_threads();

	set_thread(1);

	rc = spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, &resize_notify_count, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	bdev = spdk_bdev_desc_get_bdev(desc);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	/* Test a normal case that a resize event is notified. */
	set_thread(0);

	rc = spdk_bdev_notify_blockcnt_change(bdev, 1024 * 2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->blockcnt == 1024 * 2);
	CU_ASSERT(desc->refs == 1);
	CU_ASSERT(resize_notify_count == 0);

	poll_threads();

	CU_ASSERT(desc->refs == 0);
	CU_ASSERT(resize_notify_count == 1);

	/* Test a complex case if the bdev is closed after two event_notify messages are sent,
	 * then both event_notify messages are discarded and the desc is freed.
	 */
	rc = spdk_bdev_notify_blockcnt_change(bdev, 1024 * 3);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->blockcnt == 1024 * 3);
	CU_ASSERT(desc->refs == 1);
	CU_ASSERT(resize_notify_count == 1);

	rc = spdk_bdev_notify_blockcnt_change(bdev, 1024 * 4);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->blockcnt == 1024 * 4);
	CU_ASSERT(desc->refs == 2);
	CU_ASSERT(resize_notify_count == 1);

	set_thread(1);

	spdk_bdev_close(desc);
	CU_ASSERT(desc->closed == true);
	CU_ASSERT(desc->refs == 2);
	CU_ASSERT(resize_notify_count == 1);

	poll_threads();

	CU_ASSERT(resize_notify_count == 1);

	set_thread(0);

	/* Restore g_desc. Then, we can execute teardown_test(). */
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
	teardown_test();
}

/**
 * There was a race between reset start and complete:
 *
 * 1. reset_1 is completing. It clears bdev->internal.reset_in_progress and sends
 *    unfreeze_channel messages to remove queued resets of all channels.
 * 2. reset_2 is starting. As bdev->internal.reset_in_progress has been cleared, it
 *    is inserted to queued_resets list and starts to freeze channels.
 * 3. reset_1's unfreeze_channel message removes reset_2 from queued_resets list.
 * 4. reset_2 finishes freezing channels, but the corresponding bdev_io has gone,
 *    hence resulting in segmentation fault.
 *
 * To fix this,
 * 1. Do not queue the reset that is submitted to the underlying device.
 * 2. Queue all other resets in a per-bdev list, so all of them can be completed
 *    at once.
 */
static void
reset_start_complete_race(void)
{
	struct spdk_io_channel *io_ch;
	bool done_reset_1 = false, done_reset_2 = false;
	uint32_t num_completed;
	int rc;

	setup_test();
	set_thread(0);

	io_ch = spdk_bdev_get_io_channel(g_desc);
	SPDK_CU_ASSERT_FATAL(io_ch != NULL);

	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);

	/**
	 * Submit reset_1.
	 */
	rc = spdk_bdev_reset(g_desc, io_ch, reset_done, &done_reset_1);
	CU_ASSERT(rc == 0);

	/**
	 * Poll threads so that reset_1 completes freezing channels and gets submitted to
	 *  the undelying device.
	 */
	poll_threads();

	/**
	 * Complete reset_1. This will start the unfreezing channel stage of reset_1, but
	 *  not complete it until next poll_threads.
	 */
	num_completed = stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(num_completed == 1);

	/**
	 * Submit reset_2. It should be queued because reset_1 has not been completed yet.
	 */
	rc = spdk_bdev_reset(g_desc, io_ch, reset_done, &done_reset_2);
	CU_ASSERT(rc == 0);

	/**
	 * Poll threads. reset_1 completes unfreezing channels, then completes queued reset_2,
	 *  and finally itself gets completed.
	 */
	poll_threads();
	CU_ASSERT(done_reset_1 == true);
	CU_ASSERT(done_reset_2 == true);

	spdk_put_io_channel(io_ch);
	teardown_test();
}

static void
add_remove_bdev_done(void *cb_arg, int rc)
{
	int *status = cb_arg;

	*status = rc;
}

static void
basic_qos(void)
{
	struct spdk_bdev_qos *qos = NULL, *qos2 = NULL;
	struct spdk_bdev_qos_desc *desc = NULL, *desc2 = NULL, *desc3 = NULL;
	struct spdk_bdev *bdev1, *bdev2;
	struct spdk_io_channel *bdev1_io_ch, *bdev2_io_ch;
	struct spdk_bdev_channel *bdev1_ch, *bdev2_ch;
	struct spdk_bdev_qos_impl *qos_impl;
	struct spdk_bdev_qos_channel_impl *qos_ch_impl;
	struct ut_qos_channel *ut_qos_ch;
	const char *enabled_module[1] = {"ut_qos"};
	enum spdk_bdev_io_status io_status, io_status2;
	int rc, status, status2;

	setup_test();

	setup_bdev2(g_bdev.io_target);

	/* Create qos device and open it separately. */
	rc = spdk_bdev_qos_create("ut_qos_service", NULL, enabled_module, 1, &qos, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(qos != NULL);

	qos_impl = TAILQ_FIRST(&qos->impl_list);
	SPDK_CU_ASSERT_FATAL(qos_impl != NULL);
	CU_ASSERT(TAILQ_NEXT(qos_impl, link) == NULL);
	CU_ASSERT(qos_impl->module == &ut_qos_if);

	rc = spdk_bdev_qos_open("ut_qos_service", &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	bdev1_io_ch = spdk_bdev_get_io_channel(g_desc);
	SPDK_CU_ASSERT_FATAL(bdev1_io_ch != NULL);
	bdev1_ch = spdk_io_channel_get_ctx(bdev1_io_ch);

	CU_ASSERT(bdev1_ch->qos_ch == NULL);
	CU_ASSERT(!(bdev1_ch->flags &= BDEV_CH_QOS_ENABLED));

	bdev1 = spdk_bdev_desc_get_bdev(g_desc);
	bdev2 = spdk_bdev_desc_get_bdev(g_desc2);

	status = -1;
	spdk_bdev_qos_add_bdev(qos, bdev1, add_remove_bdev_done, &status);

	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT(bdev1_ch->qos_ch != NULL);
	CU_ASSERT(bdev1_ch->flags & BDEV_CH_QOS_ENABLED);

	qos_ch_impl = TAILQ_FIRST(&bdev1_ch->qos_ch->impl_list);
	SPDK_CU_ASSERT_FATAL(qos_ch_impl != NULL);
	CU_ASSERT(TAILQ_NEXT(qos_ch_impl, link) == NULL);
	ut_qos_ch = SPDK_CONTAINEROF(qos_ch_impl, struct ut_qos_channel, base);

	/* 2nd qos_add_bdev call to the same bdev should fail. */
	status = -1;
	spdk_bdev_qos_add_bdev(qos, bdev2, add_remove_bdev_done, &status);

	status2 = 0;
	spdk_bdev_qos_add_bdev(qos, bdev2, add_remove_bdev_done, &status2);

	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT(status2 == -EEXIST);

	/* qos_add_bdev to a bdev which belongs to another QoS device should fail too. */
	rc = spdk_bdev_qos_create("ut_qos_service2", NULL, enabled_module, 1, &qos2, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(qos2 != NULL);

	status = -1;
	spdk_bdev_qos_add_bdev(qos2, bdev2, add_remove_bdev_done, &status);

	poll_threads();
	CU_ASSERT(status == -EEXIST);
	CU_ASSERT(bdev2->internal.qos == qos);
	CU_ASSERT(TAILQ_EMPTY(&qos2->bdevs));

	rc = spdk_bdev_qos_destroy(qos2);
	CU_ASSERT(rc == 0);
	qos2 = NULL;

	bdev2_io_ch = spdk_bdev_get_io_channel(g_desc2);
	SPDK_CU_ASSERT_FATAL(bdev2_io_ch != NULL);
	bdev2_ch = spdk_io_channel_get_ctx(bdev2_io_ch);

	CU_ASSERT(bdev2_ch->qos_ch != NULL);
	CU_ASSERT(bdev2_ch->flags & BDEV_CH_QOS_ENABLED);

	/* Check if I/O completes when QoS channel is not throttled. */
	io_status = SPDK_BDEV_IO_STATUS_PENDING;

	rc = spdk_bdev_read_blocks(g_desc, bdev1_io_ch, NULL, 0, 1, io_during_io_done, &io_status);
	CU_ASSERT(rc == 0);

	CU_ASSERT(TAILQ_EMPTY(&ut_qos_ch->queued_io));

	stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	poll_threads();

	/* Check if I/O is queued when QoS channel is throttled. */
	ut_qos_ch->throttled = true;

	io_status = SPDK_BDEV_IO_STATUS_PENDING;

	rc = spdk_bdev_read_blocks(g_desc, bdev1_io_ch, NULL, 0, 1, io_during_io_done, &io_status);
	CU_ASSERT(rc == 0);

	CU_ASSERT(!TAILQ_EMPTY(&ut_qos_ch->queued_io));

	io_status2 = SPDK_BDEV_IO_STATUS_PENDING;

	/* Abort the queued I/O. */
	rc = spdk_bdev_abort(g_desc, bdev1_io_ch, &io_status, io_during_io_done, &io_status2);
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(io_status == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(io_status2 == SPDK_BDEV_IO_STATUS_SUCCESS);

	status = -1;
	spdk_bdev_qos_remove_bdev(qos, bdev1, add_remove_bdev_done, &status);

	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT(bdev1_ch->qos_ch == NULL);
	CU_ASSERT(!(bdev1_ch->flags & BDEV_CH_QOS_ENABLED));

	spdk_put_io_channel(bdev1_io_ch);

	spdk_put_io_channel(bdev2_io_ch);

	status = -1;
	spdk_bdev_qos_remove_bdev(qos, bdev2, add_remove_bdev_done, &status);

	poll_threads();
	CU_ASSERT(status == 0);

	spdk_bdev_qos_close(desc);

	rc = spdk_bdev_qos_destroy(qos);
	CU_ASSERT(rc == 0);

	qos = NULL;
	desc = NULL;

	/* Create and open qos device in a single operation. */
	rc = spdk_bdev_qos_create("ut_qos_service", NULL, enabled_module, 1, &qos, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(qos != NULL);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(spdk_bdev_qos_desc_get_qos(desc) == qos);

	/* Another open should succeed. */
	rc = spdk_bdev_qos_open("ut_qos_service", &desc2);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc2 != NULL);
	CU_ASSERT(spdk_bdev_qos_desc_get_qos(desc2) == qos);

	/* But after deletion call, open should fail even if there are descriptors. */
	spdk_bdev_qos_destroy(qos);

	rc = spdk_bdev_qos_open("ut_qos_service", &desc3);
	CU_ASSERT(rc == -ENODEV);

	spdk_bdev_qos_close(desc2);
	spdk_bdev_qos_close(desc);

	teardown_bdev2();
	poll_threads();

	teardown_test();
}

static void
ut_qos_channel_queue_io_done(struct ut_qos_channel *ut_qos_ch, struct spdk_bdev_io *bdev_io)
{
	TAILQ_REMOVE(&ut_qos_ch->queued_io, bdev_io, internal.link);
	spdk_bdev_qos_channel_impl_queue_io_done(bdev_io);
}

static void
multi_qos_modules(void)
{
	struct spdk_bdev_qos *qos = NULL;
	struct spdk_bdev_qos_desc *desc = NULL;
	struct spdk_bdev *bdev;
	struct spdk_io_channel *bdev_io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_qos_impl *qos_impl1, *qos_impl2;
	struct spdk_bdev_qos_channel_impl *qos_ch_impl1, *qos_ch_impl2;
	struct ut_qos_channel *ut_qos_ch1, *ut_qos_ch2;
	struct spdk_bdev_io *bdev_io;
	enum spdk_bdev_io_status io_status;
	int rc, status;

	setup_test();

	/* Create a QoS device with two QoS modules. */
	rc = spdk_bdev_qos_create("ut_qos_service", NULL, NULL, 0, &qos, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(qos != NULL);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(spdk_bdev_qos_desc_get_qos(desc) == qos);

	qos_impl1 = TAILQ_FIRST(&qos->impl_list);
	SPDK_CU_ASSERT_FATAL(qos_impl1 != NULL);
	qos_impl2 = TAILQ_NEXT(qos_impl1, link);
	SPDK_CU_ASSERT_FATAL(qos_impl2 != NULL);
	CU_ASSERT(TAILQ_NEXT(qos_impl2, link) == NULL);

	CU_ASSERT(qos_impl1->module == &ut_qos_if);
	CU_ASSERT(qos_impl2->module == &ut_qos2_if);

	bdev = spdk_bdev_desc_get_bdev(g_desc);

	status = -1;
	spdk_bdev_qos_add_bdev(qos, bdev, add_remove_bdev_done, &status);

	poll_threads();
	CU_ASSERT(status == 0);

	bdev_io_ch = spdk_bdev_get_io_channel(g_desc);
	SPDK_CU_ASSERT_FATAL(bdev_io_ch != NULL);
	bdev_ch = spdk_io_channel_get_ctx(bdev_io_ch);

	CU_ASSERT(bdev_ch->flags & BDEV_CH_QOS_ENABLED);

	qos_ch_impl1 = TAILQ_FIRST(&bdev_ch->qos_ch->impl_list);
	SPDK_CU_ASSERT_FATAL(qos_ch_impl1 != NULL);
	qos_ch_impl2 = TAILQ_NEXT(qos_ch_impl1, link);
	SPDK_CU_ASSERT_FATAL(qos_ch_impl2 != NULL);
	CU_ASSERT(TAILQ_NEXT(qos_ch_impl2, link) == NULL);

	ut_qos_ch1 = SPDK_CONTAINEROF(qos_ch_impl1, struct ut_qos_channel, base);
	ut_qos_ch2 = SPDK_CONTAINEROF(qos_ch_impl2, struct ut_qos_channel, base);

	/* Throttle both of ut_qos_ch1 and ut_qos_ch2. */
	ut_qos_ch1->throttled = true;
	ut_qos_ch2->throttled = true;

	io_status = SPDK_BDEV_IO_STATUS_PENDING;

	/* Submit a read I/O. */
	rc = spdk_bdev_read_blocks(g_desc, bdev_io_ch, NULL, 0, 1, io_during_io_done, &io_status);
	CU_ASSERT(rc == 0);

	/* bdev_io should be blocked at 1st module. */
	bdev_io = TAILQ_FIRST(&ut_qos_ch1->queued_io);
	CU_ASSERT(bdev_io != NULL);

	/* Pass 1st module for this bdev_io. */
	ut_qos_channel_queue_io_done(ut_qos_ch1, bdev_io);

	/* bdev_io should be blocked at 2nd module. */
	CU_ASSERT(bdev_io == TAILQ_FIRST(&ut_qos_ch2->queued_io));

	/* Pass 2nd module for this bdev_io. */
	ut_qos_channel_queue_io_done(ut_qos_ch2, bdev_io);

	CU_ASSERT(io_status == SPDK_BDEV_IO_STATUS_PENDING);

	/* Then, the bdev_io should complete successfully. */
	stub_complete_io(g_bdev.io_target, 1);

	CU_ASSERT(io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	spdk_bdev_qos_close(desc);

	spdk_bdev_qos_destroy(qos);

	spdk_put_io_channel(bdev_io_ch);

	poll_threads();

	teardown_test();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	CU_pSuite	suite_wt = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("bdev", NULL, NULL);
	suite_wt = CU_add_suite("bdev_wrong_thread", wrong_thread_setup, wrong_thread_teardown);

	CU_ADD_TEST(suite, basic);
	CU_ADD_TEST(suite, unregister_and_close);
	CU_ADD_TEST(suite, unregister_and_close_different_threads);
	CU_ADD_TEST(suite, put_channel_during_reset);
	CU_ADD_TEST(suite, aborted_reset);
	CU_ADD_TEST(suite, aborted_reset_no_outstanding_io);
	CU_ADD_TEST(suite, io_during_reset);
	CU_ADD_TEST(suite, reset_completions);
	CU_ADD_TEST(suite, enomem);
	CU_ADD_TEST(suite, enomem_multi_bdev);
	CU_ADD_TEST(suite, enomem_multi_bdev_unregister);
	CU_ADD_TEST(suite, enomem_multi_io_target);
	CU_ADD_TEST(suite, enomem_retry_during_abort);
	CU_ADD_TEST(suite, bdev_histograms_mt);
	CU_ADD_TEST(suite, bdev_set_io_timeout_mt);
	CU_ADD_TEST(suite, lock_lba_range_then_submit_io);
	CU_ADD_TEST(suite, unregister_during_reset);
	CU_ADD_TEST(suite_wt, spdk_bdev_register_wt);
	CU_ADD_TEST(suite_wt, spdk_bdev_examine_wt);
	CU_ADD_TEST(suite, event_notify_and_close);
	CU_ADD_TEST(suite, reset_start_complete_race);
	CU_ADD_TEST(suite, basic_qos);
	CU_ADD_TEST(suite, multi_qos_modules);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
