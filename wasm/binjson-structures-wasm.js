/**
 * binjson-structures-wasm.js — standalone WASM-backed B+ tree, R-tree,
 * text log, and text index, buildable and usable independently of the
 * rest of the parent project (see build-wasm.sh in this package's root).
 *
 * Extracted verbatim from the parent project's src/binjson-wasm.js (its
 * own combined WASM module bundles these together with the document-
 * database layer built on top of them, and keeps its own copy of this
 * same logic rather than depending on this package) -- this is the
 * subset of that file that is actually these four data structures (plus
 * their own C dependencies: geo for RTree, diff for TextLog, stemmer for
 * TextIndex) and nothing from the Db/Collection layer built on top of
 * them.
 *
 * Depends on binjson (this package's own third_party/binjson submodule),
 * used here purely for its JS value types (ObjectId/Pointer/TYPE) -- its
 * own separate WASM instance is not shared with this one; this module
 * keeps its own self-contained copy of encode/decode, same reasoning
 * third_party/binjson/wasm/binjson-wasm.js documents for why the parent
 * project's own combined module doesn't import this package instead of
 * keeping its own copy: every independently-buildable WASM module here
 * is fully self-contained, not just source-compatible.
 *
 * The WASM module loads asynchronously; call and await `ready()` once
 * before using any of these synchronously-shaped APIs.
 */
import createModule from '../lib/binjson-structures.wasm.mjs';
import { TYPE, ObjectId, Pointer } from '../third_party/binjson/js/binjson.js';

// Event tags — must match the BJW_EV_* constants in c/binjson_wasm.c.
const EV = {
  NULL: 0, FALSE: 1, TRUE: 2, INT: 3, FLOAT: 4, STRING: 5, OID: 6,
  DATE: 7, POINTER: 8, BINARY: 9, ARR_BEGIN: 10, ARR_END: 11,
  OBJ_BEGIN: 12, KEY: 13, OBJ_END: 14
};

// Error codes — must match the BJ_ERR_* constants in c/binjson.h.
const ERR = {
  [-1]: 'out of memory',
  [-2]: 'builder state error',
  [-3]: 'Unexpected end of data',
  [-4]: 'Unknown type byte',
  [-5]: 'Decoded integer exceeds safe range',
  [-6]: 'Pointer offset out of valid range',
  [-7]: 'Maximum nesting depth exceeded',
  [-8]: 'Structural invariant violated',
  [-9]: 'Argument out of range'
};

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

let Module = null;
let readyPromise = null;

/**
 * Instantiate the WASM module. Idempotent; returns a promise that resolves when
 * encode/decode are usable. Must be awaited before the first encode/decode.
 */
function ready() {
  if (!readyPromise) {
    readyPromise = createModule().then((m) => { Module = m; return m; });
  }
  return readyPromise;
}

/** True once the module is instantiated and encode/decode may be called. */
function isReady() {
  return Module !== null;
}

function requireModule() {
  if (!Module) {
    throw new Error('binjson-wasm not initialized: await ready() before encode/decode');
  }
  return Module;
}

function codeError(code, context) {
  const msg = ERR[code] || `binjson error ${code}`;
  return new Error(context ? `${msg} (${context})` : msg);
}

function check(code) {
  if (code !== 0) throw codeError(code);
}

/**
 * Copy `bytes` into the WASM heap, invoke `fn(ptr, len)`, then free. The C
 * builder copies immediately, so the scratch allocation is safe to release.
 */
function withBytes(M, bytes, fn) {
  const n = bytes.length;
  const ptr = n ? M._malloc(n) : 0;
  if (n) M.HEAPU8.set(bytes, ptr);
  try {
    return fn(ptr, n);
  } finally {
    if (n) M._free(ptr);
  }
}

function writeValue(M, val) {
  if (val === null) { check(M._bjw_put_null()); return; }
  if (val === false) { check(M._bjw_put_bool(0)); return; }
  if (val === true) { check(M._bjw_put_bool(1)); return; }

  if (val instanceof ObjectId) {
    withBytes(M, val.toBytes(), (p) => check(M._bjw_put_oid(p)));
    return;
  }
  if (val instanceof Date) { check(M._bjw_put_date(val.getTime())); return; }
  if (val instanceof Pointer) { check(M._bjw_put_pointer(val.offset)); return; }
  if (val instanceof Uint8Array) {
    withBytes(M, val, (p, n) => check(M._bjw_put_binary(p, n)));
    return;
  }

  const t = typeof val;
  if (t === 'number') {
    if (Number.isInteger(val) && Number.isSafeInteger(val)) check(M._bjw_put_int(val));
    else check(M._bjw_put_float(val));
    return;
  }
  if (t === 'string') {
    withBytes(M, textEncoder.encode(val), (p, n) => check(M._bjw_put_string(p, n)));
    return;
  }
  if (Array.isArray(val)) {
    check(M._bjw_begin_array());
    for (const item of val) writeValue(M, item);
    check(M._bjw_end_array());
    return;
  }
  if (t === 'object') {
    check(M._bjw_begin_object());
    for (const key of Object.keys(val)) {
      withBytes(M, textEncoder.encode(key), (p, n) => check(M._bjw_put_key(p, n)));
      writeValue(M, val[key]);
    }
    check(M._bjw_end_object());
    return;
  }
  throw new Error(`Unsupported type: ${t}`);
}

/**
 * Encode a JavaScript value to binjson binary format.
 * @returns {Uint8Array}
 */
function encode(value) {
  const M = requireModule();
  check(M._bjw_enc_reset());
  writeValue(M, value);
  const len = M._bjw_enc_finish();
  if (len < 0) throw codeError(len, 'encode');
  const ptr = M._bjw_enc_ptr();
  // Copy out: the builder buffer is reused on the next encode call.
  return M.HEAPU8.slice(ptr, ptr + len);
}

/** Rebuild a JS value from the flat event stream emitted by the C decoder. */
function readEvents(M, ptr, len) {
  const heap = M.HEAPU8;
  const dv = new DataView(heap.buffer, heap.byteOffset, heap.byteLength);
  const stack = [];
  let root;
  let off = ptr;
  const end = ptr + len;

  const emit = (v) => {
    if (stack.length === 0) { root = v; return; }
    const top = stack[stack.length - 1];
    if (top.isObject) { top.value[top.key] = v; top.key = undefined; }
    else top.value.push(v);
  };

  while (off < end) {
    const tag = heap[off++];
    switch (tag) {
      case EV.NULL: emit(null); break;
      case EV.FALSE: emit(false); break;
      case EV.TRUE: emit(true); break;
      case EV.INT: emit(dv.getFloat64(off, true)); off += 8; break;
      case EV.FLOAT: emit(dv.getFloat64(off, true)); off += 8; break;
      case EV.DATE: emit(new Date(dv.getFloat64(off, true))); off += 8; break;
      case EV.POINTER: emit(new Pointer(dv.getFloat64(off, true))); off += 8; break;
      case EV.STRING: {
        const n = dv.getUint32(off, true); off += 4;
        emit(textDecoder.decode(heap.subarray(off, off + n))); off += n;
        break;
      }
      case EV.KEY: {
        const n = dv.getUint32(off, true); off += 4;
        stack[stack.length - 1].key = textDecoder.decode(heap.subarray(off, off + n));
        off += n;
        break;
      }
      case EV.BINARY: {
        const n = dv.getUint32(off, true); off += 4;
        emit(heap.slice(off, off + n)); off += n;
        break;
      }
      case EV.OID: {
        emit(new ObjectId(heap.slice(off, off + 12))); off += 12;
        break;
      }
      case EV.ARR_BEGIN: off += 4; stack.push({ isObject: false, value: [] }); break;
      case EV.OBJ_BEGIN: off += 4; stack.push({ isObject: true, value: {}, key: undefined }); break;
      case EV.ARR_END:
      case EV.OBJ_END: emit(stack.pop().value); break;
      default: throw new Error(`binjson: bad event tag ${tag}`);
    }
  }
  return root;
}

/**
 * Decode binjson binary data to a JavaScript value.
 * @param {Uint8Array|ArrayBuffer} data
 */
function decode(data) {
  const M = requireModule();
  const u8 = data instanceof Uint8Array ? data : new Uint8Array(data);
  const n = u8.length;
  const inPtr = n ? M._malloc(n) : 0;
  if (n) M.HEAPU8.set(u8, inPtr);

  let rc;
  try {
    rc = M._bjw_decode(inPtr, n);
  } finally {
    if (n) M._free(inPtr);
  }
  if (rc !== 0) throw codeError(rc, 'decode');

  const evPtr = M._bjw_events_ptr();
  const evLen = M._bjw_events_len();
  return readEvents(M, evPtr, evLen);
}

/**
 * Total on-wire size of the value whose leading bytes are `header`, computed by
 * the C codec (bj_value_size). `header` only needs the type byte plus, for
 * length-prefixed/container types, the 4-byte size field (i.e. up to 5 bytes).
 */
function wasmValueSize(M, header) {
  const n = header.length;
  const inPtr = M._malloc(n + 4);
  M.HEAPU8.set(header, inPtr);
  const outPtr = inPtr + n;
  const rc = M._bjw_value_size(inPtr, n, 0, outPtr);
  let size = 0;
  if (rc === 0) {
    size = new DataView(M.HEAPU8.buffer).getUint32(outPtr, true);
  }
  M._free(inPtr);
  if (rc !== 0) throw codeError(rc, 'value_size');
  return size;
}

