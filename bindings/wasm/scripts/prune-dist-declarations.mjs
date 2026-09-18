/**
 * @file Removes declarations in `dist/` whose source no longer exists.
 *
 * `tsc --emitDeclarationOnly` writes one declaration per module under `src/`
 * and removes none, so a deleted or renamed source leaves its `.d.ts` behind —
 * and `files` ships `dist/` whole, so the orphan reaches the npm tarball. A
 * blanket clean is not available here: the emscripten modules reach `dist/`
 * only through the CMake `POST_BUILD` copy, so deleting the directory costs a
 * WASM toolchain build. This prunes the one family the declaration emit owns,
 * keyed on the source each file would have been emitted from — `src/<name>.ts`
 * for a compiled module, `src/<name>.js.d.ts` for a module shim, which
 * `copy-module-types.mjs` writes from a declaration input tsc never compiles.
 */

import { readdir, rm, stat } from 'node:fs/promises';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const PACKAGE_ROOT = fileURLToPath(new URL('../', import.meta.url));

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

/** Every `.d.ts` / `.d.ts.map` under @p directory, as POSIX paths relative to @p base. */
async function declarationFiles(directory, base = directory) {
  const found = [];
  let entries;
  try {
    entries = await readdir(directory, { withFileTypes: true });
  } catch {
    return found; // No dist/ at all: nothing was emitted, so nothing is orphaned.
  }
  for (const entry of entries) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      found.push(...(await declarationFiles(path, base)));
    } else if (entry.name.endsWith('.d.ts') || entry.name.endsWith('.d.ts.map')) {
      found.push(relative(base, path).split(sep).join('/'));
    }
  }
  return found;
}

/** The sources any of which accounts for `dist/<relative>`. */
function sourcesFor(relativePath) {
  const isMap = relativePath.endsWith('.map');
  const declaration = isMap ? relativePath.slice(0, -'.map'.length) : relativePath;
  const stem = declaration.slice(0, -'.d.ts'.length);
  // A shim carries no declaration map, so only a compiled module accounts for one.
  return isMap ? [`src/${stem}.ts`] : [`src/${stem}.ts`, `src/${stem}.js.d.ts`];
}

/**
 * Deletes every orphaned declaration under `dist/`. Returns how many were
 * examined alongside what was removed, so a run that reached nothing is visible
 * rather than indistinguishable from a clean tree.
 */
export async function pruneDeclarations(root = PACKAGE_ROOT, { dryRun = false } = {}) {
  const dist = join(root, 'dist');
  const files = await declarationFiles(dist);
  const removed = [];
  for (const file of files) {
    const accounted = [];
    for (const source of sourcesFor(file)) {
      if (await exists(join(root, source))) {
        accounted.push(source);
      }
    }
    if (accounted.length > 0) {
      continue;
    }
    if (!dryRun) {
      await rm(join(dist, file));
    }
    removed.push(file);
  }
  return { examined: files.length, removed };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const dryRun = process.argv.includes('--dry-run');
  const { examined, removed } = await pruneDeclarations(PACKAGE_ROOT, { dryRun });
  for (const file of removed) {
    console.log(`${dryRun ? 'Orphaned' : 'Removed orphaned'} dist/${file}`);
  }
  console.log(`Examined ${examined} declarations in dist/, ${removed.length} orphaned.`);
}
