#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Print the gcc that built the running kernel (e.g. "gcc-12"), so out-of-tree
# modules can be built with a matching compiler:
#   make CC="$(scripts/kernel-cc.sh)"
#
# Distribution kernels are often built with a newer gcc than the distro
# default; a mismatch can make kbuild reject flags it does not recognise.
# Falls back to plain "gcc" when /proc/version names no versioned binary
# (for example the WSL2 kernel).
set -euo pipefail

version=$(cat /proc/version)
if [[ $version =~ gcc-([0-9]+) ]]; then
	echo "gcc-${BASH_REMATCH[1]}"
else
	echo gcc
fi
