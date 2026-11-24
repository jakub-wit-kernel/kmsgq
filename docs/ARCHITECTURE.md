# kmsgq architecture

## 1. Requirements

**Functional**

| ID | Requirement |
|----|-------------|
| F1 | Expose `nr_devs` independent queues as character devices `/dev/kmsgq0..N-1`. |
| F2 | `write(2)` enqueues exactly one message of 1..`max_msg` bytes. |
| F3 | `read(2)` dequeues exactly one whole message. Messages are never split or merged. |
| F4 | A read buffer smaller than the next message fails with `EMSGSIZE` and leaves the message queued. |
| F5 | Blocking by default. `O_NONBLOCK` returns `EAGAIN` instead of sleeping. |
| F6 | Sleeping callers can be interrupted by signals (`EINTR`, or a transparent restart under `SA_RESTART`). |
| F7 | `poll`/`select`/`epoll` report readable/writable state accurately. |
| F8 | `ioctl` exposes info, statistics, flush, stats reset and peek-length. |
| F9 | Many readers and writers may use one device at the same time. |

**Non-functional**

| ID | Requirement |
|----|-------------|
| N1 | No data corruption, loss or duplication under any interleaving. |
| N2 | A failed user copy (`EFAULT`) leaves the queue unchanged. |
| N3 | Bounded memory: one fixed ring per device, allocated at load time. No allocation on the I/O path. |
| N4 | Clean load-failure unwinding; unload is impossible while files are open. |
| N5 | The queue algorithm can be tested without a kernel. |
| N6 | Builds on kernels on both sides of API changes it relies on (`class_create`, 6.4). |

## 2. Component overview

```
 user space                                 kernel space
 ───────────                                ─────────────────────────────────────────
                                            kmsgq.ko
 app ── read/write/poll/ioctl ──► VFS ──►   ┌──────────────────────────────────────┐
                                            │ kmsgq_main.c                         │
                                            │  file_operations: open release read  │
                                            │  write poll unlocked_ioctl compat    │
                                            │  per-device: mutex, 2 wait queues,   │
                                            │  stats, lock-free state mirrors      │
                                            │           │  (callbacks wrap         │
                                            │           ▼   copy_{to,from}_user)   │
                                            │ kmsgq_ring.c   (no kernel deps)      │
                                            │  length-prefixed byte ring           │
                                            └──────────────────────────────────────┘
 tests/unit/test_ring.c ── links kmsgq_ring.c directly (same source, user space)
```

`include/uapi/kmsgq.h` is the only header shared with applications.

## 3. Ring data layout

Each device owns one contiguous buffer of `capacity` bytes, allocated with
`kvzalloc()` so large rings can come from vmalloc space. Messages are packed
back to back as `[u32 length][payload]` with no padding or alignment:

```
 0                                                         cap
 ├────────┬──────────┬────────┬───────────────┬───────────┤
 │ len=3  │ "abc"    │ len=10 │ "0123456789"  │   free    │
 ├────────┴──────────┴────────┴───────────────┴───────────┤
 ▲ tail                                       ▲ head
```

- `head` is where the next header is written, `tail` is the oldest header.
- `used` counts bytes including headers, and `count` counts messages.
  `head == tail` is ambiguous (empty or full), so the ring tracks `used`
  explicitly and never infers fullness from the offsets.
- A header or a payload may wrap past `cap` and continue at offset 0. Every
  access goes through helpers that split a range into at most two segments.
- When the ring drains to empty it resets `head = tail = 0`. This is cheap
  and makes later messages less likely to wrap.

**Why a byte ring and not a list of `kmalloc`'d messages?** A byte ring gives
a hard memory bound (N3), needs no allocation on the I/O path, and stores
small messages densely. The cost is wrap-around handling, which is confined
to about 30 lines and tested at every split offset.

**Why u32 length headers?** Four bytes per message is small. The ring refuses
capacities whose payload limit would not fit in a u32.

### Transfer callbacks and atomicity (N2, N5)

The ring never touches the caller's buffer directly. `push` and `pop` take a
`kmsgq_xfer_fn(ctx, ring_ptr, ext_off, len)` callback:

- In the kernel, the callbacks wrap `copy_from_user()` and `copy_to_user()`.
  The user pointer travels inside a typed context struct, so no `__user`
  address-space casts are needed and sparse stays quiet.
- In tests, they are `memcpy()`, or a version that fails on its Nth call
  to inject faults.

Both operations do the payload transfer first and commit
`head`/`tail`/`used`/`count` only after it succeeds. `push` writes the
payload into free space, then the header. `pop` copies out, then advances
`tail`. A fault part-way through leaves the ring exactly as it was. The
fault-injection unit tests and the `EFAULT` integration test check this.

## 4. Concurrency design

### Locks

Each device has one `struct mutex`. It protects the ring and the stats.

A mutex, not a spinlock, because `copy_to_user()` and `copy_from_user()` can
fault and sleep, and they run while the lock is held. Holding the lock across
the copy is what makes the copy and the commit a single atomic step.

Holding a mutex across a user copy can deadlock if the page fault re-enters
the driver, for example through an `mmap` of the same device. kmsgq has no
`mmap`, so a fault cannot re-enter it.

There is one lock per device and no global lock on the I/O path, so separate
devices never contend.

### Waiting without the lock

Readers wait on `readq` for data, and writers wait on `writeq` for space. The
wait condition of `wait_event_interruptible()` runs after the task has set
itself `TASK_INTERRUPTIBLE`, so it must not sleep. In particular it cannot
take the mutex. The same applies to `poll`.

