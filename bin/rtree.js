#!/usr/bin/env node
import { RTree, ready, ObjectId, Pointer } from '../wasm/binjson-structures-wasm.js';
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

// A bounding box covering the whole world, used to list every point.
const WORLD_BBOX = { minLat: -90, maxLat: 90, minLng: -180, maxLng: 180 };

function usage() {
  console.error(`Usage: rtree <file.bj> <command> [args] [options]

An R-tree file stores geospatial points: each is a (lat, lng) location tagged
with a 24-hex-character ObjectId.

Viewing:
  list                          Print every point (default)
  bbox <minLat> <maxLat> <minLng> <maxLng>
                                Print points inside a bounding box
  radius <lat> <lng> <km>       Print points within <km> of a location,
                                with their distance
  info                          Print entry count, node capacity, and file size

Editing:
  insert <lat> <lng> [objectId] Insert a point (creates the file if needed);
                                a random ObjectId is generated when omitted
  remove <objectId>             Remove the point with the given ObjectId
  clear                         Remove every point
  compact [dest.bj]             Rewrite the file, dropping stale append history
                                (in place, or into dest.bj if given)

Options:
  --max-entries <n>  Node capacity for a newly created file (default 9, min 2)
  -h, --help         Show this help`);
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

function parseCoord(arg, label, min, max) {
  const n = Number(arg);
  if (!Number.isFinite(n)) {
    console.error(`Error: ${label} must be a number`);
    process.exit(1);
  }
  if (n < min || n > max) {
    console.error(`Error: ${label} must be between ${min} and ${max}`);
    process.exit(1);
  }
  return n;
}

function parsePositive(arg, label) {
  const n = Number(arg);
  if (!Number.isFinite(n) || n <= 0) {
    console.error(`Error: ${label} must be a positive number`);
    process.exit(1);
  }
  return n;
}

function parseObjectId(arg) {
  try {
    return new ObjectId(arg);
  } catch {
    console.error(`Error: objectId must be a 24-character hex string, got: ${arg}`);
    process.exit(1);
  }
}

function printPoints(entries, { distance = false } = {}) {
  if (entries.length === 0) {
    console.log('R-tree is empty.');
    return;
  }
  for (let i = 0; i < entries.length; i++) {
    const entry = entries[i];
    const suffix = distance && typeof entry.distance === 'number'
      ? `, distance: ${entry.distance.toFixed(3)} km`
      : '';
    console.log(`${i}: ${formatValue(entry.objectId)} (lat: ${entry.lat}, lng: ${entry.lng}${suffix})`);
  }
}

function parseArgs(argv) {
  const opts = { maxEntries: 9 };
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      usage();
    } else if (arg === '--max-entries') {
      const n = Number(argv[++i]);
      if (!Number.isInteger(n) || n < 2) {
        console.error('Error: --max-entries must be an integer >= 2');
        process.exit(1);
      }
      opts.maxEntries = n;
    } else if (arg.startsWith('--max-entries=')) {
      const n = Number(arg.slice('--max-entries='.length));
      if (!Number.isInteger(n) || n < 2) {
        console.error('Error: --max-entries must be an integer >= 2');
        process.exit(1);
      }
      opts.maxEntries = n;
    } else {
      positional.push(arg);
    }
  }
  return { opts, positional };
}

async function openTree(filePath, maxEntries, { create }) {
  const rootDirHandle = await navigator.storage.getDirectory();
  const fileHandle = await getFileHandle(rootDirHandle, filePath, { create });
  const syncHandle = await fileHandle.createSyncAccessHandle();
  const tree = new RTree(syncHandle, maxEntries);
  await tree.open();
  return { tree, syncHandle };
}

async function main() {
  const { opts, positional } = parseArgs(process.argv.slice(2));

  const filePath = positional[0];
  if (!filePath) usage();

  const command = (positional[1] || 'list').toLowerCase();
  const args = positional.slice(2);

  // Only insert creates a missing file; other commands require it to exist.
  const creating = command === 'insert';

  await ready();

  let tree;
  let syncHandle;
  try {
    ({ tree, syncHandle } = await openTree(filePath, opts.maxEntries, { create: creating }));

    switch (command) {
      case 'list':
      case 'dump':
      case 'decode': {
        printPoints(tree.searchBBox(WORLD_BBOX));
        break;
      }

      case 'bbox': {
        if (args.length < 4) {
          console.error('Error: bbox requires <minLat> <maxLat> <minLng> <maxLng>');
          process.exit(1);
        }
        const minLat = parseCoord(args[0], 'minLat', -90, 90);
        const maxLat = parseCoord(args[1], 'maxLat', -90, 90);
        const minLng = parseCoord(args[2], 'minLng', -180, 180);
        const maxLng = parseCoord(args[3], 'maxLng', -180, 180);
        printPoints(tree.searchBBox({ minLat, maxLat, minLng, maxLng }));
        break;
      }

      case 'radius': {
        if (args.length < 3) {
          console.error('Error: radius requires <lat> <lng> <km>');
          process.exit(1);
        }
        const lat = parseCoord(args[0], 'lat', -90, 90);
        const lng = parseCoord(args[1], 'lng', -180, 180);
        const km = parsePositive(args[2], 'km');
        printPoints(tree.searchRadius(lat, lng, km), { distance: true });
        break;
      }

      case 'info':
      case 'stats': {
        console.log(`file:        ${filePath}`);
        console.log(`size:        ${syncHandle.getSize()} bytes`);
        console.log(`points:      ${tree.size()}`);
        console.log(`maxEntries:  ${tree.maxEntries}`);
        break;
      }

      case 'insert':
      case 'add': {
        if (args.length < 2) {
          console.error('Error: insert requires <lat> <lng> [objectId]');
          process.exit(1);
        }
        const lat = parseCoord(args[0], 'lat', -90, 90);
        const lng = parseCoord(args[1], 'lng', -180, 180);
        const objectId = args[2] ? parseObjectId(args[2]) : new ObjectId();
        tree.insert(lat, lng, objectId);
        console.log(`Inserted ${formatValue(objectId)} at (lat: ${lat}, lng: ${lng}).`);
        break;
      }

      case 'remove':
      case 'delete':
      case 'del': {
        if (args.length < 1) {
          console.error('Error: remove requires an <objectId>');
          process.exit(1);
        }
        const objectId = parseObjectId(args[0]);
        const removed = tree.remove(objectId);
        if (removed) {
          console.log(`Removed ${formatValue(objectId)}.`);
        } else {
          console.log(`${formatValue(objectId)} not found; nothing removed.`);
          process.exitCode = 1;
        }
        break;
      }

      case 'clear': {
        const count = tree.size();
        await tree.clear();
        console.log(`Cleared ${count} point${count === 1 ? '' : 's'}.`);
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
          const result = await tree.compact(tmpSync); // compact closes tmpSync
          await tree.close(); // also closes the source syncHandle
          tree = undefined;
          syncHandle = undefined;

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
          const result = await tree.compact(destSync); // compact closes destSync
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
    if (tree && tree.isOpen) {
      await tree.close();
    } else if (syncHandle) {
      try { await syncHandle.close(); } catch { /* already closed */ }
    }
    process.exit(1);
  }
}

main().catch(err => {
  console.error(`Error: ${err.message}`);
  process.exit(1);
});
