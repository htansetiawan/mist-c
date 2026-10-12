CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
SRC = src/board.c src/belief.c src/movemodel.c src/cfr.c src/eval.c src/search.c src/gtcfr.c src/front.c src/sfeval.c

build/test_rules: $(SRC) tests/test_rules.c src/fow.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_rules.c -lm -o $@

build/test_belief: $(SRC) tests/test_belief.c src/fow.h src/belief.h
	mkdir -p build && $(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L $(SRC) tests/test_belief.c -lm -o $@

build/test_cfr: src/cfr.c src/cfr.h tests/test_cfr.c
	mkdir -p build && $(CC) $(CFLAGS) src/cfr.c tests/test_cfr.c -lm -o $@

build/test_search: $(SRC) tests/test_search.c src/search.h src/cfr.h src/belief.h src/fow.h src/eval.h
	mkdir -p build && $(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L $(SRC) tests/test_search.c -lm -o $@

build/test_guard: $(SRC) tests/test_guard.c src/fow.h src/belief.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_guard.c -lm -o $@

build/test_eval: $(SRC) tests/test_eval.c src/eval.h src/fow.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_eval.c -lm -o $@

build/test_sf: $(SRC) tests/test_sf.c src/sfeval.h src/fow.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_sf.c -lm -o $@

build/test_front: $(SRC) tests/test_front.c src/front.h src/belief.h src/fow.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tests/test_front.c -lm -o $@

test: build/test_rules build/test_belief build/test_cfr build/test_search build/test_guard build/test_front build/test_eval build/test_sf
	./build/test_rules tests/vectors.txt
	./build/test_guard tests/traces.txt
	./build/test_front
	./build/test_eval
	./build/test_sf
	./build/test_belief tests/traces.txt 100000
	./build/test_cfr
	./build/test_search tests/traces.txt 3 25

build/mist: $(SRC) src/main.c src/search.h src/cfr.h src/belief.h src/fow.h src/eval.h src/front.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) src/main.c -lm -o $@

clean:
	rm -rf build
.PHONY: test clean

build/history_table: $(SRC) tools/history_table.c src/belief.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tools/history_table.c -lm -o $@

build/move_features: $(SRC) tools/move_features.c src/movemodel.h
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) tools/move_features.c -lm -o $@
