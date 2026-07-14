/**
 * TiledTextLog (§5.5): full history retained across multiple append-only tile
 * files, no single file growing without bound, and cold open scanning only the
 * active tile. Tiles are ordinary TextLog files whose metadata records the
 * global version each continues from (baseVersion), so every tile reconstructs
 * independently.
 */
import { describe, it, expect } from 'vitest';
import { ready, TextLog, TiledTextLog } from '../wasm/binjson-structures-wasm.js';
import { MemoryHandle } from '../third_party/binjson/js/binjson.js';

await ready();

// A read-counting facade over any FileSystemSyncAccessHandle-shaped object, so
// tests can assert how much a cold open actually reads.
function counting(inner) {
  const h = {
    reads: 0,
    readBytes: 0,
    getSize: () => inner.getSize(),
    read(buf, opt) { h.reads++; const n = inner.read(buf, opt); h.readBytes += n; return n; },
    write(buf, opt) { return inner.write(buf, opt); },
    truncate(s) { return inner.truncate(s); },
    flush() { inner.flush(); },
    close() { inner.close(); },
  };
  return h;
}

// In-memory tile provider. Handles persist in `store` across close/reopen (a
// MemoryHandle's close() is a no-op), simulating storage where the tile's
// baseVersion is recoverable without opening it (here stashed alongside the
// handle, as a filename-encoded base would be).
function memProvider({ count = false } = {}) {
  const store = new Map(); // id -> { handle, baseVersion }
  let seq = 0;
  const p = {
    store,
    totalReads: 0,
    async listTiles() {
      return [...store.entries()].map(([id, t]) => ({ id, baseVersion: t.baseVersion }));
    },
    async openTile(id) {
      const t = store.get(id);
      if (count) {
        const c = counting(t.handle);
        // accumulate on close via a shim: simplest is to sample after use
        c._sampleInto = p;
        return c;
      }
      return t.handle;
    },
    async createTile(baseVersion) {
      const id = `tile-${String(baseVersion).padStart(12, '0')}-${seq++}`;
      const handle = new MemoryHandle();
      store.set(id, { handle, baseVersion });
      return { id, handle };
    },
  };
  return p;
}

// Deterministic evolving document so consecutive versions differ in realistic,
// diffable ways (not just appends).
function docAt(v) {
  const lines = [];
  const n = 20 + (v % 7);
  for (let i = 0; i < n; i++) {
    lines.push(`line ${i} at version ${v} value ${(i * 2654435761 + v) % 1000}`);
  }
  return lines.join('\n') + '\n';
}

