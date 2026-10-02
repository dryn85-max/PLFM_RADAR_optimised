# Host tests for esp32/components/core (plain C11, no ESP-IDF). Run: make -C esp32/tests test
CC      = gcc
CORE    := ../components/core
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O1 $(SAN) -I$(CORE) -I. \
           -DVECTOR_DIR='"vectors"'
LDFLAGS := $(SAN)
BUILD   := build

TESTS   := $(patsubst %.c,%,$(wildcard test_*.c))
SRCS    := $(filter-out %/test_%.c,$(wildcard $(CORE)/*.c))

.PHONY: all test clean
all: test

test: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$(BUILD)/$$t; done

$(BUILD)/%: %.c $(SRCS) $(wildcard $(CORE)/*.h) tinytest.h vec_util.h | $(BUILD)
	$(CC) $(CFLAGS) $< $(SRCS) -o $@ $(LDFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
