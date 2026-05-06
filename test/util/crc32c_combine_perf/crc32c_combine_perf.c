/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/crc32.h"

#define DEFAULT_CHUNK_SIZE	(128u * 1024u)
#define DEFAULT_NUM_CHUNKS	64u
#define DEFAULT_MIN_NS		(500ull * 1000ull * 1000ull)	/* 500 ms per data point */
#define DEFAULT_BATCH		4096

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c <bytes>     chunk size (default %u)\n"
		"  -n <count>     number of chunks per combine (default %u)\n"
		"  -t <ms>        minimum measurement window in ms (default %llu)\n"
		"  -i <count>     iterations per timing batch (default %u)\n"
		"  -h             this help\n",
		prog, DEFAULT_CHUNK_SIZE, DEFAULT_NUM_CHUNKS,
		DEFAULT_MIN_NS / 1000000ull, DEFAULT_BATCH);
}

static uint64_t
ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t
read_cpu_freq_ghz_from_sysfs(double *ghz)
{
	FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
	unsigned long khz;

	if (f == NULL) {
		return 0;
	}
	if (fscanf(f, "%lu", &khz) != 1) {
		fclose(f);
		return 0;
	}
	fclose(f);
	*ghz = (double)khz / 1000000.0;
	return 1;
}

int
main(int argc, char **argv)
{
	size_t chunk_size = DEFAULT_CHUNK_SIZE;
	size_t n_chunks = DEFAULT_NUM_CHUNKS;
	uint64_t min_ns = DEFAULT_MIN_NS;
	unsigned batch = DEFAULT_BATCH;
	int opt;
	struct spdk_crc32c_combiner *combiner;
	uint32_t *crcs;
	volatile uint32_t sink = 0;
	uint64_t t0, t1;
	uint64_t total_iters = 0;
	uint64_t elapsed_ns = 0;
	double ns_per_combine, ns_per_chunk, gbps;
	double cpu_ghz = 0.0;
	size_t i;

	while ((opt = getopt(argc, argv, "c:n:t:i:h")) != -1) {
		switch (opt) {
		case 'c':
			chunk_size = (size_t)strtoull(optarg, NULL, 0);
			break;
		case 'n':
			n_chunks = (size_t)strtoull(optarg, NULL, 0);
			break;
		case 't':
			min_ns = strtoull(optarg, NULL, 0) * 1000000ull;
			break;
		case 'i':
			batch = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (chunk_size == 0 || n_chunks == 0 || batch == 0) {
		fprintf(stderr, "chunk_size, n_chunks, and batch must all be > 0\n");
		return 1;
	}

	combiner = spdk_crc32c_combiner_create(chunk_size, n_chunks);
	if (combiner == NULL) {
		fprintf(stderr, "spdk_crc32c_combiner_create(%zu, %zu) failed\n",
			chunk_size, n_chunks);
		return 1;
	}

	crcs = malloc(n_chunks * sizeof(*crcs));
	if (crcs == NULL) {
		fprintf(stderr, "malloc failed\n");
		spdk_crc32c_combiner_free(combiner);
		return 1;
	}
	/* Deterministic non-zero CRCs so the combine math actually moves bits. */
	for (i = 0; i < n_chunks; i++) {
		crcs[i] = (uint32_t)(i * 2654435761u + 0xC0FFEEu);
	}

	read_cpu_freq_ghz_from_sysfs(&cpu_ghz);

	/* Warm up: a few thousand combines to get caches and predictors hot. */
	for (i = 0; i < 4096; i++) {
		sink ^= spdk_crc32c_combine(combiner, crcs, n_chunks);
	}

	/* Measurement loop: run `batch` combines, take a timestamp at each
	 * end, accumulate until we exceed min_ns. */
	t0 = ns_now();
	while (elapsed_ns < min_ns) {
		uint64_t b;
		for (b = 0; b < batch; b++) {
			sink ^= spdk_crc32c_combine(combiner, crcs, n_chunks);
		}
		total_iters += batch;
		t1 = ns_now();
		elapsed_ns = t1 - t0;
	}

	ns_per_combine = (double)elapsed_ns / (double)total_iters;
	ns_per_chunk = ns_per_combine / (double)n_chunks;
	/* Logical line rate the combine could keep up with: total bits per
	 * combine divided by wall time. Reported in Gbps (10^9 bits/s) to
	 * match network line-rate convention (100/200/400/800 Gbps etc.). */
	gbps = ((double)chunk_size * (double)n_chunks * 8.0) / ns_per_combine;

	printf("crc32c_combine perf\n");
	printf("  chunk size       : %zu bytes\n", chunk_size);
	printf("  chunks / combine : %zu\n", n_chunks);
	printf("  combines run     : %llu\n", (unsigned long long)total_iters);
	printf("  elapsed          : %.3f ms\n", (double)elapsed_ns / 1.0e6);
	printf("  ns / combine     : %.2f\n", ns_per_combine);
	printf("  ns / chunk       : %.3f\n", ns_per_chunk);
	if (cpu_ghz > 0.0) {
		printf("  cpu max freq     : %.3f GHz (from sysfs)\n", cpu_ghz);
		printf("  cycles / combine : %.1f (at max freq)\n",
		       ns_per_combine * cpu_ghz);
		printf("  cycles / chunk   : %.2f (at max freq)\n",
		       ns_per_chunk * cpu_ghz);
	}
	printf("  line rate kept   : %.1f Gbps\n", gbps);
	printf("  sink             : 0x%08x (prevents DCE)\n", sink);

	free(crcs);
	spdk_crc32c_combiner_free(combiner);
	return 0;
}
