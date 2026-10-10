import { describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  ErrorCode,
  Mixer,
  masteringAmpPresetCatalog,
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

  it('flags a bound the processor itself rejects as exclusive', () => {
    const hpf = masteringInsertParamInfo('dynamics.compressor').find(
      (param) => param.name === 'sidechainHpfHz',
    );
    expect(hpf?.min).toBe(0);
    expect(typeof hpf?.minExclusive).toBe('boolean');
    expect(hpf?.minExclusive).toBe(true);
    expect(hpf?.maxExclusive).toBe(false);
    expect(hpf?.maxRelativeTo).toBeNull();
    const frequency = masteringInsertParamInfo('eq.parametric').find(
      (param) => param.name === 'band0.frequencyHz',
    );
    expect(frequency?.maxRelativeTo).toBe('nyquist');
  });

  it('resolves a Nyquist-following ceiling for the host rate and leaves other keys alone', () => {
    const frequency = (rate?: number) =>
      masteringInsertParamInfo('eq.parametric', rate).find(
        (param) => param.name === 'band0.frequencyHz',
      );
    expect(frequency()?.max).toBe(24000);
    expect(frequency(44100)?.max).toBe(22050);
    expect(frequency(44100)?.maxExclusive).toBe(true);
    // An insert built for a known rate reaches that rate's Nyquist frequency.
    expect(frequency(96000)?.max).toBe(48000);
    const ratio = (rate?: number) =>
      masteringInsertParamInfo('dynamics.compressor', rate).find((param) => param.name === 'ratio');
    expect(ratio(44100)).toEqual(ratio());
    expect(() => masteringInsertParamInfo('eq.parametric', 0)).toThrow();
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

describe('insert timing takes the document an insert is built from', () => {
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

  it('refuses a non-finite array element as a RangeError and an unknown name by the core', () => {
    expect(() =>
      masteringInsertTiming('effects.acoustic.roomMorph', { bandAbsorption: [Number.NaN] }, 48000),
    ).toThrow(RangeError);
    expect(() => masteringInsertTiming('saturation.ampSim', { preset: 'nope' }, 48000)).toThrow();
  });
});
