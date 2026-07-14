/**
 * Shared BPlusTree behavioral suite, parameterized by implementation.
 *
 * Both test/bplustree.test.js (pure-JS) and test/bplustree-wasm.test.js (WASM)
 * call runBPlusTreeSuite with their BPlusTree class so the identical assertions
 * run against each. `label` distinguishes runs (and test filenames in OPFS).
 */
import { expect, describe, it, beforeEach, afterEach, beforeAll } from 'vitest';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runBPlusTreeSuite(label, BPlusTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: BPlusTree`, function () {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-bplustree-${label}-${Date.now()}-${testFileCounter++}.bj`;
    }

    async function createTestTree(order = 3) {
      const filename = getTestFilename();
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
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

    describe('Constructor', function () {
      let tree;

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should create an empty tree with default order', async function () {
        tree = await createTestTree();
        await tree.open();
        expect(tree.isEmpty()).toBe(true);
        expect(tree.size()).toBe(0);
      });

      it('should create an empty tree with custom order', async function () {
        tree = await createTestTree(5);
        await tree.open();
        expect(tree.isEmpty()).toBe(true);
        expect(tree.order).toBe(5);
      });

      it('should throw error for invalid order', async function () {
        const filename = getTestFilename();
        const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
        const syncHandle = await fileHandle.createSyncAccessHandle();
        expect(() => new BPlusTree(syncHandle, 2)).toThrow('B+ tree order must be at least 3');
        expect(() => new BPlusTree(syncHandle, 1)).toThrow('B+ tree order must be at least 3');
        await syncHandle.close();
        await cleanupFile(filename);
      });
    });

    describe('Add and Search', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(3);
        await tree.open();
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should add a single key-value pair', async function () {
        await tree.add(10, 'ten');
        expect(tree.size()).toBe(1);
        expect(await tree.search(10)).toBe('ten');
      });

      it('should add multiple key-value pairs', async function () {
        await tree.add(10, 'ten');
        await tree.add(20, 'twenty');
        await tree.add(5, 'five');
        await tree.add(15, 'fifteen');

        expect(tree.size()).toBe(4);
        expect(await tree.search(10)).toBe('ten');
        expect(await tree.search(20)).toBe('twenty');
        expect(await tree.search(5)).toBe('five');
        expect(await tree.search(15)).toBe('fifteen');
      });

      it('should return undefined for non-existent keys', async function () {
        await tree.add(10, 'ten');
        expect(await tree.search(20)).toBeUndefined();
        expect(await tree.search(5)).toBeUndefined();
      });

      it('should handle adding keys in ascending order', async function () {
        for (let i = 1; i <= 10; i++) {
          await tree.add(i, `value${i}`);
        }

        expect(tree.size()).toBe(10);
        for (let i = 1; i <= 10; i++) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }
      });

      it('should handle adding keys in descending order', async function () {
        for (let i = 10; i >= 1; i--) {
          await tree.add(i, `value${i}`);
        }

        expect(tree.size()).toBe(10);
        for (let i = 1; i <= 10; i++) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }
      });

      it('should handle adding keys in random order', async function () {
        const keys = [5, 2, 8, 1, 9, 3, 7, 4, 6, 10];
        for (const key of keys) {
          await tree.add(key, `value${key}`);
        }

        expect(tree.size()).toBe(10);
        for (const key of keys) {
          expect(await tree.search(key)).toBe(`value${key}`);
        }
      });

      it('should handle adding duplicate keys (update value)', async function () {
        await tree.add(10, 'ten');
        await tree.add(10, 'TEN');

        const result = await tree.search(10);
        expect(result).toBe('TEN');
        expect(tree.size()).toBe(1); // Updating an existing key doesn't change the key count
      });

      it('should handle string keys', async function () {
        await tree.add('apple', 1);
        await tree.add('banana', 2);
        await tree.add('cherry', 3);

        expect(await tree.search('apple')).toBe(1);
        expect(await tree.search('banana')).toBe(2);
        expect(await tree.search('cherry')).toBe(3);
      });

      it('should handle large number of insertions', { timeout: 120000 }, async function () {
        const count = 100;
        for (let i = 0; i < count; i++) {
          await tree.add(i, `value${i}`);
        }

        expect(tree.size()).toBe(count);
        for (let i = 0; i < count; i++) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }
      });

      it('should persist data after close and reopen', async function () {
        await tree.add(10, 'ten');
        await tree.add(20, 'twenty');
        await tree.add(5, 'five');

        const filename = tree._testFilename;
        await tree.close();

        // Reopen from the same file
        const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();
        tree = new BPlusTree(syncHandle, 3);
        tree._testFilename = filename;
        await tree.open();

        expect(tree.size()).toBe(3);
        expect(await tree.search(10)).toBe('ten');
        expect(await tree.search(20)).toBe('twenty');
        expect(await tree.search(5)).toBe('five');
      });
    });

    describe('Delete', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(3);
        await tree.open();
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should delete a key from tree with single element', async function () {
        await tree.add(10, 'ten');
        await tree.delete(10);
        expect(tree.size()).toBe(0);
        expect(await tree.search(10)).toBeUndefined();
      });

      it('should delete a key from tree with multiple elements', async function () {
        await tree.add(10, 'ten');
        await tree.add(20, 'twenty');
        await tree.add(5, 'five');

        await tree.delete(10);
        expect(tree.size()).toBe(2);
        expect(await tree.search(10)).toBeUndefined();
        expect(await tree.search(20)).toBe('twenty');
        expect(await tree.search(5)).toBe('five');
      });

      it('should handle deleting non-existent key', async function () {
        await tree.add(10, 'ten');
        const sizeBefore = tree.size();
        await tree.delete(20);
        expect(tree.size()).toBe(sizeBefore);
      });

      it('should handle deleting all elements one by one', async function () {
        const keys = [5, 2, 8, 1, 9, 3, 7, 4, 6, 10];
        for (const key of keys) {
          await tree.add(key, `value${key}`);
        }

        for (const key of keys) {
          await tree.delete(key);
          expect(await tree.search(key)).toBeUndefined();
        }

        expect(tree.isEmpty()).toBe(true);
      });

      it('should handle deleting from large tree', async function () {
        const count = 50;
        for (let i = 0; i < count; i++) {
          await tree.add(i, `value${i}`);
        }

        // Delete every other element
        for (let i = 0; i < count; i += 2) {
          await tree.delete(i);
        }

        expect(tree.size()).toBe(count / 2);

        // Verify remaining elements
        for (let i = 1; i < count; i += 2) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }
      });
    });

    describe('toArray', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(3);
        await tree.open();
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should return empty array for empty tree', async function () {
        expect(await tree.toArray()).toEqual([]);
      });

      it('should return all elements in sorted order', async function () {
        const keys = [5, 2, 8, 1, 9, 3];
        for (const key of keys) {
          await tree.add(key, `value${key}`);
        }

        const result = await tree.toArray();
        expect(result.length).toBe(6);

        // Verify sorted order
        for (let i = 0; i < result.length - 1; i++) {
          expect(result[i].key).toBeLessThan(result[i + 1].key);
        }

        // Verify content
        expect(result).toEqual([
          { key: 1, value: 'value1' },
          { key: 2, value: 'value2' },
          { key: 3, value: 'value3' },
          { key: 5, value: 'value5' },
          { key: 8, value: 'value8' },
          { key: 9, value: 'value9' }
        ]);
      });
    });

    describe('Async Iterator', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(3);
        await tree.open();
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should iterate empty tree', async function () {
        const result = [];
        for await (const entry of tree) {
          result.push(entry);
        }
        expect(result).toEqual([]);
      });

      it('should iterate all elements in sorted order', async function () {
        const keys = [5, 2, 8, 1, 9, 3];
        for (const key of keys) {
          await tree.add(key, `value${key}`);
        }

        const result = [];
        for await (const entry of tree) {
          result.push(entry);
        }

        expect(result.length).toBe(6);

        // Verify sorted order
        for (let i = 0; i < result.length - 1; i++) {
          expect(result[i].key).toBeLessThan(result[i + 1].key);
        }

        // Verify content matches toArray
        expect(result).toEqual([
          { key: 1, value: 'value1' },
          { key: 2, value: 'value2' },
          { key: 3, value: 'value3' },
          { key: 5, value: 'value5' },
          { key: 8, value: 'value8' },
          { key: 9, value: 'value9' }
        ]);
      });

      it('should iterate large dataset efficiently', async function () {
        // Add more entries to force multiple leaf nodes
        const count = 100;
        for (let i = 0; i < count; i++) {
          await tree.add(i, `value${i}`);
        }

        let iterationCount = 0;
        let lastKey = -1;

        for await (const entry of tree) {
          expect(entry.key).toBeGreaterThan(lastKey); // Verify sorted
          expect(entry.value).toBe(`value${entry.key}`);
          lastKey = entry.key;
          iterationCount++;
        }

        expect(iterationCount).toBe(count);
      });

      it('should throw error if tree not open', async function () {
        await tree.close();

        await expect(async () => {
          for await (const entry of tree) {
            // Should not reach here
          }
        }).rejects.toThrow('Tree must be open before iteration');
      });

      it('should handle single element', async function () {
        await tree.add(42, 'answer');

        const result = [];
        for await (const entry of tree) {
          result.push(entry);
        }

        expect(result).toEqual([{ key: 42, value: 'answer' }]);
      });
    });

    describe('rangeSearch', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(3);
        await tree.open();
        for (let i = 1; i <= 10; i++) {
          await tree.add(i, `value${i}`);
        }
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should find all elements in range', async function () {
        const result = await tree.rangeSearch(3, 7);
        expect(result.length).toBe(5);
        expect(result.map(r => r.key)).toEqual([3, 4, 5, 6, 7]);
      });

      it('should find single element range', async function () {
        const result = await tree.rangeSearch(5, 5);
        expect(result.length).toBe(1);
        expect(result[0].key).toBe(5);
      });

      it('should return empty array for range with no elements', async function () {
        const result = await tree.rangeSearch(15, 20);
        expect(result.length).toBe(0);
      });

      it('should find all elements when range covers entire tree', async function () {
        const result = await tree.rangeSearch(1, 10);
        expect(result.length).toBe(10);
      });

      it('should handle range starting before first element', async function () {
        const result = await tree.rangeSearch(0, 5);
        expect(result.length).toBe(5);
        expect(result.map(r => r.key)).toEqual([1, 2, 3, 4, 5]);
      });

      it('should handle range ending after last element', async function () {
        const result = await tree.rangeSearch(8, 15);
        expect(result.length).toBe(3);
        expect(result.map(r => r.key)).toEqual([8, 9, 10]);
      });
    });

    describe('getHeight', function () {
      let tree;

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should return 0 for single-level tree', async function () {
        tree = await createTestTree(3);
        await tree.open();
        await tree.add(1, 'one');
        expect(await tree.getHeight()).toBe(0);
      });

      it('should return correct height for multi-level tree', async function () {
        tree = await createTestTree(3);
        await tree.open();
        // Add enough elements to create multiple levels
        for (let i = 1; i <= 20; i++) {
          await tree.add(i, `value${i}`);
        }
        expect(await tree.getHeight()).toBeGreaterThan(0);
      });
    });

    describe('Edge Cases', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(3);
        await tree.open();
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should handle negative numbers', async function () {
        await tree.add(-5, 'negative five');
        await tree.add(-10, 'negative ten');
        await tree.add(0, 'zero');
        await tree.add(5, 'positive five');

        expect(await tree.search(-5)).toBe('negative five');
        expect(await tree.search(-10)).toBe('negative ten');
        expect(await tree.search(0)).toBe('zero');
        expect(await tree.search(5)).toBe('positive five');
      });

      it('should handle floating point numbers', async function () {
        await tree.add(1.5, 'one point five');
        await tree.add(2.7, 'two point seven');
        await tree.add(3.2, 'three point two');

        expect(await tree.search(1.5)).toBe('one point five');
        expect(await tree.search(2.7)).toBe('two point seven');
        expect(await tree.search(3.2)).toBe('three point two');
      });

      it('should handle complex object values', async function () {
        const obj1 = { name: 'Alice', age: 30 };
        const obj2 = { name: 'Bob', age: 25 };

        await tree.add(1, obj1);
        await tree.add(2, obj2);

        expect(await tree.search(1)).toEqual(obj1);
        expect(await tree.search(2)).toEqual(obj2);
      });

      it('should maintain tree properties with higher order', async function () {
        await tree.close();
        if (tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }

        tree = await createTestTree(5);
        await tree.open();

        for (let i = 1; i <= 50; i++) {
          await tree.add(i, `value${i}`);
        }

        expect(tree.size()).toBe(50);
        for (let i = 1; i <= 50; i++) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }
      });
    });

    describe('Stress Tests', function () {
      let tree;

      beforeEach(async function () {
        tree = await createTestTree(4);
        await tree.open();
      });

      afterEach(async function () {
        if (tree && tree.isOpen) {
          await tree.close();
        }
        if (tree && tree._testFilename) {
          await cleanupFile(tree._testFilename);
        }
      });

      it('should handle rapid insertions and deletions', async function () {
        const operations = 100;

        // Insert
        for (let i = 0; i < operations; i++) {
          await tree.add(i, `value${i}`);
        }

        // Delete half
        for (let i = 0; i < operations / 2; i++) {
          await tree.delete(i);
        }

        // Verify remaining
        expect(tree.size()).toBe(operations / 2);
        for (let i = operations / 2; i < operations; i++) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }
      });

      it('should maintain correctness with mixed operations', async function () {
        await tree.add(5, 'five');
        await tree.add(3, 'three');
        await tree.add(7, 'seven');
        await tree.delete(3);
        await tree.add(1, 'one');
        await tree.add(9, 'nine');
        await tree.delete(5);
        await tree.add(2, 'two');

        const result = await tree.toArray();
        expect(result.map(r => r.key)).toEqual([1, 2, 7, 9]);
      });
    });

    describe('Persistence Tests', function () {
      let testTrees = [];

      afterEach(async function () {
        for (const tree of testTrees) {
          if (tree && tree.isOpen) {
            await tree.close();
          }
          if (tree && tree._testFilename) {
            await cleanupFile(tree._testFilename);
          }
        }
        testTrees = [];
      });

      it('should persist data across multiple open/close cycles', async function () {
        let tree = await createTestTree(3);
        testTrees.push(tree);
        await tree.open();
        await tree.add(1, 'one');
        await tree.add(2, 'two');
        const filename = tree._testFilename;
        await tree.close();

        // Reopen with same filename
        const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();
        tree = new BPlusTree(syncHandle, 3);
        tree._testFilename = filename;
        testTrees.push(tree);
        await tree.open();
        await tree.add(3, 'three');
        await tree.close();

        // Reopen again
        const fileHandle2 = await getFileHandle(rootDirHandle, filename, { create: false });
        const syncHandle2 = await fileHandle2.createSyncAccessHandle();
        tree = new BPlusTree(syncHandle2, 3);
        tree._testFilename = filename;
        testTrees.push(tree);
        await tree.open();

        expect(tree.size()).toBe(3);
        expect(await tree.search(1)).toBe('one');
        expect(await tree.search(2)).toBe('two');
        expect(await tree.search(3)).toBe('three');

        await tree.close();
      });

      it('should persist deletions', async function () {
        let tree = await createTestTree(3);
        testTrees.push(tree);
        await tree.open();
        await tree.add(1, 'one');
        await tree.add(2, 'two');
        await tree.add(3, 'three');
        await tree.delete(2);
        const filename = tree._testFilename;
        await tree.close();

        // Reopen
        const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();
        tree = new BPlusTree(syncHandle, 3);
        tree._testFilename = filename;
        testTrees.push(tree);
        await tree.open();

        expect(tree.size()).toBe(2);
        expect(await tree.search(1)).toBe('one');
        expect(await tree.search(2)).toBeUndefined();
        expect(await tree.search(3)).toBe('three');

        await tree.close();
      });

      it('should handle large dataset persistence', async function () {
        const count = 200;

        let tree = await createTestTree(4);
        testTrees.push(tree);
        await tree.open();

        for (let i = 0; i < count; i++) {
          await tree.add(i, `value${i}`);
        }

        const filename = tree._testFilename;
        await tree.close();

        // Reopen
        const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();
        tree = new BPlusTree(syncHandle, 4);
        tree._testFilename = filename;
        testTrees.push(tree);
        await tree.open();

        expect(tree.size()).toBe(count);

        // Verify random samples
        for (let i = 0; i < count; i += 10) {
          expect(await tree.search(i)).toBe(`value${i}`);
        }

        await tree.close();
      });
    });
  });
}
