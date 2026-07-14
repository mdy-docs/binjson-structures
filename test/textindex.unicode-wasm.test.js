/**
 * UTF-8 tokenizer tests (C_DATABASE_REVIEW.md §4.8).
 *
 * The tokenizer used to treat every byte >= 0x80 as a non-word character,
 * silently dropping accented Latin, Cyrillic, CJK — any non-ASCII text —
 * from the index (matching the JS reference's \w). UTF-8 bytes now count as
 * word characters, so whole UTF-8 words are indexed and searchable. Tokens
 * containing non-ASCII bytes bypass the English-only Porter stemmer and are
 * indexed verbatim; there is no Unicode segmentation, case folding, or
 * accent folding — documents and queries must use the same byte forms.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextIndex UTF-8 tokenization', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const base = () => `test-tixutf8-${Date.now()}-${counter++}`;

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  async function makeIndex(name) {
    async function tree(suffix) {
      const filename = `${name}-${suffix}.bj`;
      files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      return new BPlusTree(await fh.createSyncAccessHandle(), 16);
    }
    const idx = new TextIndex({
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

  it('indexes and finds accented Latin, Cyrillic and CJK words', async () => {
    const idx = await makeIndex(base());
    await idx.add('fr', 'un café à Paris près de la Seine');
    await idx.add('ru', 'привет мир из Москвы');
    await idx.add('ja', '東京 大阪 京都 を訪れる');
    await idx.add('en', 'a plain english document about coffee');

    expect(await idx.query('café', { scored: false })).toEqual(['fr']);
    expect(await idx.query('привет', { scored: false })).toEqual(['ru']);
    expect(await idx.query('мир', { scored: false })).toEqual(['ru']);
    expect(await idx.query('東京', { scored: false })).toEqual(['ja']);
    expect(await idx.query('coffee', { scored: false })).toEqual(['en']);

    // requireAll intersections work across scripts too.
    expect(await idx.query('привет Москвы', { requireAll: true })).toEqual(['ru']);
    await idx.close();
  });

  it('mixes scripts in one document, keeping ASCII stemming and stop words', async () => {
    const idx = await makeIndex(base());
    await idx.add('doc', 'the visitors are visiting the café and running home');

    // ASCII words still stem ("visiting"/"visitors" -> "visit", query too)...
    expect(await idx.query('visit', { scored: false })).toEqual(['doc']);
    expect(await idx.query('runs', { scored: false })).toEqual(['doc']);
    // ...stop words are still dropped...
    expect(await idx.query('the and', { scored: false })).toEqual([]);
    // ...and the non-ASCII token is searchable alongside them.
    expect(await idx.query('café', { scored: false })).toEqual(['doc']);
    await idx.close();
  });

  it('non-ASCII terms are byte-exact: no folding, no stemming', async () => {
    const idx = await makeIndex(base());
    await idx.add('doc', 'crème brûlée');

    // Unaccented spellings are different byte sequences — no accent folding.
    expect(await idx.query('creme', { scored: false })).toEqual([]);
    // Non-ASCII tokens bypass the Porter stemmer: plural stays distinct.
    await idx.add('doc2', 'brûlées');
    expect(await idx.query('brûlée', { scored: false })).toEqual(['doc']);
    expect(await idx.query('brûlées', { scored: false })).toEqual(['doc2']);
    // Uppercase ASCII inside a mixed token still lowercases.
    expect(await idx.query('CRÈME'.toLowerCase(), { scored: false })).toEqual(['doc']);
    await idx.close();
  });

  it('removal and re-add handle non-ASCII terms', async () => {
    const idx = await makeIndex(base());
    await idx.add('doc', 'zürich geneva');
    expect(await idx.query('zürich', { scored: false })).toEqual(['doc']);

    await idx.add('doc', 'bern geneva');   // re-add: zürich vanishes (§4.3)
    expect(await idx.query('zürich')).toHaveLength(0);
    expect(await idx.query('bern', { scored: false })).toEqual(['doc']);

    expect(await idx.remove('doc')).toBe(true);
    expect(await idx.query('bern')).toHaveLength(0);
    expect(await idx.getTermCount()).toBe(0);
    await idx.close();
  });
});
