/**
 * Cursor API tests (C_DATABASE_REVIEW.md §2.2).
 *
 * bpt_cursor streams entries in sorted order with bounded memory: it holds a
 * descent stack plus one leaf (O(height) state) and pulls entries across the
 * bridge in size-capped batches instead of materializing the result set.
 * Cursors pin the root at open, so — the tree being append-only — they
 * iterate a consistent snapshot even while the tree is mutated.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree cursors', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-cursor-${Date.now()}-${counter++}.bj`;
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

  async function collect(iter) {
    const out = [];
    for await (const entry of iter) out.push(entry);
    return out;
  }

  it('full iteration equals toArray', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    const N = 500;
    for (let i = 0; i < N; i++) tree.add(i, { i, s: `v${i}` });
    expect(await collect(tree)).toEqual(tree.toArray());
    await tree.close();
  });

  it('range iteration equals rangeSearch, with open-ended bounds', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 300; i++) tree.add(i * 2, `v${i * 2}`);   // even keys

    expect(await collect(tree.iterate(100, 120))).toEqual(tree.rangeSearch(100, 120));
    expect(await collect(tree.iterate(99, 121))).toEqual(tree.rangeSearch(99, 121));
    expect(await collect(tree.iterate(597, 999))).toEqual(tree.rangeSearch(597, 999));
    expect(await collect(tree.iterate(50, 40))).toEqual([]);      // inverted

    // Open ends: min-only runs to the end, max-only starts at the front.
    const fromMin = await collect(tree.iterate(590));
    expect(fromMin.map((e) => e.key)).toEqual([590, 592, 594, 596, 598]);
    const toMax = await collect(tree.iterate(undefined, 8));
    expect(toMax.map((e) => e.key)).toEqual([0, 2, 4, 6, 8]);

    await tree.close();
  });

  it('streams with bounded reads: early termination touches a fraction of the tree', async () => {
    const file = name();
    {
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      for (let i = 0; i < 2000; i++) tree.add(i, `value-${i}`);
      await tree.close();
    }

    const proxy = counting(await sync(file));
    const tree = new BPlusTree(proxy, 4);
    await tree.open();
    const openReads = proxy.stats.reads;

    // Take only the first 10 entries, then stop.
    const first = [];
    for await (const entry of tree) {
      first.push(entry);
      if (first.length === 10) break;
    }
    const iterReads = proxy.stats.reads - openReads;
    expect(first.map((e) => e.key)).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);

    const beforeFull = proxy.stats.reads;
    tree.toArray();
    const fullReads = proxy.stats.reads - beforeFull;

    // One batch = descent + a handful of leaves; nothing close to the ~1000
    // node reads a full materialization performs.
    expect(iterReads).toBeLessThan(fullReads / 5);
    await tree.close();
  });

  it('iterates a consistent snapshot while the tree is mutated', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 200; i++) tree.add(i, `v${i}`);

    const it1 = tree.iterate();
    const first = await it1.next();               // cursor now open & pinned
    expect(first.value.key).toBe(0);

    tree.add(1000, 'added-mid-iteration');        // mutate while iterating
    tree.delete(150);

    const rest = [first.value, ...(await collect(it1))];
    // The pinned snapshot: no key 1000, and 150 still present.
    expect(rest.length).toBe(200);
    expect(rest.some((e) => e.key === 1000)).toBe(false);
    expect(rest.some((e) => e.key === 150)).toBe(true);

    // A fresh iterator sees the new state.
    const now = await collect(tree);
    expect(now.length).toBe(200);                 // +1 added, -1 deleted
    expect(now.some((e) => e.key === 1000)).toBe(true);
    expect(now.some((e) => e.key === 150)).toBe(false);
    await tree.close();
  });

  it('handles empty trees and empty leaves left by deletions', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    expect(await collect(tree)).toEqual([]);

    for (let i = 0; i < 60; i++) tree.add(i, i);
    for (let i = 10; i < 50; i++) tree.delete(i);   // hollow out the middle
    const keys = (await collect(tree)).map((e) => e.key);
    expect(keys).toEqual([...Array(10).keys(), ...Array.from({ length: 10 }, (_, j) => 50 + j)]);
    await tree.close();
  });

  it('iterates string keys with range bounds', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 100; i++) tree.add(`k-${String(i).padStart(3, '0')}`, i);
    const hits = await collect(tree.iterate('k-042', 'k-045'));
    expect(hits.map((e) => e.key)).toEqual(['k-042', 'k-043', 'k-044', 'k-045']);
    expect(hits.map((e) => e.value)).toEqual([42, 43, 44, 45]);
    await tree.close();
  });
});
