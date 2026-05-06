/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CRC-32 utility functions
 */

#ifndef SPDK_CRC32_H
#define SPDK_CRC32_H

#include "spdk/stdinc.h"
#include "spdk/config.h"

#define SPDK_CRC32_SIZE_BYTES 4

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calculate a partial CRC-32 IEEE checksum.
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32 value.
 * \return Updated CRC-32 value.
 */
uint32_t spdk_crc32_ieee_update(const void *buf, size_t len, uint32_t crc);

/**
 * Calculate a partial CRC-32C checksum.
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_update(const void *buf, size_t len, uint32_t crc);

/**
 * Calculate a partial CRC-32C checksum.
 *
 * \param iov Data buffer vectors to checksum.
 * \param iovcnt size of iov parameter.
 * \param crc32c Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_iov_update(struct iovec *iov, int iovcnt, uint32_t crc32c);

/**
 * Calculate a CRC-32C checksum, for NVMe Protection Information
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_nvme(const void *buf, size_t len, uint32_t crc);

/**
 * Opaque CRC-32C combiner state. Constructed for a fixed chunk size and
 * carries a precomputed table used to fold per-chunk CRCs into a single
 * CRC of the concatenated buffer.
 */
struct spdk_crc32c_combiner;

/**
 * Allocate a CRC-32C combiner for a fixed chunk size.
 *
 * The combiner produced by this function can be used to fold up to
 * \c max_chunks per-chunk CRC-32C values, each computed over exactly
 * \c chunk_size bytes, into a single CRC-32C value covering the
 * concatenation of those chunks.
 *
 * \param chunk_size Size in bytes of each chunk. Must be > 0.
 * \param max_chunks Maximum number of CRCs that will be passed to a single
 *                   spdk_crc32c_combine() call. Must be > 0.
 * \return Newly allocated combiner, or NULL on allocation failure or invalid
 *         arguments.
 */
struct spdk_crc32c_combiner *spdk_crc32c_combiner_create(size_t chunk_size,
		size_t max_chunks);

/**
 * Free a combiner returned by spdk_crc32c_combiner_create().
 *
 * Passing NULL is a no-op.
 */
void spdk_crc32c_combiner_free(struct spdk_crc32c_combiner *combiner);

/**
 * Combine \c n_crcs per-chunk CRC-32C values into a single CRC-32C value.
 *
 * Every input CRC must cover exactly \c chunk_size bytes (the value passed
 * to spdk_crc32c_combiner_create()) -- there is no support for a shorter
 * tail chunk. Each \c crcs[i] is in the same convention as
 * spdk_crc32c_update() (no NVMe-style init/final complement). The CRCs are
 * interpreted as covering chunks in array order: crcs[0] is the first
 * chunk, crcs[n_crcs - 1] is the last. The result is the CRC-32C of the
 * concatenated buffer in the same convention.
 *
 * \param combiner Combiner returned by spdk_crc32c_combiner_create().
 * \param crcs Array of \c n_crcs raw CRC-32C values, in concatenation order.
 * \param n_crcs Number of values in \c crcs. Behavior is undefined if
 *               \c n_crcs exceeds the \c max_chunks value passed to
 *               spdk_crc32c_combiner_create() -- the caller is responsible
 *               for sizing the combiner accordingly.
 * \return Combined CRC-32C value.
 */
uint32_t spdk_crc32c_combine(const struct spdk_crc32c_combiner *combiner,
			     const uint32_t *crcs, size_t n_crcs);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_CRC32_H */
