/**
 * Hostile-file tests (C_DATABASE_REVIEW.md §1.5).
 *
 * A database engine must survive arbitrary on-disk bytes. These tests
 * hand-craft structurally valid files whose child pointers form cycles —
 * something no writer produces but any attacker (or bad disk) can — and
 * verify every traversal errors out via its depth cap instead of hanging or
 * recursing to death. Node-level invariant violations (an "internal" node
 * carrying leaf entries) must be rejected at parse time.
 *
 * The C-level fuzz harness (third_party/binjson-structures/test/fuzz.sh)
 * covers randomized corruption of the
 * same paths under ASan/UBSan; these tests pin the deterministic cases and
 * run in every suite pass.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree, RTree } from '../wasm/binjson-structures-wasm.js';
import { ObjectId, Pointer, encode, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM hostile files', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-hostile-${Date.now()}-${counter++}.bj`;
    files.push(n);
    return n;
  };

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  /** Write `records` (arrays of encoded bytes) as a legacy-format file. */
  async function craft(filename, records) {
    const fh = await getFileHandle(root, filename, { create: true });
    const handle = await fh.createSyncAccessHandle();
    let at = 0;
    for (const rec of records) {
      handle.write(rec, { at });
      at += rec.byteLength;
    }
    handle.flush();
    await handle.close();
    return at;
  }

  const meta = (rootPtr, max, min) => encode({
    version: 1,
    maxEntries: max,
    minEntries: min,
    size: 1,
    rootPointer: new Pointer(rootPtr),
    nextId: 2
  });

  it('B+ tree with a self-referential root errors on every operation', async () => {
    // The root is an internal node whose both children point back at itself.
    // Structurally valid (n_children == n_keys + 1), so it parses — only the
    // depth caps stand between each traversal and an infinite loop.
    const file = name();
    const node = encode({
      id: 1,
      isLeaf: false,
      keys: [5],
      values: [],
      children: [new Pointer(0), new Pointer(0)],
      next: null
    });
    const m = meta(0, 4, 1);
    expect(m.byteLength).toBe(135); // must land on the fixed tail size
    const size = await craft(file, [node, m]);

    const fh = await getFileHandle(root, file, { create: false });
    const tree = new BPlusTree(await fh.createSyncAccessHandle(), 4);
    await tree.open(); // metadata alone is well-formed: open succeeds

    await expect(async () => tree.search(5)).rejects.toThrow();
    await expect(async () => tree.toArray()).rejects.toThrow();
    await expect(async () => tree.rangeSearch(0, 10)).rejects.toThrow();
    await expect(async () => tree.add(3, 'x')).rejects.toThrow();
    await expect(async () => tree.delete(5)).rejects.toThrow();
    await expect(collect(tree.iterate())).rejects.toThrow();

    // Failed mutations must roll back cleanly: nothing appended. Read
    // through tree's own still-open handle rather than fileSize()'s fresh
    // one -- node-opfs now enforces OPFS's real single-writer-per-file
    // constraint on the same still-open file.
    expect(tree.syncAccessHandle.getSize()).toBe(size);
    await tree.close();
  });

  it('R-tree with a self-referential root errors on every operation', async () => {
    const file = name();
    const node = encode({
      id: 1,
      isLeaf: false,
      children: [new Pointer(0)],
      bbox: { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 }
    });
    const m = meta(0, 4, 2);
    expect(m.byteLength).toBe(135);
    const size = await craft(file, [node, m]);

    const fh = await getFileHandle(root, file, { create: false });
    const tree = new RTree(await fh.createSyncAccessHandle(), 4);
    await tree.open();

    await expect(async () =>
      tree.searchBBox({ minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 })
    ).rejects.toThrow();
    await expect(async () => tree.searchRadius(0, 0, 100)).rejects.toThrow();
    await expect(async () => tree.insert(1, 2, oid())).rejects.toThrow();
    await expect(async () => tree.remove(oid())).rejects.toThrow();

    expect(tree.syncAccessHandle.getSize()).toBe(size);
    await tree.close();
  });

  it('rejects a node whose isLeaf flag contradicts its children', async () => {
    // Claims internal but carries leaf entries: with the flag trusted, C
    // code would walk a NULL children array. Must fail parse, not crash.
    const file = name();
    const node = encode({
      id: 1,
      isLeaf: false,
      children: [{ bbox: null, lat: 1, lng: 2, objectId: oid() }],
      bbox: { minLat: 1, maxLat: 1, minLng: 2, maxLng: 2 }
    });
    await craft(file, [node, meta(0, 4, 2)]);

    const fh = await getFileHandle(root, file, { create: false });
    const tree = new RTree(await fh.createSyncAccessHandle(), 4);
    await tree.open();
    await expect(async () => tree.insert(1, 2, oid())).rejects.toThrow();
    await tree.close();
  });

  it('rejects a B+ tree node breaking the children/keys invariant', async () => {
    // One key but three children: without the parse-time invariant check,
    // descent indexes past the routing keys.
    const file = name();
    const node = encode({
      id: 1,
      isLeaf: false,
      keys: [5],
      values: [],
      children: [new Pointer(0), new Pointer(0), new Pointer(0)],
      next: null
    });
    await craft(file, [node, meta(0, 4, 1)]);

    const fh = await getFileHandle(root, file, { create: false });
    const tree = new BPlusTree(await fh.createSyncAccessHandle(), 4);
    await tree.open();
    await expect(async () => tree.search(5)).rejects.toThrow();
    await expect(async () => tree.toArray()).rejects.toThrow();
    await tree.close();
  });

  it('rejects non-finite numeric keys (§2.7)', async () => {
    // NaN compares equal to everything in the key comparator, so inserting
    // one would silently overwrite an arbitrary key.
    const file = name();
    const fh = await getFileHandle(root, file, { create: true });
    const tree = new BPlusTree(await fh.createSyncAccessHandle(), 4);
    await tree.open();
    for (let i = 0; i < 20; i++) tree.add(i, `v${i}`);

    expect(() => tree.add(NaN, 'poison')).toThrow();
    expect(() => tree.add(Infinity, 'poison')).toThrow();
    expect(() => tree.add(-Infinity, 'poison')).toThrow();
    expect(() => tree.delete(NaN)).toThrow();
    await expect(async () => tree.search(NaN)).rejects.toThrow();
    expect(() => tree.rangeSearch(NaN, 10)).toThrow();
    await expect(collect(tree.iterate(NaN))).rejects.toThrow();

    // Nothing was overwritten, and ±infinity stays valid as a range bound.
    expect(tree.size()).toBe(20);
    expect(await tree.search(7)).toBe('v7');
    expect(tree.rangeSearch(-Infinity, Infinity).length).toBe(20);
    expect((await collect(tree.iterate(Infinity)))).toEqual([]);
    await tree.close();
  });

  async function collect(iter) {
    const out = [];
    for await (const e of iter) out.push(e);
    return out;
  }

  let oidCounter = 0;
  function oid() {
    return new ObjectId(String(oidCounter++).padStart(24, '0'));
  }
});
