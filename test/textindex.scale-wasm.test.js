/**
 * TextIndex dict hash-map tests (C_DATABASE_REVIEW.md §4.1).
 *
 * The internal string->number dictionary backing postings, term maps, scores
 * and length maps now carries a linear-probing hash index over its
 * insertion-ordered entries, so lookups are O(1) instead of a linear scan:
 * decoding an n-entry posting is O(n) instead of O(n²), and accumulating
 * scores over P posting entries is O(P) instead of O(P·S). Iteration order is
 * unchanged (insertion order, matching the JS reference's Map semantics), so
 * encoded blobs and result ordering stayed byte-identical to the (since
 * removed) JS implementation. These tests exercise the hash map at posting
 * sizes where the old scan was quadratic and the reindex path after
 * removals.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextIndex at scale (hash-map dict)', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const base = () => `test-tixscale-${Date.now()}-${counter++}`;
  const files = [];

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  async function makeIndex(Index, Tree, name) {
    async function tree(suffix) {
      const filename = `${name}-${suffix}.bj`;
      files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      return new Tree(await fh.createSyncAccessHandle(), 16);
    }
    const idx = new Index({
      order: 16,
      trees: {
        index: await tree('terms'),
        documentTerms: await tree('documents'),
        documentLengths: await tree('lengths')
      }
    });
    await idx.open();
    return idx;
  }

  // Every doc shares three terms (postings grow to N entries — the sizes at
  // which the old linear-scan dict went quadratic) plus unique and group terms.
  const docText = (i) =>
    `shared common corpus unique${i}x unique${i}y group${i % 10} quarter${i % 4}`;

  it('handles postings with a thousand entries: index, query, remove', async () => {
    const idx = await makeIndex(TextIndex, BPlusTree, base());
    const N = 1000;
    for (let i = 0; i < N; i++) await idx.add(`doc-${i}`, docText(i));

    // The shared term's posting has N entries; scoring touches all of them.
    let hits = await idx.query('shared');
    expect(hits.length).toBe(N);

    // Group queries rank the 100 docs containing the group term first.
    hits = await idx.query(`shared group7`);
    expect(hits.length).toBe(N);
    const top = hits.slice(0, N / 10).map((h) => h.id);
    for (const id of top) expect(Number(id.slice(4)) % 10).toBe(7);

    // Removal rewrites every posting the doc appears in (dict_remove +
    // reindex on ~N-entry dicts) and must leave the rest intact.
    for (let i = 0; i < N; i += 4) expect(await idx.remove(`doc-${i}`)).toBe(true);
    hits = await idx.query('shared');
    expect(hits.length).toBe(N - N / 4);
    expect(hits.some((h) => Number(h.id.slice(4)) % 4 === 0)).toBe(false);
    expect(await idx.query(`unique8x`)).toHaveLength(0);      // doc-8 removed
    expect((await idx.query(`unique9x`))[0].id).toBe('doc-9'); // doc-9 intact
    await idx.close();
  });

  it('requireAll intersects large candidate sets in first-posting order', async () => {
    const idx = await makeIndex(TextIndex, BPlusTree, base());
    const N = 600;
    for (let i = 0; i < N; i++) await idx.add(`doc-${i}`, docText(i));

    // 600 candidates from "shared" intersected down to 30: i ≡ 3 (mod 10) and
    // i ≡ 1 (mod 4) means i ≡ 13 (mod 20).
    const ids = await idx.query('shared group3 quarter1', { requireAll: true });
    expect(ids.length).toBe(30);
    const expected = [];
    for (let i = 0; i < N; i++) if (i % 10 === 3 && i % 4 === 1) expected.push(`doc-${i}`);
    expect(ids).toEqual(expected); // insertion order of the first posting

    expect(await idx.query('shared nosuchterm', { requireAll: true })).toEqual([]);
    await idx.close();
  });

});