/**
 * On-wire size (in bytes) of the top-level value whose leading bytes are
 * `header`, computed by the C codec. `header` only needs the type byte plus,
 * for length-prefixed/container types, the 4-byte size field (i.e. up to 5
 * bytes). Await ready() before calling. Useful for scanning append-only files
 * of concatenated records without decoding each one.
 */
function valueSize(header) {
  const M = requireModule();
  return wasmValueSize(M, header instanceof Uint8Array ? header : new Uint8Array(header));
}


// ---------------------------------------------------------------------------
// Shared helpers for the tree/index/log/diff/stemmer wrappers below.
// ---------------------------------------------------------------------------

// Aliases so the copied wrappers can keep using their original names.
const encoder = textEncoder;
const decoder = textDecoder;

/**
 * Host I/O registry for the file-resident C structures (c/hostio.c).
 *
 * Each open FileSystemSyncAccessHandle is registered under an integer slot in
 * `Module.bjioHandles`; the C side reads and writes the file through EM_JS
 * imports that index this table and pass HEAPU8 subarray views straight to the
 * handle's synchronous read/write — the bytes move directly between the file
 * and WASM memory with no intermediate copies, and no copy of the file is ever
 * held in memory on either side of the bridge.
 */
let nextBjioFd = 1;

function registerHandle(M, syncHandle) {
  if (!M.bjioHandles) M.bjioHandles = {};
  const fd = nextBjioFd++;
  M.bjioHandles[fd] = syncHandle;
  return fd;
}

function unregisterHandle(M, fd) {
  if (M.bjioHandles) delete M.bjioHandles[fd];
}

/** Copy a JS string into the heap as UTF-8; returns { ptr, len, free }. */
function allocStr(M, str) {
  const bytes = textEncoder.encode(str);
  const len = bytes.length;
  const ptr = M._malloc(len || 1);
  if (len) M.HEAPU8.set(bytes, ptr);
  return { ptr, len, free() { M._free(ptr); } };
}

/** Little-endian u32 read from the heap (HEAPU32 isn't exported). */
function readU32(M, addr) {
  const b = M.HEAPU8;
  return (b[addr] | (b[addr + 1] << 8) | (b[addr + 2] << 16) | (b[addr + 3] * 0x1000000)) >>> 0;
}

/** Little-endian signed i32 read from the heap -- for out-params carrying a BJ_ERR_* code (can be negative). */
function readI32(M, addr) {
  return readU32(M, addr) | 0;
}

/** Copy a JS string into the heap as UTF-8; returns { ptr, len }. */
function writeBytes(M, str) {
  const bytes = encoder.encode(str);
  const ptr = M._malloc(bytes.length || 1);
  if (bytes.length) M.HEAPU8.set(bytes, ptr);
  return { ptr, len: bytes.length };
}

/** Copy a JS string into the heap as a NUL-terminated C string; returns ptr. */
function writeCString(M, str) {
  const bytes = encoder.encode(str);
  const ptr = M._malloc(bytes.length + 1);
  if (bytes.length) M.HEAPU8.set(bytes, ptr);
  M.HEAPU8[ptr + bytes.length] = 0;
  return ptr;
}

/**
 * Read a (uint8_t** out, size_t* outlen) result the C side malloc'd, decode it
 * as UTF-8, and free the C buffer. `outPP`/`outLP` are heap slots holding the
 * pointer and length.
 */
function takeOut(M, outPP, outLP) {
  const outPtr = readU32(M, outPP);
  const outLen = readU32(M, outLP);
  const bytes = M.HEAPU8.slice(outPtr, outPtr + outLen);
  if (outPtr) M._free(outPtr);
  return decoder.decode(bytes);
}

// ---------------------------------------------------------------------------
// B+ tree
// ---------------------------------------------------------------------------

/**
 * Order-preserving byte encoding of one scalar key part, so the B+ tree's
 * byte-wise (memcmp) key comparison reproduces the value's natural order.
 * Numbers encode to a 9-byte sequence (a 0x00 tag then a sign-normalized
 * big-endian IEEE-754 double); strings to a 0x01 tag, their UTF-8 bytes, and a
 * 0x00 terminator. The number tag sorts before the string tag, matching the
 * tree's "numbers before strings" rule, and both forms are self-delimiting so
 * they can be concatenated (see compositeKey). String parts must not contain
 * U+0000 (reserved as the terminator, matching the engine's NUL convention).
 *
 * @param {number|string} value
 * @returns {Uint8Array}
 */
function orderedKey(value) {
  if (typeof value === 'number') {
    if (Number.isNaN(value)) throw new Error('orderedKey: NaN has no ordering');
    if (value === 0) value = 0; // normalize -0 to +0 so they compare equal
    const out = new Uint8Array(9);
    out[0] = 0x00; // number tag (sorts before strings)
    const dv = new DataView(out.buffer);
    dv.setFloat64(1, value, false); // big-endian
    // Total-order transform: flip the sign bit for positives, all bits for
    // negatives, so unsigned byte order matches numeric order.
    if (out[1] & 0x80) { for (let i = 1; i < 9; i++) out[i] ^= 0xff; }
    else out[1] ^= 0x80;
    return out;
  }
  if (typeof value === 'string') {
    const body = textEncoder.encode(value);
    for (let i = 0; i < body.length; i++) {
      if (body[i] === 0) throw new Error('orderedKey: string key must not contain U+0000');
    }
    const out = new Uint8Array(body.length + 2);
    out[0] = 0x01; // string tag
    out.set(body, 1);
    out[out.length - 1] = 0x00; // terminator keeps prefixes ordered correctly
    return out;
  }
  throw new Error(`orderedKey: unsupported part type: ${typeof value}`);
}

/**
 * Build a composite B+ tree key from ordered parts — the convention for
 * duplicate / secondary indexes, where the tree itself is unique-key. Encode
 * the indexed value(s) followed by the primary key:
 *   tree.add(compositeKey(tag, postId), postId)
 * All entries sharing a leading value then form a contiguous range; retrieve
 * them with a range/cursor scan whose lower bound is compositeKey(...prefix)
 * and whose upper bound appends 0xff bytes (compositeUpperBound). The row
 * reference lives in the value, so the composite key is never decoded back.
 *
 * @param {...(number|string)} parts
 * @returns {Uint8Array}
 */
function compositeKey(...parts) {
  const encoded = parts.map(orderedKey);
  let total = 0;
  for (const e of encoded) total += e.length;
  const out = new Uint8Array(total);
  let at = 0;
  for (const e of encoded) { out.set(e, at); at += e.length; }
  return out;
}

/**
 * Upper bound for scanning every composite key that begins with `parts`: the
 * prefix followed by 0xff. Because each part's encoding is self-delimiting, a
 * real continuation always starts with a tag byte (0x00/0x01) below 0xff, so
 * this sorts after every key extending the prefix yet before the next distinct
 * prefix value. Use as the max bound of a range/cursor scan grouped by `parts`
 * (bpt_range is inclusive; this sentinel is never itself a stored key).
 *
 * @param {...(number|string)} parts
 * @returns {Uint8Array}
 */
function compositeUpperBound(...parts) {
  const prefix = compositeKey(...parts);
  const out = new Uint8Array(prefix.length + 1);
  out.set(prefix, 0);
  out[prefix.length] = 0xff;
  return out;
}

/**
 * Persistent immutable B+ tree with append-only WASM-backed storage.
 * Mirrors the API of the original (since removed) pure-JS implementation.
 *
 * The tree is unique-key (add is an upsert). For duplicate / secondary-index
 * access, store composite keys built with compositeKey()/orderedKey() and scan
 * grouped ranges with compositeUpperBound(); string and Uint8Array keys are
 * both accepted (Uint8Array bytes pass through verbatim).
 */
class BPlusTree {
  /**
   * @param {FileSystemSyncAccessHandle} syncHandle - storage file handle
   * @param {number} order - tree order (default 3, minimum 3)
   *
   * The tree is durable: every add/delete writes its appended bytes straight
   * through to the file handle (matching the write-through model of
   * the original JS design), so data survives a crash before close().
   */
  constructor(syncHandle, order = 3) {
    if (order < 3) {
      throw new Error('B+ tree order must be at least 3');
    }
    this.syncAccessHandle = syncHandle;
    this.order = order;
    this.isOpen = false;
    this.ctx = 0;
    this._fd = 0;
    this._size = 0;
  }

