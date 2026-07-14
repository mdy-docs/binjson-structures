/**
 * Block-partitioned postings tests (C_DATABASE_REVIEW.md §4.2).
 *
 * The C text index stores each term's posting list as fixed-capacity blocks
 * ("term\0" header + "term\0<hex>" blocks) so adding a document rewrites one
 * bounded block instead of the whole list — the legacy single-blob layout
 * appended O(d²) total bytes for a term matching d documents. The legacy
 * layout (what the pure-JS implementation writes) is still read transparently
 * and migrated to blocks the first time a term is written.
 *
 * These tests pin the file-growth behavior, the legacy-read/migrate path
 * (C opening JS-written index files), and that observable semantics —
 * including re-adding documents whose entries live in old blocks — stay
 * identical to the JS reference.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextIndex block-partitioned postings', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const base = () => `test-tixblocks-${Date.now()}-${counter++}`;

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

  async function fileSize(filename) {
    const fh = await getFileHandle(root, filename, { create: false });
    const h = await fh.createSyncAccessHandle();
    const n = h.getSize();
    await h.close();
    return n;
  }

  const docText = (i) => `shared common corpus unique${i}x group${i % 10}`;

  it('terms file grows linearly, not quadratically, on shared terms', async () => {
    const name = base();
    const idx = await makeIndex(TextIndex, BPlusTree, name);
    const N = 600;
    for (let i = 0; i < N / 2; i++) await idx.add(`doc-${i}`, docText(i));
    await idx.close();
    const half = await fileSize(`${name}-terms.bj`);

    const reopened = await makeIndex(TextIndex, BPlusTree, name);
    for (let i = N / 2; i < N; i++) await reopened.add(`doc-${i}`, docText(i));
    await reopened.close();
    const full = await fileSize(`${name}-terms.bj`);

    // Linear growth: the second half costs about as much as the first.
    // The legacy layout rewrote every shared term's whole list per add,
    // making the second half ~3x the first (quadratic cumulative bytes) —
    // measured at 596 MB vs 122 MB at 3,000 docs before its removal.
    expect(full / half).toBeLessThan(2.4);
  }, 90000);

  it('reads JS-written (legacy) index files and migrates on write', async () => {
    const name = base();
    // Frozen fixtures: 60 docs of docText(i) indexed by the removed pure-JS
    // implementation (single-blob posting layout).
    for (const suffix of ['terms', 'documents', 'lengths']) {
      const filename = `${name}-${suffix}.bj`;
      files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      await writeFixture(await fh.createSyncAccessHandle(), `ti-blocks-60-${suffix}.bin`);
    }

    // Same files, WASM implementation: legacy blobs are read directly.
    const idx = await makeIndex(TextIndex, BPlusTree, name);
    let hits = await idx.query('shared');
    expect(hits.length).toBe(60);
    expect(await idx.query('unique7x', { scored: false })).toEqual(['doc-7']);
    expect(await idx.getDocumentCount()).toBe(60);

    // Writing migrates the touched terms to blocks; everything stays visible.
    for (let i = 60; i < 90; i++) await idx.add(`doc-${i}`, docText(i));
    hits = await idx.query('shared');
    expect(hits.length).toBe(90);
    expect(await idx.query('unique7x', { scored: false })).toEqual(['doc-7']);

    // Removing a JS-era doc reaches it through the migrated blocks.
    expect(await idx.remove('doc-7')).toBe(true);
    expect(await idx.query('unique7x')).toHaveLength(0);
    expect((await idx.query('shared')).length).toBe(89);
    expect(await idx.getTermCount()).toBeGreaterThan(0);
    await idx.close();
  });

  it('replaces docs on re-add, even across block boundaries', async () => {
    // Re-add must equal remove-then-add (C_DATABASE_REVIEW.md §4.3): build
    // one index where doc-0 is added early (its entries end up buried in
    // block 0), then re-added with different content after 50 more docs —
    // and a reference index that only ever saw the final content. Their
    // query results must be identical.
    const name = base();
    const readded = await makeIndex(TextIndex, BPlusTree, `${name}-a`);
    const fresh = await makeIndex(TextIndex, BPlusTree, `${name}-b`);

    await readded.add('doc-0', 'shared once');
    for (let i = 1; i <= 50; i++) {
      await readded.add(`doc-${i}`, docText(i));
      await fresh.add(`doc-${i}`, docText(i));
    }
    await readded.add('doc-0', 'shared shared shared thrice');
    await fresh.add('doc-0', 'shared shared shared thrice');

    expect(await readded.getDocumentCount()).toBe(51);
    // Same ids and same scores. (Ordering among *equal* scores reflects
    // posting insertion order — history-dependent and not part of the
    // replace contract, so compare id-sorted.)
    const norm = (rs) => [...rs].sort((x, y) => x.id.localeCompare(y.id));
    for (const q of ['shared', 'once', 'thrice', 'shared thrice']) {
      const a = norm(await readded.query(q));
      const b = norm(await fresh.query(q));
      expect(a.map((r) => r.id)).toEqual(b.map((r) => r.id));
      for (let i = 0; i < a.length; i++) expect(a[i].score).toBeCloseTo(b[i].score, 10);
    }
    // The vanished term no longer matches doc-0 anywhere...
    expect(await readded.query('once')).toHaveLength(0);
    expect(await readded.query('shared once', { requireAll: true })).toEqual([]);
    // ...and doc-0 appears exactly once despite entries in two blocks.
    const hits = await readded.query('shared');
    expect(hits.filter((r) => r.id === 'doc-0').length).toBe(1);

    // Removing doc-0 clears both block entries.
    await readded.remove('doc-0');
    expect((await readded.query('shared')).some((r) => r.id === 'doc-0')).toBe(false);
    expect(await readded.query('thrice')).toHaveLength(0);
    await readded.close();
    await fresh.close();
  });

  it('clear resets the files instead of growing them (§4.6)', async () => {
    const name = base();
    const idx = await makeIndex(TextIndex, BPlusTree, name);
    for (let i = 0; i < 300; i++) await idx.add(`doc-${i}`, docText(i));
    await idx.close();
    const before = await fileSize(`${name}-terms.bj`);

    const again = await makeIndex(TextIndex, BPlusTree, name);
    await again.clear();
    expect(await again.getDocumentCount()).toBe(0);
    expect(await again.getTermCount()).toBe(0);
    expect(await again.query('shared')).toHaveLength(0);

    // Clearing must still work after: the index is fully usable.
    await again.add('doc-new', 'fresh zebra');
    expect(await again.query('zebra', { scored: false })).toEqual(['doc-new']);
    await again.close();

    // The old per-key clear *grew* the append-only file; reset shrinks it
    // to a stub (header + empty root + metadata) plus one tiny re-add.
    const after = await fileSize(`${name}-terms.bj`);
    expect(before).toBeGreaterThan(50000);
    expect(after).toBeLessThan(4000);
  });

  it('deletes a term chain when its last document is removed', async () => {
    const idx = await makeIndex(TextIndex, BPlusTree, base());
    await idx.add('doc-a', 'ephemeral zebra');
    await idx.add('doc-b', 'zebra');
    const terms = await idx.getTermCount();
    expect(await idx.remove('doc-a')).toBe(true);
    // "ephemeral" chain fully deleted; "zebra" survives.
    expect(await idx.getTermCount()).toBe(terms - 1);
    expect(await idx.query('ephemeral')).toHaveLength(0);
    expect((await idx.query('zebra', { scored: false }))).toEqual(['doc-b']);
    await idx.close();
  });
});
