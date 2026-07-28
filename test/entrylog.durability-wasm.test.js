/**
 * Durability & crash-recovery tests for the WASM EntryLog.
 *
 * The durability contract is the log's whole reason to exist: an entry is on
 * disk once sync() returns (and only then), hard state is on disk before
 * setHardState() returns, and open() recovers a torn tail back to the last
 * good commit via the CRC-trailer scan shared with the other structures
 * (bjfile.h). Logical truncation must also replay correctly from the file:
 * a truncation is a metadata-only commit, and re-appended replacement
 * entries supersede the dead ones during the open-time scan.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, EntryLog, BPlusTree, TextLog } from '../wasm/binjson-structures-wasm.js';
import { BinJsonFile, Pointer, encode, deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

const dec = new TextDecoder();

// On-wire sizes from entrylog.c / bjfile.h.
const METADATA_SIZE = 164;
const TRAILER_SIZE = 17;

describe.skipIf(!hasOPFS)('WASM EntryLog durability & crash recovery', () => {
  let root = null;
  let counter = 0;
  const files = [];

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const name = () => {
    const n = `test-entrylog-dur-${Date.now()}-${counter++}.bj`;
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

  async function openLog(filename, create = true) {
    const log = new EntryLog(await sync(filename, create));
    await log.open();
    return log;
  }

  async function fileSize(filename) {
    const h = await sync(filename);
    const n = h.getSize();
    await h.close();
    return n;
  }

  /** Cut `n` bytes off the end of the file (simulates a torn append). */
  async function chopTail(filename, n) {
    const h = await sync(filename);
    h.truncate(h.getSize() - n);
    h.flush();
    await h.close();
  }

  /** Append `n` junk bytes (simulates a crash that extended the file). */
  async function appendGarbage(filename, n) {
    const h = await sync(filename);
    const junk = new Uint8Array(n).fill(0xff);
    h.write(junk, { at: h.getSize() });
    h.flush();
    await h.close();
  }

  /** Flip one byte at `offset` (simulates bit rot / a bad sector). */
  async function flipByte(filename, offset) {
    const h = await sync(filename);
    const b = new Uint8Array(1);
    h.read(b, { at: offset });
    b[0] ^= 0xff;
    h.write(b, { at: offset });
    h.flush();
    await h.close();
  }

  describe('the sync() durability point', () => {
    it('synced entries survive close and reopen', async () => {
      const file = name();
      const log = await openLog(file);
      log.setHardState(1);
      log.append(1, 'first');
      log.append(1, 'second');
      log.sync();
      await log.close();

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(2);
      expect(back.lastTerm).toBe(1);
      expect(dec.decode(back.get(1).payload)).toBe('first');
      expect(dec.decode(back.get(2).payload)).toBe('second');
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('buffered (unsynced) appends are lost on close — never half-written', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'durable');
      log.sync();
      log.append(0, 'never acked');
      log.append(0, 'never acked either');
      await log.close();   // no sync: the entries were never acknowledged

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(1);
      expect(dec.decode(back.get(1).payload)).toBe('durable');
      expect(back.append(0, 'continues')).toBe(2);
      back.sync();
      await back.close();
    });

    it('hard state survives reopen without any entries or sync', async () => {
      const file = name();
      const log = await openLog(file);
      log.setHardState(7, 3);   // commits immediately by contract
      await log.close();

      const back = await openLog(file, false);
      expect(back.currentTerm).toBe(7);
      expect(back.votedFor).toBe(3);
      expect(back.lastIndex).toBe(0);
      await back.close();
    });

    it('a staged commit index is durable only after the next sync', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'a');
      log.append(0, 'b');
      log.sync();
      log.setCommitIndex(2);    // staged, not committed
      await log.close();

      const mid = await openLog(file, false);
      expect(mid.commitIndex).toBe(0);
      mid.setCommitIndex(2);
      mid.sync();
      await mid.close();

      const back = await openLog(file, false);
      expect(back.commitIndex).toBe(2);
      await back.close();
    });
  });

  describe('logical truncation replays from the file', () => {
    it('a truncation with re-appended replacements survives reopen', async () => {
      const file = name();
      const log = await openLog(file);
      log.setHardState(2);
      for (let i = 1; i <= 5; i++) log.append(1, `original-${i}`);
      log.sync();
      log.truncateFrom(3);
      log.append(2, 'replacement-3');
      log.append(2, 'replacement-4');
      log.sync();
      await log.close();

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(4);
      expect(back.lastTerm).toBe(2);
      expect(dec.decode(back.get(2).payload)).toBe('original-2');
      expect(dec.decode(back.get(3).payload)).toBe('replacement-3');
      expect(dec.decode(back.get(4).payload)).toBe('replacement-4');
      expect(back.termAt(2)).toBe(1);
      expect(back.termAt(3)).toBe(2);
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('a truncation with no re-appends survives reopen (dead entries dropped)', async () => {
      const file = name();
      const log = await openLog(file);
      for (let i = 1; i <= 5; i++) log.append(0, `entry-${i}`);
      log.sync();
      log.truncateFrom(3);
      await log.close();

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(2);
      expect(() => back.get(3)).toThrow('Argument out of range');
      expect(back.append(0, 'fresh-3')).toBe(3);
      back.sync();
      expect(dec.decode(back.get(3).payload)).toBe('fresh-3');
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('repeated truncate/re-append cycles replay correctly', async () => {
      const file = name();
      const log = await openLog(file);
      log.setHardState(3);
      log.append(1, 'a1');
      log.append(1, 'a2');
      log.append(1, 'a3');
      log.sync();
      log.truncateFrom(2);
      log.append(2, 'b2');
      log.append(2, 'b3');
      log.sync();
      log.truncateFrom(3);
      log.append(3, 'c3');
      log.sync();
      await log.close();

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(3);
      expect(dec.decode(back.get(1).payload)).toBe('a1');
      expect(dec.decode(back.get(2).payload)).toBe('b2');
      expect(dec.decode(back.get(3).payload)).toBe('c3');
      expect(back.termAt(3)).toBe(3);
      expect(back.verify()).toBe(true);
      await back.close();
    });
  });

  describe('torn-tail recovery', () => {
    it('recovers to the previous commit when the last append is torn', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'safe-1');
      log.append(0, 'safe-2');
      log.sync();
      log.append(0, 'torn');
      log.sync();
      await log.close();

      await chopTail(file, 10);

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(2);
      expect(dec.decode(back.get(2).payload)).toBe('safe-2');
      expect(() => back.get(3)).toThrow('Argument out of range');
      // The orphan bytes were truncated; the log keeps working.
      expect(back.append(0, 'again')).toBe(3);
      back.sync();
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('recovers when garbage was appended after the last commit', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'kept');
      log.sync();
      await log.close();
      const goodSize = await fileSize(file);
      await appendGarbage(file, 400);

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(1);
      expect(dec.decode(back.get(1).payload)).toBe('kept');
      await back.close();
      expect(await fileSize(file)).toBe(goodSize);
    });

    it('detects a bit flip in the last commit via its CRC', async () => {
      const file = name();
      const log = await openLog(file);
      for (let i = 1; i <= 4; i++) {
        log.append(0, `version-${i}-payload-with-some-length`);
        log.sync();
      }
      await log.close();
      const size = await fileSize(file);

      // Flip a byte inside the last commit's entry record — structurally
      // still plausible, so only the CRC catches it.
      await flipByte(file, size - METADATA_SIZE - TRAILER_SIZE - 20);

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(3);
      expect(dec.decode(back.get(3).payload)).toBe('version-3-payload-with-some-length');
      await back.close();
    });

    it('a torn truncation commit rolls back to the pre-truncation log', async () => {
      const file = name();
      const log = await openLog(file);
      for (let i = 1; i <= 3; i++) log.append(0, `entry-${i}`);
      log.sync();
      log.truncateFrom(2);   // metadata-only commit: trailer + metadata
      await log.close();

      // Tear into the truncation commit: the crash happened mid-truncation,
      // so recovery lands on the state before it — all three entries live.
      await chopTail(file, 10);

      const back = await openLog(file, false);
      expect(back.lastIndex).toBe(3);
      expect(dec.decode(back.get(3).payload)).toBe('entry-3');
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('refuses to open when intact commits exist beyond a damaged region', async () => {
      const file = name();
      const log = await openLog(file);
      for (let i = 1; i <= 8; i++) {
        log.append(0, `entry-${i}-with-enough-bytes-to-matter`);
        log.sync();
      }
      await log.close();

      // Damage an early commit AND break the tail so the recovery scan runs.
      // Verifiable commits exist beyond the damage: truncating there would
      // destroy good data, so open must refuse instead.
      await flipByte(file, 60);
      await chopTail(file, 5);

      const broken = new EntryLog(await sync(file));
      await expect(broken.open()).rejects.toThrow('Invalid entry log file');
    });
  });

  describe('file identification', () => {
    it('writes the entrylog header record at offset 0', async () => {
      const file = name();
      const log = await openLog(file);
      log.append(0, 'x');
      log.sync();
      await log.close();

      const h = await sync(file);
      const header = new BinJsonFile(h).read(new Pointer(0));
      await h.close();
      expect(header).toEqual({ binjson: 'entrylog', fmt: 1 });
    });

    it('refuses to open files of another type (both directions)', async () => {
      const file = name();
      const tlog = new TextLog(await sync(file, true), 5);
      await tlog.open();
      await tlog.addVersion('not an entry log');
      await tlog.close();

      const asEntryLog = new EntryLog(await sync(file));
      await expect(asEntryLog.open()).rejects.toThrow('Invalid entry log file');

      const file2 = name();
      const elog = await openLog(file2);
      elog.append(0, 'x');
      elog.sync();
      await elog.close();

      const asTree = new BPlusTree(await sync(file2), 4);
      await expect(asTree.open()).rejects.toThrow('Invalid tree file');
      const asTextLog = new TextLog(await sync(file2), 5);
      await expect(asTextLog.open()).rejects.toThrow('no valid metadata');
    });
  });

  describe('hostile files', () => {
    /** Write raw records as a legacy-format file (no header, no trailers —
     * legacy commits are accepted unverified, so only the entrylog-level
     * validation stands between these bytes and the open() result). */
    async function craft(filename, records) {
      const h = await sync(filename, true);
      let at = 0;
      for (const rec of records) {
        h.write(rec, { at });
        at += rec.byteLength;
      }
      h.flush();
      await h.close();
    }

    const entry = (index, term, payload) =>
      encode({ index, term, type: 1, payload: enc(payload) });
    const metadata = (fields) =>
      encode({ baseIndex: 0, baseTerm: 0, lastIndex: 0, lastTerm: 0,
               currentTerm: 0, votedFor: 0, commitIndex: 0, ...fields });
    const enc = (s) => new TextEncoder().encode(s);

    it('refuses a gap in the index sequence', async () => {
      const file = name();
      await craft(file, [
        entry(1, 0, 'one'),
        entry(3, 0, 'three'),   // 2 is missing
        metadata({ lastIndex: 3 })
      ]);
      const log = new EntryLog(await sync(file));
      await expect(log.open()).rejects.toThrow('Invalid entry log file');
    });

    it('refuses metadata claiming entries the file does not hold', async () => {
      const file = name();
      await craft(file, [
        entry(1, 0, 'one'),
        entry(2, 0, 'two'),
        metadata({ lastIndex: 5 })
      ]);
      const log = new EntryLog(await sync(file));
      await expect(log.open()).rejects.toThrow('Invalid entry log file');
    });

    it('refuses an entry at or below the tile base', async () => {
      const file = name();
      await craft(file, [
        entry(5, 1, 'below base'),
        metadata({ baseIndex: 5, baseTerm: 1, lastIndex: 5, lastTerm: 1, currentTerm: 1 })
      ]);
      const log = new EntryLog(await sync(file));
      await expect(log.open()).rejects.toThrow('Invalid entry log file');
    });

    it('refuses metadata whose lastTerm contradicts the tail entry', async () => {
      const file = name();
      await craft(file, [
        entry(1, 2, 'term two'),
        metadata({ lastIndex: 1, lastTerm: 1, currentTerm: 2 })
      ]);
      const log = new EntryLog(await sync(file));
      await expect(log.open()).rejects.toThrow('Invalid entry log file');
    });

    it('accepts a well-formed legacy (trailer-less) file', async () => {
      const file = name();
      await craft(file, [
        entry(1, 0, 'one'),
        entry(2, 1, 'two'),
        metadata({ lastIndex: 2, lastTerm: 1, currentTerm: 1 })
      ]);
      const log = new EntryLog(await sync(file));
      await log.open();
      expect(log.lastIndex).toBe(2);
      expect(dec.decode(log.get(2).payload)).toBe('two');
      expect(log.verify()).toBe(true);
      await log.close();
    });
  });
});
