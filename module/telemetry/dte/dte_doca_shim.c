/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * Shim layer for DOCA libraries.
 *
 * dlopen's libdoca_common and libdoca_telemetry_exporter at runtime and provides
 * local wrapper implementations for every symbol used by this module.
 * This avoids a hard link-time dependency on the DOCA shared libraries.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "dte_doca_shim.h"

#include <doca_error.h>
#include <doca_version.h>
#include <doca_telemetry_exporter.h>

/* We define our macro as DOCA_VERSION_LTE_CURRENT() uses casting to size_t, which is not supported by the preprocessor. */
#define DOCA_VERSION_AT_LEAST(maj, min, pat) \
    ((DOCA_VERSION_MAJOR > (maj)) || \
     (DOCA_VERSION_MAJOR == (maj) && DOCA_VERSION_MINOR > (min)) || \
     (DOCA_VERSION_MAJOR == (maj) && DOCA_VERSION_MINOR == (min) && DOCA_VERSION_PATCH >= (pat)))

#if DOCA_VERSION_AT_LEAST(3, 5, 31)
#define SOURCE_EXPORT_ARGS (struct doca_telemetry_exporter_source *p1, doca_telemetry_exporter_type_index_t p2, const void *p3, int p4)
#else
#define SOURCE_EXPORT_ARGS (struct doca_telemetry_exporter_source *p1, doca_telemetry_exporter_type_index_t p2, void *p3, int p4)
#endif

/* DOCA Telemetry Exporter symbols info */
#define DOCA_SYMBOLS() \
	DOCA_SYMBOL_ERR1(field_create, (struct doca_telemetry_exporter_field **p1)) \
	DOCA_SYMBOL_ERR1(field_destroy, (struct doca_telemetry_exporter_field *p1)) \
	DOCA_SYMBOL_VOID2(field_set_array_len, (struct doca_telemetry_exporter_field *p1, uint16_t p2)) \
	DOCA_SYMBOL_VOID2(field_set_description, (struct doca_telemetry_exporter_field *p1, const char *p2)) \
	DOCA_SYMBOL_VOID2(field_set_name, (struct doca_telemetry_exporter_field *p1, const char *p2)) \
	DOCA_SYMBOL_VOID2(field_set_type_name, (struct doca_telemetry_exporter_field *p1, const char *p2)) \
	DOCA_SYMBOL_ERR1(type_create, (struct doca_telemetry_exporter_type **p1)) \
	DOCA_SYMBOL_ERR2(type_add_field, (struct doca_telemetry_exporter_type *p1, struct doca_telemetry_exporter_field *p2)) \
	DOCA_SYMBOL_ERR1(type_destroy, (struct doca_telemetry_exporter_type *p1)) \
	DOCA_SYMBOL_ERR2(schema_init, (const char *p1, struct doca_telemetry_exporter_schema **p2)) \
	DOCA_SYMBOL_VOID2(schema_set_buf_size, (struct doca_telemetry_exporter_schema *p1, uint64_t p2)) \
	DOCA_SYMBOL_VOID2(schema_set_buf_data_root, (struct doca_telemetry_exporter_schema *p1, const char *p2)) \
	DOCA_SYMBOL_VOID1(schema_set_file_write_enabled, (struct doca_telemetry_exporter_schema *p1)) \
	DOCA_SYMBOL_VOID2(schema_set_file_write_max_size, (struct doca_telemetry_exporter_schema *p1, size_t p2)) \
	DOCA_SYMBOL_VOID2(schema_set_file_write_max_age, (struct doca_telemetry_exporter_schema *p1, doca_telemetry_exporter_timestamp_t p2)) \
	DOCA_SYMBOL_VOID1(schema_set_ipc_enabled, (struct doca_telemetry_exporter_schema *p1)) \
	DOCA_SYMBOL_VOID2(schema_set_ipc_sockets_dir, (struct doca_telemetry_exporter_schema *p1, const char *p2)) \
	DOCA_SYMBOL_VOID2(schema_set_ipc_reconnect_time, (struct doca_telemetry_exporter_schema *p1, uint32_t p2)) \
	DOCA_SYMBOL_VOID2(schema_set_ipc_reconnect_tries, (struct doca_telemetry_exporter_schema *p1, uint8_t p2)) \
	DOCA_SYMBOL_VOID2(schema_set_ipc_socket_timeout, (struct doca_telemetry_exporter_schema *p1, uint32_t p2)) \
	DOCA_SYMBOL_ERR4(schema_add_type, (struct doca_telemetry_exporter_schema *p1, const char *p2, struct doca_telemetry_exporter_type *p3, doca_telemetry_exporter_type_index_t *p4)) \
	DOCA_SYMBOL_ERR1(schema_start, (struct doca_telemetry_exporter_schema *p1)) \
	DOCA_SYMBOL_ERR1(schema_destroy, (struct doca_telemetry_exporter_schema *p1)) \
	DOCA_SYMBOL_ERR2(source_create, (struct doca_telemetry_exporter_schema *p1, struct doca_telemetry_exporter_source **p2)) \
	DOCA_SYMBOL_VOID2(source_set_id, (struct doca_telemetry_exporter_source *p1, const char *p2)) \
	DOCA_SYMBOL_VOID2(source_set_tag, (struct doca_telemetry_exporter_source *p1, const char *p2)) \
	DOCA_SYMBOL_ERR1(source_start, (struct doca_telemetry_exporter_source *p1)) \
	DOCA_SYMBOL_ERR4(source_report, SOURCE_EXPORT_ARGS) \
	DOCA_SYMBOL_ERR1(source_flush, (struct doca_telemetry_exporter_source *p1)) \
	DOCA_SYMBOL_ERR1(source_destroy, (struct doca_telemetry_exporter_source *p1))