  /**
   * Open the tree against the file handle. The C side is file-resident: it
   * reads nodes from the handle on demand and writes each mutation's records
   * straight through, so nothing is buffered here and data survives a crash
   * before close() (matching the original JS model).
   */
  async open() {
    if (this.isOpen) {
      throw new Error('Tree is already open');
    }
    const M = await ready();

    this._fd = registerHandle(M, this.syncAccessHandle);
    const fileSize = this.syncAccessHandle.getSize();
    if (fileSize > 0) {
      this.ctx = M._bptw_open(this._fd);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close(); // isOpen never becomes true, so this.close() can't reach it -- must release it here
        throw new Error('Invalid tree file');
      }
      this.order = M._bptw_order(this.ctx);
    } else {
      this.ctx = M._bptw_create(this._fd, this.order);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close();
        throw new Error('Failed to create B+ tree');
      }
    }
    this._size = M._bptw_size(this.ctx);
    this.isOpen = true;
  }

  /** fsync the file handle (all writes are already on it). */
  flush() {
    this.syncAccessHandle.flush();
  }

  /** Close the sync handle and release the WASM context. */
  async close() {
    if (!this.isOpen) return;
    if (this.syncAccessHandle) {
      this.flush();
      await this.syncAccessHandle.close();
    }
    if (this.ctx) {
      Module._bptw_free(this.ctx);
      this.ctx = 0;
    }
    unregisterHandle(Module, this._fd);
    this._fd = 0;
    this.isOpen = false;
  }

  /** Allocate a marshalled key; caller must call .free(). */
  #allocKey(key) {
    const M = Module;
    if (typeof key === 'number') {
      return { type: 0, num: key, ptr: 0, len: 0, free() {} };
    }
    // A string key marshals as its UTF-8 bytes; a Uint8Array is passed
    // verbatim (opaque byte-string key). Both are string-type keys on the C
    // side (compared byte-for-byte), so composite / order-preserving keys
    // built with compositeKey()/orderedKey() flow through unchanged.
    let bytes = null;
    if (typeof key === 'string') bytes = textEncoder.encode(key);
    else if (key instanceof Uint8Array) bytes = key;
    if (bytes) {
      const len = bytes.length;
      const ptr = len ? M._malloc(len) : 0;
      if (len) M.HEAPU8.set(bytes, ptr);
      return { type: 1, num: 0, ptr, len, free() { if (len) M._free(ptr); } };
    }
    throw new Error(`Unsupported key type: ${typeof key}`);
  }

  /** Insert or update a key-value pair. */
  add(key, value) {
    const M = requireModule();
    const k = this.#allocKey(key);
    const vbytes = encode(value);
    const vlen = vbytes.length;
    const vptr = vlen ? M._malloc(vlen) : 0;
    if (vlen) M.HEAPU8.set(vbytes, vptr);
    try {
      const rc = M._bptw_add(this.ctx, k.type, k.num, k.ptr, k.len, vptr, vlen);
      if (rc !== 0) throw codeError(rc, 'add');
      this._size = M._bptw_size(this.ctx);
    } finally {
      k.free();
      if (vlen) M._free(vptr);
    }
  }

  /** Search for a key; returns the value or undefined. */
  search(key) {
    const M = requireModule();
    const k = this.#allocKey(key);
    try {
      const rc = M._bptw_search(this.ctx, k.type, k.num, k.ptr, k.len);
      if (rc < 0) throw codeError(rc, 'search');
      if (rc === 0) return undefined;
      return this.#readOut(M, 'search');
    } finally {
      k.free();
    }
  }

  /** Decode this tree's last output buffer (scoped to the handle: calls on
   * other trees don't disturb it). Throws if the length overflows the
   * boundary's int. */
  #readOut(M, op) {
    const ptr = M._bptw_out_ptr(this.ctx);
    const len = M._bptw_out_len(this.ctx);
    if (len < 0) throw codeError(len, op);
    return decode(M.HEAPU8.slice(ptr, ptr + len));
  }

  /** Delete a key (no-op if absent). */
  delete(key) {
    const M = requireModule();
    const k = this.#allocKey(key);
    try {
      const rc = M._bptw_delete(this.ctx, k.type, k.num, k.ptr, k.len);
      if (rc !== 0) throw codeError(rc, 'delete');
      this._size = M._bptw_size(this.ctx);
    } finally {
      k.free();
    }
  }

  /** All entries as an array of { key, value } in sorted order. */
  toArray() {
    const M = requireModule();
    const rc = M._bptw_entries(this.ctx);
    if (rc !== 0) throw codeError(rc, 'toArray');
    return this.#readOut(M, 'toArray');
  }

  /** Entries with min <= key <= max, in sorted order. */
  rangeSearch(minKey, maxKey) {
    const M = requireModule();
    const kmin = this.#allocKey(minKey);
    const kmax = this.#allocKey(maxKey);
    try {
      const rc = M._bptw_range(
        this.ctx,
        kmin.type, kmin.num, kmin.ptr, kmin.len,
        kmax.type, kmax.num, kmax.ptr, kmax.len
      );
      if (rc !== 0) throw codeError(rc, 'rangeSearch');
      return this.#readOut(M, 'rangeSearch');
    } finally {
      kmin.free();
      kmax.free();
    }
  }

  /** Allocate an optional marshalled key: undefined/null means "no bound". */
  #allocKeyOpt(key) {
    if (key === undefined || key === null) {
      return { type: -1, num: 0, ptr: 0, len: 0, free() {} };
    }
    return this.#allocKey(key);
  }

  /**
   * Stream entries in sorted order through a C cursor, optionally bounded to
   * minKey <= key <= maxKey (either bound may be omitted). Memory is bounded
   * by the batch size, not the result size: the cursor reads one leaf at a
   * time and entries cross the bridge in ~64 KB batches.
   *
   * The cursor pins the tree's root at open, so iteration sees a consistent
   * snapshot even if the tree is mutated while iterating.
   */
  async *iterate(minKey, maxKey) {
    if (!this.isOpen) {
      throw new Error('Tree must be open before iteration');
    }
    const M = requireModule();
    const kmin = this.#allocKeyOpt(minKey);
    const kmax = this.#allocKeyOpt(maxKey);
    let cur;
    try {
      cur = M._bptw_cursor_open(
        this.ctx,
        kmin.type, kmin.num, kmin.ptr, kmin.len,
        kmax.type, kmax.num, kmax.ptr, kmax.len
      );
    } finally {
      kmin.free();
      kmax.free();
    }
    if (!cur) throw new Error('Failed to open cursor');
    try {
      // Batches grow from 2 KB to 64 KB: the first results arrive after a
      // couple of leaf reads (early termination stays cheap), while long
      // scans quickly reach full batch throughput.
      let batchBytes = 2048;
      for (;;) {
        if (!this.isOpen) throw new Error('Tree closed during iteration');
        const n = M._bptw_cursor_next(cur, batchBytes);
        if (n < 0) throw codeError(n, 'cursor');
        if (n === 0) return;
        const batch = this.#readOut(M, 'cursor');
        for (const entry of batch) yield entry;
        batchBytes = Math.min(batchBytes * 4, 65536);
      }
    } finally {
      M._bptw_cursor_free(cur);
    }
  }

  /** Async iterator over { key, value } entries in sorted order. */
  async *[Symbol.asyncIterator]() {
    yield* this.iterate();
  }

  /** Tree height (0 for a single leaf). */
  getHeight() {
    const M = requireModule();
    const h = M._bptw_height(this.ctx);
    if (h < 0) throw codeError(h, 'getHeight');
    return h;
  }

  /**
   * Walk every node checking the tree's structural invariants: key order
   * and routing-key consistency, node capacity, uniform leaf depth,
   * child-before-parent offsets (no cycles), and the entry count matching
   * the metadata size. Min-fill is deliberately not checked — JS-written
   * files never rebalance and are legitimately under-filled — and unary
   * internal nodes are legal (compaction emits them at level tails).
   * Returns true, or throws describing the corruption. O(N): a
   * testing/diagnostic tool, not an every-request check.
   */
  verify() {
    const M = requireModule();
    const rc = M._bptw_verify(this.ctx);
    if (rc !== 0) throw codeError(rc, 'verify');
    return true;
  }

  size() {
    return requireModule()._bptw_size(this.ctx);
  }

  isEmpty() {
    return this.size() === 0;
  }

  /**
   * Wrap a C-side read-only handle as a snapshot object: all read APIs work
   * (search, rangeSearch, toArray, iterate, size, compact), mutations throw.
   * The snapshot shares this tree's file handle without owning it — close
   * the snapshot before closing the parent tree.
   */
  #wrapSnapshot(ctx) {
    const M = requireModule();
    // A real instance (not Object.create) so private-field methods work.
    const snap = new BPlusTree(this.syncAccessHandle, M._bptw_order(ctx));
    snap.ctx = ctx;                 // shared file handle, not owned
    snap._fd = this._fd;
    snap._size = M._bptw_size(ctx);
    snap.isOpen = true;
    snap.isSnapshot = true;
    snap.open = async () => { throw new Error('Snapshot is already open'); };
    snap.close = async function () {
      if (!this.isOpen) return;
      requireModule()._bptw_free(this.ctx);
      this.ctx = 0;
      this.isOpen = false;
    };
    return snap;
  }

  /**
   * Read-only snapshot pinned at the current root. The file is append-only,
   * so the snapshot stays consistent while this tree keeps mutating (it
   * simply never sees later changes). Invalidated if the file is truncated
   * or replaced (e.g. adopting a compaction).
   */
  snapshot() {
    if (!this.isOpen) throw new Error('Tree file is not open');
    const ctx = requireModule()._bptw_snapshot(this.ctx);
    if (!ctx) throw new Error('Failed to create snapshot');
    return this.#wrapSnapshot(ctx);
  }

  /**
   * Read-only snapshot pinned at a historical commit boundary — an `offset`
   * from boundaries(). Time-travel: the tree exactly as it was when that
   * commit landed.
   */
  snapshotAt(offset) {
    if (!this.isOpen) throw new Error('Tree file is not open');
    const ctx = requireModule()._bptw_open_at(this._fd, offset);
    if (!ctx) throw new Error(`No commit boundary at offset ${offset}`);
    return this.#wrapSnapshot(ctx);
  }

  /**
   * Every verified commit boundary in the file, oldest first, as
   * [{ offset, size }] — offset opens that state via snapshotAt(), size is
   * the entry count it had. Scans the file.
   */
  boundaries() {
    if (!this.isOpen) throw new Error('Tree file is not open');
    const M = requireModule();
    const rc = M._bptw_boundaries(this.ctx);
    if (rc !== 0) throw codeError(rc, 'boundaries');
    return this.#readOut(M, 'boundaries');
  }

  /**
   * Compact into a fresh file, dropping stale append-only history and any
   * deletion cruft. The C side streams a minimal fully-packed tree (bulk
   * load) straight to the destination handle — nothing is materialized in
   * memory.
   * @param {FileSystemSyncAccessHandle} destSyncHandle
   * @returns {Promise<{oldSize:number,newSize:number,bytesSaved:number}>}
   */
  async compact(destSyncHandle) {
    if (!this.isOpen) {
      throw new Error('Tree file is not open');
    }
    if (!destSyncHandle) {
      throw new Error('Destination sync handle is required for compaction');
    }
    const M = requireModule();
    const oldSize = this.syncAccessHandle.getSize();

    destSyncHandle.truncate(0);
    const dstFd = registerHandle(M, destSyncHandle);
    try {
      const rc = M._bptw_compact(this.ctx, dstFd);
      if (rc !== 0) throw codeError(rc, 'compact');
    } finally {
      unregisterHandle(M, dstFd);
    }
    const newSize = destSyncHandle.getSize();
    destSyncHandle.flush();
    await destSyncHandle.close();

    return {
      oldSize,
      newSize,
      bytesSaved: Math.max(0, oldSize - newSize)
    };
  }
}

