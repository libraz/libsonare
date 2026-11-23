/**
 * A copy of a zero-length array is a given argument. An empty container may carry a null element
 * pointer, which a C entry reads as an omitted argument, so every pointer into a copy the bindings
 * hand the C ABI, as a call argument or a struct field, goes through the one accessor that never
 * returns null. A pointer the binding sets to null for an empty list on purpose spells it out.
 */

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { wasmBindingSources } from './_wasm_binding_sources';

const TOP_LEVEL = new URL('../../../src/wasm/bindings.cpp', import.meta.url).pathname;

const sources = () => [
  ...wasmBindingSources(),
  { file: 'bindings.cpp', text: readFileSync(TOP_LEVEL, 'utf8') },
];

describe('C-ABI element pointers', () => {
  it('hands the C ABI every pointer into a copy through elementData', () => {
    const all = sources();
    expect(all.length).toBeGreaterThan(20);
    let uses = 0;
    const offenders: string[] = [];
    for (const source of all) {
      const text = source.text.replace(/\/\/.*$/gm, '');
      uses += text.match(/\belementData\(/g)?.length ?? 0;
      for (const call of text.matchAll(/\bsonare_\w+\s*\(/g)) {
        const start = (call.index ?? 0) + call[0].length;
        const segment = text.slice(start, text.indexOf(';', start));
        for (const read of segment.matchAll(/[\w\][>.-]+?\.data\(\)/g)) {
          const before = segment.slice(Math.max(0, (read.index ?? 0) - 60), read.index);
          if (!/empty\(\)\s*\?\s*nullptr\s*:\s*$/.test(before)) {
            offenders.push(`${source.file}: ${call[0]}${read[0]}`);
          }
        }
      }
      for (const field of text.matchAll(/\b\w+(?:\.|->)\w+\s*=\s*[\w\][>.-]+?\.data\(\);/g)) {
        offenders.push(`${source.file}: ${field[0]}`);
      }
    }
    // A floor, so a scan that stops seeing the accessor cannot pass as clean.
    expect(uses).toBeGreaterThan(50);
    expect(offenders).toEqual([]);
  });
});
