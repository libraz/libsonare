/**
 * The package ships the WASM migration ledger as `@libraz/libsonare/migration.json`:
 * the ledger's entries, field for field, without the in-repo `test` pointer.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { parse } from 'smol-toml';
import { describe, expect, it } from 'vitest';

const PACKAGE_ROOT = new URL('../', import.meta.url);

describe('the shipped migration ledger', () => {
  it('equals the ledger with only its test field removed', () => {
    const ledger = parse(
      readFileSync(new URL('../../tools/api/migration/wasm.toml', PACKAGE_ROOT), 'utf8'),
    ) as { entry: Record<string, unknown>[] };
    const expected = {
      entry: ledger.entry.map(({ test: _test, ...rest }) => rest),
    };
    const shipped = JSON.parse(readFileSync(new URL('dist/migration.json', PACKAGE_ROOT), 'utf8'));
    expect(shipped.entry.length).toBeGreaterThan(0);
    expect(shipped).toEqual(expected);
  });

  it('is exported and packed', () => {
    const manifest = JSON.parse(readFileSync(new URL('package.json', PACKAGE_ROOT), 'utf8'));
    expect(manifest.exports['./migration.json']).toBe('./dist/migration.json');
    const listing = execFileSync('yarn', ['pack', '--dry-run', '--json'], {
      cwd: PACKAGE_ROOT,
      encoding: 'utf8',
    });
    const packed = listing
      .split('\n')
      .filter((line) => line.startsWith('{'))
      .map((line) => (JSON.parse(line) as { location?: string }).location);
    expect(packed).toContain('dist/migration.json');
  }, 60_000);
});
