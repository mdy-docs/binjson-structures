// Web Worker for handling OPFS file operations with sync access handles
// This worker handles file operations that require FileSystemSyncAccessHandle

// All data structures are backed by the combined WASM module, loaded once
// via ../wasm/binjson-structures-wasm.js (which pulls in
// ../lib/binjson-structures.wasm.mjs). getFileHandle comes from this
// package's own nested third_party/binjson copy -- the same copy
// binjson-structures-wasm.js itself is built against.
import {
  ready,
  ObjectId,
  BPlusTree,
  RTree,
  TextIndex,
  TextLog
} from '../wasm/binjson-structures-wasm.js';
import { getFileHandle } from '../third_party/binjson/js/binjson.js';

// Resolve the OPFS root, with a clear error when it isn't available. OPFS is
// only exposed in a secure context (https or http://localhost) and in browsers
// that support it (Chrome/Edge/Opera 102+, Safari 16.4+).
async function getRootDir() {
  if (typeof navigator === 'undefined' || !navigator.storage || !navigator.storage.getDirectory) {
    throw new Error(
      'OPFS is not available in this context. Serve over https or http://localhost ' +
      'and use Chrome/Edge/Opera 102+ or Safari 16.4+.'
    );
  }
  return navigator.storage.getDirectory();
}

// A TextIndex is backed by three B+ tree files sharing a base name. Open all
// three and wrap them in a TextIndex (create the files when `create` is true).
async function openTextIndex(dirHandle, baseName, create) {
  const roles = { index: '-terms.bj', documentTerms: '-documents.bj', documentLengths: '-lengths.bj' };
  const trees = {};
  for (const [role, suffix] of Object.entries(roles)) {
    const fh = await getFileHandle(dirHandle, `${baseName}${suffix}`, { create });
    trees[role] = new BPlusTree(await fh.createSyncAccessHandle(), 16);
  }
  const index = new TextIndex({ trees });
  await index.open();
  return index;
}

