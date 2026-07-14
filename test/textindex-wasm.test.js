/**
 * Runs the shared TextIndex suite against the WASM implementation.
 *
 * The WASM module loads asynchronously; the trees' open() awaits it, but we also
 * await ready() up front so the module is instantiated before the suite runs.
 */
import { ready, TextIndex, BPlusTree } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runTextIndexSuite } from './textindex.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runTextIndexSuite('WASM', { TextIndex, BPlusTree }, hasOPFS, { replaceOnAdd: true });
