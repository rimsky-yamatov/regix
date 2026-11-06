CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
B      := build

RGC_SRC := $(wildcard rgc/*.c) $(wildcard common/*.c)

all: $(B)/rgc $(B)/rgfmt $(B)/rgpm $(B)/run_tests

$(B):
	mkdir -p $(B)

$(B)/rgc: $(RGC_SRC) $(wildcard rgc/*.h common/*.h) | $(B)
	$(CC) $(CFLAGS) -Icommon -Irgc -o $@ $(RGC_SRC)

$(B)/rgfmt: rgfmt/main.c common/util.c $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) -Icommon -o $@ rgfmt/main.c common/util.c

$(B)/rgpm: rgpm/main.c $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) -Icommon -o $@ rgpm/main.c

$(B)/run_tests: tests/run_tests.c common/util.c $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) -Icommon -o $@ tests/run_tests.c common/util.c

test: all
	$(B)/run_tests $(B)/rgc tests/cases $(B)/rgfmt tests/fmt

clean:
	rm -rf $(B)

.PHONY: all test clean
