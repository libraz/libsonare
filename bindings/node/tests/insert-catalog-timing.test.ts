import { describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  Mixer,
  masteringAmpPresetCatalog,
  masteringInsertParamInfo,
  masteringInsertTiming,
} from '../src/index.js';

describe('insert catalog carries construction-only params, and timing answers per-configuration', () => {
  it('reports softClipper aliasing as a construction-only enum with choices', () => {
    const info = masteringInsertParamInfo({ insert: 'saturation.softClipper' });
    const aliasing = info.find((param) => param.name === 'aliasing');
    expect(aliasing).toBeDefined();
    expect(aliasing?.type).toBe('enum');
    expect(aliasing).not.toHaveProperty('id');
    expect(aliasing?.rtSafe).toBe(false);
    expect(aliasing?.choices?.map((choice) => choice.name)).toContain('oversample4x');
    expect(aliasing?.choices?.find((choice) => choice.name === 'oversample4x')?.value).toBe(3);

    const ceiling = info.find((param) => param.name === 'ceiling');
    expect(ceiling).toBeDefined();
  });

  it('flags a bound the processor itself rejects as exclusive', () => {
    const q = masteringInsertParamInfo({ insert: 'eq.parametric' }).find(
      (param) => param.name === 'band0.q',
    );
    expect(q?.min).toBe(0);
    expect(typeof q?.minExclusive).toBe('boolean');
    expect(q?.minExclusive).toBe(true);
    expect(q?.maxExclusive).toBe(false);
    expect(q).not.toHaveProperty('maxRelativeTo');
    // The control: a high-pass corner of 0 is the filter switched off, so its 0 is inclusive.
    const hpf = masteringInsertParamInfo({ insert: 'dynamics.compressor' }).find(
      (param) => param.name === 'sidechainHighPassHz',
    );
    expect(hpf?.min).toBe(0);
    expect(hpf?.minExclusive).toBe(false);
    const frequency = masteringInsertParamInfo({ insert: 'eq.parametric' }).find(
      (param) => param.name === 'band0.frequencyHz',
    );
    expect(frequency?.maxRelativeTo).toBe('nyquist');
  });

  it('resolves a Nyquist-following ceiling for the host rate and leaves other keys alone', () => {
    const frequency = (rate?: number) =>
      masteringInsertParamInfo({ insert: 'eq.parametric', sampleRate: rate }).find(
        (param) => param.name === 'band0.frequencyHz',
      );
    expect(frequency()?.max).toBe(24000);
    expect(frequency(44100)?.max).toBe(22050);
    expect(frequency(44100)?.maxExclusive).toBe(true);
    // An insert built for a known rate reaches that rate's Nyquist frequency.
    expect(frequency(96000)?.max).toBe(48000);
    const ratio = (rate?: number) =>
      masteringInsertParamInfo({ insert: 'dynamics.compressor', sampleRate: rate }).find(
        (param) => param.name === 'ratio',
      );
    expect(ratio(44100)).toEqual(ratio());
    expect(() => masteringInsertParamInfo({ insert: 'eq.parametric', sampleRate: 0 })).toThrow();
  });

  it('reports positive latency once softClipper is built with 4x oversampling', () => {
    const timing = masteringInsertTiming({
      insert: 'saturation.softClipper',
      params: { aliasing: 3 } as never,
      sampleRate: 48000,
    });
    expect(timing.latencySamples).toBeGreaterThan(0);
  });

  it('matches the capability catalog default-probe timing at default params', () => {
    const catalogEntry = capabilityCatalog().processors.find(
      (processor) => processor.id === 'saturation.softClipper',
    );
    expect(catalogEntry).toBeDefined();
    const timing = masteringInsertTiming({
      insert: 'saturation.softClipper',
      params: {},
      sampleRate: 48000,
    });
    expect(timing.latencySamples).toBe(catalogEntry?.latencySamples);
    expect(timing.tailSamples).toBe(catalogEntry?.tailSamples);
  });

  it('rejects a key the processor does not read, naming the key', () => {
    expect(() =>
      masteringInsertTiming({
        insert: 'saturation.softClipper',
        params: { notAKey: 1 } as never,
        sampleRate: 48000,
      }),
    ).toThrowError(
      new TypeError(
        'masteringInsertTiming: unknown key notAKey (expected aliasing, ceiling, driveDb, mix)',
      ),
    );
  });

  it('rejects a non-finite number, naming the key', () => {
    expect(() =>
      masteringInsertTiming({
        insert: 'saturation.softClipper',
        params: { driveDb: Number.NaN },
        sampleRate: 48000,
      }),
    ).toThrowError(/driveDb/);
  });

  it('rejects a string value, naming the key', () => {
    expect(() =>
      masteringInsertTiming({
        insert: 'saturation.softClipper',
        params: { driveDb: 'loud' } as never,
        sampleRate: 48000,
      }),
    ).toThrowError(/driveDb/);
  });

  it('rejects an unknown insert processor', () => {
    expect(() =>
      masteringInsertTiming({
        insert: 'nope.nope' as never,
        params: {} as never,
        sampleRate: 48000,
      }),
    ).toThrowError(/unknown insert processor/);
  });
});

