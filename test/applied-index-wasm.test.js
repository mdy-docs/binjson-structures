/**
 * appliedIndex tests: the replicated-log integration threaded through the
 * state-machine structures (B+ tree, R-tree, text log, text index).
 *
 * Contract (see bpt_set_applied_index in bplustree.h): the apply loop stages
 * the entry's log index before the mutation, and the mutation's commit
 * persists both atomically — so after a crash, each structure's recovered
 * appliedIndex tells the replay exactly where to resume, and re-applying is
 * never needed for indexes at or below it. The field is optional on the
 * wire (written only when nonzero), so structures outside a cluster keep
 * producing byte-identical legacy files — the existing durability tests,
 * which hard-code the legacy 135-byte metadata size, double as proof.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, BPlusTree, RTree, TextLog, TextIndex, ObjectId } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM appliedIndex (replicated-log integration)', () => {
  let root = null;
  let counter = 0;
  const files = [];

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const name = () => {
    const n = `test-applied-${Date.now()}-${counter++}.bj`;
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

  async function chopTail(filename, n) {
    const h = await sync(filename);
    h.truncate(h.getSize() - n);
    h.flush();
    await h.close();
  }

  describe('B+ tree', () => {
    it('defaults to 0 and stays 0 for non-log-driven trees', async () => {
      const file = name();
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      expect(tree.appliedIndex()).toBe(0);
      tree.add('a', { v: 1 });
      await tree.close();

      const back = new BPlusTree(await sync(file), 4);
      await back.open();
      expect(back.appliedIndex()).toBe(0);
      await back.close();
    });

    it('persists atomically with the mutation and survives reopen', async () => {
      const file = name();
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      tree.setAppliedIndex(7);
      tree.add('cmd7', { v: 7 });   // stage-then-mutate: one atomic commit
      await tree.close();

      const back = new BPlusTree(await sync(file), 4);
      await back.open();
      expect(back.appliedIndex()).toBe(7);
      expect(back.search('cmd7')).toEqual({ v: 7 });
      expect(back.verify()).toBe(true);
      await back.close();
    });

    it('staged but uncommitted values are lost (never half-applied)', async () => {
      const file = name();
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      tree.add('a', { v: 1 });
      tree.setAppliedIndex(9);      // staged; no mutation commits it
      await tree.close();

      const back = new BPlusTree(await sync(file), 4);
      await back.open();
      expect(back.appliedIndex()).toBe(0);
      await back.close();
    });

    it('is sticky across later commits and never decreases', async () => {
      const file = name();
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      tree.setAppliedIndex(3);
      tree.add('a', { v: 1 });
      tree.add('b', { v: 2 });      // no restage: 3 rides along
      expect(() => tree.setAppliedIndex(2)).toThrow('builder state error');
      await tree.close();

      const back = new BPlusTree(await sync(file), 4);
      await back.open();
      expect(back.appliedIndex()).toBe(3);
      await back.close();
    });

    it('rolls back with a torn tail commit', async () => {
      const file = name();
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      tree.setAppliedIndex(5);
      tree.add('e5', { v: 5 });
      tree.setAppliedIndex(6);
      tree.add('e6', { v: 6 });
      await tree.close();

      await chopTail(file, 10);     // tear the appliedIndex=6 commit

      const back = new BPlusTree(await sync(file), 4);
      await back.open();
      expect(back.appliedIndex()).toBe(5);   // replay resumes at 6
      expect(back.search('e5')).toBeDefined();
      expect(back.search('e6')).toBeUndefined();
      await back.close();
    });

    it('time-travel and rewind restore the boundary value', async () => {
      const file = name();
      const tree = new BPlusTree(await sync(file, true), 4);
      await tree.open();
      tree.add('plain', { v: 0 });             // legacy-size metadata
      tree.setAppliedIndex(4);
      tree.add('e4', { v: 4 });                // extended-size metadata
      tree.setAppliedIndex(8);
      tree.add('e8', { v: 8 });

      // Boundaries must list commits of both metadata sizes.
      const bounds = tree.boundaries();
      expect(bounds.length).toBe(4);           // create + 3 adds

      const snapPlain = tree.snapshotAt(bounds[1].offset);
      expect(snapPlain.appliedIndex()).toBe(0);
      expect(() => snapPlain.setAppliedIndex(9)).toThrow('builder state error');
      await snapPlain.close();

      const snapMid = tree.snapshotAt(bounds[2].offset);
      expect(snapMid.appliedIndex()).toBe(4);
      await snapMid.close();

      // A live snapshot carries the current value.
      const snapLive = tree.snapshot();
      expect(snapLive.appliedIndex()).toBe(8);
      await snapLive.close();
      await tree.close();
    });

    it('carries through compaction', async () => {
      const src = name(), dst = name();
      const tree = new BPlusTree(await sync(src, true), 4);
      await tree.open();
      tree.setAppliedIndex(11);
      for (let i = 0; i < 10; i++) tree.add(`k${i}`, { i });
      await tree.compact(await sync(dst, true));
      await tree.close();

      const compacted = new BPlusTree(await sync(dst), 4);
      await compacted.open();
      expect(compacted.appliedIndex()).toBe(11);
      expect(compacted.size()).toBe(10);
      expect(compacted.verify()).toBe(true);
      await compacted.close();
    });

    it('legacy JS-written files open with appliedIndex 0', async () => {
      const file = name();
      await writeFixture(await sync(file, true), 'bpt-o4-legacy1.bin');
      const tree = new BPlusTree(await sync(file), 4);
      await tree.open();
      expect(tree.appliedIndex()).toBe(0);
      expect(tree.search('legacy')).toEqual({ from: 'js' });
      await tree.close();
    });
  });

  describe('R-tree', () => {
    const oid = (n) => new ObjectId(n.toString(16).padStart(24, '0'));

    it('persists atomically with the mutation and survives reopen', async () => {
      const file = name();
      const rt = new RTree(await sync(file, true), 4);
      await rt.open();
      expect(rt.appliedIndex()).toBe(0);
      rt.setAppliedIndex(12);
      rt.insert(10, 20, oid(1));
      await rt.close();

      const back = new RTree(await sync(file), 4);
      await back.open();
      expect(back.appliedIndex()).toBe(12);
      expect(back.size()).toBe(1);
      expect(() => back.setAppliedIndex(11)).toThrow('builder state error');
      await back.close();
    });

    it('carries through compaction', async () => {
      const src = name(), dst = name();
      const rt = new RTree(await sync(src, true), 4);
      await rt.open();
      rt.setAppliedIndex(20);
      for (let i = 1; i <= 6; i++) rt.insert(i * 10 - 40, i * 20 - 100, oid(i));
      await rt.compact(await sync(dst, true));
      await rt.close();

      const compacted = new RTree(await sync(dst), 4);
      await compacted.open();
      expect(compacted.appliedIndex()).toBe(20);
      expect(compacted.size()).toBe(6);
      await compacted.close();
    });
  });

  describe('text log', () => {
    it('persists atomically with addVersion and survives reopen', async () => {
      const file = name();
      const log = new TextLog(await sync(file, true), 3);
      await log.open();
      expect(log.appliedIndex()).toBe(0);
      log.setAppliedIndex(31);
      await log.addVersion('applied at 31\n');
      await log.close();

      const back = new TextLog(await sync(file), 3);
      await back.open();
      expect(back.appliedIndex()).toBe(31);
      expect(await back.getVersion(1)).toBe('applied at 31\n');
      expect(() => back.setAppliedIndex(30)).toThrow('builder state error');
      await back.close();
    });

    it('coexists with tiling (baseVersion + appliedIndex in one metadata)', async () => {
      const file = name();
      const log = new TextLog(await sync(file, true), 3, 100);
      await log.open();
      log.setAppliedIndex(42);
      await log.addVersion('tile continues\n');
      await log.close();

      const back = new TextLog(await sync(file), 3);
      await back.open();
      expect(back.baseVersion).toBe(100);
      expect(back.appliedIndex()).toBe(42);
      expect(await back.getVersion(101)).toBe('tile continues\n');
      await back.close();
    });
  });

  describe('text index', () => {
    async function makeIndex() {
      const trees = {
        index: new BPlusTree(await sync(name(), true), 16),
        documentTerms: new BPlusTree(await sync(name(), true), 16),
        documentLengths: new BPlusTree(await sync(name(), true), 16)
      };
      const ix = new TextIndex({ trees });
      await ix.open();
      return ix;
    }

    it('stages across all three trees and reports the minimum', async () => {
      const ix = await makeIndex();
      expect(ix.appliedIndex()).toBe(0);
      ix.setAppliedIndex(15);
      await ix.add('doc-1', 'hello replicated world');
      expect(ix.appliedIndex()).toBe(15);
      expect(await ix.query('replicated', { scored: false })).toEqual(['doc-1']);
      await ix.close();
    });
  });
});
