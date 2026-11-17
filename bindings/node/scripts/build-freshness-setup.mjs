/**
 * @file vitest global setup: check the addon and `dist/` before any test loads them.
 *
 * `setup` compares each output with the newest source it is built from, so a run
 * against an older build fails naming it instead of passing. `teardown` re-reads
 * the outputs, so a rebuild landing mid-run fails rather than producing results
 * split across two builds. The provenance line pairs each output's build time
 * with the time this run read it.
 */

import { ALLOW_STALE_ENV, readDigests, verifyOutputs } from './build-freshness.mjs';

let read = {};

// A finished stack: vite's source-map hook cannot map this module's frames and crashes the printer.
function refusal(message) {
  const error = new Error(message);
  error.stack = `Error: ${message}`;
  return error;
}

export async function setup() {
  const allowStale = process.env[ALLOW_STALE_ENV] === '1';
  const { problems, provenance, artifacts } = await verifyOutputs({ allowStale });
  if (problems.length > 0) {
    throw refusal(
      `\n${problems.join('\n')}\n\nSet ${ALLOW_STALE_ENV}=1 to accept a stale native addon; dist/ is never relaxed.\n`,
    );
  }
  read = artifacts;
  const readAt = new Date().toISOString();
  for (const line of provenance) console.log(`[node build] ${line}, read ${readAt}`);
}

export async function teardown() {
  const paths = Object.keys(read);
  if (paths.length === 0) return;
  const current = await readDigests(paths);
  const moved = paths.filter((path) => current[path] !== read[path]);
  if (moved.length > 0) {
    throw refusal(
      `\n${moved.join(', ')} changed while the suite was running, so these results do not attribute to one build.\nRe-run the suite.\n`,
    );
  }
}
