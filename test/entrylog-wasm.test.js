/**
 * Core behavioral tests for the WASM EntryLog (Raft log / write-ahead log):
 * append/sync/get/getBatch, the term rules, hard state, the advisory commit
 * index, suffix truncation, and verify(). Durability and crash recovery live
 * in entrylog.durability-wasm.test.js; compaction and tiling in
 * entrylog.compaction-wasm.test.js.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, EntryLog, ENTRYLOG_TYPE } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

const enc = new TextEncoder();
const dec = new TextDecoder();

describe.skipIf(!hasOPFS)('WASM EntryLog', () => {
  let root = null;
  let counter = 0;
  const files = [];

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const name = () => {
    const n = `test-entrylog-${Date.now()}-${counter++}.bj`;
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

  async function openLog(filename, options) {
    const log = new EntryLog(await sync(filename, true), options);
    await log.open();
    return log;
  }

  describe('creation & accessors', () => {
    it('creates an empty log with zeroed state', async () => {
      const log = await openLog(name());
      expect(log.baseIndex).toBe(0);
      expect(log.baseTerm).toBe(0);
      expect(log.lastIndex).toBe(0);
      expect(log.lastTerm).toBe(0);
      expect(log.currentTerm).toBe(0);
      expect(log.votedFor).toBe(0);
      expect(log.commitIndex).toBe(0);
      expect(log.verify()).toBe(true);
      expect(log.getBatch(1)).toEqual([]);
      await log.close();
    });

    it('refuses accessors and operations when not open', async () => {
      const log = new EntryLog(await sync(name(), true));
      expect(() => log.lastIndex).toThrow('EntryLog is not open');
      expect(() => log.append(0, 'x')).toThrow('EntryLog is not open');
      await log.open();
      await expect(log.open()).rejects.toThrow('EntryLog is already open');
      await log.close();
    });
  });

  describe('append / sync / get', () => {
    it('assigns strictly contiguous indexes starting at 1', async () => {
      const log = await openLog(name());
      expect(log.append(0, 'one')).toBe(1);
      expect(log.append(0, 'two')).toBe(2);
      expect(log.append(0, 'three')).toBe(3);
      log.sync();
      expect(log.lastIndex).toBe(3);
      expect(log.lastTerm).toBe(0);
      await log.close();
    });

    it('round-trips string payloads as UTF-8', async () => {
      const log = await openLog(name());
      log.append(0, 'héllo wörld ✓');
      log.sync();
      const e = log.get(1);
      expect(dec.decode(e.payload)).toBe('héllo wörld ✓');
      expect(e.index).toBe(1);
      expect(e.term).toBe(0);
      expect(e.type).toBe(ENTRYLOG_TYPE.NORMAL);
      await log.close();
    });

    it('round-trips binary payloads verbatim (including NULs)', async () => {
      const log = await openLog(name());
      const payload = new Uint8Array([0, 255, 1, 0, 128, 7]);
      log.append(0, payload, ENTRYLOG_TYPE.CONFIG);
      log.sync();
      const e = log.get(1);
      expect(e.payload).toEqual(payload);
      expect(e.type).toBe(ENTRYLOG_TYPE.CONFIG);
      await log.close();
    });

    it('accepts empty payloads (a leader no-op entry)', async () => {
      const log = await openLog(name());
      log.setHardState(3);
      log.append(3, new Uint8Array(0), ENTRYLOG_TYPE.NOOP);
      log.sync();
      const e = log.get(1);
      expect(e.payload.length).toBe(0);
      expect(e.term).toBe(3);
      expect(e.type).toBe(ENTRYLOG_TYPE.NOOP);
      await log.close();
    });

    it('stores host-defined entry types verbatim', async () => {
      const log = await openLog(name());
      log.append(0, 'custom', 0x42);
      log.sync();
      expect(log.get(1).type).toBe(0x42);
      await log.close();
    });

    it('round-trips a large payload (~300 KB)', async () => {
      const log = await openLog(name());
      const big = new Uint8Array(300 * 1024);
      for (let i = 0; i < big.length; i++) big[i] = i % 251;
      log.append(0, big);
      log.sync();
      expect(log.get(1).payload).toEqual(big);
      await log.close();
    });

    it('rejects reads outside (baseIndex, lastIndex]', async () => {
      const log = await openLog(name());
      log.append(0, 'only');
      log.sync();
      expect(() => log.get(0)).toThrow('Argument out of range');
      expect(() => log.get(2)).toThrow('Argument out of range');
      await log.close();
    });

    it('a sync with nothing buffered commits metadata only', async () => {
      const log = await openLog(name());
      log.append(0, 'a');
      log.sync();
      const before = log.syncAccessHandle.getSize();
      log.sync();
      expect(log.syncAccessHandle.getSize()).toBeGreaterThan(before);
      expect(log.lastIndex).toBe(1);
      expect(log.verify()).toBe(true);
      await log.close();
    });
  });

  describe('term rules', () => {
    it('rejects an entry term above the durable currentTerm', async () => {
      const log = await openLog(name());
      // The classic Raft bug: appending with a term the hard state has not
      // reached would let a node ack entries it could later contradict.
      expect(() => log.append(1, 'early')).toThrow('builder state error');
      log.setHardState(1);
      expect(log.append(1, 'now')).toBe(1);
      await log.close();
    });

    it('rejects an entry term below lastTerm (terms never fall)', async () => {
      const log = await openLog(name());
      log.setHardState(2);
      log.append(2, 'at term 2');
      expect(() => log.append(1, 'stale')).toThrow('builder state error');
      log.sync();
      await log.close();
    });

    it('allows terms to rise across entries within currentTerm', async () => {
      const log = await openLog(name());
      log.setHardState(3);
      log.append(1, 'a');
      log.append(2, 'b');
      log.append(3, 'c');
      log.sync();
      expect(log.termAt(1)).toBe(1);
      expect(log.termAt(2)).toBe(2);
      expect(log.termAt(3)).toBe(3);
      expect(log.lastTerm).toBe(3);
      expect(log.verify()).toBe(true);
      await log.close();
    });

    it('termAt answers the base boundary and rejects out-of-range indexes', async () => {
      const log = await openLog(name());
      expect(log.termAt(0)).toBe(0);   // baseIndex -> baseTerm
      expect(() => log.termAt(1)).toThrow('Argument out of range');
      await log.close();
    });
  });

  describe('hard state', () => {
    it('persists term and vote', async () => {
      const log = await openLog(name());
      log.setHardState(5, 42);
      expect(log.currentTerm).toBe(5);
      expect(log.votedFor).toBe(42);
      await log.close();
    });

    it('refuses to move the term backwards', async () => {
      const log = await openLog(name());
      log.setHardState(5);
      expect(() => log.setHardState(4)).toThrow('builder state error');
      await log.close();
    });

    it('enforces one vote per term', async () => {
      const log = await openLog(name());
      log.setHardState(2, 7);
      expect(() => log.setHardState(2, 9)).toThrow('builder state error');
      log.setHardState(2, 7);              // re-affirming the same vote is fine
      expect(log.votedFor).toBe(7);
      log.setHardState(2);                 // NONE never retracts a cast vote
      expect(log.votedFor).toBe(7);
      await log.close();
    });

    it('a new term resets the previous vote', async () => {
      const log = await openLog(name());
      log.setHardState(2, 7);
      log.setHardState(3);
      expect(log.currentTerm).toBe(3);
      expect(log.votedFor).toBe(0);
      log.setHardState(3, 9);              // and the fresh term may vote anew
      expect(log.votedFor).toBe(9);
      await log.close();
    });

    it('commits buffered entries along with a hard-state change', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'buffered');
      log.setHardState(1, 3);              // commits immediately, entry included
      await log.close();

      const back = new EntryLog(await sync(file));
      await back.open();
      expect(back.lastIndex).toBe(1);
      expect(dec.decode(back.get(1).payload)).toBe('buffered');
      expect(back.currentTerm).toBe(1);
      expect(back.votedFor).toBe(3);
      await back.close();
    });
  });

  describe('commit index', () => {
    it('stages within bounds and never decreases', async () => {
      const log = await openLog(name());
      log.append(0, 'a');
      log.append(0, 'b');
      log.sync();
      log.setCommitIndex(2);
      expect(log.commitIndex).toBe(2);
      expect(() => log.setCommitIndex(1)).toThrow('builder state error');
      expect(() => log.setCommitIndex(3)).toThrow('Argument out of range');
      log.setCommitIndex(2);               // idempotent
      await log.close();
    });
  });

  describe('getBatch', () => {
    it('returns all entries with their fields', async () => {
      const log = await openLog(name());
      log.setHardState(2);
      log.append(1, 'alpha');
      log.append(2, 'beta', ENTRYLOG_TYPE.NOOP);
      log.sync();
      const batch = log.getBatch(1);
      expect(batch.length).toBe(2);
      expect(batch[0].index).toBe(1);
      expect(batch[0].term).toBe(1);
      expect(batch[0].type).toBe(ENTRYLOG_TYPE.NORMAL);
      expect(dec.decode(batch[0].payload)).toBe('alpha');
      expect(batch[1].index).toBe(2);
      expect(batch[1].term).toBe(2);
      expect(batch[1].type).toBe(ENTRYLOG_TYPE.NOOP);
      expect(dec.decode(batch[1].payload)).toBe('beta');
      await log.close();
    });

    it('starts mid-log and honors the byte budget (at least one entry)', async () => {
      const log = await openLog(name());
      for (let i = 1; i <= 6; i++) log.append(0, `payload-${i}-${'x'.repeat(100)}`);
      log.sync();
      const tail = log.getBatch(4);
      expect(tail.map((e) => e.index)).toEqual([4, 5, 6]);
      // ~110-byte payloads against a 1-byte budget: exactly one entry per pull.
      expect(log.getBatch(2, 1).map((e) => e.index)).toEqual([2]);
      // A 150-byte budget crosses into (and stops after) the second entry.
      expect(log.getBatch(2, 150).map((e) => e.index)).toEqual([2, 3]);
      await log.close();
    });

    it('returns [] past the end and rejects the compacted range', async () => {
      const log = await openLog(name());
      log.append(0, 'a');
      log.sync();
      expect(log.getBatch(2)).toEqual([]);
      expect(() => log.getBatch(0)).toThrow('Argument out of range');
      await log.close();
    });
  });

  describe('suffix truncation (the Raft conflict rule)', () => {
    async function fill(log) {
      log.setHardState(2);
      log.append(1, 'e1');
      log.append(1, 'e2');
      log.append(2, 'e3');
      log.append(2, 'e4');
      log.append(2, 'e5');
      log.sync();
    }

    it('cuts the log back and re-appends replacements', async () => {
      const log = await openLog(name());
      await fill(log);
      log.truncateFrom(3);
      expect(log.lastIndex).toBe(2);
      expect(log.lastTerm).toBe(1);
      expect(() => log.get(3)).toThrow('Argument out of range');
      expect(dec.decode(log.get(2).payload)).toBe('e2');

      expect(log.append(2, 'e3-replaced')).toBe(3);
      log.sync();
      expect(dec.decode(log.get(3).payload)).toBe('e3-replaced');
      expect(log.verify()).toBe(true);
      await log.close();
    });

    it('truncating just past the end is a no-op', async () => {
      const log = await openLog(name());
      await fill(log);
      log.truncateFrom(6);
      expect(log.lastIndex).toBe(5);
      await log.close();
    });

    it('refuses to truncate committed entries', async () => {
      const log = await openLog(name());
      await fill(log);
      log.setCommitIndex(3);
      log.sync();
      expect(() => log.truncateFrom(3)).toThrow('builder state error');
      log.truncateFrom(4);                 // above the commit index is fine
      expect(log.lastIndex).toBe(3);
      await log.close();
    });

    it('refuses to truncate the base or beyond last+1', async () => {
      const log = await openLog(name());
      await fill(log);
      expect(() => log.truncateFrom(0)).toThrow('Argument out of range');
      expect(() => log.truncateFrom(7)).toThrow('Argument out of range');
      await log.close();
    });

    it('refuses to truncate with buffered (unsynced) appends', async () => {
      const log = await openLog(name());
      await fill(log);
      log.append(2, 'buffered');
      expect(() => log.truncateFrom(4)).toThrow('builder state error');
      log.sync();
      log.truncateFrom(4);
      expect(log.lastIndex).toBe(3);
      await log.close();
    });

    it('truncating to empty leaves a working log', async () => {
      const log = await openLog(name());
      await fill(log);
      log.truncateFrom(1);
      expect(log.lastIndex).toBe(0);
      expect(log.lastTerm).toBe(0);
      expect(log.getBatch(1)).toEqual([]);
      expect(log.append(2, 'fresh')).toBe(1);
      log.sync();
      expect(log.verify()).toBe(true);
      await log.close();
    });
  });
});
