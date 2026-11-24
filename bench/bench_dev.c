// SPDX-License-Identifier: GPL-2.0
/*
 * Device benchmarks for kmsgq (module must be loaded).
 *
 *   throughput  one producer thread, one consumer thread, blocking I/O on
 *               /dev/kmsgq0, for several message sizes
 *   latency     ping-pong: request on /dev/kmsgq0, reply on /dev/kmsgq1;
 *               reports round-trip percentiles (needs nr_devs >= 2)
 *
 *   bench_dev [--quick]
 *
 * Numbers depend heavily on the machine, kernel config (e.g. mitigations,
 * PREEMPT) and CPU placement; compare runs on the same host only.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "kmsgq.h"

static double now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int xopen(const char *path, int flags)
{
	int fd = open(path, flags | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		exit(77);
	}
	return fd;
}

struct tp_args {
	const char *path;
	size_t len;
	long count;
};

static void *tp_producer(void *arg)
{
	struct tp_args *a = arg;
	int fd = xopen(a->path, O_WRONLY);
	uint8_t *buf = calloc(1, a->len);

	for (long i = 0; buf && i < a->count; i++) {
		if (write(fd, buf, a->len) != (ssize_t)a->len) {
			perror("producer write");
			exit(1);
		}
	}
	free(buf);
	close(fd);
	return NULL;
}

static void throughput(const char *path, size_t max_msg, int quick)
{
	static const size_t sizes[] = { 16, 64, 256, 1024, 4096 };
	uint8_t *buf = malloc(max_msg);

	printf("\nthroughput (1 producer -> 1 consumer, blocking)\n");
	printf("%-8s %10s %14s %10s\n", "size", "msgs", "msgs/s", "MB/s");
	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		struct tp_args a = { path, sizes[s], quick ? 20000 : 500000 };
		int fd = xopen(path, O_RDONLY);
		pthread_t t;
		double t0, dt;

		if (a.len > max_msg)
			break;
		ioctl(fd, KMSGQ_IOC_FLUSH);
		t0 = now_ns();
		pthread_create(&t, NULL, tp_producer, &a);
		for (long i = 0; i < a.count; i++) {
			if (read(fd, buf, max_msg) != (ssize_t)a.len) {
				perror("consumer read");
				exit(1);
			}
		}
		pthread_join(t, NULL);
		dt = (now_ns() - t0) / 1e9;
		printf("%-8zu %10ld %14.0f %10.1f\n", a.len, a.count,
		       a.count / dt, a.len * a.count / dt / 1e6);
		close(fd);
	}
	free(buf);
}

struct echo_args {
	const char *req, *rep;
	long count;
};

static void *echo_thread(void *arg)
{
	struct echo_args *e = arg;
	int in = xopen(e->req, O_RDONLY), out = xopen(e->rep, O_WRONLY);
	uint8_t buf[64];

	for (long i = 0; i < e->count; i++) {
		ssize_t n = read(in, buf, sizeof(buf));

		if (n <= 0 || write(out, buf, (size_t)n) != n) {
			perror("echo");
			exit(1);
		}
	}
	close(in);
	close(out);
	return NULL;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void latency(const char *dev0, const char *dev1, int quick)
{
	struct echo_args e = { dev0, dev1, quick ? 5000 : 100000 };
	int req = xopen(dev0, O_WRONLY), rep;
	double *rtt = malloc(sizeof(double) * (size_t)e.count);
	uint8_t buf[64] = { 1 };
	pthread_t t;

	if (access(dev1, R_OK | W_OK) != 0) {
		printf("\nlatency: skipped (%s not present)\n", dev1);
		close(req);
		free(rtt);
		return;
	}
	rep = xopen(dev1, O_RDONLY);
	ioctl(req, KMSGQ_IOC_FLUSH);
	ioctl(rep, KMSGQ_IOC_FLUSH);
	pthread_create(&t, NULL, echo_thread, &e);
	for (long i = 0; i < e.count; i++) {
		double t0 = now_ns();

		if (write(req, buf, 32) != 32 || read(rep, buf, sizeof(buf)) != 32) {
			perror("ping");
			exit(1);
		}
		rtt[i] = (now_ns() - t0) / 1e3;
	}
	pthread_join(t, NULL);
	qsort(rtt, (size_t)e.count, sizeof(double), cmp_double);
	printf("\nlatency (32-byte ping-pong over two devices, %ld round trips)\n",
	       e.count);
	printf("p50 %.1f us   p90 %.1f us   p99 %.1f us   max %.1f us\n",
	       rtt[e.count / 2], rtt[e.count * 9 / 10], rtt[e.count * 99 / 100],
	       rtt[e.count - 1]);
	free(rtt);
	close(req);
	close(rep);
}

int main(int argc, char **argv)
{
	const char *prefix = getenv("KMSGQ_DEV_PREFIX");
	char dev0[128], dev1[128];
	int quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
	struct kmsgq_info info;
	int fd;

	snprintf(dev0, sizeof(dev0), "%s0", prefix ? prefix : "/dev/kmsgq");
	snprintf(dev1, sizeof(dev1), "%s1", prefix ? prefix : "/dev/kmsgq");
	fd = xopen(dev0, O_RDONLY);
	if (ioctl(fd, KMSGQ_IOC_GET_INFO, &info)) {
		perror("KMSGQ_IOC_GET_INFO");
		return 1;
	}
	close(fd);
	printf("kmsgq bench: capacity %llu B, max_msg %llu B%s\n",
	       (unsigned long long)info.capacity,
	       (unsigned long long)info.max_msg, quick ? " (quick)" : "");

	throughput(dev0, info.max_msg, quick);
	latency(dev0, dev1, quick);
	return 0;
}
