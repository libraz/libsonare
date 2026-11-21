/**
 * The parameter descriptors a host draws controls from, and the enum tokens the
 * mastering overrides accept: log axes with a positive floor, power-of-two
 * transform sizes as choices, repair stages served by the insert query, an EQ
 * ceiling that follows the rate it is built for, and a streaming chain whose
 * denoise stage prepares at its default.
 */

import { describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  masterAudio,
  masteringApplyProcessor,
  masteringAssistantSuggestChainStereo,
  masteringAssistantSuggestStereo,
  masteringAudioProfileStereo,
  masteringInsertParamInfo,
  masteringInsertTiming,
  masteringProcessorCatalog,
  masteringStreamingPreviewStereo,
  StreamingMasteringChain,
  streamingLoudnessGain,
} from '../src/index.js';

const SR = 48000;

function noise(n: number): Float32Array {
  const out = new Float32Array(n);
  let state = 12345;
  for (let i = 0; i < n; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = 0.1 * (state / 2 ** 31 - 1);
  }
  return out;
}

describe('log axes', () => {
  it('every logarithmic parameter publishes a positive floor and a finite top', () => {
    const defects: string[] = [];
    let logarithmic = 0;
    for (const entry of capabilityCatalog().processors) {
      for (const param of entry.params) {
        if (param.scale !== 'log') {
          continue;
        }
        logarithmic += 1;
        const low = param.uiMin ?? param.min;
        const high = param.uiMax ?? param.max;
        const label = `${entry.id}.${param.name}`;
        if (low === undefined || !(low > 0)) {
          defects.push(`${label} has no positive lower end`);
        } else if (high === undefined) {
          defects.push(`${label} has no finite upper end`);
        } else if (!(low < high)) {
          defects.push(`${label} has an empty display range`);
        }
      }
    }
    expect(logarithmic).toBeGreaterThan(800);
    expect(defects).toEqual([]);
  });
});

describe('repair stage descriptors', () => {
  it('publishes the transform size as powers of two and the hop with no stale maximum', () => {
    for (const id of ['repair.denoiseClassical', 'repair.dereverbClassical']) {
      const info = masteringInsertParamInfo({ insert: id as never });
      const nFft = info.find((param) => param.name === 'nFft');
      const hop = info.find((param) => param.name === 'hopLength');
      expect(nFft).not.toHaveProperty('min');
      expect(nFft).not.toHaveProperty('max');
      const sizes = nFft?.choices?.map((choice) => choice.value) ?? [];
      expect(sizes.length).toBeGreaterThan(1);
      expect(sizes.every((size) => Number.isInteger(Math.log2(size)))).toBe(true);
      expect(nFft?.choices?.map((choice) => choice.name)).toEqual(sizes.map(String));
      expect(sizes.at(-1)).toBe(524288);
      expect(hop).not.toHaveProperty('choices');
      expect(hop?.min).toBe(1);
      expect(hop).not.toHaveProperty('max');
      expect(hop?.dependsOn).toEqual([{ key: 'nFft', relation: 'le', factor: 0.5 }]);
    }
  });

  it('serves the catalog rows of an offline repair stage', () => {
    const catalog = new Map(masteringProcessorCatalog().map((entry) => [entry.id, entry]));
    for (const id of ['repair.declick', 'repair.declip', 'repair.trimSilence']) {
      const info = masteringInsertParamInfo({ insert: id as never });
      expect(info.length).toBeGreaterThan(0);
      expect(info).toEqual(catalog.get(id)?.params);
      expect(info.every((param) => param.id === undefined)).toBe(true);
    }
    expect(masteringInsertParamInfo({ insert: 'repair.noSuchStage' as never })).toEqual([]);
  });
});

describe('coupled parameters', () => {
  it('names the room geometry a receiver spacing is accepted against', () => {
    for (const id of ['effects.reverb.room', 'effects.acoustic.roomMorph']) {
      const info = masteringInsertParamInfo({ insert: id as never });
      const spacing = info.find((param) => param.name === 'receiverSpacingM');
      expect(spacing?.max).toBeCloseTo(2.5, 3);
      expect(spacing?.dependsOn).toEqual([]);
      expect(spacing?.coupledWith).toEqual(
        expect.arrayContaining(['lengthM', 'widthM', 'listenerX', 'listenerY']),
      );
      expect(info.every((param) => Array.isArray(param.coupledWith))).toBe(true);
    }
  });
});

describe('equalizer ceiling', () => {
  it('follows the Nyquist frequency of the rate the insert is built for', () => {
    const ceiling = (rate?: number) =>
      masteringInsertParamInfo({ insert: 'eq.parametric', sampleRate: rate }).find(
        (param) => param.name === 'band0.frequencyHz',
      )?.max;
    expect(ceiling()).toBe(24000);
    expect(ceiling(96000)).toBe(48000);

    const x = noise(2048);
    const params = { 'band0.frequencyHz': 30000, 'band0.gainDb': 3 };
    const apply = (sampleRate: number) =>
      masteringApplyProcessor({ processor: 'eq.parametric', samples: x, sampleRate, params });
    expect(() => apply(96000)).not.toThrow();
    expect(() => apply(SR)).toThrow(
      new RangeError(
        'masteringApplyProcessor: params.band0.frequencyHz must be finite and in (0, 24000)',
      ),
    );
  });
});

