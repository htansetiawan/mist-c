#!/bin/bash
# mist-c: compile the referee front end. The course runtime lists g++ 12.2.0;
# C11 with gcc or cc when present, else g++'s C front end, else as C++.
set -uo pipefail
cd "$(dirname "$0")"
[ -f bin/stockfish ] && chmod +x bin/stockfish
SOURCES="src/board.c src/belief.c src/movemodel.c src/cfr.c src/eval.c src/search.c src/gtcfr.c src/front.c src/sfeval.c src/main.c"
FLAGS="-O2 -pthread -D_POSIX_C_SOURCE=199309L"
for CC in gcc cc; do
    if command -v "$CC" >/dev/null 2>&1 && "$CC" -std=c11 $FLAGS $SOURCES -lm -o mist; then echo "build: done ($CC)"; exit 0; fi
done
# g++'s C front end: one file per call, since -x c covers only the first of
# several sources on g++ 12
if (rm -rf build-xc && mkdir build-xc && for f in $SOURCES; do g++ -x c -std=c11 $FLAGS -c "$f" -o "build-xc/$(basename "$f" .c).o" || exit 1; done && g++ $FLAGS build-xc/*.o -lm -o mist) 2>/dev/null; then echo "build: done (g++ -x c)"; exit 0; fi
if g++ -x c++ -std=c++20 -fpermissive -w $FLAGS $SOURCES -lm -o mist; then echo "build: done (g++ as C++)"; exit 0; fi
echo "build: failed" >&2
exit 1
