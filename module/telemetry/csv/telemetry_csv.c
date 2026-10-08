/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/telemetry.h"
#include "telemetry_csv_internal.h"

#define SOURCE_NAME_HEADER "source"

struct telemetry_csv_exporter {
	struct spdk_telemetry_exporter exporter;
};

struct spdk_telemetry_source_handle {
	struct spdk_telemetry_type_handle *type;
	TAILQ_ENTRY(spdk_telemetry_source_handle) link;
	char name[];
};

struct spdk_telemetry_type_handle {
	FILE *file;
	const struct spdk_telemetry_type_info *info;
	TAILQ_ENTRY(spdk_telemetry_type_handle) link;
	TAILQ_HEAD(, spdk_telemetry_source_handle) sources;
};

struct telemetry_csv {
	struct telemetry_csv_exporter exporter;
	TAILQ_HEAD(, spdk_telemetry_type_handle) types;
	char *dst_dir;
};

static struct telemetry_csv g_telemetry_csv = {0};

static int
telemetry_csv_init(void)
{
	TAILQ_INIT(&g_telemetry_csv.types);

	return 0;
}

static void
telemetry_csv_destroy_source(struct spdk_telemetry_source_handle *source)
{
	TAILQ_REMOVE(&source->type->sources, source, link);
	free(source);
}

static void
telemetry_csv_destroy_type(struct spdk_telemetry_type_handle *type)
{
	TAILQ_REMOVE(&g_telemetry_csv.types, type, link);
	while (!TAILQ_EMPTY(&type->sources)) {
		struct spdk_telemetry_source_handle *source;
		source = TAILQ_FIRST(&type->sources);
		telemetry_csv_destroy_source(source);
	}
	if (type->file != NULL) {
		fclose(type->file);
	}
	free(type);
}

static void
telemetry_csv_fini(void)
{
	if (g_telemetry_csv.exporter.exporter.ctxt) {
		telemetry_csv_delete();
	}

	assert(TAILQ_EMPTY(&g_telemetry_csv.types));

	free(g_telemetry_csv.dst_dir);
}

static struct spdk_telemetry_exporter_module telemetry_csv_module = {
	.name = "csv",
	.init = telemetry_csv_init,
	.fini = telemetry_csv_fini,
};

static void
telemetry_csv_write_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	assert(g_telemetry_csv.exporter.exporter.ctxt != NULL);
	assert(g_telemetry_csv.dst_dir != NULL);

	spdk_json_write_object_begin(w); /* method */
	spdk_json_write_named_string(w, "method", "telemetry_csv_create");
	spdk_json_write_named_object_begin(w, "params"); /* params */
	spdk_json_write_named_string(w, "dst_dir", g_telemetry_csv.dst_dir);
	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w); /* method */
}

static void
telemetry_csv_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	assert(g_telemetry_csv.exporter.exporter.ctxt != NULL);
	assert(g_telemetry_csv.dst_dir != NULL);

	spdk_json_write_named_string(w, "dst_dir", g_telemetry_csv.dst_dir);
}

static int
telemetry_csv_destruct(void *ctx)
{
	telemetry_csv_fini();

	return 0; /* synchronous destruct */
}

static struct spdk_telemetry_type_handle *
telemetry_csv_find_type(const char *name)
{
	struct spdk_telemetry_type_handle *type;
	TAILQ_FOREACH(type, &g_telemetry_csv.types, link) {
		if (strcmp(type->info->name, name) == 0) {
			return type;
		}
	}

	return NULL;
}

