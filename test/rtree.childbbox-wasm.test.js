/**
 * R-tree childBBoxes tests (C_DATABASE_REVIEW.md §3.1).
 *
 * Internal nodes persist a childBBoxes array (one box per child) so
 * choose-subtree, bbox recomputation, splitting and search pruning never
 * load child nodes: an insert reads O(depth) nodes instead of
 * O(depth x fanout), and searches skip non-overlapping subtrees without
 * parsing them. The field is optional on read — nodes written by the pure-JS
 * implementation lack it and fall back to child loads, upgrading whenever
 * rewritten — and the JS implementation ignores it, so files interoperate
 * in both directions.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, RTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { BinJsonFile } from '../third_party/binjson/js/binjson.js';
import { ObjectId, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM R-tree child bounding boxes', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-childbbox-${Date.now()}-${counter++}.bj`;
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

  const oid = (i) => new ObjectId(String(i).padStart(24, '0'));
  const pt = (i) => ({
    lat: ((i * 37) % 170) - 85 + i * 1e-5,
    lng: ((i * 73) % 350) - 175 + i * 1e-5
  });

  it('inserts read O(depth) nodes, searches prune without parsing children', async () => {
    const file = name();
    {
      const t = new RTree(await sync(file, true), 9);
      await t.open();
      for (let i = 0; i < 1000; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));
      await t.close();
    }

    const proxy = counting(await sync(file));
    const t = new RTree(proxy, 9);
    await t.open();

    let base = proxy.stats.reads;
    t.insert(12.34, 56.78, oid(100001));
    const insertReads = proxy.stats.reads - base;
    // Descent path only — with 1000 points at fanout 9 the tree has hundreds
    // of nodes; the old design loaded every child at every level (~dozens to
    // hundreds of reads per insert).
    expect(insertReads).toBeLessThan(15);

    base = proxy.stats.reads;
    const hits = t.searchBBox({ minLat: 12, maxLat: 13, minLng: 56, maxLng: 57 });
    const searchReads = proxy.stats.reads - base;
    expect(hits.some((h) => h.lat === 12.34 && h.lng === 56.78)).toBe(true);
    expect(searchReads).toBeLessThan(20);

    await t.close();
  });

  it('upgrades legacy (JS-written) trees on write and stays correct', async () => {
    const file = name();
    // Frozen fixture: order 9, points pt(0..299), no childBBoxes.
    await writeFixture(await sync(file, true), 'rtree-o9-300.bin');

    const proxy = counting(await sync(file));
    const t = new RTree(proxy, 9);
    await t.open();

    // First insert pays a one-time upgrade of the touched path (legacy nodes
    // must load children once); the second travels the upgraded path.
    let base = proxy.stats.reads;
    t.insert(11.11, 22.22, oid(200001));
    const firstReads = proxy.stats.reads - base;
    base = proxy.stats.reads;
    t.insert(11.12, 22.23, oid(200002));
    const secondReads = proxy.stats.reads - base;
    expect(secondReads).toBeLessThan(firstReads);
    expect(secondReads).toBeLessThan(15);

    expect(t.size()).toBe(302);
    const hits = t.searchBBox({ minLat: 11, maxLat: 12, minLng: 22, maxLng: 23 });
    expect(hits.length).toBe(2);
    await t.close();
  });

  it('childBBoxes-carrying files stay readable by JavaScript at the record level', async () => {
    const file = name();
    {
      const t = new RTree(await sync(file, true), 9);
      await t.open();
      for (let i = 0; i < 120; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));
      await t.close();
    }

    // JavaScript reads the file record by record (binjson.js): every node
    // decodes, internal nodes expose the childBBoxes extension, and the last
    // metadata record reports the live entry count.
    const handle = await sync(file);
    const scanFile = new BinJsonFile(handle);
    let leafEntries = 0;
    let internalWithBoxes = 0;
    let lastMeta = null;
    for (const { value } of scanFile.scan()) {
      if (value && typeof value === 'object' && 'isLeaf' in value) {
        if (value.isLeaf) leafEntries += value.children.length;
        else if (Array.isArray(value.childBBoxes)) {
          expect(value.childBBoxes.length).toBe(value.children.length);
          internalWithBoxes++;
        }
      } else if (value && typeof value === 'object' && 'rootPointer' in value) {
        lastMeta = value;
      }
    }
    await handle.close();
    expect(internalWithBoxes).toBeGreaterThan(0);
    expect(leafEntries).toBeGreaterThanOrEqual(120);   // append-only history
    expect(lastMeta.size).toBe(120);
  });

  it('keeps child boxes consistent through heavy remove/underflow churn', async () => {
    const file = name();
    const t = new RTree(await sync(file, true), 4);   // small fanout: deep tree
    await t.open();
    const N = 200;
    for (let i = 0; i < N; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));
    for (let i = 0; i < N; i += 2) expect(t.remove(oid(i))).toBe(true);
    expect(t.size()).toBe(N / 2);

    // Every remaining point must still be findable through the (rewritten,
    // merged, redistributed) internal nodes' child boxes.
    for (let i = 1; i < N; i += 2) {
      const p = pt(i);
      const hits = t.searchBBox({
        minLat: p.lat - 0.001, maxLat: p.lat + 0.001,
        minLng: p.lng - 0.001, maxLng: p.lng + 0.001
      });
      expect(hits.some((h) => h.objectId.toString() === oid(i).toString())).toBe(true);
    }
    await t.close();
  });

  it('compaction refreshes child boxes and upgrades legacy files', async () => {
    const src = name();
    const dst = name();
    // Frozen fixture: order 9, points pt(0..149), no childBBoxes.
    await writeFixture(await sync(src, true), 'rtree-o9-150.bin');

    const t = new RTree(await sync(src), 9);
    await t.open();
    await t.compact(await sync(dst, true));
    await t.close();

    // The compacted (upgraded) tree serves point queries in a few reads.
    const proxy = counting(await sync(dst));
    const packed = new RTree(proxy, 9);
    await packed.open();
    expect(packed.size()).toBe(150);
    const base = proxy.stats.reads;
    const p = pt(77);
    const hits = packed.searchBBox({
      minLat: p.lat - 0.001, maxLat: p.lat + 0.001,
      minLng: p.lng - 0.001, maxLng: p.lng + 0.001
    });
    expect(hits.length).toBeGreaterThan(0);
    expect(proxy.stats.reads - base).toBeLessThan(15);
    await packed.close();
  });
});
