/**
 * Every request-object entry point refuses a key it does not declare, by name,
 * and accepts each key it does. The key lists come from the request types, so
 * this also pins that no entry point reads a field its request type omits.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import * as api from '../dist/index.js';
import { REQUEST_SHAPES } from '../src/request_keys';

type Entry = (request: unknown, ...rest: unknown[]) => unknown;

function entry(name: string): Entry {
  const fn = (api as unknown as Record<string, unknown>)[name];
  if (typeof fn !== 'function') {
    throw new Error(`${name} is not exported`);
  }
  return fn as Entry;
}

function message(call: () => unknown): string {
  try {
    call();
  } catch (error) {
    expect(error).toBeInstanceOf(Error);
    return (error as Error).message;
  }
  return '';
}

const names = Object.keys(REQUEST_SHAPES).sort();

describe('request objects refuse unknown keys', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(names)('%s refuses an undeclared key by name', (name) => {
    // A bare room estimate is also accepted, so only `estimate` marks the request form.
    const payload = name === 'roomGeometryFromEstimate' ? { estimate: {}, bogus: 1 } : { bogus: 1 };
    const call = () => entry(name)(payload);
    expect(call).toThrow(TypeError);
    expect(message(call)).toBe(`${name}: unknown request key 'bogus'`);
  });

  it.each(names)('%s does not refuse any declared key', (name) => {
    for (const key of REQUEST_SHAPES[name].keys) {
      expect(message(() => entry(name)({ [key]: undefined }))).not.toMatch(/unknown request key/);
    }
  });

  it('every requestObject call site has a declared shape', () => {
    const dir = join(import.meta.dirname, '..', 'src');
    const used = new Set<string>();
    for (const file of readdirSync(dir).filter((f) => f.endsWith('.ts'))) {
      const text = readFileSync(join(dir, file), 'utf8');
      for (const m of text.matchAll(/requestObject\(\s*'(\w+)'/g)) {
        used.add(m[1]);
      }
    }
    expect([...used].filter((name) => !(name in REQUEST_SHAPES))).toEqual([]);
  });
});

/** Positional overloads and the index of their options bag. */
const POSITIONAL_OPTIONS: ReadonlyArray<readonly [string, number]> = [
  ['analyze', 2],
  ['analyzeBpm', 2],
  ['analyzeDynamics', 2],
  ['analyzeMelody', 2],
  ['analyzeRhythm', 2],
  ['analyzeSections', 2],
  ['analyzeTimbre', 2],
  ['bassChroma', 4],
  ['chordFunctionalAnalysis', 4],
  ['chroma', 4],
  ['chromaCens', 5],
  ['chromaCqt', 5],
  ['cqt', 6],
  ['cqtToAudio', 8],
  ['detectAcoustic', 2],
  ['detectBeats', 2],
  ['detectBpm', 2],
  ['detectChords', 2],
  ['detectDownbeats', 2],
  ['detectKey', 2],
  ['detectKeyCandidates', 2],
  ['detectOnsets', 2],
  ['estimateRoom', 2],
  ['fourierTempogram', 6],
  ['griffinLim', 8],
  ['harmonic', 2],
  ['hybridCqt', 6],
  ['lufs', 2],
  ['lufsInterleaved', 3],
  ['lufsSeriesInterleaved', 3],
  ['mastering', 2],
  ['masteringDynamicsCompressor', 2],
  ['masteringDynamicsGate', 2],
  ['masteringDynamicsTransientShaper', 2],
  ['masteringRepairDeclick', 2],
  ['masteringRepairDeclip', 2],
  ['masteringRepairDecrackle', 2],
  ['masteringRepairDehum', 2],
  ['masteringRepairDenoiseClassical', 2],
  ['masteringRepairDereverbClassical', 2],
  ['masteringRepairDetectClicks', 2],
  ['masteringRepairDetectClipping', 2],
  ['masteringRepairDetectCrackle', 2],
  ['masteringRepairDetectHum', 2],
  ['masteringRepairDetectNoiseFloor', 2],
  ['masteringRepairDetectReverb', 2],
  ['masteringRepairDetectTrimRange', 2],
  ['masteringRepairTrimSilence', 2],
  ['melSpectrogram', 8],
  ['meteringCrestFactorDb', 2],
  ['meteringDcOffset', 2],
  ['meteringDetectClipping', 2],
  ['meteringDynamicRange', 2],
  ['meteringPeakDb', 2],
  ['meteringPhaseScope', 3],
  ['meteringPhaseScopeDecimated', 4],
  ['meteringRmsDb', 2],
  ['meteringSilenceRatio', 5],
  ['meteringSpectrum', 2],
  ['meteringSpectrumFrame', 3],
  ['meteringStereoCorrelation', 3],
  ['meteringStereoWidth', 3],
  ['meteringTruePeakDb', 3],
  ['meteringVectorscope', 3],
  ['meteringVectorscopeDecimated', 4],
  ['mfcc', 10],
  ['mixStereo', 3],
  ['momentaryLufs', 2],
  ['normalize', 4],
  ['noteMove', 2],
  ['noteStretch', 2],
  ['onsetEnvelope', 5],
  ['onsetStrengthMulti', 6],
  ['percussive', 2],
  ['pitchCorrectTimevarying', 4],
  ['pitchCorrectToMidi', 4],
  ['pitchCorrectToMidiTimevarying', 7],
  ['pitchShift', 5],
  ['pseudoCqt', 6],
  ['roomGeometryFromEstimate', 1],
  ['roomMorph', 2],
  ['shortTermLufs', 2],
  ['spectralEdit', 3],
  ['stft', 4],
  ['stftDb', 4],
  ['tempogramRatio', 5],
  ['timeStretch', 5],
  ['trim', 5],
  ['voiceChange', 2],
  ['voiceChangeRealtime', 3],
  ['vqt', 7],
  ['vqtToAudio', 9],
  ['waveformPeakPyramid', 2],
  ['waveformPeaks', 2],
];

