/** Runs the shared R-tree compaction suite against the WASM tree. */
import { ready, RTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runRTreeCompactionSuite } from './rtree.compaction.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runRTreeCompactionSuite('WASM', RTree, hasOPFS);
