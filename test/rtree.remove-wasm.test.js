/**
 * Located-removal tests (C_DATABASE_REVIEW.md §3.2).
 *
 * OIDs have no spatial locality, so removing by OID alone probes subtrees
 * in insertion order — worst-case a full-tree scan. rtree_remove_at takes
 * the entry's stored coordinates and prunes the descent with child bounding
 * boxes, reading O(height) nodes on well-separated trees. The blind remove
 * remains for callers that don't have the point.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, RTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { ObjectId, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM R-tree located removal', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-rtremove-${Date.now()}-${counter++}.bj`;
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

  it('a located remove reads O(height); a blind remove probes', async () => {
    const file = name();
    {
      const t = new RTree(await sync(file, true), 9);
      await t.open();
      for (let i = 0; i < 4000; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));
      await t.close();
    }

    const proxy = counting(await sync(file));
    const t = new RTree(proxy, 9);
    await t.open();

    // Located: descend only where the point can live.
    let base = proxy.stats.reads;
    expect(t.remove(oid(3777), pt(3777).lat, pt(3777).lng)).toBe(true);
    const located = proxy.stats.reads - base;

    // Blind: probes subtrees in order until the OID turns up.
    base = proxy.stats.reads;
    expect(t.remove(oid(3778))).toBe(true);
    const blind = proxy.stats.reads - base;

    expect(located).toBeLessThan(30);
    expect(blind).toBeGreaterThan(located * 5);
    expect(t.size()).toBe(3998);
    await t.close();
  });

  it('a wrong point removes nothing and leaves the entry intact', async () => {
    const file = name();
    const t = new RTree(await sync(file, true), 4);
    await t.open();
    for (let i = 0; i < 200; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));

    expect(t.remove(oid(50), pt(50).lat + 45, pt(50).lng)).toBe(false);
    expect(t.size()).toBe(200);
    const p = pt(50);
    const hits = t.searchBBox({
      minLat: p.lat - 0.001, maxLat: p.lat + 0.001,
      minLng: p.lng - 0.001, maxLng: p.lng + 0.001
    });
    expect(hits.some((h) => h.objectId.toString() === oid(50).toString())).toBe(true);

    // The right point still removes it.
    expect(t.remove(oid(50), p.lat, p.lng)).toBe(true);
    expect(t.size()).toBe(199);
    await t.close();
  });

  it('located removal survives underflow churn and legacy files', async () => {
    // Legacy JS-written tree (frozen fixture: order 4, points pt(0..149)):
    // no childBBoxes, so pruning falls back to child loads but must stay
    // correct.
    const file = name();
    await writeFixture(await sync(file, true), 'rtree-o4-150.bin');

    const t = new RTree(await sync(file), 4);
    await t.open();
    // Remove enough to force merges/redistribution along the way.
    for (let i = 0; i < 150; i += 2) {
      expect(t.remove(oid(i), pt(i).lat, pt(i).lng)).toBe(true);
    }
    expect(t.size()).toBe(75);
    for (let i = 1; i < 150; i += 2) {
      const p = pt(i);
      const hits = t.searchBBox({
        minLat: p.lat - 0.001, maxLat: p.lat + 0.001,
        minLng: p.lng - 0.001, maxLng: p.lng + 0.001
      });
      expect(hits.some((h) => h.objectId.toString() === oid(i).toString())).toBe(true);
    }
    await t.close();
  });
});
