/**
 * @file vitest global setup: attest `dist/` before any test imports it.
 *
 * Two checks with different jobs. `setup` compares the working tree against
 * the manifest each module was built from, so a run against a `dist/` that no
 * longer matches its sources fails instead of passing. `teardown` re-reads the
 * artifacts, so a build landing mid-run — every surface shares one `dist/` —
 * fails rather than producing results split across two binaries.
 *
 * The provenance line pairs the artifact's build time with the time this run
 * read it, which is the comparison that catches a binary predating the change
 * under test.
 *
 * `SONARE_ALLOW_STALE_DIST=1` lets a run use emscripten modules built from
 * older native sources (a TypeScript change tested while native sources are
 * mid-edit); the provenance line names each module accepted stale. The bundle,
 * a missing manifest and the mid-run check are never relaxed, and the
 * `check:dist-freshness` command ignores the variable.
 */

import { ALLOW_STALE_ENV, readArtifacts, verifyAll } from './dist-source-manifest.mjs';

let readDigests = {};

// A finished stack: vite's source-map hook cannot map this module's frames and crashes the printer.
function refusal(message) {
  const error = new Error(message);
  error.stack = `Error: ${message}`;
  return error;
}

export async function setup() {
  const allowStale = process.env[ALLOW_STALE_ENV] === '1';
  const { problems, provenance, artifacts } = await verifyAll({ allowStale });
  if (problems.length > 0) {
    throw refusal(
      `\n${problems.join('\n\n')}\n\nSet ${ALLOW_STALE_ENV}=1 to accept stale emscripten modules; the bundle is never relaxed.\n`,
    );
  }
  readDigests = artifacts;
  const readAt = new Date().toISOString();
  for (const line of provenance) {
    console.log(`[wasm dist] ${line}, read ${readAt}`);
  }
}

export async function teardown() {
  const names = Object.keys(readDigests);
  if (names.length === 0) return;
  const current = await readArtifacts(names);
  const moved = names.filter((name) => current[name] !== readDigests[name]);
  if (moved.length > 0) {
    throw refusal(
      `\ndist/${moved.join(', dist/')} changed while the suite was running, so these results do not attribute to one build.\nRe-run the suite.\n`,
    );
  }
}
