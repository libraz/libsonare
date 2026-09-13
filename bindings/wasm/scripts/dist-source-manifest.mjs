/**
 * @file Content attestation for everything in `dist/` that a test imports.
 *
 * `dist/` is gitignored and is written by two independent builds — a POST_BUILD
 * copy from whichever CMake tree ran last, and tsup — so nothing in the
 * repository records which sources a given artifact was produced from. A test
 * run against an older copy is deterministic and green in both directions: a
 * fixed defect reads as unfixed, an unfixed one reads as fixed.
 *
 * Each build writes `dist/<module>.sources.json` naming every repository file
 * its module is built from, with its SHA-256. Freshness is then a content
 * comparison against the working tree, not an mtime ordering: a checkout
 * rewrites mtimes on files whose content never changed, so an ordering rule
 * fires on a clean clone, and a rebuilt-but-not-recopied tree defeats it in the
 * other direction. A module attests only itself — widening the attested set
 * past what the build actually refreshed is the false pass this exists to
 * prevent. The writer and the verifier are this one module, so the two sides
 * cannot drift apart.
 */

import { createHash } from 'node:crypto';
import { readdir, readFile, stat, writeFile } from 'node:fs/promises';
import { join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = resolve(fileURLToPath(new URL('../../../', import.meta.url)));
const DIST_DIR = resolve(fileURLToPath(new URL('../dist/', import.meta.url)));

// The full bundle compiles 476 of the 495 translation units under src/, so the
// attested set is the source roots whole rather than a link-closure subset:
// over-broad costs a rebuild, under-broad costs a silent pass.
const NATIVE_SOURCES = {
  roots: ['src', 'include', 'third_party'],
  files: ['CMakeLists.txt'],
  ignore: ['.md'],
};

/** tsup bundles four entries under `src/`; the cases beside them are not bundled. */
const BUNDLE_SOURCES = {
  roots: ['bindings/wasm/src'],
  files: ['bindings/wasm/tsup.config.ts', 'bindings/wasm/tsconfig.json'],
  ignore: ['.md', '.test.ts'],
};

/** Every `dist/` artifact a test imports, grouped by the build that writes it. */
export const MODULES = [
  {
    name: 'sonare',
    primary: 'sonare.wasm',
    artifacts: ['sonare.js', 'sonare.wasm'],
    sources: NATIVE_SOURCES,
    rebuild: 'yarn build:wasm',
  },
  {
    name: 'sonare-analysis',
    primary: 'sonare-analysis.wasm',
    artifacts: ['sonare-analysis.js', 'sonare-analysis.wasm'],
    sources: NATIVE_SOURCES,
    rebuild: 'yarn build:wasm',
  },
  {
    name: 'bundle',
    primary: 'index.js',
    artifacts: ['index.js', 'analysis.js', 'worklet.js', 'worker.js'],
    sources: BUNDLE_SOURCES,
    rebuild: 'yarn build:js',
  },
];

const MAX_LISTED = 10;

async function sha256(path) {
  return createHash('sha256').update(await readFile(path)).digest('hex');
}

async function walk(directory, ignore, out) {
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      await walk(path, ignore, out);
    } else if (entry.isFile() && !ignore.some((suffix) => entry.name.endsWith(suffix))) {
      out.push(path);
    }
  }
}

const sourceCache = new Map();

/**
 * Every file @p spec attests, keyed by its POSIX path relative to the repo
 * root. A file added later reaches the build only through an edit to a file
 * already here -- a source list or an import -- so the set stays closed.
 */
export async function collectSources(spec) {
  const cached = sourceCache.get(spec);
  if (cached !== undefined) return cached;
  const absolute = [];
  for (const root of spec.roots) {
    await walk(join(REPO_ROOT, root), spec.ignore, absolute);
  }
  for (const file of spec.files) {
    absolute.push(join(REPO_ROOT, file));
  }
  const sources = {};
  for (const path of absolute.sort()) {
    sources[relative(REPO_ROOT, path).split(sep).join('/')] = await sha256(path);
  }
  sourceCache.set(spec, sources);
  return sources;
}

async function fileState(path) {
  try {
    const info = await stat(path);
    return { sha256: await sha256(path), bytes: info.size };
  } catch {
    return null;
  }
}

export function manifestPath(moduleName) {
  return join(DIST_DIR, `${moduleName}.sources.json`);
}

/**
 * Writes a manifest for each named module, from the artifacts currently in
 * `dist/`. A build calls this for the module it just produced and no other.
 */
