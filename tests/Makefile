# SPDX-License-Identifier: GPL-2.0
#
# User-space builds: ring unit tests, device integration tests, benchmarks.
#
#   make unit                 build + run ring unit tests
#   make unit SAN=address     ... with AddressSanitizer (also: undefined, thread)
#   make valgrind             run unit tests under valgrind memcheck
#   make integration          build the device integration test (needs module)
#   make bench                build both benchmarks
#
ROOT     := ..
CC       ?= cc
SAN      ?=
OPT      ?= -O2
CFLAGS   ?= $(OPT) -g
WARN     := -std=gnu11 -Wall -Wextra -Werror -Wshadow -Wformat=2 \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla
INCS     := -I$(ROOT)/include/uapi -I$(ROOT)/driver -Icommon
ifneq ($(SAN),)
  CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer -fno-sanitize-recover=all
  LDFLAGS += -fsanitize=$(SAN)
endif
OUT      := build

RING_SRC := $(ROOT)/driver/kmsgq_ring.c

.PHONY: all unit valgrind integration bench clean
all: $(OUT)/test_ring $(OUT)/test_kmsgq $(OUT)/bench_ring $(OUT)/bench_dev

$(OUT):
	mkdir -p $@

# The ring core is compiled straight from the driver directory.
$(OUT)/test_ring: unit/test_ring.c $(RING_SRC) $(ROOT)/driver/kmsgq_ring.h common/kt.h | $(OUT)
	$(CC) $(CFLAGS) $(WARN) -Wno-missing-prototypes $(INCS) -o $@ unit/test_ring.c $(RING_SRC) $(LDFLAGS)

$(OUT)/test_kmsgq: integration/test_kmsgq.c $(ROOT)/include/uapi/kmsgq.h common/kt.h | $(OUT)
	$(CC) $(CFLAGS) $(WARN) -Wno-missing-prototypes $(INCS) -pthread -o $@ $< $(LDFLAGS)

$(OUT)/bench_ring: $(ROOT)/bench/bench_ring.c $(RING_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(WARN) $(INCS) -o $@ $< $(RING_SRC) $(LDFLAGS)

$(OUT)/bench_dev: $(ROOT)/bench/bench_dev.c $(ROOT)/include/uapi/kmsgq.h | $(OUT)
	$(CC) $(CFLAGS) $(WARN) $(INCS) -pthread -o $@ $< $(LDFLAGS)

unit: $(OUT)/test_ring
	./$(OUT)/test_ring

valgrind: $(OUT)/test_ring
	KMSGQ_RANDOM_OPS=20000 valgrind --error-exitcode=1 --leak-check=full -q ./$(OUT)/test_ring

integration: $(OUT)/test_kmsgq

bench: $(OUT)/bench_ring $(OUT)/bench_dev

clean:
	rm -rf $(OUT)
