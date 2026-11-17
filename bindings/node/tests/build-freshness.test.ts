import { readFileSync } from 'node:fs';
import { mkdir, mkdtemp, rm, utimes, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import {
  ALLOW_STALE_ENV,
  defaultOutputs,
  loadCoreSources,
  verifyOutputs,
} from '../scripts/build-freshness.mjs';

const OLD = new Date('2026-01-01T00:00:00Z');
const NEW = new Date('2026-01-02T00:00:00Z');

let root: string;

async function file(path: string, when: Date): Promise<void> {
  await mkdir(join(root, path, '..'), { recursive: true });
  await writeFile(join(root, path), path);
  await utimes(join(root, path), when, when);
}

const outputs = () => [
  {
    artifact: 'addon.node',
    sources: { suffixes: ['.cpp'], trees: ['native'], excluded: ['native/wasm'] },
    rebuild: 'rebuild addon',
    staleOptOut: true,
  },
  {
    artifact: 'dist/index.js',
    sources: { suffixes: ['.ts'], trees: ['ts'], excluded: [] },
    rebuild: 'rebuild dist',
  },
];

beforeEach(async () => {
  root = await mkdtemp(join(tmpdir(), 'sonare-node-freshness-'));
  await file('native/a.cpp', OLD);
  await file('ts/index.ts', OLD);
  await file('addon.node', NEW);
  await file('dist/index.js', NEW);
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

describe('node build freshness', () => {
  it('is wired as the vitest global setup', () => {
    const config = readFileSync(new URL('../vitest.config.ts', import.meta.url), 'utf8');
    expect(config).toContain("globalSetup: ['./scripts/build-freshness-setup.mjs']");
  });

  it('passes outputs newer than every source', async () => {
    const { problems, provenance } = await verifyOutputs({ repoRoot: root, outputs: outputs() });
    expect(problems).toEqual([]);
    expect(provenance).toHaveLength(2);
  });

  it('refuses an addon older than a native source, naming the source, one line per output', async () => {
    await file('native/b.cpp', new Date('2026-01-03T00:00:00Z'));
    await file('ts/later.ts', new Date('2026-01-03T00:00:00Z'));
    const { problems } = await verifyOutputs({ repoRoot: root, outputs: outputs() });
    expect(problems).toEqual([
      'addon.node is stale: older than native/b.cpp. Run `rebuild addon` in bindings/node.',
      'dist/index.js is stale: older than ts/later.ts. Run `rebuild dist` in bindings/node.',
    ]);
  });

  it('ignores a newer file outside the suffixes and inside an excluded tree', async () => {
    await file('native/notes.md', new Date('2026-01-03T00:00:00Z'));
    await file('native/wasm/glue.cpp', new Date('2026-01-03T00:00:00Z'));
    const { problems } = await verifyOutputs({ repoRoot: root, outputs: outputs() });
    expect(problems).toEqual([]);
  });

  it(`accepts a stale addon under ${ALLOW_STALE_ENV} and says so, but never a stale dist`, async () => {
    await file('native/b.cpp', new Date('2026-01-03T00:00:00Z'));
    await file('ts/later.ts', new Date('2026-01-03T00:00:00Z'));
    const { problems, provenance } = await verifyOutputs({
      allowStale: true,
      repoRoot: root,
      outputs: outputs(),
    });
    expect(problems).toEqual([
      'dist/index.js is stale: older than ts/later.ts. Run `rebuild dist` in bindings/node.',
    ]);
    expect(provenance).toHaveLength(1);
    expect(provenance[0]).toContain(
      `ACCEPTED STALE (${ALLOW_STALE_ENV}=1): older than native/b.cpp`,
    );
  });

  it('leaves an output that was never built to fail on import', async () => {
    await rm(join(root, 'addon.node'));
    const { problems, provenance } = await verifyOutputs({ repoRoot: root, outputs: outputs() });
    expect(problems).toEqual([]);
    expect(provenance).toHaveLength(1);
  });

  it('judges the addon by the shared core-source definition plus the addon glue', async () => {
    const core = await loadCoreSources();
    const [addon] = await defaultOutputs();
    expect(addon.sources.suffixes).toEqual(core.suffixes);
    expect(addon.sources.excluded).toEqual(core.excluded_trees);
    expect(addon.sources.trees).toEqual([...core.trees, 'bindings/node/src/addon']);
  });
});
