#!/bin/bash
# Builds the arena ZIP: run.sh and build.sh at the root, the sources under src/.
#   tools/package.sh OUT.zip
set -euo pipefail
out=$(realpath -m "$1")
root=$(cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/src"
cp "$root"/arena/build.sh "$root"/arena/run.sh "$stage/"
cp "$root"/src/*.c "$root"/src/*.h "$stage/src/"
cp "$root"/README.md "$stage/"
(cd "$stage" && bash build.sh >/dev/null && printf 'fow\nw 180 2\n' | timeout 10 ./mist | grep -q ready && rm -f mist)
rm -f "$out"
(cd "$stage" && zip -qr "$out" run.sh build.sh README.md src)
echo "$out $(sha256sum "$out" | cut -c1-12)"
