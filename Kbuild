# SPDX-License-Identifier: GPL-2.0
obj-m := kmsgq.o
kmsgq-y := driver/kmsgq_main.o driver/kmsgq_ring.o

ccflags-y := -I$(src)/include/uapi -I$(src)/driver

# Warnings are errors by default. Set KMSGQ_WERROR=0 for exploratory W=1 builds.
KMSGQ_WERROR ?= 1
ifeq ($(KMSGQ_WERROR),1)
ccflags-y += -Werror
endif
