// SPDX-License-Identifier: GPL-2.0
/*
 * Integration tests for the kmsgq module. Requires the module to be loaded
 * with at least one device (two for the independence test) and permission
 * to open /dev/kmsgq*. Normally run through scripts/run-integration.sh.
 *
 *   KMSGQ_DEV_PREFIX   node prefix (default /dev/kmsgq)
 *   KMSGQ_STRESS_N     messages per producer in stress tests (default 20000)
 *
 * Worker threads never call KT_* (which longjmp); they record failures in
 * their context and the main thread asserts after joining.
 */
#define _GNU_SOURCE
#define KT_IMPLEMENTATION
#include "kt.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "kmsgq.h"

/* ---- helpers -------------------------------------------------------- */

static const char *dev_path(int idx)
{
	static char path[2][128];
	const char *prefix = getenv("KMSGQ_DEV_PREFIX");

	snprintf(path[idx & 1], sizeof(path[0]), "%s%d",
		 prefix ? prefix : "/dev/kmsgq", idx);
	return path[idx & 1];
}

/* Open a device without touching its state (safe from worker threads). */
static int open_raw(int idx, int flags)
{
	return open(dev_path(idx), flags | O_CLOEXEC);
}

/* Open a device and reset it so every test starts from an empty queue. */
static int open_clean(int idx, int flags)
{
	int fd = open_raw(idx, flags);

	if (fd < 0)
		KT_FAIL("open %s: %s", dev_path(idx), strerror(errno));
	KT_OK(ioctl(fd, KMSGQ_IOC_FLUSH));
	KT_OK(ioctl(fd, KMSGQ_IOC_RESET_STATS));
	return fd;
}

static struct kmsgq_info info_of(int fd)
{
	struct kmsgq_info info;

	KT_OK(ioctl(fd, KMSGQ_IOC_GET_INFO, &info));
	return info;
}

static struct kmsgq_stats stats_of(int fd)
{
	struct kmsgq_stats st;

	KT_OK(ioctl(fd, KMSGQ_IOC_GET_STATS, &st));
	return st;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

static short revents(int fd, short events, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = events };
	int n = poll(&p, 1, timeout_ms);

	if (n < 0)
		KT_FAIL("poll: %s", strerror(errno));
	return n ? p.revents : 0;
}

/* Fill the queue with non-blocking writes of @len bytes; return count. */
static long fill(int nbfd, size_t len)
{
	static unsigned char buf[1 << 16];
	long n = 0;

	if (len > sizeof(buf))
		KT_FAIL("fill: len %zu exceeds test buffer", len);
	memset(buf, 'f', len);
	while (write(nbfd, buf, len) == (ssize_t)len)
		n++;
	if (errno != EAGAIN)
		KT_FAIL("fill: expected EAGAIN, got %s", strerror(errno));
	return n;
}

/* Thread error reporting. */
struct terr {
	int failed;
	char msg[256];
};

__attribute__((format(printf, 2, 3)))
static void terr_set(struct terr *e, const char *fmt, ...)
{
	va_list ap;

	if (e->failed++)
		return;
	va_start(ap, fmt);
	vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
	va_end(ap);
}

#define KT_THREAD_OK(e)							\
	do {								\
		if ((e)->failed)					\
			KT_FAIL("worker thread: %s", (e)->msg);		\
	} while (0)

/* ---- basic semantics ------------------------------------------------ */

KT_TEST(info_reports_config)
{
	int fd = open_clean(0, O_RDWR);
	struct kmsgq_info info = info_of(fd);

	KT_EQ(info.api_version, KMSGQ_API_VERSION);
	KT_EQ(info.minor, 0);
	KT_EQ(info.hdr_size, 4);
	KT_ASSERT(info.max_msg >= 1);
	KT_ASSERT(info.capacity >= info.max_msg + info.hdr_size);
	KT_EQ(info.used_bytes, 0);
	KT_EQ(info.msg_count, 0);
	close(fd);
}