// Handle messages from the main thread
self.addEventListener('message', async (event) => {
  const { id, operation, filename, data } = event.data;

  try {
    // Ensure the WASM module is instantiated before any tree operation.
    // Idempotent and cached, so this is cheap per message.
    await ready();

    let result;

    switch (operation) {
      case 'delete': {
        const dirHandle = await getRootDir();
        try {
          await dirHandle.removeEntry(filename);
        } catch (err) {
          if (err.name !== 'NotFoundError') {
            throw err;
          }
        }
        result = { success: true };
        break;
      }

      case 'bplustree-create': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: true });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const order = data?.order || 3;
        const tree = new BPlusTree(syncHandle, order);
        await tree.open();
        await tree.close();

        result = { success: true };
        break;
      }

      case 'bplustree-add': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { key, value } = data;
        const tree = new BPlusTree(syncHandle);
        await tree.open();
        await tree.add(key, value);
        await tree.close();

        result = { success: true };
        break;
      }

      case 'bplustree-toArray': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const tree = new BPlusTree(syncHandle);
        await tree.open();
        const array = await tree.toArray();
        await tree.close();

        result = array;
        break;
      }

      case 'bplustree-search': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { key } = data;
        const tree = new BPlusTree(syncHandle);
        await tree.open();
        const value = await tree.search(key);
        await tree.close();

        // `undefined` means the key is absent; distinguish it from a stored null.
        result = { found: value !== undefined, value: value === undefined ? null : value };
        break;
      }

      case 'bplustree-delete': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { key } = data;
        const tree = new BPlusTree(syncHandle);
        await tree.open();
        const existed = (await tree.search(key)) !== undefined;
        await tree.delete(key);
        await tree.close();

        result = { deleted: existed };
        break;
      }

      case 'bplustree-range': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { min, max } = data;
        const tree = new BPlusTree(syncHandle);
        await tree.open();
        const entries = await tree.rangeSearch(min, max);
        await tree.close();

        result = entries;
        break;
      }

      case 'bplustree-info': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const tree = new BPlusTree(syncHandle);
        await tree.open();
        const info = { size: tree.size(), height: tree.getHeight(), order: tree.order };
        await tree.close();

        result = info;
        break;
      }

      case 'bplustree-compact': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { compactFilename } = data;

        // Create sync handle for destination file
        const destFileHandle = await getFileHandle(dirHandle, compactFilename, { create: true });
        const destSyncHandle = await destFileHandle.createSyncAccessHandle();

        const tree = new BPlusTree(syncHandle);
        await tree.open();
        const stats = await tree.compact(destSyncHandle);
        await tree.close();

        result = stats;
        break;
      }

      case 'rtree-create': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: true });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const order = data?.order || 4;
        const tree = new RTree(syncHandle, order);
        await tree.open();
        await tree.close();

        result = { success: true };
        break;
      }

      case 'rtree-insert': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { lat, lng, objectId } = data;
        // Convert string to ObjectId if needed
        const oid = typeof objectId === 'string' ? new ObjectId(objectId) : objectId;

        const tree = new RTree(syncHandle);
        await tree.open();
        await tree.insert(lat, lng, oid);
        await tree.close();

        result = { success: true };
        break;
      }

      case 'rtree-searchRadius': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { lat, lng, radiusKm } = data;
        const tree = new RTree(syncHandle);
        await tree.open();
        const results = await tree.searchRadius(lat, lng, radiusKm);
        await tree.close();

        result = results;
        break;
      }

      case 'rtree-searchBBox': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { bbox } = data;
        const tree = new RTree(syncHandle);
        await tree.open();
        const results = await tree.searchBBox(bbox);
        await tree.close();

        result = results;
        break;
      }

      case 'rtree-compact': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { compactFilename } = data;

        // Create sync handle for destination file
        const destFileHandle = await getFileHandle(dirHandle, compactFilename, { create: true });
        const destSyncHandle = await destFileHandle.createSyncAccessHandle();

        const tree = new RTree(syncHandle);
        await tree.open();
        const stats = await tree.compact(destSyncHandle);
        await tree.close();

        result = stats;
        break;
      }

      case 'rtree-remove': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { objectId } = data;
        const oid = typeof objectId === 'string' ? new ObjectId(objectId) : objectId;
        const tree = new RTree(syncHandle);
        await tree.open();
        const removed = await tree.remove(oid);
        await tree.close();

        result = { removed };
        break;
      }

      case 'rtree-clear': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const tree = new RTree(syncHandle);
        await tree.open();
        const count = tree.size();
        await tree.clear();
        await tree.close();

        result = { cleared: count };
        break;
      }

      case 'rtree-list': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const tree = new RTree(syncHandle);
        await tree.open();
        // A whole-world bounding box returns every point.
        const points = await tree.searchBBox({ minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 });
        await tree.close();

        result = points;
        break;
      }

      case 'rtree-info': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const tree = new RTree(syncHandle);
        await tree.open();
        const info = { size: tree.size(), maxEntries: tree.maxEntries };
        await tree.close();

        result = info;
        break;
      }

      case 'textindex-create': {
        const dirHandle = await getRootDir();
        const { baseName, order } = data;

        // Create three BPlusTree files for TextIndex
        const indexFile = await getFileHandle(dirHandle, `${baseName}-terms.bj`, { create: true });
        const indexHandle = await indexFile.createSyncAccessHandle();

        const docTermsFile = await getFileHandle(dirHandle, `${baseName}-documents.bj`, { create: true });
        const docTermsHandle = await docTermsFile.createSyncAccessHandle();

        const docLengthsFile = await getFileHandle(dirHandle, `${baseName}-lengths.bj`, { create: true });
        const docLengthsHandle = await docLengthsFile.createSyncAccessHandle();

        // Create three BPlusTree instances
        const indexTree = new BPlusTree(indexHandle, order || 16);
        const docTermsTree = new BPlusTree(docTermsHandle, order || 16);
        const docLengthsTree = new BPlusTree(docLengthsHandle, order || 16);

        await indexTree.open();
        await docTermsTree.open();
        await docLengthsTree.open();

        await indexTree.close();
        await docTermsTree.close();
        await docLengthsTree.close();

        result = { success: true };
        break;
      }

      case 'textindex-add': {
        const dirHandle = await getRootDir();
        const { baseName, docId, text } = data;

        // Open the three BPlusTree files
        const indexFile = await getFileHandle(dirHandle, `${baseName}-terms.bj`, { create: false });
        const indexHandle = await indexFile.createSyncAccessHandle();
        const indexTree = new BPlusTree(indexHandle);

        const docTermsFile = await getFileHandle(dirHandle, `${baseName}-documents.bj`, { create: false });
        const docTermsHandle = await docTermsFile.createSyncAccessHandle();
        const docTermsTree = new BPlusTree(docTermsHandle);

        const docLengthsFile = await getFileHandle(dirHandle, `${baseName}-lengths.bj`, { create: false });
        const docLengthsHandle = await docLengthsFile.createSyncAccessHandle();
        const docLengthsTree = new BPlusTree(docLengthsHandle);

        // Create TextIndex with the trees
        const textIndex = new TextIndex({
          trees: {
            index: indexTree,
            documentTerms: docTermsTree,
            documentLengths: docLengthsTree
          }
        });

        await textIndex.open();
        await textIndex.add(docId, text);
        await textIndex.close();

        result = { success: true };
        break;
      }

      case 'textindex-query': {
        const dirHandle = await getRootDir();
        const { baseName, queryText, options } = data;

        // Open the three BPlusTree files
        const indexFile = await getFileHandle(dirHandle, `${baseName}-terms.bj`, { create: false });
        const indexHandle = await indexFile.createSyncAccessHandle();
        const indexTree = new BPlusTree(indexHandle);

        const docTermsFile = await getFileHandle(dirHandle, `${baseName}-documents.bj`, { create: false });
        const docTermsHandle = await docTermsFile.createSyncAccessHandle();
        const docTermsTree = new BPlusTree(docTermsHandle);

        const docLengthsFile = await getFileHandle(dirHandle, `${baseName}-lengths.bj`, { create: false });
        const docLengthsHandle = await docLengthsFile.createSyncAccessHandle();
        const docLengthsTree = new BPlusTree(docLengthsHandle);

        // Create TextIndex with the trees
        const textIndex = new TextIndex({
          trees: {
            index: indexTree,
            documentTerms: docTermsTree,
            documentLengths: docLengthsTree
          }
        });

        await textIndex.open();
        const results = await textIndex.query(queryText, options || {});
        await textIndex.close();

        result = results;
        break;
      }

      case 'textindex-compact': {
        const dirHandle = await getRootDir();
        const { baseName, compactBaseName } = data;

        // Open source trees
        const indexFile = await getFileHandle(dirHandle, `${baseName}-terms.bj`, { create: false });
        const indexHandle = await indexFile.createSyncAccessHandle();
        const indexTree = new BPlusTree(indexHandle);

        const docTermsFile = await getFileHandle(dirHandle, `${baseName}-documents.bj`, { create: false });
        const docTermsHandle = await docTermsFile.createSyncAccessHandle();
        const docTermsTree = new BPlusTree(docTermsHandle);

        const docLengthsFile = await getFileHandle(dirHandle, `${baseName}-lengths.bj`, { create: false });
        const docLengthsHandle = await docLengthsFile.createSyncAccessHandle();
        const docLengthsTree = new BPlusTree(docLengthsHandle);

        // Create destination BPlusTree instances
        const compactIndexFile = await getFileHandle(dirHandle, `${compactBaseName}-terms.bj`, { create: true });
        const compactIndexHandle = await compactIndexFile.createSyncAccessHandle();
        const compactIndexTree = new BPlusTree(compactIndexHandle);

        const compactDocTermsFile = await getFileHandle(dirHandle, `${compactBaseName}-documents.bj`, { create: true });
        const compactDocTermsHandle = await compactDocTermsFile.createSyncAccessHandle();
        const compactDocTermsTree = new BPlusTree(compactDocTermsHandle);

        const compactDocLengthsFile = await getFileHandle(dirHandle, `${compactBaseName}-lengths.bj`, { create: true });
        const compactDocLengthsHandle = await compactDocLengthsFile.createSyncAccessHandle();
        const compactDocLengthsTree = new BPlusTree(compactDocLengthsHandle);

        await compactIndexTree.open();
        await compactDocTermsTree.open();
        await compactDocLengthsTree.open();

        // Create TextIndex with source trees
        const textIndex = new TextIndex({
          trees: {
            index: indexTree,
            documentTerms: docTermsTree,
            documentLengths: docLengthsTree
          }
        });

        await textIndex.open();
        const stats = await textIndex.compact({
          index: compactIndexTree,
          documentTerms: compactDocTermsTree,
          documentLengths: compactDocLengthsTree
        });

        result = stats;
        break;
      }

      case 'textindex-info': {
        const dirHandle = await getRootDir();
        const { baseName } = data;
        const textIndex = await openTextIndex(dirHandle, baseName, false);
        const info = { terms: await textIndex.getTermCount(), documents: await textIndex.getDocumentCount() };
        await textIndex.close();

        result = info;
        break;
      }

      case 'textindex-remove': {
        const dirHandle = await getRootDir();
        const { baseName, docId } = data;
        const textIndex = await openTextIndex(dirHandle, baseName, false);
        const removed = await textIndex.remove(docId);
        await textIndex.close();

        result = { removed };
        break;
      }

      case 'textindex-clear': {
        const dirHandle = await getRootDir();
        const { baseName } = data;
        const textIndex = await openTextIndex(dirHandle, baseName, false);
        const count = await textIndex.getDocumentCount();
        await textIndex.clear();
        await textIndex.close();

        result = { cleared: count };
        break;
      }

      case 'textindex-list': {
        const dirHandle = await getRootDir();
        const { baseName } = data;
        const textIndex = await openTextIndex(dirHandle, baseName, false);
        // Document ids are the keys of the documentTerms tree.
        const ids = textIndex.documentTerms.toArray().map((e) => e.key);
        await textIndex.close();

        result = ids;
        break;
      }

      case 'textlog-create': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: true });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const diffsPerSnapshot = data?.diffsPerSnapshot || 10;
        const log = new TextLog(syncHandle, diffsPerSnapshot);
        await log.open();
        await log.close();

        result = { success: true };
        break;
      }

      case 'textlog-add': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: true });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { text, diffsPerSnapshot } = data;
        const log = new TextLog(syncHandle, diffsPerSnapshot || 10);
        await log.open();
        const version = await log.addVersion(text);
        await log.close();

        result = { version };
        break;
      }

      case 'textlog-get': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { version } = data;
        const log = new TextLog(syncHandle);
        await log.open();
        const text = await log.getVersion(version);
        await log.close();

        result = { version, text };
        break;
      }

      case 'textlog-diff': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const { from, to } = data;
        const log = new TextLog(syncHandle);
        await log.open();
        const diff = await log.getDiff(from, to);
        await log.close();

        result = { from, to, diff };
        break;
      }

      case 'textlog-info': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const log = new TextLog(syncHandle);
        await log.open();
        const info = { versions: log.getCurrentVersion(), diffsPerSnapshot: log.diffsPerSnapshot };
        await log.close();

        result = info;
        break;
      }

      case 'textlog-list': {
        const dirHandle = await getRootDir();
        const fileHandle = await getFileHandle(dirHandle, filename, { create: false });
        const syncHandle = await fileHandle.createSyncAccessHandle();

        const log = new TextLog(syncHandle);
        await log.open();
        const current = log.getCurrentVersion();
        const versions = [];
        for (let v = 1; v <= current; v++) {
          versions.push({ version: v, hash: await log.getVersionHash(v) });
        }
        await log.close();

        result = versions;
        break;
      }

      default:
        throw new Error(`Unknown operation: ${operation}`);
    }

    self.postMessage({ id, result });
  } catch (error) {
    self.postMessage({ id, error: error.message });
  }
});
