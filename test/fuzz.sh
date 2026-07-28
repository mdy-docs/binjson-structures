#!/bin/bash
# Build and run the hostile-file fuzz harness (test/fuzz.c) with ASan + UBSan.
#
#   test/fuzz.sh [iterations] [seed]     (defaults: 20000, 1)
#
# Exits non-zero on any crash, hang (20s watchdog), or sanitizer report.
# To reproduce a failure: test/fuzz.sh 1 <seed printed by the failing run>.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -f third_party/binjson/include/binjson.h ]; then
  echo "error: third_party/binjson submodule not checked out -- run: git submodule update --init" >&2
  exit 1
fi

OUT="${TMPDIR:-/tmp}/bjfuzz"
cc -std=c11 -g -O1 -Wall -Wextra -Werror \
   -Iinclude -Ithird_party/binjson/include \
   -fsanitize=address,undefined -fno-sanitize-recover=all \
   -o "$OUT" \
   test/fuzz.c third_party/binjson/src/binjson.c \
   src/bjfile.c src/bplustree.c src/rtree.c src/textlog.c \
   src/entrylog.c src/textindex.c src/stemmer.c src/diff.c src/geo.c

"$OUT" "${1:-20000}" "${2:-1}"
