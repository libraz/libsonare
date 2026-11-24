/**
 * A chain config input takes the nested form, the flat dotted form a saved chain
 * document's `params` carries, or both for different paths; they are one
 * parameter set, so every spelling renders the same chain.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  type MasteringChainConfig,
  type MasteringChainConfigInput,
  type MasteringChainResult,
  masterAudio,
  masteringPresetParams,
  StreamingMasteringChain,
} from '../src/index.js';
import { getSonareModule } from '../src/module_state.js';
import { chainUnknownKeyText } from './_helpers.js';

const sampleRate = 22_050;
const samples = Float32Array.from(
  { length: 8192 },
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

const nested: MasteringChainConfig = {
  loudness: { targetLufs: -16, ceilingDbtp: -1.5 },
  dynamics: {
    multibandComp: { enabled: true, crossover: { cutoffsHz: [200, 3000] }, bands: [{ ratio: 3 }] },
  },
};
const flat: MasteringChainConfigInput = {
  'loudness.targetLufs': -16,
  'loudness.ceilingDbtp': -1.5,
  'dynamics.multibandComp.enabled': true,
  'dynamics.multibandComp.crossover.cutoffsHz': [200, 3000],
  'dynamics.multibandComp.bands': [{ ratio: 3 }],
};

/** Every field of two results agrees, each within a tolerance taken from its own scale. */
function expectSameRender(a: MasteringChainResult, b: MasteringChainResult): void {
  expect(Object.keys(b).sort()).toEqual(Object.keys(a).sort());
  for (const [key, value] of Object.entries(a) as [keyof MasteringChainResult, unknown][]) {
    const other = b[key];
    if (value instanceof Float32Array) {
      const peak = value.reduce((m, x) => Math.max(m, Math.abs(x)), 0);
      const diff = value.reduce(
        (m, x, i) => Math.max(m, Math.abs(x - (other as Float32Array)[i])),
        0,
      );
      expect(diff, key).toBeLessThanOrEqual(1e-6 * Math.max(peak, 1e-3));
    } else if (typeof value === 'number') {
      expect(Math.abs(value - (other as number)), key).toBeLessThanOrEqual(
        1e-6 * Math.max(Math.abs(value), 1),
      );
    } else {
      expect(other, key).toEqual(value);
    }
  }
}

beforeAll(async () => init());

