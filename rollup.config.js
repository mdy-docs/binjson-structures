import { nodeResolve } from '@rollup/plugin-node-resolve';
import commonjs from '@rollup/plugin-commonjs';

export default {
  input: 'public/worker.js',
  // The bundle pulls in the WASM loader (lib/binjson-structures.wasm.mjs).
  // Its Node branch does `import('node:module')` etc., which a browser Web
  // Worker never executes -- keep those built-ins external so they aren't
  // bundled.
  external: [/^node:/],
  output: {
    file: 'site/worker.js',
    format: 'es'
  },
  plugins: [
    nodeResolve({
      browser: false, // We're in a Web Worker, not a browser main thread
      preferBuiltins: false
    }),
    commonjs()
  ]
};
