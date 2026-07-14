/**
 * Geo edge cases and structural contracts (C_DATABASE_REVIEW.md §3.5–§3.7).
 *
 * §3.6 — radius searches split their query box at the antimeridian instead
 * of silently missing points on the other side of ±180°, clamp latitude,
 * and treat a circle that encloses a pole (or a degenerate cos-scaled
 * longitude span) as covering every longitude; the kNN mindist bound is
 * wrap-aware, so best-first never mis-prunes near ±180°.
 * §3.5 — an underflow merge that exceeds node capacity splits back in two
 * (max_entries == 2 makes min == max, the case that used to overflow).
 * §3.7 — OID uniqueness is the caller's contract: duplicates coexist and
 * remove takes out one at a time. Pinned here as documented behavior.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, RTree, haversineDistance } from '../wasm/binjson-structures-wasm.js';
import { ObjectId, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM R-tree geo edges and contracts', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-rtgeo-${Date.now()}-${counter++}.bj`;
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

  const oid = (n) => new ObjectId(n.toString(16).padStart(24, '0'));

  async function makeTree(maxEntries = 4) {
    const tree = new RTree(await sync(name(), true), maxEntries);
    await tree.open();
    return tree;
  }

  it('radius search finds points across the antimeridian', async () => {
    const tree = await makeTree();
    tree.insert(0, 179.9, oid(1));    // ~5.6 km east of the query
    tree.insert(0, -179.9, oid(2));   // ~16.7 km east, across ±180°
    tree.insert(0, 178.5, oid(3));    // ~161 km west: outside
    tree.insert(0, -178.5, oid(4));   // ~172 km east: outside
    tree.insert(20, 0, oid(5));       // far away

    for (const qlng of [179.95, -179.95]) {
      const hits = tree.searchRadius(0, qlng, 25);
      const ids = hits.map((h) => h.objectId.toString()).sort();
      expect(ids).toEqual([oid(1).toString(), oid(2).toString()].sort());
      for (const h of hits) {
        expect(h.distance).toBeCloseTo(haversineDistance(0, qlng, h.lat, h.lng), 9);
        expect(h.distance).toBeLessThanOrEqual(25);
      }
    }
    await tree.close();
  });

  it('radius search near a pole covers every longitude and clamps latitude', async () => {
    const tree = await makeTree();
    const lngs = [0, 90, 179.5, -90, -179.5];
    lngs.forEach((lng, i) => tree.insert(89.9, lng, oid(10 + i)));
    tree.insert(85, 0, oid(20));   // ~550 km from the pole: outside

    const hits = tree.searchRadius(89.95, 0, 30);
    expect(hits.length).toBe(lngs.length);   // every longitude, both "sides"
    for (const h of hits) expect(h.distance).toBeLessThanOrEqual(30);

    // A circle that encloses the pole itself (query box exceeds lat 90).
    const atPole = tree.searchRadius(89.99, 123, 40);
    expect(atPole.length).toBe(lngs.length);
    await tree.close();
  });

  it('kNN never mis-prunes across the antimeridian', async () => {
    const tree = await makeTree();   // fanout 4: clusters land in separate subtrees
    const pts = [];
    for (let i = 0; i < 30; i++) {   // distinct coords: ties would make order ambiguous
      const p = { lat: (i + 1) * 0.011, lng: 179.4 + (i % 5) * 0.02, id: 100 + i };
      pts.push(p);
    }
    for (let i = 0; i < 30; i++) {
      const p = { lat: (i + 1) * 0.017, lng: -179.9 + (i % 5) * 0.02, id: 200 + i };
      pts.push(p);
    }
    for (const p of pts) tree.insert(p.lat, p.lng, oid(p.id));

    // Query sits east of +179.5: the true nearest neighbors are on the
    // *west* (-179.9) side, reachable only across ±180°.
    const qlat = 0, qlng = 179.97;
    const brute = pts
      .map((p) => ({ id: p.id, d: haversineDistance(qlat, qlng, p.lat, p.lng) }))
      .sort((a, b) => a.d - b.d)
      .slice(0, 8);
    const got = tree.nearest(qlat, qlng, 8);
    expect(got.map((g) => g.objectId.toString())).toEqual(
      brute.map((b) => oid(b.id).toString()));
    for (let i = 0; i < got.length; i++) {
      expect(got[i].distance).toBeCloseTo(brute[i].d, 9);
    }
    await tree.close();
  });

  it('max_entries == 2 survives underflow-merge churn (merged nodes re-split)', async () => {
    const tree = await makeTree(2);
    const pts = [];
    for (let i = 0; i < 40; i++) {
      const p = { lat: (i * 7) % 120 - 60, lng: (i * 13) % 340 - 170, id: 300 + i };
      pts.push(p);
      tree.insert(p.lat, p.lng, oid(p.id));
    }
    // Remove most of them in an order that forces repeated merges.
    for (let i = 0; i < 30; i++) {
      const p = pts[(i * 17) % 40];
      if (!p.removed) {
        expect(tree.remove(oid(p.id), p.lat, p.lng)).toBe(true);
        p.removed = true;
      }
    }
    const left = pts.filter((p) => !p.removed);
    expect(tree.size()).toBe(left.length);
    const world = { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 };
    const ids = tree.searchBBox(world).map((h) => h.objectId.toString()).sort();
    expect(ids).toEqual(left.map((p) => oid(p.id).toString()).sort());
    await tree.close();
  });

  it('duplicate OIDs coexist and remove takes one at a time (documented contract)', async () => {
    const tree = await makeTree();
    const dup = oid(999);
    tree.insert(10, 20, dup);
    tree.insert(-30, 40, dup);
    expect(tree.size()).toBe(2);

    const world = { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 };
    const hits = tree.searchBBox(world);
    expect(hits.filter((h) => h.objectId.toString() === dup.toString()).length).toBe(2);

    expect(tree.remove(dup)).toBe(true);
    expect(tree.size()).toBe(1);
    expect(tree.remove(dup)).toBe(true);
    expect(tree.size()).toBe(0);
    expect(tree.remove(dup)).toBe(false);
    await tree.close();
  });
});