KT_TEST(message_boundaries_preserved)
{
	int fd = open_clean(0, O_RDWR);
	char buf[64];

	KT_EQ(write(fd, "a", 1), 1);
	KT_EQ(write(fd, "bb", 2), 2);
	KT_EQ(write(fd, "ccc", 3), 3);
	KT_EQ(info_of(fd).msg_count, 3);
	KT_EQ(read(fd, buf, sizeof(buf)), 1);
	KT_ASSERT(memcmp(buf, "a", 1) == 0);
	KT_EQ(read(fd, buf, sizeof(buf)), 2);
	KT_ASSERT(memcmp(buf, "bb", 2) == 0);
	KT_EQ(read(fd, buf, sizeof(buf)), 3);
	KT_ASSERT(memcmp(buf, "ccc", 3) == 0);
	KT_EQ(info_of(fd).used_bytes, 0);
	close(fd);
}

KT_TEST(short_read_buffer_gets_emsgsize)
{
	int fd = open_clean(0, O_RDWR);
	char buf[16];
	__u32 len = 0;

	KT_EQ(write(fd, "0123456789", 10), 10);
	KT_ERRNO(read(fd, buf, 9), EMSGSIZE);
	KT_OK(ioctl(fd, KMSGQ_IOC_PEEK_LEN, &len));
	KT_EQ(len, 10);
	KT_EQ(read(fd, buf, 10), 10);
	KT_ASSERT(memcmp(buf, "0123456789", 10) == 0);
	KT_OK(ioctl(fd, KMSGQ_IOC_PEEK_LEN, &len));
	KT_EQ(len, 0);
	close(fd);
}

KT_TEST(oversize_write_rejected)
{
	int fd = open_clean(0, O_RDWR);
	struct kmsgq_info info = info_of(fd);
	char *buf = calloc(1, info.max_msg + 1);

	KT_ASSERT(buf);
	KT_ERRNO(write(fd, buf, info.max_msg + 1), EMSGSIZE);
	KT_EQ(info_of(fd).msg_count, 0);
	KT_EQ(write(fd, buf, info.max_msg), (ssize_t)info.max_msg);
	KT_EQ(read(fd, buf, info.max_msg), (ssize_t)info.max_msg);
	free(buf);
	close(fd);
}

KT_TEST(zero_length_io_is_noop)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	char buf[4];

	KT_EQ(write(fd, buf, 0), 0);
	KT_EQ(read(fd, buf, 0), 0);
	KT_EQ(info_of(fd).msg_count, 0);
	close(fd);
}

KT_TEST(lseek_rejected)
{
	int fd = open_clean(0, O_RDWR);

	KT_ERRNO(lseek(fd, 0, SEEK_SET), ESPIPE);
	close(fd);
}

KT_TEST(unknown_ioctl_is_enotty)
{
	int fd = open_clean(0, O_RDWR);

	KT_ERRNO(ioctl(fd, _IO(KMSGQ_IOC_MAGIC, 0x7f)), ENOTTY);
	close(fd);
}

KT_TEST(bad_user_pointer_is_efault_and_atomic)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	void *bad = (void *)(uintptr_t)8;	/* never mapped */
	__u32 len = 0;
	char buf[16];

	/* Raw syscalls: libc fortify would otherwise reject the pointer. */
	KT_ERRNO(syscall(SYS_write, fd, bad, 10), EFAULT);
	KT_EQ(info_of(fd).msg_count, 0);

	KT_EQ(write(fd, "keep-me", 7), 7);
	KT_ERRNO(syscall(SYS_read, fd, bad, sizeof(buf)), EFAULT);
	/* The failed read must not have consumed the message. */
	KT_OK(ioctl(fd, KMSGQ_IOC_PEEK_LEN, &len));
	KT_EQ(len, 7);
	KT_EQ(read(fd, buf, sizeof(buf)), 7);
	KT_ASSERT(memcmp(buf, "keep-me", 7) == 0);

	KT_ERRNO(syscall(SYS_ioctl, fd, KMSGQ_IOC_GET_INFO, bad), EFAULT);
	close(fd);
}

/* ---- non-blocking I/O and poll -------------------------------------- */

KT_TEST(nonblock_read_on_empty_is_eagain)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	char buf[8];

	KT_ERRNO(read(fd, buf, sizeof(buf)), EAGAIN);
	close(fd);
}

