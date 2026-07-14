/**
 * §5.4 — binary copy/insert delta as TextLog's diff storage format. The delta
 * rebuilds a target from a source with COPY runs from the source and INSERT
 * runs of literal bytes; it is several times smaller than the unified-patch
 * text older versions stored, and getDiff still renders a unified diff by
 * re-diffing the reconstructed versions.
 */
import { describe, it, expect } from 'vitest';
import { ready, TextLog, createDelta, applyDelta, createPatch } from '../wasm/binjson-structures-wasm.js';
import { MemoryHandle } from '../third_party/binjson/js/binjson.js';

await ready();

function docAt(v) {
  const lines = [];
  const n = 30 + (v % 5);
  for (let i = 0; i < n; i++) lines.push(`line ${i} of version ${v}: value ${(i * 2654435761 + v) % 1000}`);
  return lines.join('\n') + '\n';
}

describe('§5.4 binary copy/insert delta', () => {
  it('round-trips every kind of edit', () => {
    const base = docAt(1);
    const cases = [
      base,                                             // identical
      '',                                               // target emptied
      base + '\nappended paragraph\n',                  // pure suffix insert
      'new heading\n' + base,                           // pure prefix insert
      base.replace('line 5', 'LINE FIVE CHANGED'),      // single localized edit
      // two separate edits far apart (exercises multi-region copy/insert)
      base.replace('line 2', 'EDIT A').replace('line 27 of version 1', 'EDIT B WAY DOWN HERE'),
      'completely unrelated content with nothing in common at all',
    ];
    for (const target of cases) {
      const delta = createDelta(base, target);
      expect(delta).toBeInstanceOf(Uint8Array);
      expect(applyDelta(base, delta)).toBe(target);
    }
    // Also from an empty source.
    expect(applyDelta('', createDelta('', base))).toBe(base);
  });

  it('is substantially smaller than a unified-patch of the same edit', () => {
    // A realistic single-line edit on a sizeable document.
    const a = docAt(42);
    const b = a.replace('line 10 of version 42', 'line 10 of version 42 (edited today)');
    const delta = createDelta(a, b);
    const patch = new TextEncoder().encode(createPatch('document', a, b));
    // The binary delta carries no context lines / hunk headers / line prefixes.
    expect(delta.length).toBeLessThan(patch.length / 2);
  });

  it('accumulated delta storage beats unified-patch storage across a version series', () => {
    let deltaTotal = 0, patchTotal = 0;
    let prev = docAt(1);
    for (let v = 2; v <= 60; v++) {
      const next = docAt(v);
      deltaTotal += createDelta(prev, next).length;
      patchTotal += new TextEncoder().encode(createPatch('document', prev, next)).length;
      prev = next;
    }
    // Report the ratio for visibility, and assert a clear win.
    // eslint-disable-next-line no-console
    console.log(`delta series: ${deltaTotal} B vs unified-patch ${patchTotal} B (${(patchTotal / deltaTotal).toFixed(1)}x)`);
    expect(deltaTotal).toBeLessThan(patchTotal / 2);
  });

  it('returns null on a malformed / out-of-bounds delta instead of crashing', () => {
    const src = docAt(3);
    const good = createDelta(src, docAt(4));
    // Truncated delta.
    expect(applyDelta(src, good.slice(0, Math.max(1, good.length - 3)))).toBe(null);
    // Random garbage bytes (a COPY with a wild offset, etc.).
    const garbage = new Uint8Array([0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x01]);
    const r = applyDelta(src, garbage);
    expect(r === null || typeof r === 'string').toBe(true); // never throws/crashes
  });
});

describe('§5.4 TextLog stores binary deltas end to end', () => {
  it('reconstructs every version and still renders unified diffs', async () => {
    const log = new TextLog(new MemoryHandle(), 6); // snapshot every 6 versions
    await log.open();
    const N = 40;
    for (let v = 1; v <= N; v++) await log.addVersion(docAt(v));

    // Every version reconstructs from its snapshot + binary-delta chain.
    for (const v of [1, 5, 6, 7, 20, 39, 40]) {
      expect(await log.getVersion(v)).toBe(docAt(v));
    }
    // getVersionHash verifies the reconstruction internally (would throw on
    // a bad delta chain); getDiff is still human-readable unified text.
    const diff = await log.getDiff(10, 11);
    expect(diff).toContain('@@');
    expect(diff.startsWith('---')).toBe(true);
    await log.close();
  });
});
