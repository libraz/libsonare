/**
 * Every caller-supplied array's length is read through `node_list_length` before
 * anything is sized or walked from it. A sparse array claims up to 2^32 - 1
 * elements while holding none, so a `reserve(array.Length())` or a loop bounded
 * by it is an allocation or a walk the caller never paid for.
 *
 * The scan is textual: outside the helper's own body no `.Length()` may be read
 * on anything but the N-API `info`. A `Napi::Buffer` reads `ElementLength()`,
 * since its bytes are already in memory.
 */

import { describe, expect, it } from 'vitest';
import { addonSources, SHARED_READER_FILE } from './_addon_sources.js';

const HELPER = 'node_list_length';

function stripComments(text: string): string {
  return text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
}

// The helper's body, removed from its own file so its single raw read is not counted.
function withoutHelperBody(text: string): string {
  const start = text.indexOf(`inline uint32_t ${HELPER}(`);
  if (start < 0) {
    return text;
  }
  const end = text.indexOf('\n}\n', start);
  return text.slice(0, start) + text.slice(end + 3);
}

describe('addon list-length budget', () => {
  const sources = addonSources().map((source) => ({
    file: source.file,
    text: stripComments(source.text),
  }));

  it('defines the helper once, in the shared reader header', () => {
    const definitions = sources.filter((source) =>
      new RegExp(`inline uint32_t ${HELPER}\\(`).test(source.text),
    );
    expect(definitions.map((source) => source.file)).toEqual([SHARED_READER_FILE]);
  });

  it('reads no array length outside the helper', () => {
    expect(sources.length).toBeGreaterThan(20);
    const offenders: string[] = [];
    for (const source of sources) {
      const text =
        source.file === SHARED_READER_FILE ? withoutHelperBody(source.text) : source.text;
      const lines = text.split('\n');
      lines.forEach((line, index) => {
        for (const match of line.matchAll(/([\w\]).>-]+)\s*\.\s*Length\s*\(\s*\)/g)) {
          if (match[1] !== 'info') {
            offenders.push(`${source.file}:${index + 1}: ${line.trim()}`);
          }
        }
      });
    }
    expect(offenders).toEqual([]);
  });

  it('routes the readers through the helper', () => {
    // A floor, so a scan that stopped matching cannot pass on an emptied tree.
    const calls = sources.reduce(
      (total, source) =>
        total + (source.text.match(new RegExp(`\\b${HELPER}\\(`, 'g'))?.length ?? 0),
      0,
    );
    expect(calls).toBeGreaterThan(60);
  });
});