KT_TEST(nonblock_fill_matches_capacity)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	struct kmsgq_info info = info_of(fd);
	size_t len = info.max_msg < 100 ? info.max_msg : 100;
	long expect = (long)(info.capacity / (len + info.hdr_size));
	long n = fill(fd, len);

	KT_EQ(n, expect);
	info = info_of(fd);
	KT_EQ(info.msg_count, n);
	KT_EQ(info.used_bytes, (unsigned long long)n * (len + info.hdr_size));
	KT_EQ(stats_of(fd).msgs_written, n);
	KT_EQ(stats_of(fd).high_watermark, info.used_bytes);
	close(fd);
}

KT_TEST(poll_reflects_queue_state)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	struct kmsgq_info info = info_of(fd);
	char *buf = malloc(info.max_msg);

	KT_ASSERT(buf);
	KT_EQ(revents(fd, POLLIN | POLLOUT, 0), POLLOUT);

	KT_EQ(write(fd, "x", 1), 1);
	KT_EQ(revents(fd, POLLIN | POLLOUT, 0), POLLIN | POLLOUT);

	/* Fill with max-size messages: afterwards one more cannot fit. */
	fill(fd, info.max_msg);
	KT_EQ(revents(fd, POLLIN | POLLOUT, 0), POLLIN);

	/* Draining the small message plus one big one frees enough room. */
	KT_EQ(read(fd, buf, info.max_msg), 1);
	KT_EQ(read(fd, buf, info.max_msg), (ssize_t)info.max_msg);
	KT_ASSERT(revents(fd, POLLOUT, 0) & POLLOUT);
	free(buf);
	close(fd);
}

KT_TEST(poll_times_out_when_empty)
{
	int fd = open_clean(0, O_RDWR);
	double t0 = now_ms();

	KT_EQ(revents(fd, POLLIN, 100), 0);
	KT_ASSERT(now_ms() - t0 >= 90);
	close(fd);
}

/* ---- blocking I/O and wake-ups -------------------------------------- */

struct delayed_io {
	int dev;
	int delay_ms;
	int nwrites;
	struct terr err;
};

static void *delayed_writer(void *arg)
{
	struct delayed_io *d = arg;
	int fd = open_raw(d->dev, O_WRONLY);

	if (fd < 0) {
		terr_set(&d->err, "open: %s", strerror(errno));
		return NULL;
	}
	sleep_ms(d->delay_ms);
	for (int i = 0; i < (d->nwrites ? d->nwrites : 1); i++)
		if (write(fd, "ping", 4) != 4)
			terr_set(&d->err, "write: %s", strerror(errno));
	close(fd);
	return NULL;
}

KT_TEST(blocking_read_wakes_on_write)
{
	int fd = open_clean(0, O_RDWR);
	struct delayed_io d = { .dev = 0, .delay_ms = 150 };
	pthread_t t;
	char buf[8];
	double t0 = now_ms();

	KT_EQ(pthread_create(&t, NULL, delayed_writer, &d), 0);
	ssize_t n = read(fd, buf, sizeof(buf));
	double waited = now_ms() - t0;

	pthread_join(t, NULL);
	KT_THREAD_OK(&d.err);
	KT_EQ(n, 4);
	KT_ASSERT(memcmp(buf, "ping", 4) == 0);
	KT_ASSERT(waited >= 100);
	KT_ASSERT(stats_of(fd).reads_blocked >= 1);
	close(fd);
}

struct blocked_writer {
	int dev;
	size_t len;
	volatile int done;
	ssize_t ret;
	int err;
};

static void *blocking_write_thread(void *arg)
{
	struct blocked_writer *w = arg;
	char *buf = calloc(1, w->len);
	int fd = open_raw(w->dev, O_WRONLY);

	w->ret = (fd < 0 || !buf) ? -1 : write(fd, buf, w->len);
	w->err = errno;
	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
	if (fd >= 0)
		close(fd);
	free(buf);
	return NULL;
}

