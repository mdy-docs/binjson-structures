/**
 * Snapshot / MVCC tests (C_DATABASE_REVIEW.md §1.8).
 *
 * The B+ tree is append-only and immutable, so every commit boundary in the
 * file is a complete, consistent snapshot that later appends never disturb.
 * snapshot() pins the current root as a read-only handle that stays
 * consistent while the live tree mutates; boundaries() enumerates every
 * verified historical commit; snapshotAt(offset) opens the tree exactly as
 * it was at one of them (time travel). Snapshots support every read API,
 * including compact() — which is online backup: a consistent copy taken
 * while the live tree keeps writing.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree snapshots', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-snapshot-${Date.now()}-${counter++}.bj`;
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

  async function openTree(filename, create = false) {
    const tree = new BPlusTree(await sync(filename, create), 4);
    await tree.open();
    return tree;
  }

  it('snapshots stay consistent while the live tree mutates', async () => {
    const tree = await openTree(name(), true);
    for (let i = 0; i < 50; i++) tree.add(i, `v${i}`);

    const snap = tree.snapshot();
    expect(snap.isSnapshot).toBe(true);
    expect(snap.size()).toBe(50);

    // Mutate the live tree heavily.
    for (let i = 50; i < 120; i++) tree.add(i, `v${i}`);
    for (let i = 0; i < 20; i++) tree.delete(i);
    tree.add(7, 'overwritten'); // 7 was deleted; re-added with new value

    // The snapshot still sees the world exactly as it was.
    expect(snap.size()).toBe(50);
    expect(await snap.search(7)).toBe('v7');
    expect(await snap.search(80)).toBeUndefined();
    expect(snap.toArray().map((e) => e.key)).toEqual([...Array(50).keys()]);
    const range = snap.rangeSearch(10, 14).map((e) => e.value);
    expect(range).toEqual(['v10', 'v11', 'v12', 'v13', 'v14']);

    // And the live tree sees the new world.
    expect(tree.size()).toBe(101); // 50 + 70 - 20 + re-added 7
    expect(await tree.search(7)).toBe('overwritten');
    expect(await tree.search(80)).toBe('v80');

    // Multiple snapshots pin different moments simultaneously.
    const snap2 = tree.snapshot();
    tree.add(500, 'later');
    expect(snap2.size()).toBe(101);
    expect(await snap2.search(500)).toBeUndefined();
    expect(await tree.search(500)).toBe('later');

    await snap.close();
    await snap2.close();
    await tree.close();
  });

  it('snapshots are read-only', async () => {
    const tree = await openTree(name(), true);
    for (let i = 0; i < 10; i++) tree.add(i, i);
    const snap = tree.snapshot();

    expect(() => snap.add(99, 'nope')).toThrow();
    expect(() => snap.delete(3)).toThrow();
    expect(snap.size()).toBe(10);
    expect(tree.size()).toBe(10); // live tree untouched by the attempts

    await snap.close();
    await tree.close();
  });

  it('boundaries() + snapshotAt() time-travel through the whole history', async () => {
    const file = name();
    const tree = await openTree(file, true);
    const N = 12;
    for (let i = 0; i < N; i++) tree.add(i, `v${i}`);
    tree.delete(0);

    const bounds = tree.boundaries();
    // create + N adds + 1 delete = N + 2 commits, sizes 0,1,...,N,N-1.
    expect(bounds.length).toBe(N + 2);
    expect(bounds.map((b) => b.size)).toEqual([...Array(N + 1).keys(), N - 1]);
    expect(bounds.map((b) => b.offset)).toEqual([...bounds.map((b) => b.offset)].sort((a, b) => a - b));

    // Open every historical state and verify it is exactly that prefix.
    for (let k = 0; k <= N; k++) {
      const past = tree.snapshotAt(bounds[k].offset);
      expect(past.size()).toBe(k);
      expect(past.toArray().map((e) => e.key)).toEqual([...Array(k).keys()]);
      await past.close();
    }

    // A non-boundary offset is rejected.
    expect(() => tree.snapshotAt(bounds[3].offset + 1)).toThrow();
    await tree.close();
  });

  it('compacting a snapshot is an online backup', async () => {
    const src = name();
    const dst = name();
    const tree = await openTree(src, true);
    for (let i = 0; i < 200; i++) tree.add(i, `v${i}`);

    const snap = tree.snapshot();
    // The "backup" runs while the live tree keeps taking writes.
    for (let i = 200; i < 260; i++) tree.add(i, `v${i}`);
    await snap.compact(await sync(dst, true));
    for (let i = 260; i < 300; i++) tree.add(i, `v${i}`);
    await snap.close();

    const backup = await openTree(dst);
    expect(backup.size()).toBe(200); // exactly the snapshot state
    expect(await backup.search(199)).toBe('v199');
    expect(await backup.search(200)).toBeUndefined();
    expect(tree.size()).toBe(300);   // live tree unaffected
    await backup.close();
    await tree.close();
  });

  it('works on JS-written legacy files', async () => {
    const file = name();
    // Frozen legacy fixture: order-4 JS tree with keys 0..7 (`v${i}`).
    await writeFixture(await sync(file, true), 'bpt-o4-seq8.bin');

    const tree = await openTree(file);
    const bounds = tree.boundaries();
    expect(bounds.length).toBeGreaterThan(0);
    expect(bounds[bounds.length - 1].size).toBe(8);
    const mid = bounds.find((b) => b.size === 4);
    const past = tree.snapshotAt(mid.offset);
    expect(past.toArray().map((e) => e.key)).toEqual([0, 1, 2, 3]);
    await past.close();
    await tree.close();
  });
});
