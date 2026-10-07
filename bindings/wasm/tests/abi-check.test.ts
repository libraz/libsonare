import { describe, expect, it } from 'vitest';
import * as analysis from '../dist/analysis.js';
import * as full from '../dist/index.js';

type Entry = typeof full | typeof analysis;
type Factory = NonNullable<Parameters<Entry['init']>[0]>['moduleFactory'];

const wrongAbi = (async () => ({ abiVersion: () => 0x01010101 })) as unknown as Factory;
const noAbi = (async () => ({})) as unknown as Factory;

describe.each([
  ['index', full],
  ['analysis', analysis],
] as const)('%s entry ABI check', (_name, entry) => {
  it('rejects a module whose ABI version differs, then initialises with a matching one', async () => {
    const error = await entry.init({ moduleFactory: wrongAbi }).catch((e: unknown) => e);
    expect(entry.isSonareError(error)).toBe(true);
    const sonare = error as InstanceType<typeof entry.SonareError>;
    expect(sonare.code).toBe(entry.ErrorCode.AbiMismatch);
    expect(sonare.codeName).toBe('AbiMismatch');
    expect(sonare.message).toContain('0x01010101');
    expect(sonare.message).toContain('0x04020207');
    expect(sonare.message).toMatch(/wasmBinary.*locateFile.*moduleFactory/);
    expect(entry.isInitialized()).toBe(false);

    const missing = await entry.init({ moduleFactory: noAbi }).catch((e: unknown) => e);
    expect((missing as { code?: number }).code).toBe(entry.ErrorCode.AbiMismatch);
    expect(entry.isInitialized()).toBe(false);

    await expect(entry.init()).resolves.toBeUndefined();
    expect(entry.isInitialized()).toBe(true);
  });
});
