/**
 * Every request-object entry point refuses a key it does not declare, by name,
 * and accepts each key it does. The key lists come from the request types, so
 * this also pins that no entry point reads a field its request type omits.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import * as api from '../dist/index.js';
import { REQUEST_SHAPES, type RequestFn } from '../src/request_keys';

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

// Shapes of class-method requests; their methods carry their own unknown-key cases.
const METHOD_SHAPES = [
  'Mixer',
  'Mixer.fromJson',
  'RealtimeEngine',
  'SonareEngine.renderOffline',
  'addSend',
  'applyBusInsertParamNow',
  'applyProjectVocalEdit',
  'applyStripInsertParamNow',
  'armCapture',
  'bakeMidiFx',
  'canSetBusSidechain',
  'canSetStripSidechain',
  'configureCapture',
  'configureMeter',
  'importExternalSeparatedStem',
  'prepare',
  'previewMidiFx',
  'pushMidiInputProgram',
  'pushMidiProgram',
  'renderOffline',
  'restoreBusInsertParam',
  'restoreStripInsertParam',
  'scheduleFaderAutomation',
  'scheduleInsertAutomation',
  'schedulePanAutomation',
  'scheduleSendAutomation',
  'scheduleWidthAutomation',
  'setBusInsertBypassed',
  'setBusInsertParam',
  'setBusSidechain',
  'setLoop',
  'setMetronome',
  'setPartRig',
  'setPolarityInvert',
  'setStripInsertBypassed',
  'setStripInsertParam',
  'setStripSidechain',
  'scheduleParam',
  'setTrackSoloMute',
  'transcribeToClip',
  'vocalApply',
  'vocalRender',
];

const names = (Object.keys(REQUEST_SHAPES) as RequestFn[])
  .filter((name) => !METHOD_SHAPES.includes(name))
  .sort();

describe('request objects refuse unknown keys', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(names)('%s refuses an undeclared key by name', (name) => {
    // A bare room estimate is also accepted, so only `estimate` marks the request form.
    const takesEstimate =
      name === 'roomGeometryFromEstimate' || name === 'masteringRepairDereverbConfigForRoom';
    const payload = takesEstimate ? { estimate: {}, bogus: 1 } : { bogus: 1 };
    const call = () => entry(name)(payload);
    expect(call).toThrow(TypeError);
    expect(message(call)).toBe(`${name}: unknown request key 'bogus'`);
  });

  it.each(names)('%s does not refuse any declared key', (name) => {
    for (const key of REQUEST_SHAPES[name].keys) {
      expect(message(() => entry(name)({ [key]: undefined }))).not.toMatch(/unknown request key/);
    }
  });

  it('declares every other shape for a function the package root exports', () => {
    expect(
      names.filter(
        (name) => typeof (api as unknown as Record<string, unknown>)[name] !== 'function',
      ),
    ).toEqual([]);
    expect(METHOD_SHAPES.filter((name) => !(name in REQUEST_SHAPES))).toEqual([]);
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

describe('a class-method request refuses its keys through an instance', () => {
  beforeAll(async () => {
    await api.init();
  });

  it('configureCapture refuses an unknown punch or input-monitor key by name', () => {
    const engine = new api.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, punch: { notAnOption: 1 } } as never),
      ).toThrow("configureCapture: unknown option 'notAnOption' in 'punch'");
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, inputMonitor: { notAnOption: 1 } } as never),
      ).toThrow("configureCapture: unknown option 'notAnOption' in 'inputMonitor'");
    } finally {
      engine.destroy();
    }
  });

  it('the engine and mixer constructors refuse an undeclared key by name', () => {
    expect(() => new api.RealtimeEngine({ sampleRate: 48000, bogus: 1 } as never)).toThrow(
      "RealtimeEngine: unknown request key 'bogus'",
    );
    expect(() => new api.Mixer({ sampleRate: 48000, bogus: 1 } as never)).toThrow(
      "Mixer: unknown request key 'bogus'",
    );
  });
});

/** Positional overloads still read by `optionsBag`, and the index of their options bag. */
const POSITIONAL_OPTIONS: ReadonlyArray<readonly [string, number]> = [
  ['lufsInterleaved', 3],
  ['lufsSeriesInterleaved', 3],
  ['mastering', 2],
  ['masteringDynamicsCompressor', 2],
  ['masteringDynamicsGate', 2],
  ['masteringDynamicsTransientShaper', 2],
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
  ['mixStereo', 3],
  ['waveformPeakPyramid', 2],
  ['waveformPeaks', 2],
];

