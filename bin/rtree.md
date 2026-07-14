# rtree

A command-line tool for reading and writing binjson-backed R-tree files. An
R-tree file stores geospatial points: each point is a `(lat, lng)` location
tagged with a 24-hex-character `ObjectId`. The tool can list and query points and
insert, remove, clear, and compact them.

```
rtree <file.bj> <command> [args] [options]
```

If `<command>` is omitted it defaults to `list`.

Latitude is in the range `-90..90`, longitude `-180..180`.

## Commands

### Viewing

| Command | Description |
| --- | --- |
| `list` | Print every point (default) |
| `bbox <minLat> <maxLat> <minLng> <maxLng>` | Print points inside a bounding box |
| `radius <lat> <lng> <km>` | Print points within `<km>` of a location, with their distance |
| `info` | Print entry count, node capacity, and file size |

Aliases: `list` also accepts `dump`/`decode`; `info` accepts `stats`.

### Editing

| Command | Description |
| --- | --- |
| `insert <lat> <lng> [objectId]` | Insert a point (creates the file if needed); a random `ObjectId` is generated when omitted |
| `remove <objectId>` | Remove the point with the given `ObjectId` |
| `clear` | Remove every point |
| `compact [dest.bj]` | Rewrite the file, dropping stale append history (in place, or into `dest.bj` if given) |

Aliases: `insert` also accepts `add`; `remove` accepts `delete`/`del`.

## ObjectIds

An `ObjectId` is a 24-character hexadecimal string (12 bytes). Points are
identified by their `ObjectId`, so `remove` needs the exact id shown by `list`.
When you `insert` without supplying one, a fresh random `ObjectId` is generated
and printed so you can reference it later.

## Options

| Option | Description |
| --- | --- |
| `--max-entries <n>` | Node capacity for a newly created file (default 9, minimum 2) |
| `-h`, `--help` | Show help |

## Behavior notes

- `insert` creates the file if it does not exist; every other command requires
  the file to already exist.
- Writes are append-only and write-through, so data survives a crash before the
  process exits.
- `compact` defaults to rewriting the file in place; pass a destination to write
  the compacted tree to a new file instead. It reports the bytes saved.
- `remove` exits non-zero when the `ObjectId` is not found, so it composes in
  shell scripts.

## Examples

Insert points (the file is created on first `insert`):

```sh
rtree cities.bj insert 40.7128 -74.0060 5f1d7f3a0b0c0d0e0f101112   # New York
rtree cities.bj insert 34.0522 -118.2437 6a6b6c6d6e6f707172737475  # Los Angeles
rtree cities.bj insert 41.8781 -87.6298                            # Chicago (random id)
```

List every point:

```sh
rtree cities.bj list
# 0: ObjectId(5f1d7f3a0b0c0d0e0f101112) (lat: 40.7128, lng: -74.006)
# 1: ObjectId(6a6b6c6d6e6f707172737475) (lat: 34.0522, lng: -118.2437)
# ...

# or simply:
rtree cities.bj
```

Query by bounding box or by radius:

```sh
rtree cities.bj bbox 30 45 -80 -70
rtree cities.bj radius 40.7128 -74.0060 500
# 0: ObjectId(5f1d7f3a0b0c0d0e0f101112) (lat: 40.7128, lng: -74.006, distance: 0.000 km)
```

Show tree statistics:

```sh
rtree cities.bj info
# file:        cities.bj
# size:        2006 bytes
# points:      3
# maxEntries:  9
```

Remove a point, clear the tree, or compact:

```sh
rtree cities.bj remove 6a6b6c6d6e6f707172737475
rtree cities.bj clear
rtree cities.bj compact
rtree cities.bj compact cities-compacted.bj
```

Set a custom node capacity for a new file:

```sh
rtree dense.bj --max-entries 16 insert 40.0 -74.0
```

Use exit codes in a script:

```sh
if rtree cities.bj remove 5f1d7f3a0b0c0d0e0f101112 > /dev/null; then
  echo "removed"
fi
```

## Running

The tool requires [`node-opfs`](https://www.npmjs.com/package/node-opfs) to
provide OPFS storage under Node.js (installed as a dev dependency). Run it
directly:

```sh
node bin/rtree.js cities.bj list
```

or, once the package is installed, via the `rtree` bin.
