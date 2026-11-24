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

| Check | Development sandbox (Ubuntu 24.04 userland, no module support) | GitHub Actions | Your WSL2 machine |
|-------|------|------|------|
| Unit tests, gcc / clang | ✅ passed | configured | — |
| Unit tests, ASan+UBSan (gcc) | ✅ passed | configured (gcc + clang) | — |
| valgrind | ✅ passed | configured | — |
| clang-tidy | ✅ clean | configured | — |
| Integration test and benchmarks **compile** (gcc, clang) | ✅ | configured | — |
| Module **type-checks** against hand-written API stubs (both `class_create` variants) | ✅ (stubs are not kernel headers; this only catches typos and type errors) | — | — |
| Module builds against real kernel headers | ❌ not possible here | configured, **not yet run** | see [WSL2.md](WSL2.md) |
| sparse, W=1 | ❌ | configured, **not yet run** | optional |
| Module load, integration suite, kernel log check | ❌ | configured, **not yet run** | see [WSL2.md](WSL2.md) |

Update this table after the first CI run and after your first WSL2 run.

### Known CI risk

The kernel job assumes GitHub's hosted Ubuntu runners let `insmod` load an
unsigned out-of-tree module. If the runner kernel enforces module signature
verification, the load step fails with `Key was rejected by service`. The
build, W=1 and sparse steps still give a useful signal in that case.

The fallback is to boot a throwaway VM inside CI with
[virtme-ng](https://github.com/arighi/virtme-ng). Hosted Linux runners expose
`/dev/kvm`, which virtme-ng can usually use.

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
