import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  analyzeWithProgress,
  masterAudioAsync,
  masterAudioStereoAsync,
  mixStereo,
  noteSegments,
  Project,
  pcen,
  pitchCorrectTimevarying,
  RealtimeEngine,
  spectralEdit,
} from '../src/index.js';
import type { MixOptions } from '../src/types.js';

type CorpusMarker = { id: number | 'uint32_max'; ppq: number | 'nan' | 'inf'; name: string };
type CorpusCase = {
  id: string;
  accepted: boolean;
  markers: CorpusMarker[];
};
type MarkerTransaction = { initial: CorpusMarker[]; cases: CorpusCase[] };
type MixOptionCase = {
  id: string;
  field: 'input_trim_db' | 'fader_db' | 'pan' | 'pan_mode' | 'width';
  value: number | 'nan' | 'inf' | 'neg_inf' | 'unknown';
  accepted: false;
};

const corpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/public_input_corpus.json', import.meta.url),
    'utf8',
  ),
) as { marker_transaction: MarkerTransaction; mix_options: { cases: MixOptionCase[] } };

const markerValue = (marker: CorpusMarker) => ({
  id: marker.id === 'uint32_max' ? 0xffffffff : marker.id,
  ppq:
    marker.ppq === 'nan'
      ? Number.NaN
      : marker.ppq === 'inf'
        ? Number.POSITIVE_INFINITY
        : marker.ppq,
  name: marker.name,
});

const snapshot = (engine: RealtimeEngine): string =>
  JSON.stringify(Array.from({ length: engine.markerCount() }, (_, i) => engine.markerByIndex(i)));

const mixOptionValue = (value: MixOptionCase['value']): number | string => {
  if (value === 'nan') {
    return Number.NaN;
  }
  if (value === 'inf') {
    return Number.POSITIVE_INFINITY;
  }
  if (value === 'neg_inf') {
    return Number.NEGATIVE_INFINITY;
  }
  return value;
};

const mixOptions = (testCase: MixOptionCase): MixOptions => {
  const value = mixOptionValue(testCase.value);
  switch (testCase.field) {
    case 'input_trim_db':
      return { inputTrimDb: value as number };
    case 'fader_db':
      return { faderDb: value as number };
    case 'pan':
      return { pan: value as number };
    case 'pan_mode':
      return { pan: 0, panMode: value as MixOptions['panMode'] };
    case 'width':
      return { width: value as number };
  }
};

describe('shared public-input conformance corpus', () => {
  for (const testCase of corpus.marker_transaction.cases) {
    it(`keeps marker transactions conformant: ${testCase.id}`, () => {
      const engine = new RealtimeEngine(48000, 128);
      try {
        engine.setMarkers(corpus.marker_transaction.initial.map(markerValue));
        const before = snapshot(engine);
        const candidate = testCase.markers.map(markerValue);
        if (testCase.accepted) {
          engine.setMarkers(candidate);
          expect(engine.markerCount()).toBe(candidate.length);
          for (let i = 0; i < candidate.length; i++) {
            expect(engine.markerByIndex(i)).toMatchObject(candidate[i]);
          }
        } else {
          expect(() => engine.setMarkers(candidate)).toThrow();
          expect(snapshot(engine)).toBe(before);
        }
      } finally {
        engine.destroy();
      }
    });
  }

  for (const testCase of corpus.mix_options.cases) {
    it(`keeps mix option rejection conformant: ${testCase.id}`, () => {
      const channel = new Float32Array([0.25, -0.25]);
      const options = mixOptions(testCase);
      expect(() => mixStereo([channel], [channel], 48000, options)).toThrow();
      expect(() =>
        mixStereo({
          leftChannels: [channel],
          rightChannels: [channel],
          sampleRate: 48000,
          ...options,
        }),
      ).toThrow();
      const positional = mixStereo([channel], [channel], 48000);
      const request = mixStereo({ leftChannels: [channel], rightChannels: [channel] });
      expect(Array.from(request.left)).toEqual(Array.from(positional.left));
      expect(Array.from(request.right)).toEqual(Array.from(positional.right));
    });
  }
});

