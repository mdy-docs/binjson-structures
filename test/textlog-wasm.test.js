/**
 * Runs the shared TextLog behavioral suite against the WASM implementation.
 *
 * The WASM module loads asynchronously; open() awaits it, but we also await
 * ready() up front so the module is instantiated before the suite registers.
 */
import { ready, TextLog } from '../wasm/binjson-structures-wasm.js';
import { bootstrapOPFS } from './binjson.suite.js';
import { runTextLogSuite } from './textlog.suite.js';

await ready();
const { hasOPFS } = await bootstrapOPFS();

runTextLogSuite('WASM', TextLog, hasOPFS);
