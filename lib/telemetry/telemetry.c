/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/thread.h"
#include "spdk/telemetry.h"
#include "spdk/telemetry_source.h"
#include "telemetry_internal.h"

enum telemetry_source_state {
	TELEMETRY_SOURCE_IDLE,
	TELEMETRY_SOURCE_PULLING,
	TELEMETRY_SOURCE_REPORTING,
	__TELEMETRY_SOURCE_LAST,
};

SPDK_STATIC_ASSERT(__TELEMETRY_SOURCE_LAST <= 8, "telemetry_source_state is too large");
SPDK_STATIC_ASSERT(sizeof(double) == 8, "double is not 8 bytes");

struct spdk_telemetry_source {
	uint64_t state : 3; /* must be big enough to hold __TELEMETRY_SOURCE_LAST - 1 */
	uint64_t to_delete : 1;
	uint64_t reserved : 60;
	struct spdk_telemetry_type *type;
	struct spdk_telemetry_source_handle *handle;
	spdk_telemetry_pull_cb pull_cb;
	void *pull_cb_arg;
	TAILQ_ENTRY(spdk_telemetry_source) link;
	char *name;
	uint64_t stats_buffer[]; /* Variable-length buffer for stats */
};

struct spdk_telemetry_type {
	uint64_t to_delete : 1;
	uint64_t enabled : 1;
	uint64_t reserved : 62;
	uint64_t stats_buffer_size;
	struct spdk_telemetry_type_handle *handle;
	TAILQ_ENTRY(spdk_telemetry_type) link;
	TAILQ_HEAD(, spdk_telemetry_source) sources;
	const struct spdk_telemetry_type_info *info;
};

struct telemetry_mgr {
	bool initialized;
	bool started;
	spdk_telemetry_done_cb stop_done_cb;
	void *stop_done_cb_arg;
	struct spdk_telemetry_opts opts;
	struct spdk_poller *poller;
	TAILQ_HEAD(, spdk_telemetry_type) types;
	TAILQ_HEAD(, spdk_telemetry_exporter_module) modules;
	struct spdk_telemetry_exporter *exporter;
	spdk_telemetry_done_cb fini_cb_fn;
	void *fini_cb_arg;
};

static struct telemetry_mgr g_telemetry_mgr = {
	.initialized = false,
	.opts = { .interval_ms = SPDK_TELEMETRY_DEFAULT_INTERVAL_MS },
	.poller = NULL,
	.types = TAILQ_HEAD_INITIALIZER(g_telemetry_mgr.types),
	.modules = TAILQ_HEAD_INITIALIZER(g_telemetry_mgr.modules),
	.exporter = NULL,
	.fini_cb_fn = NULL,
	.fini_cb_arg = NULL,
};

static inline bool
telemetry_mgr_is_initialized(void)
{
	return g_telemetry_mgr.initialized;
}

static inline bool
telemetry_mgr_is_started(void)
{
	return g_telemetry_mgr.started;
}

static void
telemetry_delete_source(struct spdk_telemetry_source *src)
{
	assert(telemetry_mgr_is_initialized());
	assert(src != NULL);
	assert(spdk_thread_get_app_thread() == spdk_get_thread());

	if (src->handle != NULL) {
		g_telemetry_mgr.exporter->fn_table->unregister_source(g_telemetry_mgr.exporter->ctxt, src->handle);
	}

	assert(src->state == TELEMETRY_SOURCE_IDLE);

	free(src->name);
	free(src);
}

static void
telemetry_delete_type(struct spdk_telemetry_type *type)
{
	assert(telemetry_mgr_is_initialized());
	assert(type != NULL);
	assert(spdk_thread_get_app_thread() == spdk_get_thread());

	if (type->handle != NULL) {
		g_telemetry_mgr.exporter->fn_table->unregister_type(g_telemetry_mgr.exporter->ctxt, type->handle);
	}

	free(type);
}

