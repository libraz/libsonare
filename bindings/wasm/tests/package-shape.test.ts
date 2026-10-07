/**
 * Published package shape: subpath conditions, type fallbacks for legacy
 * resolution, side-effect flags and the exported binaries.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')) as {
  exports: Record<string, string | Record<string, string>>;
  typesVersions: Record<string, Record<string, string[]>>;
  sideEffects: string[];
  engines: { node: string };
};

const jsSubpaths = Object.entries(pkg.exports).filter(
  (entry): entry is [string, Record<string, string>] => typeof entry[1] === 'object',
);

describe('package.json shape', () => {
  it('gives every JS subpath types, import and default', () => {
    expect(jsSubpaths.map(([key]) => key)).toEqual([
      '.',
      './analysis',
      './worklet',
      './worker',
      './vocal-edit-worker',
    ]);
    for (const [, conditions] of jsSubpaths) {
      expect(conditions.types).toMatch(/\.d\.ts$/);
      expect(conditions.import).toMatch(/\.js$/);
      expect(conditions.default).toBe(conditions.import);
    }
  });

  it('maps every non-root subpath in typesVersions to its declaration', () => {
    const map = pkg.typesVersions['*'];
    const subpaths = jsSubpaths.filter(([key]) => key !== '.');
    expect(Object.keys(map).sort()).toEqual(subpaths.map(([key]) => key.slice(2)).sort());
    for (const [key, conditions] of subpaths) {
      expect(map[key.slice(2)]).toEqual([conditions.types.replace('./', '')]);
    }
  });

  it('lists the worker entries and the dispose polyfill as the only side-effectful files', () => {
    expect(pkg.sideEffects).toEqual([
      './dist/worker.js',
      './dist/vocal_edit_worker.js',
      './src/lifetime.ts',
    ]);
    const importTargets = jsSubpaths.map(([, conditions]) => conditions.import);
    for (const file of pkg.sideEffects.filter((entry: string) => entry.startsWith('./dist/'))) {
      expect(importTargets).toContain(file);
    }
  });

  it('exports every wasm binary in dist', () => {
    expect(pkg.exports['./wasm']).toBe('./dist/sonare.wasm');
    expect(pkg.exports['./wasm/analysis']).toBe('./dist/sonare-analysis.wasm');
    const targets = new Set(Object.values(pkg.exports).filter((v) => typeof v === 'string'));
    let distFiles: string[] = [];
    try {
      distFiles = readdirSync(join(root, 'dist')).filter((f) => f.endsWith('.wasm'));
    } catch {
      // dist is absent before a build; the explicit assertions above still hold.
    }
    for (const file of distFiles) {
      expect(targets).toContain(`./dist/${file}`);
    }
  });

  it('declares the Node floor that supports require() of ESM', () => {
    const match = /^>=(\d+)\.(\d+)\.(\d+)$/.exec(pkg.engines.node);
    expect(match).not.toBeNull();
    const [major, minor] = [Number(match?.[1]), Number(match?.[2])];
    expect(major > 22 || (major === 22 && minor >= 12)).toBe(true);
  });
});
