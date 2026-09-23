/**
 * The declaration tree in `dist/` resolves without `src/` beside it.
 *
 * A host that vendors `dist/` alone has nothing at `../src/`, and under
 * `skipLibCheck` an unresolved specifier degrades every type behind it to
 * `any` without an error, so an escaping specifier is refused here instead.
 */

import { readdir, readFile } from 'node:fs/promises';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const DIST = fileURLToPath(new URL('../dist/', import.meta.url));
const SPECIFIER = /\b(?:from|import)\s*\(?\s*['"]([^'"]+)['"]/g;

async function declarations(directory: string): Promise<string[]> {
  const found: string[] = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      found.push(...(await declarations(path)));
    } else if (entry.name.endsWith('.d.ts')) {
      found.push(path);
    }
  }
  return found;
}

describe('dist declarations', () => {
  it('reference nothing outside dist/', async () => {
    const files = await declarations(DIST);
    expect(files.length).toBeGreaterThan(0);
    const escaping: string[] = [];
    for (const file of files) {
      const fromDist = relative(DIST, file).split(sep).join('/');
      const depth = fromDist.split('/').length - 1;
      for (const [, specifier] of (await readFile(file, 'utf8')).matchAll(SPECIFIER)) {
        const ups = specifier.match(/^(?:\.\.\/)+/)?.[0].length ?? 0;
        if (ups / 3 > depth) {
          escaping.push(`${fromDist}: ${specifier}`);
        }
      }
    }
    expect(escaping).toEqual([]);
  });
});
