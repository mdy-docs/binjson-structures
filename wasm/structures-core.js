/**
 * structures-core.js — the WASM-backed data-structure wrapper classes
 * (B+ tree, R-tree, text log, entry log, snapshot store, text index,
 * stemmer, diff engine), parameterized over the WASM module instance
 * they run against.
 *
 * This file deliberately imports nothing and names no binary: every
 * module-bound dependency arrives through bindStructures(runtime), so
 * the same classes can run against ANY compiled binary whose export
 * surface includes the _bptw_/_rtw_/_tlw_/_elw_/_tixw_/_diff_/_stemmer_
 * symbols (see build-wasm.sh's export manifest). This package's own
 * standalone entry (binjson-structures-wasm.js) binds them to its own
 * lib/ binary; a consumer that links these C sources into a larger
 * combined binary (real link-time calls require one binary and one
 * linear memory) binds the same classes to that module instead of
 * keeping a copy of this file.
 *
 * The runtime contract -- everything the consumer already has:
 *   ready()          instantiate the module; idempotent promise
 *   requireModule()  the live module instance, or throw if not ready
 *   codeError(code, context)  BJ_ERR_* code -> Error (consumer may map
 *                    codes to richer error classes)
 *   check(code)      throw codeError(code) unless code === 0
 *   encode(value) / decode(bytes) / valueSize(header)
 *                    the binjson codec bound to the SAME module
 *   ObjectId, Pointer, TYPE   binjson's module-independent value types
 *   registerHandle(M, h) / unregisterHandle(M, fd)   OPTIONAL -- override
 *                    the built-in bjioHandles registry when the consumer
 *                    wraps handles before they cross the EM_JS bridge;
 *                    override both together
 *
 * Returns the full structure surface; see the return statement at the
 * bottom for the exact names.
 */
