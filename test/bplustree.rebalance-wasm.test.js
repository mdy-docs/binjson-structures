/**
 * Delete rebalancing tests (C_DATABASE_REVIEW.md §2.3).
 *
 * A delete that drops a node below min_keys concatenates it with an adjacent
 * sibling — splitting the result back in two when it exceeds capacity — so
 * churn-heavy trees stay dense and shallow instead of degrading into a tall
 * chain of near-empty nodes. Root collapse follows merges all the way down,
 * and legacy (JS-written, never-rebalanced) files are absorbed progressively
 * as deletes touch their underfull nodes.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree delete rebalancing', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-rebalance-${Date.now()}-${counter++}.bj`;
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

  // Deterministic PRNG so failures reproduce.
  function rng(seed) {
    let s = seed >>> 0;
    return () => {
      s = (s * 1664525 + 1013904223) >>> 0;
      return s / 2 ** 32;
    };
  }

  it('churn collapses the tree: height and scan cost track live data, not history', async () => {
    const file = name();
    {
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      for (let i = 0; i < 2000; i++) tree.add(i, `v${i}`);
      for (let i = 0; i < 1990; i++) tree.delete(i);
      // 10 live keys in an order-4 tree fit in height <= 2. Without
      // rebalancing this tree measured height 9.
      expect(tree.getHeight()).toBeLessThanOrEqual(2);
      expect(tree.size()).toBe(10);
      await tree.close();
    }
    const proxy = counting(await sync(file));
    const tree = new BPlusTree(proxy, 4);
    await tree.open();

    const s0 = proxy.stats.reads;
    expect(tree.search(1995)).toBe('v1995');
    // One node per level. Without rebalancing this search read 10 nodes.
    expect(proxy.stats.reads - s0).toBeLessThanOrEqual(3);

    const s1 = proxy.stats.reads;
    expect(tree.toArray().map((e) => e.key)).toEqual(
      Array.from({ length: 10 }, (_, j) => 1990 + j));
    // The whole live tree is a handful of nodes. Without rebalancing this
    // scan visited every abandoned node: 1,986 reads.
    expect(proxy.stats.reads - s1).toBeLessThanOrEqual(10);
    await tree.close();
  });

  it('random interleaved insert/delete churn matches a Map reference', async () => {
    for (const order of [4, 5]) {   // even and odd min_keys behavior
      const file = name();
      const tree = new BPlusTree(await sync(file, true), order);
      await tree.open();
      const ref = new Map();
      const rand = rng(0xbeef + order);

      for (let op = 0; op < 4000; op++) {
        const key = Math.floor(rand() * 300);
        if (rand() < 0.55) {
          tree.add(key, `v${key}-${op}`);
          ref.set(key, `v${key}-${op}`);
        } else {
          tree.delete(key);
          ref.delete(key);
        }
      }

      expect(tree.size()).toBe(ref.size);
      const expected = [...ref.entries()].sort((a, b) => a[0] - b[0]);
      expect(tree.toArray().map((e) => [e.key, e.value])).toEqual(expected);
      for (const [k, v] of expected) expect(tree.search(k)).toBe(v);
      expect(tree.search(301)).toBeUndefined();
      await tree.close();
    }
  });

  it('deleting every key empties the tree and it keeps working', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 500; i++) tree.add(i, i * 2);
    for (let i = 499; i >= 0; i--) tree.delete(i);   // reverse order

    expect(tree.size()).toBe(0);
    expect(tree.getHeight()).toBe(0);
    expect(tree.toArray()).toEqual([]);

    tree.add(7, 'again');
    expect(tree.search(7)).toBe('again');
    expect(tree.size()).toBe(1);
    await tree.close();
  });

  it('absorbs hollowed-out legacy JS-written files', async () => {
    const file = name();
    // Frozen legacy fixture: the removed JS reference never rebalanced, so
    // add 0..119 / delete 20..99 left chains of empty leaves in the file.
    await writeFixture(await sync(file, true), 'bpt-o4-hollow.bin');

    const tree = new BPlusTree(await sync(file), 4);
    await tree.open();
    const before = tree.getHeight();
    // Deletes over the survivors merge the legacy underfull nodes they touch.
    for (let i = 0; i < 20; i++) tree.delete(i);
    for (let i = 100; i < 110; i++) tree.delete(i);
    expect(tree.size()).toBe(10);
    expect(tree.getHeight()).toBeLessThanOrEqual(before);
    expect(tree.toArray().map((e) => e.key)).toEqual(
      Array.from({ length: 10 }, (_, j) => 110 + j));
    tree.add(55, 'back');
    expect(tree.search(55)).toBe('back');

    // The rebalanced file reopens as a plain B+ tree.
    expect(tree.toArray().map((e) => e.key)).toEqual(
      [55, ...Array.from({ length: 10 }, (_, j) => 110 + j)]);
    await tree.close();
  });

  it('a snapshot pinned before churn is untouched by rebalancing rewrites', async () => {
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    for (let i = 0; i < 300; i++) tree.add(i, `v${i}`);

    const snap = tree.snapshot();
    for (let i = 0; i < 290; i++) tree.delete(i);   // heavy merging

    expect(snap.size()).toBe(300);
    expect(snap.toArray().length).toBe(300);
    expect(snap.search(0)).toBe('v0');
    expect(tree.size()).toBe(10);
    expect(tree.search(0)).toBeUndefined();
    await snap.close();
    await tree.close();
  });
});
