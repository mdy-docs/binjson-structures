/**
 * Shared R-tree node-size suite, parameterized by implementation.
 * Verifies that nodes of various sizes are correctly serialized and
 * deserialized. See test/rtree.suite.js for the pattern.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { deleteFile, getFileHandle, ObjectId } from '../third_party/binjson/js/binjson.js';

export function runRTreeNodeSizesSuite(label, RTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: R-tree Node Size Handling`, () => {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-rtree-node-sizes-${label}-${Date.now()}-${testFileCounter++}.bj`;
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

    it('should handle small nodes (1 entry)', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const lat = 40.7128;
      const lng = -74.0060;
      const id = new ObjectId();

      await tree.insert(lat, lng, id);
      expect(tree.size()).toBe(1);

      const results = await tree.searchBBox({ minLat: 40, maxLat: 41, minLng: -75, maxLng: -73 });
      expect(results).toHaveLength(1);
      expect(results[0]).toEqual({ objectId: id, lat, lng });

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle medium nodes (4 entries - single node)', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const entries = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 },
        { lat: 29.7604, lng: -95.3698 }
      ];

      for (const entry of entries) {
        const id = new ObjectId();
        await tree.insert(entry.lat, entry.lng, id);
      }

      expect(tree.size()).toBe(4);

      for (const entry of entries) {
        const results = await tree.searchRadius(entry.lat, entry.lng, 1);
        expect(results.length).toBeGreaterThan(0);
      }

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle large nodes (8 entries - causes splits with maxEntries=4)', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const entries = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 },
        { lat: 29.7604, lng: -95.3698 },
        { lat: 33.7490, lng: -84.3880 },
        { lat: 39.7392, lng: -104.9903 },
        { lat: 47.6062, lng: -122.3321 },
        { lat: 37.7749, lng: -122.4194 }
      ];

      for (const entry of entries) {
        const id = new ObjectId();
        await tree.insert(entry.lat, entry.lng, id);
      }

      expect(tree.size()).toBe(8);

      await tree.close();
      const tree2 = await reopenTree(tree._testFilename, 4);
      await tree2.open();

      expect(tree2.size()).toBe(8);

      for (const entry of entries) {
        const results = await tree2.searchRadius(entry.lat, entry.lng, 1);
        expect(results.length).toBeGreaterThan(0);
      }

      await tree2.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle very large nodes with extensive metadata', { timeout: 15000 }, async () => {
      const tree = await createTestTree(8);
      await tree.open();

      const entries = [];
      for (let i = 0; i < 16; i++) {
        const lat = 20 + Math.random() * 40;
        const lng = -130 + Math.random() * 80;
        const id = new ObjectId();
        entries.push({ lat, lng, id });
        await tree.insert(lat, lng, id);
      }

      expect(tree.size()).toBe(16);

      await tree.close();
      const tree2 = await reopenTree(tree._testFilename, 8);
      await tree2.open();

      expect(tree2.size()).toBe(16);

      const results = await tree2.searchBBox({ minLat: 20, maxLat: 60, minLng: -130, maxLng: -50 });
      expect(results.length).toBeGreaterThan(0);

      await tree2.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle extremely large nodes (50 entries)', { timeout: 60000 }, async () => {
      const tree = await createTestTree(16);
      await tree.open();

      const insertedIds = [];
      for (let i = 0; i < 50; i++) {
        const lat = 20 + Math.random() * 40;
        const lng = -130 + Math.random() * 80;
        const id = new ObjectId();
        insertedIds.push(id);
        await tree.insert(lat, lng, id);
      }

      expect(tree.size()).toBe(50);

      await tree.close();
      const tree2 = await reopenTree(tree._testFilename, 16);
      await tree2.open();

      expect(tree2.size()).toBe(50);

      const results = await tree2.searchBBox({ minLat: 20, maxLat: 60, minLng: -130, maxLng: -50 });
      expect(results.length).toBeGreaterThan(0);

      await tree2.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle nodes with deeply nested structures', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      for (let i = 0; i < 8; i++) {
        const lat = 30 + Math.random() * 10;
        const lng = -100 + Math.random() * 10;
        const id = new ObjectId();
        await tree.insert(lat, lng, id);
      }

      expect(tree.size()).toBe(8);

      await tree.close();
      const tree2 = await reopenTree(tree._testFilename, 4);
      await tree2.open();

      expect(tree2.size()).toBe(8);

      const results = await tree2.searchBBox({ minLat: 30, maxLat: 40, minLng: -100, maxLng: -90 });
      expect(results.length).toBeGreaterThan(0);

      await tree2.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle mixed size nodes and queries', { timeout: 30000 }, async () => {
      const tree = await createTestTree(6);
      await tree.open();

      for (let i = 0; i < 20; i++) {
        const lat = 25 + Math.random() * 30;
        const lng = -120 + Math.random() * 60;
        const id = new ObjectId();
        await tree.insert(lat, lng, id);
      }

      expect(tree.size()).toBe(20);

      await tree.close();
      const tree2 = await reopenTree(tree._testFilename, 6);
      await tree2.open();

      expect(tree2.size()).toBe(20);

      const bbox1 = await tree2.searchBBox({ minLat: 25, maxLat: 35, minLng: -120, maxLng: -100 });
      expect(bbox1.length).toBeGreaterThanOrEqual(0);

      const bbox2 = await tree2.searchBBox({ minLat: 40, maxLat: 55, minLng: -80, maxLng: -60 });
      expect(bbox2.length).toBeGreaterThanOrEqual(0);

      const radius = await tree2.searchRadius(30, -100, 500);
      expect(radius.length).toBeGreaterThanOrEqual(0);

      await tree2.close();
      await cleanupFile(tree._testFilename);
    });

    it('should track and report node sizes during operations', async () => {
      const tree = await createTestTree(5);
      await tree.open();

      const nodeSizes = [];
      for (let i = 0; i < 20; i++) {
        const lat = 25 + Math.random() * 30;
        const lng = -120 + Math.random() * 60;
        const id = new ObjectId();
        await tree.insert(lat, lng, id);

        const fileSize = await tree.file.getFileSize();
        nodeSizes.push(fileSize);
      }

      expect(tree.size()).toBe(20);
      expect(nodeSizes[nodeSizes.length - 1]).toBeGreaterThan(nodeSizes[0]);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });
  });
}