static bool
telemetry_try_delete_source(struct spdk_telemetry_source *src)
{
	if (src->state != TELEMETRY_SOURCE_IDLE) {
		/* If the source is pulling or reporting, we need to wait for it to finish */
		return false;
	}

	/* Remove the source from the type's sources list */
	TAILQ_REMOVE(&src->type->sources, src, link);
	/* Delete the source */
	telemetry_delete_source(src);

	return true;
}

static int
telemetry_poll_type(struct spdk_telemetry_type *type)
{
	int res = SPDK_POLLER_IDLE;
	struct spdk_telemetry_source *src;
	struct spdk_telemetry_source *nsrc;

	/* If the type is marked for deletion, delete all sources and the type */
	if (type->to_delete) {
		TAILQ_FOREACH_SAFE(src, &type->sources, link, nsrc) {
			assert(src->to_delete == true);
			telemetry_try_delete_source(src);
		}

		/* If the type's sources list is empty, delete the type */
		if (TAILQ_EMPTY(&type->sources)) {
			/* Remove the type from the types list */
			TAILQ_REMOVE(&g_telemetry_mgr.types, type, link);
			/* Delete the type */
			telemetry_delete_type(type);
		}

		return SPDK_POLLER_BUSY;
	}

	/* If the type is disabled, skip it */
	if (!type->enabled) {
		return SPDK_POLLER_IDLE;
	}

	/* Pull all sources */
	TAILQ_FOREACH_SAFE(src, &type->sources, link, nsrc) {
		/* If the source is marked for deletion, try to delete it */
		if (src->to_delete) {
			telemetry_try_delete_source(src);
			continue;
		}

		/* If the source is already pulling or reporting, skip it */
		if (src->state != TELEMETRY_SOURCE_IDLE) {
			continue;
		}

		/* Start pulling the source */
		src->state = TELEMETRY_SOURCE_PULLING;
		src->pull_cb(src->pull_cb_arg, src);
		res = SPDK_POLLER_BUSY;
	}

	return res;
}

static int
telemetry_poller(void *ctx)
{
	int res = SPDK_POLLER_IDLE;
	struct spdk_telemetry_type *type;
	struct spdk_telemetry_type *ntype;

	assert(g_telemetry_mgr.exporter != NULL);
	assert(g_telemetry_mgr.stop_done_cb == NULL);

	TAILQ_FOREACH_SAFE(type, &g_telemetry_mgr.types, link, ntype) {
		if (telemetry_poll_type(type) == SPDK_POLLER_BUSY) {
			res = SPDK_POLLER_BUSY;
		}
	}

	return res;
}

int
spdk_telemetry_init(void)
{
	struct spdk_telemetry_exporter_module *module;

	assert(!telemetry_mgr_is_initialized());

	TAILQ_FOREACH(module, &g_telemetry_mgr.modules, internal.tailq) {
		int res = module->init();
		if (res != 0) {
			SPDK_ERRLOG("Failed to initialize telemetry module %s: %d\n", module->name, res);
			return res;
		}
	}

	g_telemetry_mgr.initialized = true;

	SPDK_DEBUGLOG(telemetry, "Telemetry initialized\n");
	return 0;
}

static void
telemetry_destruct_done_cb(void)
{
	assert(telemetry_mgr_is_initialized());
	assert(g_telemetry_mgr.fini_cb_fn != NULL);

	g_telemetry_mgr.fini_cb_fn(g_telemetry_mgr.fini_cb_arg, 0);

	g_telemetry_mgr.initialized = false;

	SPDK_DEBUGLOG(telemetry, "Telemetry finalized\n");
}

static void
telemetry_do_fini_stop_done(void *arg, int rc)
{
	assert(telemetry_mgr_is_initialized());
	assert(spdk_thread_is_app_thread(NULL));

	if (rc != 0) {
		SPDK_ERRLOG("Failed to stop telemetry: %d, ignoring...\n", rc);
	}

	if (g_telemetry_mgr.exporter) {
		int res = g_telemetry_mgr.exporter->fn_table->destruct(g_telemetry_mgr.exporter->ctxt);
		if (res == 1) {
			/* asynchronous destruct - the caller will call spdk_telemetry_exporter_destruct_done() later */
			SPDK_DEBUGLOG(telemetry, "Telemetry exporter destruction initiated\n");
			return;
		}

		if (res < 0) {
			SPDK_ERRLOG("Failed to destroy telemetry exporter: %d. Ignoring...\n", res);
		}
	}

	spdk_telemetry_exporter_destruct_done();
}

