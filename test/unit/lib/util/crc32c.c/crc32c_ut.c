/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"

#include "util/crc32.c"
#include "util/crc32c.c"
#include "util/crc32c_combine.c"

static void
test_crc32c(void)
{
	uint32_t crc;
	char buf[1024], buf1[1024];
	struct iovec iov[2] = {};

	/* Verify a string's CRC32-C value against the known correct result. */
	snprintf(buf, sizeof(buf), "%s", "Hello world!");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7b98e751);

	crc = 0xFFFFFFFFu;
	iov[0].iov_base = buf;
	iov[0].iov_len = strlen(buf);
	crc = spdk_crc32c_iov_update(iov, 1, crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7b98e751);

	crc = 0xFFFFFFFFu;
	snprintf(buf, sizeof(buf), "%s", "Hello");
	iov[0].iov_base = buf;
	iov[0].iov_len = strlen(buf);

	snprintf(buf1, sizeof(buf1), "%s", " world!");
	iov[1].iov_base = buf1;
	iov[1].iov_len = strlen(buf1);

	crc = spdk_crc32c_iov_update(iov, 2, crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7b98e751);

	/*
	 * The main loop of the optimized CRC32-C implementation processes data in 8-byte blocks,
	 * followed by a loop to handle the 0-7 trailing bytes.
	 * Test all buffer sizes from 0 to 7 in order to hit all possible trailing byte counts.
	 */

	/* 0-byte buffer should not modify CRC at all, so final result should be ~0 ^ ~0 == 0 */
	snprintf(buf, sizeof(buf), "%s", "");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0);

	/* 1-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "1");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x90F599E3);

	/* 2-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "12");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7355C460);

	/* 3-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "123");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x107B2FB2);

	/* 4-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "1234");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0xF63AF4EE);

	/* 5-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "12345");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x18D12335);

	/* 6-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "123456");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x41357186);

	/* 7-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "1234567");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x124297EA);

	/* Test a buffer of exactly 8 bytes (one block in the main CRC32-C loop). */
	snprintf(buf, sizeof(buf), "%s", "12345678");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x6087809A);
}

static void
test_crc32c_nvme(void)
{
	unsigned int buf_size = 4096;
	char buf[buf_size];
	uint32_t crc;
	unsigned int i, j;

	/* All the expected CRC values are compliant with
	* the NVM Command Set Specification 1.0c */

	/* Input buffer = 0s */
	memset(buf, 0, buf_size);
	crc = spdk_crc32c_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x98F94189);

	/* Input buffer = 1s */
	memset(buf, 0xFF, buf_size);
	crc = spdk_crc32c_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x25C1FE13);

	/* Input buffer = 0x00, 0x01, 0x02, ... */
	memset(buf, 0, buf_size);
	j = 0;
	for (i = 0; i < buf_size; i++) {
		buf[i] = (char)j;
		if (j == 0xFF) {
			j = 0;
		} else {
			j++;
		}
	}
	crc = spdk_crc32c_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x9C71FE32);

	/* Input buffer = 0xFF, 0xFE, 0xFD, ... */
	memset(buf, 0, buf_size);
	j = 0xFF;
	for (i = 0; i < buf_size ; i++) {
		buf[i] = (char)j;
		if (j == 0) {
			j = 0xFF;
		} else {
			j--;
		}
	}
	crc = spdk_crc32c_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x214941A8);
}

/* Compute the CRC of a deterministic buffer two ways -- linearly with
 * spdk_crc32c_update, and by chunking + spdk_crc32c_combine -- and assert
 * they match. Inputs use the raw convention (no init/final XOR), since
 * that's what spdk_crc32c_combine operates on. */
static void
combine_check(size_t chunk_size, size_t n_chunks, uint32_t seed)
{
	struct spdk_crc32c_combiner *combiner;
	uint8_t *buf;
	uint32_t *crcs;
	uint32_t reference, combined;
	size_t total = chunk_size * n_chunks;
	size_t i;

	buf = malloc(total);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	crcs = malloc(n_chunks * sizeof(*crcs));
	SPDK_CU_ASSERT_FATAL(crcs != NULL);

	/* CRC is linear over GF(2), so the data pattern is irrelevant for
	 * validating the combine math; we just want non-zero, varying data.
	 * Fill in 32-bit words for less awkwardness than the byte form. */
	SPDK_CU_ASSERT_FATAL(total % sizeof(uint32_t) == 0);
	for (i = 0; i < total / sizeof(uint32_t); i++) {
		((uint32_t *)buf)[i] = (uint32_t)i * 2654435761u + seed;
	}

	reference = spdk_crc32c_update(buf, total, 0);

	for (i = 0; i < n_chunks; i++) {
		crcs[i] = spdk_crc32c_update(buf + i * chunk_size, chunk_size, 0);
	}

	combiner = spdk_crc32c_combiner_create(chunk_size, n_chunks);
	SPDK_CU_ASSERT_FATAL(combiner != NULL);

	combined = spdk_crc32c_combine(combiner, crcs, n_chunks);
	CU_ASSERT(combined == reference);

	spdk_crc32c_combiner_free(combiner);
	free(crcs);
	free(buf);
}

static void
test_crc32c_combine(void)
{
	struct spdk_crc32c_combiner *combiner;
	uint32_t crc;

	/* Edge cases: 1 chunk, 2 chunks, odd chunk count (exercises the
	 * scalar tail in the PMULL/PCLMUL paths). */
	combine_check(4096, 1, 0);
	combine_check(4096, 2, 1);
	combine_check(512, 17, 2);

	/* The motivating workload: 64 chunks of 128 KiB. */
	combine_check(128 * 1024, 64, 3);

	/* Larger n_chunks to exercise both accumulator chains heavily. */
	combine_check(8192, 129, 4);

	/* n_crcs == 0 returns 0; create/free with various args. */
	combiner = spdk_crc32c_combiner_create(4096, 8);
	SPDK_CU_ASSERT_FATAL(combiner != NULL);
	crc = spdk_crc32c_combine(combiner, NULL, 0);
	CU_ASSERT(crc == 0);
	/* n_crcs == 1 returns crcs[0] verbatim. */
	{
		uint32_t one = 0xDEADBEEFu;
		crc = spdk_crc32c_combine(combiner, &one, 1);
		CU_ASSERT(crc == 0xDEADBEEFu);
	}
	spdk_crc32c_combiner_free(combiner);

	/* Invalid arguments return NULL rather than crashing. */
	CU_ASSERT(spdk_crc32c_combiner_create(0, 8) == NULL);
	CU_ASSERT(spdk_crc32c_combiner_create(4096, 0) == NULL);

	/* Free of NULL is a no-op. */
	spdk_crc32c_combiner_free(NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("crc32c", NULL, NULL);

	CU_ADD_TEST(suite, test_crc32c);
	CU_ADD_TEST(suite, test_crc32c_nvme);
	CU_ADD_TEST(suite, test_crc32c_combine);


	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
