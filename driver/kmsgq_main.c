// SPDX-License-Identifier: GPL-2.0
/*
 * kmsgq - message-oriented FIFO character devices.
 *
 * Creates /dev/kmsgq0 .. /dev/kmsgq<N-1>. Each device is an independent,
 * bounded, in-kernel message queue:
 *
 *   write(2)  enqueues one message (1..max_msg bytes)
 *   read(2)   dequeues one whole message; -EMSGSIZE if the buffer is short
 *   poll(2)   EPOLLIN when a message is queued,
 *             EPOLLOUT when a max_msg-sized write would not block
 *   ioctl(2)  info, statistics, flush, peek (see include/uapi/kmsgq.h)
 *
 * Blocking and O_NONBLOCK semantics follow pipes. See docs/ARCHITECTURE.md
 * for the locking and wake-up design.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "kmsgq.h"
#include "kmsgq_ring.h"

#define KMSGQ_MAX_DEVS		64U
#define KMSGQ_MIN_CAPACITY	64U
#define KMSGQ_MAX_CAPACITY	(16U << 20)

static unsigned int nr_devs = 4;
module_param(nr_devs, uint, 0444);
MODULE_PARM_DESC(nr_devs, "Number of queue devices to create (1-64, default 4)");

static unsigned int capacity = 64 * 1024;
module_param(capacity, uint, 0444);
MODULE_PARM_DESC(capacity, "Ring size per device in bytes, headers included (64 B - 16 MiB, default 64 KiB)");

static unsigned int max_msg = 4096;
module_param(max_msg, uint, 0444);
MODULE_PARM_DESC(max_msg, "Largest accepted message payload in bytes (default 4096)");

struct kmsgq_dev {
	struct mutex lock;		/* protects ring and stats */
	struct kmsgq_ring ring;
	struct kmsgq_stats stats;

	wait_queue_head_t readq;	/* readers/pollers waiting for data */
	wait_queue_head_t writeq;	/* writers/pollers waiting for space */

	/*
	 * Lock-free mirrors of ring state, updated under @lock and read with
	 * READ_ONCE() by wait conditions and poll, which must not sleep on
	 * the mutex.
	 */
	size_t snap_count;
	size_t snap_free;

	size_t max_msg;
	unsigned int minor;
	void *storage;
	struct cdev cdev;
	struct device *device;
};

static dev_t kmsgq_devt;
static struct class *kmsgq_class;
static struct kmsgq_dev *kmsgq_devs;

/* Caller-buffer context for the ring transfer callbacks. */
struct kmsgq_uctx {
	char __user *out;
	const char __user *in;
};

static int kmsgq_xfer_to_user(void *ctx, void *ring_ptr, size_t off,
			      size_t len)
{
	struct kmsgq_uctx *u = ctx;

	return copy_to_user(u->out + off, ring_ptr, len) ? -EFAULT : 0;
}

static int kmsgq_xfer_from_user(void *ctx, void *ring_ptr, size_t off,
				size_t len)
{
	struct kmsgq_uctx *u = ctx;

	return copy_from_user(ring_ptr, u->in + off, len) ? -EFAULT : 0;
}

/* Refresh the lock-free mirrors after any change to the ring. */
static void kmsgq_publish(struct kmsgq_dev *d)
{
	lockdep_assert_held(&d->lock);
	WRITE_ONCE(d->snap_count, d->ring.count);
	WRITE_ONCE(d->snap_free, kmsgq_ring_free(&d->ring));
	if (d->ring.used > d->stats.high_watermark)
		d->stats.high_watermark = d->ring.used;
}

static bool kmsgq_readable(struct kmsgq_dev *d)
{
	return READ_ONCE(d->snap_count) > 0;
}

static bool kmsgq_writable(struct kmsgq_dev *d, size_t len)
{
	return READ_ONCE(d->snap_free) >= len + KMSGQ_HDR_SIZE;
}

static int kmsgq_open(struct inode *inode, struct file *filp)
{
	struct kmsgq_dev *d = container_of(inode->i_cdev, struct kmsgq_dev,
					   cdev);

	filp->private_data = d;
	dev_dbg(d->device, "open pid=%d flags=%#x\n", task_pid_nr(current),
		filp->f_flags);
	/* A queue has no file position: reject lseek/pread/pwrite. */
	return stream_open(inode, filp);
}

static int kmsgq_release(struct inode *inode, struct file *filp)
{
	struct kmsgq_dev *d = filp->private_data;

	dev_dbg(d->device, "release pid=%d\n", task_pid_nr(current));
	return 0;
}