// ---------------------------------------------------------------------------
// R-tree
// ---------------------------------------------------------------------------

/**
 * Haversine distance in kilometers, computed by the WASM libm (c/geo.c).
 * Requires the module to be instantiated — call ready() (or open() a tree)
 * first.
 */
function haversineDistance(lat1, lng1, lat2, lng2) {
  return requireModule()._rtw_haversine(lat1, lng1, lat2, lng2);
}

/**
 * Persistent on-disk R-tree with append-only WASM-backed storage.
 * Mirrors the API of the original (since removed) pure-JS implementation.
 */
class RTree {
  /**
   * @param {FileSystemSyncAccessHandle} syncHandle - storage file handle
   * @param {number} maxEntries - node capacity (default 9, minimum 2)
   *
   * The tree is durable: every mutation writes its appended bytes straight
   * through to the file handle (matching the write-through model of
   * the original JS design), so data survives a crash before close().
   */
  constructor(syncHandle, maxEntries = 9) {
    this.syncAccessHandle = syncHandle;
    this.maxEntries = maxEntries;
    this.isOpen = false;
    this.ctx = 0;
    this._fd = 0;
    this._size = 0;

    // Shim exposing file size, used by some tests (tree.file.getFileSize()).
    this.file = {
      getFileSize: () => this.syncAccessHandle.getSize()
    };
  }

  /**
   * Open the tree against the file handle. The C side is file-resident: it
   * reads nodes from the handle on demand and writes each mutation's records
   * straight through (matching the original JS model).
   */
  async open() {
    if (this.isOpen) {
      throw new Error('R-tree is already open');
    }
    const M = await ready();

    this._fd = registerHandle(M, this.syncAccessHandle);
    const fileSize = this.syncAccessHandle.getSize();
    if (fileSize > 0) {
      this.ctx = M._rtw_open(this._fd);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close(); // isOpen never becomes true, so this.close() can't reach it -- must release it here
        throw new Error('Invalid R-tree file');
      }
      this.maxEntries = M._rtw_max_entries(this.ctx);
    } else {
      this.ctx = M._rtw_create(this._fd, this.maxEntries);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close();
        throw new Error('Failed to create R-tree');
      }
    }
    this._size = M._rtw_size(this.ctx);
    this.isOpen = true;
  }

  /** fsync the file handle (all writes are already on it). */
  flush() {
    this.syncAccessHandle.flush();
  }

  /** Close the sync handle and release the WASM context. */
  async close() {
    if (!this.isOpen) return;
    if (this.syncAccessHandle) {
      this.flush();
      await this.syncAccessHandle.close();
    }
    if (this.ctx) {
      Module._rtw_free(this.ctx);
      this.ctx = 0;
    }
    unregisterHandle(Module, this._fd);
    this._fd = 0;
    this.isOpen = false;
  }

  /**
   * Insert a point (lat, lng) associated with an ObjectId.
   *
   * ObjectId uniqueness is the caller's contract: duplicates are never
   * checked, so inserting the same id twice stores two independent entries
   * and remove() takes out only one of them.
   */
  insert(lat, lng, objectId) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    if (!(objectId instanceof ObjectId)) {
      throw new Error('objectId must be an instance of ObjectId to insert into rtree');
    }
    const M = requireModule();
    const bytes = objectId.toBytes();
    const ptr = M._malloc(12);
    M.HEAPU8.set(bytes, ptr);
    try {
      const rc = M._rtw_insert(this.ctx, lat, lng, ptr);
      if (rc !== 0) throw codeError(rc, 'insert');
      this._size = M._rtw_size(this.ctx);
    } finally {
      M._free(ptr);
    }
  }

  /**
   * Remove the entry for an ObjectId. Returns true if one was removed.
   * Pass the entry's stored coordinates when known: OIDs have no spatial
   * locality, so a blind remove probes subtrees in order (worst-case the
   * whole tree) while a located remove prunes to the point's path. A wrong
   * point finds nothing and returns false.
   */
  remove(objectId, lat, lng) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    if (!(objectId instanceof ObjectId)) {
      throw new Error('objectId must be an instance of ObjectId to remove from rtree');
    }
    const located = typeof lat === 'number' && typeof lng === 'number';
    const M = requireModule();
    const bytes = objectId.toBytes();
    const ptr = M._malloc(12);
    M.HEAPU8.set(bytes, ptr);
    try {
      const rc = located
        ? M._rtw_remove_at(this.ctx, lat, lng, ptr)
        : M._rtw_remove(this.ctx, ptr);
      if (rc < 0) throw codeError(rc, 'remove');
      this._size = M._rtw_size(this.ctx);
      return rc === 1;
    } finally {
      M._free(ptr);
    }
  }

  /**
   * Stream bounding-box matches without materializing the result set:
   * yields { objectId, lat, lng } in bounded batches, pinned to the tree
   * state at the first pull (append-only snapshot semantics). Early
   * termination reads only the nodes already visited.
   */
  async *iterateBBox(bbox) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    const M = requireModule();
    const cur = M._rtw_cursor_open(this.ctx, bbox.minLat, bbox.maxLat, bbox.minLng, bbox.maxLng);
    if (!cur) throw new Error('Failed to open cursor');
    try {
      let batchBytes = 2048;
      for (;;) {
        const n = M._rtw_cursor_next(cur, batchBytes);
        if (n < 0) throw codeError(n, 'iterateBBox');
        if (n === 0) return;
        const entries = this._readOut(M, 'iterateBBox');
        for (const e of entries) yield e;
        batchBytes = Math.min(batchBytes * 4, 65536);
      }
    } finally {
      M._rtw_cursor_free(cur);
    }
  }

  /**
   * The k nearest entries to a point, best-first over node bounding boxes —
   * reads only subtrees that can beat the current candidates. Returns
   * [{ objectId, lat, lng, distance }] by ascending haversine km.
   */
  nearest(lat, lng, k) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    const M = requireModule();
    const rc = M._rtw_nearest(this.ctx, lat, lng, k);
    if (rc !== 0) throw codeError(rc, 'nearest');
    return this._readOut(M, 'nearest');
  }

  /** Decode this tree's last output buffer (scoped to the handle: calls on
   * other trees don't disturb it). Throws if the length overflows the
   * boundary's int. */
  _readOut(M, op) {
    const ptr = M._rtw_out_ptr(this.ctx);
    const len = M._rtw_out_len(this.ctx);
    if (len < 0) throw codeError(len, op);
    if (len === 0) return [];
    return decode(M.HEAPU8.slice(ptr, ptr + len));
  }

  /** Candidate entries whose point falls inside a bounding box. */
  _searchBBoxRaw(bbox) {
    const M = requireModule();
    const rc = M._rtw_search(this.ctx, bbox.minLat, bbox.maxLat, bbox.minLng, bbox.maxLng);
    if (rc !== 0) throw codeError(rc, 'searchBBox');
    return this._readOut(M, 'searchBBox');
  }

  /** Search for points within a bounding box; returns { objectId, lat, lng }. */
  searchBBox(bbox) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    return this._searchBBoxRaw(bbox);
  }

  /**
   * Search for points within a radius (km) of a location; returns
   * { objectId, lat, lng, distance }. The radius-to-bbox conversion, tree
   * traversal and haversine distance filter all run in C (c/geo.c + rtree.c).
   */
  searchRadius(lat, lng, radiusKm) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    const M = requireModule();
    const rc = M._rtw_search_radius(this.ctx, lat, lng, radiusKm);
    if (rc !== 0) throw codeError(rc, 'searchRadius');
    return this._readOut(M, 'searchRadius');
  }

  /** Drop all entries by appending a fresh empty root. */
  async clear() {
    const M = requireModule();
    const rc = M._rtw_clear(this.ctx);
    if (rc !== 0) throw codeError(rc, 'clear');
    this._size = 0;
  }

  size() {
    return this._size;
  }

  isEmpty() {
    return this._size === 0;
  }

  /**
   * Compact into a fresh file, dropping stale append-only history.
   * @param {FileSystemSyncAccessHandle} destSyncHandle
   * @returns {Promise<{oldSize:number,newSize:number,bytesSaved:number}>}
   */
  async compact(destSyncHandle) {
    if (!this.isOpen) {
      throw new Error('R-tree file must be opened before use');
    }
    if (!destSyncHandle) {
      throw new Error('Destination sync handle is required for compaction');
    }
    const M = requireModule();
    const oldSize = this.syncAccessHandle.getSize();

    // The C side streams the compacted records straight to the destination
    // handle in chunks; the compacted file is never materialized in memory.
    destSyncHandle.truncate(0);
    const dstFd = registerHandle(M, destSyncHandle);
    try {
      const rc = M._rtw_compact(this.ctx, dstFd);
      if (rc !== 0) throw codeError(rc, 'compact');
    } finally {
      unregisterHandle(M, dstFd);
    }
    const newSize = destSyncHandle.getSize();
    destSyncHandle.flush();
    await destSyncHandle.close();

    return {
      oldSize,
      newSize,
      bytesSaved: Math.max(0, oldSize - newSize)
    };
  }
}

