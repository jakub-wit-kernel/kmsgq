# kmsgq: message-queue character devices for Linux

[![CI](https://github.com/jakub-wit-kernel/kmsgq/actions/workflows/ci.yml/badge.svg)](https://github.com/jakub-wit-kernel/kmsgq/actions/workflows/ci.yml)
![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue)

`kmsgq` is a loadable Linux kernel module that creates `/dev/kmsgq0..N`.
Each device is a bounded, in-kernel **message** queue. Unlike a pipe, it
preserves message boundaries: one `write(2)` is one message, and one
`read(2)` returns exactly one whole message.

It is a compact but complete example of the parts of a character driver
that are easy to get subtly wrong:

- blocking and non-blocking I/O
- `poll`/`epoll` readiness
- signal-safe sleeping
- a stable `ioctl` ABI
- per-device locking with no lost wake-ups
- `EFAULT`-atomic user copies
- clean load and unload error handling

## Features

- **Message semantics.** Boundaries are preserved. A short read buffer gets
  `EMSGSIZE` and the message stays queued. `KMSGQ_IOC_PEEK_LEN` reports the
  size of the next message.
- **Blocking and `O_NONBLOCK`**, with pipe-like semantics. Every sleep is
  interruptible (`EINTR`, or a restart under `SA_RESTART`).
- **`poll`/`select`/`epoll`.**
  - `EPOLLIN`: a message is queued.
  - `EPOLLOUT`: a write of *any* legal size will not block.
- **ioctl interface:** queue info, traffic statistics (including how often
  callers had to sleep, and the high-water mark), flush, and stats reset.
  Fixed-width structs work for 32-bit callers through `compat_ptr_ioctl`.
- **Concurrency.** Any number of readers and writers per device, with one
  mutex per device. Wait conditions and `poll` read lock-free mirrors of the
  queue state, so they never sleep on the lock.
- **Bounded memory.** One preallocated ring per device, and no allocation
  on the I/O path.
- **Kernel-independent core.** The ring buffer builds unchanged in user
  space, so its logic is unit-tested with sanitizers and fault injection.
- **Logging.** Load/unload and errors go through `pr_*`; per-operation
  tracing uses `dev_dbg` under dynamic debug.

## Quick start

```bash
# Build (needs headers for the running kernel; on WSL2 see docs/WSL2.md)
sudo apt install build-essential linux-headers-$(uname -r)
make

# Load four devices with a 64 KiB ring each, run the full on-target test
sudo scripts/run-integration.sh

# Or explore by hand
sudo scripts/load.sh nr_devs=2 capacity=8192 max_msg=512
sudo sh -c 'printf hello > /dev/kmsgq0; printf world > /dev/kmsgq0'
sudo dd if=/dev/kmsgq0 bs=64 count=1 status=none; echo    # hello
sudo dd if=/dev/kmsgq0 bs=64 count=1 status=none; echo    # world
sudo scripts/unload.sh
```

User-space tests need no kernel module at all:

```bash
make check        # ring unit tests under ASan+UBSan, then valgrind
```

## Using it from C

```c
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "kmsgq.h"          /* include/uapi/kmsgq.h */

int main(void)
{
	int fd = open("/dev/kmsgq0", O_RDWR);
	struct kmsgq_info info;
	char buf[4096];
	ssize_t n;

	if (fd < 0 || ioctl(fd, KMSGQ_IOC_GET_INFO, &info) < 0)
		return perror("kmsgq"), 1;
	printf("capacity %llu, max message %llu bytes\n",
	       (unsigned long long)info.capacity,
	       (unsigned long long)info.max_msg);

	write(fd, "first", 5);
	write(fd, "second", 6);
	n = read(fd, buf, sizeof(buf));     /* 5: "first", never "firstsecond" */
	printf("got %zd bytes: %.*s\n", n, (int)n, buf);
	close(fd);
	return 0;
}
```

### Module parameters

| Parameter | Default | Range | Meaning |
|-----------|---------|-------|---------|
| `nr_devs` | 4 | 1-64 | number of devices |
| `capacity` | 65536 | 64 B - 16 MiB | ring size per device, including 4-byte per-message headers |
| `max_msg` | 4096 | 1 - (capacity − 4) | largest accepted message |

Current values are readable at `/sys/module/kmsgq/parameters/`.

### ioctl reference

| Command | Argument | Effect |
|---------|----------|--------|
| `KMSGQ_IOC_GET_INFO` | `struct kmsgq_info *` | API version, minor, capacity, max_msg, header size, current fill |
| `KMSGQ_IOC_GET_STATS` | `struct kmsgq_stats *` | message/byte counters, sleep counts, high-water mark |
| `KMSGQ_IOC_FLUSH` | none | discard all messages, wake blocked writers |
| `KMSGQ_IOC_RESET_STATS` | none | zero the counters |
| `KMSGQ_IOC_PEEK_LEN` | `__u32 *` | length of the next message (0 if empty) |

## Layout

```
driver/kmsgq_main.c       file operations, ioctl, poll, locking, module init/exit
driver/kmsgq_ring.[ch]    length-prefixed ring buffer (no kernel dependencies)
include/uapi/kmsgq.h      user-space ABI
tests/unit/               ring unit tests (run without a kernel)
tests/integration/        22-case device test suite incl. stress and chaos runs
tests/common/kt.h         minimal TAP test harness
bench/                    ring microbenchmark, device throughput/latency benchmark
scripts/                  load, unload, full on-target test, kernel compiler detection
docs/                     architecture, testing, WSL2 build guide
```

## Testing

[docs/TESTING.md](docs/TESTING.md) has the full matrix and records exactly
what has been run where. In short:

- **Unit:** the ring core is tested in user space for wrap-around at every
  split offset, fault injection on each copy segment (state must be
  unchanged), and a randomized comparison against a reference FIFO. The
  tests run under ASan, UBSan and valgrind with gcc and clang.
- **Integration:** 22 cases against the loaded module, covering the errno
  contract, poll and epoll, blocking wake-ups in both directions, `EINTR`,
  `EFAULT` atomicity, multi-producer/multi-consumer exactly-once delivery,
  per-producer FIFO order, and a timed chaos run. Any kernel
  `BUG`/`WARNING`/lockdep output during the run fails the test.
- **Static analysis:** sparse and `W=1` on the module, clang-tidy on
  user-space code.

## Benchmarks

```bash
make bench
./tests/build/bench_ring                 # ring core alone, no kernel
sudo ./tests/build/bench_dev             # via the device: SPSC throughput, ping-pong RTT
```

`bench_ring` isolates the data-structure cost. `bench_dev` adds syscall
entry, `copy_{to,from}_user`, locking and scheduler wake-ups, so the gap
between the two shows where time goes. Device numbers depend heavily on the
CPU, kernel configuration and mitigations, so compare runs on one machine
only.

## Design

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the requirements, ring
layout, why the lock is a mutex, how waits avoid lost wake-ups without
taking the lock, `poll` semantics, module lifetime, the error model and
known limitations.

## License

GPL-2.0-only (see [LICENSE](LICENSE)). The UAPI header carries
`GPL-2.0 WITH Linux-syscall-note`, the same exception the kernel uses for
its own user-space headers.
