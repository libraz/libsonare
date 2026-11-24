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
import { unknownKeyError, unknownKeyText } from './_helpers';

type Entry = (request: unknown, ...rest: unknown[]) => unknown;

// A class whose constructor takes the request (ClipPageStreamer) is constructed, not called.
function entry(name: string): Entry {
  const fn = (api as unknown as Record<string, unknown>)[name];
  if (typeof fn !== 'function') {
    throw new Error(`${name} is not exported`);
  }
  // A decorated class is exported as a proxy, whose source text no longer reads `class`; its prototype carries methods.
  const proto = (fn as { prototype?: object }).prototype;
  if (proto !== undefined && Object.getOwnPropertyNames(proto).length > 1) {
    const Ctor = fn as new (...args: unknown[]) => unknown;
    return (request, ...rest) => new Ctor(request, ...rest);
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
  'ClipPageProvider',
  'Mixer.fromJson',
  'PlaybackLoudnessMeter',
  'PlaybackRenderer',
  ...(Object.keys(REQUEST_SHAPES) as RequestFn[]).filter((name) => name.startsWith('Project.')),
  'RealtimeEngine',
  'SonareEngine.pushMidiChannelPressure',
  'SonareEngine.pushMidiCc',
  'SonareEngine.pushMidiNoteOff',
  'SonareEngine.pushMidiNoteOn',
  'SonareEngine.pushMidiPitchBend',
  'SonareEngine.pushMidiPolyPressure',
  'SonareEngine.pushMidiProgram',
  'SonareEngine.pushMidiSysex',
  'SonareEngine.pushMidiUmp',
  'SonareEngine.pushMidiInputChannelPressure',
  'SonareEngine.pushMidiInputCc',
  'SonareEngine.pushMidiInputNoteOff',
  'SonareEngine.pushMidiInputNoteOn',
  'SonareEngine.pushMidiInputPitchBend',
  'SonareEngine.pushMidiInputPolyPressure',
  'SonareEngine.pushMidiInputProgram',
  'SonareEngine.pushMidiInputUmp',
  'SonareEngine.renderOffline',
  'SonareEngine.setParam',
  'addSend',
  'applyBusInsertParamNow',
  'applyProjectVocalEdit',
  'applyStripInsertParamNow',
  'armCapture',
  'bakeMidiFx',
  'bounceOffline',
  'canSetBusSidechain',
  'canSetStripSidechain',
  'configureCapture',
  'configureMeter',
  'freezeOffline',
  'importExternalSeparatedStem',
  'prepare',
  'previewMidiFx',
  'process',
  'processPrepared',
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
  'setInputMonitor',
  'setMetronome',
  'setParameter',
  'setPartRig',
  'setPolarityInvert',
  'setStripInsertBypassed',
  'setStripInsertParam',
  'setGraph',
  'setStripSidechain',
  'setStripSurroundPan',
  'setSurroundPan',
  'scheduleParam',
  'setTrackSoloMute',
  'transcribeToClip',
  'vocalApply',
  'vocalRender',
];

// Option bags trailing positional arguments; each entry's own test file carries its unknown-key case.
const TRAILING_OPTION_SHAPES = ['bindWebMidi', 'webMidiEventContextTime'];

const names = (Object.keys(REQUEST_SHAPES) as RequestFn[])
  .filter((name) => !METHOD_SHAPES.includes(name) && !TRAILING_OPTION_SHAPES.includes(name))
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
    expect(message(call)).toMatch(unknownKeyText(`${name}: unknown key bogus`));
  });

  it.each(names)('%s does not refuse any declared key', (name) => {
    for (const key of REQUEST_SHAPES[name].keys) {
      expect(message(() => entry(name)({ [key]: undefined }))).not.toMatch(/unknown key /);
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

/** Positional arguments ahead of the request object on `Project` methods that take one. */
const PROJECT_LEADING_ARGS: Partial<Record<string, unknown[]>> = {
  'Project.loadSoundFont': [new Uint8Array(4)],
  'Project.moveClip': [0, 0],
  'Project.setClipFade': [0],
  'Project.setClipLoop': [0],
  'Project.setClipTakes': [0, []],
  'Project.setProgram': [0, 0],
};

describe('a class-method request refuses its keys through an instance', () => {
  beforeAll(async () => {
    await api.init();
  });

  it('configureCapture refuses an unknown punch key and the input monitor by name', () => {
    const engine = new api.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, punch: { notAnOption: 1 } } as never),
      ).toThrow(unknownKeyText('configureCapture: unknown key punch.notAnOption'));
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, inputMonitor: { enabled: false } } as never),
      ).toThrow(unknownKeyText('configureCapture: unknown key inputMonitor'));
      expect(() => engine.setInputMonitor({ enabled: true, notAnOption: 1 } as never)).toThrow(
        unknownKeyText('setInputMonitor: unknown key notAnOption'),
      );
    } finally {
      engine.destroy();
    }
  });

  it('setGraph refuses an undeclared key by name before reading the graph', () => {
    const engine = new api.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() => engine.setGraph({ bogus: 1 } as never)).toThrow(
        unknownKeyError('RealtimeEngine.setGraph: unknown key bogus'),
      );
    } finally {
      engine.destroy();
    }
  });

  it.each(Object.keys(REQUEST_SHAPES.setGraph.nested))(
    'setGraph refuses an undeclared key inside %s[0] by its path',
    (field) => {
      const engine = new api.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        const spec = {
          nodes: [{ id: 'a', type: 'gain' }],
          connections: [],
          inputNode: 'a',
          outputNode: 'a',
          parameterBindings: [],
          [field]: [{ bogus: 1 }],
        };
        expect(() => engine.setGraph(spec as never)).toThrow(
          unknownKeyError(`RealtimeEngine.setGraph: unknown key ${field}[0].bogus`),
        );
      } finally {
        engine.destroy();
      }
    },
  );

  it.each(
    (Object.keys(REQUEST_SHAPES) as RequestFn[]).filter((name) => name.startsWith('Project.')),
  )('%s refuses an undeclared key by name through an instance', (name) => {
    const project = new api.Project();
    try {
      const method = (project as unknown as Record<string, (...args: unknown[]) => unknown>)[
        name.slice('Project.'.length)
      ];
      const call = () => method.call(project, ...(PROJECT_LEADING_ARGS[name] ?? []), { bogus: 1 });
      expect(call).toThrow(TypeError);
      expect(message(call)).toMatch(unknownKeyText(`${name}: unknown key bogus`));
    } finally {
      project.destroy();
    }
  });

  it('the engine and mixer constructors refuse an undeclared key by name', () => {
    expect(() => new api.RealtimeEngine({ sampleRate: 48000, bogus: 1 } as never)).toThrow(
      unknownKeyText('RealtimeEngine: unknown key bogus'),
    );
    expect(() => new api.Mixer({ sampleRate: 48000, bogus: 1 } as never)).toThrow(
      unknownKeyText('Mixer: unknown key bogus'),
    );
    expect(
      () =>
        new api.ClipPageProvider({
          numChannels: 1,
          numSamples: 8,
          pageFrames: 4,
          bogus: 1,
        } as never),
    ).toThrow(unknownKeyText('ClipPageProvider: unknown key bogus'));
  });
});

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
  'ebur128LoudnessRangeLu',
  'estimateRoom',
  'harmonic',
  'hybridCqt',
  'lufs',
  'masteringDynamicsCompressor',
  'masteringDynamicsGate',
  'masteringDynamicsTransientShaper',
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
  'meteringCrestFactorDb',
  'meteringDcOffset',
  'meteringDetectClipping',
  'meteringDynamicRange',
  'meteringPeakDb',
  'meteringRmsDb',
  'meteringSilenceRatio',
  'meteringSpectrum',
  'meteringSpectrumFrame',
  'meteringTruePeakDbtp',
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
    expect(message(call)).toMatch(unknownKeyText(`${name}: unknown key bogus`));
  });
});

