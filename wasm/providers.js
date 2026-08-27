/*
 * providers.js — named-file storage, the JS half of bjns.
 *
 * bjio.h abstracts reading and writing an already-open file; bjns.h
 * abstracts naming one — "one directory-scoped file namespace", in its own
 * words. A storage provider is that same abstraction seen from JavaScript,
 * and the two were arrived at independently:
 *
 *     openFile(name, { create })   -> a sync access handle
 *     deleteFile(name)
 *     listFiles()                  -> string[]   (optional)
 *     subProvider(name)            -> a nested, isolated scope
 *
 * Four verbs, deliberately not a VFS, matching bjns' four exactly. Nothing
 * here knows what is stored in the files, which is why these belong beside
 * bjns and hostio rather than inside any one consumer: a database and a
 * message broker want the same three answers to "where do the bytes live".
 *
 * The async/sync question answers itself, and is worth stating because it
 * looks like a contradiction. Opening is asynchronous here — OPFS's
 * getFileHandle() and createSyncAccessHandle() are both promises — while
 * bjns requires open() to be synchronous. That is the arrangement bjns was
 * designed around rather than a conflict with it: *C plans, the host opens,
 * C executes*. The host opens, in JS, before the call; C then works
 * synchronously over the handle it was given.
 *
 * Parameterised rather than importing, for the same reason
 * structures-core.js takes its codec as an argument: a consumer that links
 * one binjson checkout must not be made to resolve another. Pass in
 * binjson's MemoryHandle and its two OPFS helpers, from wherever your build
 * gets them.
 */

/**
 * @param {object} deps
 * @param {Function} deps.MemoryHandle    binjson's in-memory sync handle
 * @param {Function} deps.getFileHandle   (dirHandle, name, { create }) -> FileSystemFileHandle
 * @param {Function} deps.deleteFile      (dirHandle, name) -> void
 */
export function bindProviders({ MemoryHandle, getFileHandle, deleteFile }) {
  /**
   * In-memory named files: handles persist for the process lifetime, since
   * MemoryHandle.close() is a no-op and the data outlives whatever opened
   * it. For tests, and for embeddings that genuinely do not want
   * durability — a dev server, a broker whose messages are not meant to
   * survive the tab.
   */
  class MemoryStorageProvider {
    constructor() {
      this._files = new Map();
      this._children = new Map();
    }

    async openFile(name, { create = false } = {}) {
      let handle = this._files.get(name);
      if (!handle) {
        if (!create) throw new Error(`File not found: ${name}`);
        handle = new MemoryHandle();
        this._files.set(name, handle);
      }
      return handle;
    }

    async deleteFile(name) {
      this._files.delete(name);
    }

    /** Every name currently stored. Optional on a provider; having it is
     * what lets a consumer sweep files orphaned by a crashed compaction. */
    async listFiles() {
      return [...this._files.keys()];
    }

    /** A named, isolated scope nested under this one — the equivalent of
     * OPFSStorageProvider's real subdirectory, backed by its own file map.
     * Cached, so repeat calls with one name return one instance. */
    async subProvider(name) {
      let child = this._children.get(name);
      if (!child) {
        child = new MemoryStorageProvider();
        this._children.set(name, child);
      }
      return child;
    }
  }

  /**
   * OPFS-backed named files, rooted at a directory handle — the OPFS root
   * by default, resolved lazily so construction works outside a worker.
   * Real synchronous handles and a real flush, so this is the durable
   * option in a browser.
   */
  class OPFSStorageProvider {
    constructor(dirHandle) {
      this._dirHandle = dirHandle || null;
    }

    async _dir() {
      if (!this._dirHandle) this._dirHandle = await navigator.storage.getDirectory();
      return this._dirHandle;
    }

    async openFile(name, { create = false } = {}) {
      const dir = await this._dir();
      const fileHandle = await getFileHandle(dir, name, { create });
      return fileHandle.createSyncAccessHandle();
    }

    async deleteFile(name) {
      await deleteFile(await this._dir(), name);
    }

    async listFiles() {
      const names = [];
      for await (const [name, handle] of (await this._dir()).entries()) {
        if (handle.kind === 'file') names.push(name);
      }
      return names;
    }

    /** A real subdirectory, created if needed, as its own provider. */
    async subProvider(name) {
      const dir = await this._dir();
      const childDir = await dir.getDirectoryHandle(name, { create: true });
      return new OPFSStorageProvider(childDir);
    }
  }

  return { MemoryStorageProvider, OPFSStorageProvider };
}

/**
 * The Node provider, separately, because it is the only one that needs
 * `node:fs` — importing it would make this module unloadable in a browser,
 * so a caller that wants it passes the module in.
 *
 * The handle it returns duck-types FileSystemSyncAccessHandle down to
 * `read(buf, { at })`, which is what lets the C side treat all three
 * providers identically.
 *
 * @param {object} deps
 * @param {object} deps.fs    node:fs
 * @param {object} deps.path  node:path
 */
export function bindNodeProvider({ fs, path }) {
  class NodeFSSyncHandle {
    constructor(fd) { this._fd = fd; }
    getSize() { return fs.fstatSync(this._fd).size; }
    read(buffer, { at } = {}) {
      return fs.readSync(this._fd, buffer, 0, buffer.length, at ?? 0);
    }
    write(buffer, { at } = {}) {
      return fs.writeSync(this._fd, buffer, 0, buffer.length, at ?? 0);
    }
    truncate(len) { fs.ftruncateSync(this._fd, len); }
    flush() { fs.fsyncSync(this._fd); }
    close() { fs.closeSync(this._fd); }
  }

  class NodeFSStorageProvider {
    constructor(dir) { this._dir = dir; }

    async openFile(name, { create = false } = {}) {
      fs.mkdirSync(this._dir, { recursive: true });
      const file = path.join(this._dir, name);
      const fd = fs.openSync(file, create ? 'a+' : 'r+');
      return new NodeFSSyncHandle(fd);
    }

    async deleteFile(name) {
      try { fs.unlinkSync(path.join(this._dir, name)); }
      catch (err) { if (err.code !== 'ENOENT') throw err; }
    }

    async listFiles() {
      try { return fs.readdirSync(this._dir); }
      catch (err) { if (err.code === 'ENOENT') return []; throw err; }
    }

    async subProvider(name) {
      return new NodeFSStorageProvider(path.join(this._dir, name));
    }
  }

  return { NodeFSStorageProvider, NodeFSSyncHandle };
}
