#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Load kmsgq.ko and wait for its device nodes.
#   sudo scripts/load.sh [module params...]
#   sudo scripts/load.sh nr_devs=2 capacity=8192 max_msg=512
#
# Set KMSGQ_MODE (e.g. 0666) to change node permissions for non-root testing.
set -euo pipefail

cd "$(dirname "$0")/.."
KO=${KO:-./kmsgq.ko}

[[ $EUID -eq 0 ]] || { echo "load.sh: must run as root" >&2; exit 1; }
[[ -f $KO ]] || { echo "load.sh: $KO not found; run 'make' first" >&2; exit 1; }

if lsmod | grep -q '^kmsgq '; then
	rmmod kmsgq
fi
insmod "$KO" "$@"

# udev creates the nodes asynchronously.
for _ in $(seq 50); do
	ls /dev/kmsgq0 >/dev/null 2>&1 && break
	sleep 0.1
done
[[ -c /dev/kmsgq0 ]] || { echo "load.sh: /dev/kmsgq0 did not appear" >&2; exit 1; }

if [[ -n ${KMSGQ_MODE:-} ]]; then
	chmod "$KMSGQ_MODE" /dev/kmsgq*
fi
ls -l /dev/kmsgq*
dmesg | grep 'kmsgq:' | tail -n 1
