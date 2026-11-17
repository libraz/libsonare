/**
 * @file Freshness of the two build outputs the Node tests load.
 *
 * The native addon (`build/Release/sonare-node.node`) and the tsc output in
 * `dist/` are both gitignored, so nothing records which sources they came from.
 * A run against an older one is deterministic and green in both directions.
 * Each output is therefore compared with the newest source it is built from:
 * the addon with the core sources (the one definition the Python test guard
 * uses) plus the addon glue, `dist/` with the TypeScript sources.
 *
 * `SONARE_ALLOW_STALE_ADDON=1` accepts an addon built from older native sources
 * (a TypeScript change tested while native sources are mid-edit), and the
 * provenance line says so. `dist/` and the mid-run check are never relaxed.
 */

import { createHash } from 'node:crypto';
import { readdir, readFile, stat } from 'node:fs/promises';
import { join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const PACKAGE_ROOT = resolve(fileURLToPath(new URL('../', import.meta.url)));
const REPO_ROOT = resolve(PACKAGE_ROOT, '../..');

/** Test-run opt-out for an addon built from older native sources; `=1` enables it. */
export const ALLOW_STALE_ENV = 'SONARE_ALLOW_STALE_ADDON';

/** The core-source definition shared with the Python test guard. */
export const CORE_SOURCES_PATH = 'tools/abi/core-sources.json';

/** The core-source definition: `suffixes`, `trees` and `excluded_trees`, repo-relative. */
export async function loadCoreSources(repoRoot = REPO_ROOT) {
  return JSON.parse(await readFile(join(repoRoot, CORE_SOURCES_PATH), 'utf8'));
}

/**
 * The outputs checked, each with the trees whose newest file it must postdate.
 * `staleOptOut` marks the one a run may knowingly accept stale.
 */
export async function defaultOutputs(repoRoot = REPO_ROOT) {
  const core = await loadCoreSources(repoRoot);
  return [
    {
      artifact: 'bindings/node/build/Release/sonare-node.node',
      sources: {
        suffixes: core.suffixes,
        trees: [...core.trees, 'bindings/node/src/addon'],
        excluded: core.excluded_trees,
      },
      rebuild: 'yarn build:native',
      staleOptOut: true,
    },
    {
      artifact: 'bindings/node/dist/index.js',
      sources: {
        suffixes: ['.ts'],
        trees: ['bindings/node/src'],
        excluded: ['bindings/node/src/addon'],
      },
      rebuild: 'yarn build:ts',
    },
  ];
}

/** The newest file under `spec.trees` with one of `spec.suffixes`, as `{ mtimeMs, path }`. */
export async function newestSource(spec, repoRoot = REPO_ROOT) {
  const excluded = new Set(spec.excluded.map((tree) => resolve(repoRoot, tree)));
  let newest = null;
  const walk = async (directory) => {
    let entries;
    try {
      entries = await readdir(directory, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) {
        if (!excluded.has(path)) await walk(path);
      } else if (entry.isFile() && spec.suffixes.some((suffix) => entry.name.endsWith(suffix))) {
        const { mtimeMs } = await stat(path);
        if (newest === null || mtimeMs > newest.mtimeMs) newest = { mtimeMs, path };
      }
    }
  };
  for (const tree of spec.trees) await walk(resolve(repoRoot, tree));
  return newest;
}

async function digest(path) {
  try {
    return createHash('sha256').update(await readFile(path)).digest('hex');
  } catch {
    return null;
  }
}

/**
 * Checks every output against its sources. Returns one problem line per stale
 * output, a provenance line per output used, and the digests read, for the
 * mid-run comparison. An output that was never built is not judged: importing
 * it fails on its own.
 */
export async function verifyOutputs({ allowStale = false, repoRoot = REPO_ROOT, outputs } = {}) {
  const checked = outputs ?? (await defaultOutputs(repoRoot));
  const problems = [];
  const provenance = [];
  const artifacts = {};
  for (const output of checked) {
    const path = resolve(repoRoot, output.artifact);
    let info;
    try {
      info = await stat(path);
    } catch {
      continue;
    }
    const newest = await newestSource(output.sources, repoRoot);
    const sha = await digest(path);
    const built = new Date(info.mtimeMs).toISOString();
    const head = `${output.artifact} sha256 ${sha?.slice(0, 12)} built ${built}`;
    if (newest !== null && info.mtimeMs < newest.mtimeMs) {
      const source = relative(repoRoot, newest.path).split(sep).join('/');
      if (allowStale && output.staleOptOut === true) {
        provenance.push(`${head}, ACCEPTED STALE (${ALLOW_STALE_ENV}=1): older than ${source}`);
        artifacts[path] = sha;
        continue;
      }
      problems.push(
        `${output.artifact} is stale: older than ${source}. Run \`${output.rebuild}\` in bindings/node.`,
      );
      continue;
    }
    provenance.push(`${head}, newer than every source`);
    artifacts[path] = sha;
  }
  return { problems, provenance, artifacts };
}

/** Current digest of each artifact path, for a before/after comparison. */
export async function readDigests(paths) {
  const current = {};
  for (const path of paths) current[path] = await digest(path);
  return current;
}
