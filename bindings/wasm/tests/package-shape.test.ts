/**
 * Published package shape: subpath conditions, type fallbacks for legacy
 * resolution, side-effect flags, the exported binaries and the dynamic imports
 * the shipped JS makes.
 */

import { execFileSync } from 'node:child_process';
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

/** Every file under @p dir ending in @p suffix. */
function listFiles(dir: string, suffix: string): string[] {
  return readdirSync(dir, { withFileTypes: true, recursive: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith(suffix))
    .map((entry) => join(entry.parentPath, entry.name));
}

const jsSubpaths = Object.entries(pkg.exports).filter(
  (entry): entry is [string, Record<string, string>] => typeof entry[1] === 'object',
);

describe('package.json shape', () => {
  it('packs the licence text and the notice', () => {
    // A dry run still runs prepack, which is where both files are copied in.
    const listing = execFileSync('yarn', ['pack', '--dry-run', '--json'], {
      cwd: root,
      encoding: 'utf8',
    })
      .split('\n')
      .filter((line) => line.startsWith('{'))
      .map((line) => (JSON.parse(line) as { location?: string }).location);
    expect(listing).toEqual(expect.arrayContaining(['LICENSE', 'NOTICE']));
  });

  it('gives every JS subpath types, import and default', () => {
    expect(jsSubpaths.map(([key]) => key)).toEqual([
      '.',
      './analysis',
      './worklet',
      './worker',
      './vocal-edit-worker',
      './module',
      './module/analysis',
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

  it('exports each module factory beside the binary it builds, typed as the moduleFactory option', () => {
    expect(pkg.exports['./module']).toEqual({
      types: './dist/module.d.ts',
      import: './dist/sonare.js',
      default: './dist/sonare.js',
    });
    expect(pkg.exports['./module/analysis']).toEqual({
      types: './dist/module_analysis.d.ts',
      import: './dist/sonare-analysis.js',
      default: './dist/sonare-analysis.js',
    });
    for (const source of ['module.ts', 'module_analysis.ts']) {
      expect(readFileSync(join(root, 'src', source), 'utf8')).toContain(
        'const moduleFactory: ModuleFactory =',
      );
    }
  });

  it('reaches every package file the docs and refusals tell a consumer to load', () => {
    const exportKeys = new Set(Object.keys(pkg.exports));
    const targets = new Set(
      Object.values(pkg.exports).flatMap((value) =>
        typeof value === 'string' ? [value] : Object.values(value),
      ),
    );
    const sources = [join(root, 'README.md'), ...listFiles(join(root, 'src'), '.ts')];
    const specifiers = new Set<string>();
    const files = new Set<string>();
    for (const file of sources) {
      const text = readFileSync(file, 'utf8');
      for (const match of text.matchAll(/@libraz\/libsonare((?:\/[\w.-]+)*)/g)) {
        specifiers.add(`.${match[1]}`);
      }
      for (const match of text.matchAll(
        /\b(sonare(?:-analysis)?\.(?:js|wasm)|(?:vocal_edit_)?worker\.js|[\w-]+\.shrf|[\w-]+\.schema\.json)\b/g,
      )) {
        files.add(match[1]);
      }
    }
    expect(specifiers.size).toBeGreaterThan(4);
    for (const specifier of specifiers) {
      expect(exportKeys, `${specifier} is named but not exported`).toContain(specifier);
    }
    expect(files.size).toBeGreaterThan(4);
    const targetNames = new Set([...targets].map((target) => target.split('/').pop()));
    for (const name of files) {
      expect(targetNames, `${name} is named but no export reaches it`).toContain(name);
    }
  });

  it('ships no dynamic import whose specifier a bundler or dev server cannot read', () => {
    // A non-literal import() is rewritten by a dev server into a call an AudioWorklet cannot run.
    const specifiers: string[] = [];
    for (const file of listFiles(join(root, 'dist'), '.js')) {
      const source = readFileSync(file, 'utf8');
      const name = file.slice(join(root, 'dist').length + 1);
      for (const match of source.matchAll(/(?<![\w$.])import\(/g)) {
        const rest = source
          .slice(match.index + match[0].length)
          .replace(/^(\s|\/\*[\s\S]*?\*\/)*/, '');
        const literal = /^(["'])([^"'\\]*)\1\s*\)/.exec(rest);
        expect(literal, `${name}: import(${rest.slice(0, 40)}`).not.toBeNull();
        specifiers.push(`${name} -> ${literal?.[2]}`);
      }
    }
    expect(specifiers.sort()).toEqual([
      'analysis.js -> ./sonare-analysis.js',
      'index.js -> ./sonare.js',
      'vocal_edit_worker.js -> ./sonare.js',
      'worker.js -> ./sonare.js',
      'worklet.js -> ./sonare.js',
    ]);
  });

  it('declares the Node floor that supports require() of ESM', () => {
    const match = /^>=(\d+)\.(\d+)\.(\d+)$/.exec(pkg.engines.node);
    expect(match).not.toBeNull();
    const [major, minor] = [Number(match?.[1]), Number(match?.[2])];
    expect(major > 22 || (major === 22 && minor >= 12)).toBe(true);
  });
});
