// Bundles the published entry points for the browser with webpack and esbuild
// and fails on any error or warning. The emscripten glue and the worker URLs
// are the parts a browser bundler trips on, and no unit test reads them.

import { mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import * as esbuild from 'esbuild';
import webpack from 'webpack';

const root = path.resolve(new URL('..', import.meta.url).pathname);
const entries = {
  root: "import { init } from '@libraz/libsonare';\nawait init();\n",
  analysis: "import { init } from '@libraz/libsonare/analysis';\nawait init();\n",
};

async function runEsbuild(dir, name) {
  const result = await esbuild.build({
    absWorkingDir: dir,
    entryPoints: [path.join(dir, `${name}.mjs`)],
    bundle: true,
    platform: 'browser',
    format: 'esm',
    outdir: path.join(dir, 'out-esbuild', name),
    loader: { '.wasm': 'file' },
    logLevel: 'silent',
  });
  return result.warnings.map((warning) => warning.text);
}

function runWebpack(dir, name) {
  return new Promise((resolve, reject) => {
    webpack(
      {
        context: dir,
        mode: 'production',
        target: 'web',
        entry: path.join(dir, `${name}.mjs`),
        output: { path: path.join(dir, 'out-webpack', name) },
        experiments: { topLevelAwait: true },
        // The wasm asset is megabytes by design; the size hint is not a bundling defect.
        performance: { hints: false },
      },
      (error, stats) => {
        if (error) {
          reject(error);
          return;
        }
        const info = stats.toJson({ all: false, errors: true, warnings: true });
        const problems = [...info.errors, ...info.warnings].map((item) => item.message);
        resolve(problems);
      },
    );
  });
}

const dir = await mkdtemp(path.join(os.tmpdir(), 'libsonare-bundle-'));
const failures = [];
try {
  await mkdir(path.join(dir, 'node_modules', '@libraz'), { recursive: true });
  await symlink(root, path.join(dir, 'node_modules', '@libraz', 'libsonare'), 'dir');
  for (const [name, source] of Object.entries(entries)) {
    await writeFile(path.join(dir, `${name}.mjs`), source);
    for (const [bundler, run] of [
      ['esbuild', runEsbuild],
      ['webpack', runWebpack],
    ]) {
      try {
        const problems = await run(dir, name);
        for (const problem of problems) {
          failures.push(`${bundler} ${name}: ${problem}`);
        }
      } catch (error) {
        failures.push(`${bundler} ${name}: ${error.message ?? error}`);
      }
    }
  }
} finally {
  await rm(dir, { recursive: true, force: true });
}

if (failures.length > 0) {
  console.error(`Browser bundle smoke failed:\n${failures.join('\n\n')}`);
  process.exit(1);
}
console.log(`Browser bundle smoke passed: ${Object.keys(entries).length} entries x webpack, esbuild`);
