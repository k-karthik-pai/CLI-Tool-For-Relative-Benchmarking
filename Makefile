CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L -O2

TESTCASE_DIR := testcases
TESTCASE_SOURCES := $(wildcard $(TESTCASE_DIR)/*.c)
TESTCASE_BINARIES := $(TESTCASE_SOURCES:.c=)

.PHONY: all testcases clean

all: perfcmp testcases

perfcmp: perfcmp.c
	$(CC) $(CFLAGS) $< -o $@

testcases: $(TESTCASE_BINARIES)

$(TESTCASE_DIR)/%: $(TESTCASE_DIR)/%.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f perfcmp $(TESTCASE_BINARIES) .perfcmp_perf_*.log .perf_log*.txt