describe('positional options bags refuse unknown keys', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(POSITIONAL_OPTIONS)('%s refuses an undeclared option by name', (name, index) => {
    const first = name === 'mixStereo' ? [] : new Float32Array(2205);
    const args: unknown[] = Array.from({ length: index + 1 }, () => undefined);
    args[0] = first;
    args[index] = { bogus: 1 };
    const call = () => (entry(name) as (...a: unknown[]) => unknown)(...args);
    expect(call).toThrow(TypeError);
    expect(message(call)).toBe(`${name}: unknown option 'bogus'`);
  });

  it('every optionsBag call site is listed', () => {
    const dir = join(import.meta.dirname, '..', 'src');
    const used = new Set<string>();
    for (const file of readdirSync(dir).filter((f) => f.endsWith('.ts'))) {
      const text = readFileSync(join(dir, file), 'utf8');
      for (const m of text.matchAll(/optionsBag\('(\w+)'/g)) {
        used.add(m[1]);
      }
    }
    expect([...used].sort()).toEqual(POSITIONAL_OPTIONS.map(([name]) => name).sort());
  });
});

describe('reported mistakes', () => {
  const samples = new Float32Array(2205);

  beforeAll(async () => {
    await api.init();
  });

  it('masteringChain points a chain section at config', () => {
    expect(
      message(() => api.masteringChain({ samples, sampleRate: 22050, loudness: {} } as never)),
    ).toBe("masteringChain: unknown request key 'loudness' ('loudness' belongs under 'config')");
  });

  it('masterAudio points a chain section at overrides', () => {
    expect(
      message(() => api.masterAudio({ samples, sampleRate: 22050, loudness: {} } as never)),
    ).toBe("masterAudio: unknown request key 'loudness' ('loudness' belongs under 'overrides')");
  });

  it('analyzeWithProgress takes analysis options flat, as analyze does', () => {
    expect(
      message(() =>
        api.analyzeWithProgress({ samples, sampleRate: 22050, hopLength: 512, tuning: 0 } as never),
      ),
    ).not.toMatch(/unknown/);
  });

  it('autoTune points a key field at key', () => {
    expect(message(() => api.autoTune({ samples, sampleRate: 22050, root: 0 } as never))).toBe(
      "autoTune: unknown request key 'root' ('root' belongs under 'key')",
    );
  });

  it('analyzeWithProgress refuses an unknown option, in either form', () => {
    const expected = "analyzeWithProgress: unknown option 'bogus' in 'options'";
    expect(
      message(() =>
        api.analyzeWithProgress({ samples, sampleRate: 22050, options: { bogus: 1 } } as never),
      ),
    ).toBe(expected);
    expect(
      message(() => api.analyzeWithProgress(samples, 22050, () => {}, { bogus: 1 } as never)),
    ).toBe(expected);
  });

  it('detectBpm refuses a range it does not take', () => {
    expect(message(() => api.detectBpm({ samples, sampleRate: 22050, bpmMin: 60 } as never))).toBe(
      "detectBpm: unknown request key 'bpmMin'",
    );
  });

  it('a declared key set to undefined is absent', () => {
    expect(() => api.detectBpm({ samples, sampleRate: undefined })).not.toThrow(
      /unknown request key/,
    );
  });
});
