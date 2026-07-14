/**
 * Shared BPlusTree compaction suite, parameterized by implementation.
 * Called by bplustree.compaction.test.js (JS) and
 * bplustree.compaction-wasm.test.js (WASM).
 */
import { expect, describe, it, beforeEach, afterEach, beforeAll } from 'vitest';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runBPlusTreeCompactionSuite(label, BPlusTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: BPlusTree Compaction`, function () {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-bplustree-compact-${label}-${Date.now()}-${testFileCounter++}.bj`;
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

    let tree;
    let compactedFilename;

    beforeEach(async function () {
      tree = await createTestTree(3);
      compactedFilename = getTestFilename();
      await tree.open();
    });

    afterEach(async function () {
      if (tree && tree.isOpen) {
        await tree.close();
      }
      if (tree && tree._testFilename) {
        await cleanupFile(tree._testFilename);
      }
      await cleanupFile(compactedFilename);
    });

    it('should compact the tree and reduce file size while preserving data', { timeout: 120000 }, async function () {
      for (let i = 0; i < 50; i++) {
        await tree.add(i, `value${i}`);
      }

      for (let i = 0; i < 20; i++) {
        await tree.delete(i);
      }

      for (let i = 50; i < 80; i++) {
        await tree.add(i, `value${i}`);
      }

      // Create sync handle for compacted file
      const compactedFileHandle = await getFileHandle(rootDirHandle, compactedFilename, { create: true });
      const compactedSyncHandle = await compactedFileHandle.createSyncAccessHandle();

      const result = await tree.compact(compactedSyncHandle);

      expect(result.oldSize).toBeGreaterThan(result.newSize);
      expect(result.bytesSaved).toBeGreaterThan(0);

      const originalEntries = await tree.toArray();

      // Reopen compacted tree using new API
      const fileHandle = await getFileHandle(rootDirHandle, compactedFilename, { create: false });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const compactedTree = new BPlusTree(syncHandle, 3);
      compactedTree._testFilename = compactedFilename;
      await compactedTree.open();
      const compactedEntries = await compactedTree.toArray();
      const spotCheck = await compactedTree.search(79);
      await compactedTree.close();

      expect(compactedEntries).toEqual(originalEntries);
      expect(spotCheck).toBe('value79');
    });
  });
}
