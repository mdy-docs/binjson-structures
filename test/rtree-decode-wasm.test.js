/**
 * Runs the shared rtree-decode CLI suite against files written by the WASM tree.
 * The CLI reads with the JS reference RTree, so this proves the WASM on-disk
 * format is byte-compatible with the reference.
 */
import { ready, RTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runRTreeDecodeSuite } from './rtree-decode.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runRTreeDecodeSuite('WASM', RTree, hasOPFS);