// ---------------------------------------------------------------------------
// Text versioning log
// ---------------------------------------------------------------------------

/**
 * Persistent versioned text log with append-only WASM-backed storage.
 * Mirrors the API of the original (since removed) pure-JS implementation.
 */
class TextLog {
  /**
   * @param {FileSystemSyncAccessHandle} syncHandle - storage file handle
   * @param {number} diffsPerSnapshot - diffs between full snapshots (default 10)
   * @param {number} baseVersion - when creating a fresh file, the global
   *   version this tile continues from: it owns versions (baseVersion, ...].
   *   0 (the default) is an ordinary standalone log; only TiledTextLog passes
   *   a nonzero value. Ignored when opening an existing file (its stored base
   *   is adopted). See textlog_create_at.
   */
  constructor(syncHandle, diffsPerSnapshot = 10, baseVersion = 0) {
    if (diffsPerSnapshot < 1) {
      throw new Error('diffsPerSnapshot must be at least 1');
    }
    this.syncAccessHandle = syncHandle;
    this.diffsPerSnapshot = diffsPerSnapshot;
    this.baseVersion = baseVersion;
    this.isOpen = false;
    this.ctx = 0;
    this._fd = 0;
    this.version = 0;

    // Shim mirroring the reference's `file` member (used by some tests).
    this.file = {
      syncAccessHandle: syncHandle,
      getFileSize: () => this.syncAccessHandle.getSize()
    };
  }

  /**
   * Open the log against the file handle. The C side is file-resident: open
   * scans the file once to index entry offsets, then every read fetches only
   * the records it needs and every addVersion writes straight through
   * (matching the original JS model).
   */
  async open() {
    if (this.isOpen) {
      throw new Error('TextLog is already open');
    }
    const M = await ready();

    this._fd = registerHandle(M, this.syncAccessHandle);
    const fileSize = this.syncAccessHandle.getSize();
    if (fileSize > 0) {
      this.ctx = M._tlw_open(this._fd);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close(); // isOpen never becomes true, so this.close() can't reach it -- must release it here
        throw new Error('Failed to read metadata: no valid metadata found');
      }
      this.diffsPerSnapshot = M._tlw_diffs_per_snapshot(this.ctx);
      this.baseVersion = M._tlw_base_version(this.ctx);
    } else {
      this.ctx = M._tlw_create_at(this._fd, this.diffsPerSnapshot, this.baseVersion);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close();
        throw new Error('Failed to create TextLog');
      }
    }
    this.version = M._tlw_version(this.ctx);
    this.isOpen = true;
  }

  /** fsync the file handle (all writes are already on it). */
  flush() {
    this.syncAccessHandle.flush();
  }

  /** Close the sync handle and release the WASM context. */
  async close() {
    if (!this.isOpen) return;
    if (this.syncAccessHandle) {
      this.flush();
      await this.syncAccessHandle.close();
    }
    if (this.ctx) {
      Module._tlw_free(this.ctx);
      this.ctx = 0;
    }
    unregisterHandle(Module, this._fd);
    this._fd = 0;
    this.isOpen = false;
  }

  /** Read the current output buffer as a UTF-8 string. */
  _readOut(M) {
    const ptr = M._tlw_out_ptr(this.ctx);
    const len = M._tlw_out_len(this.ctx);
    if (len < 0) throw codeError(len, 'textlog');
    if (len === 0) return '';
    return decoder.decode(M.HEAPU8.slice(ptr, ptr + len));
  }

  /**
   * Add a new version of the text.
   * @param {string} text - full text content for this version
   * @returns {number} the new version number
   */
  async addVersion(text) {
    if (!this.isOpen) {
      throw new Error('TextLog is not open');
    }
    if (typeof text !== 'string') {
      throw new Error('Text must be a string');
    }
    const M = requireModule();
    const bytes = encoder.encode(text);
    const ptr = M._malloc(bytes.length || 1);
    if (bytes.length) M.HEAPU8.set(bytes, ptr);
    try {
      const v = M._tlw_add_version(this.ctx, ptr, bytes.length, Date.now());
      if (v < 0) throw codeError(v, 'addVersion');
      this.version = v;
      return v;
    } finally {
      M._free(ptr);
    }
  }

  /**
   * Get the full text at a specific version.
   * @param {number} version - version number to retrieve
   * @returns {string} the text at that version
   */
  async getVersion(version) {
    if (!this.isOpen) {
      throw new Error('TextLog is not open');
    }
    if (version <= this.baseVersion || version > this.version) {
      throw new Error(`Invalid version: ${version}. Valid range: ${this.baseVersion + 1}-${this.version}`);
    }
    const M = requireModule();
    const rc = M._tlw_get_version(this.ctx, version);
    if (rc !== 0) throw codeError(rc, 'getVersion');
    return this._readOut(M);
  }

  /**
   * Get a human-readable diff between two versions.
   * @param {number} fromVersion - starting version
   * @param {number} toVersion - ending version
   * @returns {string} human-readable unified diff
   */
  async getDiff(fromVersion, toVersion) {
    if (!this.isOpen) {
      throw new Error('TextLog is not open');
    }
    if (fromVersion <= this.baseVersion || fromVersion > this.version) {
      throw new Error(`Invalid fromVersion: ${fromVersion}. Valid range: ${this.baseVersion + 1}-${this.version}`);
    }
    if (toVersion <= this.baseVersion || toVersion > this.version) {
      throw new Error(`Invalid toVersion: ${toVersion}. Valid range: ${this.baseVersion + 1}-${this.version}`);
    }
    const M = requireModule();
    const rc = M._tlw_get_diff(this.ctx, fromVersion, toVersion);
    if (rc !== 0) throw codeError(rc, 'getDiff');
    return this._readOut(M);
  }

  /** Get current version number. */
  getCurrentVersion() {
    return this.version;
  }

  /**
   * Get the SHA-256 hash of a specific version.
   * @param {number} version - version number
   * @returns {string} hex string hash
   */
  async getVersionHash(version) {
    if (!this.isOpen) {
      throw new Error('TextLog is not open');
    }
    if (version <= this.baseVersion || version > this.version) {
      throw new Error(`Invalid version: ${version}. Valid range: ${this.baseVersion + 1}-${this.version}`);
    }
    const M = requireModule();
    const rc = M._tlw_get_version_hash(this.ctx, version);
    if (rc !== 0) throw codeError(rc, 'getVersionHash');
    return this._readOut(M);
  }
}

/**
 * A versioned text log spread across multiple append-only tile files, so full
 * history is kept while no single file grows without bound — the space lever
 * for long-lived documents (wiki pages, blog posts) whose old revisions must
 * stay available. Each tile is an ordinary TextLog whose metadata records the
 * global version it continues from (baseVersion), so every tile reconstructs
 * independently: a read opens only the tile owning the requested version, and
 * a cold open scans only the active tile instead of the whole history.
 *
 * Tiling policy lives entirely here, outside the file format. The host passes a
 * `provider` mapping tiles to storage:
 *   - listTiles():          Promise<Array<{id, baseVersion}>>  (no file opens)
 *   - openTile(id):         Promise<syncHandle>                (existing tile)
 *   - createTile(baseVer):  Promise<{id, handle}>              (fresh empty file)
 * The tiles (each identified by its baseVersion) are the manifest; the host may
 * name files however it likes — e.g. by baseVersion — and needs no separate
 * manifest record, since each tile's range is recoverable from its own base and
 * current version.
 */
class TiledTextLog {
  /**
   * @param {object} provider - tile storage provider (see class docs)
   * @param {object} [options]
   * @param {number} [options.diffsPerSnapshot=10] - diffs between snapshots
   * @param {number} [options.maxTileBytes=1048576] - roll to a new tile once
   *   the active tile's file reaches this size (checked before each add)
   * @param {number} [options.maxOpenTiles=4] - open-tile cache cap (the active
   *   tile is always kept open)
   */
  constructor(provider, options = {}) {
    const { diffsPerSnapshot = 10, maxTileBytes = 1 << 20, maxOpenTiles = 4 } = options;
    if (diffsPerSnapshot < 1) throw new Error('diffsPerSnapshot must be at least 1');
    if (maxTileBytes < 1) throw new Error('maxTileBytes must be positive');
    this.provider = provider;
    this.diffsPerSnapshot = diffsPerSnapshot;
    this.maxTileBytes = maxTileBytes;
    this.maxOpenTiles = Math.max(1, maxOpenTiles);
    this.isOpen = false;
    this.version = 0;
    this._tiles = [];            // { id, baseVersion }, ascending by baseVersion
    this._open = new Map();      // id -> { log: TextLog, lru: number }
    this._lruClock = 0;
    this._active = null;         // descriptor of the newest tile
  }