describe('positional options bags refuse unknown keys', () => {
  it('no positional options bag is read outside the shared normalizers', () => {
    const dir = join(import.meta.dirname, '..', 'src');
    const used = new Set<string>();
    for (const file of readdirSync(dir).filter((f) => f.endsWith('.ts'))) {
      const text = readFileSync(join(dir, file), 'utf8');
      for (const m of text.matchAll(/optionsBag\('(\w+)'/g)) {
        used.add(m[1]);
      }
    }
    expect([...used]).toEqual([]);
  });
});

const stereoBody = () => ({
  left: new Float32Array(2205),
  right: new Float32Array(2205),
  sampleRate: 22050,
});

/** Entry points that take only a request object, with a valid request body to add the bogus key to. */
const REQUEST_ONLY: ReadonlyArray<readonly [string, () => Record<string, unknown>]> = [
  ['masteringRepairDereverbConfigForRoom', () => ({ estimate: {} })],
  ['roomGeometryFromEstimate', () => ({ estimate: {} })],
  ['realtimeVoiceChangerRender', () => ({ channels: [new Float32Array(2205)], sampleRate: 22050 })],
  ['lufsMultichannel', () => ({ channels: [new Float32Array(2205)], sampleRate: 22050 })],
  ['lufsSeriesMultichannel', () => ({ channels: [new Float32Array(2205)], sampleRate: 22050 })],
  ['meteringCrestFactorDbStereo', () => stereoBody()],
  ['meteringPhaseScope', () => stereoBody()],
  ['meteringStereoCorrelation', () => stereoBody()],
  ['meteringStereoWidth', () => stereoBody()],
  ['meteringVectorscope', () => stereoBody()],
  ['waveformPeakPyramid', () => ({ channels: [new Float32Array(2205)] })],
  ['waveformPeaks', () => ({ channels: [new Float32Array(2205)] })],
];

describe('request-only entry points refuse unknown keys', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(REQUEST_ONLY)('%s refuses an undeclared key by name', (name, body) => {
    const call = () => entry(name)({ ...body(), bogus: 1 });
    expect(call).toThrow(TypeError);
    expect(message(call)).toMatch(unknownKeyText(`${name}: unknown key bogus`));
  });

  it.each(REQUEST_ONLY)('%s reports an unknown key before a missing field', (name) => {
    expect(message(() => entry(name)({ bogus: 1 }))).toMatch(
      unknownKeyText(`${name}: unknown key bogus`),
    );
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
    expect(message(call)).toMatch(unknownKeyText('pitchCorrectTimevarying: unknown key bogus'));
  });
});

