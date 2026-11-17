import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { type DistModule, verifyAll, writeManifests } from '../scripts/dist-source-manifest.mjs';

// Source digests are cached per spec object for the life of a process, so the
// manifest writer and each verification get their own module objects.
function modules(): DistModule[] {
  return [
    {
      name: 'native',
      primary: 'native.wasm',
      artifacts: ['native.wasm'],
      sources: { roots: ['native-src'], files: [], ignore: [] },
      rebuild: 'rebuild native',
      staleOptOut: true,
    },
    {
      name: 'bundle',
      primary: 'index.js',
      artifacts: ['index.js'],
      sources: { roots: ['ts-src'], files: [], ignore: [] },
      rebuild: 'rebuild bundle',
    },
  ];
}

let root: string;
let dirs: { distDir: string; repoRoot: string };
const ctx = () => ({ ...dirs, modules: modules() });

beforeEach(async () => {
  root = await mkdtemp(join(tmpdir(), 'sonare-dist-freshness-'));
  const repoRoot = join(root, 'repo');
  const distDir = join(root, 'dist');
  await mkdir(join(repoRoot, 'native-src'), { recursive: true });
  await mkdir(join(repoRoot, 'ts-src'), { recursive: true });
  await mkdir(distDir);
  await writeFile(join(repoRoot, 'native-src', 'a.cpp'), 'int a;\n');
  await writeFile(join(repoRoot, 'ts-src', 'index.ts'), 'export {};\n');
  await writeFile(join(distDir, 'native.wasm'), 'wasm');
  await writeFile(join(distDir, 'index.js'), 'js');
  dirs = { distDir, repoRoot };
  expect(await writeManifests(['native', 'bundle'], ctx())).toEqual(['native', 'bundle']);
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

describe('dist freshness opt-out', () => {
  it('refuses a native module built from older sources by default', async () => {
    await writeFile(join(dirs.repoRoot, 'native-src', 'a.cpp'), 'int a = 1;\n');
    const { problems } = await verifyAll(ctx());
    expect(problems).toHaveLength(1);
    expect(problems[0]).toContain('dist/native.wasm is stale');
  });

  it('accepts that native module under allowStale and names it on the provenance line', async () => {
    await writeFile(join(dirs.repoRoot, 'native-src', 'a.cpp'), 'int a = 1;\n');
    const { problems, provenance, artifacts } = await verifyAll({ ...ctx(), allowStale: true });
    expect(problems).toEqual([]);
    expect(provenance.find((line) => line.startsWith('native.wasm'))).toContain(
      'ACCEPTED STALE (SONARE_ALLOW_STALE_DIST=1): 1 of 1 sources changed',
    );
    expect(Object.keys(artifacts).sort()).toEqual(['index.js', 'native.wasm']);
  });

  it('keeps refusing a stale bundle under allowStale', async () => {
    await writeFile(join(dirs.repoRoot, 'ts-src', 'index.ts'), 'export const x = 1;\n');
    const { problems } = await verifyAll({ ...ctx(), allowStale: true });
    expect(problems).toHaveLength(1);
    expect(problems[0]).toContain('dist/index.js is stale');
  });

  it('keeps refusing a native artifact replaced after its manifest under allowStale', async () => {
    await writeFile(join(dirs.distDir, 'native.wasm'), 'rebuilt');
    const { problems } = await verifyAll({ ...ctx(), allowStale: true });
    expect(problems).toHaveLength(1);
    expect(problems[0]).toContain('was replaced after its source manifest was written');
  });

  it('passes both modules when nothing drifted', async () => {
    const { problems, provenance } = await verifyAll(ctx());
    expect(problems).toEqual([]);
    expect(provenance).toHaveLength(2);
    expect(provenance.join('\n')).not.toContain('ACCEPTED STALE');
  });
});
