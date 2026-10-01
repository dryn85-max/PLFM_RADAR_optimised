# Host unit tests. Run from g0b1/ as `make -C tests` (ADAR_COUNT=1 by default).
# The top-level `make test` runs this twice: ADAR_COUNT=1 and ADAR_COUNT=4.
ADAR_COUNT ?= 1
CC        ?= gcc
CFLAGS    := -std=c11 -Wall -Wextra -Werror -g -O0 \
             -I../Core/hal -I../Core/drivers -I../Core/app -Imocks \
             -DHOST_TEST -DADAR_COUNT=$(ADAR_COUNT)
BUILD     := build_n$(ADAR_COUNT)

TESTS     := $(patsubst %.c,%,$(wildcard test_*.c))
MOCKS     := $(wildcard mocks/*.c) ../Core/hal/hal_spi_common.c
# Modules under test (empty until the drivers/app tasks add them).
MODULES   := $(wildcard ../Core/drivers/*.c) $(wildcard ../Core/app/*.c)

.PHONY: all test clean check
all: test

check:
	./check_no_hal_includes.sh

test: check $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $(TESTS); do \
	    echo "== $$t (ADAR_COUNT=$(ADAR_COUNT))"; ./$(BUILD)/$$t; \
	done

# Per-test module exclusions: a test that supplies its own spy for a module
# entry point must not link the real module that defines it (nothing is weakened:
# test_fault/test_thermal keep their spies, test_sequencer links the real sequencer).
EXCL_test_fault   := ../Core/app/sequencer.c
EXCL_test_thermal := ../Core/app/sequencer.c
.SECONDEXPANSION:
$(BUILD)/%: %.c $(MOCKS) $(MODULES) $(wildcard *.h mocks/*.h ../Core/hal/*.h ../Core/drivers/*.h ../Core/app/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(MOCKS) $(filter-out $(EXCL_$*),$(MODULES))

clean:
	rm -rf build_n*
