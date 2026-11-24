#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Unload kmsgq. Fails (EBUSY) while any /dev/kmsgq* file is still open.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "unload.sh: must run as root" >&2; exit 1; }
if lsmod | grep -q '^kmsgq '; then
	rmmod kmsgq
fi
dmesg | grep 'kmsgq:' | tail -n 1
