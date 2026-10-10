CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
SRC = src/board.c src/belief.c src/cfr.c

build/test_rules: $(SRC) tests/test_rules.c src/fow.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_rules.c -o $@

build/test_belief: $(SRC) tests/test_belief.c src/fow.h src/belief.h
	mkdir -p build && $(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L $(SRC) tests/test_belief.c -o $@

build/test_cfr: src/cfr.c src/cfr.h tests/test_cfr.c
	mkdir -p build && $(CC) $(CFLAGS) src/cfr.c tests/test_cfr.c -lm -o $@

test: build/test_rules build/test_belief build/test_cfr
	./build/test_rules tests/vectors.txt
	./build/test_belief tests/traces.txt 100000
	./build/test_cfr

clean:
	rm -rf build
.PHONY: test clean
