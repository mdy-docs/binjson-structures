/**
 * Shared BPlusTree persistence suite, parameterized by implementation.
 * Tests that data survives close/reopen cycles. Called by
 * bplustree.persistence.test.js (JS) and bplustree.persistence-wasm.test.js.
 */
import { expect, describe, it, afterEach, beforeAll } from 'vitest';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runBPlusTreePersistenceSuite(label, BPlusTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: BPlusTree Persistence`, () => {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-bplustree-persistence-${label}-${Date.now()}-${testFileCounter++}.bj`;
    }

    async function createTestTree(order = 3) {
      const filename = getTestFilename();
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const tree = new BPlusTree(syncHandle, order);
      tree._testFilename = filename;
      return tree;
    }

    async function reopenTree(filename, order = 3) {
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const tree = new BPlusTree(syncHandle, order);
      tree._testFilename = filename;
      return tree;
    }

    async function cleanupFile(filename) {
      if (rootDirHandle) {
        await deleteFile(rootDirHandle, filename);
      }
    }

    afterEach(async () => {
      // Cleanup happens within each test
    });

    it('should persist and reload a single key-value pair', async () => {
      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      await tree.add(10, 'ten');
      expect(tree.size()).toBe(1);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(1);
      expect(await tree.search(10)).toBe('ten');

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist and reload multiple key-value pairs', async () => {
      const testData = [
        [10, 'ten'],
        [20, 'twenty'],
        [5, 'five'],
        [15, 'fifteen'],
        [30, 'thirty'],
        [3, 'three']
      ];

      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      for (const [key, value] of testData) {
        await tree.add(key, value);
      }
      expect(tree.size()).toBe(testData.length);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(testData.length);
      for (const [key, value] of testData) {
        expect(await tree.search(key)).toBe(value);
      }

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist and reload large dataset', async () => {
      const count = 100;

      let tree = await createTestTree(5);
      const filename = tree._testFilename;
      await tree.open();

      for (let i = 0; i < count; i++) {
        await tree.add(i, `value${i}`);
      }
      expect(tree.size()).toBe(count);

      await tree.close();

      tree = await reopenTree(filename, 5);
      await tree.open();

      expect(tree.size()).toBe(count);
      for (let i = 0; i < count; i++) {
        expect(await tree.search(i)).toBe(`value${i}`);
      }

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist and reload with multiple close/reopen cycles', async () => {
      const testData = [
        [5, 'five'],
        [10, 'ten'],
        [15, 'fifteen']
      ];

      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      for (const [key, value] of testData) {
        await tree.add(key, value);
      }
      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(3);
      for (const [key, value] of testData) {
        expect(await tree.search(key)).toBe(value);
      }

      await tree.add(20, 'twenty');
      await tree.add(25, 'twenty-five');
      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(5);
      for (const [key, value] of testData) {
        expect(await tree.search(key)).toBe(value);
      }
      expect(await tree.search(20)).toBe('twenty');
      expect(await tree.search(25)).toBe('twenty-five');

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist string keys across close/reopen', async () => {
      const stringData = [
        ['apple', 1],
        ['banana', 2],
        ['cherry', 3],
        ['date', 4],
        ['elderberry', 5]
      ];

      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      for (const [key, value] of stringData) {
        await tree.add(key, value);
      }
      expect(tree.size()).toBe(stringData.length);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(stringData.length);
      for (const [key, value] of stringData) {
        expect(await tree.search(key)).toBe(value);
      }

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist complex values across close/reopen', async () => {
      const complexData = [
        [1, { name: 'Alice', age: 30, active: true }],
        [2, { name: 'Bob', age: 25, active: false }],
        [3, [1, 2, 3, 4, 5]],
        [4, { nested: { deep: { value: 'test' } } }]
      ];

      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      for (const [key, value] of complexData) {
        await tree.add(key, value);
      }
      expect(tree.size()).toBe(complexData.length);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(complexData.length);
      for (const [key, value] of complexData) {
        const retrieved = await tree.search(key);
        expect(retrieved).toEqual(value);
      }

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist after deletions', async () => {
      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      const initialData = [[5, 'five'], [10, 'ten'], [15, 'fifteen'], [20, 'twenty']];
      for (const [key, value] of initialData) {
        await tree.add(key, value);
      }

      await tree.delete(10);
      expect(tree.size()).toBe(3);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.size()).toBe(3);
      expect(await tree.search(10)).toBeUndefined();
      expect(await tree.search(5)).toBe('five');
      expect(await tree.search(15)).toBe('fifteen');
      expect(await tree.search(20)).toBe('twenty');

      await tree.close();
      await cleanupFile(filename);
    });

    it('should persist empty tree after clearing all data', async () => {
      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      const data = [[5, 'five'], [10, 'ten'], [15, 'fifteen']];
      for (const [key, value] of data) {
        await tree.add(key, value);
      }

      for (const [key] of data) {
        await tree.delete(key);
      }
      expect(tree.isEmpty()).toBe(true);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      expect(tree.isEmpty()).toBe(true);
      expect(tree.size()).toBe(0);

      await tree.close();
      await cleanupFile(filename);
    });

    it('should preserve tree order across close/reopen', async () => {
      const order = 5;
      const count = 50;

      let tree = await createTestTree(order);
      const filename = tree._testFilename;
      await tree.open();

      for (let i = 0; i < count; i++) {
        await tree.add(i, `value${i}`);
      }

      await tree.close();

      tree = await reopenTree(filename, order);
      await tree.open();

      expect(tree.order).toBe(order);
      expect(tree.size()).toBe(count);

      await tree.close();
      await cleanupFile(filename);
    });

    it('should handle toArray() after reload', async () => {
      const testData = [[3, 'c'], [1, 'a'], [2, 'b']];

      let tree = await createTestTree(3);
      const filename = tree._testFilename;
      await tree.open();

      for (const [key, value] of testData) {
        await tree.add(key, value);
      }

      let array = await tree.toArray();
      expect(array).toHaveLength(3);

      await tree.close();

      tree = await reopenTree(filename, 3);
      await tree.open();

      array = await tree.toArray();
      expect(array).toHaveLength(3);
      expect(array[0].key).toBe(1);
      expect(array[1].key).toBe(2);
      expect(array[2].key).toBe(3);

      await tree.close();
      await cleanupFile(filename);
    });
  });
}
