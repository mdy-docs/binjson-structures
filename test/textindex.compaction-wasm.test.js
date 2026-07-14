/** Runs the shared TextIndex compaction suite against the WASM implementation. */
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runTextIndexCompactionSuite } from './textindex.compaction.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runTextIndexCompactionSuite('WASM', { TextIndex, BPlusTree }, hasOPFS);