  async open() {
    if (this.isOpen) throw new Error('TiledTextLog is already open');
    await ready();
    let tiles = (await this.provider.listTiles()) || [];
    tiles = tiles.slice().sort((a, b) => a.baseVersion - b.baseVersion);
    if (tiles.length === 0) {
      const { id } = await this.provider.createTile(0);
      tiles = [{ id, baseVersion: 0 }];
    }
    this._tiles = tiles;
    this._active = tiles[tiles.length - 1];
    // Open only the active tile to learn the current global version — cold
    // open scans one tile, not the whole history.
    const active = await this._openTile(this._active);
    this.version = active.version;
    this.isOpen = true;
  }

  // Fetch the tile for `desc` from the open-tile cache, opening it if needed.
  async _openTile(desc) {
    let entry = this._open.get(desc.id);
    if (entry) { entry.lru = ++this._lruClock; return entry.log; }
    const handle = await this.provider.openTile(desc.id);
    const log = new TextLog(handle, this.diffsPerSnapshot, desc.baseVersion);
    await log.open();
    entry = { log, lru: ++this._lruClock };
    this._open.set(desc.id, entry);
    await this._evict();
    return log;
  }

  // Keep at most maxOpenTiles tiles open; never evict the active tile.
  async _evict() {
    while (this._open.size > this.maxOpenTiles) {
      let victimId = null, victimLru = Infinity;
      for (const [id, e] of this._open) {
        if (id === this._active.id) continue;
        if (e.lru < victimLru) { victimLru = e.lru; victimId = id; }
      }
      if (victimId === null) break;
      const e = this._open.get(victimId);
      this._open.delete(victimId);
      await e.log.close();
    }
  }

  // Descriptor of the tile owning global `version` — the one with the largest
  // baseVersion strictly below it (a tile serves versions in (base, current]).
  _tileFor(version) {
    let lo = 0, hi = this._tiles.length - 1, ans = this._tiles[0];
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (this._tiles[mid].baseVersion < version) { ans = this._tiles[mid]; lo = mid + 1; }
      else hi = mid - 1;
    }
    return ans;
  }

  async addVersion(text) {
    if (!this.isOpen) throw new Error('TiledTextLog is not open');
    let active = await this._openTile(this._active);
    // Roll to a fresh tile once the active one passes the size threshold. The
    // new tile is anchored by writing version (this.version + 1) as a full
    // snapshot, so no version is duplicated across the boundary. Never roll a
    // tile that has not yet received a version of its own.
    if (this.version > this._active.baseVersion &&
        active.syncAccessHandle.getSize() >= this.maxTileBytes) {
      const { id } = await this.provider.createTile(this.version);
      const desc = { id, baseVersion: this.version };
      this._tiles.push(desc);
      this._active = desc;
      active = await this._openTile(desc);
    }
    const v = await active.addVersion(text);
    this.version = v;
    return v;
  }

  async getVersion(version) {
    this._checkRange(version);
    const log = await this._openTile(this._tileFor(version));
    return log.getVersion(version);
  }

  async getVersionHash(version) {
    this._checkRange(version);
    const log = await this._openTile(this._tileFor(version));
    return log.getVersionHash(version);
  }

  async getDiff(fromVersion, toVersion) {
    this._checkRange(fromVersion);
    this._checkRange(toVersion);
    // Reconstruct both texts (each from whichever tile owns it) and render the
    // unified diff through the same routine TextLog.getDiff uses, so output is
    // byte-identical whether or not the versions fall in the same tile.
    const fromText = await this.getVersion(fromVersion);
    const toText = await this.getVersion(toVersion);
    return unifiedDiff(fromText, toText, fromVersion, toVersion);
  }

  _checkRange(version) {
    if (version < 1 || version > this.version) {
      throw new Error(`Invalid version: ${version}. Valid range: 1-${this.version}`);
    }
  }

  /** Current (highest) global version. */
  getCurrentVersion() { return this.version; }

  /** Number of tiles the history currently spans. */
  get tileCount() { return this._tiles.length; }

  /** fsync the active tile (older tiles are immutable). */
  flush() {
    const e = this._open.get(this._active.id);
    if (e) e.log.flush();
  }

  async close() {
    if (!this.isOpen) return;
    for (const e of this._open.values()) await e.log.close();
    this._open.clear();
    this.isOpen = false;
  }
}

// Entry type constants (mirror the on-disk format).
const ENTRY_TYPE = {
  FULL_SNAPSHOT: 0x01,
  DIFF: 0x02
};

// ---------------------------------------------------------------------------
// Entry log (Raft log / write-ahead log)
// ---------------------------------------------------------------------------

// Entry type conventions (mirror the EL_* bytes in entrylog.h). Stored and
// returned verbatim, never interpreted by the log; values >= 0x10 are free
// for host use.
const ENTRYLOG_TYPE = {
  NORMAL: 0x01,   // state-machine command (opaque payload)
  NOOP: 0x02,     // leader's empty entry committed on election
  CONFIG: 0x03    // cluster membership change
};

/**
 * Persistent replicated-command log (the Raft log, which is also the
 * database's write-ahead log) with append-only WASM-backed storage — see
 * include/entrylog.h for the design.
 *
 * Entries are (index, term, type, payload): indexes are assigned by the log
 * and strictly contiguous, terms monotonically non-decreasing, payloads
 * opaque bytes. append() only buffers; an entry is durable once sync()
 * returns (one write + flush) — acknowledge replication RPCs only after
 * that. Hard state (currentTerm/votedFor) commits immediately via
 * setHardState(), as Raft requires before answering any RPC.
 */
class EntryLog {
  /**
   * @param {FileSystemSyncAccessHandle} syncHandle - storage file handle
   * @param {object} [options]
   * @param {number} [options.baseIndex=0] - when creating a fresh file, the
   *   snapshot boundary this tile continues from: it owns indexes
   *   (baseIndex, ...]. Ignored when opening an existing file.
   * @param {number} [options.baseTerm=0] - term of entry baseIndex (the
   *   snapshot's lastIncludedTerm). Ignored when opening an existing file.
   */
  constructor(syncHandle, { baseIndex = 0, baseTerm = 0 } = {}) {
    this.syncAccessHandle = syncHandle;
    this._createBaseIndex = baseIndex;
    this._createBaseTerm = baseTerm;
    this.isOpen = false;
    this.ctx = 0;
    this._fd = 0;
  }

