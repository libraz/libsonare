/**
 * The three states of a result level: a null the core writes at a path
 * `schemas/result-null-means.json` names is that path's infinite end, a value
 * not measured is an absent key, and a null anywhere else is a defect the
 * reader refuses rather than drops.
 */

import { describe, expect, it } from 'vitest';
import {
  analyze,
  ErrorCode,
  isSonareError,
  masteringAnalyzePair,
  masteringAnalyzeStereo,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringRepairAnalyze,
  masteringStreamingPreview,
  mixingAssistantSuggest,
  type SonareError,
} from '../dist/index.js';
import { restoreNullMeans } from '../dist/internal/record_json.js';

const SR = 22050;
const silence = (): Float32Array => new Float32Array(SR);
function tone(hz: number, amp = 0.5): Float32Array {
  return Float32Array.from({ length: SR }, (_, i) => amp * Math.sin((2 * Math.PI * hz * i) / SR));
}

/** Paths of every null left in @p value, so a record that leaked one names where. */
function nullPaths(value: unknown, path = ''): string[] {
  if (value === null) {
    return [path];
  }
  if (Array.isArray(value)) {
    return value.flatMap((e, i) => nullPaths(e, `${path}[${i}]`));
  }
  if (value instanceof Float32Array || typeof value !== 'object') {
    return [];
  }
  return Object.entries(value as Record<string, unknown>).flatMap(([k, v]) =>
    nullPaths(v, path === '' ? k : `${path}.${k}`),
  );
}

describe('restoreNullMeans', () => {
  it('restores the end the table names for a path, array elements included', () => {
    const restored = restoreNullMeans(
      {
        tracks: [{ integratedLufs: null, truePeakDbtp: -3 }],
        mix: { monoRisks: [{ width: null }] },
      },
      'mix_assistant_result',
    ) as {
      tracks: Array<{ integratedLufs: number; truePeakDbtp: number }>;
      mix: { monoRisks: Array<{ width: number }> };
    };
    expect(restored.tracks[0].integratedLufs).toBe(Number.NEGATIVE_INFINITY);
    expect(restored.tracks[0].truePeakDbtp).toBe(-3);
    expect(restored.mix.monoRisks[0].width).toBe(Number.POSITIVE_INFINITY);
  });

  it('refuses a null at a path the table does not name rather than dropping it', () => {
    expect(() =>
      restoreNullMeans({ tracks: [{ crestFactorDb: null }] }, 'mix_assistant_result'),
    ).toThrow(/mix_assistant_result report holds a null at tracks\[\]\.crestFactorDb/);
    // The path is per family: a name another family lists is still refused here.
    expect(() => restoreNullMeans({ integratedLufs: null }, 'audio_profile')).toThrow(
      /audio_profile report holds a null at integratedLufs/,
    );
  });

  it('codes a null at an unnamed path as InvalidFormat, the code a refused document carries', () => {
    let caught: unknown;
    try {
      restoreNullMeans({ integratedLufs: null }, 'audio_profile');
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as SonareError).code).toBe(ErrorCode.InvalidFormat);
    expect((caught as SonareError).message).toBe(
      'audio_profile report holds a null at integratedLufs',
    );
  });

  it('refuses a null at a log-band correlation, which the writer leaves absent instead', () => {
    // A band silent in either channel has no correlation; the core omits the key rather than
    // writing null, so a null there names no end and is a defect.
    let caught: unknown;
    try {
      restoreNullMeans({ bands: [{ correlation: null }] }, 'stereo_mono_compat_log_bands');
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as SonareError).code).toBe(ErrorCode.InvalidFormat);
    expect((caught as SonareError).message).toBe(
      'stereo_mono_compat_log_bands report holds a null at bands[].correlation',
    );
  });
});

// Each family's entry point on silence and on a tone: every null the core
// writes is a named end, so a reader that met an unnamed one would throw, and
// none survives into the record.
describe('every result family reads without a null on silence and on a tone', () => {
  const inputs: Array<[string, () => Float32Array, () => Float32Array]> = [
    ['silence', silence, silence],
    ['a tone', () => tone(440), () => tone(660, 0.25)],
    ['a silent second input', () => tone(440), silence],
    ['a silent first input', silence, () => tone(440)],
  ];
  it.each(inputs)('%s', (label, a, b) => {
    // The curve and the delay refuse a silent input, so they read only where both inputs sound.
    const bothSound = label === 'a tone';
    const records: Array<[string, unknown]> = [
      ['analyze', analyze({ samples: a(), sampleRate: SR })],
      ['masteringAssistantSuggest', masteringAssistantSuggest({ samples: a(), sampleRate: SR })],
      [
        'masteringAudioProfile',
        masteringAudioProfile({ samples: a(), sampleRate: SR, detectDefects: true }),
      ],
      ['masteringStreamingPreview', masteringStreamingPreview({ samples: a(), sampleRate: SR })],
      [
        'match.referenceLoudness',
        masteringAnalyzePair({
          analysis: 'match.referenceLoudness',
          source: a(),
          reference: b(),
          sampleRate: SR,
        }),
      ],
      [
        'match.tonalBalance',
        masteringAnalyzePair({
          analysis: 'match.tonalBalance',
          source: a(),
          reference: b(),
          sampleRate: SR,
        }),
      ],
      ...((bothSound
        ? [
            [
              'match.matchEqCurve',
              masteringAnalyzePair({
                analysis: 'match.matchEqCurve',
                source: a(),
                reference: b(),
                sampleRate: SR,
              }),
            ],
            [
              'match.estimateReferenceDelaySamples',
              masteringAnalyzePair({
                analysis: 'match.estimateReferenceDelaySamples',
                source: a(),
                reference: b(),
                sampleRate: SR,
              }),
            ],
          ]
        : []) as Array<[string, unknown]>),
      [
        'stereo.monoCompatCheck',
        masteringAnalyzeStereo({
          analysis: 'stereo.monoCompatCheck',
          left: a(),
          right: b(),
          sampleRate: SR,
        }),
      ],
      [
        'stereo.monoCompatCheckLogBands',
        masteringAnalyzeStereo({
          analysis: 'stereo.monoCompatCheckLogBands',
          left: a(),
          right: b(),
          sampleRate: SR,
        }),
      ],
      ['masteringRepairAnalyze', masteringRepairAnalyze({ channels: [a(), b()], sampleRate: SR })],
      [
        'mixingAssistantSuggest',
        mixingAssistantSuggest({
          tracks: [
            { id: 'a', channels: [a()] },
            { id: 'b', channels: [b(), a()] },
          ],
          sampleRate: SR,
        }),
      ],
    ];
    for (const [name, record] of records) {
      expect(nullPaths(record), name).toEqual([]);
    }
  });
});
