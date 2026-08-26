# SPDX-License-Identifier: GPL-2.0

obj-m += lockless_nic.o
lockless_nic-y := lockless_nic_main.o lockless_ring_buffer.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all modules clean check format

all modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

check:
	@if command -v sparse >/dev/null 2>&1; then \
		$(MAKE) -C $(KDIR) M=$(PWD) C=1 CF='-D__CHECK_ENDIAN__' modules; \
	else \
		echo 'sparse is not installed; skipping static analysis'; \
	fi

format:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i lockless_nic.h lockless_nic_main.c lockless_ring_buffer.c; \
	else \
		echo 'clang-format is not installed; skipping formatting'; \
	fi
