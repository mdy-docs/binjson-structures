/**
 * JS ↔ C/WASM interop over in-memory byte images.
 *
 * The data-structure logic lives only in C/WASM; JavaScript owns the format
 * layer (src/binjson.js). MemoryHandle implements the sync-access-handle
 * contract over a growable buffer, so the same byte image is readable and
 * writable from both sides without a file: WASM structures build/open on it,
 * BinJsonFile scans and decodes it record by record from JS, and toBytes()
 * ships it (postMessage, network, storage) to be reopened anywhere.
 */
import { describe, it, expect } from 'vitest';
import { ready, BPlusTree, RTree } from '../wasm/binjson-structures-wasm.js';
import {
  BinJsonFile, MemoryHandle, ObjectId, Pointer, encode
} from '../third_party/binjson/js/binjson.js';

await ready();

describe('MemoryHandle: JS <-> WASM byte-image interop', () => {
  it('implements the sync-access-handle contract', () => {
    const h = new MemoryHandle();
    expect(h.getSize()).toBe(0);

    h.write(new Uint8Array([1, 2, 3]), { at: 0 });
    expect(h.getSize()).toBe(3);

    // Writing past the end zero-fills the gap (OPFS semantics).
    h.write(new Uint8Array([9]), { at: 6 });
    expect(h.getSize()).toBe(7);
    expect([...h.toBytes()]).toEqual([1, 2, 3, 0, 0, 0, 9]);

    // Reads clamp at the end; reads past it return 0 bytes.
    const buf = new Uint8Array(10);
    expect(h.read(buf, { at: 5 })).toBe(2);
    expect(h.read(buf, { at: 99 })).toBe(0);

    h.truncate(2);
    expect(h.getSize()).toBe(2);
    h.truncate(4);   // growing zero-fills
    expect([...h.toBytes()]).toEqual([1, 2, 0, 0]);

    // Round-trip through bytes.
    const copy = new MemoryHandle(h.toBytes());
    expect([...copy.toBytes()]).toEqual([1, 2, 0, 0]);
  });

  it('WASM builds a B+ tree in memory; JS reads the same image record by record', async () => {
    const mem = new MemoryHandle();
    const tree = new BPlusTree(mem, 4);
    await tree.open();
    for (let i = 0; i < 40; i++) tree.add(i, { n: i, tag: `v${i}` });
    expect(tree.search(17)).toEqual({ n: 17, tag: 'v17' });
    expect(tree.verify()).toBe(true);
    await tree.close();   // MemoryHandle.close is a no-op; bytes remain

    // JavaScript walks the very same buffer with the format-layer tools.
    const file = new BinJsonFile(mem);
    let lastMeta = null;
    let sawLeafWith17 = false;
    for (const { value, offset, size } of file.scan()) {
      expect(size).toBeGreaterThan(0);
      expect(typeof offset).toBe('number');
      if (value && typeof value === 'object' && 'rootPointer' in value) lastMeta = value;
      if (value && typeof value === 'object' && value.isLeaf &&
          value.keys.includes(17)) sawLeafWith17 = true;
    }
    expect(lastMeta.size).toBe(40);
    expect(sawLeafWith17).toBe(true);

    // Random access too: the root node decodes at the metadata's pointer.
    const rootNode = file.read(lastMeta.rootPointer);
    expect(rootNode.id).toBeDefined();
    expect('isLeaf' in rootNode).toBe(true);
  });

  it('byte images ship between sides: toBytes -> new MemoryHandle -> reopen', async () => {
    const mem = new MemoryHandle();
    {
      const tree = new BPlusTree(mem, 4);
      await tree.open();
      for (let i = 0; i < 25; i++) tree.add(`key-${i}`, i * 3);
      await tree.close();
    }

    // Snapshot the image (as if postMessage'd or fetched) and reopen it.
    const shipped = mem.toBytes();
    const tree = new BPlusTree(new MemoryHandle(shipped), 4);
    await tree.open();
    expect(tree.size()).toBe(25);
    expect(tree.search('key-7')).toBe(21);
    tree.add('key-new', -1);   // and it stays fully functional
    expect(tree.size()).toBe(26);
    expect(tree.verify()).toBe(true);
    await tree.close();
  });

  it('JS crafts a legacy tree image in memory that WASM opens', async () => {
    // JavaScript writes records directly (encode): a single-leaf tree in
    // the headerless legacy format.
    const mem = new MemoryHandle();
    const leaf = encode({
      id: 1, isLeaf: true, keys: ['from-js'], values: [{ ok: true }],
      children: [], next: null
    });
    const meta = encode({
      version: 1, maxEntries: 4, minEntries: 1, size: 1,
      rootPointer: new Pointer(0), nextId: 2
    });
    mem.write(leaf, { at: 0 });
    mem.write(meta, { at: leaf.byteLength });

    const tree = new BPlusTree(mem, 4);
    await tree.open();
    expect(tree.size()).toBe(1);
    expect(tree.search('from-js')).toEqual({ ok: true });
    expect(tree.verify()).toBe(true);
    await tree.close();
  });

  it('R-tree works over memory images too', async () => {
    const mem = new MemoryHandle();
    const rt = new RTree(mem, 9);
    await rt.open();
    const oid = (n) => new ObjectId(`${n}`.padStart(24, '0'));
    for (let i = 0; i < 50; i++) rt.insert(i - 25, (i * 7) % 180 - 90, oid(i));
    expect(rt.size()).toBe(50);
    const hits = rt.searchBBox({ minLat: -5, maxLat: 5, minLng: -180, maxLng: 180 });
    expect(hits.length).toBeGreaterThan(0);
    await rt.close();

    const reopened = new RTree(new MemoryHandle(mem.toBytes()), 9);
    await reopened.open();
    expect(reopened.size()).toBe(50);
    await reopened.close();
  });
});