static int
telemetry_csv_write_type_header(FILE *file, struct spdk_telemetry_type_handle *type,
				char *prefix_buf, size_t prefix_buf_size, size_t offset)
{
	const struct spdk_telemetry_type_info *type_info = type->info;
	int res;
	struct spdk_telemetry_type_handle *nested_type;
	uint64_t i;

	for (i = 0; i < type_info->num_stats; i++) {
		const struct spdk_telemetry_stat_info *stat_info = &type_info->stats[i];

		switch (stat_info->type) {
		case SPDK_TELEMETRY_STAT_TYPE_UINT64:
		case SPDK_TELEMETRY_STAT_TYPE_INT64:
		case SPDK_TELEMETRY_STAT_TYPE_DOUBLE64:
			if (stat_info->count == 1) {
				fprintf(file, ",%.*s%s", (int)offset, prefix_buf, stat_info->name);
			} else {
				uint64_t j;
				for (j = 0; j < stat_info->count; j++) {
					fprintf(file, ",%.*s%s[%" PRIu64 "]", (int)offset, prefix_buf, stat_info->name, j);
				}
			}
			break;
		case SPDK_TELEMETRY_STAT_TYPE_SUBTYPE:
			assert(stat_info->extra.type_name != NULL);
			nested_type = telemetry_csv_find_type(stat_info->extra.type_name);
			if (nested_type == NULL) {
				SPDK_WARNLOG("Nested telemetry type %s not found\n", stat_info->extra.type_name);
				assert(false);
				return -ENOENT;
			}

			if (stat_info->count == 1) {
				int rc = snprintf(prefix_buf + offset, prefix_buf_size - offset, "%s.", stat_info->name);
				if ((size_t)rc >= prefix_buf_size - offset) {
					SPDK_ERRLOG("Failed to write prefix for type %s\n", stat_info->name);
					assert(false);
					return -ENOMEM;
				}

				res = telemetry_csv_write_type_header(file, nested_type, prefix_buf, prefix_buf_size, offset + rc);
				if (res != 0) {
					SPDK_ERRLOG("Failed to write header for type %s: %s\n", stat_info->name, spdk_strerror(-res));
					return res;
				}
			} else {
				uint64_t j;
				for (j = 0; j < stat_info->count; j++) {
					int rc = snprintf(prefix_buf + offset, prefix_buf_size - offset, "%s[%" PRIu64 "].",
							  stat_info->name, j);
					if ((size_t)rc >= prefix_buf_size - offset) {
						SPDK_ERRLOG("Failed to write prefix for type %s\n", stat_info->name);
						assert(false);
						return -ENOMEM;
					}
					res = telemetry_csv_write_type_header(file, nested_type, prefix_buf, prefix_buf_size, offset + rc);
					if (res != 0) {
						SPDK_ERRLOG("Failed to write header for type %s: %s\n", stat_info->name, spdk_strerror(-res));
						return res;
					}
				}
			}
			break;
		default:
			SPDK_WARNLOG("Unknown stat type: %d\n", type_info->stats[i].type);
			assert(false);
			return -EINVAL;
		}
	}

	return 0;
}

static int
telemetry_csv_write_header(struct spdk_telemetry_type_handle *type)
{
	int res;
	char full_prefix[4096] = {0};

	fprintf(type->file, "%s", SOURCE_NAME_HEADER);

	res = telemetry_csv_write_type_header(type->file, type, full_prefix, sizeof(full_prefix), 0);
	if (res != 0) {
		SPDK_ERRLOG("Failed to write header for type %s: %s\n", type->info->name, spdk_strerror(-res));
		return res;
	}

	fprintf(type->file, "\n");
	return 0;
}

static int
telemetry_csv_prepare_file(struct spdk_telemetry_type_handle *type)
{
	const struct spdk_telemetry_type_info *type_info = type->info;
	int res;
	char *file_path;

	file_path = spdk_sprintf_alloc("%s/%s.csv", g_telemetry_csv.dst_dir, type_info->name);
	if (file_path == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for %s file path\n", type_info->name);
		return -ENOMEM;
	}

	type->file = fopen(file_path, "w");
	free(file_path);
	if (type->file == NULL) {
		res = -errno;
		SPDK_ERRLOG("Failed to open file %s/%s.csv: %s\n", g_telemetry_csv.dst_dir, type_info->name,
			    spdk_strerror(-res));
		return res;
	}

	res = telemetry_csv_write_header(type);
	if (res != 0) {
		fclose(type->file);
		type->file = NULL;
		SPDK_ERRLOG("Failed to write header for type %s: %s\n", type_info->name, spdk_strerror(-res));
		return res;
	}

	return 0;
}

