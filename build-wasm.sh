#!/usr/bin/env bash
# Build the standalone binjson-structures WASM module: lib/binjson-structures.wasm
# + lib/binjson-structures.wasm.mjs (the ES module loader), loaded by
# wasm/binjson-structures-wasm.js. Mirrors the parent project's
# c/build-wasm.sh (same flags, same build shape) but links only this
# package's own C sources plus its nested binjson submodule (needed both
# to compile -- bplustree.c etc. call bj_builder/bj_value_size directly --
# and to give the JS wrapper its own self-contained encode/decode, same
# as every other WASM module in this project's family). Requires `emcc`
# on PATH (emsdk) and `third_party/binjson` checked out
# (`git submodule update --init`).
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p lib

if [ ! -f third_party/binjson/include/binjson.h ]; then
  echo "error: third_party/binjson submodule not checked out -- run: git submodule update --init" >&2
  exit 1
fi

# Same flags as the parent project's combined build (c/build-wasm.sh) --
# see its own comment for why the stack size/overflow-check flags matter
# (the tree traversals recurse up to BPT_MAX_DEPTH/RT_MAX_DEPTH on a
# corrupt file before erroring out).
COMMON_FLAGS=(
  -O3
  -flto
  -Iinclude
  -Ithird_party/binjson/include
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sALLOW_MEMORY_GROWTH=1
  -sSTACK_SIZE=1048576
  -sSTACK_OVERFLOW_CHECK=1
  -sENVIRONMENT=web,worker,node
  -sEXPORTED_RUNTIME_METHODS=HEAPU8
  -sALLOW_TABLE_GROWTH=0
  -sFILESYSTEM=0
  --no-entry
)

# This package's own symbols and sources come from the manifests next to
# the JS wrappers (wasm/exports.txt, wasm/sources.txt) so a consumer that
# links these sources into its own combined binary consumes the same two
# lists (prefixing each source path with its checkout path) instead of
# hand-mirroring them here and drifting.
STRUCT_EXPORTS=$(grep -v '^#' wasm/exports.txt | grep -v '^$' | paste -sd, -)

EXPORTS='_malloc,_free,'\
`# binjson (internal use -- encode/decode's own copy needs these, see wasm/binjson-structures-wasm.js)`\
'_bjw_enc_reset,_bjw_put_null,_bjw_put_bool,_bjw_put_int,_bjw_put_float,'\
'_bjw_put_date,_bjw_put_pointer,_bjw_put_string,_bjw_put_binary,_bjw_put_oid,'\
'_bjw_put_key,_bjw_begin_array,_bjw_end_array,_bjw_begin_object,_bjw_end_object,'\
'_bjw_enc_finish,_bjw_enc_ptr,_bjw_enc_size,'\
'_bjw_decode,_bjw_events_ptr,_bjw_events_len,_bjw_consumed,_bjw_value_size,'\
"$STRUCT_EXPORTS"

SOURCES=(third_party/binjson/src/binjson.c third_party/binjson/src/binjson_wasm.c)
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue ;; esac
  SOURCES+=("$line")
done < wasm/sources.txt

emcc "${SOURCES[@]}" \
  "${COMMON_FLAGS[@]}" \
  -sEXPORT_NAME=createBinjsonStructuresModule \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -o lib/binjson-structures.mjs

mv lib/binjson-structures.mjs lib/binjson-structures.wasm.mjs
echo "built lib/binjson-structures.wasm.mjs ($(wc -c < lib/binjson-structures.wasm) bytes wasm)"
