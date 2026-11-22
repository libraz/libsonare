/**
 * @file The Emscripten release the WASM modules are built with, and the guard holding a build to it.
 *
 * `bindings/wasm/.emscripten-version` is the one tracked statement of that release; the CI setup
 * steps read it. Any other release builds a module that passes every test while contradicting the
 * pin, so `build:wasm` refuses to start when `emcc` on PATH reports a different one, and the source
 * manifest records the release each module was built with so the pack step can refuse a mismatch.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
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

/** The release of the `emcc` on PATH in @p env, or null when none runs. */
export function installedVersion(env = process.env) {
  let output;
  try {
    output = execFileSync('emcc', ['--version'], {
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

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const problem = toolchainProblem();
  if (problem !== null) {
    console.error(problem);
    process.exit(1);
  }
}
