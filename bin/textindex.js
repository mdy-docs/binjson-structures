#!/usr/bin/env node
import { TextIndex, BPlusTree, ready, ObjectId, Pointer } from '../wasm/binjson-structures-wasm.js';
import { getFileHandle } from '../third_party/binjson/js/binjson.js';

// Set up node-opfs for Node.js environment
try {
  const nodeOpfs = await import('node-opfs');
  if (nodeOpfs.navigator && typeof global !== 'undefined') {
    Object.defineProperty(global, 'navigator', {
      value: nodeOpfs.navigator,
      writable: true,
      configurable: true
    });
  }
} catch (e) {
  console.error('Error: node-opfs is required to run this tool in Node.js');
  console.error('Install it with: npm install node-opfs');
  process.exit(1);
}

// A TextIndex is backed by three B+ tree files sharing a base name.
const FILE_ROLES = {
  index: '-terms.bj',
  documentTerms: '-documents.bj',
  documentLengths: '-lengths.bj'
};

function indexFiles(base) {
  return Object.fromEntries(
    Object.entries(FILE_ROLES).map(([role, suffix]) => [role, `${base}${suffix}`])
  );
}

function usage() {
  console.error(`Usage: textindex <name> <command> [args] [options]

A full-text index over documents. <name> is a base name; the index is stored in
three files: <name>-terms.bj, <name>-documents.bj, and <name>-lengths.bj.

Viewing:
  list                  List every indexed document id (default)
  query <text>          Rank documents matching <text> (BM25 scored)
  info                  Print term count, document count, and file sizes

Editing:
  add <docId> <text>    Index a document under <docId> (creates the index if
                        needed); re-adding the same id replaces it
  remove <docId>        Remove a document from the index
  clear                 Remove every document
  compact [destName]    Rewrite the index files, dropping stale append history
                        (in place, or under destName if given)

Options:
  --all             query: require every term to be present (AND); prints ids
  --ids             query: print only document ids, without scores
  --order <n>       B+ tree order for newly created files (default 16, min 3)
  -h, --help        Show this help`);
  process.exit(1);
}

function formatValue(value) {
  const indentUnit = '  ';
  const render = (val, depth) => {
    const pad = indentUnit.repeat(depth);
    const nextPad = indentUnit.repeat(depth + 1);

    if (val === null) return 'null';
    if (val === undefined) return 'undefined';
    if (typeof val === 'number' || typeof val === 'boolean') return String(val);
    if (typeof val === 'string') return JSON.stringify(val);

    if (val instanceof Pointer) return `Pointer(${val.valueOf()})`;
    if (val instanceof ObjectId) {
      return `ObjectId(${val.toHexString ? val.toHexString() : val.toString()})`;
    }
    if (val instanceof Date) return `Date(${val.toISOString()})`;

    if (Array.isArray(val)) {
      if (val.length === 0) return '[]';
      const inner = val.map(item => `${nextPad}${render(item, depth + 1)}`).join('\n');
      return `[\n${inner}\n${pad}]`;
    }

    if (typeof val === 'object') {
      const entries = Object.entries(val);
      if (entries.length === 0) return '{}';
      const inner = entries
        .map(([k, v]) => `${nextPad}${k}: ${render(v, depth + 1)}`)
        .join('\n');
      return `{\n${inner}\n${pad}}`;
    }

    return JSON.stringify(val);
  };

  return render(value, 0);
}

function parseArgs(argv) {
  const opts = { order: 16, all: false, ids: false };
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      usage();
    } else if (arg === '--all') {
      opts.all = true;
    } else if (arg === '--ids') {
      opts.ids = true;
    } else if (arg === '--order') {
      const n = Number(argv[++i]);
      if (!Number.isInteger(n) || n < 3) {
        console.error('Error: --order must be an integer >= 3');
        process.exit(1);
      }
      opts.order = n;
    } else if (arg.startsWith('--order=')) {
      const n = Number(arg.slice('--order='.length));
      if (!Number.isInteger(n) || n < 3) {
        console.error('Error: --order must be an integer >= 3');
        process.exit(1);
      }
      opts.order = n;
    } else {
      positional.push(arg);
    }
  }
  return { opts, positional };
}

async function closeHandles(handles) {
  for (const h of handles) {
    try { await h.close(); } catch { /* already closed */ }
  }
}

// Open the three backing trees and wrap them in a TextIndex.
async function openIndex(base, order, { create }) {
  const rootDirHandle = await navigator.storage.getDirectory();
  const files = indexFiles(base);
  const trees = {};
  const handles = [];
  try {
    for (const [role, name] of Object.entries(files)) {
      const fileHandle = await getFileHandle(rootDirHandle, name, { create });
      const syncHandle = await fileHandle.createSyncAccessHandle();
      handles.push(syncHandle);
      trees[role] = new BPlusTree(syncHandle, order);
    }
  } catch (err) {
    await closeHandles(handles);
    throw err;
  }
  const index = new TextIndex({ order, trees });
  await index.open();
  return { index, handles };
}