KT_TEST(blocking_write_wakes_on_read)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	struct kmsgq_info info = info_of(fd);
	struct blocked_writer w = { .dev = 0, .len = info.max_msg };
	char *buf = malloc(info.max_msg);
	pthread_t t;

	KT_ASSERT(buf);
	fill(fd, info.max_msg);
	KT_EQ(pthread_create(&t, NULL, blocking_write_thread, &w), 0);
	sleep_ms(150);
	KT_ASSERT(!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE));

	KT_EQ(read(fd, buf, info.max_msg), (ssize_t)info.max_msg);
	pthread_join(t, NULL);
	KT_EQ(w.ret, (ssize_t)info.max_msg);
	KT_ASSERT(stats_of(fd).writes_blocked >= 1);
	free(buf);
	close(fd);
}

KT_TEST(epoll_reports_readable)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN | EPOLLET };
	struct delayed_io d = { .dev = 0, .delay_ms = 50, .nwrites = 1 };
	pthread_t t;
	char buf[8];

	KT_ASSERT(ep >= 0);
	KT_OK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev));
	KT_EQ(epoll_wait(ep, &ev, 1, 0), 0);

	KT_EQ(pthread_create(&t, NULL, delayed_writer, &d), 0);
	int n = epoll_wait(ep, &ev, 1, 5000);

	pthread_join(t, NULL);
	KT_THREAD_OK(&d.err);
	KT_EQ(n, 1);
	KT_ASSERT(ev.events & EPOLLIN);
	KT_EQ(read(fd, buf, sizeof(buf)), 4);
	close(ep);
	close(fd);
}

static void on_alarm(int sig)
{
	(void)sig;
}

KT_TEST(signal_interrupts_blocking_read)
{
	int fd = open_clean(0, O_RDWR);
	struct sigaction sa = { .sa_handler = on_alarm }, old;	/* no SA_RESTART */
	struct itimerval it = { .it_value = { 0, 100000 } };
	char buf[8];

	sigemptyset(&sa.sa_mask);
	KT_OK(sigaction(SIGALRM, &sa, &old));
	KT_OK(setitimer(ITIMER_REAL, &it, NULL));
	errno = 0;
	ssize_t n = read(fd, buf, sizeof(buf));
	int e = errno;

	sigaction(SIGALRM, &old, NULL);
	KT_EQ(n, -1);
	KT_EQ(e, EINTR);
	close(fd);
}

/* ---- ioctls --------------------------------------------------------- */

KT_TEST(flush_discards_and_counts)
{
	int fd = open_clean(0, O_RDWR | O_NONBLOCK);
	struct kmsgq_stats st;
	char buf[8];

	for (int i = 0; i < 5; i++)
		KT_EQ(write(fd, "hello", 5), 5);
	KT_OK(ioctl(fd, KMSGQ_IOC_FLUSH));
	KT_ERRNO(read(fd, buf, sizeof(buf)), EAGAIN);
	st = stats_of(fd);
	KT_EQ(st.msgs_flushed, 5);
	KT_EQ(st.flushes, 1);
	KT_EQ(info_of(fd).used_bytes, 0);
	close(fd);
}

KT_TEST(stats_count_traffic)
{
	int fd = open_clean(0, O_RDWR);
	struct kmsgq_stats st;
	char buf[16];

	KT_EQ(write(fd, "abc", 3), 3);
	KT_EQ(write(fd, "defgh", 5), 5);
	KT_EQ(write(fd, "ij", 2), 2);
	KT_EQ(read(fd, buf, sizeof(buf)), 3);
	KT_EQ(read(fd, buf, sizeof(buf)), 5);
	st = stats_of(fd);
	KT_EQ(st.msgs_written, 3);
	KT_EQ(st.bytes_written, 10);
	KT_EQ(st.msgs_read, 2);
	KT_EQ(st.bytes_read, 8);
	KT_EQ(st.high_watermark, 10 + 3 * 4);

	KT_OK(ioctl(fd, KMSGQ_IOC_RESET_STATS));
	st = stats_of(fd);
	KT_EQ(st.msgs_written, 0);
	KT_EQ(st.high_watermark, 2 + 4);	/* reset to current fill */
	close(fd);
}

