/**
 * Shared TextLog behavioral suite, parameterized by implementation.
 *
 * Both test/textlog.test.js (pure-JS) and test/textlog-wasm.test.js (WASM) call
 * runTextLogSuite with their TextLog class so the identical assertions run
 * against each. `label` distinguishes runs (and test filenames in OPFS).
 */
import { describe, it, expect, beforeAll, beforeEach, afterEach, afterAll } from 'vitest';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';

export function runTextLogSuite(label, TextLog, hasOPFS) {
  describe.skipIf(!hasOPFS)(`${label}: TextLog`, function() {
    let testFileCounter = 0;
    let rootDirHandle = null;

    beforeAll(async () => {
      if (navigator.storage && navigator.storage.getDirectory) {
        rootDirHandle = await navigator.storage.getDirectory();
      }
    });

    const createdFiles = [];

    function getTestFilename() {
      const name = `test-textlog-${label}-${Date.now()}-${testFileCounter++}.bj`;
      createdFiles.push(name);
      return name;
    }

    afterAll(async () => {
      if (!rootDirHandle) return;
      for (const name of createdFiles) await deleteFile(rootDirHandle, name);
    });

    async function createTestLog(diffsPerSnapshot = 10) {
      const filename = getTestFilename();
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: true });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const log = new TextLog(syncHandle, diffsPerSnapshot);
      log._testFilename = filename;
      return log;
    }

    async function reopenLog(filename, diffsPerSnapshot = 10) {
      const fileHandle = await getFileHandle(rootDirHandle, filename, { create: false });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      const log = new TextLog(syncHandle, diffsPerSnapshot);
      log._testFilename = filename;
      return log;
    }

    async function cleanupFile(filename) {
      if (rootDirHandle) {
        await deleteFile(rootDirHandle, filename);
      }
    }

    describe('Constructor', function() {
      it('should create a TextLog with default diffsPerSnapshot', async function() {
        const log = await createTestLog();
        expect(log.diffsPerSnapshot).toBe(10);
        await log.file.syncAccessHandle.close();
        await cleanupFile(log._testFilename);
      });

      it('should create a TextLog with custom diffsPerSnapshot', async function() {
        const log = await createTestLog(5);
        expect(log.diffsPerSnapshot).toBe(5);
        await log.file.syncAccessHandle.close();
        await cleanupFile(log._testFilename);
      });

      it('should throw error for invalid diffsPerSnapshot', async function() {
        const fileHandle = await getFileHandle(rootDirHandle, getTestFilename(), { create: true });
        const syncHandle = await fileHandle.createSyncAccessHandle();
        expect(() => new TextLog(syncHandle, 0)).toThrow('diffsPerSnapshot must be at least 1');
        expect(() => new TextLog(syncHandle, -1)).toThrow('diffsPerSnapshot must be at least 1');
        await syncHandle.close();
      });
    });

    describe('Basic operations', function() {
      let log;

      afterEach(async function() {
        if (log && log.isOpen) {
          await log.close();
        }
        if (log && log._testFilename) {
          await cleanupFile(log._testFilename);
        }
      });

      it('should create and open new log', async function() {
        log = await createTestLog(5);
        await log.open();

        expect(log.isOpen).toBe(true);
        expect(log.getCurrentVersion()).toBe(0);

        await log.close();
      });

      it('should add first version as snapshot', async function() {
        log = await createTestLog(5);
        await log.open();

        const version = await log.addVersion('Hello, World!');
        expect(version).toBe(1);
        expect(log.getCurrentVersion()).toBe(1);

        await log.close();
      });

      it('should retrieve added version', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Hello, World!');
        const text = await log.getVersion(1);

        expect(text).toBe('Hello, World!');

        await log.close();
      });

      it('should add multiple versions', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Version 1');
        await log.addVersion('Version 2');
        await log.addVersion('Version 3');

        expect(log.getCurrentVersion()).toBe(3);

        const v1 = await log.getVersion(1);
        const v2 = await log.getVersion(2);
        const v3 = await log.getVersion(3);

        expect(v1).toBe('Version 1');
        expect(v2).toBe('Version 2');
        expect(v3).toBe('Version 3');

        await log.close();
      });

      it('should handle empty text', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('');
        const text = await log.getVersion(1);

        expect(text).toBe('');

        await log.close();
      });

      it('should throw error for invalid version', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Test');

        await expect(log.getVersion(0)).rejects.toThrow('Invalid version');
        await expect(log.getVersion(2)).rejects.toThrow('Invalid version');
        await expect(log.getVersion(-1)).rejects.toThrow('Invalid version');

        await log.close();
      });
    });

    describe('Snapshot vs Diff strategy', function() {
      let filename;
      let log;

      beforeEach(function() {
        filename = getTestFilename();
      });

      afterEach(async function() {
        if (log && log.isOpen) {
          await log.close();
        }
        await cleanupFile(filename);
      });

      it('should create snapshot every N diffs', async function() {
        // Set diffsPerSnapshot to 3
        log = await createTestLog(3);
        await log.open();

        // Add 5 versions
        await log.addVersion('Version 1'); // Snapshot (first version)
        await log.addVersion('Version 2'); // Diff 1
        await log.addVersion('Version 3'); // Diff 2
        await log.addVersion('Version 4'); // Diff 3
        await log.addVersion('Version 5'); // Snapshot (after 3 diffs)

        // Verify all versions are retrievable
        expect(await log.getVersion(1)).toBe('Version 1');
        expect(await log.getVersion(2)).toBe('Version 2');
        expect(await log.getVersion(3)).toBe('Version 3');
        expect(await log.getVersion(4)).toBe('Version 4');
        expect(await log.getVersion(5)).toBe('Version 5');

        await log.close();
      });

      it('should handle many versions with periodic snapshots', async function() {
        log = await createTestLog(5);
        await log.open();

        // Add 12 versions (should create snapshots at v1, v6, v11)
        for (let i = 1; i <= 12; i++) {
          await log.addVersion(`Version ${i} text content`);
        }

        // Verify all versions are retrievable
        for (let i = 1; i <= 12; i++) {
          const text = await log.getVersion(i);
          expect(text).toBe(`Version ${i} text content`);
        }

        await log.close();
      });
    });

    describe('Diff functionality', function() {
      let filename;
      let log;

      beforeEach(function() {
        filename = getTestFilename();
      });

      afterEach(async function() {
        if (log && log.isOpen) {
          await log.close();
        }
        await cleanupFile(filename);
      });

      it('should create human-readable diff between versions', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Hello\nWorld\n');
        await log.addVersion('Hello\nBeautiful World\n');

        const diff = await log.getDiff(1, 2);

        expect(diff).toContain('--- version 1');
        expect(diff).toContain('+++ version 2');
        expect(diff).toContain('-World');
        expect(diff).toContain('+Beautiful World');

        await log.close();
      });

      it('should handle diff with no changes', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Same text');
        await log.addVersion('Same text');

        const diff = await log.getDiff(1, 2);

        // No hunks means no changes
        expect(diff).toContain('--- version 1');
        expect(diff).toContain('+++ version 2');

        await log.close();
      });

      it('should create diff between non-adjacent versions', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Line 1\nLine 2\nLine 3\n');
        await log.addVersion('Line 1\nLine 2 modified\nLine 3\n');
        await log.addVersion('Line 1\nLine 2 modified\nLine 3\nLine 4\n');

        const diff = await log.getDiff(1, 3);

        expect(diff).toContain('--- version 1');
        expect(diff).toContain('+++ version 3');
        expect(diff).toContain('-Line 2');
        expect(diff).toContain('+Line 2 modified');
        expect(diff).toContain('+Line 4');

        await log.close();
      });

      it('should throw error for invalid diff versions', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Test');

        await expect(log.getDiff(0, 1)).rejects.toThrow('Invalid fromVersion');
        await expect(log.getDiff(1, 2)).rejects.toThrow('Invalid toVersion');
        await expect(log.getDiff(2, 1)).rejects.toThrow('Invalid fromVersion');

        await log.close();
      });
    });

    describe('Hash functionality', function() {
      let filename;
      let log;

      beforeEach(function() {
        filename = getTestFilename();
      });

      afterEach(async function() {
        if (log && log.isOpen) {
          await log.close();
        }
        await cleanupFile(filename);
      });

      it('should compute SHA hash for each version', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Hello, World!');
        const hash = await log.getVersionHash(1);

        expect(hash).toBeDefined();
        expect(typeof hash).toBe('string');
        expect(hash.length).toBe(64); // SHA-256 produces 64 hex characters

        await log.close();
      });

      it('should produce same hash for same content', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Same content');
        await log.addVersion('Same content');

        const hash1 = await log.getVersionHash(1);
        const hash2 = await log.getVersionHash(2);

        expect(hash1).toBe(hash2);

        await log.close();
      });

      it('should produce different hash for different content', async function() {
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Content A');
        await log.addVersion('Content B');

        const hash1 = await log.getVersionHash(1);
        const hash2 = await log.getVersionHash(2);

        expect(hash1).not.toBe(hash2);

        await log.close();
      });
    });

    describe('Persistence', function() {
      let filename;
      let log;

      beforeEach(function() {
        filename = getTestFilename();
      });

      afterEach(async function() {
        if (log && log.isOpen) {
          await log.close();
        }
        await cleanupFile(filename);
      });

      it('should persist data across open/close cycles', async function() {
        // Create and populate log
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Version 1');
        await log.addVersion('Version 2');
        await log.addVersion('Version 3');

        const filename = log._testFilename;
        await log.close();

        // Reopen and verify
        log = await reopenLog(filename, 5);
        await log.open();

        expect(log.getCurrentVersion()).toBe(3);
        expect(await log.getVersion(1)).toBe('Version 1');
        expect(await log.getVersion(2)).toBe('Version 2');
        expect(await log.getVersion(3)).toBe('Version 3');

        await log.close();
      });

      it('should maintain metadata across sessions', async function() {
        // Create log with specific settings
        log = await createTestLog(7);
        await log.open();

        await log.addVersion('Test');

        const filename2 = log._testFilename;
        await log.close();

        // Reopen and verify settings
        log = await reopenLog(filename2, 7);
        await log.open();

        expect(log.diffsPerSnapshot).toBe(7);
        expect(log.getCurrentVersion()).toBe(1);

        await log.close();
      });

      it('should handle adding versions after reopening', async function() {
        // Create log and add versions
        log = await createTestLog(5);
        await log.open();

        await log.addVersion('Version 1');
        await log.addVersion('Version 2');

        const filename3 = log._testFilename;
        await log.close();

        // Reopen and add more versions
        log = await reopenLog(filename3, 5);
        await log.open();

        await log.addVersion('Version 3');
        await log.addVersion('Version 4');

        expect(log.getCurrentVersion()).toBe(4);
        expect(await log.getVersion(1)).toBe('Version 1');
        expect(await log.getVersion(2)).toBe('Version 2');
        expect(await log.getVersion(3)).toBe('Version 3');
        expect(await log.getVersion(4)).toBe('Version 4');

        await log.close();
      });
    });

    describe('Edge cases', function() {
      let filename;
      let log;

      beforeEach(function() {
        filename = getTestFilename();
      });

      afterEach(async function() {
        if (log && log.isOpen) {
          await log.close();
        }
        await cleanupFile(filename);
      });

      it('should handle large text content', async function() {
        log = await createTestLog(5);
        await log.open();

        const largeText = 'Lorem ipsum '.repeat(1000);
        await log.addVersion(largeText);

        const retrieved = await log.getVersion(1);
        expect(retrieved).toBe(largeText);

        await log.close();
      });

      it('should handle special characters', async function() {
        log = await createTestLog(5);
        await log.open();

        const specialText = '🎉 Unicode: café, naïve, 中文, 日本語\n\t\r\n';
        await log.addVersion(specialText);

        const retrieved = await log.getVersion(1);
        expect(retrieved).toBe(specialText);

        await log.close();
      });

      it('should handle line-by-line changes', async function() {
        log = await createTestLog(5);
        await log.open();

        const text1 = 'Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n';
        const text2 = 'Line 1\nLine 2 modified\nLine 3\nLine 4\nLine 5\n';
        const text3 = 'Line 1\nLine 2 modified\nLine 3\nLine 4 changed\nLine 5\n';

        await log.addVersion(text1);
        await log.addVersion(text2);
        await log.addVersion(text3);

        expect(await log.getVersion(1)).toBe(text1);
        expect(await log.getVersion(2)).toBe(text2);
        expect(await log.getVersion(3)).toBe(text3);

        await log.close();
      });
    });
  });
}
