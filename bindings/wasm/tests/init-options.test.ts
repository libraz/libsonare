/**
 * init() option handling and the not-initialized error, for the root and the
 * analysis entry.
 */

import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { ErrorCode, SonareError } from '../dist/index.js';

const dist = join(dirname(fileURLToPath(import.meta.url)), '..', 'dist');

type Entry = typeof import('../dist/index.js') | typeof import('../dist/analysis.js');

const entries: [string, () => Promise<Entry>][] = [
  ['root', () => import('../dist/index.js')],
  ['analysis', () => import('../dist/analysis.js')],
];

describe.each(entries)('init() on the %s entry', (_name, load) => {
  let fresh: Entry;

  beforeEach(async () => {
    vi.resetModules();
    fresh = await load();
  });

  it('throws SonareError InvalidState before init', () => {
    let caught: unknown;
    try {
      fresh.version();
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(SonareError);
    expect((caught as SonareError).code).toBe(ErrorCode.InvalidState);
  });

  it('accepts a second call with no options or the same options', async () => {
    const wasmBinary = new Uint8Array(0);
    await fresh.init();
    await expect(fresh.init()).resolves.toBeUndefined();
    await expect(fresh.init({})).resolves.toBeUndefined();
    expect(fresh.isInitialized()).toBe(true);
    expect(wasmBinary.length).toBe(0);
  });

  it('rejects a second call whose options differ from the first', async () => {
    await fresh.init();
    const error = await fresh.init({ locateFile: (path) => path }).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(SonareError);
    expect((error as SonareError).code).toBe(ErrorCode.InvalidState);
    expect((error as SonareError).message).toMatch(/first call only/);
  });

  it('compares options shallowly with functions by reference', async () => {
    const locateFile = (path: string) => join(dist, path);
    await fresh.init({ locateFile });
    await expect(fresh.init({ locateFile })).resolves.toBeUndefined();
    await expect(fresh.init({ locateFile: (path) => path })).rejects.toBeInstanceOf(SonareError);
  });

  it('rejects a concurrent call with differing options', async () => {
    const first = fresh.init();
    const second = fresh.init({ locateFile: (path) => path });
    await expect(second).rejects.toMatchObject({ code: ErrorCode.InvalidState });
    await expect(first).resolves.toBeUndefined();
  });
});
