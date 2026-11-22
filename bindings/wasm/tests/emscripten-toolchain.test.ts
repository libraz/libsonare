/**
 * The Emscripten release a build and a pack are held to.
 *
 * `build:wasm` starts only under the pinned release and the pack step ships only modules whose
 * manifest records it, so a dist from another compiler cannot sit beside the pin unnoticed. Each
 * case runs against an `emcc` stub on PATH, never the toolchain the machine happens to have.
 */

import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { delimiter, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import {
  type DistModule,
  manifestPath,
  verifyToolchain,
  writeManifests,
} from '../scripts/dist-source-manifest.mjs';
import {
  parseEmccVersion,
  pinnedVersion,
  toolchainProblem,
} from '../scripts/emscripten-toolchain.mjs';

const GUARD = fileURLToPath(new URL('../scripts/emscripten-toolchain.mjs', import.meta.url));
const BANNER = (version: string) =>
  `emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) ${version} (ac0578743420bcbe9a4f77d0b6599c25f8d43702)`;

let root: string;

beforeEach(() => {
  root = mkdtempSync(join(tmpdir(), 'sonare-emscripten-toolchain-'));
});

afterEach(() => {
  rmSync(root, { recursive: true, force: true });
});

/** A PATH whose `emcc` prints @p banner, or that has no `emcc` at all when @p banner is null. */
function stubPath(banner: string | null): NodeJS.ProcessEnv {
  const bin = join(root, 'bin');
  rmSync(bin, { recursive: true, force: true });
  mkdirSync(bin, { recursive: true });
  if (banner !== null) {
    const emcc = join(bin, 'emcc');
    writeFileSync(emcc, `#!/bin/sh\necho '${banner}'\n`);
    chmodSync(emcc, 0o755);
  }
  // Only the stub and the system tools: the developer's own emcc must not answer.
  return { ...process.env, PATH: [bin, '/usr/bin', '/bin'].join(delimiter) };
}

function runGuard(env: NodeJS.ProcessEnv) {
  return spawnSync(process.execPath, [GUARD], { env, encoding: 'utf8' });
}

describe('the pinned release', () => {
  it('is one release, which the guard and the manifest both read', () => {
    expect(pinnedVersion()).toMatch(/^\d+\.\d+\.\d+$/);
  });

  it('reads the release token off the banner, suffix included', () => {
    expect(parseEmccVersion(BANNER('7.0.1'))).toBe('7.0.1');
    expect(parseEmccVersion(BANNER('6.0.10-git'))).toBe('6.0.10-git');
    expect(parseEmccVersion('clang version 21')).toBeNull();
  });
});

describe('build:wasm refuses another compiler', () => {
  it('starts under the pinned release', () => {
    const result = runGuard(stubPath(BANNER(pinnedVersion())));
    expect(result.status).toBe(0);
    expect(result.stderr).toBe('');
  });

  it('refuses a different release, naming both and the activation command', () => {
    const pinned = pinnedVersion();
    const result = runGuard(stubPath(BANNER('6.0.10-git')));
    expect(result.status).toBe(1);
    expect(result.stderr).toContain(
      `built with Emscripten ${pinned} (bindings/wasm/.emscripten-version), but emcc on PATH is 6.0.10-git`,
    );
    expect(result.stderr).toContain(`emsdk activate ${pinned}`);
    expect(result.stderr).toContain('emsdk_env.sh');
  });

  it('refuses a development build of the pinned release', () => {
    expect(toolchainProblem({ env: stubPath(BANNER(`${pinnedVersion()}-git`)) })).toContain(
      `emcc on PATH is ${pinnedVersion()}-git`,
    );
  });

  it('refuses a PATH with no emcc', () => {
    const result = runGuard(stubPath(null));
    expect(result.status).toBe(1);
    expect(result.stderr).toContain('but no emcc runs from PATH');
  });
});

describe('the pack step refuses a module from another compiler', () => {
  const modules = (): DistModule[] => [
    {
      name: 'native',
      primary: 'native.wasm',
      artifacts: ['native.wasm'],
      sources: { roots: ['native-src'], files: [], ignore: [] },
      rebuild: 'rebuild native',
      toolchain: 'emscripten',
    },
  ];
  let dirs: { distDir: string; repoRoot: string };

  beforeEach(async () => {
    dirs = { distDir: join(root, 'dist'), repoRoot: join(root, 'repo') };
    await mkdir(join(dirs.repoRoot, 'native-src'), { recursive: true });
    await mkdir(dirs.distDir);
    await writeFile(join(dirs.repoRoot, 'native-src', 'a.cpp'), 'int a;\n');
    await writeFile(join(dirs.distDir, 'native.wasm'), 'wasm');
  });

  const record = () =>
    JSON.parse(readFileSync(manifestPath('native', dirs.distDir), 'utf8')).toolchain;

  it('records the release that built the module and passes it under the same pin', async () => {
    await writeManifests(['native'], {
      ...dirs,
      modules: modules(),
      emscriptenVersion: () => '7.0.1',
    });
    expect(record()).toEqual({ emscripten: '7.0.1' });
    expect(
      await verifyToolchain({ distDir: dirs.distDir, modules: modules(), pinned: '7.0.1' }),
    ).toEqual([]);
  });

  it('refuses a module built with another release', async () => {
    await writeManifests(['native'], {
      ...dirs,
      modules: modules(),
      emscriptenVersion: () => '6.0.10-git',
    });
    const problems = await verifyToolchain({
      distDir: dirs.distDir,
      modules: modules(),
      pinned: '7.0.1',
    });
    expect(problems).toHaveLength(1);
    expect(problems[0]).toContain(
      'dist/native.wasm was built with Emscripten 6.0.10-git, but the package is built with 7.0.1',
    );
    expect(problems[0]).toContain('rebuild native');
  });

  it('refuses a manifest that records no release', async () => {
    await writeFile(
      manifestPath('native', dirs.distDir),
      JSON.stringify({ module: 'native', artifacts: {}, sources: {} }),
    );
    const problems = await verifyToolchain({
      distDir: dirs.distDir,
      modules: modules(),
      pinned: '7.0.1',
    });
    expect(problems).toEqual([
      expect.stringContaining('dist/native.wasm records no Emscripten release'),
    ]);
  });

  it('refuses to write a native manifest when no compiler can be asked', async () => {
    await expect(
      writeManifests(['native'], { ...dirs, modules: modules(), emscriptenVersion: () => null }),
    ).rejects.toThrow('no emcc runs from PATH');
  });
});