describe('reported mistakes', () => {
  const samples = new Float32Array(2205);

  beforeAll(async () => {
    await api.init();
  });

  it('streamingLoudnessGain refuses a chain section left at the top level', () => {
    expect(
      message(() =>
        api.streamingLoudnessGain({ samples, sampleRate: 22050, loudness: {} } as never),
      ),
    ).toMatch(unknownKeyText('streamingLoudnessGain: unknown key loudness'));
  });

  it('masterAudio refuses a chain section left at the top level', () => {
    expect(
      message(() => api.masterAudio({ samples, sampleRate: 22050, loudness: {} } as never)),
    ).toMatch(unknownKeyText('masterAudio: unknown key loudness'));
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
    expect(message(() => api.autoTune({ samples, sampleRate: 22050, root: 0 } as never))).toMatch(
      unknownKeyText('autoTune: unknown key root', 'key.root'),
    );
  });

  it('detectBpm takes a tempo range and refuses a key it does not take', () => {
    expect(
      message(() => api.detectBpm({ samples, sampleRate: 22050, bpmMin: 60, bpmMax: 180 })),
    ).not.toMatch(/unknown/);
    expect(
      message(() => api.detectBpm({ samples, sampleRate: 22050, hopLength: 512 } as never)),
    ).toMatch(unknownKeyText('detectBpm: unknown key hopLength'));
  });

  it('detectBeats takes a tempo range', () => {
    expect(
      message(() => api.detectBeats({ samples, sampleRate: 22050, bpmMin: 60, bpmMax: 180 })),
    ).not.toMatch(/unknown/);
  });

  it('a declared key set to undefined is absent', () => {
    expect(() => api.detectBpm({ samples, sampleRate: 22050, bpmMin: undefined })).not.toThrow(
      /unknown key /,
    );
  });
});

describe('record fields inside a request refuse an undeclared key by its path', () => {
  const samples = new Float32Array(22050).fill(0.1);
  const platform = { name: 'Unit Test', targetLufs: -14, ceilingDbtp: -1 };
  const placement = { x: 1, y: 1, z: 1.2 };

  beforeAll(async () => {
    await api.init();
  });

  it('masteringStreamingPreview refuses a key no platform declares', () => {
    expect(() =>
      api.masteringStreamingPreview({
        samples,
        sampleRate: 22050,
        platforms: [platform, { ...platform, ceiling: -1 }],
      } as never),
    ).toThrow(
      new TypeError(
        'masteringStreamingPreview: unknown key platforms[1].ceiling (expected ceilingDbtp, name, targetLufs)',
      ),
    );
  });

  it('synthesizeRir and roomGeometryFromEstimate refuse a key no placement declares', () => {
    expect(() =>
      api.synthesizeRir({
        sampleRate: 22050,
        lengthM: 7,
        widthM: 5,
        heightM: 3,
        listener: { ...placement, w: 0 },
      } as never),
    ).toThrow(new TypeError('synthesizeRir: unknown key listener.w (expected x, y, z)'));
    const rir = api.synthesizeRir({ sampleRate: 48000, lengthM: 7, widthM: 5, heightM: 3 });
    const estimate = api.estimateRoom(rir.rir, 48000);
    expect(() =>
      api.roomGeometryFromEstimate({ estimate, source: { ...placement, height: 1 } } as never),
    ).toThrow(
      new TypeError('roomGeometryFromEstimate: unknown key source.height (expected x, y, z)'),
    );
  });
});
