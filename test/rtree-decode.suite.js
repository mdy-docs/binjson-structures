/**
 * Shared rtree-decode CLI suite, parameterized by the implementation used to
 * *write* the file. The CLI (bin/rtree-decode.js) always reads with the JS
 * reference RTree, so the WASM run additionally proves on-disk format
 * compatibility. See test/rtree.suite.js for the pattern.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { execFile } from 'child_process';
import { deleteFile, getFileHandle, ObjectId } from '../third_party/binjson/js/binjson.js';

function runCli(filePath) {
  return new Promise((resolve, reject) => {
    execFile('node', ['bin/rtree.js', filePath], { cwd: process.cwd() }, (error, stdout, stderr) => {
      if (error) {
        error.stdout = stdout;
        error.stderr = stderr;
        reject(error);
        return;
      }
      resolve({ stdout, stderr });
    });
  });
}

export function runRTreeDecodeSuite(label, RTree, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: rtree-decode CLI`, () => {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    function getTestFilename() {
      return `test-rtree-decode-${label}-${Date.now()}-${testFileCounter++}.bj`;
    }

    async function createTestTree(order = 4) {
      const filename = getTestFilename();
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
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

    it('decodes and prints R-tree with spatial points', async () => {
      const tree = await createTestTree(4);
      await tree.open();
      const filename = tree._testFilename;

      const id1 = new ObjectId('5f1d7f3a0b0c0d0e0f101112');
      const id2 = new ObjectId('6a6b6c6d6e6f707172737475');
      const id3 = new ObjectId('7b7c7d7e7f80818283848586');

      await tree.insert(40.7128, -74.0060, id1); // NYC
      await tree.insert(34.0522, -118.2437, id2); // LA
      await tree.insert(41.8781, -87.6298, id3); // Chicago

      await tree.close();

      const { stdout } = await runCli(filename);
      await cleanupFile(filename);

      expect(stdout).toContain('0:');
      expect(stdout).toContain('1:');
      expect(stdout).toContain('2:');

      expect(stdout).toContain('ObjectId(5f1d7f3a0b0c0d0e0f101112)');
      expect(stdout).toContain('ObjectId(6a6b6c6d6e6f707172737475)');
      expect(stdout).toContain('ObjectId(7b7c7d7e7f80818283848586)');

      expect(stdout).toContain('lat: 40.7128');
      expect(stdout).toContain('lng: -74.006');
      expect(stdout).toContain('lat: 34.0522');
      expect(stdout).toContain('lng: -118.2437');
      expect(stdout).toContain('lat: 41.8781');
      expect(stdout).toContain('lng: -87.6298');
    });

    it('handles empty R-tree', async () => {
      const tree = await createTestTree(4);
      await tree.open();
      const filename = tree._testFilename;
      await tree.close();

      const { stdout } = await runCli(filename);
      await cleanupFile(filename);

      expect(stdout).toContain('R-tree is empty');
    });

    it('displays all points from tree with many entries', async () => {
      const tree = await createTestTree(4);
      await tree.open();
      const filename = tree._testFilename;

      const points = [];
      for (let i = 0; i < 10; i++) {
        const id = new ObjectId();
        const lat = 25 + Math.random() * 24;
        const lng = -125 + Math.random() * 59;
        points.push({ id, lat, lng });
        await tree.insert(lat, lng, id);
      }

      await tree.close();

      const { stdout } = await runCli(filename);
      await cleanupFile(filename);

      for (const point of points) {
        expect(stdout).toContain(`ObjectId(${point.id.toHexString()})`);
      }
    });

    it('displays points with different ObjectId formats', async () => {
      const tree = await createTestTree(4);
      await tree.open();
      const filename = tree._testFilename;

      const id1 = new ObjectId(); // Generated
      const id2 = new ObjectId('000000000000000000000000'); // All zeros
      const id3 = new ObjectId('ffffffffffffffffffffffff'); // All Fs

      await tree.insert(40.0, -74.0, id1);
      await tree.insert(41.0, -75.0, id2);
      await tree.insert(42.0, -76.0, id3);

      await tree.close();

      const { stdout } = await runCli(filename);
      await cleanupFile(filename);

      expect(stdout).toContain('ObjectId(000000000000000000000000)');
      expect(stdout).toContain('ObjectId(ffffffffffffffffffffffff)');
      expect(stdout).toContain(`ObjectId(${id1.toHexString()})`);
    });
  });
}
