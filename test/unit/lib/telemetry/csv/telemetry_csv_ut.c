/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "unit/lib/json_mock.c"

/* Pull in the declarations for the functions we are stubbing out. */
#include "spdk/stdinc.h"
#include "spdk/telemetry.h"

/*
 * Stubs for the telemetry core functions called by telemetry_csv.c.
 * We test the CSV module in isolation; the core is not linked here.
 */
static struct spdk_telemetry_exporter *g_registered_exporter;
static int g_register_rc;

int
spdk_telemetry_exporter_register(struct spdk_telemetry_exporter *exporter)
{
	g_registered_exporter = exporter;
	return g_register_rc;
}

int
spdk_telemetry_exporter_unregister(struct spdk_telemetry_exporter *exporter)
{
	if (g_registered_exporter == exporter) {
		g_registered_exporter = NULL;
	}
	return 0;
}

void
spdk_telemetry_module_list_add(struct spdk_telemetry_exporter_module *module)
{
}

#include "telemetry/csv/telemetry_csv.c"

/* ── Global temp dir for CSV files ─────────────────────────────────────── */
static char g_tmpdir[256];

/* Read entire file into buf (NUL-terminated). Returns bytes read or -1. */
static int
ut_read_file(const char *path, char *buf, size_t buf_size)
{
	FILE *f = fopen(path, "r");
	int   n;

	if (!f) {
		return -1;
	}
	n = (int)fread(buf, 1, buf_size - 1, f);
	buf[n] = '\0';
	fclose(f);
	return n;
}

/* Reset CSV global state between tests */
static void
ut_reset_csv(void)
{
	memset(&g_telemetry_csv, 0, sizeof(g_telemetry_csv));
	TAILQ_INIT(&g_telemetry_csv.types);
	g_registered_exporter = NULL;
	g_register_rc         = 0;
}

/*
 * Remove all regular files in dir and then the directory itself. The tests
 * only ever produce flat .csv files, so a single non-recursive pass suffices.
 */
static void
ut_remove_tmpdir(const char *dir)
{
	DIR           *d;
	struct dirent *ent;
	char           path[512];

	d = opendir(dir);
	if (d == NULL) {
		return;
	}

	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		unlink(path);
	}

	closedir(d);
	rmdir(dir);
}

/* ── Lifecycle tests ────────────────────────────────────────────────────── */

static void
test_csv_create_delete(void)
{
	int rc;

	ut_reset_csv();

	rc = telemetry_csv_create(g_tmpdir);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_registered_exporter != NULL);
	CU_ASSERT(g_telemetry_csv.dst_dir != NULL);
	CU_ASSERT_STRING_EQUAL(g_telemetry_csv.dst_dir, g_tmpdir);
	CU_ASSERT(g_telemetry_csv.exporter.exporter.ctxt != NULL);

	rc = telemetry_csv_delete();
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_registered_exporter == NULL);
	CU_ASSERT(g_telemetry_csv.dst_dir == NULL);
	CU_ASSERT(g_telemetry_csv.exporter.exporter.ctxt == NULL);
}

static void
test_csv_create_duplicate(void)
{
	int rc;

	ut_reset_csv();

	rc = telemetry_csv_create(g_tmpdir);
	CU_ASSERT_EQUAL(rc, 0);

	rc = telemetry_csv_create(g_tmpdir);
	CU_ASSERT_EQUAL(rc, -EEXIST);

	CU_ASSERT_EQUAL(telemetry_csv_delete(), 0);
}

static void
test_csv_delete_not_created(void)
{
	ut_reset_csv();
	CU_ASSERT_EQUAL(telemetry_csv_delete(), -ENOENT);
}

/* ── Header format tests ────────────────────────────────────────────────── */

