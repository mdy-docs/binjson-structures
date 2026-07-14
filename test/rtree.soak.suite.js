/**
 * Shared R-tree soak/load suite, parameterized by implementation.
 * Measures timing/performance under load. See test/rtree.suite.js.
 */
import { describe, it, expect, afterEach, beforeAll } from 'vitest';
import { deleteFile, getFileHandle, ObjectId } from '../third_party/binjson/js/binjson.js';

// Detect if running in browser
const isBrowser = typeof navigator !== 'undefined' && typeof process === 'undefined';

function generateRandomLocation() {
  const minLat = 25, maxLat = 49;
  const minLng = -125, maxLng = -66;
  return {
    lat: minLat + Math.random() * (maxLat - minLat),
    lng: minLng + Math.random() * (maxLng - minLng),
    name: `Location-${Math.random().toString(36).substr(2, 9)}`
  };
}

function generateLocationWithId(index) {
  const location = generateRandomLocation();
  return { ...location, objectId: new ObjectId(), index };
}

function printTimingReport(label, timings, count) {
  if (timings.length === 0) return;
  const total = timings.reduce((a, b) => a + b, 0);
  const avg = total / count;
  const min = Math.min(...timings);
  const max = Math.max(...timings);

  console.log(`\n  ${label}:`);
  console.log(`    Total: ${total.toFixed(2)}ms`);
  console.log(`    Avg: ${avg.toFixed(4)}ms`);
  console.log(`    Min: ${min.toFixed(4)}ms`);
  console.log(`    Max: ${max.toFixed(4)}ms`);
  console.log(`    Throughput: ${(count / (total / 1000)).toFixed(0)} ops/sec`);
}

