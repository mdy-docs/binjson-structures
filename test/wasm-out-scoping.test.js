/**
 * Output-buffer scoping tests (C_DATABASE_REVIEW.md §6).
 *
 * The bplustree and textindex WASM glue used to hold the last result in
 * module-level globals, so an operation on one handle silently clobbered
 * another handle's unread result. Outputs are now scoped to the handle
 * (the tree itself for bplustree, a per-index output slot for textindex,
 * matching what rtree/textlog always did), which these tests pin by
 * driving the raw exports: produce a result on A, operate on B, then read
 * A's result. The *_out_len accessors also now fail loudly (negative
 * error) instead of silently truncating lengths >= 2 GB — not reproducible
 * in a test, but the wrapper checks are exercised on every read here.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree, RTree, TextIndex, ObjectId, decode } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

const M = await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM output buffers are scoped to the handle', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-outscope-${Date.now()}-${counter++}.bj`;
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

  function readOut(ptrFn, lenFn, ctx) {
    const ptr = ptrFn(ctx);
    const len = lenFn(ctx);
    expect(len).toBeGreaterThanOrEqual(0);
    return decode(M.HEAPU8.slice(ptr, ptr + len));
  }

  it('B+ tree: reading A\'s result after operating on B returns A\'s result', async () => {
    const a = new BPlusTree(await sync(name(), true), 4);
    const b = new BPlusTree(await sync(name(), true), 4);
    await a.open();
    await b.open();
    for (let i = 0; i < 20; i++) a.add(i, `a${i}`);
    for (let i = 0; i < 20; i++) b.add(i, `b${i}`);

    // Search A, then B — A's result must still be readable afterwards.
    expect(M._bptw_search(a.ctx, 0, 7, 0, 0)).toBe(1);
    expect(M._bptw_search(b.ctx, 0, 3, 0, 0)).toBe(1);
    expect(readOut(M._bptw_out_ptr, M._bptw_out_len, a.ctx)).toBe('a7');
    expect(readOut(M._bptw_out_ptr, M._bptw_out_len, b.ctx)).toBe('b3');

    // Same for materialized entries across handles.
    expect(M._bptw_entries(a.ctx)).toBe(0);
    expect(M._bptw_entries(b.ctx)).toBe(0);
    const ea = readOut(M._bptw_out_ptr, M._bptw_out_len, a.ctx);
    expect(ea.length).toBe(20);
    expect(ea[0].value).toBe('a0');

    await a.close();
    await b.close();
  });

  it('text index: two open indexes keep independent query results', async () => {
    async function makeIndex(prefix) {
      const trees = {
        index: new BPlusTree(await sync(name(), true), 16),
        documentTerms: new BPlusTree(await sync(name(), true), 16),
        documentLengths: new BPlusTree(await sync(name(), true), 16)
      };
      const ix = new TextIndex({ trees });
      await ix.open();
      await ix.add(`${prefix}-doc`, `unmistakable ${prefix} content`);
      return ix;
    }
    const one = await makeIndex('alpha');
    const two = await makeIndex('beta');

    // Query one, then two, through the raw exports; read one's result last.
    const q = (ix, text) => {
      const bytes = new TextEncoder().encode(text);
      const p = M._malloc(bytes.length);
      M.HEAPU8.set(bytes, p);
      const rc = M._tixw_query(ix.outCtx, ix.index.ctx, ix.documentTerms.ctx,
                               ix.documentLengths.ctx, p, bytes.length);
      M._free(p);
      expect(rc).toBe(0);
    };
    q(one, 'alpha');
    q(two, 'beta');
    const r1 = readOut(M._tixw_out_ptr, M._tixw_out_len, one.outCtx);
    const r2 = readOut(M._tixw_out_ptr, M._tixw_out_len, two.outCtx);
    expect(r1.map((r) => r.id)).toEqual(['alpha-doc']);
    expect(r2.map((r) => r.id)).toEqual(['beta-doc']);

    // And through the wrapper, interleaved.
    expect((await one.query('unmistakable')).map((r) => r.id)).toEqual(['alpha-doc']);
    expect((await two.query('unmistakable')).map((r) => r.id)).toEqual(['beta-doc']);

    await one.close();
    await two.close();
  });

  it('R-tree results were always handle-scoped and still are', async () => {
    const a = new RTree(await sync(name(), true), 9);
    const b = new RTree(await sync(name(), true), 9);
    await a.open();
    await b.open();
    const oid = (n) => new ObjectId(`${n}`.padStart(24, '0'));
    a.insert(10, 10, oid(1));
    b.insert(-20, -20, oid(2));

    const world = { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 };
    expect(M._rtw_search(a.ctx, world.minLat, world.maxLat, world.minLng, world.maxLng)).toBe(0);
    expect(M._rtw_search(b.ctx, world.minLat, world.maxLat, world.minLng, world.maxLng)).toBe(0);
    const ra = readOut(M._rtw_out_ptr, M._rtw_out_len, a.ctx);
    expect(ra.length).toBe(1);
    expect(ra[0].lat).toBe(10);

    await a.close();
    await b.close();
  });
});