#define DECLARE_DOCA_SYMBOL(return_type, name, args) \
	return_type (*name)args; \

#define LOAD_DOCA_SYMBOL(field, handle, symbol_name) \
	do { \
		g_shim.field = (typeof(g_shim.field))dlsym(handle, symbol_name); \
		if (g_shim.field == NULL) { \
			SPDK_ERRLOG("doca shim: dlsym(\"%s\") failed: %s\n", \
					symbol_name, dlerror()); \
			dte_doca_shim_fini(); \
			return -ENOTSUP; \
		} \
	} while (0)

#define LOAD_DOCA_TELEMETRY_EXPORTER_SYMBOL(field, handle) \
	LOAD_DOCA_SYMBOL(field, handle, "doca_telemetry_exporter_" #field)

static struct {
	void *hdl_common;
	void *hdl_telemetry_exporter;

	/* Symbols that require special handling */
	const char *(*error_get_name)(doca_error_t error);

	/* Rest of the symbols */
#define DOCA_SYMBOL_ERR1(name, args) DECLARE_DOCA_SYMBOL(doca_error_t, name, args)
#define DOCA_SYMBOL_VOID1(name, args) DECLARE_DOCA_SYMBOL(void, name, args)
#define DOCA_SYMBOL_VOID2(name, args) DECLARE_DOCA_SYMBOL(void, name, args)
#define DOCA_SYMBOL_ERR2(name, args) DECLARE_DOCA_SYMBOL(doca_error_t, name, args)
#define DOCA_SYMBOL_ERR4(name, args) DECLARE_DOCA_SYMBOL(doca_error_t, name, args)

	DOCA_SYMBOLS()

#undef DOCA_SYMBOL_ERR1
#undef DOCA_SYMBOL_VOID1
#undef DOCA_SYMBOL_VOID2
#undef DOCA_SYMBOL_ERR2
#undef DOCA_SYMBOL_ERR4
} g_shim;

int
dte_doca_shim_init(void)
{
	g_shim.hdl_common = dlopen(DOCA_COMMON_LIB, RTLD_NOW | RTLD_GLOBAL);
	if (g_shim.hdl_common == NULL) {
		SPDK_ERRLOG("doca shim: dlopen(" DOCA_COMMON_LIB ") failed: %s\n", dlerror());
		return -ENOTSUP;
	}

	g_shim.hdl_telemetry_exporter = dlopen(DOCA_TELEMETRY_EXPORTER_LIB,
					       RTLD_NOW | RTLD_GLOBAL);
	if (g_shim.hdl_telemetry_exporter == NULL) {
		SPDK_ERRLOG("doca shim: dlopen(" DOCA_TELEMETRY_EXPORTER_LIB ") failed: %s\n",
			    dlerror());
		dlclose(g_shim.hdl_common);
		g_shim.hdl_common = NULL;
		return -ENOTSUP;
	}

	LOAD_DOCA_SYMBOL(error_get_name, g_shim.hdl_common, "doca_error_get_name");

#define DOCA_SYMBOL_ERR1(name, args) LOAD_DOCA_TELEMETRY_EXPORTER_SYMBOL(name, g_shim.hdl_telemetry_exporter);
#define DOCA_SYMBOL_VOID1(name, args) LOAD_DOCA_TELEMETRY_EXPORTER_SYMBOL(name, g_shim.hdl_telemetry_exporter);
#define DOCA_SYMBOL_VOID2(name, args) LOAD_DOCA_TELEMETRY_EXPORTER_SYMBOL(name, g_shim.hdl_telemetry_exporter);
#define DOCA_SYMBOL_ERR2(name, args) LOAD_DOCA_TELEMETRY_EXPORTER_SYMBOL(name, g_shim.hdl_telemetry_exporter);
#define DOCA_SYMBOL_ERR4(name, args) LOAD_DOCA_TELEMETRY_EXPORTER_SYMBOL(name, g_shim.hdl_telemetry_exporter);

	DOCA_SYMBOLS()

#undef DOCA_SYMBOL_ERR1
#undef DOCA_SYMBOL_VOID1
#undef DOCA_SYMBOL_VOID2
#undef DOCA_SYMBOL_ERR2
#undef DOCA_SYMBOL_ERR4

	SPDK_DEBUGLOG(telemetry_dte, "doca shim initialized\n");
	return 0;
}

void
dte_doca_shim_fini(void)
{
	if (g_shim.hdl_telemetry_exporter != NULL) {
		dlclose(g_shim.hdl_telemetry_exporter);
		g_shim.hdl_telemetry_exporter = NULL;
	}
	if (g_shim.hdl_common != NULL) {
		dlclose(g_shim.hdl_common);
		g_shim.hdl_common = NULL;
	}
	memset(&g_shim, 0, sizeof(g_shim));
	SPDK_DEBUGLOG(telemetry_dte, "doca shim finalized\n");
}

const char *
shim_doca_error_get_name(doca_error_t error)
{
	if (g_shim.error_get_name == NULL) {
		return "DOCA_ERROR_UNKNOWN";
	}

	return g_shim.error_get_name(error);
}

#define DOCA_SYMBOL_ERR1(name, args) \
doca_error_t \
doca_telemetry_exporter_##name args \
{ \
	return g_shim.name(p1); \
}

#define DOCA_SYMBOL_VOID1(name, args) \
void \
doca_telemetry_exporter_##name args \
{ \
	g_shim.name(p1); \
}

#define DOCA_SYMBOL_VOID2(name, args) \
void \
doca_telemetry_exporter_##name args \
{ \
	g_shim.name(p1, p2); \
}

#define DOCA_SYMBOL_ERR2(name, args) \
doca_error_t \
doca_telemetry_exporter_##name args \
{ \
	return g_shim.name(p1, p2); \
}

#define DOCA_SYMBOL_ERR4(name, args) \
doca_error_t \
doca_telemetry_exporter_##name args \
{ \
	return g_shim.name(p1, p2, p3, p4); \
}

DOCA_SYMBOLS()

#undef DOCA_SYMBOL_ERR1
#undef DOCA_SYMBOL_VOID1
#undef DOCA_SYMBOL_VOID2
#undef DOCA_SYMBOL_ERR2
#undef DOCA_SYMBOL_ERR4
