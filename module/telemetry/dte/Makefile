#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

# Find a library's SONAME (e.g. "libfoo.so.2") via the ldconfig cache.
# $(1) - unversioned linker name (e.g. "libfoo.so").
# Prefers that entry (resolving its symlink to the SONAME), then falls back to a
# versioned cache entry.
define find_lib_soname
$(shell \
	lib_path=$$(ldconfig -p | grep '$(1)$$' | sed 's/.*=> //' | head -n 1); \
	if [ -n "$$lib_path" ]; then \
		if [ -L "$$lib_path" ]; then \
			basename "$$(readlink "$$lib_path")"; \
		else \
			basename "$$lib_path"; \
		fi; \
	else \
		lib_path=$$(ldconfig -p | grep -E '$(1)\.[0-9]+' | sed 's/.*=> //' | head -n 1); \
		if [ -n "$$lib_path" ]; then \
			basename "$$lib_path"; \
		fi; \
	fi \
)
endef

# Find a DOCA library SONAME.
# $(1) - name without libdoca_ prefix (e.g. "common" or "telemetry_exporter").
define find_doca_lib
$(call find_lib_soname,libdoca_$(1).so)
endef

DOCA_COMMON_LIB := $(call find_doca_lib,common)
ifeq ($(DOCA_COMMON_LIB),)
DOCA_COMMON_LIB := libdoca_common.so
endif

DOCA_TELEMETRY_EXPORTER_LIB := $(call find_doca_lib,telemetry_exporter)
ifeq ($(DOCA_TELEMETRY_EXPORTER_LIB),)
DOCA_TELEMETRY_EXPORTER_LIB := libdoca_telemetry_exporter.so
endif

CFLAGS += -DDOCA_COMMON_LIB=\"$(DOCA_COMMON_LIB)\"
CFLAGS += -DDOCA_TELEMETRY_EXPORTER_LIB=\"$(DOCA_TELEMETRY_EXPORTER_LIB)\"

C_SRCS = telemetry_dte.c telemetry_dte_rpc.c dte_doca_shim.c

LIBNAME = telemetry_dte

LOCAL_SYS_LIBS += -ldl

CHECK_DOCA_SYMBOLS = y

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
