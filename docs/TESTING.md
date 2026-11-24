# Testing kmsgq

## Test layers

| Layer | What | Needs kernel? | Command |
|-------|------|---------------|---------|
| Unit | Ring core (`driver/kmsgq_ring.c`) compiled into a user-space test binary: boundaries, wrap-around at every split offset, fault injection on each transfer segment, corruption detection, and a randomized comparison against a reference FIFO (200k ops × 7 ring sizes) | no | `make unit` |
| Memory safety | Unit tests under ASan+UBSan, then valgrind memcheck | no | `make check` |
| Static analysis | clang-tidy on user-space code. W=1 and sparse (`__user` address-space checking) on the module | sparse/W=1: headers only | CI, or `make sparse` |
| Integration | 22 cases against the loaded module: semantics, errno contract, poll/epoll, blocking wake-ups, signals, `EFAULT` atomicity, stats, multi-device, MPSC/MPMC stress, timed chaos | yes, loaded module | `sudo scripts/run-integration.sh` |
| Integration + TSan | Same suite built with ThreadSanitizer, to check the test's own threading | yes | CI |
| Kernel log check | `run-integration.sh` fails if the kernel logged `BUG`/`WARNING`/`Oops`/lockdep/KASAN/UBSAN output after its start marker | yes | included above |
| Benchmarks | `bench_ring` (ring core only), `bench_dev` (SPSC throughput, ping-pong RTT percentiles) | ring: no, dev: yes | `make bench` |

## How the unit tests were checked

Two deliberate bugs were planted in the ring, one at a time. Both made the
suite fail:

1. Off-by-one in the wrap check (`>=` → `>`): 4 tests failed.
2. `pop` advancing `tail` even when the copy failed: the fault-injection
   test failed.

## Verification status

What has actually been run, and where:

| Check | Development sandbox (no module support) | GitHub Actions: Ubuntu 22.04, Linux 6.8 (azure) | GitHub Actions: Ubuntu 24.04, Linux 7.0 (azure, PREEMPT) | Your WSL2 machine |
|-------|------|------|------|------|
| Unit tests, gcc and clang | ✅ | — | ✅ | not yet |
| Unit tests, ASan+UBSan | ✅ gcc | — | ✅ gcc + clang | not yet |
| valgrind | ✅ | — | ✅ | not yet |
| clang-tidy | ✅ | — | ✅ | — |
| Module build with `-Werror` against real headers | ❌ no headers | ✅ | ✅ | not yet |
| `W=1` and sparse, zero findings in our sources | ❌ | ✅ | ✅ | — |
| Module load and unload | ❌ | ✅ | ✅ | not yet |
| Integration suite (22 cases) | ❌ | ✅ 22/22 | ✅ 22/22 | not yet |
| Integration suite under ThreadSanitizer | ❌ | ✅ 22/22 | ✅ 22/22 | — |
| No kernel BUG/WARNING/Oops after the test's start marker | ❌ | ✅ | ✅ | not yet |

The first CI run passed every module and integration step on both kernels.
The only failure was ThreadSanitizer reporting a data race in the test
program itself: a shared static buffer in `dev_path()`. That was fixed in a
follow-up commit, and every job has passed since.

### Benchmark snapshot (CI, quick mode)

These are single `bench_dev --quick` runs on shared GitHub-hosted VMs,
20,000 messages per size. Use them as rough reference points, not as a
characterisation of the driver.

| Message size | Linux 6.8 msgs/s | Linux 7.0 msgs/s |
|---|---|---|
| 16 B | 2.00 M | 2.18 M |
| 256 B | 2.11 M | 2.42 M |
| 4096 B | 1.85 M (7.6 GB/s) | 2.06 M (8.5 GB/s) |
| 32 B ping-pong RTT p50 / p99 | 14.7 / 20.6 µs | 13.0 / 18.6 µs |

## Running individual cases

Both test binaries take a substring filter:

```bash
./tests/build/test_ring wrap            # unit tests whose name contains "wrap"
sudo ./tests/build/test_kmsgq stress    # just the stress tests
sudo KMSGQ_STRESS_N=100000 ./tests/build/test_kmsgq stress_mpmc
```

`test_kmsgq` exits with 77 (the conventional "skipped" status) when
`/dev/kmsgq0` is missing or inaccessible.

## Kernel debug options worth enabling on a test kernel

If you build your own kernel (for example on WSL2), turning these on makes the
integration suite much stronger, since the kernel then checks the driver's
locking and memory accesses at runtime:

- `CONFIG_PROVE_LOCKING`: lockdep, which validates mutex usage.
- `CONFIG_DEBUG_ATOMIC_SLEEP`: catches sleeping in atomic context.
- `CONFIG_KASAN`: out-of-bounds and use-after-free.
- `CONFIG_KCSAN`: data races. This would check the `READ_ONCE`/`WRITE_ONCE`
  mirror design.
- `CONFIG_DYNAMIC_DEBUG`: enables the driver's `dev_dbg` tracing.