void
spdk_telemetry_fini(spdk_telemetry_done_cb cb_fn, void *cb_arg)
{
	assert(telemetry_mgr_is_initialized());
	assert(cb_fn != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	g_telemetry_mgr.fini_cb_fn = cb_fn;
	g_telemetry_mgr.fini_cb_arg = cb_arg;

	if (g_telemetry_mgr.started) {
		/* If telemetry is started, stop it */
		spdk_telemetry_stop(telemetry_do_fini_stop_done, NULL);
	} else {
		/* Otherwise, just call the finalization callback */
		telemetry_do_fini_stop_done(NULL, 0);
	}
}

void
spdk_telemetry_start(const struct spdk_telemetry_opts *_opts, spdk_telemetry_done_cb cb,
		     void *cb_arg)
{
	struct spdk_telemetry_opts opts;
	int rc;

	assert(telemetry_mgr_is_initialized());
	assert(_opts != NULL);
	assert(cb != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	opts = *_opts;

	if (!g_telemetry_mgr.exporter) {
		SPDK_ERRLOG("No telemetry exporter registered\n");
		rc = -EINVAL;
		goto do_return;
	}

	if (g_telemetry_mgr.started) {
		SPDK_ERRLOG("Telemetry already started\n");
		rc = -EALREADY;
		goto do_return;
	}

	if (g_telemetry_mgr.stop_done_cb) {
		SPDK_ERRLOG("Telemetry is stopping, will retry later\n");
		rc = -EBUSY;
		goto do_return;
	}

	if (opts.interval_ms == 0) {
		SPDK_NOTICELOG("Interval not specified, using default interval of %d ms\n",
			       SPDK_TELEMETRY_DEFAULT_INTERVAL_MS);
		opts.interval_ms = SPDK_TELEMETRY_DEFAULT_INTERVAL_MS;
	}

	g_telemetry_mgr.poller = SPDK_POLLER_REGISTER(telemetry_poller, NULL,
				 opts.interval_ms * SPDK_MSEC_TO_USEC);
	if (g_telemetry_mgr.poller == NULL) {
		SPDK_ERRLOG("Failed to register telemetry poller\n");
		rc = -ENOMEM;
		goto do_return;
	}

	if (g_telemetry_mgr.exporter->fn_table->start) {
		rc = g_telemetry_mgr.exporter->fn_table->start(g_telemetry_mgr.exporter->ctxt);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to start telemetry exporter: %d\n", rc);
			spdk_poller_unregister(&g_telemetry_mgr.poller);
			g_telemetry_mgr.poller = NULL;
			goto do_return;
		}
	}

	g_telemetry_mgr.opts = opts;

	SPDK_DEBUGLOG(telemetry, "Telemetry started\n");
	g_telemetry_mgr.started = true;
	rc = 0;

do_return:
	cb(cb_arg, rc);
}

static void
telemetry_do_stop(void *arg)
{
	struct spdk_telemetry_type *type;
	struct spdk_telemetry_source *src;
	spdk_telemetry_done_cb stop_done_cb;
	void *stop_done_cb_arg;
	bool sources_pulling = false;
	bool sources_reporting = false;

	assert(telemetry_mgr_is_initialized());
	assert(spdk_thread_is_app_thread(NULL));
	assert(g_telemetry_mgr.stop_done_cb != NULL);

	/* Check if any sources are pulling */
	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		TAILQ_FOREACH(src, &type->sources, link) {
			if (src->state == TELEMETRY_SOURCE_PULLING) {
				sources_pulling = true;
				break;
			}
		}

		if (sources_pulling) {
			break;
		}
	}

	if (sources_pulling) {
		SPDK_DEBUGLOG(telemetry, "Some sources are still pulling, will retry later\n");
		spdk_thread_send_msg(spdk_thread_get_app_thread(), telemetry_do_stop, NULL);
		return;
	}

	/* We only need wait for the exporter to finish reporting, allowing any stats that are
	 * ready to be released while avoiding the new stats from being collected.
	 */
	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		TAILQ_FOREACH(src, &type->sources, link) {
			if (src->state == TELEMETRY_SOURCE_REPORTING) {
				sources_reporting = true;
				break;
			}
		}

		if (sources_reporting) {
			break;
		}
	}

	/* If any sources are reporting, we need to wait for them to finish */
	if (sources_reporting) {
		SPDK_DEBUGLOG(telemetry, "Some sources are still reporting, will retry later\n");
		spdk_thread_send_msg(spdk_thread_get_app_thread(), telemetry_do_stop, NULL);
		return;
	}

	if (g_telemetry_mgr.exporter->fn_table->stop) {
		g_telemetry_mgr.exporter->fn_table->stop(g_telemetry_mgr.exporter->ctxt);
	}

	g_telemetry_mgr.started = false;

	/* Store the stop done callback and argument and clear them, so the stop_done_cb could restart telemetry */
	stop_done_cb = g_telemetry_mgr.stop_done_cb;
	stop_done_cb_arg = g_telemetry_mgr.stop_done_cb_arg;
	g_telemetry_mgr.stop_done_cb = NULL;
	g_telemetry_mgr.stop_done_cb_arg = NULL;

	SPDK_DEBUGLOG(telemetry, "Telemetry stopped\n");

	stop_done_cb(stop_done_cb_arg, 0);
}

