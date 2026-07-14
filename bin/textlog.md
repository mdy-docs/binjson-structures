# textlog

A command-line tool for reading and writing binjson-backed versioned text logs.
A `textlog` keeps a full history of a piece of text: each `add` records a new
version, and the log stores periodic snapshots plus diffs so any past version can
be reconstructed and compared.

```
textlog <file.bj> <command> [args] [options]
```

If `<command>` is omitted it defaults to `list`. Versions are numbered from `1`.

## Commands

### Viewing

| Command | Description |
| --- | --- |
| `list` | List every version with its hash (default) |
| `get [version]` | Print the full text at `<version>` (default: latest) |
| `diff <from> <to>` | Print a human-readable diff between two versions |
| `hash <version>` | Print the SHA-256 hash of a version |
| `info` | Print current version, snapshot interval, and file size |

Aliases: `list` also accepts `log`; `get` accepts `show`; `info` accepts `stats`.

### Editing

| Command | Description |
| --- | --- |
| `add [text...]` | Append a new version (creates the file if needed) |

Alias: `add` also accepts `commit`.

The text for a new version is taken from, in order of precedence:

1. the `--file <path>` option (read from a real file on disk), or
2. the command arguments (joined with spaces), or
3. standard input, when neither of the above is given.

```sh
textlog notes.bj add "first draft"          # from arguments
textlog notes.bj add --file draft.txt        # from a file
pbpaste | textlog notes.bj add               # from stdin
```

## Options

| Option | Description |
| --- | --- |
| `-f`, `--file <path>` | `add`: read the version text from a file |
| `--diffs-per-snapshot <n>` | Snapshot interval for a newly created log (default 10, minimum 1) |
| `-h`, `--help` | Show help |

`--diffs-per-snapshot` controls how often a full snapshot is written between
diffs. It only takes effect when the log file is first created; an existing log
keeps the interval it was created with.

## Behavior notes

- `add` creates the file if it does not exist; every other command requires the
  log to already exist.
- Writes are append-only and write-through, so history survives a crash before
  the process exits. Versions are never modified or deleted.
- `get` on an empty log exits non-zero.

## Examples

Record some versions (the file is created on the first `add`):

```sh
textlog story.bj add "the quick brown fox"
textlog story.bj add "the quick red fox jumps over the lazy dog"
textlog story.bj add --file chapter1.txt
```

List the version history:

```sh
textlog story.bj list
# v1: 9ecb3656...e25fc8f
# v2: 1448d863...bffbb57f
# v3: 6ea58a67...11df183 (latest)

# or simply:
textlog story.bj
```

Print a version (defaults to the latest) and compare two:

```sh
textlog story.bj get        # latest
textlog story.bj get 1      # first version
textlog story.bj diff 1 2
# --- version 1
# +++ version 2
# @@ -1 +1 @@
# -the quick brown fox
# +the quick red fox jumps over the lazy dog
```

Show the hash of a version and the log's statistics:

```sh
textlog story.bj hash 2
textlog story.bj info
# file:             story.bj
# size:             1437 bytes
# versions:         3
# diffsPerSnapshot: 10
```

Create a log that snapshots more frequently:

```sh
textlog dense.bj --diffs-per-snapshot 5 add "initial"
```

## Running

The tool requires [`node-opfs`](https://www.npmjs.com/package/node-opfs) to
provide OPFS storage under Node.js (installed as a dev dependency). Run it
directly:

```sh
node bin/textlog.js story.bj list
```

or, once the package is installed, via the `textlog` bin.
