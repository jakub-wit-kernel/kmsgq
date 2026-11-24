// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for the kmsgq ring core. The ring source is compiled directly
 * into this binary, so these tests exercise the same code the module runs.
 */
#define KT_IMPLEMENTATION
#include "kt.h"

#include <stdint.h>

#include "kmsgq_ring.h"

/* ---- transfer callbacks --------------------------------------------- */

struct buf_ctx {
	unsigned char *ext;	/* caller-side buffer */
	int calls;		/* xfer invocations so far */
	int fail_at;		/* fail the Nth call (1-based); 0 = never */
};

static int xfer_in(void *ctx, void *ring_ptr, size_t off, size_t len)
{
	struct buf_ctx *c = ctx;

	if (++c->calls == c->fail_at)
		return -EFAULT;
	memcpy(ring_ptr, c->ext + off, len);
	return 0;
}

static int xfer_out(void *ctx, void *ring_ptr, size_t off, size_t len)
{
	struct buf_ctx *c = ctx;

	if (++c->calls == c->fail_at)
		return -EFAULT;
	memcpy(c->ext + off, ring_ptr, len);
	return 0;
}

static int push(struct kmsgq_ring *r, const void *msg, size_t len)
{
	struct buf_ctx c = { .ext = (unsigned char *)msg };

	return kmsgq_ring_push(r, len, xfer_in, &c);
}

static long pop(struct kmsgq_ring *r, void *dst, size_t room)
{
	struct buf_ctx c = { .ext = dst };

	return kmsgq_ring_pop(r, room, xfer_out, &c);
}

static void fill_pattern(unsigned char *p, size_t len, unsigned seed)
{
	for (size_t i = 0; i < len; i++)
		p[i] = (unsigned char)(seed * 131u + i * 7u);
}

/* ---- tests ---------------------------------------------------------- */

KT_TEST(init_rejects_bad_args)
{
	struct kmsgq_ring r;
	unsigned char buf[16];

	KT_EQ(kmsgq_ring_init(&r, NULL, sizeof(buf)), -EINVAL);
	KT_EQ(kmsgq_ring_init(&r, buf, KMSGQ_RING_MIN_CAP - 1), -EINVAL);
	KT_EQ(kmsgq_ring_init(&r, buf, KMSGQ_RING_MIN_CAP), 0);
	KT_EQ(kmsgq_ring_max_payload(&r), 1);
	KT_EQ(kmsgq_ring_check(&r), 0);
}

KT_TEST(roundtrip_preserves_boundaries)
{
	struct kmsgq_ring r;
	unsigned char buf[128], out[64];

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	KT_EQ(push(&r, "a", 1), 0);
	KT_EQ(push(&r, "bb", 2), 0);
	KT_EQ(push(&r, "ccc", 3), 0);
	KT_EQ(r.count, 3);
	KT_EQ(r.used, 3 * KMSGQ_HDR_SIZE + 6);
	KT_EQ(kmsgq_ring_check(&r), 0);

	KT_EQ(kmsgq_ring_peek_len(&r), 1);
	KT_EQ(pop(&r, out, sizeof(out)), 1);
	KT_ASSERT(memcmp(out, "a", 1) == 0);
	KT_EQ(pop(&r, out, sizeof(out)), 2);
	KT_ASSERT(memcmp(out, "bb", 2) == 0);
	KT_EQ(pop(&r, out, sizeof(out)), 3);
	KT_ASSERT(memcmp(out, "ccc", 3) == 0);
	KT_EQ(pop(&r, out, sizeof(out)), -ENODATA);
	KT_EQ(kmsgq_ring_peek_len(&r), 0);
	KT_EQ(kmsgq_ring_check(&r), 0);
}

KT_TEST(zero_length_rejected)
{
	struct kmsgq_ring r;
	unsigned char buf[32];

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	KT_EQ(push(&r, "", 0), -EINVAL);
	KT_EQ(r.count, 0);
}

