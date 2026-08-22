import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  analyzeWithProgress,
  masterAudio,
  masterAudioAsync,
  masterAudioStereo,
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
type MidiCcBindingCase = {
  id: string;
  field: 'channel' | 'ccNumber';
  value: number;
  accepted: boolean;
};

const corpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/public_input_corpus.json', import.meta.url),
    'utf8',
  ),
) as {
  marker_transaction: MarkerTransaction;
  mix_options: { cases: MixOptionCase[] };
  midi_cc_binding: { cases: MidiCcBindingCase[] };
};

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

  // `channel`, `ccNumber` and the selector fields are uint8_t in the C-ABI
  // binding struct. A value beyond a byte must be rejected rather than reduced
  // modulo 256: `channel: 271` arriving as 15 and `ccNumber: 263` arriving as 7
  // both land INSIDE the range the C ABI's own check accepts, so the binding
  // would be installed against a different control with no error at all.
  it('carries the byte-field corpus, so the loop below is not vacuous', () => {
    expect(corpus.midi_cc_binding.cases.length).toBeGreaterThanOrEqual(11);
    expect(corpus.midi_cc_binding.cases.some((testCase) => testCase.accepted)).toBe(true);
    expect(corpus.midi_cc_binding.cases.some((testCase) => !testCase.accepted)).toBe(true);
  });

  for (const testCase of corpus.midi_cc_binding.cases) {
    const binding = () => ({ ccNumber: 7, paramId: 1, [testCase.field]: testCase.value });

    it(`keeps RealtimeEngine.bindMidiCcBinding byte fields conformant: ${testCase.id}`, () => {
      const engine = new RealtimeEngine(48000, 128);
      try {
        if (testCase.accepted) {
          engine.bindMidiCcBinding(binding() as never);
          expect(engine.midiCcBindingCount()).toBe(1);
        } else {
          expect(() => engine.bindMidiCcBinding(binding() as never)).toThrow(RangeError);
          expect(engine.midiCcBindingCount()).toBe(0);
        }
      } finally {
        engine.destroy();
      }
    });

    it(`keeps Project.midiCcToBreakpoint byte fields conformant: ${testCase.id}`, () => {
      const event = Project.midiCc(0, 0, 0, 7, 64);
      if (testCase.accepted) {
        expect(() => Project.midiCcToBreakpoint([binding() as never], event)).not.toThrow();
      } else {
        expect(() => Project.midiCcToBreakpoint([binding() as never], event)).toThrow(RangeError);
      }
    });
  }

  it('rejects an out-of-byte-range positional MIDI argument', () => {
    const learnEvents = [0, 64, 127].map((value) => Project.midiCc(0, 0, 0, 7, value));

    // The same hazard reached through positional arguments rather than an
    // object key: each of these is a uint8_t C-ABI parameter.
    expect(() => Project.midiParamToCc([], 1, 0, 256)).toThrow(RangeError);
    expect(() => Project.midiParamToCc([], 1, 0, -1)).toThrow(RangeError);
    expect(() => Project.midiBankProgram(0, 256, 0, 0, 0, 24)).toThrow(RangeError);
    expect(() => Project.midiBankProgram(0, 0, 271, 0, 0, 24)).toThrow(RangeError);
    expect(() => Project.midiCcLearn(learnEvents, 3, { minMovement: 256 })).toThrow(RangeError);

    const project = Project.create();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgramOnChannel(clipId, 256, 0, 24)).toThrow(RangeError);
      expect(() => project.setProgramOnChannel(clipId, 0, 271, 24)).toThrow(RangeError);
      // In-range values on the same entry points still succeed.
      expect(() => project.setProgramOnChannel(clipId, 0, 3, 24)).not.toThrow();
    } finally {
      project.destroy();
    }

    // In-range values on the pure helpers still succeed.
    expect(Project.midiBankProgram(0, 0, 3, 0x79, 1, 24).length).toBeGreaterThan(0);
    expect(Project.midiCcLearn(learnEvents, 3, { minMovement: 4 })).not.toBeNull();
  });
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
      // The facade takes (config, destinationId); passing them in the addon's
      // (destinationId, config) order made `config` the number 1, so the
      // explicitly-undefined bag never reached the reader and this test
      // established nothing about undefined-safety.
      expect(() =>
        engine.setBuiltinInstrument(
          {
            waveform: undefined,
            gain: undefined,
            attackMs: undefined,
            decayMs: undefined,
            sustain: undefined,
            releaseMs: undefined,
            polyphony: undefined,
          },
          1,
        ),
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

// The progress and cancel callbacks re-enter JS synchronously, so a callback
// can transfer, detach or overwrite the very ArrayBuffer the run was handed.
// These runs stay unaffected because the C ABI copies the samples before the
// analyzer or chain that owns the callback exists.
//
// READ THE GREEN CAREFULLY: these three cases have no regression power over the
// addon layer. The copy they depend on is one layer down, and it was verified
// by measurement that they pass whether or not the addon takes a copy of its
// own — so a green here is not evidence that this file copies anything. What
// can actually fail if the copy is dropped is the C++ pair that pins it:
// "MasteringChain copies its input before the first progress callback"
// (tests/mastering/chain_test.cpp) and the "copies the input before the first
// progress callback" section of tests/api/sonare_c_core_test.cpp. These cases
// stay as an end-to-end statement that a sabotaging callback is survivable from
// plain JS.
describe('a progress callback cannot corrupt its own input', () => {
  const sampleRate = 22050;
  const tone = (freq: number): Float32Array =>
    new Float32Array(sampleRate).map((_, i) => Math.sin((2 * Math.PI * freq * i) / sampleRate));

  // Overwrites the samples on the first callback, then detaches the backing
  // store on the second, so both corruption routes are exercised in one run.
  function sabotage(view: Float32Array): () => void {
    let step = 0;
    return () => {
      if (step === 0) view.fill(1);
      if (step === 1 && view.byteLength > 0)
        structuredClone(view, { transfer: [view.buffer as ArrayBuffer] });
      step += 1;
    };
  }

  it('analyzeWithProgress keeps its result identical under a sabotaging callback', () => {
    const reference = analyzeWithProgress(tone(440), sampleRate, () => {});
    const victim = tone(440);
    const result = analyzeWithProgress(victim, sampleRate, sabotage(victim));
    expect(victim.byteLength).toBe(0);
    expect(result.bpm).toBe(reference.bpm);
    expect(result.key.name).toBe(reference.key.name);
    expect(result.bpmConfidence).toBe(reference.bpmConfidence);
  });

  it('masterAudio with onProgress keeps its result identical under a sabotaging callback', () => {
    const reference = masterAudio({
      samples: tone(440),
      sampleRate,
      preset: 'pop',
      onProgress: () => {},
    });
    const victim = tone(440);
    const result = masterAudio({
      samples: victim,
      sampleRate,
      preset: 'pop',
      onProgress: sabotage(victim),
    });
    expect(victim.byteLength).toBe(0);
    expect(Array.from(result.samples)).toEqual(Array.from(reference.samples));
  });

  it('masterAudioStereo with onProgress keeps both channels intact', () => {
    const reference = masterAudioStereo({
      left: tone(440),
      right: tone(660),
      sampleRate,
      preset: 'pop',
      onProgress: () => {},
    });
    const left = tone(440);
    const right = tone(660);
    const sabotageLeft = sabotage(left);
    const sabotageRight = sabotage(right);
    const result = masterAudioStereo({
      left,
      right,
      sampleRate,
      preset: 'pop',
      onProgress: () => {
        sabotageLeft();
        sabotageRight();
      },
    });
    expect(left.byteLength).toBe(0);
    expect(right.byteLength).toBe(0);
    expect(Array.from(result.left)).toEqual(Array.from(reference.left));
    expect(Array.from(result.right)).toEqual(Array.from(reference.right));
  });
});

describe('async entry points reject instead of throwing synchronously', () => {
  const badOverrides = { loudness: { targetLufs: 'x' } } as never;
  const invalidAsyncCalls: ReadonlyArray<[string, () => Promise<unknown>, RegExp]> = [
    [
      'masterAudioAsync(undefined)',
      () => masterAudioAsync(undefined as never),
      /Expected \(presetName/,
    ],
    ['masterAudioAsync({})', () => masterAudioAsync({} as never), /Expected \(presetName/],
    [
      'masterAudioAsync without samples',
      () => masterAudioAsync({ sampleRate: 22050 } as never),
      /Expected \(presetName/,
    ],
    [
      'masterAudioStereoAsync(undefined)',
      () => masterAudioStereoAsync(undefined as never),
      /Expected \(presetName/,
    ],
    [
      'masterAudioStereoAsync({})',
      () => masterAudioStereoAsync({} as never),
      /Expected \(presetName/,
    ],
    [
      'masterAudioAsync with a non-numeric override leaf',
      () => masterAudioAsync({ samples: new Float32Array(1024), overrides: badOverrides }),
      /must be a number or boolean/,
    ],
    [
      'masterAudioStereoAsync with a non-numeric override leaf',
      () =>
        masterAudioStereoAsync({
          left: new Float32Array(1024),
          right: new Float32Array(1024),
          overrides: badOverrides,
        }),
      /must be a number or boolean/,
    ],
    [
      'masterAudioAsync (positional) with a non-numeric override leaf',
      () => masterAudioAsync(new Float32Array(1024), 22050, 'pop', badOverrides),
      /must be a number or boolean/,
    ],
    [
      'masterAudioStereoAsync (positional) with a non-numeric override leaf',
      () =>
        masterAudioStereoAsync(
          new Float32Array(1024),
          new Float32Array(1024),
          22050,
          'pop',
          badOverrides,
        ),
      /must be a number or boolean/,
    ],
  ];

  for (const [label, call, expected] of invalidAsyncCalls) {
    it(`rejects rather than throwing: ${label}`, async () => {
      let promise: Promise<unknown> | undefined;
      expect(() => {
        promise = call();
      }).not.toThrow();
      expect(promise).toBeInstanceOf(Promise);
      await expect(promise).rejects.toThrow(expected);
    });
  }
});