describe('streaming denoise', () => {
  it('prepares at its default estimator', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 512,
      repair: { denoise: { enabled: true } },
    });
    expect(chain.stageNames()).toEqual(['repair.denoise']);
    expect(chain.processMono(noise(512)).length).toBe(512);
    chain.destroy();
  });

  it('takes an estimator by name and still refuses the whole-signal one', () => {
    for (const name of ['mcra', 'imcra', 'spp'] as const) {
      const chain = new StreamingMasteringChain({
        sampleRate: SR,
        numChannels: 1,
        maxBlockSize: 512,
        repair: { denoise: { enabled: true, noiseEstimator: name } },
      });
      chain.destroy();
    }
    expect(() => {
      const chain = new StreamingMasteringChain({
        sampleRate: SR,
        numChannels: 1,
        maxBlockSize: 512,
        repair: { denoise: { enabled: true, noiseEstimator: 'quantile' } },
      });
      chain.destroy();
    }).toThrow(/quantile/);
  });

  it('measures the loudness gain through the same estimator', () => {
    const x = noise(SR);
    const gain = streamingLoudnessGain({
      samples: x,
      sampleRate: SR,
      config: { loudness: { targetLufs: -20 }, repair: { denoise: { enabled: true } } },
    });
    expect(Number.isFinite(gain.loudnessStaticGainDb)).toBe(true);
  });
});

describe('enum tokens in mastering overrides', () => {
  const x = noise(SR);
  const denoise = (extra: object) => ({ repair: { denoise: { enabled: true, ...extra } } });

  it('takes a name, and a number is refused as a TypeError naming the key', () => {
    const byName = masterAudio({
      samples: x,
      sampleRate: SR,
      preset: 'pop',
      overrides: denoise({ noiseEstimator: 'mcra', mode: 'mmseStsa' }),
    });
    expect(byName.samples.length).toBe(x.length);
    expect(() =>
      masterAudio({
        samples: x,
        sampleRate: SR,
        preset: 'pop',
        overrides: denoise({ noiseEstimator: 1, mode: 1 }) as never,
      }),
    ).toThrow(
      new TypeError(
        'masterAudio: overrides.repair.denoise.mode must be one of logMmse, mmseStsa, spectralSubtraction',
      ),
    );
  });

  it('takes the dotted spelling as the same override as the nested one', () => {
    const nested = masterAudio({
      samples: x,
      sampleRate: SR,
      preset: 'pop',
      overrides: denoise({ noiseEstimator: 'mcra' }),
    });
    const dotted = masterAudio({
      samples: x,
      sampleRate: SR,
      preset: 'pop',
      overrides: {
        'repair.denoise.enabled': true,
        'repair.denoise.noiseEstimator': 'mcra',
      } as never,
    });
    expect(Array.from(dotted.samples)).toEqual(Array.from(nested.samples));
  });

  it('an unknown name is a RangeError on both chains', () => {
    expect(() =>
      masterAudio({
        samples: x,
        sampleRate: SR,
        preset: 'pop',
        overrides: denoise({ noiseEstimator: 'nope' }) as never,
      }),
    ).toThrow(
      new RangeError(
        'masterAudio: overrides.repair.denoise.noiseEstimator must be one of quantile, mcra, imcra, spp',
      ),
    );
    expect(() => {
      new StreamingMasteringChain({
        sampleRate: SR,
        numChannels: 1,
        ...denoise({ noiseEstimator: 'nope' }),
      } as never);
    }).toThrow(RangeError);
  });

  it('a string for a number-valued key is a TypeError', () => {
    expect(() =>
      masterAudio({
        samples: x,
        sampleRate: SR,
        preset: 'pop',
        overrides: { loudness: { targetLufs: 'x' } } as never,
      }),
    ).toThrow(
      new TypeError('masterAudio: overrides.loudness.targetLufs must be a number or a boolean'),
    );
  });
});

describe('request-only stereo helpers', () => {
  it('say so when a typed array is passed positionally', () => {
    const left = noise(2048);
    const right = noise(2048);
    const helpers = {
      masteringAudioProfileStereo,
      masteringAssistantSuggestStereo,
      masteringAssistantSuggestChainStereo,
      masteringStreamingPreviewStereo,
    };
    for (const [name, helper] of Object.entries(helpers)) {
      const call = helper as (...args: unknown[]) => unknown;
      expect(() => call(left, right, SR)).toThrow(TypeError);
      expect(name).toMatch(/^mastering/);
    }
  });
});

describe('enum names in per-processor params', () => {
  const x = noise(2048);
  const apply = (params: object) =>
    masteringApplyProcessor({
      processor: 'saturation.softClipper',
      samples: x,
      sampleRate: SR,
      params: params as never,
    });
  const timing = (params: object) =>
    masteringInsertTiming({
      insert: 'saturation.softClipper',
      params: params as never,
      sampleRate: SR,
    });

  it('resolve to the number the flat list carries', () => {
    const byName = apply({ aliasing: 'oversample4x' });
    expect(byName.latencySamples).toBeGreaterThan(0);
    expect(timing({ aliasing: 'oversample4x' }).latencySamples).toBe(byName.latencySamples);
  });

  it('refuse an unknown name', () => {
    expect(() => apply({ aliasing: 'nope' })).toThrow(RangeError);
    expect(() => timing({ aliasing: 'nope' })).toThrow(RangeError);
  });

  it('keep a string for a number-valued key a TypeError', () => {
    expect(() => apply({ ceiling: 'loud' })).toThrow(TypeError);
    expect(() => timing({ ceiling: 'loud' })).toThrow(TypeError);
  });
});
