/**
 * @file Writes `dist/migration.json` from the WASM migration ledger.
 *
 * The ledger (`tools/api/migration/wasm.toml`) is the source of `UPGRADING.md`
 * for this surface; the package ships it as data so a consumer can check its
 * code against the renames, removals and reshapes without copying the file.
 * Each entry keeps the ledger's own fields except `test`, which names the
 * in-repo test that verifies the entry and means nothing to a caller.
 */

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { parse } from 'smol-toml';

/** Ledger fields describing how this repository verifies an entry, not what a caller changes. */
export const DROPPED_FIELDS = ['test'];

const PACKAGE_ROOT = new URL('../', import.meta.url);
const LEDGER = new URL('../../tools/api/migration/wasm.toml', PACKAGE_ROOT);
const TARGET = new URL('dist/migration.json', PACKAGE_ROOT);

/** The ledger's entries as the package publishes them. */
export async function publishedLedger(ledger = LEDGER) {
  const document = parse(await readFile(ledger, 'utf8'));
  return {
    entry: document.entry.map((entry) =>
      Object.fromEntries(Object.entries(entry).filter(([key]) => !DROPPED_FIELDS.includes(key))),
    ),
  };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  await mkdir(new URL('dist/', PACKAGE_ROOT), { recursive: true });
  await writeFile(TARGET, `${JSON.stringify(await publishedLedger())}\n`);
}