static struct spdk_telemetry_type_handle *
telemetry_csv_register_type(void *ctx, const struct spdk_telemetry_type_info *type_info)
{
	struct spdk_telemetry_type_handle *type;

	type = calloc(1, sizeof(*type));
	if (type == NULL) {
		return NULL;
	}

	type->info = type_info;
	TAILQ_INIT(&type->sources);
	TAILQ_INSERT_TAIL(&g_telemetry_csv.types, type, link);

	return type;
}

static int
telemetry_csv_report_type_stats(struct spdk_telemetry_type_handle *type, const char *stats,
				uint64_t *offset, uint64_t stats_buffer_size)
{
	uint64_t i;

	for (i = 0; i < type->info->num_stats; i++) {
		const struct spdk_telemetry_stat_info *stat_info = &type->info->stats[i];
		struct spdk_telemetry_type_handle *nested_type;
		uint64_t j;

#define REPORT_SINGLE_TYPE(t, fmt) \
	if (*offset + sizeof(t) * stat_info->count > stats_buffer_size) { \
		SPDK_ERRLOG("Stats buffer too small for type %s\n", type->info->name); \
		assert(false); \
		return -EINVAL; \
	} \
	for (j = 0; j < stat_info->count; j++) { \
		t value = *(const t *)(stats + (*offset)); \
		fprintf(type->file, "," fmt, value); \
		*offset += sizeof(t); \
	}

		switch (stat_info->type) {
		case SPDK_TELEMETRY_STAT_TYPE_UINT64:
			REPORT_SINGLE_TYPE(uint64_t, "%" PRIu64);
			break;
		case SPDK_TELEMETRY_STAT_TYPE_INT64:
			REPORT_SINGLE_TYPE(int64_t, "%" PRId64);
			break;
		case SPDK_TELEMETRY_STAT_TYPE_DOUBLE64:
			REPORT_SINGLE_TYPE(double, "%g");
			break;
		case SPDK_TELEMETRY_STAT_TYPE_SUBTYPE:
			assert(stat_info->extra.type_name != NULL);
			nested_type = telemetry_csv_find_type(stat_info->extra.type_name);
			if (nested_type == NULL) {
				SPDK_WARNLOG("Nested telemetry type %s not found\n", stat_info->extra.type_name);
				assert(false);
				return -ENOENT;
			}
			for (j = 0; j < stat_info->count; j++) {
				int res = telemetry_csv_report_type_stats(nested_type, stats, offset, stats_buffer_size);
				if (res != 0) {
					SPDK_ERRLOG("Failed to report stats for type %s: %s\n", nested_type->info->name,
						    spdk_strerror(-res));
					return res;
				}
			}
			break;
		default:
			SPDK_WARNLOG("Unknown stat type: %d\n", stat_info->type);
			assert(false);
			return -EINVAL;
		}
	}
#undef REPORT_SINGLE_TYPE

	return 0;
}

static bool
telemetry_csv_report_stats(void *ctx, struct spdk_telemetry_source_handle *source,
			   const void *stats_buffer, uint64_t stats_buffer_size)
{
	struct spdk_telemetry_type_handle *type;
	int res;
	uint64_t offset = 0;

	assert(source != NULL);
	assert(stats_buffer != NULL);
	assert(stats_buffer_size > 0);

	type = source->type;
	fprintf(type->file, "%s", source->name);
	res = telemetry_csv_report_type_stats(type, stats_buffer, &offset, stats_buffer_size);
	if (res != 0) {
		SPDK_ERRLOG("Failed to report stats for type %s: %s\n", type->info->name, spdk_strerror(-res));
		return false;
	}

	fprintf(type->file, "\n");
	fflush(type->file);
	return true;
}

