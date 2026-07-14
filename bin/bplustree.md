# bplustree

A command-line tool for reading and writing binjson-backed B+ tree files. It can
decode existing trees and create, append to, update, and compact them.

```
bplustree <file.bj> <command> [args] [options]
```

If `<command>` is omitted it defaults to `list`.

## Commands

| Command | Description |
| --- | --- |
| `list` | Print every entry in sorted order (default) |
| `get <key>` | Look up a single key |
| `range <min> <max>` | Print entries with `min <= key <= max` |
| `put <key> <value>` | Insert or update a key (creates the file if needed) |
| `delete <key>` | Remove a key |
| `info` | Print size, height, and order |
| `compact [dest.bj]` | Rewrite the file, dropping stale append history (in place, or into `dest.bj` if given) |

Aliases: `list` also accepts `dump`/`decode`; `get` accepts `search`; `put`
accepts `set`/`add`; `delete` accepts `del`/`remove`; `info` accepts `stats`.

## Keys

Keys that look numeric are read as numbers, otherwise as strings (the tree
supports number and string keys only). Use `--string-keys` / `-s` to force every
key to be treated as a string — for example, to store `"42"` as a string rather
than the number `42`.

## Values

Values are parsed as JSON when possible, and fall back to a raw string
otherwise:

- `'{"id":1,"label":"first"}'` → object
- `42` → number
- `true` → boolean
- `'"text"'` → string `"text"`
- `text` → string `"text"` (not valid JSON, kept literally)

## Options

| Option | Description |
| --- | --- |
| `--order <n>` | Tree order for a newly created file (default 3, minimum 3) |
| `-s`, `--string-keys` | Treat keys as strings even when they look numeric |
| `-h`, `--help` | Show help |

## Behavior notes

- `put` and `delete` create the file if it does not exist; read commands error
  cleanly on a missing file.
- Writes are append-only and write-through, so data survives a crash before the
  process exits.
- `compact` defaults to rewriting the file in place; pass a destination to write
  the compacted tree to a new file instead. It reports the bytes saved.
- `get` and `delete` exit non-zero when the key is not found, so they compose in
  shell scripts.

## Examples

Create a tree and insert some entries (the file is created on first `put`):

```sh
bplustree data.bj put alpha '{"id":1,"label":"first"}'
bplustree data.bj put beta  '{"id":2}'
bplustree data.bj put gamma '"third"'
bplustree data.bj put 42    true
```

Update an existing key:

```sh
bplustree data.bj put alpha '{"id":1,"label":"updated"}'
```

List everything:

```sh
bplustree data.bj list
# or simply:
bplustree data.bj
```

Look up a single key and do a range scan:

```sh
bplustree data.bj get beta
bplustree data.bj range alpha gamma
```

Show tree statistics:

```sh
bplustree data.bj info
# file:   data.bj
# order:  3
# size:   4 entries
# height: 1
```

Delete a key:

```sh
bplustree data.bj delete beta
```

Compact in place, or into a new file:

```sh
bplustree data.bj compact
bplustree data.bj compact data-compacted.bj
```

Force string keys and set a custom order for a new file:

```sh
bplustree ids.bj --string-keys put 42 '"answer"'
bplustree big.bj --order 64 put alpha '{"id":1}'
```

Use exit codes in a script:

```sh
if bplustree data.bj get alpha > /dev/null; then
  echo "alpha exists"
fi
```

## Running

The tool requires [`node-opfs`](https://www.npmjs.com/package/node-opfs) to
provide OPFS storage under Node.js (installed as a dev dependency). Run it
directly:

```sh
node bin/bplustree.js data.bj list
```

or, once the package is installed, via the `bplustree` bin.
