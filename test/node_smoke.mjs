// End-to-end sanity check of the actual compiled wasm/binjson-structures-wasm.js
// (not a mock) against a real in-memory handle -- run with:
//   node test/node_smoke.mjs
// after `git submodule update --init && ./build-wasm.sh`.
import { ready, BPlusTree, RTree, TextLog, TextIndex, orderedKey, compositeKey } from '../wasm/binjson-structures-wasm.js';
import { MemoryHandle, ObjectId } from '../third_party/binjson/js/binjson.js';

let failures = 0;
function check(cond, what) {
  if (cond) {
    console.log(`[PASS] ${what}`);
  } else {
    console.log(`[FAIL] ${what}`);
    failures++;
  }
}

await ready();

// --- BPlusTree ---
{
  const tree = new BPlusTree(new MemoryHandle(), 32);
  await tree.open();
  await tree.add('alice', { age: 30 });
  await tree.add('bob', { age: 25 });
  check((await tree.search('alice'))?.age === 30, 'BPlusTree: search finds an inserted key');
  check((await tree.search('nobody')) === undefined, 'BPlusTree: search misses a non-existent key');
  const all = [];
  for await (const e of tree) all.push(e.key);
  check(all.join(',') === 'alice,bob', 'BPlusTree: iteration is sorted by key');
  await tree.close();
}

// --- orderedKey / compositeKey ---
{
  const k1 = compositeKey('team', 'alice');
  const k2 = compositeKey('team', 'bob');
  check(k1.length > 0 && k2.length > 0 && Buffer.compare(k1, k2) < 0, 'compositeKey: preserves ordering across parts');
  check(orderedKey(42) instanceof Uint8Array, 'orderedKey: encodes a number to bytes');
}

// --- RTree ---
{
  const rtree = new RTree(new MemoryHandle());
  await rtree.open();
  const id = new ObjectId();
  await rtree.insert(40.7128, -74.0060, id); // New York
  const hits = await rtree.searchRadius(40.7128, -74.0060, 100);
  check(hits.length === 1 && hits[0].objectId.equals(id), 'RTree: radius search finds an inserted point');
  await rtree.close();
}

// --- TextLog ---
{
  const log = new TextLog(new MemoryHandle());
  await log.open();
  await log.addVersion('hello world');
  await log.addVersion('hello there world');
  check((await log.getVersion(1)) === 'hello world', 'TextLog: retrieves version 1 verbatim');
  check((await log.getVersion(2)) === 'hello there world', 'TextLog: retrieves version 2 verbatim');
  await log.close();
}

// --- TextIndex (built on three BPlusTrees: terms, document terms, document lengths) ---
{
  const trees = {
    index: new BPlusTree(new MemoryHandle(), 16),
    documentTerms: new BPlusTree(new MemoryHandle(), 16),
    documentLengths: new BPlusTree(new MemoryHandle(), 16)
  };
  const index = new TextIndex({ order: 16, trees });
  await index.open();
  const id = new ObjectId();
  await index.add(id, 'the quick brown fox jumps over the lazy dog');
  const results = await index.query('fox');
  check(results.length === 1 && String(results[0].id) === String(id), 'TextIndex: query finds an indexed document by stemmed term');
  await index.close();
}

console.log(failures === 0 ? `\nAll checks passed.` : `\n${failures} check(s) failed.`);
process.exit(failures === 0 ? 0 : 1);
