/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_CRC_INTERNAL_H
#define SPDK_CRC_INTERNAL_H

#include "spdk/config.h"

#ifdef SPDK_CONFIG_ISAL
#define SPDK_HAVE_ISAL
#include <isa-l/include/crc.h>
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define SPDK_HAVE_ARM_CRC
#include <arm_acle.h>
#elif defined(__x86_64__) && defined(__SSE4_2__)
#define SPDK_HAVE_SSE4_2
#include <x86intrin.h>
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
#define SPDK_HAVE_ARM_PMULL
#include <arm_acle.h>
#include <arm_neon.h>
#elif defined(__x86_64__) && defined(__PCLMUL__)
#define SPDK_HAVE_X86_PCLMUL
#include <wmmintrin.h>
#endif

#endif /* SPDK_CRC_INTERNAL_H */
