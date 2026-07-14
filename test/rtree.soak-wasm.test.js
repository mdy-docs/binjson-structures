/** Runs the shared R-tree soak suite against the WASM tree. */
import { ready, RTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runRTreeSoakSuite } from './rtree.soak.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runRTreeSoakSuite('WASM', RTree, hasOPFS);