static void
telemetry_csv_unregister_type(void *ctx, struct spdk_telemetry_type_handle *type)
{
	telemetry_csv_destroy_type(type);
}

static struct spdk_telemetry_source_handle *
telemetry_csv_register_source(void *ctx, struct spdk_telemetry_type_handle *type, const char *name)
{
	struct spdk_telemetry_source_handle *source;
	size_t len = strlen(name);

	source = calloc(1, sizeof(*source) + len + 1);
	if (source == NULL) {
		return NULL;
	}

	if (type->file == NULL) {
		int rc = telemetry_csv_prepare_file(type);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to prepare file for type %s: %s\n", type->info->name, spdk_strerror(-rc));
			free(source);
			return NULL;
		}
	}

	source->type = type;
	memcpy(source->name, name, len + 1);

	TAILQ_INSERT_TAIL(&type->sources, source, link);

	return source;
}

static void
telemetry_csv_unregister_source(void *ctx, struct spdk_telemetry_source_handle *source)
{
	telemetry_csv_destroy_source(source);
}

static struct spdk_telemetry_exporter_fn_table telemetry_csv_fn_table = {
	.destruct = telemetry_csv_destruct,
	.register_type = telemetry_csv_register_type,
	.unregister_type = telemetry_csv_unregister_type,
	.register_source = telemetry_csv_register_source,
	.unregister_source = telemetry_csv_unregister_source,
	.report_stats = telemetry_csv_report_stats,
	.write_config_json = telemetry_csv_write_config_json,
	.dump_info_json = telemetry_csv_dump_info_json,
};

int
telemetry_csv_create(const char *dst_dir)
{
	int res;

	if (g_telemetry_csv.exporter.exporter.ctxt != NULL) {
		SPDK_ERRLOG("Telemetry CSV exporter already registered\n");
		return -EEXIST;
	}

	g_telemetry_csv.dst_dir = strdup(dst_dir);
	if (g_telemetry_csv.dst_dir == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for dst_dir\n");
		return -ENOMEM;
	}

	g_telemetry_csv.exporter.exporter.ctxt = &g_telemetry_csv.exporter.exporter;
	g_telemetry_csv.exporter.exporter.fn_table = &telemetry_csv_fn_table;
	g_telemetry_csv.exporter.exporter.module = &telemetry_csv_module;

	res = spdk_telemetry_exporter_register(&g_telemetry_csv.exporter.exporter);
	if (res != 0) {
		SPDK_ERRLOG("Failed to register telemetry CSV exporter: %d\n", res);
		free(g_telemetry_csv.dst_dir);
		g_telemetry_csv.dst_dir = NULL;
		g_telemetry_csv.exporter.exporter.ctxt = NULL;
		g_telemetry_csv.exporter.exporter.fn_table = NULL;
		g_telemetry_csv.exporter.exporter.module = NULL;
		return res;
	}

	return 0;
}

int
telemetry_csv_delete(void)
{
	int res;

	if (g_telemetry_csv.exporter.exporter.ctxt == NULL) {
		SPDK_ERRLOG("Telemetry CSV exporter not registered\n");
		return -ENOENT;
	}

	res = spdk_telemetry_exporter_unregister(&g_telemetry_csv.exporter.exporter);
	if (res != 0) {
		SPDK_ERRLOG("Failed to unregister telemetry CSV exporter: %d\n", res);
		return res;
	}

	while (!TAILQ_EMPTY(&g_telemetry_csv.types)) {
		struct spdk_telemetry_type_handle *type;
		type = TAILQ_FIRST(&g_telemetry_csv.types);
		telemetry_csv_destroy_type(type);
	}

	free(g_telemetry_csv.dst_dir);
	g_telemetry_csv.dst_dir = NULL;
	g_telemetry_csv.exporter.exporter.ctxt = NULL;
	return 0;
}

SPDK_TELEMETRY_MODULE_REGISTER(csv, &telemetry_csv_module);
