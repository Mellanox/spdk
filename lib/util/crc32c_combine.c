/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "crc_internal.h"
#include "spdk/crc32.h"
#include "spdk/stdinc.h"

/*
 * CRC-32C (Castagnoli) polynomial in non-reflected form, including the
 * implicit x^32 term. SPDK's spdk_crc32c_update() returns reflected CRCs
 * (the convention used by ARMv8 __crc32cx and SSE4.2 _mm_crc32_*); the
 * combine math below works in non-reflected form and converts at the
 * boundaries, which keeps the constants and reduction step easy to verify.
 *
 * P(x) = x^32 + x^28 + x^27 + x^26 + x^25 + x^23 + x^22 + x^20 + x^19
 *      + x^18 + x^14 + x^13 + x^11 + x^10 + x^9 + x^8 + x^6 + 1
 */
#define SPDK_CRC32C_POLY_NR	0x11EDC6F41ULL

/*
 * Barrett reduction constant mu = floor(x^64 / P), 33 bits with bit 32 set.
 * Pure function of the polynomial above; precomputed once via long division
 * so combiner_create() does not have to repeat ~65 loop iterations.
 *
 * If SPDK_CRC32C_POLY_NR ever changes, the combine unit tests will catch a
 * stale value here -- a wrong mu corrupts every multi-chunk combine.
 */
#define SPDK_CRC32C_BARRETT_MU	0x11F91CAF6ULL

struct spdk_crc32c_combiner {
	size_t		chunk_size;
	size_t		max_chunks;
	/* Barrett reduction constant: floor(x^64 / P), 33 bits with bit 32 set. */
	uint64_t	mu;
	/* Powers of K = x^(8 * chunk_size) mod P, in non-reflected form.
	 * K_pow[i] is K^i; chunk crcs[j] in a combine call with n_crcs entries
	 * is multiplied by K_pow[n_crcs - 1 - j]. */
	uint64_t	K_pow[];
};

/*
 * --- Slow software helpers, used only at init time. ---
 */

/* Polynomial multiplication of two 32-bit operands mod P, non-reflected. */
static uint32_t
gf2_mul_nr(uint32_t a, uint32_t b)
{
	uint64_t prod = 0;
	int i;

	for (i = 0; i < 32; i++) {
		if ((b >> i) & 1u) {
			prod ^= (uint64_t)a << i;
		}
	}
	for (i = 63; i >= 32; i--) {
		if ((prod >> i) & 1ull) {
			prod ^= SPDK_CRC32C_POLY_NR << (i - 32);
		}
	}
	return (uint32_t)prod;
}

/* Compute x^n mod P, non-reflected, via fast exponentiation. */
static uint32_t
gf2_xpow_nr(uint64_t n)
{
	uint32_t r = 1u;        /* x^0 */
	uint32_t base = 0x2u;   /* x^1 */

	while (n) {
		if (n & 1ull) {
			r = gf2_mul_nr(r, base);
		}
		base = gf2_mul_nr(base, base);
		n >>= 1;
	}
	return r;
}

/*
 * Reverse the bits of a 32-bit value. Converts between SPDK's reflected
 * CRC representation (matching __crc32cx / _mm_crc32_*) and the
 * non-reflected polynomial representation that the combine math uses.
 */
static inline uint32_t
crc32c_bitrev32(uint32_t x)
{
#ifdef SPDK_HAVE_ARM_PMULL
	uint32_t r;
	__asm__("rbit %w0, %w1" : "=r"(r) : "r"(x));
	return r;
#else
	x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
	x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
	x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
	return __builtin_bswap32(x);
#endif
}

/*
 * --- Hot path: per-architecture combine implementations. ---
 *
 * All variants compute, in non-reflected GF(2)[x] mod P:
 *
 *   result = sum_{i=0}^{n-1} crc[i] * K^(n-1-i)   mod P
 *
 * where K = x^(8 * chunk_size). Inputs and the result are bit-reversed
 * once on the way in/out so callers see SPDK's reflected convention.
 */

#if defined(SPDK_HAVE_ARM_PMULL)

static uint32_t
crc32c_combine_impl(const struct spdk_crc32c_combiner *c,
		    const uint32_t *crcs, size_t n_crcs)
{
	uint64x2_t acc0 = vdupq_n_u64(0);
	uint64x2_t acc1 = vdupq_n_u64(0);
	size_t i;

	/* Two independent accumulator chains keep the PMULL pipeline full
	 * despite the multi-cycle latency of vmull_p64. */
	for (i = 0; i + 2 <= n_crcs; i += 2) {
		uint32_t x0 = crc32c_bitrev32(crcs[i]);
		uint32_t x1 = crc32c_bitrev32(crcs[i + 1]);
		poly128_t p0 = vmull_p64((poly64_t)(uint64_t)x0,
					 (poly64_t)c->K_pow[n_crcs - 1 - i]);
		poly128_t p1 = vmull_p64((poly64_t)(uint64_t)x1,
					 (poly64_t)c->K_pow[n_crcs - 2 - i]);
		acc0 = veorq_u64(acc0, vreinterpretq_u64_p128(p0));
		acc1 = veorq_u64(acc1, vreinterpretq_u64_p128(p1));
	}
	if (i < n_crcs) {
		uint32_t x = crc32c_bitrev32(crcs[i]);
		poly128_t p = vmull_p64((poly64_t)(uint64_t)x,
					(poly64_t)c->K_pow[n_crcs - 1 - i]);
		acc0 = veorq_u64(acc0, vreinterpretq_u64_p128(p));
	}

	/* Each PMULL of two 32-bit operands produces a <=63-bit polynomial in
	 * the low 64 bits of the 128-bit lane; the high 64 bits stay zero. */
	uint64_t T = vgetq_lane_u64(acc0, 0) ^ vgetq_lane_u64(acc1, 0);

	/* Barrett reduction: T (degree < 64) mod P (degree 32).
	 *   q = (T_hi * mu) >> 32          [first PMULL]
	 *   r = T XOR (q * P)              [second PMULL + XOR, low 32 bits] */
	uint64_t T_hi = T >> 32;
	poly128_t t1 = vmull_p64((poly64_t)T_hi, (poly64_t)c->mu);
	uint64_t q = vgetq_lane_u64(vreinterpretq_u64_p128(t1), 0) >> 32;
	poly128_t t2 = vmull_p64((poly64_t)q, (poly64_t)SPDK_CRC32C_POLY_NR);
	uint32_t reduced = (uint32_t)(T ^ vgetq_lane_u64(vreinterpretq_u64_p128(t2), 0));

	return crc32c_bitrev32(reduced);
}

