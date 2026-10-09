CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
SRC = src/board.c

build/test_rules: $(SRC) tests/test_rules.c src/fow.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_rules.c -o $@

test: build/test_rules
	./build/test_rules tests/vectors.txt

clean:
	rm -rf build
.PHONY: test clean