describe('mastering amp-sim preset catalog', () => {
  it('reports every rig in stable index order with resolved params', () => {
    const catalog = masteringAmpPresetCatalog();
    expect(catalog.length).toBeGreaterThan(0);
    expect(catalog.map((entry) => entry.index)).toEqual(catalog.map((_, index) => index));

    const clean = catalog.find((entry) => entry.name === 'cleanCombo');
    expect(clean).toBeDefined();
    expect(typeof clean?.params.drive).toBe('number');
    for (const entry of catalog) {
      expect(typeof entry.name).toBe('string');
      expect(entry.name.length).toBeGreaterThan(0);
    }
  });

  it('returns fresh parameter objects for read-only consumers', () => {
    const first = masteringAmpPresetCatalog();
    const originalDrive = first[0].params.drive;
    first[0].params.drive = -1;
    const second = masteringAmpPresetCatalog();
    expect(second[0].params.drive).toBe(originalDrive);
  });
});

describe('the causal repair stages reach the generic insert path', () => {
  it('reports each repair insert latency from its configuration', () => {
    expect(
      masteringInsertTiming({ insert: 'repair.decrackle', params: {}, sampleRate: 48000 })
        .latencySamples,
    ).toBe(1);
    expect(
      masteringInsertTiming({ insert: 'repair.dehum', params: {}, sampleRate: 48000 })
        .latencySamples,
    ).toBe(0);
    expect(
      masteringInsertTiming({
        insert: 'repair.dehum',
        params: { adaptive: true, frameSize: 1024 },
        sampleRate: 48000,
      }).latencySamples,
    ).toBe(1024);
    expect(
      masteringInsertTiming({
        insert: 'repair.dereverbClassical',
        params: { nFft: 512 },
        sampleRate: 48000,
      }).latencySamples,
    ).toBe(511);
    // n_fft - 1 plus the hop the gain smoothing lags by.
    expect(
      masteringInsertTiming({ insert: 'repair.denoiseClassical', params: {}, sampleRate: 48000 })
        .latencySamples,
    ).toBe(1279);
  });

  it('publishes only the causal choices and refuses an offline-only setting', () => {
    const mode = masteringInsertParamInfo({ insert: 'repair.decrackle' }).find(
      (param) => param.name === 'mode',
    );
    expect(mode?.choices?.map((choice) => choice.name)).toEqual(['median']);
    expect(() =>
      masteringInsertTiming({
        insert: 'repair.dereverbClassical',
        params: { wpeEnabled: true },
        sampleRate: 48000,
      }),
    ).toThrow(
      expect.objectContaining({
        codeName: 'NotSupported',
        message: `masteringInsertTiming: wpeEnabled is not supported for a streaming dereverb: the WPE predictor is fitted over the whole signal`,
      }),
    );
    // The quantile estimator (0) ranks a whole signal; the insert defaults to spp.
    expect(() =>
      masteringInsertTiming({
        insert: 'repair.denoiseClassical',
        params: { noiseEstimator: 0 } as never,
        sampleRate: 48000,
      }),
    ).toThrow(
      expect.objectContaining({
        codeName: 'NotSupported',
        message: `masteringInsertTiming: noiseEstimator quantile is not supported for a streaming denoise: it ranks every frame of the whole signal by energy; spp, mcra and imcra track recursively`,
      }),
    );
    const estimator = masteringInsertParamInfo({ insert: 'repair.denoiseClassical' }).find(
      (param) => param.name === 'noiseEstimator',
    );
    expect(estimator?.choices?.map((choice) => choice.name)).not.toContain('quantile');
  });
});

describe('insert timing takes the document an insert is built from', () => {
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
    const timing = masteringInsertTiming({
      insert: 'effects.acoustic.roomMorph',
      params,
      sampleRate: 48000,
    });
    expect(timing).toEqual(sceneTiming('effects.acoustic.roomMorph', params));
    expect(timing.tailSamples).not.toBe(
      masteringInsertTiming({ insert: 'effects.acoustic.roomMorph', params: {}, sampleRate: 48000 })
        .tailSamples,
    );
  });

  it('matches the constructed insert for a string-typed key', () => {
    const params = { cabIrF32Base64: cabIr, cabIrSampleRate: 48000 };
    const timing = masteringInsertTiming({
      insert: 'saturation.ampSim',
      params,
      sampleRate: 48000,
    });
    expect(timing).toEqual(sceneTiming('saturation.ampSim', params));
    expect(timing.tailSamples).not.toBe(
      masteringInsertTiming({ insert: 'saturation.ampSim', params: {}, sampleRate: 48000 })
        .tailSamples,
    );
    const named = { preset: 'crunchStack' };
    expect(
      masteringInsertTiming({ insert: 'saturation.ampSim', params: named, sampleRate: 48000 }),
    ).toEqual(sceneTiming('saturation.ampSim', named));
  });

  it('refuses a value that does not match the key type as a TypeError', () => {
    const timing = (name: string, params: Record<string, unknown>) =>
      masteringInsertTiming({ insert: name as never, params: params as never, sampleRate: 48000 });
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

  it('refuses a non-finite array element as a RangeError and an unknown name by the core', () => {
    expect(() =>
      masteringInsertTiming({
        insert: 'effects.acoustic.roomMorph',
        params: { bandAbsorption: [Number.NaN] },
        sampleRate: 48000,
      }),
    ).toThrow(RangeError);
    expect(() =>
      masteringInsertTiming({
        insert: 'saturation.ampSim',
        params: { preset: 'nope' },
        sampleRate: 48000,
      }),
    ).toThrow();
  });
});
