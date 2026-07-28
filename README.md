# binjson-structures

WASM/C data structures for OPFS-backed storage, built on the [binjson](https://github.com/mdy-docs/binjson)
wire format and file model:

- **B+ tree** (`BPlusTree`) — persistent, append-only, ordered key/value index.
- **R-tree** (`RTree`) — spatial index (bounding-box search, k-nearest, geo/haversine).
- **Text log** (`TextLog`/`TiledTextLog`) — versioned text storage with compact binary deltas.
- **Text index** (`TextIndex`) — full-text search (BM25 scoring, Porter stemming), itself built on the B+ tree above.
- **Entry log** (`EntryLog`) — replicated-command log (a Raft log, which is
  also a write-ahead log): contiguous term-tagged entries with explicit
  sync-for-durability, durable Raft hard state (currentTerm/votedFor),
  logical suffix truncation (the Raft conflict rule), and snapshot-boundary
  prefix compaction/tiling.

Split out from the parent `binjson` document-database project (currently
staged ahead of becoming its own git submodule/repo there — see that
project's `third_party/regex-engine` for the pattern this is following).

## Dependency on binjson

This package's C sources call directly into binjson's encoder/builder
(`bj_builder`, `bj_value_size`, ...) — it's a real compile-time and
link-time dependency, not just a shared header. Its own standalone WASM
build links this package's sources together with a checkout of
[binjson](https://github.com/mdy-docs/binjson) into one binary, and its
JS wrapper imports binjson's `ObjectId`/`Pointer`/`TYPE` for its own
self-contained `encode`/`decode`.

That checkout is a git submodule of *this* repo (`third_party/binjson`),
used only for this package's own standalone build/test — a project that
already depends on both `binjson` and `binjson-structures` (like the
parent `binjson` project itself) should supply its own single copy of
binjson to build against (via an `-I` include path for C, and its own
import path for JS) rather than relying on this nested copy, to avoid
ending up with two separate binjson checkouts linked into the same
binary.

```
git submodule update --init
./build-wasm.sh
```

## Usage

```js
import { ready, BPlusTree, RTree, TextLog, TextIndex } from './wasm/binjson-structures-wasm.js';
import { MemoryHandle } from './third_party/binjson/js/binjson.js';

await ready();

const tree = new BPlusTree(new MemoryHandle(), 32);
await tree.open();
await tree.add('alice', { age: 30 });
console.log(await tree.search('alice')); // { age: 30 }
await tree.close();
```

Each structure takes an already-open storage handle (a real
`FileSystemSyncAccessHandle` for OPFS-backed persistence, or binjson's
`MemoryHandle` for in-memory/ephemeral use) — this package has no
opinion on multi-file or multi-database organization; that's a layer
built on top (see the parent `binjson` project's `Db`/`Client`).
