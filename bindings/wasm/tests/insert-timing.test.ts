/**
 * WASM coverage for the instance-latency query (`masteringInsertTiming`) and
 * the construction-key expansion of `masteringInsertParamInfo` (choices,
 * nullable id). Mirrors the C ABI `sonare_mastering_insert_timing` /
 * `sonare_mastering_insert_param_info` contract.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  ErrorCode,
  init,
  Mixer,
  masteringInsertParamInfo,
  masteringInsertTiming,
} from '../dist/index.js';

describe('insert timing and construction-key catalog (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports softClipper construction keys, including the aliasing enum', () => {
    const info = masteringInsertParamInfo('saturation.softClipper');
    expect(info.length).toBeGreaterThan(0);

    const aliasing = info.find((param) => param.name === 'aliasing');
    expect(aliasing).toBeDefined();
    expect(aliasing?.type).toBe('enum');
    expect(aliasing?.id).toBeNull();
    expect(aliasing?.rtSafe).toBe(false);
    expect(aliasing?.choices).toEqual(
      expect.arrayContaining([expect.objectContaining({ name: 'oversample4x', value: 3 })]),
    );

    const ceiling = info.find((param) => param.name === 'ceiling');
    expect(ceiling).toBeDefined();
  });

  it('reports latency > 0 for the 4x-oversampled configuration', () => {
    const timing = masteringInsertTiming('saturation.softClipper', { aliasing: 3 }, 48000);
    expect(timing.latencySamples).toBeGreaterThan(0);
  });

  it('matches the capability catalog default at {} / 48 kHz', () => {
    const timing = masteringInsertTiming('saturation.softClipper', {}, 48000);
    const catalog = capabilityCatalog();
    const entry = catalog.processors.find((p) => p.id === 'saturation.softClipper');
    expect(entry).toBeDefined();
    expect(timing.latencySamples).toBe(entry?.latencySamples);
    expect(timing.tailSamples).toBe(entry?.tailSamples);
  });

  it('rejects an unknown insert name', () => {
    expect(() => masteringInsertTiming('not.a.real.processor', {}, 48000)).toThrow(
      /unknown insert processor/,
    );
  });

  it('rejects a key the insert does not read, naming it', () => {
    expect(() =>
      masteringInsertTiming('saturation.softClipper', { notARealKey: 1 }, 48000),
    ).toThrow(/does not read parameter\(s\).*notARealKey/);
  });

  it('rejects a non-finite or non-numeric param value, naming the key', () => {
    expect(() =>
      masteringInsertTiming('saturation.softClipper', { aliasing: Number.NaN }, 48000),
    ).toThrow(/aliasing/);
    expect(() =>
      masteringInsertTiming(
        'saturation.softClipper',
        { ceiling: 'loud' as unknown as number },
        48000,
      ),
    ).toThrow(/ceiling/);
  });
});

describe('the causal repair stages reach the generic insert path (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports each repair insert latency from its configuration', () => {
    expect(masteringInsertTiming('repair.decrackle', {}, 48000).latencySamples).toBe(1);
    expect(masteringInsertTiming('repair.dehum', {}, 48000).latencySamples).toBe(0);
    expect(
      masteringInsertTiming('repair.dehum', { adaptive: true, frameSize: 1024 }, 48000)
        .latencySamples,
    ).toBe(1024);
    expect(
      masteringInsertTiming('repair.dereverbClassical', { nFft: 512 }, 48000).latencySamples,
    ).toBe(511);
    // n_fft - 1 plus the hop the gain smoothing lags by.
    expect(masteringInsertTiming('repair.denoiseClassical', {}, 48000).latencySamples).toBe(1279);
  });

  it('publishes only the causal choices and refuses an offline-only setting', () => {
    const mode = masteringInsertParamInfo('repair.decrackle').find(
      (param) => param.name === 'mode',
    );
    expect(mode?.choices?.map((choice) => choice.name)).toEqual(['median']);
    expect(() =>
      masteringInsertTiming('repair.dereverbClassical', { wpeEnabled: true }, 48000),
    ).toThrow(expect.objectContaining({ code: ErrorCode.InvalidParameter }));
    // The quantile estimator (0) ranks a whole signal; the insert defaults to spp.
    expect(() =>
      masteringInsertTiming('repair.denoiseClassical', { noiseEstimator: 0 }, 48000),
    ).toThrow(expect.objectContaining({ code: ErrorCode.InvalidParameter }));
    const estimator = masteringInsertParamInfo('repair.denoiseClassical').find(
      (param) => param.name === 'noiseEstimator',
    );
    expect(estimator?.choices?.map((choice) => choice.name)).not.toContain('quantile');
  });
});

describe('insert timing takes the document an insert is built from (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const sceneTiming = (processor: string, params: Record<string, unknown>) => {
    const mixer = Mixer.fromSceneJson(
      JSON.stringify({
        version: 1,
        strips: [{ id: 'a', inserts: [{ slot: 'pre', processor, params }] }],
        buses: [{ id: 'master', role: 'master', inserts: [] }],
        connections: [{ source: 'a', destination: 'master' }],
      }),
      48000,
    );
    try {
      return { latencySamples: mixer.latencySamples(), tailSamples: mixer.tailSamples() };
    } finally {
      mixer.destroy();
    }
  };
  const cabIr = Buffer.from(
    new Float32Array([0.5, ...new Array<number>(1999).fill(0)]).buffer,
  ).toString('base64');

  it('matches the constructed insert for an array-typed key', () => {
    const params = { bandAbsorption: [0.1, 0.1, 0.1, 0.1, 0.1, 0.1] };
    const timing = masteringInsertTiming('effects.acoustic.roomMorph', params, 48000);
    expect(timing).toEqual(sceneTiming('effects.acoustic.roomMorph', params));
    expect(timing.tailSamples).not.toBe(
      masteringInsertTiming('effects.acoustic.roomMorph', {}, 48000).tailSamples,
    );
  });

  it('matches the constructed insert for a string-typed key', () => {
    const params = { cabIrF32Base64: cabIr, cabIrSampleRate: 48000 };
    const timing = masteringInsertTiming('saturation.ampSim', params, 48000);
    expect(timing).toEqual(sceneTiming('saturation.ampSim', params));
    expect(timing.tailSamples).not.toBe(
      masteringInsertTiming('saturation.ampSim', {}, 48000).tailSamples,
    );
    const named = { preset: 'britStack' };
    expect(masteringInsertTiming('saturation.ampSim', named, 48000)).toEqual(
      sceneTiming('saturation.ampSim', named),
    );
  });

  it('refuses a value that does not match the key type as a TypeError', () => {
    const timing = (name: string, params: Record<string, unknown>) =>
      masteringInsertTiming(name, params as never, 48000);
    expect(() => timing('effects.acoustic.roomMorph', { bandAbsorption: 0.1 })).toThrow(
      new TypeError('masteringInsertTiming: params.bandAbsorption must be an array of numbers'),
    );
    expect(() => timing('effects.acoustic.roomMorph', { bandAbsorption: ['a'] })).toThrow(
      TypeError,
    );
    expect(() => timing('saturation.ampSim', { preset: 3 })).toThrow(TypeError);
    expect(() => timing('saturation.softClipper', { driveDb: [1] })).toThrow(TypeError);
    expect(() => timing('saturation.softClipper', { driveDb: null })).toThrow(TypeError);
  });

  it('refuses a non-finite number as a RangeError and an unknown name by the core', () => {
    expect(() =>
      masteringInsertTiming('effects.acoustic.roomMorph', { bandAbsorption: [Number.NaN] }, 48000),
    ).toThrow(RangeError);
    expect(() =>
      masteringInsertTiming('saturation.softClipper', { aliasing: Number.NaN }, 48000),
    ).toThrow(RangeError);
    expect(() => masteringInsertTiming('saturation.ampSim', { preset: 'nope' }, 48000)).toThrow();
  });
});
