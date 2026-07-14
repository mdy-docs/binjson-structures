/**
 * Query-path scalability tests (C_DATABASE_REVIEW.md §4.4, §4.5).
 *
 * A scored query used to (a) load the lengths of every document in the
 * corpus (a full documentLengths tree scan per query) and (b) re-read every
 * matched document's full term map for the coverage boost. Both passes are
 * gone: lengths are fetched lazily for scored docs only, the doc count
 * comes from the tree's O(1) size, and coverage is counted during posting
 * accumulation. Query cost is now proportional to matched postings, not
 * corpus size — pinned here with read counts on the underlying handles.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextIndex query scalability', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const base = () => `test-tixquery-${Date.now()}-${counter++}`;

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

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

  it('a selective query reads O(hits) records, not O(corpus)', async () => {
    const name = base();
    async function handleFor(suffix) {
      const filename = `${name}-${suffix}.bj`;
      files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      return fh.createSyncAccessHandle();
    }

    const proxies = {
      terms: counting(await handleFor('terms')),
      documents: counting(await handleFor('documents')),
      lengths: counting(await handleFor('lengths'))
    };
    const idx = new TextIndex({
      order: 16,
      trees: {
        index: new BPlusTree(proxies.terms, 16),
        documentTerms: new BPlusTree(proxies.documents, 16),
        documentLengths: new BPlusTree(proxies.lengths, 16)
      }
    });
    await idx.open();

    const N = 1500;
    for (let i = 0; i < N; i++) {
      await idx.add(`doc-${i}`, `shared corpus unique${i}x group${i % 25}`);
    }

    // A query matching exactly one document.
    const before = {
      documents: proxies.documents.stats.reads,
      lengths: proxies.lengths.stats.reads
    };
    const hits = await idx.query('unique42x');
    expect(hits.length).toBe(1);
    expect(hits[0].id).toBe('doc-42');

    const lengthsReads = proxies.lengths.stats.reads - before.lengths;
    const documentsReads = proxies.documents.stats.reads - before.documents;

    // §4.4: one root-to-leaf search for the single scored doc's length —
    // the old full-tree scan read hundreds of records for N=1500.
    expect(lengthsReads).toBeLessThan(10);
    // §4.5: the coverage pass no longer touches documentTerms at all.
    expect(documentsReads).toBe(0);

    // A broader query still touches only its own candidates' lengths.
    const b2 = proxies.lengths.stats.reads;
    const groupHits = await idx.query('group7');   // 60 docs
    expect(groupHits.length).toBe(60);
    expect(proxies.lengths.stats.reads - b2).toBeLessThan(60 * 6);

    // Scoring is unchanged: idf uses the true doc count.
    expect(await idx.getDocumentCount()).toBe(N);
    await idx.close();
  });
});
