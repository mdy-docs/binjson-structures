/**
 * C-side compaction tests (beyond the shared behavioral suite).
 *
 * bpt_compact (c/bplustree.c) rebuilds the tree with a streaming bulk load:
 * entries are packed into full nodes level by level and written straight to
 * the destination file. The output is minimal — one node per packed group,
 * one metadata record, no append-only history and no deletion cruft — which
 * these tests pin down: compacting an already-compacted tree yields an
 * identical file size, and the packed file stays readable by the pure-JS
 * implementation (it contains the durability header/trailer records).
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { BinJsonFile, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM B+ tree bulk-load compaction', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-compaction-${Date.now()}-${counter++}.bj`;
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

  async function openTree(filename, order = 4, create = false) {
    const tree = new BPlusTree(await sync(filename, create), order);
    await tree.open();
    return tree;
  }

  it('compacts a churned tree to a fraction of its size, preserving entries', async () => {
    const src = name();
    const dst = name();
    const tree = await openTree(src, 4, true);
    for (let i = 0; i < 200; i++) tree.add(i, `value${i}`);
    for (let i = 0; i < 100; i++) tree.delete(i);           // leaves cruft
    for (let i = 0; i < 100; i += 2) tree.add(i, `again${i}`);
    const expected = tree.toArray();

    const dstHandle = await sync(dst, true);
    const { oldSize, newSize, bytesSaved } = await tree.compact(dstHandle);
    await tree.close();

    expect(newSize).toBeLessThan(oldSize / 5);
    expect(bytesSaved).toBe(oldSize - newSize);

    const packed = await openTree(dst, 4);
    expect(packed.toArray()).toEqual(expected);
    expect(packed.size()).toBe(expected.length);
    expect(packed.search(150)).toBe('value150');
    expect(packed.search(98)).toBe('again98');
    expect(packed.search(99)).toBeUndefined();              // deleted, not re-added

    // The packed tree is a normal tree: mutations keep working.
    packed.add(99, 'back');
    packed.delete(150);
    expect(packed.search(99)).toBe('back');
    expect(packed.search(150)).toBeUndefined();
    await packed.close();
  });

  it('is a fixpoint: compacting a compacted tree yields an identical size', async () => {
    const src = name();
    const dst1 = name();
    const dst2 = name();
    const tree = await openTree(src, 4, true);
    for (let i = 0; i < 300; i++) tree.add(`key-${String(i).padStart(4, '0')}`, { i });
    for (let i = 0; i < 150; i += 3) tree.delete(`key-${String(i).padStart(4, '0')}`);
    await tree.compact(await sync(dst1, true));
    await tree.close();

    const once = await openTree(dst1, 4);
    const entriesOnce = once.toArray();
    const { oldSize, newSize } = await once.compact(await sync(dst2, true));
    await once.close();

    // A bulk-loaded file contains no reclaimable bytes: recompaction is a
    // no-op size-wise (the old wrapper's rebuild-by-insertion wrote a path
    // copy plus a metadata record per entry, so it could never converge).
    expect(newSize).toBe(oldSize);

    const twice = await openTree(dst2, 4);
    expect(twice.toArray()).toEqual(entriesOnce);
    await twice.close();
  });

  it('produces a file JavaScript reads record by record', async () => {
    const src = name();
    const dst = name();
    const tree = await openTree(src, 4, true);
    for (let i = 0; i < 50; i++) tree.add(`k${i}`, { n: i });
    await tree.compact(await sync(dst, true));
    await tree.close();

    // Record-level read from JS (binjson.js): the packed file holds nodes,
    // exactly one metadata record (no append-only history), and the entries
    // are all there — including k7's value, decoded straight from its leaf.
    const handle = await sync(dst);
    const file = new BinJsonFile(handle);
    let metas = 0;
    let leafKeys = [];
    let k7 = null;
    for (const { value } of file.scan()) {
      if (value && typeof value === 'object' && 'rootPointer' in value) {
        metas++;
        expect(value.size).toBe(50);
      } else if (value && typeof value === 'object' && value.isLeaf) {
        leafKeys = leafKeys.concat(value.keys);
        const at = value.keys.indexOf('k7');
        if (at >= 0) k7 = value.values[at];
      }
    }
    await handle.close();
    expect(metas).toBe(1);
    expect(leafKeys.length).toBe(50);
    expect(k7).toEqual({ n: 7 });
  });

  it('compacts an empty tree', async () => {
    const src = name();
    const dst = name();
    const tree = await openTree(src, 4, true);
    tree.add('only', 1);
    tree.delete('only');
    await tree.compact(await sync(dst, true));
    await tree.close();

    const packed = await openTree(dst, 4);
    expect(packed.size()).toBe(0);
    expect(packed.isEmpty()).toBe(true);
    packed.add('fresh', 2);
    expect(packed.search('fresh')).toBe(2);
    await packed.close();
  });

  it('handles a larger tree across several packed levels', async () => {
    const src = name();
    const dst = name();
    const tree = await openTree(src, 4, true);   // order 4: many levels
    const N = 1000;
    for (let i = 0; i < N; i++) tree.add(i, `v${i}`);
    await tree.compact(await sync(dst, true));
    await tree.close();

    const packed = await openTree(dst, 4);
    expect(packed.size()).toBe(N);
    expect(packed.getHeight()).toBeGreaterThan(2);
    for (const probe of [0, 1, 499, 500, 998, 999]) {
      expect(packed.search(probe)).toBe(`v${probe}`);
    }
    const entries = packed.toArray();
    expect(entries.length).toBe(N);
    for (let i = 0; i < N; i++) expect(entries[i].key).toBe(i);  // sorted
    await packed.close();
  });
});
