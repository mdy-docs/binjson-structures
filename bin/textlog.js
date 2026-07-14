#!/usr/bin/env node
import { readFileSync } from 'fs';
import { TextLog, ready } from '../wasm/binjson-structures-wasm.js';
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
  console.error(`Usage: textlog <file.bj> <command> [args] [options]

A versioned text log. Each version stores a full text snapshot; the log keeps a
snapshot every few versions plus diffs, so any version can be reconstructed.
Versions are numbered from 1.

Viewing:
  list                  List every version with its hash (default)
  get [version]         Print the full text at <version> (default: latest)
  diff <from> <to>      Print a human-readable diff between two versions
  hash <version>        Print the SHA-256 hash of a version
  info                  Print current version, snapshot interval, and file size

Editing:
  add [text...]         Append a new version (creates the file if needed).
                        The text is taken from the arguments, or from --file,
                        or from stdin when neither is given.

Options:
  -f, --file <path>            add: read the version text from a file
  --diffs-per-snapshot <n>     Snapshot interval for a newly created log
                               (default 10, minimum 1)
  -h, --help                   Show this help`);
  process.exit(1);
}

function parseVersion(arg, label) {
  if (!/^\d+$/.test(arg)) {
    console.error(`Error: ${label} must be a positive integer`);
    process.exit(1);
  }
  const n = Number(arg);
  if (n < 1) {
    console.error(`Error: ${label} must be at least 1`);
    process.exit(1);
  }
  return n;
}

function parseArgs(argv) {
  const opts = { diffsPerSnapshot: 10, file: null };
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      usage();
    } else if (arg === '-f' || arg === '--file') {
      opts.file = argv[++i];
      if (!opts.file) {
        console.error('Error: --file requires a path');
        process.exit(1);
      }
    } else if (arg.startsWith('--file=')) {
      opts.file = arg.slice('--file='.length);
    } else if (arg === '--diffs-per-snapshot') {
      const n = Number(argv[++i]);
      if (!Number.isInteger(n) || n < 1) {
        console.error('Error: --diffs-per-snapshot must be an integer >= 1');
        process.exit(1);
      }
      opts.diffsPerSnapshot = n;
    } else if (arg.startsWith('--diffs-per-snapshot=')) {
      const n = Number(arg.slice('--diffs-per-snapshot='.length));
      if (!Number.isInteger(n) || n < 1) {
        console.error('Error: --diffs-per-snapshot must be an integer >= 1');
        process.exit(1);
      }
      opts.diffsPerSnapshot = n;
    } else {
      positional.push(arg);
    }
  }
  return { opts, positional };
}

function readStdin() {
  return new Promise((resolve, reject) => {
    let data = '';
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', chunk => { data += chunk; });
    process.stdin.on('end', () => resolve(data));
    process.stdin.on('error', reject);
  });
}

// Resolve the text for an `add` from arguments, --file, or stdin.
async function resolveText(args, opts) {
  if (opts.file) return readFileSync(opts.file, 'utf8');
  if (args.length) return args.join(' ');
  if (!process.stdin.isTTY) return readStdin();
  console.error('Error: add requires text (as arguments, via --file, or on stdin)');
  process.exit(1);
}

async function openLog(filePath, diffsPerSnapshot, { create }) {
  const rootDirHandle = await navigator.storage.getDirectory();
  const fileHandle = await getFileHandle(rootDirHandle, filePath, { create });
  const syncHandle = await fileHandle.createSyncAccessHandle();
  const log = new TextLog(syncHandle, diffsPerSnapshot);
  await log.open();
  return { log, syncHandle };
}

async function main() {
  const { opts, positional } = parseArgs(process.argv.slice(2));

  const filePath = positional[0];
  if (!filePath) usage();

  const command = (positional[1] || 'list').toLowerCase();
  const args = positional.slice(2);

  // Only `add` creates a missing file; other commands require it to exist.
  const creating = command === 'add';

  // Resolve `add` text before opening the log, since stdin may be involved.
  let addText;
  if (creating) addText = await resolveText(args, opts);

  await ready();

  let log;
  let syncHandle;
  try {
    ({ log, syncHandle } = await openLog(filePath, opts.diffsPerSnapshot, { create: creating }));

    switch (command) {
      case 'list':
      case 'log': {
        const current = log.getCurrentVersion();
        if (current === 0) {
          console.log('Log has no versions.');
          break;
        }
        for (let v = 1; v <= current; v++) {
          const hash = await log.getVersionHash(v);
          const marker = v === current ? ' (latest)' : '';
          console.log(`v${v}: ${hash}${marker}`);
        }
        break;
      }

      case 'get':
      case 'show': {
        const current = log.getCurrentVersion();
        if (current === 0) {
          console.log('Log is empty.');
          process.exitCode = 1;
          break;
        }
        const version = args.length ? parseVersion(args[0], 'version') : current;
        if (version > current) {
          console.error(`Error: no version ${version} (log has versions 1-${current})`);
          process.exit(1);
        }
        console.log(await log.getVersion(version));
        break;
      }

      case 'diff': {
        if (args.length < 2) {
          console.error('Error: diff requires <from> and <to> versions');
          process.exit(1);
        }
        const current = log.getCurrentVersion();
        const from = parseVersion(args[0], 'from');
        const to = parseVersion(args[1], 'to');
        for (const [label, v] of [['from', from], ['to', to]]) {
          if (v > current) {
            console.error(`Error: ${label} version ${v} does not exist (log has versions 1-${current})`);
            process.exit(1);
          }
        }
        const diff = await log.getDiff(from, to);
        if (diff === '') {
          console.log(`(versions ${from} and ${to} are identical)`);
        } else {
          console.log(diff);
        }
        break;
      }

      case 'hash': {
        if (args.length < 1) {
          console.error('Error: hash requires a <version>');
          process.exit(1);
        }
        const current = log.getCurrentVersion();
        const version = parseVersion(args[0], 'version');
        if (version > current) {
          console.error(`Error: no version ${version} (log has versions 1-${current})`);
          process.exit(1);
        }
        console.log(await log.getVersionHash(version));
        break;
      }

      case 'info':
      case 'stats': {
        console.log(`file:             ${filePath}`);
        console.log(`size:             ${syncHandle.getSize()} bytes`);
        console.log(`versions:         ${log.getCurrentVersion()}`);
        console.log(`diffsPerSnapshot: ${log.diffsPerSnapshot}`);
        break;
      }

      case 'add':
      case 'commit': {
        const version = await log.addVersion(addText);
        console.log(`Added version ${version}.`);
        break;
      }

      default:
        console.error(`Error: unknown command '${command}'`);
        usage();
    }

    if (log) await log.close();
  } catch (err) {
    console.error(`Error: ${err.message}`);
    if (log && log.isOpen) {
      await log.close();
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