// An optional field declared `k?: T` accepts an explicit `undefined` in
// TypeScript, so `{ k: undefined }` reaches the addon whenever a caller spreads
// a partially-populated options object. Every options-accepting entry point must
// read it exactly like an omitted field: same result, same defaults, and never a
// process abort from a second N-API throw landing on a pending exception.
describe('explicit undefined reads as an omitted option', () => {
  const noteOn = (note: number) => ({
    ppq: 0,
    data0: (0x2 << 28) | (0x9 << 20) | (note << 8) | 100,
  });

  it('keeps the Project.midiRouteEvents thru default with an explicit undefined', () => {
    const events = [noteOn(60)];
    const omitted = Project.midiRouteEvents(events, {});
    const explicit = Project.midiRouteEvents(events, { thru: undefined });
    expect(omitted.events.length).toBe(1);
    expect(explicit).toEqual(omitted);
  });

  it('keeps every Project.midiRouteEvents config field undefined-safe', () => {
    const events = [noteOn(60)];
    expect(
      Project.midiRouteEvents(events, {
        filterGroup: undefined,
        filterChannel: undefined,
        remapChannel: undefined,
        thru: undefined,
      }),
    ).toEqual(Project.midiRouteEvents(events, {}));
  });

  it('routes events whose optional data1 is explicitly undefined', () => {
    const events = [noteOn(60), noteOn(64)];
    expect(
      Project.midiRouteEvents(
        events.map((event) => ({ ...event, data1: undefined })),
        {},
      ),
    ).toEqual(Project.midiRouteEvents(events, {}));
  });

  it('learns a CC binding with every option explicitly undefined', () => {
    const cc = (value: number) => ({ ppq: 0, data0: (0x2 << 28) | (0xb << 20) | (7 << 8) | value });
    const events = [cc(0), cc(64), cc(127)];
    expect(
      Project.midiCcLearn(events, 3, {
        minValue: undefined,
        maxValue: undefined,
        minMovement: undefined,
      }),
    ).toEqual(Project.midiCcLearn(events, 3, {}));
  });

  it('applies pcen defaults for explicitly undefined options', () => {
    const nBins = 4;
    const nFrames = 8;
    const values = new Float32Array(nBins * nFrames).map((_, i) => (i % 5) * 0.1);
    const omitted = pcen(values, nBins, nFrames, {});
    const explicit = pcen(values, nBins, nFrames, {
      sampleRate: undefined,
      hopLength: undefined,
      timeConstant: undefined,
      gain: undefined,
      bias: undefined,
      power: undefined,
      eps: undefined,
    });
    expect(Array.from(explicit)).toEqual(Array.from(omitted));
  });

  it('applies noteSegments config defaults for explicitly undefined fields', () => {
    const f0Hz = new Float32Array(200).fill(440);
    const voicedProb = new Float32Array(200).fill(0.9);
    expect(
      noteSegments({
        f0Hz,
        voicedProb,
        frameRate: 100,
        config: {
          segmentationThresholdCents: undefined,
          minNoteMs: undefined,
          referenceHz: undefined,
        },
      }),
    ).toEqual(noteSegments({ f0Hz, voicedProb, frameRate: 100 }));
  });

  it('applies spectralEdit op and config defaults for explicitly undefined fields', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(4096).map((_, i) =>
      Math.sin((2 * Math.PI * 440 * i) / sampleRate),
    );
    const omitted = spectralEdit(samples, sampleRate, [
      { lowHz: 1000, highHz: 3000, mode: 'mute' },
    ]);
    const explicit = spectralEdit(
      samples,
      sampleRate,
      [
        {
          lowHz: 1000,
          highHz: 3000,
          mode: 'mute',
          startSample: undefined,
          endSample: undefined,
          gainDb: undefined,
        },
      ],
      { nFft: undefined, hopLength: undefined, healRadiusFrames: undefined, window: undefined },
    );
    expect(Array.from(explicit)).toEqual(Array.from(omitted));
  });

  it('schedules MIDI clips whose optional fields are explicitly undefined', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() =>
        engine.setMidiClips([
          {
            id: 1,
            trackId: 1,
            startSample: 0,
            lengthSamples: 4800,
            startPpq: undefined,
            loopLengthSamples: undefined,
            destinationId: undefined,
            events: [{ renderFrame: 0, word0: 0x20903c64, wordCount: undefined, group: undefined }],
          },
        ]),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('binds a builtin instrument whose config fields are explicitly undefined', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() =>
        engine.setBuiltinInstrument(1, {
          waveform: undefined,
          gain: undefined,
          attackMs: undefined,
          decayMs: undefined,
          sustain: undefined,
          releaseMs: undefined,
          polyphony: undefined,
        }),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('keeps engine struct readers undefined-safe across the RealtimeEngine surface', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() =>
        engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: undefined }]),
      ).not.toThrow();
      expect(() =>
        engine.setMetronome({
          enabled: true,
          beatGain: undefined,
          accentGain: undefined,
          clickSamples: undefined,
          clickSeconds: undefined,
        }),
      ).not.toThrow();
      expect(() =>
        engine.setMarkers([{ id: 1, ppq: 0, name: undefined, kind: undefined }]),
      ).not.toThrow();
      expect(() =>
        engine.bindMidiCcBinding({
          ccNumber: 7,
          paramId: 1,
          channel: undefined,
          kind: undefined,
          ccLsbNumber: undefined,
          selectorMsb: undefined,
          selectorLsb: undefined,
          minValue: undefined,
          maxValue: undefined,
        }),
      ).not.toThrow();
      expect(() =>
        engine.setSf2Instrument({
          gain: undefined,
          polyphony: undefined,
          preferModelForModeledFamilies: undefined,
        }),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('applies the pitchCorrectTimevarying mode default for an explicit undefined', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(4096).map((_, i) =>
      Math.sin((2 * Math.PI * 220 * i) / sampleRate),
    );
    const f0Hz = new Float32Array(16).fill(220);
    const omitted = pitchCorrectTimevarying(samples, f0Hz, sampleRate, 256, {});
    const explicit = pitchCorrectTimevarying(samples, f0Hz, sampleRate, 256, { mode: undefined });
    expect(Array.from(explicit)).toEqual(Array.from(omitted));
  });
});

