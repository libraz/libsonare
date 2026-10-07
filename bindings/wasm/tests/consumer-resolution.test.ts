/**
 * The published declarations type-check from a consumer project under the
 * resolution modes consumers actually use. The package is linked into a scratch
 * `node_modules` and `tsc` runs with `skipLibCheck` off, because a relative
 * specifier that only resolves under `bundler` resolution degrades every type
 * behind it to `any` when the check is skipped. Requires a built `dist/`.
 */

import { execFileSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const tsc = join(root, 'node_modules', '.bin', 'tsc');
const subpaths = ['', '/analysis', '/worklet', '/worker', '/vocal-edit-worker'];

let scratch = '';

beforeAll(() => {
  scratch = mkdtempSync(join(tmpdir(), 'libsonare-consumer-'));
  mkdirSync(join(scratch, 'node_modules', '@libraz'), { recursive: true });
  symlinkSync(root, join(scratch, 'node_modules', '@libraz', 'libsonare'), 'dir');
});

afterAll(() => {
  rmSync(scratch, { recursive: true, force: true });
});

/** Runs tsc over `files` with `options` merged into a strict base; returns its output, empty when clean. */
function typeCheck(files: Record<string, string>, options: Record<string, unknown>): string {
  for (const [name, text] of Object.entries(files)) {
    writeFileSync(join(scratch, name), text);
  }
  const compilerOptions = {
    noEmit: true,
    strict: true,
    skipLibCheck: false,
    lib: ['ES2022', 'DOM', 'ESNext.Disposable'],
    types: [],
    ...options,
  };
  writeFileSync(
    join(scratch, 'tsconfig.json'),
    JSON.stringify({ compilerOptions, files: Object.keys(files) }),
  );
  try {
    execFileSync(tsc, ['-p', 'tsconfig.json'], { cwd: scratch, encoding: 'utf8', stdio: 'pipe' });
    return '';
  } catch (error) {
    return String((error as { stdout?: string }).stdout ?? error);
  }
}

const importAll = subpaths
  .map(
    (sub, i) =>
      `import type * as m${i} from '@libraz/libsonare${sub}';\nexport type T${i} = typeof m${i};`,
  )
  .join('\n');

describe('published declarations from a consumer project', () => {
  it('resolve every subpath under nodenext from ESM and CommonJS', { timeout: 60000 }, () => {
    const output = typeCheck(
      {
        'esm.mts': importAll,
        'cjs.cts': `import type * as root from '@libraz/libsonare';\nimport type * as analysis from '@libraz/libsonare/analysis';\nexport type R = typeof root;\nexport type A = typeof analysis;\n`,
      },
      { module: 'nodenext', moduleResolution: 'nodenext' },
    );
    expect(output).toBe('');
  });

  // node10 resolution is removed in TypeScript 7; retire this case with that upgrade.
  it('resolve every subpath under node10 through typesVersions (until TypeScript 7)', {
    timeout: 60000,
  }, () => {
    const output = typeCheck(
      { 'legacy.ts': importAll },
      {
        module: 'commonjs',
        moduleResolution: 'node10',
        ignoreDeprecations: '6.0',
      },
    );
    expect(output).toBe('');
  });
});