export function runRTreeSoakSuite(label, RTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: R-tree Soak Tests`, { timeout: 30000 }, () => {
    let rootDirHandle = null;
    const filename = `test-rtree-soak-${label}.bj`;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    async function cleanup() {
      if (rootDirHandle) {
        await deleteFile(rootDirHandle, filename);
      }
    }

    async function createSoakTestTree() {
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      return new RTree(syncHandle, 4);
    }

    afterEach(async () => {
      await cleanup();
    });

    it('should benchmark small insertions with timing', async () => {
      const tree = await createSoakTestTree();
      await tree.open();

      const count = 8;
      const insertTimings = [];

      console.log(`\n  Benchmarking ${count} insertions...`);
      const insertStartTime = performance.now();

      for (let i = 0; i < count; i++) {
        const location = generateLocationWithId(i);
        const start = performance.now();
        tree.insert(location.lat, location.lng, location.objectId);
        insertTimings.push(performance.now() - start);
      }

      const insertTotalTime = performance.now() - insertStartTime;
      expect(tree.size()).toBe(count);

      printTimingReport('Insert Timing', insertTimings, count);
      console.log(`    Total Time: ${insertTotalTime.toFixed(2)}ms`);
      console.log(`    Operations: ${count}`);

      await tree.close();
    });

    it('should benchmark insertions and queries on small dataset', async () => {
      const tree = await createSoakTestTree();
      await tree.open();

      const insertCount = 8;
      console.log(`\n  Inserting ${insertCount} locations...`);
      const insertStartTime = performance.now();
      const insertTimings = [];

      for (let i = 0; i < insertCount; i++) {
        const location = generateLocationWithId(i);
        const start = performance.now();
        tree.insert(location.lat, location.lng, location.objectId);
        insertTimings.push(performance.now() - start);
      }

      console.log(`    Completed in ${(performance.now() - insertStartTime).toFixed(2)}ms`);

      const queryCount = 16;
      const queryTimings = [];
      let totalResultsFound = 0;

      console.log(`\n  Performing ${queryCount} bounding box queries...`);
      const queryStartTime = performance.now();

      for (let i = 0; i < queryCount; i++) {
        const bbox = {
          minLat: 30 + Math.random() * 10,
          maxLat: 40 + Math.random() * 10,
          minLng: -100 + Math.random() * 10,
          maxLng: -90 + Math.random() * 10
        };
        const start = performance.now();
        const results = tree.searchBBox(bbox);
        queryTimings.push(performance.now() - start);
        totalResultsFound += results.length;
      }

      const queryTotalTime = performance.now() - queryStartTime;
      printTimingReport('Query Timing', queryTimings, queryCount);
      console.log(`    Total Query Time: ${queryTotalTime.toFixed(2)}ms`);
      console.log(`    Results Found: ${totalResultsFound}`);
      console.log(`    Avg Results/Query: ${(totalResultsFound / queryCount).toFixed(1)}`);

      expect(tree.size()).toBe(insertCount);

      await tree.close();
    });

    it('should benchmark insertions and radius searches', async () => {
      const tree = await createSoakTestTree();
      await tree.open();

      const insertCount = 8;
      console.log(`\n  Inserting ${insertCount} locations...`);
      const insertStartTime = performance.now();
      const insertTimings = [];

      for (let i = 0; i < insertCount; i++) {
        const location = generateLocationWithId(i);
        const start = performance.now();
        tree.insert(location.lat, location.lng, location.objectId);
        insertTimings.push(performance.now() - start);
      }

      console.log(`    Completed in ${(performance.now() - insertStartTime).toFixed(2)}ms`);

      const searchCount = 16;
      const searchTimings = [];
      let totalResultsFound = 0;

      console.log(`\n  Performing ${searchCount} radius searches...`);
      const searchStartTime = performance.now();

      for (let i = 0; i < searchCount; i++) {
        const centerLat = 25 + Math.random() * 24;
        const centerLng = -125 + Math.random() * 59;
        const radiusKm = 50 + Math.random() * 200;
        const start = performance.now();
        const results = tree.searchRadius(centerLat, centerLng, radiusKm);
        searchTimings.push(performance.now() - start);
        totalResultsFound += results.length;
      }

      const searchTotalTime = performance.now() - searchStartTime;
      printTimingReport('Radius Search Timing', searchTimings, searchCount);
      console.log(`    Total Search Time: ${searchTotalTime.toFixed(2)}ms`);
      console.log(`    Results Found: ${totalResultsFound}`);
      console.log(`    Avg Results/Search: ${(totalResultsFound / searchCount).toFixed(1)}`);

      expect(tree.size()).toBe(insertCount);

      await tree.close();
    });

    it('should measure insert latency distribution', async () => {
      const tree = await createSoakTestTree();
      await tree.open();

      const insertCount = 8;
      const insertTimings = [];

      console.log(`\n  === Insert Latency Analysis ===`);
      const insertStartTime = performance.now();

      for (let i = 0; i < insertCount; i++) {
        const location = generateLocationWithId(i);
        const start = performance.now();
        tree.insert(location.lat, location.lng, location.objectId);
        insertTimings.push(performance.now() - start);

        if ((i + 1) % 4 === 0) {
          const rate = ((i + 1) / ((performance.now() - insertStartTime) / 1000)).toFixed(1);
          console.log(`    After ${i + 1}: ${rate} ops/sec`);
        }
      }

      printTimingReport('Insert Operation Latency', insertTimings, insertCount);

      if (insertTimings.length > 1) {
        const firstHalf = insertTimings.slice(0, Math.floor(insertCount / 2));
        const secondHalf = insertTimings.slice(Math.floor(insertCount / 2));
        const firstAvg = firstHalf.reduce((a, b) => a + b, 0) / firstHalf.length;
        const secondAvg = secondHalf.reduce((a, b) => a + b, 0) / secondHalf.length;

        console.log(`\n  Latency Trend Analysis:`);
        console.log(`    First half avg: ${firstAvg.toFixed(4)}ms`);
        console.log(`    Second half avg: ${secondAvg.toFixed(4)}ms`);
        console.log(`    Trend: ${((secondAvg - firstAvg) / firstAvg * 100).toFixed(1)}%`);
      }

      await tree.close();
    });

    it('should measure query performance', async () => {
      const tree = await createSoakTestTree();
      await tree.open();

      const insertCount = 8;
      console.log(`\n  Setting up ${insertCount} locations...`);
      for (let i = 0; i < insertCount; i++) {
        const location = generateLocationWithId(i);
        tree.insert(location.lat, location.lng, location.objectId);
      }

      tree.searchBBox({ minLat: 30, maxLat: 40, minLng: -100, maxLng: -90 });

      const queryCount = 32;
      const bboxTimings = [];
      const radiusTimings = [];

      console.log(`\n  === Query Performance Measurement ===`);
      console.log(`  Performing ${queryCount} each of bbox and radius queries...`);

      const bboxStart = performance.now();
      for (let i = 0; i < queryCount; i++) {
        const bbox = {
          minLat: 30 + Math.random() * 10,
          maxLat: 40 + Math.random() * 10,
          minLng: -100 + Math.random() * 10,
          maxLng: -90 + Math.random() * 10
        };
        const start = performance.now();
        tree.searchBBox(bbox);
        bboxTimings.push(performance.now() - start);
      }
      const bboxTotal = performance.now() - bboxStart;

      const radiusStart = performance.now();
      for (let i = 0; i < queryCount; i++) {
        const start = performance.now();
        tree.searchRadius(25 + Math.random() * 24, -125 + Math.random() * 59, 100);
        radiusTimings.push(performance.now() - start);
      }
      const radiusTotal = performance.now() - radiusStart;

      console.log(`\n  Bounding Box Queries:`);
      printTimingReport('BBox Query', bboxTimings, queryCount);
      console.log(`    Total Time: ${bboxTotal.toFixed(2)}ms`);

      console.log(`\n  Radius Queries:`);
      printTimingReport('Radius Query', radiusTimings, queryCount);
      console.log(`    Total Time: ${radiusTotal.toFixed(2)}ms`);

      expect(tree.size()).toBe(insertCount);

      await tree.close();
    });

    it.skipIf(isBrowser)('should measure memory usage', async () => {
      const tree = await createSoakTestTree();
      await tree.open();

      const startMemory = process.memoryUsage();
      const insertCount = 8;

      console.log(`\n  === Memory Usage Analysis ===`);
      console.log(`  Initial Heap: ${(startMemory.heapUsed / 1024 / 1024).toFixed(2)} MB`);

      for (let i = 0; i < insertCount; i++) {
        const location = generateLocationWithId(i);
        tree.insert(location.lat, location.lng, location.objectId);

        if ((i + 1) % 2 === 0) {
          const currentMemory = process.memoryUsage();
          const heapDiff = (currentMemory.heapUsed - startMemory.heapUsed) / 1024 / 1024;
          console.log(`  After ${i + 1} insertions: ${currentMemory.heapUsed / 1024 / 1024 | 0} MB (Δ ${heapDiff.toFixed(2)} MB)`);
        }
      }

      const endMemory = process.memoryUsage();
      const totalHeapIncrease = (endMemory.heapUsed - startMemory.heapUsed) / 1024 / 1024;

      console.log(`\n  === Memory Summary ===`);
      console.log(`    Total Insertions: ${insertCount}`);
      console.log(`    Total Heap Increase: ${totalHeapIncrease.toFixed(2)} MB`);
      console.log(`    Memory per Entry: ${((totalHeapIncrease * 1024 * 1024) / insertCount).toFixed(0)} bytes`);

      expect(tree.size()).toBe(insertCount);

      await tree.close();
    });
  });
}