async function main() {
  const { opts, positional } = parseArgs(process.argv.slice(2));

  const base = positional[0];
  if (!base) usage();

  const command = (positional[1] || 'list').toLowerCase();
  const args = positional.slice(2);

  // Only `add` creates missing files; the rest require the index to exist.
  const creating = command === 'add';

  await ready();

  let index;
  let handles = [];
  try {
    ({ index, handles } = await openIndex(base, opts.order, { create: creating }));

    switch (command) {
      case 'list':
      case 'docs': {
        const entries = index.documentTerms.toArray();
        if (entries.length === 0) {
          console.log('Index has no documents.');
          break;
        }
        for (let i = 0; i < entries.length; i++) {
          const { key, value } = entries[i];
          const terms = Array.isArray(value) ? ` (${value.length} term${value.length === 1 ? '' : 's'})` : '';
          console.log(`${i}: ${formatValue(key)}${terms}`);
        }
        break;
      }

      case 'query':
      case 'search': {
        if (args.length < 1) {
          console.error('Error: query requires <text>');
          process.exit(1);
        }
        const text = args.join(' ');
        if (opts.all) {
          const ids = await index.query(text, { requireAll: true });
          if (ids.length === 0) {
            console.log('No documents matched.');
            process.exitCode = 1;
            break;
          }
          ids.forEach((id, i) => console.log(`${i}: ${formatValue(id)}`));
        } else if (opts.ids) {
          const ids = await index.query(text, { scored: false });
          if (ids.length === 0) {
            console.log('No documents matched.');
            process.exitCode = 1;
            break;
          }
          ids.forEach((id, i) => console.log(`${i}: ${formatValue(id)}`));
        } else {
          const results = await index.query(text, { scored: true });
          if (results.length === 0) {
            console.log('No documents matched.');
            process.exitCode = 1;
            break;
          }
          results.forEach((r, i) => console.log(`${i}: ${formatValue(r.id)} (score: ${r.score.toFixed(4)})`));
        }
        break;
      }

      case 'info':
      case 'stats': {
        const rootDirHandle = await navigator.storage.getDirectory();
        const files = indexFiles(base);
        console.log(`name:      ${base}`);
        console.log(`terms:     ${await index.getTermCount()}`);
        console.log(`documents: ${await index.getDocumentCount()}`);
        for (const [role, name] of Object.entries(files)) {
          const fh = await getFileHandle(rootDirHandle, name, { create: false });
          const size = (await fh.getFile()).size;
          console.log(`  ${name}: ${size} bytes (${role})`);
        }
        break;
      }

      case 'add':
      case 'index': {
        if (args.length < 2) {
          console.error('Error: add requires a <docId> and <text>');
          process.exit(1);
        }
        const docId = args[0];
        const text = args.slice(1).join(' ');
        await index.add(docId, text);
        console.log(`Indexed ${formatValue(docId)} (${await index.getDocumentCount()} document${await index.getDocumentCount() === 1 ? '' : 's'} total).`);
        break;
      }

      case 'remove':
      case 'delete':
      case 'del': {
        if (args.length < 1) {
          console.error('Error: remove requires a <docId>');
          process.exit(1);
        }
        const docId = args[0];
        const removed = await index.remove(docId);
        if (removed) {
          console.log(`Removed ${formatValue(docId)}.`);
        } else {
          console.log(`${formatValue(docId)} not found; nothing removed.`);
          process.exitCode = 1;
        }
        break;
      }

      case 'clear': {
        const count = await index.getDocumentCount();
        await index.clear();
        console.log(`Cleared ${count} document${count === 1 ? '' : 's'}.`);
        break;
      }

      case 'compact': {
        const destBase = args[0] || base;
        const rootDirHandle = await navigator.storage.getDirectory();

        if (destBase === base) {
          // In-place: compact into temp files, then copy them back over the originals.
          const files = indexFiles(base);
          const tmpTrees = {};
          const tmpNames = {};
          for (const role of Object.keys(FILE_ROLES)) {
            const tmpName = `${files[role]}.compact-${Date.now()}.tmp`;
            tmpNames[role] = tmpName;
            const fh = await getFileHandle(rootDirHandle, tmpName, { create: true });
            tmpTrees[role] = { syncAccessHandle: await fh.createSyncAccessHandle() };
          }

          const result = await index.compact(tmpTrees); // compacts + closes source handles
          index = undefined;
          handles = [];

          for (const [role, name] of Object.entries(files)) {
            const tmpName = tmpNames[role];
            const readHandle = await (await getFileHandle(rootDirHandle, tmpName, { create: false })).createSyncAccessHandle();
            const size = readHandle.getSize();
            const bytes = new Uint8Array(size);
            readHandle.read(bytes, { at: 0 });
            await readHandle.close();

            const destSync = await (await getFileHandle(rootDirHandle, name, { create: true })).createSyncAccessHandle();
            destSync.truncate(0);
            destSync.write(bytes, { at: 0 });
            destSync.flush();
            await destSync.close();
            await rootDirHandle.removeEntry(tmpName);
          }

          const saved = result.terms.bytesSaved + result.documents.bytesSaved + result.lengths.bytesSaved;
          console.log(`Compacted ${base} in place (saved ${saved} bytes across 3 files).`);
        } else {
          const destFiles = indexFiles(destBase);
          const destTrees = {};
          for (const [role, name] of Object.entries(destFiles)) {
            const fh = await getFileHandle(rootDirHandle, name, { create: true });
            destTrees[role] = { syncAccessHandle: await fh.createSyncAccessHandle() };
          }
          const result = await index.compact(destTrees);
          index = undefined;
          handles = [];
          const saved = result.terms.bytesSaved + result.documents.bytesSaved + result.lengths.bytesSaved;
          console.log(`Compacted ${base} -> ${destBase} (saved ${saved} bytes across 3 files).`);
        }
        break;
      }

      default:
        console.error(`Error: unknown command '${command}'`);
        usage();
    }

    if (index) await index.close();
  } catch (err) {
    console.error(`Error: ${err.message}`);
    if (index && index.isOpen) {
      await index.close();
    } else {
      await closeHandles(handles);
    }
    process.exit(1);
  }
}

main().catch(err => {
  console.error(`Error: ${err.message}`);
  process.exit(1);
});
