import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { describe, expect, it } from 'vitest';

// Hosts that cannot fetch by URL (workers, AudioWorklets) hand the module the
// bytes through Module.wasmBinary. Emscripten decides per SDK release which
// incoming Module keys the glue reads, so this pins the ones the facades pass.
const distDir = path.resolve(import.meta.dirname, '../dist');
const unreachable = (file: string) => path.join(distDir, 'missing', file);

type Factory = (options?: {
  locateFile?: (file: string, prefix: string) => string;
  wasmBinary?: Uint8Array;
}) => Promise<unknown>;

describe.each([
  ['full', 'sonare'],
  ['analysis', 'sonare-analysis'],
])('%s module factory', (_label, name) => {
  const load = async () => {
    const factory = (await import(path.join(distDir, `${name}.js`))).default as Factory;
    const bytes = new Uint8Array(await readFile(path.join(distDir, `${name}.wasm`)));
    return { factory, bytes };
  };

  it('instantiates from wasmBinary without resolving the wasm location', async () => {
    const { factory, bytes } = await load();
    await expect(factory({ locateFile: unreachable, wasmBinary: bytes })).resolves.toBeTruthy();
  });

  it('fails on the same unreachable location when no bytes are given', async () => {
    const { factory } = await load();
    await expect(factory({ locateFile: unreachable })).rejects.toBeDefined();
  });
});
