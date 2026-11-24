/**
 * WASM coverage for the instance-latency query (`masteringInsertTiming`) and
 * the construction-key expansion of `masteringInsertParamInfo` (choices,
 * nullable id). Mirrors the C ABI `sonare_mastering_insert_timing` /
 * `sonare_mastering_insert_param_info` contract.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  init,
  Mixer,
  masteringInsertParamInfo,
  masteringInsertTiming,
} from '../dist/index.js';
import createSonareModule from '../dist/sonare.js';
import { nativeExceptionPtr } from '../src/module_state.js';

/** Positional shorthand over the request-only `masteringInsertTiming`. */
const timingOf = (insert: string, params: object, sampleRate: number) =>
  masteringInsertTiming({ insert: insert as never, params: params as never, sampleRate });

function nativeRefusal(
  module: Awaited<ReturnType<typeof createSonareModule>>,
  call: () => unknown,
) {
  try {
    call();
  } catch (error) {
    const ptr = nativeExceptionPtr(module, error);
    if (ptr === null) {
      throw error;
    }
    try {
      return module.sonareExceptionInfo(ptr);
    } finally {
      module.sonareReleaseException(ptr);
    }
  }
  throw new Error('Expected a native argument refusal');
}

describe('insert timing and construction-key catalog (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports softClipper construction keys, including the aliasing enum', () => {
    const info = masteringInsertParamInfo({ insert: 'saturation.softClipper' });
    expect(info.length).toBeGreaterThan(0);

    const aliasing = info.find((param) => param.name === 'aliasing');
    expect(aliasing).toBeDefined();
    expect(aliasing?.type).toBe('enum');
    expect(aliasing).not.toHaveProperty('id');
    expect(aliasing?.rtSafe).toBe(false);
    expect(aliasing?.choices).toEqual(
      expect.arrayContaining([expect.objectContaining({ name: 'oversample4x', value: 3 })]),
    );

    const ceiling = info.find((param) => param.name === 'ceiling');
    expect(ceiling).toBeDefined();
  });

  it('reports latency > 0 for the 4x-oversampled configuration', () => {
    const timing = timingOf('saturation.softClipper', { aliasing: 'oversample4x' }, 48000);
    expect(timing.latencySamples).toBeGreaterThan(0);
  });

  it('matches the capability catalog default at {} / 48 kHz', () => {
    const timing = timingOf('saturation.softClipper', {}, 48000);
    const catalog = capabilityCatalog();
    const entry = catalog.processors.find((p) => p.id === 'saturation.softClipper');
    expect(entry).toBeDefined();
    expect(timing.latencySamples).toBe(entry?.latencySamples);
    expect(timing.tailSamples).toBe(entry?.tailSamples);
  });

  it('refuses an out-of-range timing rate at both boundaries', async () => {
    const module = await createSonareModule();
    expect(module.masteringInsertTiming('utility.gain', '{}', 8000)).toEqual(
      timingOf('utility.gain', {}, 8000),
    );
    expect(
      nativeRefusal(module, () => module.masteringInsertTiming('utility.gain', '{}', 7999)),
    ).toMatchObject({
      kind: 'RangeError',
      message: 'sampleRate 7999 is out of range',
    });
    expect(() => timingOf('utility.gain', {}, 7999)).toThrow(
      new RangeError('masteringInsertTiming: sampleRate 7999 is out of range'),
    );
  });

  it('requires whole hertz for raw metadata and keeps an omitted rate distinct', async () => {
    const module = await createSonareModule();
    const omitted = module.masteringInsertParamInfo('utility.gain', undefined);
    expect(JSON.parse(omitted)).toEqual(
      expect.arrayContaining([expect.objectContaining({ name: 'levelDb' })]),
    );
    expect(module.masteringInsertParamInfo('utility.gain', null as never)).toBe(omitted);
    expect(
      nativeRefusal(module, () => module.masteringInsertParamInfo('utility.gain', 8000.5)),
    ).toMatchObject({
      kind: 'RangeError',
      message: 'sampleRate must be an integer in [-2147483648, 2147483647]',
    });
    expect(
      nativeRefusal(module, () => module.masteringInsertParamInfo('utility.gain', 0)),
    ).toMatchObject({
      kind: 'RangeError',
      message: 'sampleRate 0 is out of range',
    });
    expect(() =>
      masteringInsertParamInfo({ insert: 'not.a.real.processor' as never, sampleRate: 7999 }),
    ).toThrow(
      new RangeError('masteringInsertParamInfo: unknown insert processor: not.a.real.processor'),
    );
  });

  it('refuses raw insert metadata membership before considering the rate', async () => {
    const module = await createSonareModule();
    expect(
      nativeRefusal(module, () => module.masteringInsertParamInfo('not.a.real.processor', 7999)),
    ).toMatchObject({
      kind: 'RangeError',
      message: 'unknown insert processor: not.a.real.processor',
    });
  });

  it('rejects an unknown insert name', () => {
    expect(() => timingOf('not.a.real.processor', {}, 48000)).toThrow(RangeError);
  });

  it('rejects a key the insert does not read, naming it', () => {
    expect(() => timingOf('saturation.softClipper', { notARealKey: 1 }, 48000)).toThrow(
      new TypeError(
        'masteringInsertTiming: unknown key params.notARealKey (expected aliasing, ceiling, driveDb, mix)',
      ),
    );
  });

  it('rejects a non-finite or non-numeric param value, naming the key', () => {
    expect(() => timingOf('saturation.softClipper', { aliasing: Number.NaN }, 48000)).toThrow(
      /aliasing/,
    );
    expect(() =>
      timingOf('saturation.softClipper', { ceiling: 'loud' as unknown as number }, 48000),
    ).toThrow(/ceiling/);
  });
});