void
spdk_telemetry_stop(spdk_telemetry_done_cb cb, void *cb_arg)
{
	assert(telemetry_mgr_is_initialized());
	assert(cb != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	if (!g_telemetry_mgr.started) {
		SPDK_ERRLOG("Telemetry not started\n");
		cb(cb_arg, -EINVAL);
		return;
	}

	if (g_telemetry_mgr.stop_done_cb) {
		SPDK_ERRLOG("Telemetry is already stopping, will retry later\n");
		cb(cb_arg, -EBUSY);
		return;
	}

	assert(g_telemetry_mgr.poller != NULL);
	spdk_poller_unregister(&g_telemetry_mgr.poller);
	g_telemetry_mgr.poller = NULL;

	g_telemetry_mgr.stop_done_cb = cb;
	g_telemetry_mgr.stop_done_cb_arg = cb_arg;

	telemetry_do_stop(NULL);
}

void
spdk_telemetry_dump_info_json(struct spdk_json_write_ctx *w)
{
	assert(telemetry_mgr_is_initialized());
	assert(w != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	spdk_json_write_object_begin(w);

	spdk_json_write_named_bool(w, "started", g_telemetry_mgr.poller ? true : false);
	if (g_telemetry_mgr.poller) {
		spdk_json_write_named_uint64(w, "interval_ms", g_telemetry_mgr.opts.interval_ms);
	}
	if (g_telemetry_mgr.exporter) {
		spdk_json_write_named_object_begin(w, "exporter");
		spdk_json_write_named_string(w, "name", g_telemetry_mgr.exporter->module->name);
		spdk_json_write_named_object_begin(w, "module_specific");
		if (g_telemetry_mgr.exporter->fn_table && g_telemetry_mgr.exporter->fn_table->dump_info_json) {
			g_telemetry_mgr.exporter->fn_table->dump_info_json(g_telemetry_mgr.exporter->ctxt, w);
		}
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);
}

void
spdk_telemetry_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_telemetry_exporter_module *module;
	struct spdk_telemetry_type *type;

	assert(telemetry_mgr_is_initialized());
	assert(spdk_thread_is_app_thread(NULL));

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(module, &g_telemetry_mgr.modules, internal.tailq) {
		if (module->config_json) {
			module->config_json(w);
		}
	}

	if (g_telemetry_mgr.exporter && g_telemetry_mgr.exporter->fn_table->write_config_json) {
		g_telemetry_mgr.exporter->fn_table->write_config_json(g_telemetry_mgr.exporter->ctxt, w);
	}

	if (g_telemetry_mgr.poller) {
		spdk_json_write_object_begin(w); /* method */
		spdk_json_write_named_string(w, "method", "telemetry_start");
		spdk_json_write_named_object_begin(w, "params"); /* params */
		spdk_json_write_named_uint64(w, "interval_ms", g_telemetry_mgr.opts.interval_ms);
		spdk_json_write_object_end(w); /* params */
		spdk_json_write_object_end(w); /* method */
	}


	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		/* If the type is marked for deletion, skip it */
		if (type->to_delete) {
			continue;
		}

		/* If the type is disabled, skip it */
		if (!type->enabled) {
			continue;
		}

		spdk_json_write_object_begin(w); /* method */
		spdk_json_write_named_string(w, "method", "telemetry_enable_type");
		spdk_json_write_named_object_begin(w, "params"); /* params */
		spdk_json_write_named_string(w, "name", type->info->name);
		spdk_json_write_object_end(w); /* params */
		spdk_json_write_object_end(w); /* method */
	}

	spdk_json_write_array_end(w);
}