export async function writeManifests(names) {
  const written = [];
  for (const module of MODULES.filter((candidate) => names.includes(candidate.name))) {
    const artifacts = {};
    let complete = true;
    for (const artifact of module.artifacts) {
      const state = await fileState(join(DIST_DIR, artifact));
      if (state === null) {
        complete = false;
        break;
      }
      artifacts[artifact] = { sha256: state.sha256, bytes: state.bytes };
    }
    if (!complete) continue;
    const manifest = {
      module: module.name,
      builtAt: new Date().toISOString(),
      artifacts,
      sources: await collectSources(module.sources),
    };
    await writeFile(manifestPath(module.name), `${JSON.stringify(manifest, null, 2)}\n`);
    written.push(module.name);
  }
  return written;
}

function summarize(paths) {
  const shown = paths.slice(0, MAX_LISTED).map((path) => `  ${path}`);
  if (paths.length > MAX_LISTED) shown.push(`  ... and ${paths.length - MAX_LISTED} more`);
  return shown.join('\n');
}

async function verifyModule(module) {
  const present = [];
  for (const artifact of module.artifacts) {
    const state = await fileState(join(DIST_DIR, artifact));
    if (state !== null) present.push({ artifact, state });
  }
  // A module nothing built is not this check's business; importing it fails on its own.
  if (present.length === 0) return null;

  const hint = `Run \`${module.rebuild}\` in bindings/wasm.`;
  let manifest;
  try {
    manifest = JSON.parse(await readFile(manifestPath(module.name), 'utf8'));
  } catch {
    return {
      problem: `dist/${module.primary} has no source manifest (dist/${module.name}.sources.json), so the sources it was built from are unknown.\n${hint}`,
    };
  }

  for (const { artifact, state } of present) {
    const recorded = manifest.artifacts?.[artifact];
    if (recorded === undefined) {
      return { problem: `dist/${artifact} is not covered by dist/${module.name}.sources.json.\n${hint}` };
    }
    if (recorded.sha256 !== state.sha256) {
      return {
        problem: `dist/${artifact} was replaced after its source manifest was written (manifest ${recorded.sha256.slice(0, 12)}, on disk ${state.sha256.slice(0, 12)}).\n${hint}`,
      };
    }
  }

  const sources = await collectSources(module.sources);
  const drifted = [];
  for (const [path, digest] of Object.entries(manifest.sources ?? {})) {
    if (sources[path] !== digest) drifted.push(path);
  }
  const total = Object.keys(manifest.sources ?? {}).length;
  if (drifted.length > 0) {
    return {
      problem: `dist/${module.primary} is stale: ${drifted.length} of ${total} sources it was built from have changed.\n${summarize(drifted.sort())}\n${hint}`,
    };
  }

  const digests = Object.fromEntries(present.map(({ artifact, state }) => [artifact, state.sha256]));
  return {
    provenance: `${module.primary} sha256 ${digests[module.primary]?.slice(0, 12) ?? 'absent'} built ${manifest.builtAt}, ${total} sources verified`,
    artifacts: digests,
  };
}

/**
 * Verifies every built module against the working tree. Returns the problems
 * found and, for the modules that passed, the artifact digests read.
 */
export async function verifyAll() {
  const problems = [];
  const provenance = [];
  const artifacts = {};
  for (const module of MODULES) {
    const result = await verifyModule(module);
    if (result === null) continue;
    if (result.problem !== undefined) {
      problems.push(result.problem);
      continue;
    }
    provenance.push(result.provenance);
    Object.assign(artifacts, result.artifacts);
  }
  return { problems, provenance, artifacts };
}

/** Current digest of each named artifact, for a before/after comparison. */
export async function readArtifacts(names) {
  const current = {};
  for (const name of names) {
    const state = await fileState(join(DIST_DIR, name));
    current[name] = state === null ? null : state.sha256;
  }
  return current;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const writeIndex = process.argv.indexOf('--write');
  if (writeIndex !== -1) {
    const names = process.argv.slice(writeIndex + 1);
    const unknown = names.filter((name) => !MODULES.some((module) => module.name === name));
    if (names.length === 0 || unknown.length > 0) {
      console.error(`Usage: --write <${MODULES.map((module) => module.name).join('|')}> ...`);
      process.exit(2);
    }
    const written = await writeManifests(names);
    const missing = names.filter((name) => !written.includes(name));
    if (missing.length > 0) {
      console.error(`dist/ has no complete build of ${missing.join(', ')}.`);
      process.exit(1);
    }
    console.log(`Wrote source manifests for ${written.join(', ')}.`);
  } else {
    const { problems, provenance } = await verifyAll();
    for (const line of provenance) console.log(line);
    if (problems.length > 0) {
      console.error(problems.join('\n\n'));
      process.exit(1);
    }
  }
}
