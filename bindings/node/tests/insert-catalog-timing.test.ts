import { describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  masteringInsertParamInfo,
  masteringInsertTiming,
} from '../src/index.js';

describe('insert catalog carries construction-only params, and timing answers per-configuration', () => {
  it('reports softClipper aliasing as a construction-only enum with choices', () => {
    const info = masteringInsertParamInfo('saturation.softClipper');
    const aliasing = info.find((param) => param.name === 'aliasing');
    expect(aliasing).toBeDefined();
    expect(aliasing?.type).toBe('enum');
    expect(aliasing?.id).toBeNull();
    expect(aliasing?.rtSafe).toBe(false);
    expect(aliasing?.choices?.map((choice) => choice.name)).toContain('oversample4x');
    expect(aliasing?.choices?.find((choice) => choice.name === 'oversample4x')?.value).toBe(3);

    const ceiling = info.find((param) => param.name === 'ceiling');
    expect(ceiling).toBeDefined();
  });

  it('reports positive latency once softClipper is built with 4x oversampling', () => {
    const timing = masteringInsertTiming('saturation.softClipper', { aliasing: 3 }, 48000);
    expect(timing.latencySamples).toBeGreaterThan(0);
  });

  it('matches the capability catalog default-probe timing at default params', () => {
    const catalogEntry = capabilityCatalog().processors.find(
      (processor) => processor.id === 'saturation.softClipper',
    );
    expect(catalogEntry).toBeDefined();
    const timing = masteringInsertTiming('saturation.softClipper', {}, 48000);
    expect(timing.latencySamples).toBe(catalogEntry?.latencySamples);
    expect(timing.tailSamples).toBe(catalogEntry?.tailSamples);
  });

  it('rejects a key the processor does not read, naming the key', () => {
    expect(() =>
      masteringInsertTiming('saturation.softClipper', { notAKey: 1 }, 48000),
    ).toThrowError(/does not read parameter\(s\).*notAKey/);
  });

  it('rejects a non-finite number, naming the key', () => {
    expect(() =>
      masteringInsertTiming('saturation.softClipper', { driveDb: Number.NaN }, 48000),
    ).toThrowError(/driveDb/);
  });

  it('rejects a string value, naming the key', () => {
    expect(() =>
      masteringInsertTiming(
        'saturation.softClipper',
        { driveDb: 'loud' } as unknown as Record<string, number>,
        48000,
      ),
    ).toThrowError(/driveDb/);
  });

  it('rejects an unknown insert processor', () => {
    expect(() => masteringInsertTiming('nope.nope', {}, 48000)).toThrowError(
      /unknown insert processor/,
    );
  });
});