void
spdk_telemetry_module_list_add(struct spdk_telemetry_exporter_module *module)
{
	assert(module != NULL);
	TAILQ_INSERT_TAIL(&g_telemetry_mgr.modules, module, internal.tailq);
}

static int
telemetry_exporter_register_type(struct spdk_telemetry_type *type)
{
	assert(g_telemetry_mgr.exporter != NULL);
	assert(type != NULL);

	type->handle = g_telemetry_mgr.exporter->fn_table->register_type(g_telemetry_mgr.exporter->ctxt,
			type->info);
	if (type->handle == NULL) {
		SPDK_ERRLOG("Failed to register telemetry type %s\n", type->info->name);
		return -EFAULT;
	}

	return 0;
}

static int
telemetry_exporter_register_source(struct spdk_telemetry_type *type,
				   struct spdk_telemetry_source *source)
{
	assert(g_telemetry_mgr.exporter != NULL);
	assert(type != NULL);
	assert(source != NULL);

	source->handle = g_telemetry_mgr.exporter->fn_table->register_source(g_telemetry_mgr.exporter->ctxt,
			 type->handle, source->name);
	if (source->handle == NULL) {
		SPDK_ERRLOG("Failed to register telemetry source %s\n", source->name);
		return -EFAULT;
	}

	return 0;
}

int
spdk_telemetry_exporter_register(struct spdk_telemetry_exporter *telemetry_exporter)
{
	struct spdk_telemetry_type *type;
	struct spdk_telemetry_source *src;

	assert(telemetry_mgr_is_initialized());
	assert(telemetry_exporter != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	if (g_telemetry_mgr.exporter) {
		SPDK_ERRLOG("Telemetry exporter already registered\n");
		return -EEXIST;
	}

	g_telemetry_mgr.exporter = telemetry_exporter;

	/* Register all types and sources with the exporter */
	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		int res;
		assert(type->handle == NULL);
		res = telemetry_exporter_register_type(type);
		if (res != 0) {
			goto register_failed;
		}

		TAILQ_FOREACH(src, &type->sources, link) {
			assert(src->handle == NULL);
			res = telemetry_exporter_register_source(type, src);
			if (res != 0) {
				goto register_failed;
			}
		}
	}

	return 0;

register_failed:
	/* In case of failure, unregister all types and sources previously registered with the exporter */
	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		TAILQ_FOREACH(src, &type->sources, link) {
			if (src->handle != NULL) {
				g_telemetry_mgr.exporter->fn_table->unregister_source(g_telemetry_mgr.exporter->ctxt, src->handle);
				src->handle = NULL;
			}
		}
		if (type->handle != NULL) {
			g_telemetry_mgr.exporter->fn_table->unregister_type(g_telemetry_mgr.exporter->ctxt, type->handle);
			type->handle = NULL;
		}
	}

	g_telemetry_mgr.exporter = NULL;
	return -EFAULT;
}

