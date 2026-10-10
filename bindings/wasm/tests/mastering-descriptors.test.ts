/**
 * The parameter descriptors a host draws controls from, and the enum names the
 * mastering overrides accept: log axes with a positive floor, power-of-two
 * transform sizes as choices, repair stages served by the insert query, an EQ
 * ceiling that follows the rate it is built for, and a streaming chain whose
 * denoise stage prepares at its default.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  init,
  masterAudio,
  masteringAssistantSuggestChainStereo,
  masteringAssistantSuggestStereo,
  masteringAudioProfileStereo,
  masteringInsertParamInfo,
  masteringInsertTiming,
  masteringProcess,
  masteringProcessorCatalog,
  masteringStreamingPreviewStereo,
  StreamingMasteringChain,
  streamingLoudnessGain,
} from '../dist/index.js';

const SR = 48000;

beforeAll(async () => {
  await init();
});

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
        if (low === null || !(low > 0)) {
          defects.push(`${label} has no positive lower end`);
        } else if (high === null) {
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
      const info = masteringInsertParamInfo(id);
      const nFft = info.find((param) => param.name === 'nFft');
      const hop = info.find((param) => param.name === 'hopLength');
      expect(nFft?.min).toBeNull();
      expect(nFft?.max).toBeNull();
      const sizes = nFft?.choices?.map((choice) => choice.value) ?? [];
      expect(sizes.length).toBeGreaterThan(1);
      expect(sizes.every((size) => Number.isInteger(Math.log2(size)))).toBe(true);
      expect(nFft?.choices?.map((choice) => choice.name)).toEqual(sizes.map(String));
      expect(sizes.at(-1)).toBe(524288);
      expect(hop?.choices).toBeNull();
      expect(hop?.min).toBe(1);
      expect(hop?.max).toBeNull();
      expect(hop?.dependsOn).toEqual([{ key: 'nFft', relation: 'le', factor: 0.5 }]);
    }
  });

  it('serves the catalog rows of an offline repair stage', () => {
    const catalog = new Map(masteringProcessorCatalog().map((entry) => [entry.id, entry]));
    for (const id of ['repair.declick', 'repair.declip', 'repair.trimSilence']) {
      const info = masteringInsertParamInfo(id);
      expect(info.length).toBeGreaterThan(0);
      expect(info).toEqual(catalog.get(id)?.params);
      expect(info.every((param) => param.id === null)).toBe(true);
    }
    expect(masteringInsertParamInfo('repair.noSuchStage')).toEqual([]);
  });
});

describe('equalizer ceiling', () => {
  it('follows the Nyquist frequency of the rate the insert is built for', () => {
    const ceiling = (rate?: number) =>
      masteringInsertParamInfo('eq.parametric', rate).find(
        (param) => param.name === 'band0.frequencyHz',
      )?.max;
    expect(ceiling()).toBe(24000);
    expect(ceiling(96000)).toBe(48000);

    const x = noise(2048);
    const params = { 'band0.frequencyHz': 30000, 'band0.gainDb': 3 };
    expect(() => masteringProcess('eq.parametric', x, 96000, params)).not.toThrow();
    expect(() => masteringProcess('eq.parametric', x, SR, params)).toThrow(
      /below 24000 Hz \(Nyquist at 48000 Hz\)/,
    );
  });
});

describe('streaming denoise', () => {
  it('prepares at its default estimator', () => {
    const chain = new StreamingMasteringChain({ repair: { denoise: { enabled: true } } });
    chain.prepare(SR, 512, 1);
    expect(chain.stageNames()).toEqual(['repair.denoise']);
    expect(chain.processMono(noise(512)).length).toBe(512);
    chain.delete();
  });

  it('takes an estimator by name and still refuses the whole-signal one', () => {
    for (const name of ['mcra', 'imcra', 'spp'] as const) {
      const chain = new StreamingMasteringChain({
        repair: { denoise: { enabled: true, noiseEstimator: name } },
      });
      chain.prepare(SR, 512, 1);
      chain.delete();
    }
    expect(() => {
      const chain = new StreamingMasteringChain({
        repair: { denoise: { enabled: true, noiseEstimator: 'quantile' } },
      });
      chain.prepare(SR, 512, 1);
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

describe('enum names in mastering overrides', () => {
  const x = noise(SR);

  it('resolves a name to the number the flat list carries', () => {
    const byName = masterAudio({
      samples: x,
      sampleRate: SR,
      preset: 'pop',
      overrides: {
        repair: { denoise: { enabled: true, noiseEstimator: 'mcra', mode: 'mmseStsa' } },
      },
    });
    const byNumber = masterAudio({
      samples: x,
      sampleRate: SR,
      preset: 'pop',
      overrides: { repair: { denoise: { enabled: true, noiseEstimator: 1, mode: 1 } } },
    });
    expect(Array.from(byName.samples)).toEqual(Array.from(byNumber.samples));
    const flat = masterAudio({
      samples: x,
      sampleRate: SR,
      preset: 'pop',
      overrides: {
        'repair.denoise.enabled': true,
        'repair.denoise.noiseEstimator': 'mcra',
        'repair.denoise.mode': 'mmseStsa',
      },
    });
    expect(Array.from(flat.samples)).toEqual(Array.from(byNumber.samples));
  });

  it('refuses an unknown name with the key and the valid names', () => {
    expect(() =>
      masterAudio({
        samples: x,
        sampleRate: SR,
        preset: 'pop',
        overrides: { repair: { denoise: { enabled: true, noiseEstimator: 'nope' as never } } },
      }),
    ).toThrow(/repair\.denoise\.noiseEstimator: unknown name 'nope' \(valid names: quantile, mcra/);
    expect(() => {
      new StreamingMasteringChain({
        repair: { denoise: { enabled: true, noiseEstimator: 'nope' as never } },
      });
    }).toThrow(/repair\.denoise\.noiseEstimator: unknown name 'nope'/);
  });

  it('keeps a string for a number-valued key a TypeError', () => {
    expect(() =>
      masterAudio({
        samples: x,
        sampleRate: SR,
        preset: 'pop',
        overrides: { loudness: { targetLufs: 'x' } } as never,
      }),
    ).toThrow(TypeError);
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
      expect(() => call(left, right, SR)).toThrow(
        `${name} takes a request object { left, right, sampleRate }`,
      );
    }
  });
});

describe('enum names in per-processor params', () => {
  const x = noise(2048);

  it('resolve to the number the flat list carries', () => {
    const byName = masteringProcess('saturation.softClipper', x, SR, { aliasing: 'oversample4x' });
    const byNumber = masteringProcess('saturation.softClipper', x, SR, { aliasing: 3 });
    expect(Array.from(byName.samples)).toEqual(Array.from(byNumber.samples));
    expect(byName.latencySamples).toBe(byNumber.latencySamples);
    expect(byName.latencySamples).toBeGreaterThan(0);
    const timing = masteringInsertTiming(
      'saturation.softClipper',
      { aliasing: 'oversample4x' },
      SR,
    );
    expect(timing).toEqual(masteringInsertTiming('saturation.softClipper', { aliasing: 3 }, SR));
  });

  it('refuse an unknown name with the key and the valid names', () => {
    expect(() => masteringProcess('saturation.softClipper', x, SR, { aliasing: 'nope' })).toThrow(
      /aliasing: unknown name 'nope' \(valid names: none, adaa1, adaa2, oversample4x\)/,
    );
    expect(() => masteringInsertTiming('saturation.softClipper', { aliasing: 'nope' }, SR)).toThrow(
      /unknown name 'nope'/,
    );
  });

  it('keep a string for a number-valued key a TypeError', () => {
    expect(() => masteringProcess('saturation.softClipper', x, SR, { ceiling: 'loud' })).toThrow(
      TypeError,
    );
    expect(() => masteringInsertTiming('saturation.softClipper', { ceiling: 'loud' }, SR)).toThrow(
      TypeError,
    );
  });
});
