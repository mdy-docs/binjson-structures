/**
 * Textlog integrity and latest-text cache (C_DATABASE_REVIEW.md §5.1/§5.3/§5.6).
 *
 * §5.3 — reconstructed text is verified against the entry's stored SHA-256,
 * so diff-chain corruption surfaces as BJ_ERR_VERIFY at read time instead of
 * silently wrong text. §5.6 — version ranges are validated in C
 * (BJ_ERR_RANGE), not just by the JS host. §5.1 — the current version's text
 * is cached, so consecutive addVersion calls and latest-version reads do no
 * reconstruction reads.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ready, TextLog } from '../wasm/binjson-structures-wasm.js';
import { writeFixture } from './legacy-fixtures.js';
import { deleteFile, getFileHandle } from '../third_party/binjson/js/binjson.js';
import { bootstrapOPFS } from './binjson.suite.js';

const M = await ready();
const { hasOPFS } = await bootstrapOPFS();

describe.skipIf(!hasOPFS)('WASM TextLog integrity and cache', () => {
  let root = null;
  let counter = 0;

  beforeAll(async () => {
    root = await navigator.storage.getDirectory();
  });

  const files = [];
  const name = () => {
    const n = `test-tlint-${Date.now()}-${counter++}.bj`;
    files.push(n);
    return n;
  };

  afterAll(async () => {
    for (const f of files) await deleteFile(root, f);
  });

  async function sync(filename, create = false) {
    const fh = await getFileHandle(root, filename, { create });
    return fh.createSyncAccessHandle();
  }

  function counting(handle) {
    const stats = { reads: 0 };
    return {
      stats,
      getSize: () => handle.getSize(),
      read: (buf, opts) => { stats.reads++; return handle.read(buf, opts); },
      write: (buf, opts) => handle.write(buf, opts),
      truncate: (n) => handle.truncate(n),
      flush: () => handle.flush(),
      close: () => handle.close()
    };
  }

  it('rejects out-of-range versions in C with a distinct error', async () => {
    const log = new TextLog(await sync(name(), true), 3);
    await log.open();
    await log.addVersion('one');
    await log.addVersion('two');

    // The JS wrapper pre-validates, so pin the C behavior via raw exports:
    // 0 and past-the-end must fail with BJ_ERR_RANGE (-9), not succeed with
    // empty/latest text as the raw reconstruction used to.
    expect(M._tlw_get_version(log.ctx, 0)).toBe(-9);
    expect(M._tlw_get_version(log.ctx, 3)).toBe(-9);
    expect(M._tlw_get_version_hash(log.ctx, 0)).toBe(-9);
    expect(M._tlw_get_version_hash(log.ctx, 99)).toBe(-9);
    expect(M._tlw_get_diff(log.ctx, 0, 2)).toBe(-9);
    expect(M._tlw_get_diff(log.ctx, 1, 3)).toBe(-9);
    expect(M._tlw_get_version(log.ctx, 2)).toBe(0);   // in range still works
    await log.close();
  });

  it('detects snapshot corruption via the stored hash', async () => {
    // The legacy (JS-written) fixture carries no commit CRCs, so a flipped
    // content byte passes open and is only catchable by the per-entry hash.
    // Fixture: 6 versions at dps 3 — v1 and v5 are snapshots, so corrupting
    // v1's text poisons the v1–v4 chain while v5+ stays verifiable.
    const file = name();
    await writeFixture(await sync(file, true), 'textlog-v6-dps3.bin');
    {
      const h = await sync(file);
      const buf = new Uint8Array(h.getSize());
      h.read(buf, { at: 0 });
      // First occurrence of "Line 2" is inside v1's snapshot data (records
      // are appended in version order).
      const bytes = new TextEncoder().encode('Line 2');
      const at = buf.findIndex((_, i) =>
        bytes.every((b, j) => buf[i + j] === b));
      expect(at).toBeGreaterThan(0);
      buf[at] ^= 0x01;                    // 'L' -> 'M', structure intact
      h.write(buf, { at: 0 });
      h.flush();
      await h.close();
    }

    const log = new TextLog(await sync(file), 3);
    await log.open();                     // legacy scan accepts the bytes
    await expect(log.getVersion(1)).rejects.toThrow(/invariant/i);
    expect(await log.getVersion(5)).toContain('Line 5');   // clean chain
    await log.close();
  });

  it('caches the latest text: repeat adds and latest reads do no reconstruction reads', async () => {
    const file = name();
    const base = Array.from({ length: 100 }, (_, i) => `line ${i}`).join('\n');
    {
      const log = new TextLog(await sync(file, true), 25);
      await log.open();
      await log.addVersion(base);
      await log.close();
    }
    const proxy = counting(await sync(file));
    const log = new TextLog(proxy, 25);
    await log.open();

    // First diff add must rebuild the previous version from the file...
    const before = proxy.stats.reads;
    await log.addVersion(base + '\nv2');
    expect(proxy.stats.reads - before).toBeGreaterThan(0);

    // ...every later add and latest-version read is served from the cache.
    const warm = proxy.stats.reads;
    for (let i = 3; i <= 20; i++) await log.addVersion(base + `\nv${i}`);
    expect(await log.getVersion(20)).toBe(base + '\nv20');
    expect(proxy.stats.reads).toBe(warm);

    // Non-latest versions still reconstruct (and verify) from the file.
    expect(await log.getVersion(2)).toBe(base + '\nv2');
    expect(proxy.stats.reads).toBeGreaterThan(warm);
    await log.close();
  });

  it('reads and extends legacy JS-written logs (frozen fixture)', async () => {
    // Fixture provenance (see test/fixtures/generate-legacy-fixtures.mjs):
    // six versions at diffsPerSnapshot 3, written by the removed pure-JS
    // implementation with jsdiff patch entries.
    const VERSIONS = [
      'Line 1\nLine 2\nLine 3\n',
      'Line 1\nLine 2 changed\nLine 3\n',
      'Line 1\nLine 2 changed\nLine 3\nLine 4\n',
      'Header\nLine 1\nLine 2 changed\nLine 3\nLine 4\n',
      'Header\nLine 1\nLine 2 changed\nLine 3\nLine 4\nLine 5\n',
      'no trailing newline here'
    ];
    const file = name();
    await writeFixture(await sync(file, true), 'textlog-v6-dps3.bin');

    const log = new TextLog(await sync(file), 3);
    await log.open();
    expect(log.getCurrentVersion()).toBe(6);
    for (let i = 0; i < 6; i++) {
      expect(await log.getVersion(i + 1)).toBe(VERSIONS[i]);
    }
    await log.addVersion('appended by wasm');
    expect(await log.getVersion(7)).toBe('appended by wasm');
    expect(await log.getVersion(3)).toBe(VERSIONS[2]);   // history intact
    await log.close();
  });

  it('cache stays correct across snapshot boundaries and reopens', async () => {
    const file = name();
    const log = new TextLog(await sync(file, true), 2);   // snapshot every 2 diffs
    await log.open();
    const texts = [];
    for (let i = 1; i <= 9; i++) {
      const t = `document at version ${i}\n` + 'x'.repeat(i * 10);
      texts.push(t);
      await log.addVersion(t);
    }
    for (let i = 1; i <= 9; i++) expect(await log.getVersion(i)).toBe(texts[i - 1]);
    await log.close();

    const re = new TextLog(await sync(file), 2);
    await re.open();
    expect(await re.getVersion(9)).toBe(texts[8]);   // cold: reconstructed + cached
    expect(await re.getVersion(9)).toBe(texts[8]);   // warm: from cache
    await re.addVersion('after reopen');
    expect(await re.getVersion(10)).toBe('after reopen');
    expect(await re.getVersion(4)).toBe(texts[3]);
    await re.close();
  });
});
