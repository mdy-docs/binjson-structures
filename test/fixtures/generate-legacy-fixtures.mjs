/**
 * One-off generator for the legacy (pure-JS-written) binary fixtures under
 * test/fixtures/legacy/.
 *
 * The pure-JS data-structure implementations (src/bplustree.js, src/rtree.js,
 * src/textindex.js, src/textlog.js) were removed after the C/WASM engine
 * became canonical — but files they wrote exist in the wild, and the WASM
 * engine's ability to read/upgrade them is tested against these frozen
 * fixtures. Every setup below is deterministic and mirrors the exact setup
 * the corresponding *-wasm test used to run inline against the JS classes.
 *
 * PROVENANCE: this script can no longer run against the current tree. To
 * regenerate a fixture, check out a revision that still contains the JS
 * implementations (the commit that added these fixtures is the last one),
 * then: node test/fixtures/generate-legacy-fixtures.mjs
 */
import { writeFileSync, mkdirSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const OUT = join(dirname(fileURLToPath(import.meta.url)), 'legacy');
mkdirSync(OUT, { recursive: true });

// --- node-opfs bootstrap (same pattern as the test files) -----------------
const nodeOpfs = await import('node-opfs');
if (nodeOpfs.navigator && typeof global !== 'undefined') {
  Object.defineProperty(global, 'navigator', {
    value: nodeOpfs.navigator, writable: true, configurable: true
  });
}
const root = await navigator.storage.getDirectory();

const { BPlusTree } = await import('../../src/bplustree.js');
const { RTree } = await import('../../src/rtree.js');
const { TextIndex } = await import('../../src/textindex.js');
const { TextLog } = await import('../../src/textlog.js');
const { ObjectId, getFileHandle } = await import('../../src/binjson.js');

async function sync(filename, create = false) {
  const fh = await getFileHandle(root, filename, { create });
  return fh.createSyncAccessHandle();
}

async function dump(opfsName, fixtureName) {
  const fh = await getFileHandle(root, opfsName, { create: false });
  const file = await fh.getFile();
  const bytes = new Uint8Array(await file.arrayBuffer());
  writeFileSync(join(OUT, fixtureName), bytes);
  console.log(`${fixtureName}: ${bytes.length} bytes`);
}

// Shared deterministic helpers (copied from the rtree wasm tests).
const oid = (i) => new ObjectId(String(i).padStart(24, '0'));
const pt = (i) => ({
  lat: ((i * 37) % 170) - 85 + i * 1e-5,
  lng: ((i * 73) % 350) - 175 + i * 1e-5
});

// --- B+ tree fixtures ------------------------------------------------------

// bpt-o4-hollow: order 4, add 0..119 (v${i}), delete 20..99. The JS tree never
// rebalanced, so chains of empty leaves remain. Used by:
//   bplustree.rebalance-wasm ("absorbs hollowed-out legacy JS files")
//   bplustree.verify-wasm    ("accepts under-filled legacy JS-written files")
{
  const js = new BPlusTree(await sync('fx-bpt-hollow.bj', true), 4);
  await js.open();
  for (let i = 0; i < 120; i++) await js.add(i, `v${i}`);
  for (let i = 20; i < 100; i++) await js.delete(i);
  await js.close();
  await dump('fx-bpt-hollow.bj', 'bpt-o4-hollow.bin');
}

// bpt-o4-seq8: order 4, add 0..7. Used by bplustree.snapshot-wasm.
{
  const js = new BPlusTree(await sync('fx-bpt-seq8.bj', true), 4);
  await js.open();
  for (let i = 0; i < 8; i++) await js.add(i, `v${i}`);
  await js.close();
  await dump('fx-bpt-seq8.bj', 'bpt-o4-seq8.bin');
}

// bpt-o4-legacy1: order 4, single record, no header/trailers.
// Used by durability-wasm ("WASM opens a JS-written tree").
{
  const js = new BPlusTree(await sync('fx-bpt-legacy1.bj', true), 4);
  await js.open();
  await js.add('legacy', { from: 'js' });
  await js.close();
  await dump('fx-bpt-legacy1.bj', 'bpt-o4-legacy1.bin');
}

// --- R-tree fixtures (no childBBoxes in the legacy format) -----------------

async function rtreeFixture(order, n, fixtureName) {
  const opfsName = `fx-${fixtureName}.bj`;
  const js = new RTree(await sync(opfsName, true), order);
  await js.open();
  for (let i = 0; i < n; i++) await js.insert(pt(i).lat, pt(i).lng, oid(i));
  await js.close();
  await dump(opfsName, `${fixtureName}.bin`);
}
await rtreeFixture(9, 200, 'rtree-o9-200'); // rtree.cursor-wasm
await rtreeFixture(4, 150, 'rtree-o4-150'); // rtree.remove-wasm
await rtreeFixture(9, 300, 'rtree-o9-300'); // rtree.childbbox-wasm (upgrade)
await rtreeFixture(9, 150, 'rtree-o9-150'); // rtree.childbbox-wasm (compaction)

// --- TextIndex fixtures (terms/documents/lengths tree triple) --------------

async function textIndexFixture(docs, fixtureBase) {
  const opfsBase = `fx-${fixtureBase}`;
  async function tree(suffix) {
    return new BPlusTree(await sync(`${opfsBase}-${suffix}.bj`, true), 16);
  }
  const idx = new TextIndex({
    order: 16,
    trees: {
      index: await tree('terms'),
      documentTerms: await tree('documents'),
      documentLengths: await tree('lengths')
    }
  });
  await idx.open();
  for (const [id, text] of docs) await idx.add(id, text);
  await idx.close();
  for (const suffix of ['terms', 'documents', 'lengths']) {
    await dump(`${opfsBase}-${suffix}.bj`, `${fixtureBase}-${suffix}.bin`);
  }
}

// ti-blocks-60: 60 docs of the blocks-suite corpus. Used by
// textindex.blocks-wasm ("reads JS-written (legacy) index files").
await textIndexFixture(
  Array.from({ length: 60 }, (_, i) => [`doc-${i}`, `shared common corpus unique${i}x group${i % 10}`]),
  'ti-blocks-60');

// ti-bm25-25: 25 docs of the bm25-suite corpus. Used by
// textindex.bm25-wasm ("scores legacy indexes via the scan fallback").
await textIndexFixture(
  Array.from({ length: 25 }, (_, i) => [`doc-${i}`, `orchard apple pear${i % 5} fruit${i}`]),
  'ti-bm25-25');

// --- TextLog fixture --------------------------------------------------------

// textlog-v6-dps3: the interop suite's VERSIONS[0..5] with diffsPerSnapshot=3.
// Timestamps in the file are wall-clock (ignored by the assertions).
{
  const VERSIONS = [
    'Line 1\nLine 2\nLine 3\n',
    'Line 1\nLine 2 changed\nLine 3\n',
    'Line 1\nLine 2 changed\nLine 3\nLine 4\n',
    'Header\nLine 1\nLine 2 changed\nLine 3\nLine 4\n',
    'Header\nLine 1\nLine 2 changed\nLine 3\nLine 4\nLine 5\n',
    'no trailing newline here'
  ];
  const log = new TextLog(await sync('fx-textlog.bj', true), 3);
  await log.open();
  for (const v of VERSIONS) await log.addVersion(v);
  await log.close();
  await dump('fx-textlog.bj', 'textlog-v6-dps3.bin');
}

console.log('done');
