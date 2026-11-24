# SPDX-License-Identifier: GPL-2.0
#
# kmsgq top-level build. Kernel objects are described in ./Kbuild; this file
# drives kbuild and the user-space test/benchmark builds in ./tests.
#
#   make                 build kmsgq.ko against the running kernel
#   make KDIR=/path      build against another kernel tree (e.g. WSL2 source)
#   make sparse          kernel build under the sparse static checker
#   make unit            ring unit tests (no kernel needed)
#   make check           unit tests under ASan+UBSan, then valgrind
#   make integration     build the device test suite (run with scripts/run-integration.sh)
#   make bench           build the benchmarks
#   make clean
#
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(CURDIR)

.PHONY: all module sparse unit check integration bench clean

all: module

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# C=2 forces sparse on every file, not just rebuilt ones.
sparse:
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 CF="-D__CHECK_ENDIAN__" modules

unit:
	$(MAKE) -C tests unit

check:
	$(MAKE) -C tests clean
	$(MAKE) -C tests unit SAN=address,undefined
	$(MAKE) -C tests clean
	$(MAKE) -C tests valgrind OPT=-O1

integration:
	$(MAKE) -C tests integration

bench:
	$(MAKE) -C tests bench

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null
	$(MAKE) -C tests clean
	rm -f driver/*.o driver/.*.cmd .*.cmd *.ko *.mod *.mod.c *.o \
	      modules.order Module.symvers
