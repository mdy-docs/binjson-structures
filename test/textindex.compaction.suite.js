/**
 * Shared TextIndex compaction suite, parameterized by implementation.
 */
import { describe, it, beforeEach, afterEach, expect, beforeAll } from 'vitest';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runTextIndexCompactionSuite(label, { TextIndex, BPlusTree }, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: TextIndex compaction`, function() {
    let index;
    let baseName;
    let compactBase;
    let counter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    async function cleanupFiles(name) {
      if (!name) return;
      const files = [`${name}-terms.bj`, `${name}-documents.bj`, `${name}-lengths.bj`];
      for (const file of files) {
        if (rootDirHandle) await deleteFile(rootDirHandle, file);
      }
    }

    async function makeTree(name) {
      const handle = await getFileHandle(rootDirHandle, name, { create: true });
      const syncHandle = await handle.createSyncAccessHandle();
      return new BPlusTree(syncHandle, 16);
    }

    async function reopenTree(name) {
      const handle = await getFileHandle(rootDirHandle, name, { create: false });
      const syncHandle = await handle.createSyncAccessHandle();
      return new BPlusTree(syncHandle, 16);
    }

    async function createTestIndex() {
      baseName = `text-index-${label}-${Date.now()}-${counter++}`;
      const indexTree = await makeTree(`${baseName}-terms.bj`);
      const docTermsTree = await makeTree(`${baseName}-documents.bj`);
      const lengthsTree = await makeTree(`${baseName}-lengths.bj`);
      return new TextIndex({
        order: 16,
        trees: { index: indexTree, documentTerms: docTermsTree, documentLengths: lengthsTree }
      });
    }

    beforeEach(async function() {
      compactBase = null;
      index = await createTestIndex();
      await index.open();
    });

    afterEach(async function() {
      if (index) {
        await index.close();
        index = null;
      }
      await cleanupFiles(baseName);
      await cleanupFiles(compactBase);
    });

    it('compacts underlying trees and keeps queries working', async function() {
      await index.add('doc1', 'The quick brown fox jumps');
      await index.add('doc2', 'Lazy dogs nap all day');

      const before = await index.query('quick fox', { scored: false });
      expect(before).toContain('doc1');

      compactBase = `${baseName}-compact`;

      const destIndexTree = await makeTree(`${compactBase}-terms.bj`);
      const destDocTermsTree = await makeTree(`${compactBase}-documents.bj`);
      const destLengthsTree = await makeTree(`${compactBase}-lengths.bj`);

      const result = await index.compact({
        index: destIndexTree,
        documentTerms: destDocTermsTree,
        documentLengths: destLengthsTree
      });

      expect(result.terms.oldSize).toBeGreaterThan(0);
      expect(result.documents.oldSize).toBeGreaterThan(0);
      expect(result.lengths.oldSize).toBeGreaterThan(0);

      // Compaction closes the index; reopen with the compacted data.
      const compactedIndex = await reopenTree(`${compactBase}-terms.bj`);
      const compactedDocTerms = await reopenTree(`${compactBase}-documents.bj`);
      const compactedLengths = await reopenTree(`${compactBase}-lengths.bj`);

      index = new TextIndex({
        order: 16,
        trees: { index: compactedIndex, documentTerms: compactedDocTerms, documentLengths: compactedLengths }
      });

      await index.open();

      const after = await index.query('quick fox', { scored: false });
      expect(after).toEqual(expect.arrayContaining(before));
      expect(await index.getDocumentCount()).toBe(2);

      await index.add('doc3', 'quick dogs and foxes together');
      const post = await index.query('dogs', { scored: false });
      expect(post).toContain('doc3');
    });
  });
}
