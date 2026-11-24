// SPDX-License-Identifier: GPL-2.0
/*
 * Microbenchmark of the ring core in user space (no kernel involved).
 * Measures the cost of the data structure itself so device benchmark
 * results can be split into "ring work" vs "syscall + copy_*_user + wake".
 *
 *   bench_ring [--quick]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kmsgq_ring.h"

static int xfer_in(void *ctx, void *ring_ptr, size_t off, size_t len)
{
	memcpy(ring_ptr, (const uint8_t *)ctx + off, len);
	return 0;
}

static int xfer_out(void *ctx, void *ring_ptr, size_t off, size_t len)
{
	memcpy((uint8_t *)ctx + off, ring_ptr, len);
	return 0;
}

static double now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	static const size_t sizes[] = { 16, 64, 256, 1024, 4096 };
	const size_t cap = 64 * 1024;
	int quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
	long iters = quick ? 200000 : 5000000;
	uint8_t *storage = malloc(cap), in[4096], out[4096];
	struct kmsgq_ring r;
	volatile uint8_t sink = 0;

	if (!storage || kmsgq_ring_init(&r, storage, cap)) {
		fprintf(stderr, "init failed\n");
		return 1;
	}
	memset(in, 0xA5, sizeof(in));

	printf("%-8s %12s %12s %10s\n", "size", "pairs", "ns/pair", "GB/s");
	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		size_t len = sizes[s];
		/* Keep a backlog so head and tail travel and wrap. */
		size_t backlog = (cap / 2) / (len + KMSGQ_HDR_SIZE);
		double t0, dt;

		kmsgq_ring_reset(&r);
		for (size_t i = 0; i < backlog; i++)
			kmsgq_ring_push(&r, len, xfer_in, in);

		t0 = now_ns();
		for (long i = 0; i < iters; i++) {
			if (kmsgq_ring_push(&r, len, xfer_in, in) ||
			    kmsgq_ring_pop(&r, sizeof(out), xfer_out, out) !=
				    (long)len) {
				fprintf(stderr, "ring op failed\n");
				return 1;
			}
			sink ^= out[0];
		}
		dt = now_ns() - t0;
		printf("%-8zu %12ld %12.1f %10.2f\n", len, iters, dt / iters,
		       (double)len * iters / dt);
	}
	(void)sink;
	free(storage);
	return 0;
}