export function bindStructures(runtime) {
  const {
    ready, requireModule, codeError, check,
    encode, decode, valueSize,
    ObjectId, Pointer, TYPE
  } = runtime;

  // ---------------------------------------------------------------------------
  // Shared helpers for the tree/index/log/diff/stemmer wrappers below.
  // ---------------------------------------------------------------------------

  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  /**
   * Host I/O registry for the file-resident C structures (c/hostio.c).
   *
   * Each open FileSystemSyncAccessHandle is registered under an integer slot in
   * the module's `bjioHandles` table; the C side reads and writes the file through EM_JS
   * imports that index this table and pass HEAPU8 subarray views straight to the
   * handle's synchronous read/write — the bytes move directly between the file
   * and WASM memory with no intermediate copies, and no copy of the file is ever
   * held in memory on either side of the bridge.
   */
  let nextBjioFd = 1;

  function defaultRegisterHandle(M, syncHandle) {
    if (!M.bjioHandles) M.bjioHandles = {};
    const fd = nextBjioFd++;
    M.bjioHandles[fd] = syncHandle;
    return fd;
  }

  function defaultUnregisterHandle(M, fd) {
    if (M.bjioHandles) delete M.bjioHandles[fd];
  }

  // A consumer that wraps handles before they cross the EM_JS bridge (e.g.
  // to swallow QuotaExceededError into a short write so C unwinds cleanly)
  // injects its own pair; both must be overridden together so the fd space
  // stays consistent.
  const registerHandle = runtime.registerHandle ?? defaultRegisterHandle;
  const unregisterHandle = runtime.unregisterHandle ?? defaultUnregisterHandle;

  /** Copy a JS string into the heap as UTF-8; returns { ptr, len, free }. */
  function allocStr(M, str) {
    const bytes = encoder.encode(str);
    const len = bytes.length;
    const ptr = M._malloc(len || 1);
    if (len) M.HEAPU8.set(bytes, ptr);
    return { ptr, len, free() { M._free(ptr); } };
  }

  /** Copy `bytes` into the heap, call fn(ptr, len), then free. */
  function withBytes(M, bytes, fn) {
    const n = bytes.length;
    const ptr = M._malloc(n || 1);
    if (n) M.HEAPU8.set(bytes, ptr);
    try { return fn(ptr, n); } finally { M._free(ptr); }
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
   * Composite-key encoding: marshalling only. The encoding itself lives in
   * src/keyenc.c (see keyenc.h for the wire shape), which is also what the
   * document layer calls to key its secondary indexes.
   *
   * These three used to be a second, independent implementation of that
   * encoding in JavaScript -- two encoders that had to agree byte-for-byte
   * forever, with nothing checking that they did. They now build the key in
   * C through one shared, reused builder (keyenc_wasm.c's qkw).
   *
   * Consequence worth knowing: like everything else in this file, they now
   * require the module (`await ready()`), where the pure-JS versions did
   * not.
   */
  let keyCtx = 0;

  /** The shared key builder, allocated once, rewound per call. */
  function keyBuilder(M) {
    if (!keyCtx) {
      keyCtx = M._qkw_new();
      if (!keyCtx) throw codeError(-1, 'qkw_new');   // BJ_ERR_OOM
    }
    M._qkw_reset(keyCtx);
    return keyCtx;
  }

  /** Append one part. The JS *type* dispatch is genuinely a JS concern -- C
   *  never sees a JS value -- but every domain rule (NaN has no ordering, a
   *  string part must not contain U+0000) is C's. */
  function putKeyPart(M, ctx, value) {
    let rc;
    if (typeof value === 'number') {
      rc = M._qkw_put_number(ctx, value);
    } else if (typeof value === 'string') {
      const s = allocStr(M, value);
      try { rc = M._qkw_put_string(ctx, s.ptr, s.len); } finally { s.free(); }
    } else {
      throw new Error(`orderedKey: unsupported part type: ${typeof value}`);
    }
    if (rc !== 0) {
      throw new Error(
        `orderedKey: ${JSON.stringify(value)} has no order-preserving encoding ` +
        '(NaN, or a string containing U+0000)'
      );
    }
  }

  /** Copy the built key out of the heap. Re-reads HEAPU8: any of the calls
   *  above may have grown the heap and swapped the ArrayBuffer. */
  function takeKey(M, ctx) {
    const len = M._qkw_len(ctx);
    check(len < 0 ? len : 0);
    const ptr = M._qkw_ptr(ctx);
    return M.HEAPU8.slice(ptr, ptr + len);
  }

  /**
   * Order-preserving byte encoding of one scalar key part, so the B+ tree's
   * byte-wise (memcmp) key comparison reproduces the value's natural order.
   *
   * @param {number|string} value
   * @returns {Uint8Array}
   */
  function orderedKey(value) {
    const M = requireModule();
    const ctx = keyBuilder(M);
    putKeyPart(M, ctx, value);
    return takeKey(M, ctx);
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
    const M = requireModule();
    const ctx = keyBuilder(M);
    for (const part of parts) putKeyPart(M, ctx, part);
    return takeKey(M, ctx);
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
    const M = requireModule();
    const ctx = keyBuilder(M);
    for (const part of parts) putKeyPart(M, ctx, part);
    check(M._qkw_put_upper_bound(ctx));
    return takeKey(M, ctx);
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
        requireModule()._bptw_free(this.ctx);
        this.ctx = 0;
      }
      unregisterHandle(requireModule(), this._fd);
      this._fd = 0;
      this.isOpen = false;
    }

    /** Allocate a marshalled key; caller must call .free(). */
    #allocKey(key) {
      const M = requireModule();
      if (typeof key === 'number') {
        return { type: 0, num: key, ptr: 0, len: 0, free() {} };
      }
      // A string key marshals as its UTF-8 bytes; a Uint8Array is passed
      // verbatim (opaque byte-string key). Both are string-type keys on the C
      // side (compared byte-for-byte), so composite / order-preserving keys
      // built with compositeKey()/orderedKey() flow through unchanged.
      let bytes = null;
      if (typeof key === 'string') bytes = encoder.encode(key);
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
     * Last replicated-log index applied to this tree (0 = not log-driven).
     * Recovered from the file's metadata on open — after a crash, the apply
     * loop replays committed log entries with index > appliedIndex().
     */
    appliedIndex() {
      return requireModule()._bptw_applied_index(this.ctx);
    }

    /**
     * Stage the log index about to be applied; the next mutation's commit
     * persists it atomically with the mutation (call before add/delete).
     * Sticky across commits; never decreases; snapshots refuse it.
     */
    setAppliedIndex(index) {
      const rc = requireModule()._bptw_set_applied_index(this.ctx, index);
      if (rc !== 0) throw codeError(rc, 'setAppliedIndex');
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
        requireModule()._rtw_free(this.ctx);
        this.ctx = 0;
      }
      unregisterHandle(requireModule(), this._fd);
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

    /** Last replicated-log index applied to this tree (0 = not log-driven).
     * See BPlusTree.appliedIndex. */
    appliedIndex() {
      return requireModule()._rtw_applied_index(this.ctx);
    }

    /** Stage the log index about to be applied; persisted atomically with the
     * next mutation's commit. See BPlusTree.setAppliedIndex. */
    setAppliedIndex(index) {
      const rc = requireModule()._rtw_set_applied_index(this.ctx, index);
      if (rc !== 0) throw codeError(rc, 'setAppliedIndex');
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
        requireModule()._tlw_free(this.ctx);
        this.ctx = 0;
      }
      unregisterHandle(requireModule(), this._fd);
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

    /** Last replicated-log index applied to this log (0 = not log-driven).
     * See BPlusTree.appliedIndex. */
    appliedIndex() {
      if (!this.isOpen) throw new Error('TextLog is not open');
      return requireModule()._tlw_applied_index(this.ctx);
    }

    /** Stage the log index about to be applied; persisted atomically with the
     * next addVersion's commit. See BPlusTree.setAppliedIndex. */
    setAppliedIndex(index) {
      if (!this.isOpen) throw new Error('TextLog is not open');
      const rc = requireModule()._tlw_set_applied_index(this.ctx, index);
      if (rc !== 0) throw codeError(rc, 'setAppliedIndex');
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
      this._ownsCtx = true;
    }

    /**
     * Wrap a log some other C component in this module opened and OWNS.
     * There is one: raft_node.h's rn_adopt rebases a node's log onto a
     * snapshot boundary through a bj_ns, entirely inside C, and hands
     * back only the pointer -- so a host that needs to READ that log
     * (terms, batches, the hard state) has no constructor to reach it
     * with.
     *
     * `ctx` is BORROWED and outlives this wrapper: close() releases the
     * file handle and the fd, which are the host's, and leaves the elog
     * to whoever created it. Closing it here instead would free a log the
     * node is still using, and the node frees it again at rn_free.
     */
    static adopting(ctx, syncHandle, fd) {
      const log = new EntryLog(syncHandle);
      log.ctx = ctx;
      log._fd = fd;
      log._ownsCtx = false;
      log.isOpen = true;
      return log;
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
        if (this._ownsCtx) requireModule()._elw_free(this.ctx);
        this.ctx = 0;
      }
      unregisterHandle(requireModule(), this._fd);
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
      const M = requireModule();
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
      // No flush() here: elog_sync fsyncs through bj_io.sync itself. This
      // used to be where durability actually happened, which meant C
      // declared the durability point and JS provided it -- two owners of
      // one contract, and a native host got neither.
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
      // Durable inside elog_set_hard_state -- see the note in sync().
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
      // Durable inside elog_truncate_from -- see the note in sync().
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
  // Snapshot store (Raft snapshot manifest & crash-safe adoption)
  // ---------------------------------------------------------------------------

  /**
   * CRC-32 of `bytes` (zlib polynomial — the exact routine that protects
   * every file commit, c/bjfile.c). Incremental: pass the previous value as
   * `prev` to continue a running checksum over streamed chunks. Requires the
   * module to be instantiated (await ready()).
   */
  function crc32(bytes, prev = 0) {
    const M = requireModule();
    const CH = 65536;
    let crc = prev >>> 0;
    const ptr = M._malloc(Math.min(bytes.length, CH) || 1);
    try {
      for (let off = 0; off < bytes.length; off += CH) {
        const chunk = bytes.subarray(off, Math.min(off + CH, bytes.length));
        M.HEAPU8.set(chunk, ptr);
        crc = M._bjfw_crc32(crc, ptr, chunk.length) >>> 0;
      }
    } finally {
      M._free(ptr);
    }
    return crc;
  }

  let checkCtx = 0;

  /**
   * Do these measured files match what a manifest promised?
   *
   * `actual` and `expected` are both [{ role, size, crc }] (`expected`
   * may carry more fields, e.g. a local manifest's `name`). Every
   * expected role must appear in `actual` and agree on BOTH size and
   * crc; a role that never arrived counts as a mismatch, because an
   * install missing a structure has not received the snapshot. Throws
   * naming the first offending role.
   *
   * Standalone, not a SnapshotStore method, because the third caller has
   * no store: test/raft-harness.js's in-memory snapshotter validates a
   * transferred snapshot with no files anywhere. It had its own copy of
   * this rule, as did the replicated install path and the store's
   * verify() -- and an independent implementation of a checksum rule is
   * not a second opinion worth having. A harness laxer than production
   * passes what production would reject.
   *
   * The comparison itself is C's (snapstore.h, sst_check_files); this is
   * marshalling.
   */
  function snapshotCheckFiles(actual, expected) {
    const M = requireModule();
    if (!checkCtx) {
      // Prefix-less: this context is used only for the comparison and
      // its output slot, never to name a file.
      checkCtx = M._sstw_new(0, 0);
      if (!checkCtx) throw codeError(-1, 'sstw_new');
    }
    const record = encode({ files: expected });
    const a = encode(actual);
    const rp = M._malloc(record.length || 1);
    const ap = M._malloc(a.length || 1);
    try {
      if (record.length) M.HEAPU8.set(record, rp);
      if (a.length) M.HEAPU8.set(a, ap);
      const rc = M._sstw_check_files(checkCtx, rp, record.length, ap, a.length);
      if (rc !== 0) {
        const ptr = M._sstw_out_ptr(checkCtx);
        const len = M._sstw_out_len(checkCtx);
        const role = len ? decoder.decode(M.HEAPU8.slice(ptr, ptr + len)) : '?';
        throw new Error(`Snapshot file ${role} failed its checksum`);
      }
    } finally { M._free(ap); M._free(rp); }
    return true;
  }

  /**
   * Crash-safe storage for Raft state-machine snapshots (and the compacted
   * entry-log files that pair with them), built on a naming convention
   * instead of atomic rename — OPFS has neither rename nor multi-file
   * atomicity, so adoption works like the commit protocol inside the data
   * files themselves: write everything, then write one small record whose
   * validity IS the commit.
   *
   * A snapshot is one *generation*: the immutable files
   *
   *   <prefix>-<gen>-<role>.bj          one per structure (host-chosen roles)
   *   <prefix>-<gen>.manifest.bj        written LAST — the commit point
   *
   * The manifest records { lastIncludedIndex, lastIncludedTerm, config,
   * files: [{ role, name, size, crc }] } as binjson followed by a CRC-32 of
   * those bytes, so a torn manifest never validates. open() adopts the
   * highest generation whose manifest validates and whose files are all
   * present at their recorded sizes, then sweeps every other generation's
   * files (a crashed snapshot attempt leaves data files with no manifest;
   * an adopted newer generation obsoletes older ones). Deletions are
   * best-effort — a locked or missing file is simply retried at the next
   * open() — so no failure here can lose the adopted snapshot.
   *
   * Generation files are immutable copies: take one by compacting each
   * structure into tx.createFile(role) (bpt_compact works from a live tree's
   * MVCC snapshot, so applies continue meanwhile), then tx.commit(meta). A
   * follower installing a leader's snapshot streams chunks into the same
   * shape and commits with the leader's meta; commit() recomputes each
   * file's CRC, so a corrupted transfer fails validation against the
   * leader's manifest before anything is adopted. To go live after install,
   * copy the snapshot files to the database's own live filenames
   * (copyFile) — the generation stays immutable and prunable.
   *
   * The paired entry-log convention: after a snapshot commits, compact the
   * log through (lastIncludedIndex, lastIncludedTerm) into createLogFile()
   * — named <prefix>-log-<gen>.bj — then pruneLogs(). On open, try
   * logCandidates() newest-first with EntryLog.open and adopt the first
   * that succeeds: a crash mid-compaction leaves a torn newest file that
   * fails to open, falling back to the previous one, which is only ever
   * deleted after its successor is durable.
   */
  class SnapshotStore {
    /**
     * @param {FileSystemDirectoryHandle} dirHandle - directory the snapshot
     *   files live in (e.g. navigator.storage.getDirectory() or a subdir)
     * @param {object} [options]
     * @param {string} [options.prefix='snap'] - filename prefix
     */
    constructor(dirHandle, { prefix = 'snap' } = {}) {
      this.dir = dirHandle;
      this.prefix = prefix;
      this.isOpen = false;
      this._latest = null;
      this._ctx = 0;    // the C store (snapstore.h), created on open()
    }

    /**
     * Call `fn(M, ctx)` and return the store's output buffer as bytes.
     * Every C call here writes its answer -- a filename, a plan, an
     * encoded manifest -- into that one slot.
     */
    _call(fn, context) {
      const M = requireModule();
      const rc = fn(M, this._ctx);
      if (rc !== 0) throw codeError(rc, context);
      const ptr = M._sstw_out_ptr(this._ctx);
      const len = M._sstw_out_len(this._ctx);
      return len ? M.HEAPU8.slice(ptr, ptr + len) : new Uint8Array(0);
    }

    _text(fn, context) { return decoder.decode(this._call(fn, context)); }

    /** NUL-separated names, as every plan in snapstore.h emits them. */
    _names(fn, context) {
      const s = this._text(fn, context);
      return s ? s.split('\0').filter(Boolean) : [];
    }

    _withStr(str, fn, context) {
      const M = requireModule();
      const a = allocStr(M, str);
      try { return fn(M, a); } finally { a.free(); }
    }

    _manifestName(gen) {
      return this._text((M, c) => M._sstw_manifest_name(c, gen), 'manifest name');
    }
    _dataName(gen, role) {
      return this._withStr(role, (M, a) =>
        this._text((MM, c) => MM._sstw_data_name(c, gen, a.ptr, a.len), `role ${role}`));
    }
    _logName(gen) {
      return this._text((M, c) => M._sstw_log_name(c, gen), 'log name');
    }

    async _sync(name, create = false) {
      const fh = await this.dir.getFileHandle(name, { create });
      return fh.createSyncAccessHandle();
    }

    async _remove(name) {
      try { await this.dir.removeEntry(name); } catch (e) { /* best-effort */ }
    }

    /** Size and streamed CRC-32 of a stored file. */
    async _crcOfFile(name) {
      const h = await this._sync(name);
      try {
        const size = h.getSize();
        const CH = 65536;
        const buf = new Uint8Array(Math.min(CH, size) || 1);
        let crc = 0;
        for (let at = 0; at < size; at += CH) {
          const n = Math.min(CH, size - at);
          const view = buf.subarray(0, n);
          h.read(view, { at });
          crc = crc32(view, crc);
        }
        return { size, crc };
      } finally {
        await h.close();
      }
    }

    /** Every name in the directory, NUL-separated, as C consumes it. */
    async _listing() {
      const names = [];
      for await (const [name] of this.dir.entries()) names.push(name);
      return encoder.encode(names.length ? names.join('\0') + '\0' : '');
    }

    /** A file's length, or -1 if it cannot be opened at all. */
    async _sizeOf(name) {
      try {
        const h = await this._sync(name);
        try { return h.getSize(); } finally { await h.close(); }
      } catch (e) { return -1; }
    }

    /**
     * Scan the directory, adopt the newest valid generation, and sweep the
     * files of every other generation -- crashed attempts and superseded
     * snapshots alike. Log files are untouched (see pruneLogs).
     *
     * The body is snapstore.h's two-beat trampoline: C decides which
     * generations exist and in what order to try them, this side does the
     * awaiting. Every rule about what makes a generation adoptable -- the
     * manifest's CRC, its shape, whether its files are present at their
     * recorded lengths -- is in sst_try_manifest/sst_confirm, so a host
     * that cannot open files asynchronously runs the identical policy
     * through a loop that never suspends.
     */
    async open() {
      if (this.isOpen) throw new Error('SnapshotStore is already open');
      await ready();
      const M = requireModule();

      const p = allocStr(M, this.prefix);
      try {
        this._ctx = M._sstw_new(p.ptr, p.len);
      } finally { p.free(); }
      if (!this._ctx) throw codeError(-1, 'sstw_new');

      const listing = await this._listing();
      this._call((MM, c) => withBytes(MM, listing, (ptr, len) => MM._sstw_scan(c, ptr, len)), 'scan');

      const count = M._sstw_candidate_count(this._ctx);
      for (let i = 0; i < count; i++) {
        const manifestName = this._text((MM, c) => MM._sstw_candidate_manifest(c, i), 'candidate');
        let bytes;
        try {
          const h = await this._sync(manifestName);
          bytes = new Uint8Array(h.getSize());
          if (bytes.length) h.read(bytes, { at: 0 });
          await h.close();
        } catch (e) { continue; }   // vanished under us: try the next

        const MM = requireModule();
        if (withBytes(MM, bytes, (ptr, len) => MM._sstw_try_manifest(this._ctx, i, ptr, len)) !== 0) continue;

        // Beat two: only this candidate's files get opened.
        const pending = MM._sstw_pending_count(this._ctx);
        const sizes = new Float64Array(pending);
        for (let k = 0; k < pending; k++) {
          sizes[k] = await this._sizeOf(this._text((M3, c) => M3._sstw_pending_name(c, k), 'pending'));
        }
        const M4 = requireModule();
        const sp = M4._malloc(pending * 8 || 1);
        let ok;
        try {
          if (pending) M4.HEAPU8.set(new Uint8Array(sizes.buffer), sp);
          ok = M4._sstw_confirm(this._ctx, sp, pending) === 0;
        } finally { M4._free(sp); }
        if (ok) break;
      }

      for (const name of this._names((MM, c) => MM._sstw_sweep_plan(c), 'sweep')) {
        await this._remove(name);
      }

      this._latest = this._readLatest();
      this.isOpen = true;
    }

    _readLatest() {
      const bytes = this._call((M, c) => M._sstw_latest(c), 'latest');
      return bytes.length ? decode(bytes) : null;
    }

    /** The adopted snapshot: { gen, lastIncludedIndex, lastIncludedTerm,
     * config, files: [{ role, name, size, crc }] }, or null if none. */
    get latest() {
      return this._latest;
    }

    /**
     * The C store behind this one (snapstore.h's `sst`), for another C
     * component in the same module that takes one -- a Raft node serving
     * and receiving snapshot installs is the reason it is reachable
     * (raft_node.h's rn_set_snapstore).
     *
     * BORROWED, and deliberately this store rather than a second one over
     * the same prefix: `latest` moves when an install commits, and two
     * stores scanning one directory would be two answers to "which
     * generation is live". Whoever borrows it must be finished before
     * close(). Public for the same reason EntryLog.ctx is.
     */
    get storeCtx() {
      this._ensureOpen();
      return requireModule()._sstw_store(this._ctx);
    }

    _ensureOpen() {
      if (!this.isOpen) throw new Error('SnapshotStore is not open');
    }

    /** Release the C store. Idempotent; the store is unusable afterwards. */
    close() {
      if (this._ctx) {
        requireModule()._sstw_free(this._ctx);
        this._ctx = 0;
      }
      this.isOpen = false;
    }

    /**
     * Start writing a new generation. Write each structure into
     * tx.createFile(role) (e.g. via compact(), which closes the handle),
     * then tx.commit({ lastIncludedIndex, lastIncludedTerm, config }) — the
     * commit point. Nothing is visible (and open() sweeps the files) until
     * commit returns; abort() deletes them eagerly.
     */
    async begin() {
      this._ensureOpen();
      const M = requireModule();
      const gen = M._sstw_next_gen(this._ctx);
      const created = new Map();   // role -> name
      const store = this;
      return {
        gen,
        async createFile(role) {
          // sst_data_name rejects a role that cannot be a filename; the
          // duplicate check is here because a Map is where duplicates are
          // visible, and C sees only one role at a time until commit.
          if (created.has(role)) throw new Error(`Duplicate snapshot role: ${role}`);
          const name = store._dataName(gen, role);
          created.set(role, name);
          return store._sync(name, true);
        },
        async commit({ lastIncludedIndex, lastIncludedTerm, config = null }) {
          const files = [];
          for (const [role, name] of created) {
            const { size, crc } = await store._crcOfFile(name);
            files.push({ role, name, size, crc });
          }
          // The record AND its trailing CRC come from C: writing this
          // file is the commit point, so what counts as a valid manifest
          // must have exactly one definition (snapstore.h).
          const out = store._call((MM, c) => {
            const f = encode(files);
            const cfg = config === null ? new Uint8Array(0) : encode(config);
            const fp = MM._malloc(f.length || 1);
            const cp = MM._malloc(cfg.length || 1);
            try {
              if (f.length) MM.HEAPU8.set(f, fp);
              if (cfg.length) MM.HEAPU8.set(cfg, cp);
              return MM._sstw_manifest_encode(c, lastIncludedIndex, lastIncludedTerm,
                                              cp, cfg.length, fp, f.length);
            } finally { MM._free(cp); MM._free(fp); }
          }, 'manifest');

          const h = await store._sync(store._manifestName(gen), true);
          h.truncate(0);
          h.write(out, { at: 0 });
          h.flush();
          await h.close();

          // Only now is the predecessor obsolete -- C hands back its
          // files, so the sweep rule is the one open() uses rather than a
          // second reading of the same idea here.
          const sweep = store._names(
            (MM, c) => withBytes(MM, out, (ptr, len) => MM._sstw_adopt_committed(c, gen, ptr, len)),
            'commit'
          );
          store._latest = store._readLatest();
          for (const name of sweep) await store._remove(name);
          return store._latest;
        },
        async abort() {
          for (const name of created.values()) await store._remove(name);
          created.clear();
        }
      };
    }

    _fileForRole(role) {
      this._ensureOpen();
      if (!this._latest) throw new Error('No snapshot to open');
      const f = this._latest.files.find((x) => x.role === role);
      if (!f) throw new Error(`No snapshot file for role: ${role}`);
      return f;
    }

    /** Open a role file of the adopted snapshot for reading (e.g. to serve
     * InstallSnapshot chunks). Caller closes the handle. */
    async openFile(role) {
      return this._sync(this._fileForRole(role).name);
    }

    /** Stream-copy a role file of the adopted snapshot into `destSyncHandle`
     * (the database's live filename after an install). Closes the
     * destination handle; verifies the copied bytes against the manifest CRC. */
    async copyFile(role, destSyncHandle) {
      const f = this._fileForRole(role);
      const src = await this._sync(f.name);
      try {
        const CH = 65536;
        const buf = new Uint8Array(Math.min(CH, f.size) || 1);
        let crc = 0;
        destSyncHandle.truncate(0);
        for (let at = 0; at < f.size; at += CH) {
          const n = Math.min(CH, f.size - at);
          const view = buf.subarray(0, n);
          src.read(view, { at });
          crc = crc32(view, crc);
          destSyncHandle.write(view, { at });
        }
        this.checkFiles([{ role, size: f.size, crc }], [f]);
        destSyncHandle.flush();
      } finally {
        await src.close();
        await destSyncHandle.close();
      }
    }

    /**
     * Check measured (size, crc) pairs against a manifest -- the adopted
     * one by default, or a leader's during an install.
     *
     * Every caller that decides whether a set of bytes IS a snapshot goes
     * through here: verify() below, copyFile above, and the replicated
     * install path in src/db-replicated.js. They each used to carry their
     * own copy of the comparison, which is three chances to disagree
     * about whether a follower may adopt what it was sent.
     *
     * `actual` is [{ role, size, crc }]. `expected` defaults to the
     * adopted manifest's files; an install passes the leader's, and
     * copyFile passes the single entry it just copied. Every expected
     * role must appear in `actual` and match on both fields -- absence
     * counts as a mismatch, because an install missing a structure has
     * not received the snapshot.
     *
     * Throws naming the first role that does not match; returns true
     * otherwise.
     */
    checkFiles(actual, expected = null) {
      this._ensureOpen();
      return snapshotCheckFiles(actual, expected ?? this._latest?.files ?? []);
    }

    /** Re-checksum every file of the adopted snapshot against the manifest.
     * Returns true, or throws naming the corrupt file. O(total bytes). */
    async verify() {
      this._ensureOpen();
      if (!this._latest) throw new Error('No snapshot to verify');
      const actual = [];
      for (const f of this._latest.files) {
        // A file that will not open at all is reported as a mismatch by
        // the same rule everything else is, rather than as a raw I/O
        // exception the install path would have to special-case.
        try {
          const { size, crc } = await this._crcOfFile(f.name);
          actual.push({ role: f.role, size, crc });
        } catch (e) { actual.push({ role: f.role, size: -1, crc: 0 }); }
      }
      return this.checkFiles(actual);
    }

    /**
     * Fresh file for the entry log compacted at the adopted snapshot's
     * boundary: <prefix>-log-<gen>.bj. Returns { name, handle }; write via
     * EntryLog.compact (which closes the handle), then pruneLogs(name).
     */
    async createLogFile() {
      this._ensureOpen();
      if (!this._latest) throw new Error('No snapshot to pair a log with');
      const name = this._logName(this._latest.gen);
      return { name, handle: await this._sync(name, true) };
    }

    /** Existing entry-log files, newest generation first. Try each with
     * EntryLog.open and adopt the first that succeeds (a torn newest file
     * fails to open; its predecessor is only deleted after a successor is
     * durable). */
    async logCandidates() {
      this._ensureOpen();
      const listing = await this._listing();
      return this._names(
        (M, c) => withBytes(M, listing, (ptr, len) => M._sstw_log_candidates(c, ptr, len)),
        'logCandidates'
      );
    }

    /** Delete every entry-log file except `keepName` (call once the newly
     * compacted log is durable). Best-effort, like all sweeps here. */
    async pruneLogs(keepName) {
      this._ensureOpen();
      const listing = await this._listing();
      const doomed = this._withStr(keepName ?? '', (M, k) => this._names(
        (MM, c) => withBytes(MM, listing, (ptr, len) =>
          MM._sstw_prune_logs_plan(c, ptr, len, k.ptr, k.len)),
        'pruneLogs'
      ));
      for (const name of doomed) await this._remove(name);
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

    /**
     * Last replicated-log index applied to this index: the minimum across its
     * three trees (each records its own in its commit metadata; the journal
     * keeps them consistent, but a recovery rewind can land them on different
     * commits — the minimum is the safe replay point).
     */
    appliedIndex() {
      this._ensureOpen();
      return Math.min(
        this.index.appliedIndex(),
        this.documentTerms.appliedIndex(),
        this.documentLengths.appliedIndex()
      );
    }

    /** Stage the log index about to be applied on all three trees; each
     * tree's next commit persists it. Call before add/remove/clear. */
    setAppliedIndex(index) {
      this._ensureOpen();
      this.index.setAppliedIndex(index);
      this.documentTerms.setAppliedIndex(index);
      this.documentLengths.setAppliedIndex(index);
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


  return {
    orderedKey, compositeKey, compositeUpperBound, BPlusTree,
    haversineDistance, RTree, TextLog, TiledTextLog, ENTRY_TYPE,
    EntryLog, ENTRYLOG_TYPE, SnapshotStore, crc32, snapshotCheckFiles, TextIndex,
    stemmer, createPatch, unifiedDiff, applyPatch, createDelta, applyDelta
  };
}
