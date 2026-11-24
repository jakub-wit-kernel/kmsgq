/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kmsgq_ring - a byte ring buffer that stores length-prefixed messages.
 *
 * This file has no dependency on kernel-only facilities so the exact same
 * code is linked into the kernel module and into the user-space unit tests.
 *
 * The ring does not lock; callers serialise access. It never touches the
 * caller's message buffer directly: all payload copies go through a
 * kmsgq_xfer_fn callback so the kernel can use copy_{to,from}_user() and the
 * tests can use memcpy() or inject faults.
 *
 * Every operation is all-or-nothing: if a transfer callback fails, the ring
 * state is left exactly as it was before the call.
 */
#ifndef KMSGQ_RING_H
#define KMSGQ_RING_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

/* Each message is stored as a native-endian u32 length followed by payload. */
#define KMSGQ_HDR_SIZE		sizeof(u32)
/* Smallest ring that can hold a one-byte message. */
#define KMSGQ_RING_MIN_CAP	(KMSGQ_HDR_SIZE + 1)

/*
 * Copy @len bytes between ring memory at @ring_ptr and the caller's buffer at
 * offset @ext_off. The direction is implied by which operation calls it.
 * Return 0 on success or a negative errno.
 */
typedef int (*kmsgq_xfer_fn)(void *ctx, void *ring_ptr, size_t ext_off,
			     size_t len);

struct kmsgq_ring {
	u8 *buf;
	size_t cap;	/* size of buf in bytes */
	size_t head;	/* offset where the next message header is written */
	size_t tail;	/* offset of the oldest message header */
	size_t used;	/* bytes occupied, headers included */
	size_t count;	/* number of queued messages */
};

int kmsgq_ring_init(struct kmsgq_ring *r, void *storage, size_t cap);
void kmsgq_ring_reset(struct kmsgq_ring *r);

static inline size_t kmsgq_ring_free(const struct kmsgq_ring *r)
{
	return r->cap - r->used;
}

static inline bool kmsgq_ring_empty(const struct kmsgq_ring *r)
{
	return r->count == 0;
}

/* Largest payload the ring could ever hold, even when empty. */
static inline size_t kmsgq_ring_max_payload(const struct kmsgq_ring *r)
{
	return r->cap - KMSGQ_HDR_SIZE;
}

/* True if a message of @len payload bytes fits right now. */
static inline bool kmsgq_ring_fits(const struct kmsgq_ring *r, size_t len)
{
	return len <= kmsgq_ring_max_payload(r) &&
	       len + KMSGQ_HDR_SIZE <= kmsgq_ring_free(r);
}

/*
 * Enqueue one message of @len bytes, pulled through @xfer.
 * Returns 0, -EINVAL (len == 0), -EMSGSIZE (can never fit),
 * -ENOSPC (does not fit now), or the error returned by @xfer.
 */
int kmsgq_ring_push(struct kmsgq_ring *r, size_t len, kmsgq_xfer_fn xfer,
		    void *ctx);

/* Payload length of the oldest message, or 0 if the ring is empty. */
size_t kmsgq_ring_peek_len(const struct kmsgq_ring *r);

/*
 * Dequeue the oldest message into a caller buffer of @room bytes.
 * Returns the payload length (> 0), -ENODATA (empty), -EMSGSIZE (@room too
 * small; the message stays queued), or the error returned by @xfer.
 */
long kmsgq_ring_pop(struct kmsgq_ring *r, size_t room, kmsgq_xfer_fn xfer,
		    void *ctx);

/* Discard every message. Returns how many were dropped. */
size_t kmsgq_ring_flush(struct kmsgq_ring *r);

/*
 * Walk every stored header and verify the bookkeeping is self-consistent.
 * Returns 0 if consistent, or a negative errno describing the first problem.
 * Intended for tests and debug builds; O(count).
 */
int kmsgq_ring_check(const struct kmsgq_ring *r);

#endif /* KMSGQ_RING_H */