KT_TEST(oversize_and_full)
{
	struct kmsgq_ring r;
	unsigned char buf[32], msg[32] = { 0 };
	size_t max = sizeof(buf) - KMSGQ_HDR_SIZE;

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	KT_EQ(push(&r, msg, max + 1), -EMSGSIZE);
	KT_EQ(push(&r, msg, max), 0);		/* exact fit */
	KT_EQ(kmsgq_ring_free(&r), 0);
	KT_EQ(push(&r, msg, 1), -ENOSPC);
	KT_EQ(kmsgq_ring_check(&r), 0);
	KT_EQ(pop(&r, msg, sizeof(msg)), (long)max);
	KT_EQ(kmsgq_ring_free(&r), sizeof(buf));
}

KT_TEST(small_reader_buffer_keeps_message)
{
	struct kmsgq_ring r;
	unsigned char buf[64], out[8];

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	KT_EQ(push(&r, "0123456789", 10), 0);
	KT_EQ(pop(&r, out, 9), -EMSGSIZE);
	KT_EQ(r.count, 1);
	KT_EQ(kmsgq_ring_peek_len(&r), 10);
	KT_EQ(kmsgq_ring_check(&r), 0);
}

/*
 * Force the header and then the payload to straddle the end of the buffer,
 * at every possible split point.
 */
KT_TEST(wraparound_every_offset)
{
	enum { CAP = 23 };	/* prime, so offsets cycle through all values */

	for (size_t skew = 1; skew < CAP - KMSGQ_HDR_SIZE; skew++) {
		for (size_t len = 1; len <= CAP - KMSGQ_HDR_SIZE; len++) {
			struct kmsgq_ring r;
			unsigned char buf[CAP], in[CAP], out[CAP], pad[CAP];

			KT_EQ(kmsgq_ring_init(&r, buf, CAP), 0);
			memset(pad, 0xAA, sizeof(pad));
			/* Move head/tail to @skew while keeping one msg queued. */
			KT_EQ(push(&r, pad, skew), 0);
			if (!kmsgq_ring_fits(&r, len)) {
				/* does not fit alongside the pad; next case */
				KT_EQ(pop(&r, out, CAP), (long)skew);
				continue;
			}
			fill_pattern(in, len, (unsigned)(skew + len));
			KT_EQ(push(&r, in, len), 0);
			KT_EQ(kmsgq_ring_check(&r), 0);
			KT_EQ(pop(&r, out, CAP), (long)skew);
			KT_EQ(pop(&r, out, CAP), (long)len);
			KT_ASSERT(memcmp(in, out, len) == 0);
			KT_EQ(kmsgq_ring_check(&r), 0);
		}
	}
}

/*
 * Wrap without the empty-ring re-anchor: keep one message queued at all
 * times so head and tail genuinely travel round the buffer.
 */
KT_TEST(sustained_wrap_with_backlog)
{
	enum { CAP = 61 };
	struct kmsgq_ring r;
	unsigned char buf[CAP], in[16], out[16];
	unsigned next_in = 0, next_out = 0;

	KT_EQ(kmsgq_ring_init(&r, buf, CAP), 0);
	for (int i = 0; i < 5000; i++) {
		size_t len = 1 + (size_t)(i % 13);

		while (!kmsgq_ring_fits(&r, len)) {
			long got = pop(&r, out, sizeof(out));

			KT_ASSERT(got > 0);
			fill_pattern(in, (size_t)got, next_out++);
			KT_ASSERT(memcmp(in, out, (size_t)got) == 0);
		}
		fill_pattern(in, len, next_in++);
		KT_EQ(push(&r, in, len), 0);
		KT_EQ(kmsgq_ring_check(&r), 0);
	}
	KT_ASSERT(next_out > 1000);
}