static void
test_csv_simple_header(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("bytes_read",  1),
		SPDK_TELEMETRY_STAT_INFO_UINT64("bytes_write", 1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_simple_hdr",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	char file_path[512], content[1024];
	int  rc;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	SPDK_CU_ASSERT_FATAL(type_h != NULL);
	/* File is created lazily on first source registration */
	CU_ASSERT(type_h->file == NULL);

	src_h = fn->register_source(ctx, type_h, "bdev0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);
	CU_ASSERT(type_h->file != NULL);

	snprintf(file_path, sizeof(file_path), "%s/csv_simple_hdr.csv", g_tmpdir);
	fflush(type_h->file);

	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT_STRING_EQUAL(content, "source,bytes_read,bytes_write\n");

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* Array stat: header should expand to field[0], field[1], ... */
static void
test_csv_array_header(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("latency_ns", 4),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_arr_hdr",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	char file_path[512], content[1024];
	int  rc;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	SPDK_CU_ASSERT_FATAL(type_h != NULL);
	src_h  = fn->register_source(ctx, type_h, "bdev0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	snprintf(file_path, sizeof(file_path), "%s/csv_arr_hdr.csv", g_tmpdir);
	fflush(type_h->file);

	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT_STRING_EQUAL(content,
			       "source,latency_ns[0],latency_ns[1],latency_ns[2],latency_ns[3]\n");

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* Nested subtype: header should be parent_field.sub_field */
static void
test_csv_nested_header(void)
{
	static const struct spdk_telemetry_stat_info sub_stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("reads",  1),
		SPDK_TELEMETRY_STAT_INFO_UINT64("writes", 1),
	};
	static const struct spdk_telemetry_type_info sub_type_info = {
		.name      = "csv_nested_sub",
		.num_stats = SPDK_COUNTOF(sub_stats),
		.stats     = sub_stats,
	};
	static const struct spdk_telemetry_stat_info parent_stats[] = {
		SPDK_TELEMETRY_STAT_INFO_SUBTYPE("io", 1, "csv_nested_sub"),
	};
	static const struct spdk_telemetry_type_info parent_type_info = {
		.name      = "csv_nested_parent",
		.num_stats = SPDK_COUNTOF(parent_stats),
		.stats     = parent_stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *sub_h, *parent_h;
	struct spdk_telemetry_source_handle *src_h;
	char file_path[512], content[1024];
	int  rc;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	/* Subtype must be registered first so the header writer can find it */
	sub_h    = fn->register_type(ctx, &sub_type_info);
	SPDK_CU_ASSERT_FATAL(sub_h != NULL);
	parent_h = fn->register_type(ctx, &parent_type_info);
	SPDK_CU_ASSERT_FATAL(parent_h != NULL);

	src_h = fn->register_source(ctx, parent_h, "device0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	snprintf(file_path, sizeof(file_path), "%s/csv_nested_parent.csv", g_tmpdir);
	fflush(parent_h->file);

	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT_STRING_EQUAL(content, "source,io.reads,io.writes\n");

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, parent_h);
	fn->unregister_type(ctx, sub_h);
	telemetry_csv_delete();
}

/* Array of subtypes: parent_field[i].sub_field */
static void
test_csv_nested_array_header(void)
{
	static const struct spdk_telemetry_stat_info sub_stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("ops", 1),
	};
	static const struct spdk_telemetry_type_info sub_type_info = {
		.name      = "csv_queue_sub",
		.num_stats = SPDK_COUNTOF(sub_stats),
		.stats     = sub_stats,
	};
	static const struct spdk_telemetry_stat_info parent_stats[] = {
		SPDK_TELEMETRY_STAT_INFO_SUBTYPE("queues", 3, "csv_queue_sub"),
	};
	static const struct spdk_telemetry_type_info parent_type_info = {
		.name      = "csv_queue_parent",
		.num_stats = SPDK_COUNTOF(parent_stats),
		.stats     = parent_stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *sub_h, *parent_h;
	struct spdk_telemetry_source_handle *src_h;
	char file_path[512], content[1024];
	int  rc;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	sub_h    = fn->register_type(ctx, &sub_type_info);
	SPDK_CU_ASSERT_FATAL(sub_h != NULL);
	parent_h = fn->register_type(ctx, &parent_type_info);
	SPDK_CU_ASSERT_FATAL(parent_h != NULL);

	src_h = fn->register_source(ctx, parent_h, "dev0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	snprintf(file_path, sizeof(file_path), "%s/csv_queue_parent.csv", g_tmpdir);
	fflush(parent_h->file);

	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT_STRING_EQUAL(content,
			       "source,queues[0].ops,queues[1].ops,queues[2].ops\n");

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, parent_h);
	fn->unregister_type(ctx, sub_h);
	telemetry_csv_delete();
}

/* ── Stats reporting tests ──────────────────────────────────────────────── */

/* A single source reports stats; verify source name and values in the row */
static void
test_csv_report_stats_single(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("bytes_read",  1),
		SPDK_TELEMETRY_STAT_INFO_UINT64("iops",        1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_report_single",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	uint64_t stat_buf[] = { 1000, 500 };
	char     file_path[512], content[2048];
	int      rc;
	bool     result;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	src_h  = fn->register_source(ctx, type_h, "src0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	result = fn->report_stats(ctx, src_h, stat_buf, sizeof(stat_buf));
	CU_ASSERT_EQUAL(result, true);

	snprintf(file_path, sizeof(file_path), "%s/csv_report_single.csv", g_tmpdir);

	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,bytes_read,iops\n") != NULL);
	CU_ASSERT(strstr(content, "src0,1000,500\n")          != NULL);

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* Two sources of the same type write into the same CSV file */
static void
test_csv_multiple_sources(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("io_count", 1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_multi_src",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src0_h, *src1_h;
	uint64_t buf0[] = { 100 };
	uint64_t buf1[] = { 200 };
	char file_path[512], content[2048];
	int  rc;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	src0_h = fn->register_source(ctx, type_h, "bdev0");
	SPDK_CU_ASSERT_FATAL(src0_h != NULL);
	src1_h = fn->register_source(ctx, type_h, "bdev1");
	SPDK_CU_ASSERT_FATAL(src1_h != NULL);

	fn->report_stats(ctx, src0_h, buf0, sizeof(buf0));
	fn->report_stats(ctx, src1_h, buf1, sizeof(buf1));

	snprintf(file_path, sizeof(file_path), "%s/csv_multi_src.csv", g_tmpdir);

	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,io_count\n") != NULL);
	CU_ASSERT(strstr(content, "bdev0,100\n")        != NULL);
	CU_ASSERT(strstr(content, "bdev1,200\n")        != NULL);

	fn->unregister_source(ctx, src0_h);
	fn->unregister_source(ctx, src1_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* Two distinct types produce two separate CSV files */
static void
test_csv_multiple_types(void)
{
	static const struct spdk_telemetry_stat_info stats_a[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("field_a", 1),
	};
	static const struct spdk_telemetry_type_info type_a_info = {
		.name      = "csv_type_a",
		.num_stats = SPDK_COUNTOF(stats_a),
		.stats     = stats_a,
	};
	static const struct spdk_telemetry_stat_info stats_b[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("field_b", 1),
	};
	static const struct spdk_telemetry_type_info type_b_info = {
		.name      = "csv_type_b",
		.num_stats = SPDK_COUNTOF(stats_b),
		.stats     = stats_b,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_a_h, *type_b_h;
	struct spdk_telemetry_source_handle *src_a_h, *src_b_h;
	uint64_t buf_a[] = { 111 };
	uint64_t buf_b[] = { 222 };
	char file_path[512], content[1024];
	int  rc;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_a_h = fn->register_type(ctx, &type_a_info);
	type_b_h = fn->register_type(ctx, &type_b_info);
	src_a_h  = fn->register_source(ctx, type_a_h, "device0");
	SPDK_CU_ASSERT_FATAL(src_a_h != NULL);
	src_b_h  = fn->register_source(ctx, type_b_h, "device0");
	SPDK_CU_ASSERT_FATAL(src_b_h != NULL);

	fn->report_stats(ctx, src_a_h, buf_a, sizeof(buf_a));
	fn->report_stats(ctx, src_b_h, buf_b, sizeof(buf_b));

	snprintf(file_path, sizeof(file_path), "%s/csv_type_a.csv", g_tmpdir);
	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,field_a\n")  != NULL);
	CU_ASSERT(strstr(content, "device0,111\n")      != NULL);
	CU_ASSERT(strstr(content, "222")               == NULL);

	snprintf(file_path, sizeof(file_path), "%s/csv_type_b.csv", g_tmpdir);
	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,field_b\n")  != NULL);
	CU_ASSERT(strstr(content, "device0,222\n")      != NULL);
	CU_ASSERT(strstr(content, "111")               == NULL);

	fn->unregister_source(ctx, src_a_h);
	fn->unregister_source(ctx, src_b_h);
	fn->unregister_type(ctx, type_a_h);
	fn->unregister_type(ctx, type_b_h);
	telemetry_csv_delete();
}

/*
 * When spdk_telemetry_exporter_register() fails (g_register_rc != 0),
 * telemetry_csv_create() must return the error, free dst_dir and leave the
 * module unregistered, so that a retry can succeed.
 */
static void
test_csv_create_register_failure(void)
{
	int rc;

	ut_reset_csv();
	g_register_rc = -ENOENT;

	rc = telemetry_csv_create(g_tmpdir);
	CU_ASSERT_EQUAL(rc, -ENOENT);
	CU_ASSERT(g_telemetry_csv.dst_dir == NULL);
	CU_ASSERT(g_telemetry_csv.exporter.exporter.ctxt == NULL);

	g_register_rc = 0;
	rc = telemetry_csv_create(g_tmpdir);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT(g_telemetry_csv.dst_dir != NULL);

	rc = telemetry_csv_delete();
	CU_ASSERT_EQUAL(rc, 0);
}

/*
 * When the destination directory is not writable, register_source() must
 * return NULL and leave the type's file pointer NULL.
 */
static void
test_csv_register_source_file_failure(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("x", 1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_file_fail",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	/* Redirect dst_dir to a non-existent path so fopen will fail */
	free(g_telemetry_csv.dst_dir);
	g_telemetry_csv.dst_dir = strdup("/nonexistent_telemetry_test_path");
	SPDK_CU_ASSERT_FATAL(g_telemetry_csv.dst_dir != NULL);

	type_h = fn->register_type(ctx, &type_info);
	SPDK_CU_ASSERT_FATAL(type_h != NULL);

	src_h = fn->register_source(ctx, type_h, "dev0");
	CU_ASSERT(src_h == NULL);
	CU_ASSERT(type_h->file == NULL);

	/* Restore a valid dir so telemetry_csv_delete() can run cleanly */
	free(g_telemetry_csv.dst_dir);
	g_telemetry_csv.dst_dir = strdup(g_tmpdir);

	telemetry_csv_delete();
}

/* INT64 values, including negative, are formatted as signed integers */
static void
test_csv_int64_report_stats(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_INT64("temperature_c", 1),
		SPDK_TELEMETRY_STAT_INFO_INT64("offset_ns",     1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_int64_report",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	uint64_t stat_buf[2];
	int64_t  temp = -42, offset = 1000000;
	char     file_path[512], content[2048];
	int      rc;

	memcpy(&stat_buf[0], &temp,   sizeof(temp));
	memcpy(&stat_buf[1], &offset, sizeof(offset));

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	src_h  = fn->register_source(ctx, type_h, "sensor0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	CU_ASSERT_EQUAL(fn->report_stats(ctx, src_h, stat_buf, sizeof(stat_buf)), true);

	snprintf(file_path, sizeof(file_path), "%s/csv_int64_report.csv", g_tmpdir);
	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,temperature_c,offset_ns\n") != NULL);
	CU_ASSERT(strstr(content, "sensor0,-42,1000000\n")            != NULL);

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* DOUBLE64 values are formatted with %g (no trailing zeros) */
static void
test_csv_double_report_stats(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_DOUBLE64("utilization", 1),
		SPDK_TELEMETRY_STAT_INFO_DOUBLE64("latency_us",  1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_double_report",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	uint64_t stat_buf[2];
	double   util = 0.75, lat = 123.5;
	char     file_path[512], content[2048];
	int      rc;

	memcpy(&stat_buf[0], &util, sizeof(util));
	memcpy(&stat_buf[1], &lat,  sizeof(lat));

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	src_h  = fn->register_source(ctx, type_h, "dev0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	CU_ASSERT_EQUAL(fn->report_stats(ctx, src_h, stat_buf, sizeof(stat_buf)), true);

	snprintf(file_path, sizeof(file_path), "%s/csv_double_report.csv", g_tmpdir);
	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,utilization,latency_us\n") != NULL);
	CU_ASSERT(strstr(content, "dev0,0.75,123.5\n")               != NULL);

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* Array of INT64: exercises the per-element count loop with new types */
static void
test_csv_int64_array_report_stats(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_INT64("delta_ns", 3),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_int64_arr_report",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	uint64_t stat_buf[3];
	int64_t  v0 = -10, v1 = 20, v2 = -30;
	char     file_path[512], content[2048];
	int      rc;

	memcpy(&stat_buf[0], &v0, sizeof(v0));
	memcpy(&stat_buf[1], &v1, sizeof(v1));
	memcpy(&stat_buf[2], &v2, sizeof(v2));

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	src_h  = fn->register_source(ctx, type_h, "ch0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	CU_ASSERT_EQUAL(fn->report_stats(ctx, src_h, stat_buf, sizeof(stat_buf)), true);

	snprintf(file_path, sizeof(file_path), "%s/csv_int64_arr_report.csv", g_tmpdir);
	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,delta_ns[0],delta_ns[1],delta_ns[2]\n") != NULL);
	CU_ASSERT(strstr(content, "ch0,-10,20,-30\n")                             != NULL);

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* Mixed scalar types in one row: UINT64, INT64, DOUBLE64 all formatted correctly */
static void
test_csv_mixed_scalar_types(void)
{
	static const struct spdk_telemetry_stat_info stats[] = {
		SPDK_TELEMETRY_STAT_INFO_UINT64("io_count",    1),
		SPDK_TELEMETRY_STAT_INFO_INT64("queue_depth",  1),
		SPDK_TELEMETRY_STAT_INFO_DOUBLE64("bandwidth",  1),
	};
	static const struct spdk_telemetry_type_info type_info = {
		.name      = "csv_mixed_scalars",
		.num_stats = SPDK_COUNTOF(stats),
		.stats     = stats,
	};
	const struct spdk_telemetry_exporter_fn_table *fn;
	void *ctx;
	struct spdk_telemetry_type_handle   *type_h;
	struct spdk_telemetry_source_handle *src_h;
	uint64_t stat_buf[3];
	uint64_t io_count    = 500;
	int64_t  queue_depth = -3;
	double   bandwidth   = 1.5;
	char     file_path[512], content[2048];
	int      rc;

	memcpy(&stat_buf[0], &io_count,    sizeof(io_count));
	memcpy(&stat_buf[1], &queue_depth, sizeof(queue_depth));
	memcpy(&stat_buf[2], &bandwidth,   sizeof(bandwidth));

	ut_reset_csv();
	telemetry_csv_create(g_tmpdir);

	fn  = g_telemetry_csv.exporter.exporter.fn_table;
	ctx = g_telemetry_csv.exporter.exporter.ctxt;

	type_h = fn->register_type(ctx, &type_info);
	src_h  = fn->register_source(ctx, type_h, "dev0");
	SPDK_CU_ASSERT_FATAL(src_h != NULL);

	CU_ASSERT_EQUAL(fn->report_stats(ctx, src_h, stat_buf, sizeof(stat_buf)), true);

	snprintf(file_path, sizeof(file_path), "%s/csv_mixed_scalars.csv", g_tmpdir);
	rc = ut_read_file(file_path, content, sizeof(content));
	CU_ASSERT(rc > 0);
	CU_ASSERT(strstr(content, "source,io_count,queue_depth,bandwidth\n") != NULL);
	CU_ASSERT(strstr(content, "dev0,500,-3,1.5\n")                       != NULL);

	fn->unregister_source(ctx, src_h);
	fn->unregister_type(ctx, type_h);
	telemetry_csv_delete();
}

/* ── main ───────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
	CU_pSuite    suite = NULL;
	unsigned int num_failures;
	char         tmpdir_template[] = "/tmp/telemetry_csv_ut_XXXXXX";

	if (mkdtemp(tmpdir_template) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", tmpdir_template);

	CU_initialize_registry();

	suite = CU_add_suite("telemetry_csv", NULL, NULL);

	CU_ADD_TEST(suite, test_csv_create_delete);
	CU_ADD_TEST(suite, test_csv_create_duplicate);
	CU_ADD_TEST(suite, test_csv_delete_not_created);
	CU_ADD_TEST(suite, test_csv_simple_header);
	CU_ADD_TEST(suite, test_csv_array_header);
	CU_ADD_TEST(suite, test_csv_nested_header);
	CU_ADD_TEST(suite, test_csv_nested_array_header);
	CU_ADD_TEST(suite, test_csv_report_stats_single);
	CU_ADD_TEST(suite, test_csv_multiple_sources);
	CU_ADD_TEST(suite, test_csv_multiple_types);
	CU_ADD_TEST(suite, test_csv_create_register_failure);
	CU_ADD_TEST(suite, test_csv_register_source_file_failure);
	CU_ADD_TEST(suite, test_csv_int64_report_stats);
	CU_ADD_TEST(suite, test_csv_double_report_stats);
	CU_ADD_TEST(suite, test_csv_int64_array_report_stats);
	CU_ADD_TEST(suite, test_csv_mixed_scalar_types);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	ut_remove_tmpdir(g_tmpdir);

	return num_failures;
}
