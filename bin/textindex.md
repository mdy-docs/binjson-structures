# textindex

A command-line tool for reading and writing binjson-backed full-text indexes. A
`textindex` indexes documents by their words and answers BM25-ranked queries. It
can add and remove documents, run queries, and compact the index.

```
textindex <name> <command> [args] [options]
```

If `<command>` is omitted it defaults to `list`.

`<name>` is a **base name**, not a single file. An index is stored across three
B+ tree files:

| File | Contents |
| --- | --- |
| `<name>-terms.bj` | the inverted index (term → postings) |
| `<name>-documents.bj` | per-document term lists |
| `<name>-lengths.bj` | per-document lengths (for BM25) |

## Commands

### Viewing

| Command | Description |
| --- | --- |
| `list` | List every indexed document id (default) |
| `query <text>` | Rank documents matching `<text>` (BM25 scored) |
| `info` | Print term count, document count, and file sizes |

Aliases: `list` also accepts `docs`; `query` accepts `search`; `info` accepts
`stats`.

### Editing

| Command | Description |
| --- | --- |
| `add <docId> <text>` | Index a document under `<docId>` (creates the index if needed) |
| `remove <docId>` | Remove a document from the index |
| `clear` | Remove every document |
| `compact [destName]` | Rewrite the index files, dropping stale append history (in place, or under `destName` if given) |

Aliases: `add` also accepts `index`; `remove` accepts `delete`/`del`.

Re-adding an existing `docId` replaces its previous text. The `<text>` may be
multiple shell words; they are joined with spaces, so quoting is optional:

```sh
textindex blog add post-1 the quick brown fox
textindex blog add post-1 "the quick brown fox"   # equivalent
```

## Query options

By default `query` returns documents ranked by BM25 score. Two flags change this:

| Option | Description |
| --- | --- |
| `--ids` | Print only document ids (any term matches), without scores |
| `--all` | Require every query term to be present (AND); prints ids |

```sh
textindex blog query quick brown          # ranked, scored
textindex blog query quick brown --ids     # matching ids, unscored
textindex blog query quick brown --all     # ids containing BOTH terms
```

## Options

| Option | Description |
| --- | --- |
| `--order <n>` | B+ tree order for newly created files (default 16, minimum 3) |
| `-h`, `--help` | Show help |

## Behavior notes

- `add` creates the three files if they do not exist; every other command
  requires the index to already exist.
- Writes are append-only and write-through, so data survives a crash before the
  process exits.
- `compact` defaults to rewriting the files in place; pass a destination base
  name to write the compacted index alongside the original. It reports the total
  bytes saved across the three files.
- `query` (and `remove`) exit non-zero when there is no match, so they compose in
  shell scripts.

## Examples

Build an index (files are created on the first `add`):

```sh
textindex blog add post-1 "the quick brown fox"
textindex blog add post-2 "the lazy brown dog"
textindex blog add post-3 "quick foxes jump"
```

List the indexed documents:

```sh
textindex blog list
# 0: "post-1"
# 1: "post-2"
# 2: "post-3"

# or simply:
textindex blog
```

Run queries:

```sh
textindex blog query quick
# 0: "post-1" (score: 0.2703)
# 1: "post-3" (score: 0.2703)

textindex blog query brown --ids
textindex blog query "brown dog" --all
```

Show index statistics:

```sh
textindex blog info
# name:      blog
# terms:     6
# documents: 3
#   blog-terms.bj: 3761 bytes (index)
#   blog-documents.bj: 1361 bytes (documentTerms)
#   blog-lengths.bj: 1052 bytes (documentLengths)
```

Remove a document, clear the index, or compact:

```sh
textindex blog remove post-2
textindex blog clear
textindex blog compact
textindex blog compact blog-compacted
```

## Running

The tool requires [`node-opfs`](https://www.npmjs.com/package/node-opfs) to
provide OPFS storage under Node.js (installed as a dev dependency). Run it
directly:

```sh
node bin/textindex.js blog query quick
```

or, once the package is installed, via the `textindex` bin.
