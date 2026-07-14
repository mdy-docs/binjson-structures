/** Runs the shared R-tree node-size suite against the WASM tree. */
import { ready, RTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runRTreeNodeSizesSuite } from './rtree.node-sizes.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runRTreeNodeSizesSuite('WASM', RTree, hasOPFS);
