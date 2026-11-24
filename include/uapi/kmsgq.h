/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * kmsgq - message-oriented FIFO character device: user-space API.
 *
 * Each write(2) enqueues exactly one message; each read(2) dequeues exactly
 * one whole message. Message boundaries are always preserved.
 *
 * This header is shared by the kernel module and user-space programs.
 */
#ifndef _UAPI_KMSGQ_H
#define _UAPI_KMSGQ_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KMSGQ_NAME		"kmsgq"
#define KMSGQ_API_VERSION	1

/* Static description of a queue plus its current fill level. */
struct kmsgq_info {
	__u32 api_version;	/* KMSGQ_API_VERSION */
	__u32 minor;		/* device index (N in /dev/kmsgqN) */
	__u64 capacity;		/* ring size in bytes, including headers */
	__u64 max_msg;		/* largest payload accepted by write(2) */
	__u64 hdr_size;		/* per-message overhead in the ring */
	__u64 used_bytes;	/* bytes in use, including headers */
	__u64 msg_count;	/* messages currently queued */
};

/* Monotonic counters, reset by KMSGQ_IOC_RESET_STATS. */
struct kmsgq_stats {
	__u64 msgs_written;
	__u64 msgs_read;
	__u64 bytes_written;	/* payload bytes only */
	__u64 bytes_read;	/* payload bytes only */
	__u64 msgs_flushed;	/* discarded by KMSGQ_IOC_FLUSH */
	__u64 flushes;
	__u64 reads_blocked;	/* times a reader had to sleep */
	__u64 writes_blocked;	/* times a writer had to sleep */
	__u64 high_watermark;	/* max used_bytes observed */
};

#define KMSGQ_IOC_MAGIC		0xB7

/* Fill a struct kmsgq_info. */
#define KMSGQ_IOC_GET_INFO	_IOR(KMSGQ_IOC_MAGIC, 1, struct kmsgq_info)
/* Fill a struct kmsgq_stats. */
#define KMSGQ_IOC_GET_STATS	_IOR(KMSGQ_IOC_MAGIC, 2, struct kmsgq_stats)
/* Discard every queued message and wake blocked writers. */
#define KMSGQ_IOC_FLUSH		_IO(KMSGQ_IOC_MAGIC, 3)
/* Zero all counters in struct kmsgq_stats. */
#define KMSGQ_IOC_RESET_STATS	_IO(KMSGQ_IOC_MAGIC, 4)
/* Store the payload length of the next message (0 if the queue is empty). */
#define KMSGQ_IOC_PEEK_LEN	_IOR(KMSGQ_IOC_MAGIC, 5, __u32)

#endif /* _UAPI_KMSGQ_H */