describe('a throwing progress callback surfaces as a catchable JS exception', () => {
  it('propagates an onProgress throw out of analyzeWithProgress', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(sampleRate).map((_, i) =>
      Math.sin((2 * Math.PI * 440 * i) / sampleRate),
    );
    expect(() =>
      analyzeWithProgress(samples, sampleRate, () => {
        throw new Error('boom from onProgress');
      }),
    ).toThrow('boom from onProgress');
  });
});

describe('async entry points reject instead of throwing synchronously', () => {
  const invalidAsyncCalls: ReadonlyArray<[string, () => Promise<unknown>]> = [
    ['masterAudioAsync(undefined)', () => masterAudioAsync(undefined as never)],
    ['masterAudioAsync({})', () => masterAudioAsync({} as never)],
    ['masterAudioAsync without samples', () => masterAudioAsync({ sampleRate: 22050 } as never)],
    ['masterAudioStereoAsync(undefined)', () => masterAudioStereoAsync(undefined as never)],
    ['masterAudioStereoAsync({})', () => masterAudioStereoAsync({} as never)],
  ];

  for (const [label, call] of invalidAsyncCalls) {
    it(`rejects rather than throwing: ${label}`, async () => {
      let promise: Promise<unknown> | undefined;
      expect(() => {
        promise = call();
      }).not.toThrow();
      expect(promise).toBeInstanceOf(Promise);
      await expect(promise).rejects.toThrow(/Expected \(presetName/);
    });
  }
});
