/**
 * Compaction & tiling tests for the WASM EntryLog.
 *
 * Prefix compaction is how the Raft log stays bounded: once the state
 * machine has snapshotted through index N, entries <= N are dead weight and
 * compact(dst, N, termAt(N)) rewrites only the live suffix into a fresh
 * file whose base records the snapshot boundary (index AND term — the term
 * is what keeps the AppendEntries consistency check working right at the
 * boundary). createBaseIndex/baseTerm cover the same boundary for a
 * follower installing a snapshot from scratch.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, EntryLog } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

const dec = new TextDecoder();

describe.skipIf(!hasOPFS)('WASM EntryLog compaction & tiling', () => {
  let root = null;
  let counter = 0;
  const files = [];

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const name = () => {
    const n = `test-entrylog-cmp-${Date.now()}-${counter++}.bj`;
    files.push(n);
    return n;
  };

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  async function sync(filename, create = false) {
    const fh = await getFileHandle(root, filename, { create });
    return fh.createSyncAccessHandle();
  }

  async function openLog(filename, options, create = true) {
    const log = new EntryLog(await sync(filename, create), options);
    await log.open();
    return log;
  }

  /** 10 entries across terms 1..3 (terms 1,1,1,2,2,2,3,3,3,3). */
  async function fill(log) {
    log.setHardState(3, 5);
    for (let i = 1; i <= 10; i++) {
      const term = Math.min(3, Math.ceil(i / 3));
      log.append(term, `entry-${i}`);
    }
    log.setCommitIndex(7);
    log.sync();
  }

  describe('compaction after a snapshot', () => {
    it('rewrites the live suffix into a tile based at the snapshot boundary', async () => {
      const src = name(), dst = name();
      const log = await openLog(src);
      await fill(log);
      const result = await log.compact(await sync(dst, true), 6, log.termAt(6));
      expect(result.newSize).toBeGreaterThan(0);
      await log.close();

      const tile = await openLog(dst, undefined, false);
      expect(tile.baseIndex).toBe(6);
      expect(tile.baseTerm).toBe(2);
      expect(tile.lastIndex).toBe(10);
      expect(tile.lastTerm).toBe(3);
      // Hard state and commit index carried over (commitIndex was 7 >= 6).
      expect(tile.currentTerm).toBe(3);
      expect(tile.votedFor).toBe(5);
      expect(tile.commitIndex).toBe(7);
      for (let i = 7; i <= 10; i++) {
        expect(dec.decode(tile.get(i).payload)).toBe(`entry-${i}`);
      }
      // The compacted-away range answers as compacted, not as present.
      expect(() => tile.get(6)).toThrow('Argument out of range');
      expect(() => tile.getBatch(6)).toThrow('Argument out of range');
      expect(tile.termAt(6)).toBe(2);   // the boundary term is still known
      expect(tile.verify()).toBe(true);

      // The tile keeps working: truncate within it, append beyond it.
      tile.truncateFrom(9);
      expect(tile.append(3, 'entry-9-replaced')).toBe(9);
      tile.sync();
      expect(dec.decode(tile.get(9).payload)).toBe('entry-9-replaced');
      await tile.close();
    });

    it('raises the carried commit index to the snapshot boundary', async () => {
      const src = name(), dst = name();
      const log = await openLog(src);
      log.setHardState(1);
      for (let i = 1; i <= 5; i++) log.append(1, `e${i}`);
      log.setCommitIndex(2);
      log.sync();
      // Compacting through 4 means entries <= 4 are snapshotted, hence
      // committed — the tile must not claim a commit index below its base.
      await log.compact(await sync(dst, true), 4, 1);
      await log.close();

      const tile = await openLog(dst, undefined, false);
      expect(tile.commitIndex).toBe(4);
      await tile.close();
    });

    it('drops truncated dead bytes and shrinks the file', async () => {
      const src = name(), dst = name();
      const log = await openLog(src);
      log.setHardState(2);
      const blob = 'x'.repeat(2000);
      for (let i = 1; i <= 10; i++) log.append(1, `${i}-${blob}`);
      log.sync();
      log.truncateFrom(4);                       // 7 dead 2 KB entries
      for (let i = 4; i <= 6; i++) log.append(2, `${i}-replaced`);
      log.sync();
      const result = await log.compact(await sync(dst, true), 3, 1);
      expect(result.newSize).toBeLessThan(result.oldSize);
      await log.close();

      const tile = await openLog(dst, undefined, false);
      expect(tile.lastIndex).toBe(6);
      expect(dec.decode(tile.get(4).payload)).toBe('4-replaced');
      expect(dec.decode(tile.get(6).payload)).toBe('6-replaced');
      expect(tile.verify()).toBe(true);
      await tile.close();
    });

    it('compacting through lastIndex leaves an empty tile that continues', async () => {
      const src = name(), dst = name();
      const log = await openLog(src);
      await fill(log);
      await log.compact(await sync(dst, true), 10, 3);
      await log.close();

      const tile = await openLog(dst, undefined, false);
      expect(tile.baseIndex).toBe(10);
      expect(tile.lastIndex).toBe(10);
      expect(tile.getBatch(11)).toEqual([]);
      expect(tile.append(3, 'entry-11')).toBe(11);
      tile.sync();
      expect(dec.decode(tile.get(11).payload)).toBe('entry-11');
      await tile.close();
    });

    it('rejects a wrong boundary term or index', async () => {
      const src = name();
      const log = await openLog(src);
      await fill(log);
      // A wrong term here would poison every future consistency check.
      // (A failed compact leaves the destination handle open — close it.)
      const h1 = await sync(name(), true);
      await expect(log.compact(h1, 6, 3)).rejects.toThrow('builder state error');
      await h1.close();
      const h2 = await sync(name(), true);
      await expect(log.compact(h2, 11, 3)).rejects.toThrow('Argument out of range');
      await h2.close();
      await log.close();
    });

    it('refuses to compact with buffered (unsynced) appends', async () => {
      const src = name();
      const log = await openLog(src);
      await fill(log);
      log.append(3, 'buffered');
      const dst = await sync(name(), true);
      await expect(log.compact(dst, 6, 2)).rejects.toThrow('builder state error');
      await dst.close();
      await log.close();
    });
  });

  describe('tiling (a follower installing a snapshot)', () => {
    it('creates a fresh tile at a nonzero snapshot boundary', async () => {
      const file = name();
      const log = await openLog(file, { baseIndex: 100, baseTerm: 5 });
      expect(log.baseIndex).toBe(100);
      expect(log.baseTerm).toBe(5);
      expect(log.lastIndex).toBe(100);
      expect(log.lastTerm).toBe(5);
      expect(log.currentTerm).toBe(5);   // starts at the snapshot's term
      expect(log.commitIndex).toBe(100); // snapshotted entries are committed
      expect(log.termAt(100)).toBe(5);   // the AppendEntries prev-check works
      expect(() => log.get(100)).toThrow('Argument out of range');

      // Entry terms continue from the boundary term.
      expect(() => log.append(4, 'stale term')).toThrow('builder state error');
      expect(log.append(5, 'first-of-tile')).toBe(101);
      log.sync();
      await log.close();

      const back = await openLog(file, undefined, false);
      expect(back.baseIndex).toBe(100);
      expect(back.baseTerm).toBe(5);
      expect(back.lastIndex).toBe(101);
      expect(dec.decode(back.get(101).payload)).toBe('first-of-tile');
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('the creation base is ignored when opening an existing file', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'a');
      log.sync();
      await log.close();

      const back = await openLog(file, { baseIndex: 50, baseTerm: 9 }, false);
      expect(back.baseIndex).toBe(0);
      expect(back.lastIndex).toBe(1);
      await back.close();
    });
  });
});
