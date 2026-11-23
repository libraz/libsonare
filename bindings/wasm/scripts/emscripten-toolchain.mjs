/**
 * @file The Emscripten release the WASM modules are built with, and the guard holding a build to it.
 *
 * `bindings/wasm/.emscripten-version` is the one tracked statement of that release; the CI setup
 * steps read it. Any other release builds a module that passes every test while contradicting the
 * pin, so `build:wasm` refuses to start when `emcc` on PATH reports a different one, and the source
 * manifest records the release each module was built with so the pack step can refuse a mismatch.
 * CMake keeps the compiler a build tree was first configured with, whatever PATH says later, so
 * the guard also refuses a build tree whose recorded compiler is gone or reports another release
 * than the active `emcc`. The release decides rather than the path, because an `emcc` on PATH may
 * be a wrapper script that runs the emsdk compiler from a directory no symlink leads to.
 */

import { execFileSync } from 'node:child_process';
import { accessSync, constants, existsSync, readdirSync, readFileSync, realpathSync } from 'node:fs';
import { delimiter, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** The tracked pin, relative to nothing: always this package's own file. */
export const VERSION_FILE = fileURLToPath(new URL('../.emscripten-version', import.meta.url));

/** The pinned release, e.g. `7.0.1`. */
export function pinnedVersion(file = VERSION_FILE) {
  const version = readFileSync(file, 'utf8').trim();
  if (!/^\d+\.\d+\.\d+$/.test(version)) {
    throw new Error(`${file} must hold one Emscripten release such as 7.0.1, not '${version}'`);
  }
  return version;
}

/** The release token `emcc --version` prints on its first line (`7.0.1`, `6.0.10-git`), or null. */
export function parseEmccVersion(text) {
  const match = /^emcc \(.*?\)\s+(\S+)/m.exec(text);
  return match === null ? null : match[1];
}

/** The release @p compiler (by default the `emcc` on PATH in @p env) reports, or null when none runs. */
export function installedVersion(env = process.env, compiler = 'emcc') {
  let output;
  try {
    output = execFileSync(compiler, ['--version'], {
      encoding: 'utf8',
      env,
      stdio: ['ignore', 'pipe', 'ignore'],
    });
  } catch {
    return null;
  }
  return parseEmccVersion(output);
}

export function activationHint(version) {
  return `Activate it with \`emsdk install ${version} && emsdk activate ${version}\`, then \`source <emsdk>/emsdk_env.sh\` in this shell.`;
}

/** Why the `emcc` on PATH cannot build the modules, or null when it is the pinned release. */
export function toolchainProblem({ env = process.env, pinned = pinnedVersion() } = {}) {
  const installed = installedVersion(env);
  if (installed === pinned) return null;
  const found = installed === null ? 'no emcc runs from PATH' : `emcc on PATH is ${installed}`;
  return `The WASM modules are built with Emscripten ${pinned} (bindings/wasm/.emscripten-version), but ${found}.\n${activationHint(pinned)}`;
}

/** The real directory of the `emcc` PATH in @p env resolves to, or null when there is none. */
export function emccDirectory(env = process.env) {
  for (const dir of (env.PATH ?? '').split(delimiter)) {
    if (dir === '') continue;
    const candidate = join(dir, 'emcc');
    try {
      accessSync(candidate, constants.X_OK);
    } catch {
      continue;
    }
    return dirname(realpathSync(candidate));
  }
  return null;
}

/** The C++ compiler @p buildDir was configured with, read from CMake's own record, or null. */
export function configuredCompiler(buildDir) {
  const filesDir = join(buildDir, 'CMakeFiles');
  if (!existsSync(filesDir)) return null;
  for (const entry of readdirSync(filesDir).sort()) {
    const record = join(filesDir, entry, 'CMakeCXXCompiler.cmake');
    if (!existsSync(record)) continue;
    const match = /^set\(CMAKE_CXX_COMPILER "([^"]+)"\)/m.exec(readFileSync(record, 'utf8'));
    if (match !== null) return match[1];
  }
  return null;
}

/** Why @p buildDir would compile with another toolchain than the active `emcc`, or null. */
export function buildTreeProblem(buildDir, { env = process.env } = {}) {
  const compiler = configuredCompiler(buildDir);
  if (compiler === null) return null;
  const active = emccDirectory(env);
  if (active === null) return null; // toolchainProblem already names a missing emcc.
  const dir = dirname(compiler);
  const configured = existsSync(compiler)
    ? dirname(realpathSync(compiler))
    : existsSync(dir)
      ? realpathSync(dir)
      : dir;
  if (configured === active) return null;
  if (existsSync(compiler)) {
    const release = installedVersion(env, compiler);
    if (release !== null && release === installedVersion(env)) return null;
  }
  return `${buildDir} is configured with ${compiler}, but the active Emscripten is ${join(active, 'emcc')}.\nCMake keeps the compiler a tree was first configured with, so move ${buildDir} aside and build again.`;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const buildDir = process.argv[2];
  const problem =
    toolchainProblem() ?? (buildDir === undefined ? null : buildTreeProblem(resolve(buildDir)));
  if (problem !== null) {
    console.error(problem);
    process.exit(1);
  }
}
