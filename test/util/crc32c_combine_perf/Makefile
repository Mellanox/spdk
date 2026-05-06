#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = crc32c_combine_perf

C_SRCS = crc32c_combine_perf.c

SPDK_LIB_LIST = util

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
