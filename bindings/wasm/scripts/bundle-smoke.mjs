// Bundles the published entry points for the browser with webpack, esbuild and
// Vite (Rolldown) and fails on any error or warning. The emscripten glue and the
// worker URLs are the parts a browser bundler trips on, and no unit test reads
// them. Each bundler stands for consumers who build with it, not for our build,
// so a leg is dropped only once consumers stop building with that bundler.

import { mkdir, mkdtemp, readdir, rm, symlink, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import * as esbuild from 'esbuild';
import { build as viteBuild, createLogger } from 'vite';
import webpack from 'webpack';

const root = path.resolve(new URL('..', import.meta.url).pathname);
const entries = {
  root: "import { init } from '@libraz/libsonare';\nawait init();\n",
  analysis: "import { init } from '@libraz/libsonare/analysis';\nawait init();\n",
  worklet:
    "import { init, registerSonareWorkletProcessor } from '@libraz/libsonare/worklet';\n" +
    'await init();\nregisterSonareWorkletProcessor();\n',
  workers:
    "import { OfflineWorkerClient, VocalEditWorkerClient } from '@libraz/libsonare';\n" +
    'globalThis.clients = [new OfflineWorkerClient(), new VocalEditWorkerClient()];\n',
};

/** Worker scripts an entry must carry into the output as their own chunks. */
const expectedWorkers = { workers: ['worker', 'vocal_edit_worker'] };

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

async function runVite(dir, name) {
  const problems = [];
  const logger = createLogger('warn', { allowClearScreen: false });
  logger.warn = (message) => problems.push(message);
  logger.warnOnce = (message) => problems.push(message);
  logger.error = (message) => problems.push(message);
  const outDir = path.join(dir, 'out-vite', name);
  await viteBuild({
    root: dir,
    configFile: false,
    customLogger: logger,
    logLevel: 'warn',
    build: {
      outDir,
      emptyOutDir: true,
      target: 'esnext',
      // The wasm asset is megabytes by design; the size hint is not a bundling defect.
      chunkSizeWarningLimit: Number.MAX_SAFE_INTEGER,
      rolldownOptions: { input: path.join(dir, `${name}.mjs`) },
    },
    worker: { format: 'es' },
  });
  const emitted = await readdir(path.join(outDir, 'assets'));
  for (const worker of expectedWorkers[name] ?? []) {
    if (!emitted.some((file) => file.startsWith(`${worker}-`) && file.endsWith('.js'))) {
      problems.push(`no ${worker} chunk among ${emitted.join(', ')}`);
    }
  }
  return problems;
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
      ['vite', runVite],
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
console.log(`Browser bundle smoke passed: ${Object.keys(entries).length} entries x webpack, esbuild, vite`);
