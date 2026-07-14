/**
 * Out-of-line large value tests (C_DATABASE_REVIEW.md §2.6).
 *
 * Values over ~256 bytes are stored out-of-line: appended to the file once
 * as their own record, with the leaf holding a small { "\0ool": Pointer }
 * marker instead of the raw bytes. Every leaf rewrite that merely carries a
 * large value along (an insert/delete/split touching a sibling key) then
 * copies the small marker instead of re-copying the whole value — the fix
 * for the write amplification an append-only, whole-leaf-rewrite design
 * otherwise pays on every operation. Resolution is transparent to every
 * read API (search, toArray, rangeSearch, cursors); only a raw record-level
 * read (BinJsonFile) sees the marker, exactly like child/next pointers
 * already work.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { BinJsonFile, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree out-of-line large values', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-largeval-${Date.now()}-${counter++}.bj`;
    files.push(n);
    return n;
  };

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  async function sync(filename, create = false) {
    const fh = await getFileHandle(root, filename, { create });
    return fh.createSyncAccessHandle();
  }

  function counting(handle) {
    const stats = { reads: 0, writtenBytes: 0 };
    return {
      stats,
      getSize: () => handle.getSize(),
      read: (buf, opts) => {
        stats.reads++;
        return handle.read(buf, opts);
      },
      write: (buf, opts) => {
        stats.writtenBytes += buf.length;
        return handle.write(buf, opts);
      },
      truncate: (n) => handle.truncate(n),
      flush: () => handle.flush(),
      close: () => handle.close()
    };
  }

  const big = (tag, n = 1000) => ({ tag, pad: tag.repeat(Math.ceil(n / tag.length)).slice(0, n) });

  /** True if the raw leaf record's value for `key` is the out-of-line
   *  marker rather than the literal value (record-level, via BinJsonFile). */
  function isOolMarker(rawValue) {
    return rawValue && typeof rawValue === 'object' &&
      Object.keys(rawValue).length === 1 && '\0ool' in rawValue;
  }

  async function scanLeafValues(filename) {
    const handle = await sync(filename);
    const file = new BinJsonFile(handle);
    const byKey = new Map();
    for (const { value } of file.scan()) {
      if (value && typeof value === 'object' && value.isLeaf) {
        value.keys.forEach((k, i) => byKey.set(k, value.values[i]));
      }
    }
    await handle.close();
    return byKey;
  }

  it('round-trips large values through every read API', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 8);
    await tree.open();
    for (let i = 0; i < 30; i++) tree.add(i, big(`v${i}-`));
    expect(tree.verify()).toBe(true);

    expect(tree.search(17)).toEqual(big('v17-'));
    const all = tree.toArray();
    expect(all.length).toBe(30);
    expect(all.find((e) => e.key === 17).value).toEqual(big('v17-'));
    const ranged = tree.rangeSearch(10, 15);
    expect(ranged.map((e) => e.key)).toEqual([10, 11, 12, 13, 14, 15]);
    expect(ranged[3].value).toEqual(big('v13-'));

    let iterCount = 0;
    for await (const e of tree) {
      expect(e.value).toEqual(big(`v${e.key}-`));
      iterCount++;
    }
    expect(iterCount).toBe(30);
    await tree.close();
  });

  it('small values stay inline; values over the threshold go out-of-line', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 8);
    await tree.open();
    // A STRING value encodes as 1 (type) + 4 (length) + N bytes, so N = 251
    // and N = 252 land exactly either side of the 256-byte threshold.
    tree.add('small', 'x'.repeat(50));
    tree.add('at-threshold', 'x'.repeat(251));      // encodes to exactly 256
    tree.add('over-threshold', 'x'.repeat(252));    // encodes to exactly 257
    tree.add('large', big('L', 900));               // well over threshold
    await tree.close();

    const values = await scanLeafValues(file);
    expect(isOolMarker(values.get('small'))).toBe(false);
    expect(isOolMarker(values.get('at-threshold'))).toBe(false);
    expect(isOolMarker(values.get('over-threshold'))).toBe(true);
    expect(isOolMarker(values.get('large'))).toBe(true);
  });

  it('write amplification: siblings carry a marker, not the value, on every leaf rewrite', async () => {
    const file = name();
    {
      const tree = new BPlusTree(await sync(file, true), 8);
      await tree.open();
      // Fill one leaf with 7 large values, leaving room for one more insert
      // (order 8) to trigger a rewrite that carries all 7 siblings along.
      for (let i = 0; i < 7; i++) tree.add(i, big(`s${i}-`, 1000));
      await tree.close();
    }
    const proxy = counting(await sync(file));
    const tree = new BPlusTree(proxy, 8);
    await tree.open();
    tree.add(7, 'tiny');   // triggers a leaf rewrite carrying the 7 big siblings
    // The rewritten leaf must be small: 8 markers plus small overhead, not
    // 7 KB of re-copied value bytes. (Read side already proves resolution
    // works; this pins the disk-size side of the fix.)
    expect(proxy.stats.writtenBytes).toBeLessThan(2000);
    expect(tree.search(3)).toEqual(big('s3-', 1000));
    await tree.close();
  });

  it('updating a large value to small (and back) works and abandons the old blob', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    tree.add('k', big('first', 900));
    expect(tree.search('k')).toEqual(big('first', 900));
    tree.add('k', 'now small');
    expect(tree.search('k')).toBe('now small');
    expect(tree.size()).toBe(1);
    tree.add('k', big('second', 900));
    expect(tree.search('k')).toEqual(big('second', 900));
    expect(tree.verify()).toBe(true);
    await tree.close();
  });

  it('deleting a key with a large value works and rebalancing carries markers correctly', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 60; i++) tree.add(i, big(`d${i}-`, 800));
    for (let i = 0; i < 45; i++) tree.delete(i);   // heavy merge/redistribute churn
    expect(tree.size()).toBe(15);
    expect(tree.verify()).toBe(true);
    for (let i = 45; i < 60; i++) expect(tree.search(i)).toEqual(big(`d${i}-`, 800));
    await tree.close();
  });

  it('a snapshot pinned before a large-value update keeps serving the old bytes', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    tree.add('k', big('old', 900));
    const snap = tree.snapshot();
    tree.add('k', big('new', 900));
    expect(snap.search('k')).toEqual(big('old', 900));
    expect(tree.search('k')).toEqual(big('new', 900));
    await snap.close();
    await tree.close();
  });

  it('compaction resolves out-of-line values and re-externalizes them, shrinking the file', async () => {
    const src = name();
    const dst = name();
    const tree = new BPlusTree(await sync(src, true), 8);
    await tree.open();
    for (let i = 0; i < 100; i++) tree.add(i, big(`c${i}-`, 900));
    for (let i = 0; i < 60; i++) tree.delete(i);   // churn: abandoned history
    // tree still has src open at this point -- read the size through its own
    // handle rather than opening a second one (node-opfs now enforces OPFS's
    // real single-writer-per-file constraint on the same still-open file).
    const beforeSize = tree.syncAccessHandle.getSize();
    await tree.compact(await sync(dst, true));
    await tree.close();

    const dstHandle = await sync(dst);
    const afterSize = dstHandle.getSize();
    await dstHandle.close();
    expect(afterSize).toBeLessThan(beforeSize);

    const packed = new BPlusTree(await sync(dst), 8);
    await packed.open();
    expect(packed.size()).toBe(40);
    expect(packed.verify()).toBe(true);
    for (let i = 60; i < 100; i++) expect(packed.search(i)).toEqual(big(`c${i}-`, 900));
    await packed.close();

    // The compacted file still stores large values out-of-line (compaction
    // re-applies the threshold against the destination, not a raw copy).
    const values = await scanLeafValues(dst);
    expect(isOolMarker(values.get(70))).toBe(true);
  });

  it('a large-value marker is only visible at the raw record level, and is itself followable', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    tree.add('doc', big('payload', 900));
    await tree.close();

    // Low-level: BinJsonFile sees the marker object, exactly like children
    // pointers — following it manually decodes the real value.
    const handle = await sync(file);
    const bjFile = new BinJsonFile(handle);
    let markerPointer = null;
    for (const { value } of bjFile.scan()) {
      if (value && typeof value === 'object' && value.isLeaf) {
        const idx = value.keys.indexOf('doc');
        if (idx >= 0) markerPointer = value.values[idx]['\0ool'];
      }
    }
    expect(markerPointer).toBeDefined();
    expect(bjFile.read(markerPointer)).toEqual(big('payload', 900));
    await handle.close();

    // High-level API never exposes the marker.
    const reopened = new BPlusTree(await sync(file), 4);
    await reopened.open();
    expect(reopened.search('doc')).toEqual(big('payload', 900));
    await reopened.close();
  });
});