KT_TEST(failed_push_leaves_ring_unchanged)
{
	enum { CAP = 20 };
	unsigned char buf[CAP], snap[CAP], msg[CAP];

	fill_pattern(msg, sizeof(msg), 9);
	for (int fail_at = 1; fail_at <= 2; fail_at++) {
		struct kmsgq_ring r, before;
		struct buf_ctx c = { .ext = msg, .fail_at = fail_at };
		unsigned char out[CAP];

		/* tail = 5 ("bcd" queued at 5..11), head = 12. */
		KT_EQ(kmsgq_ring_init(&r, buf, CAP), 0);
		KT_EQ(push(&r, "a", 1), 0);
		KT_EQ(push(&r, "bcd", 3), 0);
		KT_EQ(pop(&r, out, CAP), 1);
		before = r;
		memcpy(snap, buf, CAP);

		/* 9 bytes: payload 16..19 + 0..4, two transfers. */
		KT_EQ(kmsgq_ring_push(&r, 9, xfer_in, &c), -EFAULT);
		KT_EQ(c.calls, fail_at);
		KT_EQ(r.head, before.head);
		KT_EQ(r.used, before.used);
		KT_EQ(r.count, before.count);
		KT_EQ(kmsgq_ring_check(&r), 0);
		/* The queued message must be untouched. */
		KT_ASSERT(memcmp(buf + 5, snap + 5, 7) == 0);
		KT_EQ(pop(&r, out, CAP), 3);
		KT_ASSERT(memcmp(out, "bcd", 3) == 0);
	}
}

KT_TEST(failed_pop_leaves_ring_unchanged)
{
	enum { CAP = 20 };
	unsigned char buf[CAP], msg[CAP], out[CAP];

	for (int fail_at = 1; fail_at <= 2; fail_at++) {
		struct kmsgq_ring r, before;
		struct buf_ctx c = { .ext = out, .fail_at = fail_at };

		/*
		 * Build: [m1:1][m2:3] then drop m1, so tail = 5, head = 12.
		 * A 9-byte message then has its header at 12..15 and its
		 * payload at 16..19 + 0..4, i.e. split into two transfers.
		 */
		KT_EQ(kmsgq_ring_init(&r, buf, CAP), 0);
		KT_EQ(push(&r, "a", 1), 0);
		KT_EQ(push(&r, "bcd", 3), 0);
		KT_EQ(pop(&r, out, CAP), 1);
		fill_pattern(msg, 9, 3);
		KT_EQ(push(&r, msg, 9), 0);
		KT_EQ(r.head, 5);
		KT_EQ(pop(&r, out, CAP), 3);
		KT_EQ(r.tail, 12);

		before = r;
		KT_EQ(kmsgq_ring_pop(&r, CAP, xfer_out, &c), -EFAULT);
		KT_EQ(c.calls, fail_at);
		KT_EQ(r.tail, before.tail);
		KT_EQ(r.used, before.used);
		KT_EQ(r.count, before.count);
		KT_EQ(kmsgq_ring_check(&r), 0);

		KT_EQ(pop(&r, out, CAP), 9);
		KT_ASSERT(memcmp(out, msg, 9) == 0);
	}
}

KT_TEST(flush_drops_everything)
{
	struct kmsgq_ring r;
	unsigned char buf[64];

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	for (int i = 0; i < 5; i++)
		KT_EQ(push(&r, "hello", 5), 0);
	KT_EQ(kmsgq_ring_flush(&r), 5);
	KT_EQ(r.count, 0);
	KT_EQ(r.used, 0);
	KT_EQ(kmsgq_ring_check(&r), 0);
	KT_EQ(kmsgq_ring_flush(&r), 0);
}

KT_TEST(check_detects_corruption)
{
	struct kmsgq_ring r;
	unsigned char buf[64];
	u32 bogus = 1000;

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	KT_EQ(push(&r, "abc", 3), 0);
	memcpy(buf, &bogus, sizeof(bogus));
	KT_ASSERT(kmsgq_ring_check(&r) < 0);

	KT_EQ(kmsgq_ring_init(&r, buf, sizeof(buf)), 0);
	KT_EQ(push(&r, "abc", 3), 0);
	r.count = 2;
	KT_ASSERT(kmsgq_ring_check(&r) < 0);
}

