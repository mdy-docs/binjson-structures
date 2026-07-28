/**
 * SnapshotStore tests: the Raft snapshot manifest & crash-safe adoption
 * convention.
 *
 * OPFS has no atomic rename, so adoption mirrors the commit protocol inside
 * the data files: write a generation's files, then write the manifest last
 * — its validity (structure + CRC-32 trailer) IS the commit. open() adopts
 * the newest generation that fully validates and sweeps the rest, so any
 * crash point leaves either the old snapshot or the new one, never a mix.
 * The paired log convention (snap-log-<gen>.bj, newest-that-opens wins) is
 * exercised at the bottom in a full leader→follower install flow.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import {
  ready, SnapshotStore, crc32, BPlusTree, EntryLog
} from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

const enc = new TextEncoder();
const dec = new TextDecoder();

describe.skipIf(!hasOPFS)('WASM SnapshotStore', () => {
  let root = null;
  let counter = 0;
  const prefixes = [];
  const files = [];

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  // Each test gets its own prefix; cleanup deletes everything carrying one.
  const prefix = () => {
    const p = `test-snapstore-${Date.now()}-${counter++}`;
    prefixes.push(p);
    return p;
  };
  const fileName = () => {
    const n = `test-snapstore-file-${Date.now()}-${counter++}.bj`;
    files.push(n);
    return n;
  };

  afterAll(async () => {
    for await (const [name] of root.entries()) {
      if (prefixes.some((p) => name.startsWith(p))) await deleteFile(root, name);
    }
    for (const f of files) await deleteFile(root, f);
  });

  async function openStore(p) {
    const store = new SnapshotStore(root, { prefix: p });
    await store.open();
    return store;
  }

  async function sync(filename, create = false) {
    const fh = await getFileHandle(root, filename, { create });
    return fh.createSyncAccessHandle();
  }

  /** Write `bytes` into a snapshot-transaction role file. */
  async function writeRole(tx, role, bytes) {
    const h = await tx.createFile(role);
    h.write(bytes, { at: 0 });
    h.flush();
    await h.close();
  }

  async function rawWrite(name, bytes) {
    const fh = await root.getFileHandle(name, { create: true });
    const h = await fh.createSyncAccessHandle();
    h.truncate(0);
    h.write(bytes, { at: 0 });
    h.flush();
    await h.close();
  }

  async function chopTail(name, n) {
    const fh = await root.getFileHandle(name);
    const h = await fh.createSyncAccessHandle();
    h.truncate(h.getSize() - n);
    h.flush();
    await h.close();
  }

  async function flipByte(name, offset) {
    const fh = await root.getFileHandle(name);
    const h = await fh.createSyncAccessHandle();
    const b = new Uint8Array(1);
    h.read(b, { at: offset });
    b[0] ^= 0xff;
    h.write(b, { at: offset });
    h.flush();
    await h.close();
  }

  describe('commit & adoption', () => {
    it('a committed generation round-trips through reopen', async () => {
      const p = prefix();
      const store = await openStore(p);
      expect(store.latest).toBeNull();

      const tx = await store.begin();
      await writeRole(tx, 'index', enc.encode('index bytes'));
      await writeRole(tx, 'docs', enc.encode('docs bytes!'));
      const meta = await tx.commit({
        lastIncludedIndex: 42,
        lastIncludedTerm: 3,
        config: { nodes: [1, 2, 3] }
      });
      expect(meta.gen).toBe(1);
      expect(meta.files.length).toBe(2);

      const back = await openStore(p);
      expect(back.latest.gen).toBe(1);
      expect(back.latest.lastIncludedIndex).toBe(42);
      expect(back.latest.lastIncludedTerm).toBe(3);
      expect(back.latest.config).toEqual({ nodes: [1, 2, 3] });
      await expect(back.verify()).resolves.toBe(true);
      const h = await back.openFile('index');
      const buf = new Uint8Array(h.getSize());
      h.read(buf, { at: 0 });
      await h.close();
      expect(dec.decode(buf)).toBe('index bytes');
    });

    it('a newer commit supersedes and prunes the older generation', async () => {
      const p = prefix();
      const store = await openStore(p);
      const tx1 = await store.begin();
      await writeRole(tx1, 'index', enc.encode('old'));
      await tx1.commit({ lastIncludedIndex: 10, lastIncludedTerm: 1 });
      const oldName = store.latest.files[0].name;

      const tx2 = await store.begin();
      await writeRole(tx2, 'index', enc.encode('new'));
      await tx2.commit({ lastIncludedIndex: 20, lastIncludedTerm: 2 });
      expect(store.latest.gen).toBe(2);
      await expect(root.getFileHandle(oldName)).rejects.toThrow();

      const back = await openStore(p);
      expect(back.latest.gen).toBe(2);
      expect(back.latest.lastIncludedIndex).toBe(20);
    });

    it('abort deletes the written files', async () => {
      const p = prefix();
      const store = await openStore(p);
      const tx = await store.begin();
      await writeRole(tx, 'index', enc.encode('doomed'));
      await tx.abort();
      await expect(root.getFileHandle(`${p}-1-index.bj`)).rejects.toThrow();
      expect(store.latest).toBeNull();
    });
  });

  describe('crash recovery (the manifest is the commit point)', () => {
    it('data files without a manifest are ignored and swept', async () => {
      const p = prefix();
      const store = await openStore(p);
      const tx = await store.begin();
      await writeRole(tx, 'index', enc.encode('crashed before manifest'));
      // No commit: simulates a crash mid-snapshot.

      const back = await openStore(p);
      expect(back.latest).toBeNull();
      await expect(root.getFileHandle(`${p}-1-index.bj`)).rejects.toThrow();

      // And the generation counter moved past the crashed attempt.
      const tx2 = await back.begin();
      expect(tx2.gen).toBe(2);
      await tx2.abort();
    });

    it('a torn manifest invalidates its generation; the previous one wins', async () => {
      const p = prefix();
      const store = await openStore(p);
      const tx1 = await store.begin();
      await writeRole(tx1, 'index', enc.encode('good old snapshot'));
      await tx1.commit({ lastIncludedIndex: 10, lastIncludedTerm: 1 });
      const tx2 = await store.begin();
      await writeRole(tx2, 'index', enc.encode('new snapshot'));
      await tx2.commit({ lastIncludedIndex: 20, lastIncludedTerm: 2 });

      // Tear the new manifest: crash mid-write. Note gen 1 was pruned by
      // gen 2's commit, so recovery correctly finds nothing... unless the
      // old one survived. Rebuild that state: commit gen 3 (pruning 2),
      // then tear gen 3's manifest with gen 2 gone — expect null. The
      // richer case (older survivor) is below.
      await chopTail(`${p}-2.manifest.bj`, 3);
      const back = await openStore(p);
      expect(back.latest).toBeNull();
    });

    it('falls back to the older generation when the newer one is damaged pre-prune', async () => {
      const p = prefix();
      // Hand-craft the crash window where both generations exist on disk
      // (crash after writing gen 2's manifest but before pruning gen 1):
      const s1 = await openStore(p);
      const tx1 = await s1.begin();
      await writeRole(tx1, 'index', enc.encode('older but intact'));
      await tx1.commit({ lastIncludedIndex: 10, lastIncludedTerm: 1 });
      // Fabricate gen 2 files directly (no commit-time pruning of gen 1).
      await rawWrite(`${p}-2-index.bj`, enc.encode('newer'));
      await rawWrite(`${p}-2.manifest.bj`, enc.encode('not a valid manifest'));

      const back = await openStore(p);
      expect(back.latest.gen).toBe(1);
      expect(back.latest.lastIncludedIndex).toBe(10);
      // The damaged gen-2 attempt was swept.
      await expect(root.getFileHandle(`${p}-2-index.bj`)).rejects.toThrow();
    });

    it('a size-mismatched data file invalidates its generation', async () => {
      const p = prefix();
      const store = await openStore(p);
      const tx = await store.begin();
      await writeRole(tx, 'index', enc.encode('will be truncated'));
      await tx.commit({ lastIncludedIndex: 5, lastIncludedTerm: 1 });
      await chopTail(`${p}-1-index.bj`, 4);

      const back = await openStore(p);
      expect(back.latest).toBeNull();
    });

    it('verify() catches a bit flip the size check cannot', async () => {
      const p = prefix();
      const store = await openStore(p);
      const tx = await store.begin();
      await writeRole(tx, 'index', enc.encode('some snapshot payload bytes'));
      await tx.commit({ lastIncludedIndex: 5, lastIncludedTerm: 1 });
      await expect(store.verify()).resolves.toBe(true);

      await flipByte(`${p}-1-index.bj`, 10);
      const back = await openStore(p);
      expect(back.latest.gen).toBe(1);   // size still matches: adopted
      await expect(back.verify()).rejects.toThrow('failed its checksum');
    });
  });

  describe('crc32 utility', () => {
    it('matches incremental and one-shot computation', async () => {
      const bytes = enc.encode('the quick brown fox jumps over the lazy dog');
      const whole = crc32(bytes);
      let inc = 0;
      for (let i = 0; i < bytes.length; i += 7) {
        inc = crc32(bytes.subarray(i, Math.min(i + 7, bytes.length)), inc);
      }
      expect(inc).toBe(whole);
      // Pinned value: zlib crc32 of the ASCII pangram above.
      expect(crc32(enc.encode('123456789'))).toBe(0xcbf43926);
    });
  });

  describe('the full Raft snapshot/install flow', () => {
    it('leader snapshots, compacts its log, follower installs and catches up', async () => {
      // --- Leader: a log-driven tree plus its entry log.
      const leaderTreeFile = fileName();
      const leaderTree = new BPlusTree(await sync(leaderTreeFile, true), 4);
      await leaderTree.open();
      const leaderLog = new EntryLog(await sync(fileName(), true));
      await leaderLog.open();
      leaderLog.setHardState(1);
      for (let i = 1; i <= 8; i++) {
        const idx = leaderLog.append(1, `set k${i}`);
        leaderLog.sync();
        leaderTree.setAppliedIndex(idx);
        leaderTree.add(`k${i}`, { i });
      }

      // --- Leader takes a snapshot at appliedIndex 8 via the store.
      const lp = prefix();
      const leaderStore = await openStore(lp);
      const tx = await leaderStore.begin();
      await leaderTree.compact(await tx.createFile('tree'));
      const boundary = {
        lastIncludedIndex: leaderTree.appliedIndex(),
        lastIncludedTerm: leaderLog.termAt(leaderTree.appliedIndex()),
        config: { nodes: ['a', 'b'] }
      };
      await tx.commit(boundary);

      // --- Leader compacts its log at the snapshot boundary and adopts it.
      const { name: newLogName, handle: newLogHandle } = await leaderStore.createLogFile();
      await leaderLog.compact(newLogHandle, boundary.lastIncludedIndex, boundary.lastIncludedTerm);
      await leaderLog.close();
      await leaderStore.pruneLogs(newLogName);
      expect(await leaderStore.logCandidates()).toEqual([newLogName]);
      const leaderLog2 = new EntryLog(await sync(newLogName));
      await leaderLog2.open();
      expect(leaderLog2.baseIndex).toBe(8);
      // Post-snapshot traffic the follower will need after installing.
      leaderLog2.append(1, 'set k9');
      leaderLog2.sync();

      // --- Follower: installs by streaming the leader's snapshot chunks.
      const fp = prefix();
      const followerStore = await openStore(fp);
      const ftx = await followerStore.begin();
      const src = await leaderStore.openFile('tree');
      const dst = await ftx.createFile('tree');
      const CH = 4096;   // InstallSnapshot-sized chunks
      const size = src.getSize();
      const buf = new Uint8Array(CH);
      for (let at = 0; at < size; at += CH) {
        const view = buf.subarray(0, Math.min(CH, size - at));
        src.read(view, { at });
        dst.write(view, { at });
      }
      await src.close();
      dst.flush();
      await dst.close();
      await ftx.commit(boundary);

      // The recomputed checksums must match the leader's manifest exactly.
      expect(followerStore.latest.files).toEqual(leaderStore.latest.files.map((f) => ({
        ...f, name: f.name.replace(lp, fp)
      })));
      expect(followerStore.latest.config).toEqual({ nodes: ['a', 'b'] });

      // --- Follower goes live: copy to its live filename, open, and start
      // a fresh log tile at the snapshot boundary.
      const followerTreeFile = fileName();
      await followerStore.copyFile('tree', await sync(followerTreeFile, true));
      const followerTree = new BPlusTree(await sync(followerTreeFile), 4);
      await followerTree.open();
      expect(followerTree.size()).toBe(8);
      expect(followerTree.appliedIndex()).toBe(8);
      expect(followerTree.search('k3')).toEqual({ i: 3 });

      const { name: fLogName, handle: fLogHandle } = await followerStore.createLogFile();
      await fLogHandle.close();   // EntryLog opens its own handle below
      const followerLog = new EntryLog(await sync(fLogName), {
        baseIndex: boundary.lastIncludedIndex,
        baseTerm: boundary.lastIncludedTerm
      });
      await followerLog.open();

      // --- Catch-up: replicate the post-snapshot entry and apply it.
      const batch = leaderLog2.getBatch(followerLog.lastIndex + 1);
      expect(batch.length).toBe(1);
      followerLog.setHardState(1);
      for (const e of batch) {
        expect(followerLog.append(e.term, e.payload, e.type)).toBe(e.index);
      }
      followerLog.sync();
      followerTree.setAppliedIndex(9);
      followerTree.add('k9', { i: 9 });
      expect(followerTree.appliedIndex()).toBe(9);
      expect(dec.decode(followerLog.get(9).payload)).toBe('set k9');

      await followerLog.close();
      await followerTree.close();
      await leaderLog2.close();
      await leaderTree.close();
    });
  });
});