int
spdk_telemetry_exporter_unregister(struct spdk_telemetry_exporter *telemetry_exporter)
{
	struct spdk_telemetry_type *type;

	assert(telemetry_mgr_is_initialized());
	assert(telemetry_exporter != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	if (!g_telemetry_mgr.exporter) {
		SPDK_ERRLOG("Telemetry exporter not registered\n");
		return -ENOENT;
	}

	if (g_telemetry_mgr.exporter != telemetry_exporter) {
		SPDK_ERRLOG("This telemetry exporter is not the one registered\n");
		return -EINVAL;
	}

	if (g_telemetry_mgr.poller != NULL || g_telemetry_mgr.stop_done_cb != NULL) {
		SPDK_ERRLOG("Telemetry is running, cannot unregister exporter\n");
		return -EBUSY;
	}

	/* Unregister all types and sources with the exporter */
	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		struct spdk_telemetry_source *src;

		assert(type->handle != NULL); /* All types must still be registered with the exporter */

		TAILQ_FOREACH(src, &type->sources, link) {
			assert(src->handle != NULL); /* All sources must still be registered with the exporter */
			g_telemetry_mgr.exporter->fn_table->unregister_source(g_telemetry_mgr.exporter->ctxt, src->handle);
			src->handle = NULL;
		}

		g_telemetry_mgr.exporter->fn_table->unregister_type(g_telemetry_mgr.exporter->ctxt, type->handle);
		type->handle = NULL;
	}

	g_telemetry_mgr.exporter = NULL;
	return 0;
}

void
spdk_telemetry_exporter_release_stats(struct spdk_telemetry_source_handle *handle,
				      const void *stats_buffer, uint64_t stats_buffer_size)
{
	struct spdk_telemetry_source *src;

	assert(telemetry_mgr_is_initialized());
	assert(handle != NULL);
	assert(stats_buffer != NULL);
	assert(stats_buffer_size > 0);
	assert(spdk_thread_is_app_thread(NULL));

	src = SPDK_CONTAINEROF(stats_buffer, struct spdk_telemetry_source, stats_buffer);
	assert(src->handle == handle);
	assert(src->state == TELEMETRY_SOURCE_REPORTING);
	assert(stats_buffer_size == src->type->stats_buffer_size);

	src->state = TELEMETRY_SOURCE_IDLE;
}

static struct spdk_telemetry_type *
telemetry_find_type(const char *name)
{
	struct spdk_telemetry_type *type;

	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		if (strcmp(type->info->name, name) == 0) {
			return type;
		}
	}

	return NULL;
}

static int
telemetry_type_set_stats_buffer_size(struct spdk_telemetry_type *type)
{
	uint64_t i;
	uint64_t size = 0;
	struct spdk_telemetry_type *nested_type;

	for (i = 0; i < type->info->num_stats; i++) {
		const struct spdk_telemetry_stat_info *stat_info = &type->info->stats[i];

		assert(stat_info->type < __SPDK_TELEMETRY_STAT_TYPE_LAST);

		if (stat_info->count == 0) {
			SPDK_ERRLOG("Telemetry stat %s has count == 0\n", stat_info->name);
			return -EINVAL;
		}

		switch (stat_info->type) {
		case SPDK_TELEMETRY_STAT_TYPE_UINT64:
		case SPDK_TELEMETRY_STAT_TYPE_INT64:
		case SPDK_TELEMETRY_STAT_TYPE_DOUBLE64:
			size += sizeof(uint64_t) * stat_info->count;
			break;
		case SPDK_TELEMETRY_STAT_TYPE_SUBTYPE:
			assert(stat_info->extra.type_name != NULL);
			nested_type = telemetry_find_type(stat_info->extra.type_name);
			if (nested_type == NULL) {
				SPDK_ERRLOG("Nested telemetry type %s not found\n", stat_info->extra.type_name);
				return -EINVAL;
			}
			size += nested_type->stats_buffer_size * stat_info->count;
			break;
		default:
			SPDK_WARNLOG("Unknown stat type: %d\n", stat_info->type);
			return -EINVAL;
		}
	}

	type->stats_buffer_size = size;
	return 0;
}

