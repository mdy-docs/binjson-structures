#!/usr/bin/env node
import { BPlusTree, ObjectId, Pointer } from '../wasm/binjson-structures-wasm.js';
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

function usage() {
  console.error(`Usage: bplustree <file.bj> <command> [args] [options]

Commands:
  list                       Print every entry in sorted order (default)
  get <key>                  Look up a single key
  range <min> <max>          Print entries with min <= key <= max
  put <key> <value>          Insert or update a key (creates the file if needed)
  delete <key>               Remove a key
  info                       Print size, height, and order
  compact [dest.bj]          Rewrite the file, dropping stale append history
                             (in place, or into dest.bj if given)

Keys are read as numbers when they look numeric, otherwise as strings.
Use --string-keys to force every key to be treated as a string.

Values are parsed as JSON when possible (e.g. '{"a":1}', '42', 'true',
'"text"'), and fall back to a raw string otherwise.

Options:
  --order <n>       Tree order for a newly created file (default 3, min 3)
  -s, --string-keys Treat keys as strings even when they look numeric
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

    if (val instanceof Pointer) {
      return `Pointer(${val.valueOf()})`;
    }

    if (val instanceof ObjectId) {
      return `ObjectId(${val.toHexString ? val.toHexString() : val.toString()})`;
    }

    if (val instanceof Date) {
      return `Date(${val.toISOString()})`;
    }

    if (Array.isArray(val)) {
      if (val.length === 0) return '[]';
      const inner = val.map(item => `${nextPad}${render(item, depth + 1)}`).join('\n');
      return `[
${inner}
${pad}]`;
    }

    if (typeof val === 'object') {
      const entries = Object.entries(val);
      if (entries.length === 0) return '{}';
      const inner = entries
        .map(([k, v]) => `${nextPad}${k}: ${render(v, depth + 1)}`)
        .join('\n');
      return `{
${inner}
${pad}}`;
    }

    return JSON.stringify(val);
  };

  return render(value, 0);
}

// Keys are number|string (see BPlusTree#allocKey). Read numeric-looking args as
// numbers unless the caller forced string keys.
function parseKey(arg, stringKeys) {
  if (!stringKeys && /^-?\d+(\.\d+)?$/.test(arg)) {
    return Number(arg);
  }
  return arg;
}

// Values are arbitrary binjson-encodable data. Accept JSON on the command line,
// falling back to the literal string when it is not valid JSON.
function parseValue(arg) {
  try {
    return JSON.parse(arg);
  } catch {
    return arg;
  }
}

function printEntries(entries) {
  if (entries.length === 0) {
    console.log('B+ tree is empty.');
    return;
  }
  console.log(`B+ tree contains ${entries.length} ${entries.length === 1 ? 'entry' : 'entries'}:\n`);
  for (let i = 0; i < entries.length; i++) {
    const entry = entries[i];
    console.log(`Entry ${i}:`);
    console.log(`  key: ${formatValue(entry.key)}`);
    console.log(`  value: ${formatValue(entry.value)}`);
    if (i < entries.length - 1) console.log('');
  }
}

function parseArgs(argv) {
  const opts = { order: 3, stringKeys: false };
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      usage();
    } else if (arg === '-s' || arg === '--string-keys') {
      opts.stringKeys = true;
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

async function openTree(filePath, order, { create }) {
  const rootDirHandle = await navigator.storage.getDirectory();
  const fileHandle = await getFileHandle(rootDirHandle, filePath, { create });
  const syncHandle = await fileHandle.createSyncAccessHandle();
  const tree = new BPlusTree(syncHandle, order);
  await tree.open();
  return tree;
}

async function main() {
  const { opts, positional } = parseArgs(process.argv.slice(2));

  const filePath = positional[0];
  if (!filePath) usage();

  const command = (positional[1] || 'list').toLowerCase();
  const args = positional.slice(2);

  // Commands that mutate create the file if it is missing; read-only ones don't.
  const mutating = command === 'put' || command === 'set' || command === 'add' ||
    command === 'delete' || command === 'del' || command === 'remove';

  let tree;
  try {
    tree = await openTree(filePath, opts.order, { create: mutating });

    switch (command) {
      case 'list':
      case 'dump':
      case 'decode': {
        printEntries(tree.toArray());
        break;
      }

      case 'get':
      case 'search': {
        if (args.length < 1) {
          console.error('Error: get requires a <key>');
          process.exit(1);
        }
        const key = parseKey(args[0], opts.stringKeys);
        const value = tree.search(key);
        if (value === undefined) {
          console.log(`Key ${formatValue(key)} not found.`);
          process.exitCode = 1;
        } else {
          console.log(formatValue(value));
        }
        break;
      }

      case 'range': {
        if (args.length < 2) {
          console.error('Error: range requires <min> and <max> keys');
          process.exit(1);
        }
        const min = parseKey(args[0], opts.stringKeys);
        const max = parseKey(args[1], opts.stringKeys);
        printEntries(tree.rangeSearch(min, max));
        break;
      }

      case 'put':
      case 'set':
      case 'add': {
        if (args.length < 2) {
          console.error('Error: put requires a <key> and a <value>');
          process.exit(1);
        }
        const key = parseKey(args[0], opts.stringKeys);
        const value = parseValue(args[1]);
        const existed = tree.search(key) !== undefined;
        tree.add(key, value);
        console.log(`${existed ? 'Updated' : 'Inserted'} ${formatValue(key)} = ${formatValue(value)}`);
        break;
      }

      case 'delete':
      case 'del':
      case 'remove': {
        if (args.length < 1) {
          console.error('Error: delete requires a <key>');
          process.exit(1);
        }
        const key = parseKey(args[0], opts.stringKeys);
        if (tree.search(key) === undefined) {
          console.log(`Key ${formatValue(key)} not found; nothing deleted.`);
          process.exitCode = 1;
        } else {
          tree.delete(key);
          console.log(`Deleted ${formatValue(key)}.`);
        }
        break;
      }

      case 'info':
      case 'stats': {
        console.log(`file:   ${filePath}`);
        console.log(`order:  ${tree.order}`);
        console.log(`size:   ${tree.size()} ${tree.size() === 1 ? 'entry' : 'entries'}`);
        console.log(`height: ${tree.getHeight()}`);
        break;
      }

      case 'compact': {
        const dest = args[0] || filePath;
        const rootDirHandle = await navigator.storage.getDirectory();
        if (dest === filePath) {
          // In-place: compact to a temp file, then copy it back over the source.
          const tmpName = `${filePath}.compact-${Date.now()}.tmp`;
          const tmpHandle = await getFileHandle(rootDirHandle, tmpName, { create: true });
          const tmpSync = await tmpHandle.createSyncAccessHandle();
          const result = await tree.compact(tmpSync);
          await tree.close();
          tree = undefined;

          // Read the compacted bytes back and write them over the original.
          const freshTmp = await getFileHandle(rootDirHandle, tmpName, { create: false });
          const tmpRead = await freshTmp.createSyncAccessHandle();
          const size = tmpRead.getSize();
          const bytes = new Uint8Array(size);
          tmpRead.read(bytes, { at: 0 });
          await tmpRead.close();

          const destHandle = await getFileHandle(rootDirHandle, filePath, { create: true });
          const destSync = await destHandle.createSyncAccessHandle();
          destSync.truncate(0);
          destSync.write(bytes, { at: 0 });
          destSync.flush();
          await destSync.close();
          await rootDirHandle.removeEntry(tmpName);

          console.log(`Compacted ${filePath}: ${result.oldSize} -> ${result.newSize} bytes (saved ${result.bytesSaved}).`);
        } else {
          const destHandle = await getFileHandle(rootDirHandle, dest, { create: true });
          const destSync = await destHandle.createSyncAccessHandle();
          const result = await tree.compact(destSync);
          console.log(`Compacted ${filePath} -> ${dest}: ${result.oldSize} -> ${result.newSize} bytes (saved ${result.bytesSaved}).`);
        }
        break;
      }

      default:
        console.error(`Error: unknown command '${command}'`);
        usage();
    }

    if (tree) await tree.close();
  } catch (err) {
    console.error(`Error: ${err.message}`);
    if (tree && tree.isOpen) await tree.close();
    process.exit(1);
  }
}

main().catch(err => {
  console.error(`Error: ${err.message}`);
  process.exit(1);
});