static ssize_t kmsgq_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct kmsgq_dev *d = filp->private_data;
	struct kmsgq_uctx u = { .out = buf };
	bool slept = false;
	long ret;

	if (count == 0)
		return 0;

	for (;;) {
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		if (!kmsgq_ring_empty(&d->ring))
			break;
		mutex_unlock(&d->lock);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		slept = true;
		if (wait_event_interruptible(d->readq, kmsgq_readable(d)))
			return -ERESTARTSYS;
		/* Another reader may have won the race; loop and recheck. */
	}

	if (slept)
		d->stats.reads_blocked++;
	/*
	 * copy_to_user() may fault and sleep while we hold the mutex. That is
	 * safe: the mutex is sleepable and this driver has no mmap, so a
	 * fault can never re-enter the driver and take the lock again.
	 */
	ret = kmsgq_ring_pop(&d->ring, count, kmsgq_xfer_to_user, &u);
	if (ret > 0) {
		d->stats.msgs_read++;
		d->stats.bytes_read += ret;
		kmsgq_publish(d);
	}
	mutex_unlock(&d->lock);

	if (ret > 0)
		wake_up_interruptible_poll(&d->writeq, EPOLLOUT | EPOLLWRNORM);
	else
		dev_dbg(d->device, "read failed: %ld\n", ret);
	return ret;
}

static ssize_t kmsgq_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct kmsgq_dev *d = filp->private_data;
	struct kmsgq_uctx u = { .in = buf };
	bool slept = false;
	int ret;

	if (count == 0)
		return 0;
	if (count > d->max_msg)
		return -EMSGSIZE;

	for (;;) {
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		if (kmsgq_ring_fits(&d->ring, count))
			break;
		mutex_unlock(&d->lock);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		slept = true;
		if (wait_event_interruptible(d->writeq,
					     kmsgq_writable(d, count)))
			return -ERESTARTSYS;
	}

	if (slept)
		d->stats.writes_blocked++;
	ret = kmsgq_ring_push(&d->ring, count, kmsgq_xfer_from_user, &u);
	if (!ret) {
		d->stats.msgs_written++;
		d->stats.bytes_written += count;
		kmsgq_publish(d);
	}
	mutex_unlock(&d->lock);

	if (ret) {
		dev_dbg(d->device, "write failed: %d\n", ret);
		return ret;
	}
	wake_up_interruptible_poll(&d->readq, EPOLLIN | EPOLLRDNORM);
	return count;
}