describe('the causal repair stages reach the generic insert path (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports each repair insert latency from its configuration', () => {
    expect(timingOf('repair.decrackle', {}, 48000).latencySamples).toBe(1);
    expect(timingOf('repair.dehum', {}, 48000).latencySamples).toBe(0);
    expect(
      timingOf('repair.dehum', { adaptive: true, frameSize: 1024 }, 48000).latencySamples,
    ).toBe(1024);
    expect(timingOf('repair.dereverbClassical', { nFft: 512 }, 48000).latencySamples).toBe(511);
    // n_fft - 1 plus the hop the gain smoothing lags by.
    expect(timingOf('repair.denoiseClassical', {}, 48000).latencySamples).toBe(1279);
  });

  it('publishes only the causal choices and refuses an offline-only setting', () => {
    const mode = masteringInsertParamInfo({ insert: 'repair.decrackle' }).find(
      (param) => param.name === 'mode',
    );
    expect(mode?.choices?.map((choice) => choice.name)).toEqual(['median']);
    expect(() => timingOf('repair.dereverbClassical', { wpeEnabled: true }, 48000)).toThrow(
      expect.objectContaining({
        codeName: 'NotSupported',
        message: `masteringInsertTiming: params.wpeEnabled is not supported for a streaming dereverb: the WPE predictor is fitted over the whole signal`,
      }),
    );
    // The quantile estimator (0) ranks a whole signal; the insert defaults to spp.
    expect(() =>
      timingOf('repair.denoiseClassical', { noiseEstimator: 'quantile' }, 48000),
    ).toThrow(
      expect.objectContaining({
        codeName: 'NotSupported',
        message: `masteringInsertTiming: params.noiseEstimator quantile is not supported for a streaming denoise: it ranks every frame of the whole signal by energy; spp, mcra and imcra track recursively`,
      }),
    );
    const estimator = masteringInsertParamInfo({ insert: 'repair.denoiseClassical' }).find(
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
    const mixer = Mixer.fromJson(
      JSON.stringify({
        version: 2,
        strips: [{ id: 'a', inserts: [{ slot: 'pre', processor, params }] }],
        buses: [{ id: 'master', role: 'master', inserts: [] }],
        connections: [{ source: 'a', destination: 'master' }],
      }),
      { sampleRate: 48000 },
    );
    try {
      return { latencySamples: mixer.latencySamples, tailSamples: mixer.tailSamples };
    } finally {
      mixer.destroy();
    }
  };
  const cabIr = Buffer.from(
    new Float32Array([0.5, ...new Array<number>(1999).fill(0)]).buffer,
  ).toString('base64');

  it('matches the constructed insert for an array-typed key', () => {
    const params = { bandAbsorption: [0.1, 0.1, 0.1, 0.1, 0.1, 0.1] };
    const timing = timingOf('effects.acoustic.roomMorph', params, 48000);
    expect(timing).toEqual(sceneTiming('effects.acoustic.roomMorph', params));
    expect(timing.tailSamples).not.toBe(
      timingOf('effects.acoustic.roomMorph', {}, 48000).tailSamples,
    );
  });

  it('matches the constructed insert for a string-typed key', () => {
    const params = { cabIrF32Base64: cabIr, cabIrSampleRate: 48000 };
    const timing = timingOf('saturation.ampSim', params, 48000);
    expect(timing).toEqual(sceneTiming('saturation.ampSim', params));
    expect(timing.tailSamples).not.toBe(timingOf('saturation.ampSim', {}, 48000).tailSamples);
    const named = { preset: 'crunchStack' };
    expect(timingOf('saturation.ampSim', named, 48000)).toEqual(
      sceneTiming('saturation.ampSim', named),
    );
  });

  it('refuses a value that does not match the key type as a TypeError', () => {
    const timing = (name: string, params: Record<string, unknown>) =>
      timingOf(name, params as never, 48000);
    expect(() => timing('effects.acoustic.roomMorph', { bandAbsorption: 0.1 })).toThrow(TypeError);
    expect(() => timing('effects.acoustic.roomMorph', { bandAbsorption: ['a'] })).toThrow(
      TypeError,
    );
    expect(() => timing('saturation.ampSim', { preset: 3 })).toThrow(TypeError);
    expect(() => timing('saturation.softClipper', { driveDb: [1] })).toThrow(TypeError);
    expect(() => timing('saturation.softClipper', { driveDb: null })).toThrow(TypeError);
  });

  it('refuses a non-finite number as a RangeError and an unknown name by the core', () => {
    expect(() =>
      timingOf('effects.acoustic.roomMorph', { bandAbsorption: [Number.NaN] }, 48000),
    ).toThrow(RangeError);
    expect(() => timingOf('saturation.softClipper', { aliasing: Number.NaN }, 48000)).toThrow(
      RangeError,
    );
    expect(() => timingOf('saturation.ampSim', { preset: 'nope' }, 48000)).toThrow();
  });
});
