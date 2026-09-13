/**
 * @file Declaration shims in `dist/` for the emscripten modules.
 *
 * Each emscripten artifact is copied into `dist/` by the CMake build and is
 * declared by a hand-written `src/<name>.js.d.ts`. That file is a declaration
 * input, so tsc consumes it rather than emitting `dist/<name>.d.ts`, and every
 * import of `./<name>.js` from inside `dist/` — the emitted declarations, and
 * the tests that load a module directly — resolves to a file nothing writes.
 *
 * A re-export rather than a copy: the declaration pulls further types from
 * `./public_types`, which only resolves next to the source, and the package
 * ships `src/` alongside `dist/`.
 *
 * The module list comes from the build's own manifest, and presence of the
 * declaration input is the selector, so a module added there is shimmed without
 * an edit here. The tsup bundle has no such input and emits its own types.
 */

import { access, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { MODULES } from './dist-source-manifest.mjs';

const PACKAGE_ROOT = new URL('../', import.meta.url);

const shimFor = (name) => `export * from '../src/${name}.js';
export { default } from '../src/${name}.js';
`;

/** The modules under @p root that carry a `src/<name>.js.d.ts` declaration input. */
export async function declaredModules(root = PACKAGE_ROOT, modules = MODULES) {
  const declared = [];
  for (const module of modules) {
    try {
      await access(new URL(`src/${module.name}.js.d.ts`, root));
      declared.push(module.name);
    } catch {
      // No declaration input: nothing in dist/ imports `./<name>.js` as a type.
    }
  }
  return declared;
}

/** Writes `dist/<name>.d.ts` for each declared module. Returns the names written. */
export async function writeShims(root = PACKAGE_ROOT, modules = MODULES) {
  const names = await declaredModules(root, modules);
  for (const name of names) {
    await writeFile(fileURLToPath(new URL(`dist/${name}.d.ts`, root)), shimFor(name));
  }
  return names;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const written = await writeShims();
  console.log(`Wrote declaration shims for ${written.join(', ')}.`);
}