  /**
   * Open the log against the file handle. The C side is file-resident: open
   * scans the file once (verifying every protected commit's CRC, recovering
   * a torn tail by truncation) to index entry offsets, then every read
   * fetches only the records it needs.
   */
  async open() {
    if (this.isOpen) {
      throw new Error('EntryLog is already open');
    }
    const M = await ready();

    this._fd = registerHandle(M, this.syncAccessHandle);
    const fileSize = this.syncAccessHandle.getSize();
    if (fileSize > 0) {
      this.ctx = M._elw_open(this._fd);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close(); // isOpen never becomes true, so this.close() can't reach it -- must release it here
        throw new Error('Invalid entry log file');
      }
    } else {
      this.ctx = M._elw_create_at(this._fd, this._createBaseIndex, this._createBaseTerm);
      if (!this.ctx) {
        unregisterHandle(M, this._fd);
        await this.syncAccessHandle.close();
        throw new Error('Failed to create EntryLog');
      }
    }
    this.isOpen = true;
  }

  /** fsync the file handle (all committed writes are already on it). */
  flush() {
    this.syncAccessHandle.flush();
  }

  /** Close the sync handle and release the WASM context. Buffered
   * (un-synced) appends are NOT written — they were never acknowledged. */
  async close() {
    if (!this.isOpen) return;
    if (this.syncAccessHandle) {
      this.flush();
      await this.syncAccessHandle.close();
    }
    if (this.ctx) {
      Module._elw_free(this.ctx);
      this.ctx = 0;
    }
    unregisterHandle(Module, this._fd);
    this._fd = 0;
    this.isOpen = false;
  }

  _ensureOpen() {
    if (!this.isOpen) throw new Error('EntryLog is not open');
  }

  /** Tile base: the log holds indexes (baseIndex, lastIndex]. */
  get baseIndex() { this._ensureOpen(); return requireModule()._elw_base_index(this.ctx); }
  /** Term of entry baseIndex (the snapshot boundary's term). */
  get baseTerm() { this._ensureOpen(); return requireModule()._elw_base_term(this.ctx); }
  /** Highest index in the log (== baseIndex when empty). */
  get lastIndex() { this._ensureOpen(); return requireModule()._elw_last_index(this.ctx); }
  /** Term of the highest entry (== baseTerm when empty). */
  get lastTerm() { this._ensureOpen(); return requireModule()._elw_last_term(this.ctx); }
  /** Raft hard state, as of the last committed metadata record. */
  get currentTerm() { this._ensureOpen(); return requireModule()._elw_current_term(this.ctx); }
  /** Voted-for node id this term (0 = none). */
  get votedFor() { this._ensureOpen(); return requireModule()._elw_voted_for(this.ctx); }
  /** Advisory commit index (0 if never recorded). */
  get commitIndex() { this._ensureOpen(); return requireModule()._elw_commit_index(this.ctx); }

  /** Payload marshalling: a Uint8Array passes verbatim, a string as UTF-8. */
  _allocPayload(payload) {
    const M = Module;
    let bytes = null;
    if (payload instanceof Uint8Array) bytes = payload;
    else if (typeof payload === 'string') bytes = encoder.encode(payload);
    else throw new Error('Payload must be a Uint8Array or string');
    const len = bytes.length;
    const ptr = M._malloc(len || 1);
    if (len) M.HEAPU8.set(bytes, ptr);
    return { ptr, len, free() { M._free(ptr); } };
  }

  /**
   * Buffer one entry with index lastIndex + 1. `term` must be between the
   * current lastTerm and currentTerm (persist a term bump with
   * setHardState() first). NOT durable until sync().
   * @returns {number} the assigned index
   */
  append(term, payload, type = ENTRYLOG_TYPE.NORMAL) {
    this._ensureOpen();
    const M = requireModule();
    const p = this._allocPayload(payload);
    try {
      const index = M._elw_append(this.ctx, term, type, p.ptr, p.len);
      if (index < 0) throw codeError(index, 'append');
      return index;
    } finally {
      p.free();
    }
  }

  /**
   * Commit everything buffered since the last sync as one protected commit
   * and fsync it — the durability point. Acknowledge an AppendEntries RPC,
   * or count local persistence toward a quorum, only after this returns.
   */
  sync() {
    this._ensureOpen();
    const rc = requireModule()._elw_sync(this.ctx);
    if (rc !== 0) throw codeError(rc, 'sync');
    this.flush();
  }

  /**
   * Persist Raft hard state (currentTerm, votedFor). Commits and fsyncs
   * immediately (plus any buffered entries): answer the RequestVote /
   * AppendEntries RPC only after this returns. A new term resets any
   * previous vote; changing an existing vote within its term throws.
   */
  setHardState(term, votedFor = 0) {
    this._ensureOpen();
    const rc = requireModule()._elw_set_hard_state(this.ctx, term, votedFor);
    if (rc !== 0) throw codeError(rc, 'setHardState');
    this.flush();
  }

  /**
   * Record the commit index (highest index known replicated to a quorum).
   * Advisory — Raft rederives it after restart — so this only stages the
   * value; it rides along with the next sync().
   */
  setCommitIndex(index) {
    this._ensureOpen();
    const rc = requireModule()._elw_set_commit_index(this.ctx, index);
    if (rc !== 0) throw codeError(rc, 'setCommitIndex');
  }

  /** Term of entry `index`; baseIndex answers baseTerm (the AppendEntries
   * consistency check at the snapshot boundary). Served from memory. */
  termAt(index) {
    this._ensureOpen();
    const term = requireModule()._elw_term_at(this.ctx, index);
    if (term < 0) throw codeError(term, 'termAt');
    return term;
  }

  /**
   * Read one entry (one file read).
   * @returns {{index:number,term:number,type:number,payload:Uint8Array}}
   */
  get(index) {
    this._ensureOpen();
    const M = requireModule();
    const slots = M._malloc(16);   // f64 term at +0, i32 type at +8
    try {
      const rc = M._elw_get(this.ctx, index, slots);
      if (rc !== 0) throw codeError(rc, 'get');
      const heap = M.HEAPU8;
      const dv = new DataView(heap.buffer, heap.byteOffset + slots, 16);
      const term = dv.getFloat64(0, true);
      const type = dv.getInt32(8, true);
      const ptr = M._elw_out_ptr(this.ctx);
      const len = M._elw_out_len(this.ctx);
      if (len < 0) throw codeError(len, 'get');
      return { index, term, type, payload: M.HEAPU8.slice(ptr, ptr + len) };
    } finally {
      M._free(slots);
    }
  }

  /**
   * Pull entries in bulk for an AppendEntries batch or the apply loop:
   * entries from `fromIndex` upward until roughly `maxBytes` of payload is
   * gathered (always at least one) or the log ends. fromIndex at or below
   * baseIndex throws (compacted entries live in the snapshot — answer with
   * InstallSnapshot instead); fromIndex beyond lastIndex returns [].
   * @returns {Array<{index:number,term:number,type:number,payload:Uint8Array}>}
   */
  getBatch(fromIndex, maxBytes = 65536) {
    this._ensureOpen();
    const M = requireModule();
    const n = M._elw_get_batch(this.ctx, fromIndex, maxBytes);
    if (n < 0) throw codeError(n, 'getBatch');
    if (n === 0) return [];
    const ptr = M._elw_out_ptr(this.ctx);
    const len = M._elw_out_len(this.ctx);
    if (len < 0) throw codeError(len, 'getBatch');
    return decode(M.HEAPU8.slice(ptr, ptr + len));
  }

  /**
   * Discard entries from `index` upward (the Raft conflict rule) and commit
   * the truncation immediately. Logical: dead bytes stay in the file until
   * compact(). Entries at or below commitIndex may not be truncated, and
   * buffered appends must be synced first.
   */
  truncateFrom(index) {
    this._ensureOpen();
    const rc = requireModule()._elw_truncate_from(this.ctx, index);
    if (rc !== 0) throw codeError(rc, 'truncateFrom');
    this.flush();
  }

  /**
   * Walk every live entry checking the log's invariants: contiguous
   * indexes, monotonically non-decreasing terms, stored records matching
   * the in-memory index, bounds matching the metadata. Returns true, or
   * throws describing the corruption. O(N).
   */
  verify() {
    this._ensureOpen();
    const rc = requireModule()._elw_verify(this.ctx);
    if (rc !== 0) throw codeError(rc, 'verify');
    return true;
  }

  /**
   * Rewrite the live entries above the snapshot boundary (newBaseIndex,
   * whose term must be newBaseTerm) into a fresh file, dropping compacted
   * entries, truncated dead bytes, and superseded metadata. Hard state
   * carries over; commitIndex carries over raised to at least newBaseIndex.
   * The host adopts the destination file in place of the old one.
   * @param {FileSystemSyncAccessHandle} destSyncHandle
   * @returns {Promise<{oldSize:number,newSize:number,bytesSaved:number}>}
   */
  async compact(destSyncHandle, newBaseIndex, newBaseTerm) {
    this._ensureOpen();
    if (!destSyncHandle) {
      throw new Error('Destination sync handle is required for compaction');
    }
    const M = requireModule();
    const oldSize = this.syncAccessHandle.getSize();

    destSyncHandle.truncate(0);
    const dstFd = registerHandle(M, destSyncHandle);
    try {
      const rc = M._elw_compact(this.ctx, dstFd, newBaseIndex, newBaseTerm);
      if (rc !== 0) throw codeError(rc, 'compact');
    } finally {
      unregisterHandle(M, dstFd);
    }
    const newSize = destSyncHandle.getSize();
    destSyncHandle.flush();
    await destSyncHandle.close();

    return {
      oldSize,
      newSize,
      bytesSaved: Math.max(0, oldSize - newSize)
    };
  }
}

// ---------------------------------------------------------------------------
// Full-text index
// ---------------------------------------------------------------------------

/**
 * WASM full-text index. Mirrors the API of the original (since removed) pure-JS implementation.
 */
class TextIndex {
  constructor(options = {}) {
    const { order = 16, trees, journal } = options;
    this.order = order;
    this.index = trees?.index || null;
    this.documentTerms = trees?.documentTerms || null;
    this.documentLengths = trees?.documentLengths || null;
    // Optional sync access handle for the cross-tree commit journal: with it,
    // every add/remove/clear is atomic across the three tree files (a crash
    // between tree writes is rolled back on the next open). A journal belongs
    // to one set of tree files; give freshly compacted files an empty one.
    this.journal = journal || null;
    this.journalFd = -1;
    this.outCtx = 0;   // per-index query-output slot in the WASM heap
    this.isOpen = false;
  }

  async open() {
    if (this.isOpen) throw new Error('TextIndex is already open');
    if (!this.index || !this.documentTerms || !this.documentLengths) {
      throw new Error('Trees must be initialized before opening');
    }
    if (!this.outCtx) this.outCtx = requireModule()._tixw_out_new();
    if (!this.outCtx) throw new Error('Failed to allocate query output slot');
    await Promise.all([this.index.open(), this.documentTerms.open(), this.documentLengths.open()]);
    if (this.journal) {
      const M = requireModule();
      this.journalFd = registerHandle(M, this.journal);
      const [ix, dt, dl] = this._ctxs();
      const rc = M._tixw_recover(this.journalFd, ix, dt, dl);
      if (rc !== 0) {
        unregisterHandle(M, this.journalFd);
        this.journalFd = -1;
        this.journal.close();
        await Promise.all([this.index.close(), this.documentTerms.close(), this.documentLengths.close()]);
        throw codeError(rc, 'recover');
      }
    }
    this.isOpen = true;
  }

  async close() {
    if (this.outCtx) {
      requireModule()._tixw_out_free(this.outCtx);
      this.outCtx = 0;
    }
    if (!this.isOpen) return;
    if (this.journalFd >= 0) {
      unregisterHandle(requireModule(), this.journalFd);
      this.journalFd = -1;
      this.journal.flush();
      this.journal.close();
    }
    await Promise.all([this.index.close(), this.documentTerms.close(), this.documentLengths.close()]);
    this.isOpen = false;
  }

