#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = fsdev.c fsdev_op.c fsdev_rpc.c
LIBNAME = fsdev

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_fsdev.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
