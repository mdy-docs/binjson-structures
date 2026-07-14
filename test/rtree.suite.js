/**
 * Shared R-tree behavioral suite, parameterized by implementation.
 *
 * Both test/rtree.test.js (pure-JS) and test/rtree-wasm.test.js (WASM) call
 * runRTreeSuite with their RTree class so the identical assertions run against
 * each. `label` distinguishes runs (and test filenames in OPFS).
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { ObjectId, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runRTreeSuite(label, RTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: On-Disk R-tree Implementation`, () => {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-rtree-${label}-${Date.now()}-${testFileCounter++}.bj`;
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

    it('should create and open new tree', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      expect(tree.size()).toBe(0);
      expect(tree.isOpen).toBe(true);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should insert points', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const id1 = new ObjectId();
      await tree.insert(40.7128, -74.0060, id1);
      expect(tree.size()).toBe(1);

      const id2 = new ObjectId();
      const id3 = new ObjectId();
      await tree.insert(34.0522, -118.2437, id2);
      await tree.insert(41.8781, -87.6298, id3);
      expect(tree.size()).toBe(3);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should search by bounding box', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const idNY = new ObjectId();
      const idLA = new ObjectId();
      const idCH = new ObjectId();

      await tree.insert(40.7128, -74.0060, idNY);
      await tree.insert(34.0522, -118.2437, idLA);
      await tree.insert(41.8781, -87.6298, idCH);

      const bbox = {
        minLat: 40,
        maxLat: 42,
        minLng: -75,
        maxLng: -73
      };

      const results = tree.searchBBox(bbox);
      expect(results).toHaveLength(1);
      expect(results[0].objectId).toEqual(idNY);
      expect(results[0].lat).toBeCloseTo(40.7128);
      expect(results[0].lng).toBeCloseTo(-74.0060);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should search by radius', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const idNY = new ObjectId();
      const idLA = new ObjectId();
      const idCH = new ObjectId();

      await tree.insert(40.7128, -74.0060, idNY);
      await tree.insert(34.0522, -118.2437, idLA);
      await tree.insert(41.8781, -87.6298, idCH);

      const results = await tree.searchRadius(40.7128, -74.0060, 100);
      expect(results).toHaveLength(1);
      expect(results[0].objectId).toEqual(idNY);
      expect(results[0].lat).toBeCloseTo(40.7128);
      expect(results[0].lng).toBeCloseTo(-74.0060);

      const largeResults = await tree.searchRadius(40.7128, -74.0060, 5000);
      expect(largeResults).toHaveLength(3);
      const largeIds = largeResults.map(r => r.objectId);
      expect(largeIds).toContainEqual(idNY);
      expect(largeIds).toContainEqual(idLA);
      expect(largeIds).toContainEqual(idCH);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should persist and reopen tree', async () => {
      // Create and insert
      let tree = await createTestTree(4);
      const filename = tree._testFilename;
      await tree.open();

      const idNY = new ObjectId();
      const idLA = new ObjectId();
      const idCH = new ObjectId();

      await tree.insert(40.7128, -74.0060, idNY);
      await tree.insert(34.0522, -118.2437, idLA);
      await tree.insert(41.8781, -87.6298, idCH);

      await tree.close();
      expect(tree.isOpen).toBe(false);

      // Reopen and verify
      tree = await reopenTree(filename, 4);
      await tree.open();
      expect(tree.size()).toBe(3);

      const bbox = {
        minLat: 40,
        maxLat: 42,
        minLng: -75,
        maxLng: -73
      };

      const results = await tree.searchBBox(bbox);
      expect(results).toHaveLength(1);
      expect(results[0].objectId).toEqual(idNY);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle node splitting with 8 entries', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      // Insert cities to force splits
      const cities = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 },
        { lat: 29.7604, lng: -95.3698 },
        { lat: 33.4484, lng: -112.0740 },
        { lat: 39.9526, lng: -75.1652 },
        { lat: 29.4241, lng: -98.4936 },
        { lat: 32.7157, lng: -117.1611 }
      ];

      for (const city of cities) {
        const id = new ObjectId();
        await tree.insert(city.lat, city.lng, id);
      }

      expect(tree.size()).toBe(8);

      // Verify all cities can be found
      const allResults = await tree.searchBBox({
        minLat: 25,
        maxLat: 45,
        minLng: -125,
        maxLng: -70
      });

      expect(allResults).toHaveLength(8);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should clear tree', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const id1 = new ObjectId();
      const id2 = new ObjectId();

      await tree.insert(40.7128, -74.0060, id1);
      await tree.insert(34.0522, -118.2437, id2);

      await tree.clear();
      expect(tree.size()).toBe(0);

      const results = await tree.searchBBox({
        minLat: 25,
        maxLat: 45,
        minLng: -125,
        maxLng: -70
      });

      expect(results).toHaveLength(0);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should remove a single entry', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const id1 = new ObjectId();
      const id2 = new ObjectId();
      const id3 = new ObjectId();

      await tree.insert(40.7128, -74.0060, id1);
      await tree.insert(34.0522, -118.2437, id2);
      await tree.insert(41.8781, -87.6298, id3);

      expect(tree.size()).toBe(3);

      // Remove one entry
      const removed = await tree.remove(id2);
      expect(removed).toBe(true);
      expect(tree.size()).toBe(2);

      // Verify id2 is gone but others remain
      const results = await tree.searchBBox({
        minLat: 25,
        maxLat: 45,
        minLng: -125,
        maxLng: -70
      });

      expect(results).toHaveLength(2);
      const idsAfterRemoval = results.map(r => r.objectId);
      expect(idsAfterRemoval).toContainEqual(id1);
      expect(idsAfterRemoval).toContainEqual(id3);
      expect(idsAfterRemoval).not.toContainEqual(id2);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should return false when removing non-existent entry', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const id1 = new ObjectId();
      const id2 = new ObjectId();

      await tree.insert(40.7128, -74.0060, id1);

      // Try to remove non-existent id
      const removed = await tree.remove(id2);
      expect(removed).toBe(false);
      expect(tree.size()).toBe(1);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should remove all entries one by one', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const ids = [];
      const cities = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 }
      ];

      for (const city of cities) {
        const id = new ObjectId();
        ids.push(id);
        await tree.insert(city.lat, city.lng, id);
      }

      expect(tree.size()).toBe(3);

      // Remove all entries
      for (const id of ids) {
        const removed = await tree.remove(id);
        expect(removed).toBe(true);
      }

      expect(tree.size()).toBe(0);

      const results = await tree.searchBBox({
        minLat: 25,
        maxLat: 45,
        minLng: -125,
        maxLng: -70
      });

      expect(results).toHaveLength(0);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle removal causing node underflow and merging', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      // Insert enough entries to force splits, creating internal nodes
      const cities = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 },
        { lat: 29.7604, lng: -95.3698 },
        { lat: 33.4484, lng: -112.0740 },
        { lat: 39.9526, lng: -75.1652 },
        { lat: 29.4241, lng: -98.4936 },
        { lat: 32.7157, lng: -117.1611 },
        { lat: 37.7749, lng: -122.4194 },
        { lat: 47.6062, lng: -122.3321 }
      ];

      const ids = [];
      for (const city of cities) {
        const id = new ObjectId();
        ids.push(id);
        await tree.insert(city.lat, city.lng, id);
      }

      expect(tree.size()).toBe(10);

      // Remove several entries to trigger underflow and merging
      for (let i = 0; i < 6; i++) {
        const removed = await tree.remove(ids[i]);
        expect(removed).toBe(true);
      }

      expect(tree.size()).toBe(4);

      // Verify remaining entries are still searchable
      const results = await tree.searchBBox({
        minLat: 25,
        maxLat: 50,
        minLng: -125,
        maxLng: -70
      });

      expect(results).toHaveLength(4);
      const remainingIds = results.map(r => r.objectId);
      for (let i = 6; i < 10; i++) {
        expect(remainingIds).toContainEqual(ids[i]);
      }

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should maintain tree integrity after mixed insertions and removals', async () => {
      const tree = await createTestTree(4);
      await tree.open();

      const cities = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 },
        { lat: 29.7604, lng: -95.3698 },
        { lat: 33.4484, lng: -112.0740 }
      ];

      const ids = [];

      // Insert first 3
      for (let i = 0; i < 3; i++) {
        const id = new ObjectId();
        ids.push(id);
        await tree.insert(cities[i].lat, cities[i].lng, id);
      }

      // Remove middle one
      await tree.remove(ids[1]);

      // Insert 2 more
      for (let i = 3; i < 5; i++) {
        const id = new ObjectId();
        ids.push(id);
        await tree.insert(cities[i].lat, cities[i].lng, id);
      }

      // Remove another
      await tree.remove(ids[2]);

      expect(tree.size()).toBe(3);

      // Verify correct entries remain
      const results = await tree.searchBBox({
        minLat: 25,
        maxLat: 50,
        minLng: -125,
        maxLng: -70
      });

      expect(results).toHaveLength(3);
      const mixedIds = results.map(r => r.objectId);
      expect(mixedIds).toContainEqual(ids[0]); // First entry
      expect(mixedIds).not.toContainEqual(ids[1]); // Removed
      expect(mixedIds).not.toContainEqual(ids[2]); // Removed
      expect(mixedIds).toContainEqual(ids[3]); // Fourth entry
      expect(mixedIds).toContainEqual(ids[4]); // Fifth entry

      await tree.close();
      await cleanupFile(tree._testFilename);
    });

    it('should handle removal in persisted tree', async () => {
      // Create and insert
      let tree = await createTestTree(4);
      const filename = tree._testFilename;
      await tree.open();

      const ids = [];
      const cities = [
        { lat: 40.7128, lng: -74.0060 },
        { lat: 34.0522, lng: -118.2437 },
        { lat: 41.8781, lng: -87.6298 },
        { lat: 29.7604, lng: -95.3698 }
      ];

      for (const city of cities) {
        const id = new ObjectId();
        ids.push(id);
        await tree.insert(city.lat, city.lng, id);
      }

      await tree.close();

      // Reopen and remove
      tree = await reopenTree(filename, 4);
      await tree.open();

      expect(tree.size()).toBe(4);

      const removed = await tree.remove(ids[1]);
      expect(removed).toBe(true);
      expect(tree.size()).toBe(3);

      await tree.close();

      // Reopen again and verify
      tree = await reopenTree(filename, 4);
      await tree.open();

      expect(tree.size()).toBe(3);

      const results = await tree.searchBBox({
        minLat: 25,
        maxLat: 50,
        minLng: -125,
        maxLng: -70
      });

      expect(results).toHaveLength(3);
      const persistedIds = results.map(r => r.objectId);
      expect(persistedIds).toContainEqual(ids[0]);
      expect(persistedIds).not.toContainEqual(ids[1]);
      expect(persistedIds).toContainEqual(ids[2]);
      expect(persistedIds).toContainEqual(ids[3]);

      await tree.close();
      await cleanupFile(tree._testFilename);
    });
  });
}