/** Sample-taking entry points whose options bag is read by the shared `(samples, sampleRate, options)` normalizer, which refuses an unknown key before any defining field. */
const SAMPLE_OPTIONS = [
  'analyze',
  'analyzeBpm',
  'analyzeDynamics',
  'analyzeMelody',
  'analyzeRhythm',
  'analyzeSections',
  'analyzeTimbre',
  'bassChroma',
  'chordFunctionalAnalysis',
  'chroma',
  'chromaCens',
  'chromaCqt',
  'cqt',
  'detectAcoustic',
  'detectBeats',
  'detectBpm',
  'detectChords',
  'detectDownbeats',
  'detectKey',
  'detectKeyCandidates',
  'detectOnsets',
  'estimateRoom',
  'harmonic',
  'hybridCqt',
  'lufs',
  'masteringRepairDeclick',
  'masteringRepairDeclip',
  'masteringRepairDecrackle',
  'masteringRepairDehum',
  'masteringRepairDenoiseClassical',
  'masteringRepairDereverbClassical',
  'masteringRepairDetectClicks',
  'masteringRepairDetectClipping',
  'masteringRepairDetectCrackle',
  'masteringRepairDetectHum',
  'masteringRepairDetectNoiseFloor',
  'masteringRepairDetectReverb',
  'masteringRepairDetectTrimRange',
  'masteringRepairTrimSilence',
  'melSpectrogram',
  'mfcc',
  'momentaryLufs',
  'normalize',
  'noteMove',
  'noteStretch',
  'onsetStrengthMulti',
  'percussive',
  'pitchCorrectTimevarying',
  'pitchCorrectToMidi',
  'pitchShift',
  'pseudoCqt',
  'roomMorph',
  'shortTermLufs',
  'spectralEdit',
  'stft',
  'stftDb',
  'timeStretch',
  'trim',
  'voiceChange',
  'vqt',
];

describe('sample-taking options bags refuse unknown keys', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(SAMPLE_OPTIONS)('%s refuses an undeclared option by name', (name) => {
    const call = () =>
      (entry(name) as (...a: unknown[]) => unknown)(new Float32Array(2205), 22050, { bogus: 1 });
    expect(call).toThrow(TypeError);
    expect(message(call)).toBe(`${name}: unknown option 'bogus'`);
  });
});

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

/** Entry points that take only a request object, with a valid request body to add the bogus key to. */
const REQUEST_ONLY: ReadonlyArray<readonly [string, () => Record<string, unknown>]> = [
  ['masteringRepairDereverbConfigForRoom', () => ({ estimate: {} })],
  ['roomGeometryFromEstimate', () => ({ estimate: {} })],
  ['voiceChangeRealtime', () => ({ channels: [new Float32Array(2205)], sampleRate: 22050 })],
];

describe('request-only entry points refuse unknown keys', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(REQUEST_ONLY)('%s refuses an undeclared key by name', (name, body) => {
    const call = () => entry(name)({ ...body(), bogus: 1 });
    expect(call).toThrow(TypeError);
    expect(message(call)).toBe(`${name}: unknown request key 'bogus'`);
  });

  it.each(REQUEST_ONLY)('%s reports an unknown key before a missing field', (name) => {
    expect(message(() => entry(name)({ bogus: 1 }))).toBe(`${name}: unknown request key 'bogus'`);
  });

  it.each(REQUEST_ONLY)('%s takes no positional form', (name) => {
    const call = () =>
      (entry(name) as (...a: unknown[]) => unknown)(new Float32Array(2205), 22050, { bogus: 1 });
    expect(call).toThrow(TypeError);
    expect(message(call)).toBe(`${name}: request must be an object`);
  });

  it('pitchCorrectToMidiTimevarying is gone; pitchCorrectTimevarying takes targetMidi', () => {
    expect(
      (api as unknown as Record<string, unknown>).pitchCorrectToMidiTimevarying,
    ).toBeUndefined();
    expect(REQUEST_SHAPES.pitchCorrectTimevarying.keys).toContain('targetMidi');
    const call = () =>
      api.pitchCorrectTimevarying({
        samples: new Float32Array(2205),
        sampleRate: 22050,
        bogus: 1,
      } as never);
    expect(message(call)).toBe("pitchCorrectTimevarying: unknown request key 'bogus'");
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

  it('analyze takes analysis options and progress callbacks flat', () => {
    expect(
      message(() =>
        api.analyze({
          samples,
          sampleRate: 22050,
          hopLength: 512,
          tuningCents: 0,
          onProgress: () => {},
        } as never),
      ),
    ).not.toMatch(/unknown/);
  });

  it('autoTune points a key field at key', () => {
    expect(message(() => api.autoTune({ samples, sampleRate: 22050, root: 0 } as never))).toBe(
      "autoTune: unknown request key 'root' ('root' belongs under 'key')",
    );
  });

  it('detectBpm takes a tempo range and refuses a key it does not take', () => {
    expect(
      message(() => api.detectBpm({ samples, sampleRate: 22050, bpmMin: 60, bpmMax: 180 })),
    ).not.toMatch(/unknown/);
    expect(
      message(() => api.detectBpm({ samples, sampleRate: 22050, hopLength: 512 } as never)),
    ).toBe("detectBpm: unknown request key 'hopLength'");
  });

  it('detectBeats takes a tempo range', () => {
    expect(
      message(() => api.detectBeats({ samples, sampleRate: 22050, bpmMin: 60, bpmMax: 180 })),
    ).not.toMatch(/unknown/);
  });

  it('a declared key set to undefined is absent', () => {
    expect(() => api.detectBpm({ samples, sampleRate: 22050, validate: undefined })).not.toThrow(
      /unknown request key/,
    );
  });
});