The device therefore keeps two **mirrors**, `snap_count` and `snap_free`.
They are written with `WRITE_ONCE()` under the mutex at every state change,
and read with `READ_ONCE()` by wait conditions and `poll`. The mirrors are
only hints. Every sleeper re-takes the mutex and re-checks the real ring
before acting:

```
read():                                 write():
  loop:                                   lock
    lock_interruptible                    push (copy_from_user)
    if ring non-empty: break              publish mirrors
    unlock                                unlock
    if O_NONBLOCK: return -EAGAIN         wake_up(readq)  ───────┐
    wait_event_interruptible(readq,                              │
         READ_ONCE(snap_count) > 0) ◄────────────────────────────┘
  pop (copy_to_user)
  publish mirrors
  unlock
  wake_up(writeq)
```

**No lost wake-ups.** The writer updates the mirror before calling
`wake_up()`. The reader's `wait_event` adds itself to the queue and sets its
task state before re-evaluating the condition. The wait-queue lock and the
barriers in `set_current_state()` and `try_to_wake_up()` order these
operations. So either the reader sees the new mirror value, or it is
already on the queue when the wake-up arrives.

**Wake-up keys.** Wake-ups use `wake_up_interruptible_poll()` with
`EPOLLIN` or `EPOLLOUT` keys, so epoll waiters only wake for the event they
asked about.

**Non-exclusive waits (a deliberate trade-off).** All waiters are
non-exclusive, so one message wakes every blocked reader, and all but one go
back to sleep. Exclusive waits would avoid that "thundering herd", but they
carry a classic hazard. If the single woken reader is interrupted by a signal
before it consumes the message, no other reader is woken, and the message
sits until the next write. Correctness wins here. Section 7 discusses ways
to remove the herd without that hazard.

### poll semantics

- `EPOLLIN | EPOLLRDNORM` when at least one message is queued.
- `EPOLLOUT | EPOLLWRNORM` only when `free >= max_msg + header`, i.e. when a
  write of *any* legal size is guaranteed not to block. An event loop that
  sees `EPOLLOUT` therefore never gets `EAGAIN`. The cost is that a writer of
  small messages may see "not writable" while a small write would still
  succeed. That is documented behaviour, and the reason is that the driver
  cannot know the size of the next write.

### Signals

Every sleep is interruptible: the mutex acquisition, `wait_event`, and the
ioctl lock paths. They return `-ERESTARTSYS`. Under `SA_RESTART` the kernel
restarts the syscall transparently. Otherwise the caller sees `EINTR`, which
the integration suite tests with a `SIGALRM` timer.

## 5. Lifetime

**Load** (`kmsgq_init`):

1. Validate the module parameters.
2. `alloc_chrdev_region`.
3. `class_create`.
4. `kcalloc` the device array.
5. For each device: `kvzalloc` the ring, `cdev_add`, then `device_create`.

`cdev_add` happens *before* `device_create`, so the `/dev` node only appears
once `open()` already works. Any failure unwinds in reverse through `goto`
labels, including the devices already set up.

**Open files pin the module.** `fops.owner = THIS_MODULE` makes every open
file hold a module reference, so `rmmod` fails with `EBUSY` until the last
descriptor closes. `kmsgq_exit` therefore never races with I/O, and it frees
the device array without extra reference counting.

**Positionless files.** `open()` calls `stream_open()`, so `lseek`, `pread`
and `pwrite` fail with `ESPIPE`. A queue has no file position.

## 6. Error model

| Situation | Result |
|-----------|--------|
| `write` of 0 bytes / `read` with 0-byte buffer | returns 0, no state change |
| `write` larger than `max_msg` | `EMSGSIZE` |
| `read` buffer smaller than next message | `EMSGSIZE`, message stays queued |
| queue full / empty with `O_NONBLOCK` | `EAGAIN` |
| signal while sleeping | `EINTR` (or restart under `SA_RESTART`) |
| bad user pointer | `EFAULT`, queue unchanged |
| unknown ioctl | `ENOTTY` |
| `lseek`/`pread`/`pwrite` | `ESPIPE` |
| bad module parameter | load fails with `EINVAL`, reason in the kernel log |

**Logging:**

- `pr_fmt` prefixes every message with `kmsgq:`.
- Load/unload and setup failures use `pr_info`/`pr_err`.
- Per-operation tracing (open/release, failed reads and writes, flushes) uses
  `dev_dbg`. Under dynamic debug it is off by default and costs nearly
  nothing. Enable it with:

  ```
  echo 'module kmsgq +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
  ```

## 7. Limitations and possible extensions

- **Thundering herd** with many blocked readers (see §4). A fix that keeps
  correctness: exclusive waits, plus passing the wake-up on when a woken
  waiter leaves without consuming.
- **Single lock per device.** Fine for a message queue, where every operation
  touches `head` or `tail` anyway. A lock-free SPSC fast path is possible but
  would complicate the multi-reader semantics.
- **Copy cost.** Every message is copied twice (user → ring → user). An
  `mmap`-ed shared ring (io_uring style) would remove both copies, but brings
  back the fault-under-lock problem described in §4.
- **Fixed size.** Ring size and `max_msg` are load-time parameters. A
  resize ioctl would need to re-pack live messages into a new buffer under
  the lock.
- **Out-of-tree only.** The driver has not been run against the upstream
  `checkpatch.pl`.