KT_TEST(devices_are_independent)
{
	int a = open_clean(0, O_RDWR | O_NONBLOCK);
	int b = open_raw(1, O_RDWR | O_NONBLOCK);
	char buf[8];

	if (b < 0) {
		close(a);
		KT_SKIP("%s not present", dev_path(1));
	}
	KT_OK(ioctl(b, KMSGQ_IOC_FLUSH));
	KT_EQ(info_of(b).minor, 1);
	KT_EQ(write(a, "only-a", 6), 6);
	KT_ERRNO(read(b, buf, sizeof(buf)), EAGAIN);
	KT_EQ(info_of(a).msg_count, 1);
	close(a);
	close(b);
}

/* ---- concurrency stress -------------------------------------------- */

#define SMSG_MAGIC 0x6b6d7371u	/* "kmsq" */
#define POISON_PRODUCER 0xffffffffu

struct smsg {
	uint32_t magic;
	uint32_t producer;
	uint32_t seq;
	uint32_t len;	/* total message length */
	uint32_t sum;
	uint8_t payload[];
};

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;

	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

static size_t smsg_build(uint8_t *buf, size_t max, uint32_t prod,
			 uint32_t seq)
{
	struct smsg *m = (struct smsg *)buf;
	size_t span = max - sizeof(*m);
	size_t plen = span ? (seq * 2654435761u + prod * 97u) % span : 0;

	m->magic = SMSG_MAGIC;
	m->producer = prod;
	m->seq = seq;
	m->len = (uint32_t)(sizeof(*m) + plen);
	for (size_t i = 0; i < plen; i++)
		m->payload[i] = (uint8_t)(prod * 31u + seq + i);
	m->sum = fnv1a(m->payload, plen);
	return m->len;
}

/* Validate a received message; returns NULL or a description of the fault. */
static const char *smsg_check(const uint8_t *buf, ssize_t n)
{
	const struct smsg *m = (const struct smsg *)buf;

	if (n < (ssize_t)sizeof(*m))
		return "short message";
	if (m->magic != SMSG_MAGIC)
		return "bad magic";
	if (m->len != (uint32_t)n)
		return "length mismatch";
	if (fnv1a(m->payload, n - sizeof(*m)) != m->sum)
		return "checksum mismatch";
	return NULL;
}

struct stress {
	int dev;
	uint32_t nprod, ncons, n;
	size_t max;
	uint8_t *seen;			/* [nprod * n] receive counters */
	uint32_t *next_seq;		/* per-producer, single-consumer mode */
	int ordered;			/* single consumer: check FIFO order */
	struct terr err;
	pthread_mutex_t err_lock;
};

struct worker {
	struct stress *s;
	uint32_t id;
};

static void stress_fail(struct stress *s, const char *what, uint32_t a,
			uint32_t b)
{
	pthread_mutex_lock(&s->err_lock);
	terr_set(&s->err, "%s (%u, %u)", what, a, b);
	pthread_mutex_unlock(&s->err_lock);
}

static void *producer(void *arg)
{
	struct worker *w = arg;
	struct stress *s = w->s;
	uint8_t *buf = malloc(s->max);
	int fd = open_raw(s->dev, O_WRONLY);

	if (fd < 0 || !buf) {
		stress_fail(s, "producer setup", w->id, 0);
		goto out;
	}
	for (uint32_t seq = 0; seq < s->n; seq++) {
		size_t len = smsg_build(buf, s->max, w->id, seq);

		if (write(fd, buf, len) != (ssize_t)len) {
			stress_fail(s, "producer write", w->id, seq);
			break;
		}
	}
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return NULL;
}

static void *consumer(void *arg)
{
	struct worker *w = arg;
	struct stress *s = w->s;
	uint8_t *buf = malloc(s->max);
	int fd = open_raw(s->dev, O_RDONLY);

	if (fd < 0 || !buf) {
		stress_fail(s, "consumer setup", w->id, 0);
		goto out;
	}
	for (;;) {
		ssize_t n = read(fd, buf, s->max);
		const struct smsg *m = (const struct smsg *)buf;
		const char *bad;

		if (n < 0) {
			stress_fail(s, "consumer read errno", w->id,
				    (uint32_t)errno);
			break;
		}
		if (n == (ssize_t)sizeof(*m) && m->producer == POISON_PRODUCER)
			break;
		bad = smsg_check(buf, n);
		if (bad) {
			stress_fail(s, bad, w->id, (uint32_t)n);
			break;
		}
		if (m->producer >= s->nprod || m->seq >= s->n) {
			stress_fail(s, "id out of range", m->producer, m->seq);
			break;
		}
		if (s->ordered) {
			if (m->seq != s->next_seq[m->producer])
				stress_fail(s, "out of order", m->producer,
					    m->seq);
			s->next_seq[m->producer] = m->seq + 1;
		}
		__atomic_fetch_add(&s->seen[(size_t)m->producer * s->n + m->seq],
				   1, __ATOMIC_RELAXED);
	}
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return NULL;
}