#elif defined(SPDK_HAVE_X86_PCLMUL)

static uint32_t
crc32c_combine_impl(const struct spdk_crc32c_combiner *c,
		    const uint32_t *crcs, size_t n_crcs)
{
	__m128i acc0 = _mm_setzero_si128();
	__m128i acc1 = _mm_setzero_si128();
	size_t i;

	for (i = 0; i + 2 <= n_crcs; i += 2) {
		uint32_t x0 = crc32c_bitrev32(crcs[i]);
		uint32_t x1 = crc32c_bitrev32(crcs[i + 1]);
		__m128i p0 = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)(uint64_t)x0),
						  _mm_cvtsi64_si128((int64_t)c->K_pow[n_crcs - 1 - i]),
						  0x00);
		__m128i p1 = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)(uint64_t)x1),
						  _mm_cvtsi64_si128((int64_t)c->K_pow[n_crcs - 2 - i]),
						  0x00);
		acc0 = _mm_xor_si128(acc0, p0);
		acc1 = _mm_xor_si128(acc1, p1);
	}
	if (i < n_crcs) {
		uint32_t x = crc32c_bitrev32(crcs[i]);
		__m128i p = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)(uint64_t)x),
						 _mm_cvtsi64_si128((int64_t)c->K_pow[n_crcs - 1 - i]),
						 0x00);
		acc0 = _mm_xor_si128(acc0, p);
	}

	__m128i acc = _mm_xor_si128(acc0, acc1);
	uint64_t T = (uint64_t)_mm_cvtsi128_si64(acc);

	uint64_t T_hi = T >> 32;
	__m128i t1 = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)T_hi),
					  _mm_cvtsi64_si128((int64_t)c->mu),
					  0x00);
	uint64_t q = (uint64_t)_mm_cvtsi128_si64(t1) >> 32;
	__m128i t2 = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)q),
					  _mm_cvtsi64_si128((int64_t)SPDK_CRC32C_POLY_NR),
					  0x00);
	uint32_t reduced = (uint32_t)(T ^ (uint64_t)_mm_cvtsi128_si64(t2));

	return crc32c_bitrev32(reduced);
}

#else /* No PMULL/PCLMUL: portable software fallback. */

static uint32_t
crc32c_combine_impl(const struct spdk_crc32c_combiner *c,
		    const uint32_t *crcs, size_t n_crcs)
{
	uint32_t acc = 0;
	size_t i;

	for (i = 0; i < n_crcs; i++) {
		uint32_t x = crc32c_bitrev32(crcs[i]);
		uint32_t k = (uint32_t)c->K_pow[n_crcs - 1 - i];
		acc ^= gf2_mul_nr(x, k);
	}
	return crc32c_bitrev32(acc);
}

#endif

struct spdk_crc32c_combiner *
spdk_crc32c_combiner_create(size_t chunk_size, size_t max_chunks)
{
	struct spdk_crc32c_combiner *c;
	uint32_t K_step;
	uint32_t k;
	size_t i;

	if (chunk_size == 0 || max_chunks == 0) {
		return NULL;
	}
	if (max_chunks > (SIZE_MAX - sizeof(*c)) / sizeof(uint64_t)) {
		return NULL;
	}

	c = calloc(1, sizeof(*c) + max_chunks * sizeof(uint64_t));
	if (c == NULL) {
		return NULL;
	}

	c->chunk_size = chunk_size;
	c->max_chunks = max_chunks;
	c->mu = SPDK_CRC32C_BARRETT_MU;

	/* K_step = x^(8 * chunk_size) mod P */
	K_step = gf2_xpow_nr(8ull * chunk_size);

	/* K_pow[i] = K_step^i, computed iteratively (one multiply per entry). */
	k = 1u;
	for (i = 0; i < max_chunks; i++) {
		c->K_pow[i] = (uint64_t)k;
		k = gf2_mul_nr(k, K_step);
	}

	return c;
}

void
spdk_crc32c_combiner_free(struct spdk_crc32c_combiner *combiner)
{
	free(combiner);
}

uint32_t
spdk_crc32c_combine(const struct spdk_crc32c_combiner *combiner,
		    const uint32_t *crcs, size_t n_crcs)
{
	assert(combiner != NULL);
	assert(n_crcs <= combiner->max_chunks);
	assert(n_crcs == 0 || crcs != NULL);

	if (n_crcs == 0) {
		return 0;
	}
	if (n_crcs == 1) {
		return crcs[0];
	}
	return crc32c_combine_impl(combiner, crcs, n_crcs);
}