describe('masterAudio overrides in nested, flat and mixed spelling', () => {
  it('reports the same progress events in the request and positional forms', () => {
    const requestProgress: [number, string][] = [];
    const positionalProgress: [number, string][] = [];
    const options = { preset: 'pop', overrides: nested } as const;
    const request = masterAudio({
      ...options,
      samples,
      sampleRate,
      onProgress: (progress, stage) => {
        requestProgress.push([progress, stage]);
      },
    });
    const positional = masterAudio(samples, sampleRate, {
      ...options,
      onProgress: (progress, stage) => {
        positionalProgress.push([progress, stage]);
      },
    });
    expect(requestProgress.length).toBeGreaterThan(0);
    expect(requestProgress).toEqual(positionalProgress);
    expectSameRender(request, positional);
  });

  it('renders the flat spelling exactly as the nested one, and a changed value differently', () => {
    const fromNested = masterAudio({ samples, sampleRate, preset: 'pop', overrides: nested });
    const fromFlat = masterAudio({ samples, sampleRate, preset: 'pop', overrides: flat });
    expectSameRender(fromNested, fromFlat);
    const louder = masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      overrides: { ...flat, 'loudness.targetLufs': -8 },
    });
    expect(louder.outputLufs).toBeGreaterThan(fromFlat.outputLufs + 1);
  });

  it('accepts an object mixing both spellings for different paths', () => {
    const mixed: MasteringChainConfigInput = {
      loudness: { targetLufs: -16 },
      'loudness.ceilingDbtp': -1.5,
      dynamics: { multibandComp: { enabled: true, bands: [{ ratio: 3 }] } },
      'dynamics.multibandComp.crossover.cutoffsHz': [200, 3000],
    };
    expectSameRender(
      masterAudio({ samples, sampleRate, preset: 'pop', overrides: nested }),
      masterAudio({ samples, sampleRate, preset: 'pop', overrides: mixed }),
    );
  });

  it('refuses one path given in both spellings, and an unknown dotted path, by name', () => {
    expect(() =>
      masterAudio({
        samples,
        sampleRate,
        preset: 'pop',
        overrides: { 'loudness.targetLufs': -14, loudness: { targetLufs: -12 } },
      }),
    ).toThrow(new TypeError('masterAudio: overrides.loudness.targetLufs must not be given twice'));
    expect(() =>
      masterAudio({
        samples,
        sampleRate,
        preset: 'pop',
        overrides: { 'loudness.targetLufz': -14 } as MasteringChainConfigInput,
      }),
    ).toThrow(
      new TypeError(chainUnknownKeyText('masterAudio: unknown key overrides.loudness.targetLufz')),
    );
  });

  it.each([{}, 'x', null, []])(
    'refuses %j under an undeclared key as an unknown key, overrides and setter alike',
    (value) => {
      const overrides = { eq: { tilt: { undeclared: value } } } as MasteringChainConfigInput;
      expect(() => masterAudio({ samples, sampleRate, overrides })).toThrow(
        new TypeError(chainUnknownKeyText('masterAudio: unknown key overrides.eq.tilt.undeclared')),
      );
    },
  );

  it('accepts $- and x- annotation keys in overrides', () => {
    const overrides = { 'x-note': [], eq: { tilt: { $comment: {} } } } as MasteringChainConfigInput;
    expectSameRender(
      masterAudio({ samples, sampleRate, overrides }),
      masterAudio({ samples, sampleRate, overrides: {} }),
    );
  });

  it('refuses an undeclared streaming parameter as an unknown key', () => {
    const chain = new StreamingMasteringChain({
      sampleRate,
      numChannels: 1,
      eq: { tilt: { enabled: true } },
    });
    try {
      expect(() => chain.setParameter('eq.tilt.undeclared', 2)).toThrow(
        new TypeError(
          chainUnknownKeyText(
            'StreamingMasteringChain.setParameter: unknown key eq.tilt.undeclared',
          ),
        ),
      );
    } finally {
      chain.destroy();
    }
  });

  it("takes a saved chain document's params as written, multibandComp object included", () => {
    const document = JSON.parse(getSonareModule().masteringPresetParams('pop')) as {
      params: MasteringChainConfigInput;
    };
    expect(typeof document.params['dynamics.multibandComp']).toBe('object');
    expectSameRender(
      masterAudio({ samples, sampleRate, overrides: masteringPresetParams('pop') }),
      masterAudio({ samples, sampleRate, overrides: document.params }),
    );
  });

  it('builds a streaming chain from the flat spelling', () => {
    const chain = new StreamingMasteringChain({
      sampleRate,
      numChannels: 1,
      'dynamics.multibandComp.enabled': true,
      'dynamics.multibandComp.crossover.cutoffsHz': [200, 3000],
    });
    try {
      expect(chain.processMono(samples.subarray(0, 128))).toHaveLength(128);
    } finally {
      chain.destroy();
    }
  });

  it('types a dotted path by the value the nested config holds there', () => {
    const saved: MasteringChainConfigInput = {
      'dynamics.multibandComp': { enabled: true, bands: [{ ratio: 2, detector: 'rms' }] },
      'saturation.tape.enabled': false,
    };
    // @ts-expect-error a misspelt dotted path is not a chain parameter
    const misspelt: MasteringChainConfigInput = { 'loudness.targetLufz': -14 };
    // @ts-expect-error the value of a dotted path has the type the nested config gives it
    const mistyped: MasteringChainConfigInput = { 'loudness.targetLufs': 'loud' };
    expect([saved, misspelt, mistyped]).toHaveLength(3);
  });
});
