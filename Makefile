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

# M4 (Astra): paper-only gadgets, knowledge closure, and blueprint carry-over.
M4_SRC = src/cfr.c src/gadget.c src/kluss.c
M4_HDR = src/cfr.h src/gadget.h src/kluss.h src/m4policy.h
build/test_gadget: $(M4_SRC) $(M4_HDR) tests/test_gadget.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) tests/test_gadget.c -lm -o $@
build/test_kluss: $(M4_SRC) $(M4_HDR) tests/test_kluss.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) tests/test_kluss.c -lm -o $@
build/test_gadget_kuhn: $(M4_SRC) $(M4_HDR) tests/test_gadget_kuhn.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) tests/test_gadget_kuhn.c -lm -o $@
build/test_gadget_controller: $(M4_SRC) $(M4_HDR) tests/test_gadget_controller.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) tests/test_gadget_controller.c -lm -o $@
build/test_kluss_prepare: $(M4_SRC) $(M4_HDR) tests/test_kluss_prepare.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) tests/test_kluss_prepare.c -lm -o $@
build/test_kluss_carry: $(M4_SRC) $(M4_HDR) tests/test_kluss_carry.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) tests/test_kluss_carry.c -lm -o $@
test_m4: build/test_gadget build/test_kluss build/test_gadget_kuhn build/test_gadget_controller build/test_kluss_prepare build/test_kluss_carry
	./build/test_gadget
	./build/test_kluss
	./build/test_gadget_kuhn
	./build/test_gadget_controller
	./build/test_kluss_prepare
	./build/test_kluss_carry
test: test_m4
.PHONY: test_m4

# Experimental standalone M4 adapter; active build/mist stays unchanged.
M4_CHESS = $(SRC) src/gadget.c src/kluss.c src/m4sample.c src/m4frontier.c src/m4policy.c src/m4search.c src/m4sf.c
build/test_m4_chess: $(M4_CHESS) $(M4_HDR) src/m4sample.h src/m4frontier.h src/m4search.h src/m4sf.h tests/test_m4_chess.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_CHESS) tests/test_m4_chess.c -lm -o $@
build/mist-m4: $(M4_CHESS) src/m4main.c $(M4_HDR) src/m4sample.h src/m4frontier.h src/m4search.h src/m4sf.h
	mkdir -p build && $(CC) $(CFLAGS) $(M4_CHESS) src/m4main.c -lm -o $@
build/test_m4_sample: $(M4_CHESS) $(M4_HDR) src/m4sample.h src/m4frontier.h src/m4search.h src/m4sf.h tests/test_m4_sample.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_CHESS) tests/test_m4_sample.c -lm -o $@
test_m4_chess: build/test_m4_chess build/test_m4_sample
	./build/test_m4_chess
	./build/test_m4_sample
.PHONY: test_m4_chess
test_m4: test_m4_chess

build/fake_m4_uci: tests/fake_m4_uci.c
	mkdir -p build && $(CC) $(CFLAGS) $< -o $@
build/test_m4_sf: $(SRC) src/m4sf.c src/m4sf.h tests/test_m4_sf.c
	mkdir -p build && $(CC) $(CFLAGS) $(SRC) src/m4sf.c tests/test_m4_sf.c -lm -o $@
test_m4_sf: build/fake_m4_uci build/test_m4_sf
	./build/test_m4_sf ./build/fake_m4_uci
.PHONY: test_m4_sf
test_m4: test_m4_sf
build/test_m4_eval: $(M4_CHESS) $(M4_HDR) src/m4search.h src/m4sf.h tests/test_m4_eval.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_CHESS) tests/test_m4_eval.c -lm -o $@
test_m4_eval: build/test_m4_eval
	./build/test_m4_eval
.PHONY: test_m4_eval
test_m4: test_m4_eval

# Offline information-conflict primitive; not linked into the frozen engine.
build/test_m4_escape: src/board.c src/m4escape.c src/m4escape.h src/fow.h tests/test_m4_escape.c
	mkdir -p build && $(CC) $(CFLAGS) src/board.c src/m4escape.c tests/test_m4_escape.c -o $@
build/escape_probe: src/board.c src/m4escape.c src/m4escape.h src/fow.h tools/escape_probe.c
	mkdir -p build && $(CC) $(CFLAGS) src/board.c src/m4escape.c tools/escape_probe.c -o $@
test_m4_escape: build/test_m4_escape
	./build/test_m4_escape
.PHONY: test_m4_escape
test_m4: test_m4_escape

# Bounded tactical scheduling (disabled unless explicitly enabled).
build/test_m4_frontier: src/m4frontier.c src/m4frontier.h tests/test_m4_frontier.c
	mkdir -p build && $(CC) $(CFLAGS) src/m4frontier.c tests/test_m4_frontier.c -o $@
test_m4_frontier: build/test_m4_frontier
	./build/test_m4_frontier
.PHONY: test_m4_frontier
test_m4: test_m4_frontier

build/test_m4_scale: $(M4_CHESS) $(M4_HDR) src/m4search.h tests/test_m4_scale.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_CHESS) tests/test_m4_scale.c -lm -o $@
test_m4_scale: build/test_m4_scale
	./build/test_m4_scale
.PHONY: test_m4_scale
test_m4: test_m4_scale

build/test_m4_policy: $(M4_SRC) $(M4_HDR) src/m4policy.c src/m4policy.h tests/test_m4_policy.c
	mkdir -p build && $(CC) $(CFLAGS) $(M4_SRC) src/m4policy.c tests/test_m4_policy.c -lm -o $@
test_m4_policy: build/test_m4_policy
	./build/test_m4_policy
.PHONY: test_m4_policy
test_m4: test_m4_policy
