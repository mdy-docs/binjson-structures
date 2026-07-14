/**
 * Range-scan pruning tests (C_DATABASE_REVIEW.md §2.1).
 *
 * bpt_range prunes its descent with the internal routing keys, so a range
 * scan reads O(height) nodes plus the leaves that actually hold matches —
 * previously it visited every node in the tree and filtered at the leaves.
 * A counting proxy around the sync access handle pins the read counts down,
 * and singleton-range sweeps exercise every separator-boundary edge case.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree range pruning', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-pruning-${Date.now()}-${counter++}.bj`;
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

  /** Delegating proxy that counts host reads. */
  function counting(handle) {
    const stats = { reads: 0 };
    return {
      stats,
      getSize: () => handle.getSize(),
      read: (buf, opts) => { stats.reads++; return handle.read(buf, opts); },
      write: (buf, opts) => handle.write(buf, opts),
      truncate: (n) => handle.truncate(n),
      flush: () => handle.flush(),
      close: () => handle.close()
    };
  }

  const N = 1000;

  async function buildTree(filename) {
    const tree = new BPlusTree(await sync(filename, true), 4);
    await tree.open();
    for (let i = 0; i < N; i++) tree.add(i, `v${i}`);
    await tree.close();
  }

  it('reads only the overlapping subtrees for a narrow range', async () => {
    const file = name();
    await buildTree(file);

    const proxy = counting(await sync(file));
    const tree = new BPlusTree(proxy, 4);
    await tree.open();
    const openReads = proxy.stats.reads;

    const hits = tree.rangeSearch(500, 510);
    const rangeReads = proxy.stats.reads - openReads;
    expect(hits.map((h) => h.key)).toEqual([500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510]);
    expect(hits[0].value).toBe('v500');

    const beforeFull = proxy.stats.reads;
    const all = tree.toArray();
    const fullReads = proxy.stats.reads - beforeFull;
    expect(all.length).toBe(N);

    // Order-4 tree of 1000 keys has hundreds of nodes; the narrow range must
    // touch only the two boundary paths plus the few leaves in between.
    expect(rangeReads).toBeLessThan(60);
    expect(rangeReads * 5).toBeLessThan(fullReads);

    await tree.close();
  });

  it('handles every separator boundary: singleton ranges over all keys', async () => {
    const file = name();
    await buildTree(file);
    const tree = new BPlusTree(await sync(file), 4);
    await tree.open();
    for (let k = 0; k < N; k++) {
      const hits = tree.rangeSearch(k, k);
      expect(hits.length).toBe(1);
      expect(hits[0].key).toBe(k);
      expect(hits[0].value).toBe(`v${k}`);
    }
    await tree.close();
  });

  it('matches full-scan semantics at the edges', async () => {
    const file = name();
    await buildTree(file);
    const tree = new BPlusTree(await sync(file), 4);
    await tree.open();

    expect(tree.rangeSearch(-100, N + 100).length).toBe(N);   // superset range
    expect(tree.rangeSearch(0, N - 1).length).toBe(N);        // exact range
    expect(tree.rangeSearch(10, 5)).toEqual([]);              // inverted: empty
    expect(tree.rangeSearch(N + 1, N + 50)).toEqual([]);      // beyond max
    expect(tree.rangeSearch(-50, -1)).toEqual([]);            // below min
    expect(tree.rangeSearch(499.5, 500.5).map((h) => h.key)).toEqual([500]);

    await tree.close();
  });

  it('prunes string-key ranges', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 300; i++) tree.add(`key-${String(i).padStart(4, '0')}`, i);
    const hits = tree.rangeSearch('key-0100', 'key-0104');
    expect(hits.map((h) => h.key)).toEqual(
      ['key-0100', 'key-0101', 'key-0102', 'key-0103', 'key-0104']);
    expect(hits.map((h) => h.value)).toEqual([100, 101, 102, 103, 104]);
    await tree.close();
  });
});
