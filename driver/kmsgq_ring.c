// SPDX-License-Identifier: GPL-2.0
/*
 * kmsgq_ring - length-prefixed message ring. See kmsgq_ring.h.
 *
 * Layout: messages are packed back to back with no padding. A header or a
 * payload may straddle the end of the buffer and continue at offset 0; all
 * accesses go through the wrap-aware helpers below.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "kmsgq_ring.h"

static size_t ring_advance(const struct kmsgq_ring *r, size_t off, size_t n)
{
	/* off < cap and n <= cap, so one subtraction is enough. */
	off += n;
	return off >= r->cap ? off - r->cap : off;
}

static void ring_write_raw(struct kmsgq_ring *r, size_t off, const void *src,
			   size_t len)
{
	size_t first = r->cap - off;

	if (first > len)
		first = len;
	memcpy(r->buf + off, src, first);
	if (len > first)
		memcpy(r->buf, (const u8 *)src + first, len - first);
}

static void ring_read_raw(const struct kmsgq_ring *r, size_t off, void *dst,
			  size_t len)
{
	size_t first = r->cap - off;

	if (first > len)
		first = len;
	memcpy(dst, r->buf + off, first);
	if (len > first)
		memcpy((u8 *)dst + first, r->buf, len - first);
}

/* Run @xfer over the (at most two) contiguous ring segments of a range. */
static int ring_xfer(struct kmsgq_ring *r, size_t off, size_t len,
		     kmsgq_xfer_fn xfer, void *ctx)
{
	size_t first = r->cap - off;
	int ret;

	if (first > len)
		first = len;
	ret = xfer(ctx, r->buf + off, 0, first);
	if (ret)
		return ret;
	if (len > first)
		ret = xfer(ctx, r->buf, first, len - first);
	return ret;
}

int kmsgq_ring_init(struct kmsgq_ring *r, void *storage, size_t cap)
{
	if (!r || !storage || cap < KMSGQ_RING_MIN_CAP)
		return -EINVAL;
	/* Lengths are stored as u32; refuse rings that could exceed that. */
	if (cap - KMSGQ_HDR_SIZE > (u32)-1)
		return -EINVAL;
	r->buf = storage;
	r->cap = cap;
	kmsgq_ring_reset(r);
	return 0;
}

void kmsgq_ring_reset(struct kmsgq_ring *r)
{
	r->head = 0;
	r->tail = 0;
	r->used = 0;
	r->count = 0;
}

int kmsgq_ring_push(struct kmsgq_ring *r, size_t len, kmsgq_xfer_fn xfer,
		    void *ctx)
{
	u32 hdr;
	size_t payload_off;
	int ret;

	if (len == 0)
		return -EINVAL;
	if (len > kmsgq_ring_max_payload(r))
		return -EMSGSIZE;
	if (!kmsgq_ring_fits(r, len))
		return -ENOSPC;

	/*
	 * Fill the payload first, into free space. Nothing is published until
	 * the header is written and head/used/count are updated, so a failed
	 * transfer leaves the ring unchanged.
	 */
	payload_off = ring_advance(r, r->head, KMSGQ_HDR_SIZE);
	ret = ring_xfer(r, payload_off, len, xfer, ctx);
	if (ret)
		return ret;

	hdr = (u32)len;
	ring_write_raw(r, r->head, &hdr, sizeof(hdr));
	r->head = ring_advance(r, payload_off, len);
	r->used += KMSGQ_HDR_SIZE + len;
	r->count++;
	return 0;
}

size_t kmsgq_ring_peek_len(const struct kmsgq_ring *r)
{
	u32 hdr;

	if (kmsgq_ring_empty(r))
		return 0;
	ring_read_raw(r, r->tail, &hdr, sizeof(hdr));
	return hdr;
}

long kmsgq_ring_pop(struct kmsgq_ring *r, size_t room, kmsgq_xfer_fn xfer,
		    void *ctx)
{
	size_t len, payload_off;
	int ret;

	if (kmsgq_ring_empty(r))
		return -ENODATA;
	len = kmsgq_ring_peek_len(r);
	if (len > room)
		return -EMSGSIZE;

	payload_off = ring_advance(r, r->tail, KMSGQ_HDR_SIZE);
	ret = ring_xfer(r, payload_off, len, xfer, ctx);
	if (ret)
		return ret;

	r->tail = ring_advance(r, payload_off, len);
	r->used -= KMSGQ_HDR_SIZE + len;
	r->count--;
	/* Re-anchor an empty ring so future messages are less likely to wrap. */
	if (r->count == 0)
		kmsgq_ring_reset(r);
	return (long)len;
}

size_t kmsgq_ring_flush(struct kmsgq_ring *r)
{
	size_t dropped = r->count;

	kmsgq_ring_reset(r);
	return dropped;
}

int kmsgq_ring_check(const struct kmsgq_ring *r)
{
	size_t off, walked = 0, n;
	u32 hdr;

	if (!r->buf || r->cap < KMSGQ_RING_MIN_CAP)
		return -EINVAL;
	if (r->head >= r->cap || r->tail >= r->cap || r->used > r->cap)
		return -ERANGE;
	if ((r->count == 0) != (r->used == 0))
		return -EBADMSG;

	off = r->tail;
	for (n = 0; n < r->count; n++) {
		if (r->used - walked < KMSGQ_HDR_SIZE)
			return -EBADMSG;
		ring_read_raw(r, off, &hdr, sizeof(hdr));
		if (hdr == 0 || hdr > r->used - walked - KMSGQ_HDR_SIZE)
			return -EBADMSG;
		walked += KMSGQ_HDR_SIZE + hdr;
		off = ring_advance(r, ring_advance(r, off, KMSGQ_HDR_SIZE), hdr);
	}
	if (walked != r->used)
		return -EBADMSG;
	/* A full ring has head == tail too, so only check the offsets. */
	if (off != r->head)
		return -EBADMSG;
	return 0;
}
