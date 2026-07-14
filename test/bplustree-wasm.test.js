/**
 * Runs the shared BPlusTree behavioral suite against the WASM tree.
 *
 * The WASM module loads asynchronously; open() awaits it, but we also await
 * ready() up front so the module is instantiated before the suite registers.
 */
import { ready, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runBPlusTreeSuite } from './bplustree.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runBPlusTreeSuite('WASM', BPlusTree, hasOPFS);
