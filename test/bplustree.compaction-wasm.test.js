/** Runs the shared BPlusTree compaction suite against the WASM tree. */
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runBPlusTreeCompactionSuite } from './bplustree.compaction.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runBPlusTreeCompactionSuite('WASM', BPlusTree, hasOPFS);