/*
 * Model-based randomized test: drive the ring with random pushes and pops
 * and compare every result against a trivially-correct reference FIFO.
 */
struct model {
	size_t lens[4096];
	unsigned seeds[4096];
	size_t head, tail, n;
};

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void)
{
	/* xorshift64* */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 2685821657736338717ull;
}

KT_TEST(randomized_against_model)
{
	static const size_t caps[] = { 5, 7, 16, 61, 256, 1021, 4096 };
	static struct model m;
	unsigned char in[4096], out[4096];
	const char *env = getenv("KMSGQ_RANDOM_OPS");
	long ops = env ? atol(env) : 200000;

	for (size_t ci = 0; ci < KT_N(caps); ci++) {
		size_t cap = caps[ci];
		unsigned char *buf = malloc(cap);
		struct kmsgq_ring r;
		unsigned seed = 0;

		KT_ASSERT(buf);
		KT_EQ(kmsgq_ring_init(&r, buf, cap), 0);
		memset(&m, 0, sizeof(m));

		for (long op = 0; op < ops; op++) {
			uint64_t x = rng();
			size_t maxp = kmsgq_ring_max_payload(&r);

			if (x % 100 < 52) {
				size_t len = 1 + (size_t)((x >> 8) % maxp);
				int want = len + KMSGQ_HDR_SIZE <=
						   kmsgq_ring_free(&r) ? 0 :
						   -ENOSPC;

				if (m.n == KT_N(m.lens))
					continue;
				fill_pattern(in, len, seed);
				KT_EQ(push(&r, in, len), want);
				if (want == 0) {
					m.lens[m.head] = len;
					m.seeds[m.head] = seed++;
					m.head = (m.head + 1) % KT_N(m.lens);
					m.n++;
				}
			} else if (x % 100 < 98) {
				size_t room = (x >> 20) % 4 == 0 ?
					      (size_t)((x >> 24) % (maxp + 1)) :
					      sizeof(out);
				long got = pop(&r, out, room);

				if (m.n == 0) {
					KT_EQ(got, -ENODATA);
				} else if (m.lens[m.tail] > room) {
					KT_EQ(got, -EMSGSIZE);
				} else {
					KT_EQ(got, (long)m.lens[m.tail]);
					fill_pattern(in, (size_t)got,
						     m.seeds[m.tail]);
					KT_ASSERT(memcmp(in, out,
							 (size_t)got) == 0);
					m.tail = (m.tail + 1) % KT_N(m.lens);
					m.n--;
				}
			} else {
				KT_EQ(kmsgq_ring_flush(&r), m.n);
				m.head = m.tail = m.n = 0;
			}
			KT_EQ(r.count, m.n);
			if (op % 64 == 0)
				KT_EQ(kmsgq_ring_check(&r), 0);
		}
		KT_EQ(kmsgq_ring_check(&r), 0);
		free(buf);
	}
}

static const struct kt_case cases[] = {
	KT_CASE(init_rejects_bad_args),
	KT_CASE(roundtrip_preserves_boundaries),
	KT_CASE(zero_length_rejected),
	KT_CASE(oversize_and_full),
	KT_CASE(small_reader_buffer_keeps_message),
	KT_CASE(wraparound_every_offset),
	KT_CASE(sustained_wrap_with_backlog),
	KT_CASE(failed_push_leaves_ring_unchanged),
	KT_CASE(failed_pop_leaves_ring_unchanged),
	KT_CASE(flush_drops_everything),
	KT_CASE(check_detects_corruption),
	KT_CASE(randomized_against_model),
};

int main(int argc, char **argv)
{
	return kt_run(cases, KT_N(cases), argc, argv);
}
