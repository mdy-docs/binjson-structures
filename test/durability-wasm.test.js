/**
 * Durability & crash-recovery tests for the WASM (C) structures.
 *
 * The C file layer (c/bjfile.c) writes a type/format header record at offset
 * 0 of new files and ends every commit with a CRC32 trailer record placed
 * just before the metadata record. Open verifies the tail commit and, when
 * anything is wrong, runs a recovery scan: a torn tail is truncated back to
 * the last good commit, while verifiable commits beyond a damaged region
 * refuse to open (truncating would destroy intact data). These records stay
 * invisible to the JS reference implementations, which read metadata at a
 * fixed tail offset and ignore unknown records — the interop tests at the
 * bottom prove that both directions still work.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree, RTree, TextLog } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { BinJsonFile, ObjectId, Pointer, encode, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM durability & crash recovery', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-durability-${Date.now()}-${counter++}.bj`;
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

  async function fileSize(filename) {
    const h = await sync(filename);
    const n = h.getSize();
    await h.close();
    return n;
  }

  /** Cut `n` bytes off the end of the file (simulates a torn append). */
  async function chopTail(filename, n) {
    const h = await sync(filename);
    h.truncate(h.getSize() - n);
    h.flush();
    await h.close();
  }

  /** Append `n` junk bytes (simulates a crash that extended the file). */
  async function appendGarbage(filename, n) {
    const h = await sync(filename);
    const junk = new Uint8Array(n).fill(0xff);
    h.write(junk, { at: h.getSize() });
    h.flush();
    await h.close();
  }

  /** Flip one byte at `offset` (simulates bit rot / a bad sector). */
  async function flipByte(filename, offset) {
    const h = await sync(filename);
    const b = new Uint8Array(1);
    h.read(b, { at: offset });
    b[0] ^= 0xff;
    h.write(b, { at: offset });
    h.flush();
    await h.close();
  }

  async function makeTree(filename, keys, create = true) {
    const tree = new BPlusTree(await sync(filename, create), 4);
    await tree.open();
    for (const k of keys) tree.add(k, { key: k, payload: `value of ${k}` });
    await tree.close();
  }

  const METADATA_SIZE = 135;
  const TRAILER_SIZE = 17;

  describe('file header', () => {
    it('identifies new B+ tree files at offset 0', async () => {
      const file = name();
      await makeTree(file, ['a']);
      const h = await sync(file);
      const header = new BinJsonFile(h).read(new Pointer(0));
      await h.close();
      expect(header).toEqual({ binjson: 'bplustree', fmt: 1 });
    });

    it('refuses to open a file of the wrong type', async () => {
      const file = name();
      const rt = new RTree(await sync(file, true), 4);
      await rt.open();
      rt.insert(10, 20, new ObjectId('5f1d7f3a0b0c0d0e0f101112'));
      await rt.close();

      const tree = new BPlusTree(await sync(file), 4);
      await expect(tree.open()).rejects.toThrow('Invalid tree file');

      const rt2 = new RTree(await sync(file), 4);
      await rt2.open();
      expect(rt2.size()).toBe(1);
      await rt2.close();
    });

    it('refuses to open a B+ tree file as an R-tree', async () => {
      const file = name();
      await makeTree(file, ['a', 'b']);
      const rt = new RTree(await sync(file), 4);
      await expect(rt.open()).rejects.toThrow('Invalid R-tree file');
    });
  });

  describe('B+ tree torn-tail recovery', () => {
    it('recovers to the previous commit when the last append is torn', async () => {
      const file = name();
      await makeTree(file, ['alpha', 'beta', 'gamma', 'delta']);
      const fullSize = await fileSize(file);

      // Tear the tail mid-commit: the final metadata record loses 10 bytes.
      await chopTail(file, 10);

      const tree = new BPlusTree(await sync(file), 4);
      await tree.open();
      // The commit for 'delta' was torn away; everything before survives.
      expect(tree.search('alpha')).toEqual({ key: 'alpha', payload: 'value of alpha' });
      expect(tree.search('beta')).toBeDefined();
      expect(tree.search('gamma')).toBeDefined();
      expect(tree.search('delta')).toBeUndefined();
      expect(tree.size()).toBe(3);

      // Recovery truncated the orphan bytes, and the tree keeps working.
      tree.add('delta', { key: 'delta', payload: 'value of delta' });
      await tree.close();
      expect(await fileSize(file)).toBeLessThan(fullSize + 200);

      const again = new BPlusTree(await sync(file), 4);
      await again.open();
      expect(again.size()).toBe(4);
      expect(again.search('delta')).toEqual({ key: 'delta', payload: 'value of delta' });
      await again.close();
    });

    it('recovers when garbage was appended after the last commit', async () => {
      const file = name();
      await makeTree(file, ['k1', 'k2', 'k3']);
      const goodSize = await fileSize(file);
      await appendGarbage(file, 400);

      const tree = new BPlusTree(await sync(file), 4);
      await tree.open();
      expect(tree.size()).toBe(3);
      expect(tree.search('k3')).toBeDefined();
      await tree.close();
      expect(await fileSize(file)).toBe(goodSize);
    });

    it('detects a bit flip in the last commit via its CRC', async () => {
      const file = name();
      await makeTree(file, ['one', 'two', 'three', 'four', 'five']);
      const size = await fileSize(file);

      // Flip a byte inside the last commit's node records — the record still
      // sits in a structurally plausible stream, so only the CRC catches it.
      await flipByte(file, size - METADATA_SIZE - TRAILER_SIZE - 20);

      const tree = new BPlusTree(await sync(file), 4);
      await tree.open();
      // The corrupted commit ('five') was rolled back.
      expect(tree.size()).toBe(4);
      expect(tree.search('four')).toBeDefined();
      expect(tree.search('five')).toBeUndefined();
      await tree.close();
    });

    it('refuses to open when intact commits exist beyond a damaged region', async () => {
      const file = name();
      await makeTree(file, ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h']);

      // Damage an early commit (just after the ~50-byte header record) AND
      // break the tail so the recovery scan runs. The scan stops at the
      // damage, finds CRC-verifiable commits beyond it, and must refuse:
      // truncating there would silently destroy the later good commits.
      await flipByte(file, 60);
      await chopTail(file, 5);

      const tree = new BPlusTree(await sync(file), 4);
      await expect(tree.open()).rejects.toThrow('Invalid tree file');
    });
  });

  describe('R-tree torn-tail recovery', () => {
    it('recovers to the previous commit when the last append is torn', async () => {
      const file = name();
      const oids = [
        new ObjectId('5f1d7f3a0b0c0d0e0f101112'),
        new ObjectId('6a6b6c6d6e6f707172737475'),
        new ObjectId('7a7b7c7d7e7f808182838485')
      ];
      const rt = new RTree(await sync(file, true), 4);
      await rt.open();
      rt.insert(10, 10, oids[0]);
      rt.insert(20, 20, oids[1]);
      rt.insert(30, 30, oids[2]);
      await rt.close();

      await chopTail(file, 10);

      const rt2 = new RTree(await sync(file), 4);
      await rt2.open();
      expect(rt2.size()).toBe(2);
      const hits = rt2.searchBBox({ minLat: 0, maxLat: 50, minLng: 0, maxLng: 50 });
      expect(hits.map((h) => h.lat).sort((a, b) => a - b)).toEqual([10, 20]);
      rt2.insert(30, 30, oids[2]);
      expect(rt2.size()).toBe(3);
      await rt2.close();
    });
  });

  describe('text log torn-tail recovery', () => {
    it('recovers to the previous version when the last append is torn', async () => {
      const file = name();
      const log = new TextLog(await sync(file, true), 3);
      await log.open();
      for (let v = 1; v <= 5; v++) await log.addVersion(`content of version ${v}\nline two\n`);
      await log.close();

      await chopTail(file, 10);

      const log2 = new TextLog(await sync(file), 3);
      await log2.open();
      expect(log2.getCurrentVersion()).toBe(4);
      expect(await log2.getVersion(4)).toBe('content of version 4\nline two\n');
      expect(await log2.getVersion(1)).toBe('content of version 1\nline two\n');
      const v5 = await log2.addVersion('content of version 5 again\n');
      expect(v5).toBe(5);
      expect(await log2.getVersion(5)).toBe('content of version 5 again\n');
      await log2.close();
    });

    it('detects a bit flip in the last commit via its CRC', async () => {
      const file = name();
      const log = new TextLog(await sync(file, true), 10);
      await log.open();
      await log.addVersion('first version text\n');
      await log.addVersion('second version text\n');
      await log.close();
      const size = await fileSize(file);

      await flipByte(file, size - METADATA_SIZE - TRAILER_SIZE - 20);

      const log2 = new TextLog(await sync(file), 10);
      await log2.open();
      expect(log2.getCurrentVersion()).toBe(1);
      expect(await log2.getVersion(1)).toBe('first version text\n');
      await log2.close();
    });
  });

  describe('legacy (JS-written) commit interop', () => {
    it('a trailer-less commit appended from JS is accepted as a legacy commit', async () => {
      const file = name();
      await makeTree(file, ['w1', 'w2', 'w3']);

      // JavaScript appends records directly (binjson.js encode): a fresh
      // single-leaf root plus a metadata record, with no CRC trailer —
      // byte-for-byte what the removed JS implementation's commits looked
      // like. Legacy readers never saw headers or trailers either way.
      const handle = await sync(file);
      const rootOff = handle.getSize();
      const leaf = encode({
        id: 90, isLeaf: true, keys: ['j1'],
        values: [{ key: 'j1', payload: 'value of j1' }],
        children: [], next: null
      });
      const meta = encode({
        version: 1, maxEntries: 4, minEntries: 1, size: 1,
        rootPointer: new Pointer(rootOff), nextId: 91
      });
      handle.write(leaf, { at: rootOff });
      handle.write(meta, { at: rootOff + leaf.byteLength });
      handle.flush();
      await handle.close();

      // WASM reopens the mixed file: the JS-written tail commit carries no
      // trailer and is accepted as a legacy commit; mutations keep working.
      const back = new BPlusTree(await sync(file), 4);
      await back.open();
      expect(back.size()).toBe(1);
      expect(back.search('j1')).toEqual({ key: 'j1', payload: 'value of j1' });
      back.add('w4', { key: 'w4', payload: 'value of w4' });
      expect(back.size()).toBe(2);
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('WASM opens a JS-written tree (no header, no trailers)', async () => {
      const file = name();
      // Frozen fixture: order 4, one add('legacy', { from: 'js' }).
      await writeFixture(await sync(file, true), 'bpt-o4-legacy1.bin');

      const tree = new BPlusTree(await sync(file), 4);
      await tree.open();
      expect(tree.search('legacy')).toEqual({ from: 'js' });
      await tree.close();
    });
  });
});
