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
 */

import { readArtifacts, verifyAll } from './dist-source-manifest.mjs';

let readDigests = {};

export async function setup() {
  const { problems, provenance, artifacts } = await verifyAll();
  if (problems.length > 0) {
    throw new Error(`\n${problems.join('\n\n')}\n`);
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
    throw new Error(
      `\ndist/${moved.join(', dist/')} changed while the suite was running, so these results do not attribute to one build.\nRe-run the suite.\n`,
    );
  }
}
