/**
 * BM25 scoring tests (C_DATABASE_REVIEW.md §4.9).
 *
 * The C engine scores with BM25 (k1 = 1.2, b = 0.75, Lucene-style
 * always-positive idf) instead of the JS reference's plain TF-IDF — a
 * deliberate divergence. The coverage boost on top is unchanged. BM25's
 * average document length comes from a corpus length sum maintained
 * incrementally under a reserved index-tree key by add/remove; legacy
 * indexes without the key fall back to one documentLengths scan per query
 * (never persisted by a query) until their next add writes it.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextIndex BM25 scoring', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const base = () => `test-tixbm25-${Date.now()}-${counter++}`;

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  async function makeIndex(name, Index = TextIndex, Tree = BPlusTree) {
    async function tree(suffix) {
      const filename = `${name}-${suffix}.bj`;
      if (!files.includes(filename)) files.push(filename);
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

  const score = (hits, id) => hits.find((h) => h.id === id)?.score;

  it('ranks rare terms above ubiquitous ones, with positive scores throughout', async () => {
    const idx = await makeIndex(base());
    // "common" is in every doc; "sapphire" in one.
    for (let i = 0; i < 30; i++) await idx.add(`doc-${i}`, `common filler text number${i}`);
    await idx.add('gem', 'common sapphire');

    // The ubiquitous term used to score log(N/N) = 0 for everyone; BM25's
    // idf is always positive.
    const common = await idx.query('common');
    expect(common.length).toBe(31);
    for (const h of common) expect(h.score).toBeGreaterThan(0);

    // A doc matching the rare term dominates docs matching only the
    // ubiquitous one.
    const mixed = await idx.query('common sapphire');
    expect(mixed[0].id).toBe('gem');
    expect(score(mixed, 'gem')).toBeGreaterThan(3 * score(mixed, 'doc-0'));
    await idx.close();
  });

  it('normalizes by document length and saturates term frequency', async () => {
    const idx = await makeIndex(base());
    const pad = (n) => Array.from({ length: n }, (_, i) => `pad${i}`).join(' ');
    await idx.add('short', `zebra ${pad(3)}`);
    await idx.add('long', `zebra ${pad(60)}`);
    // Same tf, different lengths: the shorter doc ranks higher.
    let hits = await idx.query('zebra');
    expect(hits[0].id).toBe('short');
    expect(score(hits, 'short')).toBeGreaterThan(score(hits, 'long'));

    // tf saturation: 10x the occurrences must score higher but far less
    // than 10x (k1 caps the tf contribution).
    await idx.add('tf2', `yak yak ${pad(20)}`);
    await idx.add('tf20', `${'yak '.repeat(20)}${pad(2)}`);
    hits = await idx.query('yak');
    const s2 = score(hits, 'tf2'), s20 = score(hits, 'tf20');
    expect(s20).toBeGreaterThan(s2);
    expect(s20).toBeLessThan(3 * s2);
    await idx.close();
  });

  it('keeps the corpus length sum exact through re-adds and removes', async () => {
    // An index churned by re-adds (changing doc lengths) and removes must
    // score identically to a fresh index holding the same final content —
    // which is only true if the maintained length sum stays exact.
    const churned = await makeIndex(base());
    const fresh = await makeIndex(base());

    for (let i = 0; i < 20; i++) await churned.add(`doc-${i}`, `alpha beta word${i} extra padding here`);
    for (let i = 0; i < 20; i += 2) await churned.add(`doc-${i}`, `alpha gamma word${i}`); // shorter re-add
    for (let i = 1; i < 20; i += 4) await churned.remove(`doc-${i}`);

    for (let i = 0; i < 20; i++) {
      if (i % 4 === 1) continue;
      if (i % 2 === 0) await fresh.add(`doc-${i}`, `alpha gamma word${i}`);
      else await fresh.add(`doc-${i}`, `alpha beta word${i} extra padding here`);
    }

    for (const q of ['alpha', 'gamma', 'beta', 'alpha gamma']) {
      const a = (await churned.query(q)).sort((x, y) => x.id.localeCompare(y.id));
      const b = (await fresh.query(q)).sort((x, y) => x.id.localeCompare(y.id));
      expect(a.map((r) => r.id)).toEqual(b.map((r) => r.id));
      for (let i = 0; i < a.length; i++) expect(a[i].score).toBeCloseTo(b[i].score, 10);
    }
    await churned.close();
    await fresh.close();
  });

  it('scores legacy (JS-written) indexes via the scan fallback, identically', async () => {
    const name = base();
    // Frozen fixtures: 25 docs of `orchard apple pear${i % 5} fruit${i}`
    // indexed by the removed pure-JS implementation (no stats key).
    for (const suffix of ['terms', 'documents', 'lengths']) {
      const filename = `${name}-legacy-${suffix}.bj`;
      if (!files.includes(filename)) files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      await writeFixture(await fh.createSyncAccessHandle(), `ti-bm25-25-${suffix}.bin`);
    }
    const native = await makeIndex(`${name}-native`);
    for (let i = 0; i < 25; i++) {
      await native.add(`doc-${i}`, `orchard apple pear${i % 5} fruit${i}`);
    }

    // Reopen the JS-written files with the C engine: no stats key exists,
    // so avgdl comes from the per-query fallback scan — and must produce
    // exactly the scores the natively-built index produces.
    const legacy = await makeIndex(`${name}-legacy`);
    for (const q of ['orchard', 'apple pear3', 'fruit7 orchard']) {
      const a = (await legacy.query(q)).sort((x, y) => x.id.localeCompare(y.id));
      const b = (await native.query(q)).sort((x, y) => x.id.localeCompare(y.id));
      expect(a.map((r) => r.id)).toEqual(b.map((r) => r.id));
      for (let i = 0; i < a.length; i++) expect(a[i].score).toBeCloseTo(b[i].score, 10);
    }

    // The first write persists the sum; scores stay consistent after.
    await legacy.add('doc-new', 'orchard plum');
    await native.add('doc-new', 'orchard plum');
    const a = (await legacy.query('orchard')).sort((x, y) => x.id.localeCompare(y.id));
    const b = (await native.query('orchard')).sort((x, y) => x.id.localeCompare(y.id));
    for (let i = 0; i < a.length; i++) expect(a[i].score).toBeCloseTo(b[i].score, 10);
    await legacy.close();
    await native.close();
  });
});
