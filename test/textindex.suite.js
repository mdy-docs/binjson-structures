/**
 * Shared TextIndex behavioral suite, parameterized by implementation.
 *
 * test/textindex.test.js (pure-JS) and test/textindex-wasm.test.js (WASM) call
 * runTextIndexSuite with their { TextIndex, BPlusTree } so the identical
 * assertions run against each. `label` distinguishes runs and test filenames.
 */
import { describe, it, beforeEach, afterEach, expect, beforeAll } from 'vitest';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runTextIndexSuite(label, { TextIndex, BPlusTree }, hasOPFS, opts = {}) {
  const { replaceOnAdd = false } = opts;
  describe.skipIf(!hasOPFS)(`${label}: TextIndex`, function() {
    let index;
    let baseName;
    let counter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    async function cleanupFiles(name) {
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
      index = await createTestIndex();
      await index.open();
    });

    afterEach(async function() {
      if (index) {
        await index.close();
        index = null;
      }
      if (baseName) await cleanupFiles(baseName);
    });

    describe('Constructor', function() {
      it('should create an empty index', async function() {
        expect(await index.getTermCount()).toBe(0);
        expect(await index.getDocumentCount()).toBe(0);
      });
    });

    describe('add()', function() {
      it('should add a simple term to the index', async function() {
        await index.add('doc1', 'hello');
        expect(await index.getDocumentCount()).toBe(1);
        expect(await index.getTermCount()).toBe(1);
      });

      it('should add multiple terms to the index', async function() {
        await index.add('doc1', 'hello world');
        expect(await index.getDocumentCount()).toBe(1);
        expect(await index.getTermCount()).toBe(2);
      });

      it('should add multiple documents to the index', async function() {
        await index.add('doc1', 'hello world');
        await index.add('doc2', 'goodbye world');
        expect(await index.getDocumentCount()).toBe(2);
        expect(await index.getTermCount()).toBe(3);
      });

      it('should handle stemming correctly', async function() {
        await index.add('doc1', 'running runs run');
        expect(await index.getTermCount()).toBe(1);
      });

      it('should handle case-insensitive text', async function() {
        await index.add('doc1', 'Hello WORLD hello');
        expect(await index.getTermCount()).toBe(2);
      });

      it('should handle punctuation and special characters', async function() {
        await index.add('doc1', 'Hello, world! How are you?');
        expect(await index.getTermCount()).toBe(2);
      });

      it('should handle empty text', async function() {
        await index.add('doc1', '');
        expect(await index.getDocumentCount()).toBe(1);
        expect(await index.getTermCount()).toBe(0);
      });

      it('should handle non-string text gracefully', async function() {
        await index.add('doc1', null);
        expect(await index.getDocumentCount()).toBe(1);
        expect(await index.getTermCount()).toBe(0);
      });

      it('should throw error when document ID is missing', async function() {
        await expect(index.add(null, 'hello')).rejects.toThrow('Document ID is required');
        await expect(index.add('', 'hello')).rejects.toThrow('Document ID is required');
      });

      it('should handle adding same document multiple times', async function() {
        await index.add('doc1', 'hello world');
        await index.add('doc1', 'goodbye world');
        expect(await index.getDocumentCount()).toBe(1);
        // The C implementation replaces the document on re-add, so "hello"
        // disappears from the index; the JS reference merges terms and
        // keeps it (C_DATABASE_REVIEW.md §4.3).
        expect(await index.getTermCount()).toBe(replaceOnAdd ? 2 : 3);
        const hello = await index.query('hello', { scored: false });
        if (replaceOnAdd) expect(hello).toEqual([]);
        else expect(hello).toContain('doc1');
      });

      it('should handle complex text with various word forms', async function() {
        await index.add('doc1', 'The quick brown foxes are jumping over the lazy dogs');
        const results = await index.query('fox jump dog', { scored: false });
        expect(results).toContain('doc1');
      });
    });

    describe('remove()', function() {
      it('should remove a document from the index', async function() {
        await index.add('doc1', 'hello world');
        expect(await index.getDocumentCount()).toBe(1);

        const removed = await index.remove('doc1');
        expect(removed).toBe(true);
        expect(await index.getDocumentCount()).toBe(0);
        expect(await index.getTermCount()).toBe(0);
      });

      it('should remove only the specified document', async function() {
        await index.add('doc1', 'hello world');
        await index.add('doc2', 'hello universe');

        await index.remove('doc1');
        expect(await index.getDocumentCount()).toBe(1);
        expect(await index.getTermCount()).toBe(2);
      });

      it('should clean up terms that are only in removed document', async function() {
        await index.add('doc1', 'unique');
        await index.add('doc2', 'common');

        await index.remove('doc1');
        expect(await index.getTermCount()).toBe(1);
      });

      it('should keep shared terms when one document is removed', async function() {
        await index.add('doc1', 'hello world');
        await index.add('doc2', 'hello universe');

        await index.remove('doc1');
        const results = await index.query('hello', { scored: false });
        expect(results).toEqual(['doc2']);
      });

      it('should return false when removing non-existent document', async function() {
        const removed = await index.remove('nonexistent');
        expect(removed).toBe(false);
      });

      it('should handle removing from empty index', async function() {
        const removed = await index.remove('doc1');
        expect(removed).toBe(false);
        expect(await index.getDocumentCount()).toBe(0);
      });

      it('should allow re-adding a removed document', async function() {
        await index.add('doc1', 'hello');
        await index.remove('doc1');
        await index.add('doc1', 'world');

        expect(await index.getDocumentCount()).toBe(1);
        const results = await index.query('world', { scored: false });
        expect(results).toContain('doc1');
      });
    });

    describe('query()', function() {
      beforeEach(async function() {
        await index.add('doc1', 'The quick brown fox jumps over the lazy dog');
        await index.add('doc2', 'A fast brown fox');
        await index.add('doc3', 'The lazy cat sleeps');
        await index.add('doc4', 'Dogs and cats are friends');
      });

      it('should find documents with single term', async function() {
        const results = await index.query('fox', { scored: false });
        expect(results).toHaveLength(2);
        expect(results).toContain('doc1');
        expect(results).toContain('doc2');
      });

      it('should find documents with multiple terms (AND)', async function() {
        const results = await index.query('brown fox', { scored: false });
        expect(results).toHaveLength(2);
        expect(results).toContain('doc1');
        expect(results).toContain('doc2');
      });

      it('should apply stemming to query terms', async function() {
        const results = await index.query('jumping', { scored: false });
        expect(results).toContain('doc1');
      });

      it('should return empty array when no matches found', async function() {
        const results = await index.query('elephant', { scored: false });
        expect(results).toEqual([]);
      });

      it('should handle case-insensitive queries', async function() {
        const results = await index.query('FOX', { scored: false });
        expect(results).toHaveLength(2);
      });

      it('should handle empty query', async function() {
        const results = await index.query('', { scored: false });
        expect(results).toEqual([]);
      });

      it('should handle query with punctuation', async function() {
        const results = await index.query('fox!', { scored: false });
        expect(results).toHaveLength(2);
      });

      it('should return only documents matching ALL terms', async function() {
        const results = await index.query('lazy dog', { scored: false, requireAll: true });
        expect(results).toHaveLength(1);
        expect(results).toContain('doc1');
      });

      it('should handle queries with stemming variations', async function() {
        const results = await index.query('dogs', { scored: false });
        expect(results).toHaveLength(2);
        expect(results).toContain('doc1');
        expect(results).toContain('doc4');
      });

      it('should handle complex queries', async function() {
        const results = await index.query('the lazy', { scored: false, requireAll: true });
        expect(results).toHaveLength(2);
        expect(results).toContain('doc1');
        expect(results).toContain('doc3');
      });

      it('should return empty when one term does not match', async function() {
        const results = await index.query('fox elephant', { scored: false, requireAll: true });
        expect(results).toEqual([]);
      });
    });

    describe('getTermCount()', function() {
      it('should return 0 for empty index', async function() {
        expect(await index.getTermCount()).toBe(0);
      });

      it('should return correct count after adding terms', async function() {
        await index.add('doc1', 'hello world');
        expect(await index.getTermCount()).toBe(2);
      });

      it('should account for stemming', async function() {
        await index.add('doc1', 'running runs');
        expect(await index.getTermCount()).toBe(1);
      });

      it('should update count after removal', async function() {
        await index.add('doc1', 'unique term');
        await index.add('doc2', 'shared term');
        expect(await index.getTermCount()).toBe(3);

        await index.remove('doc1');
        expect(await index.getTermCount()).toBe(2);
      });
    });

    describe('getDocumentCount()', function() {
      it('should return 0 for empty index', async function() {
        expect(await index.getDocumentCount()).toBe(0);
      });

      it('should return correct count after adding documents', async function() {
        await index.add('doc1', 'hello');
        await index.add('doc2', 'world');
        expect(await index.getDocumentCount()).toBe(2);
      });

      it('should not double count same document', async function() {
        await index.add('doc1', 'hello');
        await index.add('doc1', 'world');
        expect(await index.getDocumentCount()).toBe(1);
      });

      it('should update count after removal', async function() {
        await index.add('doc1', 'hello');
        await index.add('doc2', 'world');
        await index.remove('doc1');
        expect(await index.getDocumentCount()).toBe(1);
      });
    });

    describe('clear()', function() {
      it('should clear all data from the index', async function() {
        await index.add('doc1', 'hello world');
        await index.add('doc2', 'goodbye world');

        await index.clear();

        expect(await index.getTermCount()).toBe(0);
        expect(await index.getDocumentCount()).toBe(0);
      });

      it('should allow adding data after clear', async function() {
        await index.add('doc1', 'hello');
        await index.clear();
        await index.add('doc2', 'world');

        expect(await index.getDocumentCount()).toBe(1);
        expect(await index.getTermCount()).toBe(1);
      });

      it('should handle clearing empty index', async function() {
        await index.clear();
        expect(await index.getTermCount()).toBe(0);
        expect(await index.getDocumentCount()).toBe(0);
      });
    });

    describe('Integration tests', function() {
      it('should handle a realistic document collection', async function() {
        await index.add('blog1', 'How to learn JavaScript programming');
        await index.add('blog2', 'JavaScript is a programming language');
        await index.add('blog3', 'Learning Python for beginners');
        await index.add('blog4', 'Advanced JavaScript techniques');

        let results = await index.query('JavaScript programming', { scored: false, requireAll: true });
        expect(results).toHaveLength(2);
        expect(results).toContain('blog1');
        expect(results).toContain('blog2');

        results = await index.query('learning', { scored: false });
        expect(results).toHaveLength(2);
        expect(results).toContain('blog1');
        expect(results).toContain('blog3');

        await index.remove('blog1');
        results = await index.query('learning', { scored: false });
        expect(results).toHaveLength(1);
        expect(results).toContain('blog3');
      });

      it('should handle documents with overlapping terms', async function() {
        await index.add('doc1', 'apple orange banana');
        await index.add('doc2', 'orange banana grape');
        await index.add('doc3', 'banana grape mango');

        const results = await index.query('banana', { scored: false });
        expect(results).toHaveLength(3);

        const results2 = await index.query('orange banana', { scored: false, requireAll: true });
        expect(results2).toHaveLength(2);
        expect(results2).toContain('doc1');
        expect(results2).toContain('doc2');
      });

      it('should handle adding, querying, removing, and re-adding', async function() {
        await index.add('doc1', 'test document');
        let results = await index.query('test', { scored: false });
        expect(results).toContain('doc1');

        await index.remove('doc1');
        results = await index.query('test', { scored: false });
        expect(results).toHaveLength(0);

        await index.add('doc1', 'new test content');
        results = await index.query('test', { scored: false });
        expect(results).toContain('doc1');

        results = await index.query('document', { scored: false });
        expect(results).toHaveLength(0);
      });
    });

    describe('Relevance Scoring', function() {
      beforeEach(async function() {
        await index.add('doc1', 'The quick brown fox jumps over the lazy dog');
        await index.add('doc2', 'A quick brown dog runs through the forest');
        await index.add('doc3', 'The lazy cat sleeps under the tree');
        await index.add('doc4', 'Foxes are quick and clever animals');
        await index.add('doc5', 'Dogs and cats are popular pets');
      });

      it('should return scored results by default', async function() {
        const results = await index.query('quick');
        expect(Array.isArray(results)).toBe(true);
        expect(results[0]).toHaveProperty('id');
        expect(results[0]).toHaveProperty('score');
        expect(typeof results[0].score).toBe('number');
      });

      it('should rank documents by relevance', async function() {
        const results = await index.query('quick');
        expect(results.length).toBe(3);
        for (let i = 0; i < results.length - 1; i++) {
          expect(results[i].score).toBeGreaterThanOrEqual(results[i + 1].score);
        }
      });

      it('should give higher scores to documents with multiple matching terms', async function() {
        const results = await index.query('lazy dog');
        expect(results[0].id).toBe('doc1');
        expect(results[0].score).toBeGreaterThan(results[1].score);
      });

      it('should include partial matches when not using requireAll', async function() {
        const results = await index.query('lazy dog', { scored: false });
        expect(results.length).toBeGreaterThan(1);
        expect(results).toContain('doc1');
      });

      it('should return only exact matches with requireAll option', async function() {
        const results = await index.query('lazy dog', { scored: false, requireAll: true });
        expect(results).toHaveLength(1);
        expect(results[0]).toBe('doc1');
      });

      it('should handle term frequency in scoring', async function() {
        await index.clear();
        await index.add('doc1', 'apple apple apple banana');
        await index.add('doc2', 'apple banana cherry');
        await index.add('doc3', 'banana cherry date');

        const results = await index.query('apple');
        expect(results.length).toBeGreaterThanOrEqual(2);
        expect(results[0]).toHaveProperty('score');
        expect(results[0].score).toBeGreaterThan(0);
      });

      it('should calculate TF-IDF correctly', async function() {
        await index.clear();
        await index.add('doc1', 'rare word');
        await index.add('doc2', 'common common common');
        await index.add('doc3', 'common word');
        await index.add('doc4', 'common stuff');

        const results = await index.query('rare', { scored: false });
        expect(results).toContain('doc1');

        const commonResults = await index.query('common', { scored: false });
        expect(commonResults.length).toBe(3);
      });
    });
  });
}