  _ensureOpen() {
    if (!this.isOpen) throw new Error('TextIndex is not open');
  }

  _ctxs() {
    return [this.index.ctx, this.documentTerms.ctx, this.documentLengths.ctx];
  }

  async add(docId, text) {
    this._ensureOpen();
    if (!docId) throw new Error('Document ID is required');
    const M = requireModule();
    const t = typeof text === 'string' ? text : '';
    const d = allocStr(M, docId);
    const x = allocStr(M, t);
    try {
      const [ix, dt, dl] = this._ctxs();
      const rc = M._tixw_add(ix, dt, dl, this.journalFd, d.ptr, d.len, x.ptr, x.len);
      if (rc !== 0) throw codeError(rc, 'add');
    } finally {
      d.free(); x.free();
    }
  }

  async remove(docId) {
    this._ensureOpen();
    const M = requireModule();
    const d = allocStr(M, String(docId));
    try {
      const [ix, dt, dl] = this._ctxs();
      const rc = M._tixw_remove(ix, dt, dl, this.journalFd, d.ptr, d.len);
      if (rc < 0) throw codeError(rc, 'remove');
      return rc === 1;
    } finally {
      d.free();
    }
  }

  _readOut(M) {
    const ptr = M._tixw_out_ptr(this.outCtx);
    const len = M._tixw_out_len(this.outCtx);
    if (len < 0) throw codeError(len, 'query');
    if (len === 0) return [];
    return decode(M.HEAPU8.slice(ptr, ptr + len));
  }

  async query(queryText, options = { scored: true, requireAll: false }) {
    this._ensureOpen();
    const M = requireModule();
    const q = allocStr(M, typeof queryText === 'string' ? queryText : '');
    try {
      const [ix, dt, dl] = this._ctxs();
      if (options.requireAll) {
        const rc = M._tixw_query_all(this.outCtx, ix, dt, dl, q.ptr, q.len);
        if (rc !== 0) throw codeError(rc, 'query');
        return this._readOut(M); // array of id strings
      }
      const rc = M._tixw_query(this.outCtx, ix, dt, dl, q.ptr, q.len);
      if (rc !== 0) throw codeError(rc, 'query');
      const results = this._readOut(M); // array of { id, score }
      if (options.scored === false) return results.map(r => r.id);
      return results;
    } finally {
      q.free();
    }
  }

  async getTermCount() {
    this._ensureOpen();
    const M = requireModule();
    const n = M._tixw_term_count(this.index.ctx);
    if (n < 0) throw codeError(n, 'getTermCount');
    return n;
  }

  async getDocumentCount() {
    this._ensureOpen();
    return this.documentTerms.size();
  }

  async clear() {
    this._ensureOpen();
    const M = requireModule();
    const [ix, dt, dl] = this._ctxs();
    const rc = M._tixw_clear(ix, dt, dl, this.journalFd);
    if (rc !== 0) throw codeError(rc, 'clear');
  }

  async compact({ index: destIndex, documentTerms: destDocTerms, documentLengths: destDocLengths }) {
    this._ensureOpen();
    if (!destIndex || !destDocTerms || !destDocLengths) {
      throw new Error('Destination trees must be provided for compaction');
    }
    const terms = await this.index.compact(destIndex.syncAccessHandle);
    const documents = await this.documentTerms.compact(destDocTerms.syncAccessHandle);
    const lengths = await this.documentLengths.compact(destDocLengths.syncAccessHandle);
    await this.close();
    this.isOpen = false;
    return { terms, documents, lengths };
  }
}

// ---------------------------------------------------------------------------
// Porter stemmer
// ---------------------------------------------------------------------------

/**
 * Return the Porter stem of `value`. Matches stemmer@2.0.1 byte-for-byte for
 * ASCII words. Requires the module to be instantiated (await ready()).
 */
function stemmer(value) {
  const M = requireModule();
  const bytes = encoder.encode(String(value));
  const len = bytes.length;
  // Worst case the stem length equals the input; +2 for a possible appended
  // 'e'/'i' and the NUL terminator the C side writes.
  const inPtr = M._malloc(len || 1);
  const outPtr = M._malloc(len + 2);
  try {
    if (len) M.HEAPU8.set(bytes, inPtr);
    const outLen = M._stemmer_stem(inPtr, len, outPtr);
    return decoder.decode(M.HEAPU8.slice(outPtr, outPtr + outLen));
  } finally {
    M._free(inPtr);
    M._free(outPtr);
  }
}

// ---------------------------------------------------------------------------
// Diff engine
// ---------------------------------------------------------------------------

/** createPatch(fileName, a, b) — full unified diff with INCLUDE_HEADERS. */
function createPatch(fileName, a, b) {
  const M = requireModule();
  const namePtr = writeCString(M, fileName);
  const A = writeBytes(M, a), B = writeBytes(M, b);
  const outPP = M._malloc(4), outLP = M._malloc(4);
  try {
    const rc = M._diff_create_patch(namePtr, A.ptr, A.len, B.ptr, B.len, outPP, outLP);
    if (rc !== 0) throw new Error(`createPatch failed (${rc})`);
    return takeOut(M, outPP, outLP);
  } finally {
    M._free(namePtr); M._free(A.ptr); M._free(B.ptr); M._free(outPP); M._free(outLP);
  }
}

/**
 * The unified diff textlog.js's getDiff renders: `--- <fromLabel>` / `+++
 * <toLabel>` headers followed by `@@`/context/`+`/`-` lines. Labels default to
 * matching textlog's `version 1` / `version 2`.
 */
function unifiedDiff(a, b, fromLabel = 1, toLabel = 2) {
  const M = requireModule();
  const A = writeBytes(M, a), B = writeBytes(M, b);
  const outPP = M._malloc(4), outLP = M._malloc(4);
  try {
    const rc = M._diff_get_diff(fromLabel | 0, toLabel | 0, A.ptr, A.len, B.ptr, B.len, outPP, outLP);
    if (rc !== 0) throw new Error(`unifiedDiff failed (${rc})`);
    return takeOut(M, outPP, outLP);
  } finally {
    M._free(A.ptr); M._free(B.ptr); M._free(outPP); M._free(outLP);
  }
}

/** applyPatch(source, patch) — returns the patched string, or null if it doesn't fit. */
function applyPatch(source, patch) {
  const M = requireModule();
  const S = writeBytes(M, source), P = writeBytes(M, patch);
  const outPP = M._malloc(4), outLP = M._malloc(4), appliedP = M._malloc(4);
  try {
    const rc = M._diff_apply_patch(S.ptr, S.len, P.ptr, P.len, outPP, outLP, appliedP);
    if (rc !== 0) throw new Error(`applyPatch failed (${rc})`);
    if (readU32(M, appliedP) === 0) return null;
    return takeOut(M, outPP, outLP);
  } finally {
    M._free(S.ptr); M._free(P.ptr); M._free(outPP); M._free(outLP); M._free(appliedP);
  }
}

/** Like takeOut, but returns the raw bytes (the delta is binary, not text). */
function takeOutBytes(M, outPP, outLP) {
  const outPtr = readU32(M, outPP);
  const outLen = readU32(M, outLP);
  const bytes = M.HEAPU8.slice(outPtr, outPtr + outLen);
  if (outPtr) M._free(outPtr);
  return bytes;
}

/**
 * Binary copy/insert delta that rebuilds `target` from `source` — the compact
 * format TextLog stores for diffs (diff.h). Returns a Uint8Array; feed it to
 * applyDelta(source, delta) to reconstruct the target.
 */
function createDelta(source, target) {
  const M = requireModule();
  const S = writeBytes(M, source), T = writeBytes(M, target);
  const outPP = M._malloc(4), outLP = M._malloc(4);
  try {
    const rc = M._diff_create_delta(S.ptr, S.len, T.ptr, T.len, outPP, outLP);
    if (rc !== 0) throw new Error(`createDelta failed (${rc})`);
    return takeOutBytes(M, outPP, outLP);
  } finally {
    M._free(S.ptr); M._free(T.ptr); M._free(outPP); M._free(outLP);
  }
}

/** Apply a createDelta delta to `source`; returns the target string, or null
 * if the delta is malformed / out of bounds. */
function applyDelta(source, delta) {
  const M = requireModule();
  const S = writeBytes(M, source);
  const dlen = delta.length;
  const dptr = M._malloc(dlen || 1);
  if (dlen) M.HEAPU8.set(delta, dptr);
  const outPP = M._malloc(4), outLP = M._malloc(4), appliedP = M._malloc(4);
  try {
    const rc = M._diff_apply_delta(S.ptr, S.len, dptr, dlen, outPP, outLP, appliedP);
    if (rc !== 0) throw new Error(`applyDelta failed (${rc})`);
    if (readU32(M, appliedP) === 0) return null;
    return takeOut(M, outPP, outLP);
  } finally {
    M._free(S.ptr); M._free(dptr); M._free(outPP); M._free(outLP); M._free(appliedP);
  }
}

export {
  ready,
  isReady,
  TYPE,
  ObjectId,
  Pointer,
  encode,
  decode,
  valueSize,
  orderedKey,
  compositeKey,
  compositeUpperBound,
  BPlusTree,
  haversineDistance,
  RTree,
  TextLog,
  TiledTextLog,
  EntryLog,
  ENTRYLOG_TYPE,
  TextIndex,
  ENTRY_TYPE,
  stemmer,
  createPatch,
  unifiedDiff,
  applyPatch,
  createDelta,
  applyDelta
};
