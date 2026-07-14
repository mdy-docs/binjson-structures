/**
 * Invariant checker tests (C_DATABASE_REVIEW.md §2.10).
 *
 * bpt_verify walks every node checking key order, routing-key consistency
 * with ancestors, node capacity, non-empty internals, child-before-parent
 * offsets (which also rules out pointer cycles), uniform leaf depth, and
 * that the leaf entry total matches the metadata size. It must accept
 * everything legitimate writers produce — including JS-written files that
 * never rebalance — and reject hand-crafted violations of each invariant.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { Pointer, encode, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree verify', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-verify-${Date.now()}-${counter++}.bj`;
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

  /** Write encoded records as a legacy-format (headerless) file; returns
   *  each record's offset. */
  async function craft(filename, records) {
    const handle = await sync(filename, true);
    const offsets = [];
    let at = 0;
    for (const rec of records) {
      offsets.push(at);
      handle.write(rec, { at });
      at += rec.byteLength;
    }
    handle.flush();
    await handle.close();
    return offsets;
  }

  const meta = (rootPtr, size) => encode({
    version: 1,
    maxEntries: 4,
    minEntries: 1,
    size,
    rootPointer: new Pointer(rootPtr),
    nextId: 99
  });

  const leaf = (id, keys, next = null) => encode({
    id, isLeaf: true, keys, values: keys.map((k) => `v${k}`), children: [], next
  });

  const internal = (id, keys, childOffsets) => encode({
    id, isLeaf: false, keys, values: [], children: childOffsets.map((o) => new Pointer(o)), next: null
  });

  async function openCrafted(filename) {
    const tree = new BPlusTree(await sync(filename), 4);
    await tree.open();
    return tree;
  }

  it('accepts everything legitimate writers produce', async () => {
    // Fresh, churned (rebalanced), emptied, and re-filled — all at order 4.
    const file = name();
    const tree = new BPlusTree(await sync(file, true), 4);
    await tree.open();
    expect(tree.verify()).toBe(true);                    // empty tree
    for (let i = 0; i < 500; i++) tree.add(i, `v${i}`);
    expect(tree.verify()).toBe(true);
    for (let i = 100; i < 480; i++) tree.delete(i);      // heavy merging
    expect(tree.verify()).toBe(true);
    for (let i = 0; i < 100; i++) tree.delete(i);
    for (let i = 480; i < 500; i++) tree.delete(i);      // down to empty
    expect(tree.verify()).toBe(true);
    for (let i = 0; i < 50; i++) tree.add(`s${i}`, i);   // string keys
    expect(tree.verify()).toBe(true);

    // A snapshot verifies while the live tree keeps mutating.
    const snap = tree.snapshot();
    for (let i = 0; i < 50; i++) tree.add(`t${i}`, i);
    expect(snap.verify()).toBe(true);
    expect(tree.verify()).toBe(true);
    await snap.close();

    // A compacted destination verifies.
    const dst = name();
    await tree.compact(await sync(dst, true));
    const compacted = new BPlusTree(await sync(dst), 4);
    await compacted.open();
    expect(compacted.verify()).toBe(true);
    await compacted.close();
    await tree.close();
  });

  it('accepts under-filled legacy JS-written files', async () => {
    const file = name();
    // Frozen legacy fixture: add 0..119, delete 20..99 — empty leaves remain.
    await writeFixture(await sync(file, true), 'bpt-o4-hollow.bin');

    const tree = await openCrafted(file);
    expect(tree.verify()).toBe(true);
    await tree.close();
  });

  it('rejects out-of-order keys within a node', async () => {
    const file = name();
    const l = leaf(1, [2, 1]);
    await craft(file, [l, meta(0, 2)]);
    const tree = await openCrafted(file);
    expect(() => tree.verify()).toThrow(/invariant/i);
    await tree.close();
  });

  it('rejects a key on the wrong side of its routing separator', async () => {
    // Separator 5, but the left leaf holds a 7 (equal-keys-route-right
    // means the left child must hold keys strictly below 5).
    const file = name();
    const a = leaf(1, [7]);
    const b = leaf(2, [5]);
    const offs = await craft(file, [a, b, internal(3, [5], [0, a.byteLength]),
                                    meta(a.byteLength + b.byteLength, 2)]);
    void offs;
    const tree = await openCrafted(file);
    expect(() => tree.verify()).toThrow(/invariant/i);
    await tree.close();
  });

  it('rejects an entry count that disagrees with the metadata size', async () => {
    const file = name();
    const l = leaf(1, [1]);
    await craft(file, [l, meta(0, 3)]);
    const tree = await openCrafted(file);
    expect(() => tree.verify()).toThrow(/invariant/i);
    await tree.close();
  });

  it('rejects self or forward child pointers (cycles) without hanging', async () => {
    // The hostile-file case: an internal root whose children point at
    // itself. verify flags the offset-order violation immediately.
    const file = name();
    const node = internal(1, [5], [0, 0]);
    await craft(file, [node, meta(0, 1)]);
    const tree = await openCrafted(file);
    expect(() => tree.verify()).toThrow(/invariant/i);
    await tree.close();
  });

  it('rejects leaves at different depths', async () => {
    const l1 = leaf(1, [1]);
    const l2 = leaf(2, [5]);
    const off2 = l1.byteLength;
    const inner = internal(3, [5], [0, off2]);
    const offInner = off2 + l2.byteLength;
    const l3 = leaf(4, [10]);
    const off3 = offInner + inner.byteLength;
    const rootNode = internal(5, [10], [offInner, off3]);
    const offRoot = off3 + l3.byteLength;
    const file = name();
    await craft(file, [l1, l2, inner, l3, rootNode, meta(offRoot, 3)]);
    const tree = await openCrafted(file);
    expect(() => tree.verify()).toThrow(/invariant/i);
    await tree.close();
  });

  it('rejects an over-capacity node but accepts a unary internal', async () => {
    const fileA = name();   // order 4 allows at most 3 keys per node
    const big = leaf(1, [1, 2, 3, 4, 5]);
    await craft(fileA, [big, meta(0, 5)]);
    const treeA = await openCrafted(fileA);
    expect(() => treeA.verify()).toThrow(/invariant/i);
    await treeA.close();

    // A zero-key (one-child) internal node is legal: the compaction bulk
    // loader's rightmost spine emits them at level tails.
    const fileB = name();
    const l = leaf(1, [1]);
    await craft(fileB, [l, internal(2, [], [0]), meta(l.byteLength, 1)]);
    const treeB = await openCrafted(fileB);
    expect(treeB.verify()).toBe(true);
    await treeB.close();
  });
});