static void run_stress(uint32_t nprod, uint32_t ncons, int ordered)
{
	const char *env = getenv("KMSGQ_STRESS_N");
	int fd = open_clean(0, O_RDWR);
	struct kmsgq_info info = info_of(fd);
	struct stress s = {
		.dev = 0, .nprod = nprod, .ncons = ncons,
		.n = env ? (uint32_t)atol(env) : 20000,
		.ordered = ordered,
		.err_lock = PTHREAD_MUTEX_INITIALIZER,
	};
	pthread_t tp[16], tc[16];
	struct worker wp[16], wc[16];
	struct kmsgq_stats st;
	double t0;

	KT_ASSERT(nprod <= 16 && ncons <= 16);
	s.max = info.max_msg < 1024 ? info.max_msg : 1024;
	KT_ASSERT(s.max >= sizeof(struct smsg));
	s.seen = calloc((size_t)nprod * s.n, 1);
	s.next_seq = calloc(nprod, sizeof(*s.next_seq));
	KT_ASSERT(s.seen && s.next_seq);

	t0 = now_ms();
	for (uint32_t i = 0; i < ncons; i++) {
		wc[i] = (struct worker){ &s, i };
		KT_EQ(pthread_create(&tc[i], NULL, consumer, &wc[i]), 0);
	}
	for (uint32_t i = 0; i < nprod; i++) {
		wp[i] = (struct worker){ &s, i };
		KT_EQ(pthread_create(&tp[i], NULL, producer, &wp[i]), 0);
	}
	for (uint32_t i = 0; i < nprod; i++)
		pthread_join(tp[i], NULL);

	/* One poison pill per consumer; each consumer stops at its first. */
	for (uint32_t i = 0; i < ncons; i++) {
		struct smsg pill = { .magic = SMSG_MAGIC,
				     .producer = POISON_PRODUCER };

		if (write(fd, &pill, sizeof(pill)) != sizeof(pill))
			stress_fail(&s, "poison write", i, 0);
	}
	for (uint32_t i = 0; i < ncons; i++)
		pthread_join(tc[i], NULL);

	printf("  # %u producers x %u msgs, %u consumers: %.0f ms\n", nprod,
	       s.n, ncons, now_ms() - t0);
	KT_THREAD_OK(&s.err);
	for (size_t i = 0; i < (size_t)nprod * s.n; i++)
		if (s.seen[i] != 1)
			KT_FAIL("producer %zu seq %zu received %u times",
				i / s.n, i % s.n, s.seen[i]);
	st = stats_of(fd);
	KT_EQ(st.msgs_written, (unsigned long long)nprod * s.n + ncons);
	KT_EQ(st.msgs_read, st.msgs_written);
	KT_EQ(info_of(fd).msg_count, 0);
	free(s.seen);
	free(s.next_seq);
	close(fd);
}

/* Many producers, one consumer: every message once, per-producer FIFO. */
KT_TEST(stress_mpsc_ordered)
{
	run_stress(4, 1, 1);
}

/* Many producers, many consumers: every message delivered exactly once. */
KT_TEST(stress_mpmc_exactly_once)
{
	run_stress(4, 4, 0);
}

/*
 * Chaos: concurrent non-blocking I/O, flushes, stats and info calls for a
 * fixed time. Checks for deadlocks, crashes and accounting that stays
 * self-consistent; content is not checked because flushes drop messages.
 */
struct chaos {
	volatile int stop;
	uint32_t id;
	struct terr err;
};

