#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Full on-target test: build, load, run the integration suite and a short
# benchmark, unload, then scan the kernel log for warnings.
#   sudo scripts/run-integration.sh
#
# Exits non-zero if any step fails or the kernel logged a BUG/WARNING/Oops.
set -euo pipefail

cd "$(dirname "$0")/.."
[[ $EUID -eq 0 ]] || { echo "run-integration.sh: must run as root" >&2; exit 1; }

MARK="kmsgq-integration-$$-$(date +%s)"
echo "$MARK" > /dev/kmsg

make -s module ${CC:+CC="$CC"}
make -s -C tests build/test_kmsgq build/bench_dev
scripts/load.sh nr_devs=4 capacity=65536 max_msg=4096

status=0
./tests/build/test_kmsgq || status=$?
./tests/build/bench_dev --quick || status=$?

scripts/unload.sh || status=$?

# Only look at log lines written after our marker.
log=$(dmesg | sed -n "/$MARK/,\$p")
if grep -Eq 'BUG:|WARNING:|Oops|general protection|lockdep|KASAN|UBSAN' <<<"$log"; then
	echo "run-integration.sh: kernel reported problems:" >&2
	grep -E 'BUG:|WARNING:|Oops|general protection|lockdep|KASAN|UBSAN' <<<"$log" >&2
	status=1
fi
exit "$status"
