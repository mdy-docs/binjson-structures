/**
 * Spatial cursor + k-nearest-neighbor tests (C_DATABASE_REVIEW.md §3.3).
 *
 * iterateBBox streams bounding-box matches with O(height) state — a descent
 * stack plus one leaf — pinned to the root at open (append-only snapshot
 * semantics), so early termination reads a fraction of the tree and
 * concurrent mutations are invisible to a running cursor. nearest() is
 * best-first over node bounding boxes: it pops the closest node/entry from
 * a distance heap and reads only subtrees that can still beat the current
 * candidates, instead of scanning a search box.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, RTree, haversineDistance } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { ObjectId, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM R-tree spatial cursor and kNN', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-rtcursor-${Date.now()}-${counter++}.bj`;
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
    for await (const e of iter) out.push(e);
    return out;
  }

  const oid = (i) => new ObjectId(String(i).padStart(24, '0'));
  const pt = (i) => ({
    lat: ((i * 37) % 170) - 85 + i * 1e-5,
    lng: ((i * 73) % 350) - 175 + i * 1e-5
  });
  const WORLD = { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 };

  it('full iteration equals searchBBox, order included', async () => {
    const tree = new RTree(await sync(name(), true), 9);
    await tree.open();
    for (let i = 0; i < 800; i++) tree.insert(pt(i).lat, pt(i).lng, oid(i));

    const box = { minLat: -40, maxLat: 40, minLng: -90, maxLng: 90 };
    const streamed = await collect(tree.iterateBBox(box));
    const materialized = tree.searchBBox(box);
    expect(streamed.length).toBeGreaterThan(50);
    expect(streamed.map((e) => e.objectId.toString()))
      .toEqual(materialized.map((e) => e.objectId.toString()));

    expect(await collect(tree.iterateBBox({ minLat: 89.9, maxLat: 90, minLng: 179.9, maxLng: 180 }))).toEqual([]);
    await tree.close();
  });

  it('early termination reads a fraction of the tree', async () => {
    const file = name();
    {
      const t = new RTree(await sync(file, true), 9);
      await t.open();
      for (let i = 0; i < 3000; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));
      await t.close();
    }
    const proxy = counting(await sync(file));
    const tree = new RTree(proxy, 9);
    await tree.open();

    const base = proxy.stats.reads;
    const first = [];
    for await (const e of tree.iterateBBox(WORLD)) {
      first.push(e);
      if (first.length === 10) break;
    }
    const iterReads = proxy.stats.reads - base;
    expect(first.length).toBe(10);

    const b2 = proxy.stats.reads;
    tree.searchBBox(WORLD);
    const fullReads = proxy.stats.reads - b2;
    expect(iterReads).toBeLessThan(fullReads / 5);
    await tree.close();
  });

  it('iterates a consistent snapshot while the tree mutates', async () => {
    const tree = new RTree(await sync(name(), true), 4);
    await tree.open();
    for (let i = 0; i < 150; i++) tree.insert(pt(i).lat, pt(i).lng, oid(i));

    const it1 = tree.iterateBBox(WORLD)[Symbol.asyncIterator]();
    const first = await it1.next();          // cursor now open and pinned
    expect(first.done).toBe(false);

    tree.insert(0.123, 0.456, oid(9001));    // mutate mid-iteration
    tree.remove(oid(140), pt(140).lat, pt(140).lng);

    const rest = [first.value];
    for (;;) {
      const r = await it1.next();
      if (r.done) break;
      rest.push(r.value);
    }
    expect(rest.length).toBe(150);           // the pinned snapshot
    expect(rest.some((e) => e.objectId.toString() === oid(9001).toString())).toBe(false);
    expect(rest.some((e) => e.objectId.toString() === oid(140).toString())).toBe(true);

    expect((await collect(tree.iterateBBox(WORLD))).length).toBe(150); // fresh view: +1 -1
    await tree.close();
  });

  it('nearest() matches brute force and comes back sorted', async () => {
    const tree = new RTree(await sync(name(), true), 9);
    await tree.open();
    const N = 600;
    for (let i = 0; i < N; i++) tree.insert(pt(i).lat, pt(i).lng, oid(i));

    const q = { lat: 12.3, lng: -45.6 };
    const brute = [];
    for (let i = 0; i < N; i++) {
      brute.push({ id: oid(i).toString(), d: haversineDistance(q.lat, q.lng, pt(i).lat, pt(i).lng) });
    }
    brute.sort((a, b) => a.d - b.d);

    for (const k of [1, 10, 50]) {
      const hits = tree.nearest(q.lat, q.lng, k);
      expect(hits.length).toBe(k);
      expect(hits.map((h) => h.objectId.toString())).toEqual(brute.slice(0, k).map((b) => b.id));
      for (let i = 0; i < hits.length; i++) {
        expect(hits[i].distance).toBeCloseTo(brute[i].d, 6);
        if (i > 0) expect(hits[i].distance).toBeGreaterThanOrEqual(hits[i - 1].distance);
      }
    }
    expect(tree.nearest(q.lat, q.lng, N + 50).length).toBe(N); // k > size
    expect(tree.nearest(q.lat, q.lng, 0)).toEqual([]);
    await tree.close();
  });

  it('nearest() reads only the subtrees that can compete', async () => {
    const file = name();
    {
      const t = new RTree(await sync(file, true), 9);
      await t.open();
      for (let i = 0; i < 3000; i++) t.insert(pt(i).lat, pt(i).lng, oid(i));
      await t.close();
    }
    const proxy = counting(await sync(file));
    const tree = new RTree(proxy, 9);
    await tree.open();

    const base = proxy.stats.reads;
    const hits = tree.nearest(30, 60, 5);
    const knnReads = proxy.stats.reads - base;
    expect(hits.length).toBe(5);

    const b2 = proxy.stats.reads;
    tree.searchBBox(WORLD);
    const fullReads = proxy.stats.reads - b2;
    expect(knnReads).toBeLessThan(fullReads / 5);
    await tree.close();
  });

  it('works on legacy JS-written trees', async () => {
    const file = name();
    // Frozen fixture: order 9, points pt(0..199) — written by the removed
    // pure-JS implementation (no childBBoxes).
    await writeFixture(await sync(file, true), 'rtree-o9-200.bin');

    const tree = new RTree(await sync(file), 9);
    await tree.open();
    const streamed = await collect(tree.iterateBBox(WORLD));
    expect(streamed.length).toBe(200);
    const near = tree.nearest(pt(42).lat, pt(42).lng, 1);
    expect(near[0].objectId.toString()).toBe(oid(42).toString());
    expect(near[0].distance).toBeCloseTo(0, 6);
    await tree.close();
  });
});