describe('TiledTextLog (§5.5 tiled history)', () => {
  it('retains full history across many tiles; every version reconstructs', async () => {
    const provider = memProvider();
    const log = new TiledTextLog(provider, { diffsPerSnapshot: 8, maxTileBytes: 3000 });
    await log.open();

    const N = 200;
    const hashes = [];
    for (let v = 1; v <= N; v++) {
      const got = await log.addVersion(docAt(v));
      expect(got).toBe(v);
      hashes.push(await log.getVersionHash(v));
    }

    // It actually split into several tiles (the whole point).
    expect(log.tileCount).toBeGreaterThan(3);
    expect(log.getCurrentVersion()).toBe(N);

    // Every historical version is still readable and correct, in any order.
    for (const v of [1, 2, 7, 8, 9, 50, 123, 199, 200]) {
      expect(await log.getVersion(v)).toBe(docAt(v));
      expect(await log.getVersionHash(v)).toBe(hashes[v - 1]);
    }
    // Reverse sweep of the entire history.
    for (let v = N; v >= 1; v--) expect(await log.getVersion(v)).toBe(docAt(v));

    await log.close();
  });

  it('reopens from the persisted tiles with full history intact', async () => {
    const provider = memProvider();
    let log = new TiledTextLog(provider, { diffsPerSnapshot: 5, maxTileBytes: 2500 });
    await log.open();
    const N = 120;
    for (let v = 1; v <= N; v++) await log.addVersion(docAt(v));
    const tilesBefore = log.tileCount;
    await log.close();

    // Fresh instance, same provider (= same persisted files).
    log = new TiledTextLog(provider, { diffsPerSnapshot: 5, maxTileBytes: 2500 });
    await log.open();
    expect(log.getCurrentVersion()).toBe(N);
    expect(log.tileCount).toBe(tilesBefore);
    for (const v of [1, 6, 42, 119, 120]) expect(await log.getVersion(v)).toBe(docAt(v));

    // Appends continue with correct global numbering after reopen.
    expect(await log.addVersion(docAt(N + 1))).toBe(N + 1);
    expect(await log.getVersion(N + 1)).toBe(docAt(N + 1));
    await log.close();
  });

  it('getDiff spans tile boundaries, matching a single-file log byte-for-byte', async () => {
    // Same content into a tiled log and a standalone TextLog; diffs must match.
    const provider = memProvider();
    const tiled = new TiledTextLog(provider, { diffsPerSnapshot: 6, maxTileBytes: 2000 });
    await tiled.open();
    const single = new TextLog(new MemoryHandle(), 6);
    await single.open();

    const N = 80;
    for (let v = 1; v <= N; v++) {
      const text = docAt(v);
      await tiled.addVersion(text);
      await single.addVersion(text);
    }
    expect(tiled.tileCount).toBeGreaterThan(2);

    // Diffs across tile boundaries and within a tile alike.
    for (const [a, b] of [[1, 80], [3, 40], [10, 11], [79, 80], [1, 2]]) {
      expect(await tiled.getDiff(a, b)).toBe(await single.getDiff(a, b));
    }
    await tiled.close();
    await single.close();
  });

  it('cold open scans only the active tile, not the whole history', async () => {
    const N = 300;

    // Standalone log: one growing file, cold open re-scans everything.
    const singleHandle = new MemoryHandle();
    let single = new TextLog(singleHandle, 10);
    await single.open();
    for (let v = 1; v <= N; v++) await single.addVersion(docAt(v));
    await single.close();
    const singleCounter = counting(singleHandle);
    single = new TextLog(singleCounter, 10);
    await single.open();
    const singleReadBytes = singleCounter.readBytes;
    await single.close();

    // Tiled log: same history, but cold open touches only the active tile.
    const provider = memProvider();
    let log = new TiledTextLog(provider, { diffsPerSnapshot: 10, maxTileBytes: 4000 });
    await log.open();
    for (let v = 1; v <= N; v++) await log.addVersion(docAt(v));
    const tiles = log.tileCount;
    await log.close();

    // Reopen with counting handles and measure the open scan.
    const activeId = [...provider.store.keys()].reduce((best, id) =>
      provider.store.get(id).baseVersion > provider.store.get(best).baseVersion ? id : best);
    const activeCounter = counting(provider.store.get(activeId).handle);
    const countingProvider = {
      async listTiles() { return provider.listTiles(); },
      async openTile(id) { return id === activeId ? activeCounter : provider.store.get(id).handle; },
      async createTile(b) { return provider.createTile(b); },
    };
    log = new TiledTextLog(countingProvider, { diffsPerSnapshot: 10, maxTileBytes: 4000 });
    await log.open();
    const tiledOpenReadBytes = activeCounter.readBytes;
    await log.close();

    expect(tiles).toBeGreaterThan(4);
    // The active tile is bounded by maxTileBytes, so the cold-open scan is a
    // fraction of the full-history scan — the win grows with total history.
    expect(tiledOpenReadBytes).toBeLessThan(singleReadBytes / 3);
  });

  it('rejects out-of-range versions', async () => {
    const provider = memProvider();
    const log = new TiledTextLog(provider, { diffsPerSnapshot: 4, maxTileBytes: 1500 });
    await log.open();
    for (let v = 1; v <= 30; v++) await log.addVersion(docAt(v));
    await expect(log.getVersion(0)).rejects.toThrow();
    await expect(log.getVersion(31)).rejects.toThrow();
    await expect(log.getVersion(1000)).rejects.toThrow();
    await log.close();
  });
});