int
spdk_telemetry_register_type(const struct spdk_telemetry_type_info *type_info,
			     struct spdk_telemetry_type **_type)
{
	struct spdk_telemetry_type *type;
	int res;

	assert(telemetry_mgr_is_initialized());
	assert(type_info != NULL);
	assert(type_info->num_stats > 0);
	assert(_type != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	if (g_telemetry_mgr.started) {
		SPDK_ERRLOG("Telemetry is running, cannot register type\n");
		return -EBUSY;
	}

	type = calloc(1, sizeof(*type));
	if (type == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for telemetry source type\n");
		return -ENOMEM;
	}

	type->info = type_info;
	res = telemetry_type_set_stats_buffer_size(type);
	if (res != 0) {
		telemetry_delete_type(type);
		return res;
	}

	if (g_telemetry_mgr.exporter) {
		res = telemetry_exporter_register_type(type);
		if (res != 0) {
			telemetry_delete_type(type);
			return res;
		}
	}

	TAILQ_INIT(&type->sources);

	TAILQ_INSERT_TAIL(&g_telemetry_mgr.types, type, link);

	*_type = type;
	return 0;
}

void
spdk_telemetry_unregister_type(struct spdk_telemetry_type *type)
{
	struct spdk_telemetry_source *src;

	assert(telemetry_mgr_is_initialized());
	assert(type != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	type->to_delete = true;
	TAILQ_FOREACH(src, &type->sources, link) {
		src->to_delete = true;
	}
}

int
spdk_telemetry_register_source(struct spdk_telemetry_type *type, const char *name,
			       spdk_telemetry_pull_cb pull_cb, void *pull_cb_arg,
			       struct spdk_telemetry_source **_src)
{
	struct spdk_telemetry_source *src;

	assert(telemetry_mgr_is_initialized());
	assert(type != NULL);
	assert(name != NULL);
	assert(pull_cb != NULL);
	assert(pull_cb_arg != NULL);
	assert(_src != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	src = calloc(1, sizeof(*src) + type->stats_buffer_size);
	if (src == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for telemetry source\n");
		return -ENOMEM;
	}

	src->name = strdup(name);
	if (src->name == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for telemetry source name\n");
		free(src);
		return -ENOMEM;
	}

	if (g_telemetry_mgr.exporter) {
		int res = telemetry_exporter_register_source(type, src);
		if (res != 0) {
			free(src->name);
			free(src);
			return res;
		}
	}

	src->type = type;
	src->pull_cb = pull_cb;
	src->pull_cb_arg = pull_cb_arg;

	TAILQ_INSERT_TAIL(&type->sources, src, link);

	*_src = src;
	return 0;
}

void
spdk_telemetry_unregister_source(struct spdk_telemetry_source *src)
{
	assert(telemetry_mgr_is_initialized());
	assert(src != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	src->to_delete = true;
}

void *
spdk_telemetry_source_get_stats_buffer(struct spdk_telemetry_source *src)
{
	assert(telemetry_mgr_is_initialized());
	assert(src != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	assert(src->state == TELEMETRY_SOURCE_PULLING);
	return src->stats_buffer;
}

uint64_t
spdk_telemetry_source_get_stats_buffer_size(struct spdk_telemetry_source *src)
{
	assert(telemetry_mgr_is_initialized());
	assert(src != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	assert(src->state == TELEMETRY_SOURCE_PULLING);
	return src->type->stats_buffer_size;
}

void
spdk_telemetry_source_pull_complete(struct spdk_telemetry_source *src, int status)
{
	bool res;

	assert(telemetry_mgr_is_initialized());
	assert(src != NULL);
	assert(spdk_thread_is_app_thread(NULL));
	assert(src->state == TELEMETRY_SOURCE_PULLING);

	src->state = TELEMETRY_SOURCE_REPORTING;

	if (status != 0) {
		SPDK_ERRLOG("Failed to pull telemetry stats for source %s: %d\n", src->name, status);
		src->state = TELEMETRY_SOURCE_IDLE;
		return;
	}

	res = g_telemetry_mgr.exporter->fn_table->report_stats(g_telemetry_mgr.exporter->ctxt, src->handle,
			src->stats_buffer, src->type->stats_buffer_size);
	if (res) {
		src->state = TELEMETRY_SOURCE_IDLE;
		return;
	}
}

const char *
spdk_telemetry_stat_type_name(enum spdk_telemetry_stat_type type)
{
	switch (type) {
	case SPDK_TELEMETRY_STAT_TYPE_UINT64:
		return "uint64_t";
	case SPDK_TELEMETRY_STAT_TYPE_INT64:
		return "int64_t";
	case SPDK_TELEMETRY_STAT_TYPE_DOUBLE64:
		return "double";
	case SPDK_TELEMETRY_STAT_TYPE_SUBTYPE:
		return "subtype";
	default:
		return "unknown";
	}
}

void
spdk_telemetry_exporter_destruct_done(void)
{
	struct spdk_telemetry_exporter_module *module;

	assert(telemetry_mgr_is_initialized());
	assert(spdk_thread_is_app_thread(NULL));

	SPDK_DEBUGLOG(telemetry, "Telemetry exporter destruction completed\n");

	TAILQ_FOREACH(module, &g_telemetry_mgr.modules, internal.tailq) {
		if (module->fini) {
			module->fini();
		}
	}

	assert(g_telemetry_mgr.exporter == NULL);

	telemetry_destruct_done_cb();
}

static void
telemetry_dump_type_json(const struct spdk_telemetry_type *type, struct spdk_json_write_ctx *w)
{
	uint64_t i;

	assert(type != NULL);
	assert(w != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", type->info->name);
	spdk_json_write_named_bool(w, "enabled", type->enabled);
	spdk_json_write_named_array_begin(w, "stat_names");
	for (i = 0; i < type->info->num_stats; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name", type->info->stats[i].name);
		spdk_json_write_named_uint64(w, "count", type->info->stats[i].count);
		spdk_json_write_named_string(w, "type", spdk_telemetry_stat_type_name(type->info->stats[i].type));
		if (type->info->stats[i].type == SPDK_TELEMETRY_STAT_TYPE_SUBTYPE) {
			spdk_json_write_named_string(w, "type_name", type->info->stats[i].extra.type_name);
		}
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

void
telemetry_dump_types_json(struct spdk_json_write_ctx *w, const char *name)
{
	struct spdk_telemetry_type *type;

	assert(telemetry_mgr_is_initialized());
	assert(w != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(type, &g_telemetry_mgr.types, link) {
		/* If the type is marked for deletion, skip it */
		if (type->to_delete) {
			continue;
		}

		/* If the type name matches the name parameter, dump it */
		if (name != NULL && strcmp(type->info->name, name) != 0) {
			continue;
		}

		telemetry_dump_type_json(type, w);
	}
	spdk_json_write_array_end(w);
}

int
telemetry_type_enable(const char *name, bool enable)
{
	struct spdk_telemetry_type *type;

	assert(telemetry_mgr_is_initialized());
	assert(name != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	type = telemetry_find_type(name);
	if (type == NULL) {
		SPDK_ERRLOG("Telemetry type %s not found\n", name);
		return -ENOENT;
	}

	if (type->to_delete) {
		SPDK_ERRLOG("Telemetry type %s is being deleted\n", name);
		return -EBUSY;
	}

	if (type->enabled == enable) {
		SPDK_ERRLOG("Telemetry type %s is already %s\n", name, enable ? "enabled" : "disabled");
		return -EALREADY;
	}

	type->enabled = enable;

	SPDK_DEBUGLOG(telemetry, "Telemetry type %s %s\n", name, enable ? "enabled" : "disabled");

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(telemetry);