static void *chaos_thread(void *arg)
{
	struct chaos *c = arg;
	int fd = open_raw(0, O_RDWR | O_NONBLOCK);
	uint8_t buf[512];
	uint32_t x = 2463534242u + c->id;

	if (fd < 0) {
		terr_set(&c->err, "open: %s", strerror(errno));
		return NULL;
	}
	memset(buf, 0x5a, sizeof(buf));
	while (!__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE)) {
		struct kmsgq_info info;
		struct kmsgq_stats st;
		ssize_t r = 0;

		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		switch (x % 16) {
		case 0:
			r = ioctl(fd, KMSGQ_IOC_FLUSH);
			break;
		case 1:
			r = ioctl(fd, KMSGQ_IOC_GET_STATS, &st);
			break;
		case 2:
			r = ioctl(fd, KMSGQ_IOC_GET_INFO, &info);
			if (r == 0 && (info.used_bytes > info.capacity ||
				       (info.msg_count == 0) !=
					       (info.used_bytes == 0)))
				terr_set(&c->err, "inconsistent info");
			break;
		case 3: case 4: case 5: case 6: case 7: case 8:
			r = write(fd, buf, 1 + (x >> 8) % sizeof(buf));
			break;
		default:
			r = read(fd, buf, sizeof(buf));
			break;
		}
		if (r < 0 && errno != EAGAIN && errno != EMSGSIZE)
			terr_set(&c->err, "op %u: %s", x % 16, strerror(errno));
	}
	close(fd);
	return NULL;
}

KT_TEST(chaos_concurrent_ops)
{
	enum { N = 8 };
	int fd = open_clean(0, O_RDWR);
	struct chaos c[N];
	pthread_t t[N];

	for (int i = 0; i < N; i++) {
		memset(&c[i], 0, sizeof(c[i]));
		c[i].id = (uint32_t)i;
		KT_EQ(pthread_create(&t[i], NULL, chaos_thread, &c[i]), 0);
	}
	sleep_ms(2000);
	for (int i = 0; i < N; i++)
		__atomic_store_n(&c[i].stop, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < N; i++)
		pthread_join(t[i], NULL);
	for (int i = 0; i < N; i++)
		KT_THREAD_OK(&c[i].err);

	/* Draining must reach an empty queue with zero bytes used. */
	struct kmsgq_info info = info_of(fd);
	int nb = open_raw(0, O_RDONLY | O_NONBLOCK);
	char buf[4096];

	KT_ASSERT(nb >= 0);
	while (read(nb, buf, sizeof(buf)) > 0)
		info.msg_count--;
	KT_EQ(info.msg_count, 0);
	KT_EQ(info_of(fd).used_bytes, 0);
	close(nb);
	close(fd);
}

static const struct kt_case cases[] = {
	KT_CASE(info_reports_config),
	KT_CASE(message_boundaries_preserved),
	KT_CASE(short_read_buffer_gets_emsgsize),
	KT_CASE(oversize_write_rejected),
	KT_CASE(zero_length_io_is_noop),
	KT_CASE(lseek_rejected),
	KT_CASE(unknown_ioctl_is_enotty),
	KT_CASE(bad_user_pointer_is_efault_and_atomic),
	KT_CASE(nonblock_read_on_empty_is_eagain),
	KT_CASE(nonblock_fill_matches_capacity),
	KT_CASE(poll_reflects_queue_state),
	KT_CASE(poll_times_out_when_empty),
	KT_CASE(blocking_read_wakes_on_write),
	KT_CASE(blocking_write_wakes_on_read),
	KT_CASE(epoll_reports_readable),
	KT_CASE(signal_interrupts_blocking_read),
	KT_CASE(flush_discards_and_counts),
	KT_CASE(stats_count_traffic),
	KT_CASE(devices_are_independent),
	KT_CASE(stress_mpsc_ordered),
	KT_CASE(stress_mpmc_exactly_once),
	KT_CASE(chaos_concurrent_ops),
};

int main(int argc, char **argv)
{
	if (access(dev_path(0), R_OK | W_OK) != 0) {
		fprintf(stderr, "%s: %s (is the module loaded? are you root?)\n",
			dev_path(0), strerror(errno));
		return 77;	/* conventional "skipped" exit status */
	}
	return kt_run(cases, KT_N(cases), argc, argv);
}
