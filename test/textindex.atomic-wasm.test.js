/**
 * Cross-tree atomicity tests (C_DATABASE_REVIEW.md §1.9).
 *
 * A TextIndex spans three B+ tree files; one add/remove/clear performs many
 * individual tree commits, so a crash in between used to leave the index
 * internally inconsistent. With a journal file supplied, the three file
 * lengths are recorded after each operation and open() rewinds every tree to
 * the newest recorded consistent triple — partially applied operations
 * disappear whole, because rewinding an append-only file to a commit
 * boundary restores exactly the state at that commit.
 *
 * Crashes are simulated by snapshotting the four files mid-sequence and
 * restoring subsets, reproducing every interleaving a real crash can leave.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextIndex cross-tree atomicity', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const base = () => `test-tixatomic-${Date.now()}-${counter++}`;

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  const SUFFIXES = ['terms', 'documents', 'lengths', 'journal'];

  async function openIndex(name) {
    async function tree(suffix) {
      const filename = `${name}-${suffix}.bj`;
      if (!files.includes(filename)) files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      return new BPlusTree(await fh.createSyncAccessHandle(), 16);
    }
    const jname = `${name}-journal.bj`;
    if (!files.includes(jname)) files.push(jname);
    const jfh = await getFileHandle(root, jname, { create: true });
    const idx = new TextIndex({
      order: 16,
      trees: {
        index: await tree('terms'),
        documentTerms: await tree('documents'),
        documentLengths: await tree('lengths')
      },
      journal: await jfh.createSyncAccessHandle()
    });
    await idx.open();
    return idx;
  }

  async function readBytes(filename) {
    const fh = await getFileHandle(root, filename, { create: false });
    const h = await fh.createSyncAccessHandle();
    const buf = new Uint8Array(h.getSize());
    h.read(buf, { at: 0 });
    await h.close();
    return buf;
  }
  async function writeBytes(filename, buf) {
    const fh = await getFileHandle(root, filename, { create: false });
    const h = await fh.createSyncAccessHandle();
    h.truncate(0);
    h.write(buf, { at: 0 });
    h.flush();
    await h.close();
    return buf;
  }
  async function snapshot(name) {
    const out = {};
    for (const s of SUFFIXES) out[s] = await readBytes(`${name}-${s}.bj`);
    return out;
  }

  const docText = (i) => `shared corpus unique${i}x number${i}`;

  async function seed(name, k) {
    const idx = await openIndex(name);
    for (let i = 0; i < k; i++) await idx.add(`doc-${i}`, docText(i));
    await idx.close();
  }

  async function expectConsistentAt(name, k) {
    const idx = await openIndex(name);
    expect(await idx.getDocumentCount()).toBe(k);
    expect((await idx.query('shared')).length).toBe(k);
    for (const probe of [0, k - 1]) {
      expect(await idx.query(`unique${probe}x`, { scored: false })).toEqual([`doc-${probe}`]);
    }
    expect(await idx.query(`unique${k}x`)).toHaveLength(0); // rolled-back doc
    await idx.close();
  }

  it('journaled indexes work normally and the journal stays tiny', async () => {
    const name = base();
    const idx = await openIndex(name);
    for (let i = 0; i < 20; i++) await idx.add(`doc-${i}`, docText(i));
    expect((await idx.query('shared')).length).toBe(20);
    expect(await idx.remove('doc-3')).toBe(true);
    expect((await idx.query('shared')).length).toBe(19);
    await idx.close();

    const j = await readBytes(`${name}-journal.bj`);
    expect(j.byteLength).toBeLessThanOrEqual(96); // two ping-pong slots

    const again = await openIndex(name);
    expect(await again.getDocumentCount()).toBe(19);
    await again.close();
  });

  it('rolls back an add whose journal record never landed', async () => {
    const name = base();
    await seed(name, 8);
    const snap = await snapshot(name);

    const idx = await openIndex(name);
    await idx.add('doc-8', docText(8)); // trees + journal advance
    await idx.close();

    // Crash simulation: every tree write persisted, the journal write did
    // not. The reopened index must not contain any trace of doc-8.
    await writeBytes(`${name}-journal.bj`, snap.journal);
    await expectConsistentAt(name, 8);
  });

  it('rolls back a partially persisted add (one tree behind)', async () => {
    const name = base();
    await seed(name, 8);
    const snap = await snapshot(name);

    const idx = await openIndex(name);
    await idx.add('doc-8', docText(8));
    await idx.close();

    // Crash simulation: postings and documentTerms persisted, but
    // documentLengths and the journal did not — the exact inconsistency
    // §1.9 describes (postings referencing a doc with no length entry).
    await writeBytes(`${name}-lengths.bj`, snap.lengths);
    await writeBytes(`${name}-journal.bj`, snap.journal);
    await expectConsistentAt(name, 8);
  });

  it('falls back to the previous journal slot when the newest is unsatisfiable', async () => {
    const name = base();
    await seed(name, 8);
    const snap = await snapshot(name);

    const idx = await openIndex(name);
    await idx.add('doc-8', docText(8));
    await idx.close();

    // Crash simulation: the journal's newest slot persisted but none of the
    // tree writes did. The previous slot matches the trees exactly.
    for (const s of ['terms', 'documents', 'lengths']) {
      await writeBytes(`${name}-${s}.bj`, snap[s]);
    }
    await expectConsistentAt(name, 8);
  });

  it('refuses to open when the trees are behind every journal record', async () => {
    const name = base();
    await seed(name, 8);

    // Pair the populated journal with brand-new empty tree files: both
    // recorded transactions reference data the trees do not have.
    const fresh = base();
    for (const s of ['terms', 'documents', 'lengths']) {
      const filename = `${fresh}-${s}.bj`;
      files.push(filename);
      const fh = await getFileHandle(root, filename, { create: true });
      const tree = new BPlusTree(await fh.createSyncAccessHandle(), 16);
      await tree.open();
      await tree.close();
    }
    async function tree(suffix) {
      const fh = await getFileHandle(root, `${fresh}-${suffix}.bj`, { create: false });
      return new BPlusTree(await fh.createSyncAccessHandle(), 16);
    }
    const jfh = await getFileHandle(root, `${name}-journal.bj`, { create: false });
    const idx = new TextIndex({
      order: 16,
      trees: {
        index: await tree('terms'),
        documentTerms: await tree('documents'),
        documentLengths: await tree('lengths')
      },
      journal: await jfh.createSyncAccessHandle()
    });
    await expect(idx.open()).rejects.toThrow();
  });

  it('keeps remove and clear atomic too', async () => {
    const name = base();
    await seed(name, 6);
    const snap = await snapshot(name);

    // Partially persisted remove: postings updated, documentTerms /
    // documentLengths / journal not.
    const idx = await openIndex(name);
    expect(await idx.remove('doc-2')).toBe(true);
    await idx.close();
    await writeBytes(`${name}-documents.bj`, snap.documents);
    await writeBytes(`${name}-lengths.bj`, snap.lengths);
    await writeBytes(`${name}-journal.bj`, snap.journal);

    const back = await openIndex(name);
    expect(await back.getDocumentCount()).toBe(6);
    expect(await back.query('unique2x', { scored: false })).toEqual(['doc-2']);

    // And a journaled clear that fully lands stays cleared after reopen.
    await back.clear();
    expect(await back.getDocumentCount()).toBe(0);
    await back.close();
    const cleared = await openIndex(name);
    expect(await cleared.getDocumentCount()).toBe(0);
    expect(await cleared.query('shared')).toHaveLength(0);
    await cleared.close();
  });
});
