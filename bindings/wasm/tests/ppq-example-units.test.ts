/**
 * A published `@example` for a PPQ-typed value (a quarter-note count, not a
 * MIDI tick count) must never carry a `* 960`-style scale factor: 960 is a
 * common MIDI ticks-per-quarter-note resolution, and multiplying a bar/beat
 * count by it turns a 1-4 bar example into one roughly 800x too long. Two
 * separate examples (transcribe.ts, align_take.ts) carried exactly this bug.
 *
 * Regex-level rather than a doc-comment parse: scans every `src/*.ts` source
 * file's text for the literal pattern, which is enough to catch the shape
 * that actually recurred without needing to locate `@example` blocks first.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

const SRC_ROOT = new URL('../src/', import.meta.url).pathname;

function tsFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      out.push(...tsFiles(full));
    } else if (entry.name.endsWith('.ts')) {
      out.push(full);
    }
  }
  return out;
}

describe('no MIDI-tick scale factor in a PPQ example', () => {
  it('finds no `* 960` (or 960 * / 480 *) literal in any src/*.ts file', () => {
    const offenders: string[] = [];
    for (const file of tsFiles(SRC_ROOT)) {
      const text = readFileSync(file, 'utf8');
      if (/\*\s*960\b|\b960\s*\*|\*\s*480\b|\b480\s*\*/.test(text)) {
        offenders.push(file.slice(SRC_ROOT.length));
      }
    }
    expect(offenders).toEqual([]);
  });
});