static __poll_t kmsgq_poll(struct file *filp, poll_table *wait)
{
	struct kmsgq_dev *d = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &d->readq, wait);
	poll_wait(filp, &d->writeq, wait);

	if (kmsgq_readable(d))
		mask |= EPOLLIN | EPOLLRDNORM;
	/*
	 * Report writable only when a write of any legal size would succeed,
	 * so an event loop that sees EPOLLOUT never gets -EAGAIN.
	 */
	if (kmsgq_writable(d, d->max_msg))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static long kmsgq_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct kmsgq_dev *d = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case KMSGQ_IOC_GET_INFO: {
		struct kmsgq_info info;

		memset(&info, 0, sizeof(info));
		info.api_version = KMSGQ_API_VERSION;
		info.minor = d->minor;
		info.max_msg = d->max_msg;
		info.hdr_size = KMSGQ_HDR_SIZE;
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		info.capacity = d->ring.cap;
		info.used_bytes = d->ring.used;
		info.msg_count = d->ring.count;
		mutex_unlock(&d->lock);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case KMSGQ_IOC_GET_STATS: {
		struct kmsgq_stats st;

		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		st = d->stats;
		mutex_unlock(&d->lock);
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
	}
	case KMSGQ_IOC_FLUSH: {
		size_t dropped;

		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		dropped = kmsgq_ring_flush(&d->ring);
		d->stats.msgs_flushed += dropped;
		d->stats.flushes++;
		kmsgq_publish(d);
		mutex_unlock(&d->lock);
		dev_dbg(d->device, "flushed %zu messages\n", dropped);
		wake_up_interruptible_poll(&d->writeq, EPOLLOUT | EPOLLWRNORM);
		return 0;
	}
	case KMSGQ_IOC_RESET_STATS:
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		memset(&d->stats, 0, sizeof(d->stats));
		d->stats.high_watermark = d->ring.used;
		mutex_unlock(&d->lock);
		return 0;
	case KMSGQ_IOC_PEEK_LEN: {
		__u32 len;

		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		len = (__u32)kmsgq_ring_peek_len(&d->ring);
		mutex_unlock(&d->lock);
		return put_user(len, (__u32 __user *)uarg);
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations kmsgq_fops = {
	.owner		= THIS_MODULE,
	.open		= kmsgq_open,
	.release	= kmsgq_release,
	.read		= kmsgq_read,
	.write		= kmsgq_write,
	.poll		= kmsgq_poll,
	.unlocked_ioctl	= kmsgq_ioctl,
	/* All ioctl structs use fixed-width fields, so 32-bit callers work. */
	.compat_ioctl	= compat_ptr_ioctl,
};

static int kmsgq_validate_params(void)
{
	if (nr_devs < 1 || nr_devs > KMSGQ_MAX_DEVS) {
		pr_err("nr_devs=%u out of range 1..%u\n", nr_devs,
		       KMSGQ_MAX_DEVS);
		return -EINVAL;
	}
	if (capacity < KMSGQ_MIN_CAPACITY || capacity > KMSGQ_MAX_CAPACITY) {
		pr_err("capacity=%u out of range %u..%u\n", capacity,
		       KMSGQ_MIN_CAPACITY, KMSGQ_MAX_CAPACITY);
		return -EINVAL;
	}
	if (max_msg < 1 || max_msg > capacity - KMSGQ_HDR_SIZE) {
		pr_err("max_msg=%u out of range 1..%zu for capacity=%u\n",
		       max_msg, capacity - KMSGQ_HDR_SIZE, capacity);
		return -EINVAL;
	}
	return 0;
}

static int kmsgq_dev_setup(struct kmsgq_dev *d, unsigned int minor)
{
	dev_t devt = MKDEV(MAJOR(kmsgq_devt), MINOR(kmsgq_devt) + minor);
	int ret;

	d->minor = minor;
	d->max_msg = max_msg;
	mutex_init(&d->lock);
	init_waitqueue_head(&d->readq);
	init_waitqueue_head(&d->writeq);

	d->storage = kvzalloc(capacity, GFP_KERNEL);
	if (!d->storage) {
		ret = -ENOMEM;
		goto err_mutex;
	}
	ret = kmsgq_ring_init(&d->ring, d->storage, capacity);
	if (ret)
		goto err_free;
	d->snap_count = 0;
	d->snap_free = capacity;

	/* Make the cdev live before the node appears, so open() never races. */
	cdev_init(&d->cdev, &kmsgq_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, devt, 1);
	if (ret)
		goto err_free;

	d->device = device_create(kmsgq_class, NULL, devt, d,
				  KMSGQ_NAME "%u", minor);
	if (IS_ERR(d->device)) {
		ret = PTR_ERR(d->device);
		d->device = NULL;
		goto err_cdev;
	}
	return 0;

err_cdev:
	cdev_del(&d->cdev);
err_free:
	kvfree(d->storage);
	d->storage = NULL;
err_mutex:
	mutex_destroy(&d->lock);
	pr_err("setup of %s%u failed: %d\n", KMSGQ_NAME, minor, ret);
	return ret;
}

static void kmsgq_dev_teardown(struct kmsgq_dev *d)
{
	device_destroy(kmsgq_class, d->cdev.dev);
	cdev_del(&d->cdev);
	kvfree(d->storage);
	d->storage = NULL;
	mutex_destroy(&d->lock);
}

static int __init kmsgq_init(void)
{
	unsigned int i;
	int ret;

	ret = kmsgq_validate_params();
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&kmsgq_devt, 0, nr_devs, KMSGQ_NAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	kmsgq_class = class_create(KMSGQ_NAME);
#else
	kmsgq_class = class_create(THIS_MODULE, KMSGQ_NAME);
#endif
	if (IS_ERR(kmsgq_class)) {
		ret = PTR_ERR(kmsgq_class);
		pr_err("class_create failed: %d\n", ret);
		goto err_region;
	}

	kmsgq_devs = kcalloc(nr_devs, sizeof(*kmsgq_devs), GFP_KERNEL);
	if (!kmsgq_devs) {
		ret = -ENOMEM;
		goto err_class;
	}

	for (i = 0; i < nr_devs; i++) {
		ret = kmsgq_dev_setup(&kmsgq_devs[i], i);
		if (ret)
			goto err_devs;
	}

	pr_info("loaded: %u devices, major %d, capacity %u B, max_msg %u B\n",
		nr_devs, MAJOR(kmsgq_devt), capacity, max_msg);
	return 0;

err_devs:
	while (i--)
		kmsgq_dev_teardown(&kmsgq_devs[i]);
	kfree(kmsgq_devs);
	kmsgq_devs = NULL;
err_class:
	class_destroy(kmsgq_class);
err_region:
	unregister_chrdev_region(kmsgq_devt, nr_devs);
	return ret;
}

/*
 * No file can be open here: fops.owner holds a module reference for every
 * open file, so rmmod fails with -EBUSY until the last one is closed.
 */
static void __exit kmsgq_exit(void)
{
	unsigned int i;

	for (i = 0; i < nr_devs; i++)
		kmsgq_dev_teardown(&kmsgq_devs[i]);
	kfree(kmsgq_devs);
	class_destroy(kmsgq_class);
	unregister_chrdev_region(kmsgq_devt, nr_devs);
	pr_info("unloaded\n");
}

module_init(kmsgq_init);
module_exit(kmsgq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Message-oriented FIFO character devices with poll and ioctl");
MODULE_VERSION("1.0.0");
