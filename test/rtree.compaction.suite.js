/**
 * Shared R-tree compaction suite, parameterized by implementation.
 * See test/rtree.suite.js for the pattern.
 */
import { describe, it, expect, beforeEach, afterEach, beforeAll } from 'vitest';
import { deleteFile, getFileHandle, ObjectId } from '../third_party/binjson/js/binjson.js';

export function runRTreeCompactionSuite(label, RTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: R-tree Compaction`, function () {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-rtree-compaction-${label}-${Date.now()}-${testFileCounter++}.bj`;
    }

    async function createTestTree(order = 4) {
      const filename = getTestFilename();
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const tree = new RTree(syncHandle, order);
      tree._testFilename = filename;
      return tree;
    }

    async function reopenTree(filename, order = 4) {
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const tree = new RTree(syncHandle, order);
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
      tree = await createTestTree(4);
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

    it('should compact the tree and reduce file size while preserving data', { timeout: 240000 }, async function () {
      const insertedIds = [];
      for (let i = 0; i < 120; i++) {
        const lat = -80 + Math.random() * 160;
        const lng = -170 + Math.random() * 340;
        const id = new ObjectId();
        insertedIds.push(id);
        await tree.insert(lat, lng, id);
      }

      const worldBox = { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 };
      const originalIds = (await tree.searchBBox(worldBox)).map(r => r.objectId.toString()).sort();
      expect(originalIds.length).toBe(insertedIds.length);

      const compactedFileHandle = await getFileHandle(rootDirHandle, compactedFilename, { create: true });
      const compactedSyncHandle = await compactedFileHandle.createSyncAccessHandle();

      const result = await tree.compact(compactedSyncHandle);
      expect(result.oldSize).toBeGreaterThan(result.newSize);
      expect(result.bytesSaved).toBeGreaterThan(0);

      const compactedTree = await reopenTree(compactedFilename, 4);
      await compactedTree.open();
      const compactedIds = (await compactedTree.searchBBox(worldBox)).map(r => r.objectId.toString()).sort();
      expect(compactedIds).toEqual(originalIds);
      expect(compactedTree.size()).toBe(tree.size());
      await compactedTree.close();
    });
  });
}
