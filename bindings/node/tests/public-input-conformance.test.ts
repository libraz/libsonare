import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeMelody,
  cqt,
  detectBeats,
  detectBpm,
  gm2DrumName,
  gm2DrumSetName,
  gm2InstrumentName,
  gmDrumName,
  gmFamilyFirstProgram,
  gmFamilyName,
  gmInstrumentName,
  hybridCqt,
  Mixer,
  masterAudio,
  masterAudioAsync,
  masterAudioStereo,
  masterAudioStereoAsync,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringInsertTiming,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
  midiBankProgram,
  midiCc,
  midiCcLearn,
  midiCcName,
  midiCcToBreakpoint,
  midiNoteOff,
  midiNoteOn,
  midiParamToCc,
  midiRouteEvents,
  mixingScenePreset,
  mixStereo,
  noteSegments,
  Project,
  pcen,
  perNoteControllerName,
  piptrack,
  pitchCorrectTimevarying,
  pitchPyin,
  pitchYin,
  pseudoCqt,
  RealtimeEngine,
  roomMorphStereo,
  SonareError,
  StreamingRetune,
  spectralEdit,
  transcribe,
  trim,
  vqt,
} from '../src/index.js';
import type { MixStereoStrip } from '../src/mixing_oneshot.js';
import { addon } from '../src/native.js';

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
  field: 'channel' | 'ccNumber' | 'ccLsbNumber' | 'selectorMsb' | 'selectorLsb';
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

const snapshot = (engine: RealtimeEngine): string => JSON.stringify(engine.markers());

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

type MixStripOptions = Omit<MixStereoStrip, 'left' | 'right'>;

const mixOptions = (testCase: MixOptionCase): MixStripOptions => {
  const value = mixOptionValue(testCase.value);
  switch (testCase.field) {
    case 'input_trim_db':
      return { inputTrimDb: value as number };
    case 'fader_db':
      return { faderDb: value as number };
    case 'pan':
      return { pan: value as number };
    case 'pan_mode':
      return { pan: 0, panMode: value as MixStripOptions['panMode'] };
    case 'width':
      return { width: value as number };
  }
};

describe('shared public-input conformance corpus', () => {
  for (const testCase of corpus.marker_transaction.cases) {
    it(`keeps marker transactions conformant: ${testCase.id}`, () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        engine.setMarkers(corpus.marker_transaction.initial.map(markerValue));
        const before = snapshot(engine);
        const candidate = testCase.markers.map(markerValue);
        if (testCase.accepted) {
          engine.setMarkers(candidate);
          expect(engine.markers().length).toBe(candidate.length);
          for (let i = 0; i < candidate.length; i++) {
            expect(engine.markers()[i]).toMatchObject(candidate[i]);
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
      expect(() =>
        mixStereo({
          strips: [{ left: channel, right: channel, ...options }],
          sampleRate: 48000,
        }),
      ).toThrow();
      expect(() =>
        mixStereo({ strips: [{ left: channel, right: channel }], sampleRate: 48000 }),
      ).not.toThrow();
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

    it(`keeps RealtimeEngine.bindMidiCc byte fields conformant: ${testCase.id}`, () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        if (testCase.accepted) {
          engine.bindMidiCc(binding() as never);
          expect(engine.midiCcBindingCount).toBe(1);
        } else {
          expect(() => engine.bindMidiCc(binding() as never)).toThrow(RangeError);
          expect(engine.midiCcBindingCount).toBe(0);
        }
      } finally {
        engine.destroy();
      }
    });

    it(`keeps Project.midiCcToBreakpoint byte fields conformant: ${testCase.id}`, () => {
      const event = midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 64 });
      if (testCase.accepted) {
        expect(() => midiCcToBreakpoint({ bindings: [binding() as never], event })).not.toThrow();
      } else {
        expect(() => midiCcToBreakpoint({ bindings: [binding() as never], event })).toThrow(
          RangeError,
        );
      }
    });
    it(`keeps midiParamToCc byte fields conformant: ${testCase.id}`, () => {
      const request = { bindings: [binding() as never], paramId: 1, unitValue: 0, group: 0 };
      if (testCase.accepted) {
        expect(() => midiParamToCc(request)).not.toThrow();
      } else {
        expect(() => midiParamToCc(request)).toThrow(RangeError);
      }
    });
  }

  // Each byte field with the binding kind that reads it and an in-range value the
  // library accepts there (a 14-bit LSB controller is the MSB's pair, 32..63).
  const midiCcByteFields = {
    ccNumber: ['controlChange7', 127],
    ccLsbNumber: ['controlChange14', 33],
    selectorMsb: ['rpn', 127],
    selectorLsb: ['nrpn', 127],
  } as const;

  for (const [field, [kind, accepted]] of Object.entries(midiCcByteFields)) {
    for (const value of [128, 255]) {
      // The C fields are uint8, so 128..255 fit the struct and only the MIDI domain refuses them.
      it(`refuses ${field} = ${value}, above the MIDI data range, by name`, () => {
        const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
        try {
          const binding = (byte: number) => ({ ccNumber: 1, paramId: 1, kind, [field]: byte });
          expect(() => engine.bindMidiCc(binding(value) as never)).toThrow(
            new RangeError(`RealtimeEngine.bindMidiCc: ${field} must be an integer in [0, 127]`),
          );
          expect(engine.midiCcBindingCount).toBe(0);
          engine.bindMidiCc(binding(accepted) as never);
          expect(engine.midiCcBindingCount).toBe(1);
        } finally {
          engine.destroy();
        }
      });
    }
  }

  // The corpus above drives only out-of-range/fractional byte values (all
  // RangeError). A present, wrong-TYPE byte field used to report a bare, unnamed
  // "A number was expected" instead of naming which field was wrong.
  it('names the field for a wrong-typed MidiByteProperty value instead of an anonymous type error', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() => engine.bindMidiCc({ ccNumber: 7, paramId: 1, channel: '3' } as never)).toThrow(
        /^RealtimeEngine\.bindMidiCc: channel must be a number$/,
      );
      expect(engine.midiCcBindingCount).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  it('rejects an out-of-byte-range positional MIDI argument', () => {
    const learnEvents = [0, 64, 127].map((value) =>
      midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value }),
    );

    // The same hazard reached through positional arguments rather than an
    // object key: each of these is a uint8_t C-ABI parameter.
    expect(() => midiParamToCc({ bindings: [], paramId: 1, unitValue: 0, group: 256 })).toThrow(
      RangeError,
    );
    expect(() => midiParamToCc({ bindings: [], paramId: 1, unitValue: 0, group: -1 })).toThrow(
      RangeError,
    );
    expect(() =>
      midiBankProgram({ ppq: 0, group: 256, channel: 0, program: 24, bank: { msb: 0, lsb: 0 } }),
    ).toThrow(RangeError);
    expect(() =>
      midiBankProgram({ ppq: 0, group: 0, channel: 271, program: 24, bank: { msb: 0, lsb: 0 } }),
    ).toThrow(RangeError);
    expect(() => midiCcLearn({ events: learnEvents, paramId: 3, minMovement: 256 })).toThrow(
      RangeError,
    );
    // 128..255 fit the byte but not the 7-bit CC units the core compares in.
    expect(() => midiCcLearn({ events: learnEvents, paramId: 3, minMovement: 128 })).toThrow(
      RangeError,
    );
    expect(() => midiCcLearn({ events: learnEvents, paramId: 3, minMovement: 255 })).toThrow(
      RangeError,
    );

    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgram(clipId, 24, { group: 256, channel: 0 })).toThrow(RangeError);
      expect(() => project.setProgram(clipId, 24, { group: 0, channel: 271 })).toThrow(RangeError);
      // In-range values on the same entry points still succeed.
      expect(() => project.setProgram(clipId, 24, { group: 0, channel: 3 })).not.toThrow();
    } finally {
      project.destroy();
    }

    // In-range values on the pure helpers still succeed.
    expect(
      midiBankProgram({ ppq: 0, group: 0, channel: 3, program: 24, bank: { msb: 0x79, lsb: 1 } })
        .length,
    ).toBeGreaterThan(0);
    expect(midiCcLearn({ events: learnEvents, paramId: 3, minMovement: 4 })).not.toBeNull();
    expect(() => midiCcLearn({ events: learnEvents, paramId: 3, minMovement: 127 })).not.toThrow();
  });
});

// The addon's argument readers pick the class by what the refusal stands in for:
// a RangeError is this surface refusing a value its target C type cannot hold, a
// TypeError a value of the wrong JavaScript type. One reader per width, so a
// consumer's `catch (e) { if (e instanceof RangeError) }` cannot miss one.
/**
 * The addon handle behind a Project. The facade refuses every number for a
 * synth token, so the native resolvers' own ordinal checks are reached here.
 */
/** A native project bounce: the interleaved render and the width it was mixed at. */
type NativeBounce = { interleaved: Float32Array; numChannels: number };

const nativeBounce = (
  project: Project,
): {
  bounce(settings: object, builtin?: unknown[], synth?: unknown[]): NativeBounce;
} => (project as unknown as { handle: ReturnType<typeof nativeBounce> }).handle;

describe('a number no target C type can hold is a RangeError on every width', () => {
  it('refuses an unrepresentable number with RangeError', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      // uint8_t: 256 would wrap to 0, a channel the C ABI accepts.
      expect(() => project.setProgram(clipId, 24, { group: 256, channel: 0 })).toThrow(RangeError);
      // int: Int32Value() would wrap 2 ** 31 to -2147483648.
      expect(() => project.setProgram(clipId, 2 ** 31)).toThrow(RangeError);
      // size_t: a negative index would wrap to an enormous positive one.
      expect(() => project.setMaxUndoDepth(-1)).toThrow(RangeError);
      // int, through the native waveform resolver's own pre-narrowing check.
      expect(() => nativeBounce(project).bounce({}, [{ waveform: 2 ** 31 }])).toThrow(RangeError);
    } finally {
      project.destroy();
    }
  });

  // The facade takes the waveform by name only: an unknown name is a RangeError
  // and any number a TypeError. Past the facade the native resolver still reads
  // an ordinal, and answers every unresolvable one with RangeError.
  it('answers every unresolvable waveform by its fault, at the facade and past it', () => {
    for (const waveform of ['noise', 4, -1, 2 ** 31, Number.NaN]) {
      const project = new Project();
      try {
        expect(
          () =>
            project.bounce({
              builtinInstruments: [
                {
                  instrument: { waveform: waveform as unknown as 'sine' },
                },
              ],
            }),
          `waveform ${waveform} must be refused by the facade`,
        ).toThrow(typeof waveform === 'string' ? RangeError : TypeError);
        expect(
          () => nativeBounce(project).bounce({}, [{ waveform }]),
          `waveform ${waveform} must be refused as out of range by the addon`,
        ).toThrow(RangeError);
      } finally {
        project.destroy();
      }
    }
  });

  it('keeps TypeError for a value of the wrong JavaScript type', () => {
    // The contrast that stops the assertions above from being satisfied by a
    // reader that answers RangeError to everything.
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgram(clipId, 'x' as never)).toThrow(TypeError);
      expect(() => project.setMaxUndoDepth('x' as never)).toThrow(TypeError);
      expect(() =>
        project.bounce({
          builtinInstruments: [
            {
              instrument: { waveform: true as unknown as 'sine' },
            },
          ],
        }),
      ).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('still accepts every in-domain waveform, by name and natively by ordinal', () => {
    // Vacuity guard: a resolver that refused everything would satisfy the two
    // cases above.
    const project = new Project();
    try {
      for (const waveform of ['sine', 'saw', 'sawtooth', 'square', 'triangle'] as const) {
        expect(() =>
          project.bounce({ builtinInstruments: [{ instrument: { waveform } }] }),
        ).not.toThrow();
      }
      for (const waveform of [0, 1, 2, 3]) {
        expect(() => nativeBounce(project).bounce({}, [{ waveform }])).not.toThrow();
      }
    } finally {
      project.destroy();
    }
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
    word0: (0x2 << 28) | (0x9 << 20) | (note << 8) | 100,
  });

  it('keeps the Project.midiRouteEvents thru default with an explicit undefined', () => {
    const events = [noteOn(60)];
    const omitted = midiRouteEvents({ events });
    const explicit = midiRouteEvents({ events, thru: undefined });
    expect(omitted.events.length).toBe(1);
    expect(explicit).toEqual(omitted);
  });

  it('keeps every Project.midiRouteEvents config field undefined-safe', () => {
    const events = [noteOn(60)];
    expect(
      midiRouteEvents({
        events,
        filterGroup: undefined,
        filterChannel: undefined,
        remapChannel: undefined,
        thru: undefined,
      }),
    ).toEqual(midiRouteEvents({ events }));
  });

  it('routes events whose optional word1 is explicitly undefined', () => {
    const events = [noteOn(60), noteOn(64)];
    expect(
      midiRouteEvents({ events: events.map((event) => ({ ...event, word1: undefined })) }),
    ).toEqual(midiRouteEvents({ events }));
  });

  it('learns a CC binding with every option explicitly undefined', () => {
    const cc = (value: number) => ({ ppq: 0, word0: (0x2 << 28) | (0xb << 20) | (7 << 8) | value });
    const events = [cc(0), cc(64), cc(127)];
    expect(
      midiCcLearn({
        events,
        paramId: 3,
        minValue: undefined,
        maxValue: undefined,
        minMovement: undefined,
      }),
    ).toEqual(midiCcLearn({ events, paramId: 3 }));
  });

  it('applies pcen defaults for explicitly undefined options', () => {
    const nBins = 4;
    const nFrames = 8;
    const values = new Float32Array(nBins * nFrames).map((_, i) => (i % 5) * 0.1);
    const matrix = { data: values, rows: nBins, cols: nFrames };
    const omitted = pcen({ values: matrix });
    const explicit = pcen({
      values: matrix,
      sampleRate: undefined,
      hopLength: undefined,
      timeConstant: undefined,
      gain: undefined,
      bias: undefined,
      power: undefined,
      eps: undefined,
    });
    expect(Array.from(explicit.data)).toEqual(Array.from(omitted.data));
  });

  it('applies noteSegments config defaults for explicitly undefined fields', () => {
    const f0Hz = new Float32Array(200).fill(440);
    const voicedProb = new Float32Array(200).fill(0.9);
    expect(
      noteSegments({
        f0Hz,
        voicedProb,
        frameRate: 100,
        segmentationThresholdCents: undefined,
        minNoteMs: undefined,
        referenceHz: undefined,
      }),
    ).toEqual(noteSegments({ f0Hz, voicedProb, frameRate: 100 }));
  });

  it('applies spectralEdit op and config defaults for explicitly undefined fields', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(4096).map((_, i) =>
      Math.sin((2 * Math.PI * 440 * i) / sampleRate),
    );
    const omitted = spectralEdit(samples, sampleRate, {
      ops: [{ lowHz: 1000, highHz: 3000, mode: 'mute' }],
    });
    const explicit = spectralEdit(samples, sampleRate, {
      ops: [
        {
          lowHz: 1000,
          highHz: 3000,
          mode: 'mute',
          startSample: undefined,
          endSample: undefined,
          gainDb: undefined,
        },
      ],
      nFft: undefined,
      hopLength: undefined,
      healRadiusFrames: undefined,
      window: undefined,
    });
    expect(Array.from(explicit)).toEqual(Array.from(omitted));
  });

  it('schedules MIDI clips whose optional fields are explicitly undefined', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      // The facade takes (destinationId, config); a swapped order would make
      // `config` a number and the explicitly-undefined bag would never reach
      // the reader.
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
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
          clickSec: undefined,
        }),
      ).not.toThrow();
      expect(() =>
        engine.setMarkers([{ id: 1, ppq: 0, name: undefined, kind: undefined }]),
      ).not.toThrow();
      expect(() =>
        engine.bindMidiCc({
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
        engine.setSf2Instrument(0, {
          gain: undefined,
          polyphony: undefined,
          preferModelForModeledFamilies: undefined,
        }),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('applies the pitchCorrectTimevarying targetMode default for an explicit undefined', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(4096).map((_, i) =>
      Math.sin((2 * Math.PI * 220 * i) / sampleRate),
    );
    const f0Hz = new Float32Array(16).fill(220);
    const omitted = pitchCorrectTimevarying(samples, sampleRate, { f0Hz, hopLength: 256 });
    const explicit = pitchCorrectTimevarying(samples, sampleRate, {
      f0Hz,
      hopLength: 256,
      targetMode: undefined,
    });
    expect(Array.from(explicit)).toEqual(Array.from(omitted));
  });
});

describe('a throwing progress callback surfaces as a catchable JS exception', () => {
  it('propagates an onProgress throw out of analyze', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(sampleRate).map((_, i) =>
      Math.sin((2 * Math.PI * 440 * i) / sampleRate),
    );
    expect(() =>
      analyze(samples, sampleRate, {
        onProgress: () => {
          throw new Error('boom from onProgress');
        },
      }),
    ).toThrow('boom from onProgress');
  });

  // masterAudio runs the same MasteringChain as masteringChain/masterAudio's
  // other progress entry points (mastering.cpp), all of which share the
  // ReportProgress/CancellationRequested pair. A throw must both surface the
  // caller's own error (not a wrapped SonareError) and stop the chain at its
  // next checkpoint instead of finishing the whole render first -- the two
  // halves of the invariant, and neither implies the other: the error message
  // already propagated correctly even when the chain kept running to
  // completion behind it (the C-ABI error check ran after the pending-exception
  // check even before this fix, just not before it).
  it('masterAudio stops the chain early instead of running to completion when onProgress throws', () => {
    // Once onProgress has thrown, N-API silently no-ops any further attempt to
    // call into JS on that context (Napi::Function::Call returns without
    // invoking anything while an exception is pending), so ReportProgress and
    // CancellationRequested never reach the callback again either way -- fixed
    // or not. The onProgress call COUNT therefore cannot tell the two states
    // apart; only the amount of native work the chain went on to do can. CPU
    // time attributed to this process, not wall-clock, carries that: a
    // preempted synchronous call accrues no "user"/"system" time while it is
    // not actually running, so process.cpuUsage() stays close to the real
    // work done regardless of what else this shared machine is doing at the
    // same moment, unlike Date.now().
    const sampleRate = 22050;
    const samples = new Float32Array(sampleRate * 8).map(
      (_, i) => Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.3,
    );
    const cpuMicros = (usage: NodeJS.CpuUsage): number => usage.user + usage.system;

    // Warm up caches/allocations once, unmeasured, so the timed calls below
    // are not skewed by first-call cold start in either direction.
    masterAudio({ samples, sampleRate, preset: 'pop', onProgress: () => {} });

    const baselineStart = process.cpuUsage();
    masterAudio({ samples, sampleRate, preset: 'pop', onProgress: () => {} });
    const baselineCpuUs = cpuMicros(process.cpuUsage(baselineStart));

    const throwStart = process.cpuUsage();
    expect(() =>
      masterAudio({
        samples,
        sampleRate,
        preset: 'pop',
        onProgress: () => {
          throw new Error('boom from onProgress');
        },
      }),
    ).toThrow('boom from onProgress');
    const throwCpuUs = cpuMicros(process.cpuUsage(throwStart));

    // A render cancelled at its first checkpoint bails out of the remaining
    // stages, so it must consume a clear fraction of the full render's own
    // CPU time. Measured on this codebase's 'pop' preset: throwCpuUs/baselineCpuUs
    // sits at ~0.285-0.289 across five repeated trials on a quiet machine (see
    // the finding's DB result for the raw numbers); 0.5 leaves comfortable
    // headroom above that without weakening the assertion to the point of
    // vacuity (0.5 still requires the throwing call to skip real work).
    expect(throwCpuUs).toBeLessThan(Math.max(baselineCpuUs * 0.5, 1));
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
      if (step === 0) {
        view.fill(1);
      }
      if (step === 1 && view.byteLength > 0) {
        structuredClone(view, { transfer: [view.buffer as ArrayBuffer] });
      }
      step += 1;
    };
  }

  it('analyze with onProgress keeps its result identical under a sabotaging callback', () => {
    const reference = analyze(tone(440), sampleRate, { onProgress: () => {} });
    const victim = tone(440);
    const result = analyze(victim, sampleRate, { onProgress: sabotage(victim) });
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
      /samples must be a Float32Array/,
    ],
    [
      'masterAudioAsync({})',
      () => masterAudioAsync({} as never),
      /masterAudioAsync: sampleRate must be an integer/,
    ],
    [
      'masterAudioAsync without samples',
      () => masterAudioAsync({ sampleRate: 22050 } as never),
      /must be a Float32Array/,
    ],
    [
      'masterAudioStereoAsync(undefined)',
      () => masterAudioStereoAsync(undefined as never),
      /request must be an object/,
    ],
    ['masterAudioStereoAsync({})', () => masterAudioStereoAsync({} as never), /left must be/],
    [
      'masterAudioAsync with a non-numeric override leaf',
      () =>
        masterAudioAsync({
          samples: new Float32Array(1024),
          sampleRate: 22050,
          preset: 'pop',
          overrides: badOverrides,
        }),
      /^masterAudio(?:Stereo)?Async: overrides\.loudness\.targetLufs must be a number or a boolean$/,
    ],
    [
      'masterAudioStereoAsync with a non-numeric override leaf',
      () =>
        masterAudioStereoAsync({
          left: new Float32Array(1024),
          right: new Float32Array(1024),
          sampleRate: 22050,
          preset: 'pop',
          overrides: badOverrides,
        }),
      /^masterAudio(?:Stereo)?Async: overrides\.loudness\.targetLufs must be a number or a boolean$/,
    ],
    [
      'masterAudioAsync (positional) with a non-numeric override leaf',
      () =>
        masterAudioAsync(new Float32Array(1024), 22050, { preset: 'pop', overrides: badOverrides }),
      /^masterAudio(?:Stereo)?Async: overrides\.loudness\.targetLufs must be a number or a boolean$/,
    ],
    [
      'masterAudioStereoAsync refuses a positional call',
      () =>
        (masterAudioStereoAsync as (...args: unknown[]) => Promise<unknown>)(
          new Float32Array(1024),
          new Float32Array(1024),
        ),
      /request must be an object/,
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

// Every NativeSynth patch enum reaches the addon through one reader, whose
// ordinal path used to be a bare Int32Value(). That WRAPS, so `2 ** 32 + k`
// arrived as `k` and RENDERED -- the defect's signature is a successful call,
// not a throw, so these compare rendered output rather than the refusal. A
// throw-shaped assertion goes green on the fix while staying green on the
// defect, which is the one thing it must not do.
// The facade refuses every number for these fields; the addon still reads an
// ordinal, so its own domain checks are exercised on the handle directly.
describe('a wrapping ordinal cannot reach a NativeSynth patch enum', () => {
  const bounceOptions = { totalFrames: 4096, numChannels: 1 };

  const midiProject = (): Project => {
    const project = new Project();
    project.setSampleRate(48000);
    const { trackId, clipId } = project.addMidiClip(0, 4);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
    ]);
    return project;
  };

  /** The render as one number, or the error class when the patch is refused. */
  const outcome = (patch: Record<string, unknown>): string => {
    const project = midiProject();
    try {
      const audio = nativeBounce(project).bounce(bounceOptions, undefined, [patch]).interleaved;
      return `render:${audio.reduce((h, s) => (h * 31 + Math.round(s * 1e6)) | 0, 0)}`;
    } catch (error) {
      return (error as Error).constructor.name;
    } finally {
      project.destroy();
    }
  };

  const routing = (source: unknown, destination: unknown) => ({
    engineMode: 'subtractive',
    modRoutings: [{ source, destination, depth: 400 }],
  });

  // `audible` holds two ordinals this patch shape renders differently; the two
  // sample fields have none without a SampleBank, so they carry the wrap check
  // alone rather than a positive control that cannot exist here.
  const fields: {
    name: string;
    patch: (value: unknown) => Record<string, unknown>;
    audible: number[];
  }[] = [
    { name: 'engineMode', patch: (v) => ({ engineMode: v }), audible: [3, 13] },
    {
      name: 'waveform',
      patch: (v) => ({ engineMode: 'subtractive', waveform: v }),
      audible: [2, 4],
    },
    {
      name: 'filterModel',
      patch: (v) => ({ engineMode: 'subtractive', filterModel: v, cutoffHz: 800, resonance: 0.7 }),
      audible: [1, 2],
    },
    {
      name: 'filterOutput',
      patch: (v) => ({
        engineMode: 'subtractive',
        filterOutput: v,
        cutoffHz: 800,
        resonance: 0.7,
      }),
      audible: [1, 3],
    },
    {
      name: 'body',
      patch: (v) => ({ engineMode: 'subtractive', body: v, bodyMix: 0.9 }),
      audible: [2, 3],
    },
    {
      name: 'sampleLoop',
      patch: (v) => ({ engineMode: 'subtractive', sampleLoop: v }),
      audible: [],
    },
    {
      name: 'sampleKeyTrack',
      patch: (v) => ({ engineMode: 'subtractive', sampleKeyTrack: v }),
      audible: [],
    },
    { name: 'modSource', patch: (v) => routing(v, 2), audible: [4, 5] },
    { name: 'modDestination', patch: (v) => routing(3, v), audible: [1, 3] },
  ];

  for (const field of fields) {
    const probe = field.audible[0] ?? 1;

    it(`${field.name}: 2 ** 32 + ${probe} must not render as ${probe}`, () => {
      expect(
        outcome(field.patch(2 ** 32 + probe)),
        `a wrapping ${field.name} ordinal reached the patch as ${probe}`,
      ).not.toBe(outcome(field.patch(probe)));
    });

    it(`${field.name}: an ordinal past the enum is refused rather than rendered`, () => {
      // 5000 does not wrap, so before the reader gained a domain check four of
      // these fields rendered it as an accepted value with no refusal anywhere.
      expect(outcome(field.patch(5000))).toBe('RangeError');
    });

    if (field.audible.length === 2) {
      it(`${field.name}: its two in-domain ordinals still render, and differently`, () => {
        // Without this a reader that refused every ordinal would pass above.
        const [first, second] = field.audible;
        const rendered = [outcome(field.patch(first)), outcome(field.patch(second))];
        expect(rendered[0]).toMatch(/^render:/);
        expect(rendered[1]).toMatch(/^render:/);
        expect(rendered[0]).not.toBe(rendered[1]);
      });
    }
  }
});

// The addon picks the class by the fault, not by where the value came from: a
// value of the wrong JS type is a TypeError, a value of the right type that the
// target domain cannot accept is a RangeError. These three readers each pin one
// end of that, with the wrong-type counterpart alongside so the convention is
// not satisfied by a reader that answers RangeError to everything.
describe('an out-of-domain scalar is a RangeError across the addon readers', () => {
  const tone = new Float32Array(4096).map(
    (_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / 22050),
  );

  it('trim refuses a non-integer frame option with RangeError', () => {
    // The condition catches non-finite AND fractional, both of which the Int32Arg
    // convention assigns to RangeError; the <= 0 branch beside it already did.
    // The facade resolver in front of the addon was split the same way, so this
    // pins both halves of the path a caller actually travels.
    for (const frameLength of [12.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => trim(tone, 22050, { thresholdDb: -60, frameLength, hopLength: 512 })).toThrow(
        RangeError,
      );
    }
    expect(() => trim(tone, 22050, { thresholdDb: -60, frameLength: 0, hopLength: 512 })).toThrow(
      RangeError,
    );
  });

  it('the addon trim reader refuses a non-integer frame option with RangeError', () => {
    // Asserted on the addon directly because the facade resolver above refuses
    // the same value first, so the reader's own class is unreachable from the
    // public entry point and a facade-level case would pass either way.
    for (const frameLength of [12.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => addon.trim(tone, 22050, -60, frameLength, 512)).toThrow(RangeError);
    }
    expect(() => addon.trim(tone, 22050, -60, 'x' as never, 512)).toThrow(TypeError);
    expect(addon.trim(tone, 22050, -60, 1024, 512)).toBeInstanceOf(Float32Array);
  });

  it('trim keeps TypeError for a non-number frame option, and still trims', () => {
    expect(() =>
      trim(tone, 22050, { thresholdDb: -60, frameLength: 'x' as never, hopLength: 512 }),
    ).toThrow(TypeError);
    expect(
      trim(tone, 22050, { thresholdDb: -60, frameLength: 1024, hopLength: 512 }),
    ).toBeInstanceOf(Float32Array);
  });

  it('the mixer refuses a block past its configured size with RangeError', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
      sampleRate: 48000,
      maxBlockSize: 8,
    });
    try {
      const over = Array.from({ length: 3 }, () => new Float32Array(16));
      expect(() => mixer.processStereo(over, over)).toThrow(RangeError);
      // Positive control: a block inside the configured size still processes.
      const fits = Array.from({ length: 3 }, () => new Float32Array(8));
      expect(mixer.processStereo(fits, fits).left).toBeInstanceOf(Float32Array);
    } finally {
      mixer.destroy();
    }
  });

  it('the mixer keeps TypeError for a non-Float32Array strip', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
      sampleRate: 48000,
      maxBlockSize: 8,
    });
    try {
      const bad = Array.from({ length: 3 }, () => [0, 0, 0, 0] as never);
      expect(() => mixer.processStereo(bad, bad)).toThrow(TypeError);
    } finally {
      mixer.destroy();
    }
  });
});

type GmLookupCase = {
  id: string;
  lookup: string;
  args: Record<string, unknown>;
  name?: string;
  value?: number;
  absent?: true;
  refused?: 'range' | 'type';
  key?: string;
  predicate?: string;
};

const camel = (key: string) => key.replace(/_([a-z])/g, (_, c: string) => c.toUpperCase());

const GM_LOOKUPS: Record<string, (args: Record<string, unknown>) => unknown> = {
  gm_instrument_name: (a) => gmInstrumentName(a.program as number),
  gm_family_name: (a) => gmFamilyName(a.family as number),
  gm_family_first_program: (a) => gmFamilyFirstProgram(a.family as number),
  gm2_instrument_name: (a) =>
    gm2InstrumentName({ bankLsb: a.bank_lsb as number, program: a.program as number }),
  gm_drum_name: (a) => gmDrumName(a.note as number),
  gm2_drum_set_name: (a) => gm2DrumSetName(a.bank_lsb as number),
  gm2_drum_name: (a) => gm2DrumName({ bankLsb: a.bank_lsb as number, note: a.note as number }),
  midi_cc_name: (a) => midiCcName(a.controller as number),
  per_note_controller_name: (a) => perNoteControllerName(a.index as number),
};

describe('GM lookups answer the shared corpus with a name, absence or a domain refusal', () => {
  for (const testCase of (corpus as unknown as { gm_lookup: { cases: GmLookupCase[] } }).gm_lookup
    .cases) {
    it(testCase.id, () => {
      const lookup = GM_LOOKUPS[testCase.lookup];
      expect(lookup, testCase.lookup).toBeDefined();
      const call = () => lookup(testCase.args);
      if (testCase.refused !== undefined) {
        const refusal = `${camel(testCase.lookup)}: ${camel(testCase.key ?? '')} ${testCase.predicate}`;
        expect(call).toThrow(
          testCase.refused === 'range' ? new RangeError(refusal) : new TypeError(refusal),
        );
      } else if (testCase.absent) {
        expect(call()).toBeUndefined();
      } else {
        expect(call()).toBe(testCase.name ?? testCase.value);
      }
    });
  }
});

type ReaderSplitCase = {
  id: string;
  key: string;
  value: unknown;
  accepted?: boolean;
  refused?: 'range' | 'type';
  predicate?: string;
};

/** A corpus value: JSON has no NaN or infinity, so those arrive as `{ "$number": "nan" | "inf" }`. */
function corpusValue(value: unknown): unknown {
  if (typeof value === 'object' && value !== null && '$number' in value) {
    const spelled = (value as { $number: string }).$number;
    return spelled === 'nan'
      ? Number.NaN
      : spelled === '-inf'
        ? Number.NEGATIVE_INFINITY
        : Number.POSITIVE_INFINITY;
  }
  return value;
}

describe('an integer and a float field refuse a wrong type by type and a wrong value by domain', () => {
  const split = (
    corpus as unknown as {
      number_reader_split: { base: Record<string, unknown>; cases: ReaderSplitCase[] };
    }
  ).number_reader_split;
  for (const testCase of split.cases) {
    it(testCase.id, () => {
      const call = () =>
        midiNoteOn({ ...split.base, [testCase.key]: corpusValue(testCase.value) } as never);
      if (testCase.accepted) {
        expect(call).not.toThrow();
        return;
      }
      const refusal = `midiNoteOn: ${testCase.key} ${testCase.predicate}`;
      expect(call).toThrow(
        testCase.refused === 'range' ? new RangeError(refusal) : new TypeError(refusal),
      );
    });
  }
});

/**
 * A corpus row this surface does not answer yet, keyed by surface with the reason. It runs as an
 * expected failure, so the row turns red the moment the surface starts answering and the entry
 * has to be removed with the fix.
 */
type PendingCase = ReaderSplitCase & { pending?: Record<string, string> };

const SURFACE = 'node';

type RetuneSection = {
  entries: ('constructor' | 'set_config')[];
  base: Record<string, unknown>;
  cases: PendingCase[];
};

/** The corpus spells keys in snake case; this surface spells them in camel case. */
function camelKey(key: string): string {
  return key.replace(/_([a-z])/g, (_, letter: string) => letter.toUpperCase());
}

function camelRecord(record: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(Object.entries(record).map(([k, v]) => [camelKey(k), v]));
}

describe('the streaming retune controls answer the shared corpus on construction and on setConfig', () => {
  const section = (corpus as unknown as { streaming_retune: RetuneSection }).streaming_retune;
  const base = camelRecord(section.base);
  for (const entry of section.entries) {
    for (const testCase of section.cases) {
      const pending = testCase.pending?.[SURFACE];
      const test = pending ? it.fails : it;
      test(`${entry} ${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
        const key = camelKey(testCase.key);
        const config = { [key]: corpusValue(testCase.value) };
        const live = entry === 'set_config' ? new StreamingRetune(base as never) : undefined;
        const call = () =>
          live
            ? live.setConfig(config as never)
            : new StreamingRetune({ ...base, ...config } as never);
        if (testCase.accepted) {
          expect(call).not.toThrow();
          if (live) {
            expect(live.config()).toEqual({ ...base, ...config });
          }
          return;
        }
        const fn = live ? 'StreamingRetune.setConfig' : 'StreamingRetune';
        const refusal = `${fn}: ${key} ${testCase.predicate}`;
        expect(call).toThrow(
          testCase.refused === 'range' ? new RangeError(refusal) : new TypeError(refusal),
        );
        if (live) {
          expect(live.config()).toEqual(base);
        }
      });
    }
  }
});

type TrimSection = {
  entries: string[];
  sample_rate: number;
  mode: Record<string, string>;
  cases: PendingCase[];
};

describe('the trim detector fields answer the shared corpus on every trim entry', () => {
  const section = (corpus as unknown as { trim_detector: TrimSection }).trim_detector;
  const signal = () => {
    const x = new Float32Array(4800);
    for (let i = 1000; i < 3000; i++) {
      x[i] = 0.5 * Math.sin(i * 0.1);
    }
    return x;
  };
  const calls: Record<string, (options: Record<string, unknown>) => unknown> = {
    mastering_repair_trim_silence: (o) =>
      masteringRepairTrimSilence({
        samples: signal(),
        sampleRate: section.sample_rate,
        ...o,
      } as never),
    mastering_repair_trim_silence_stereo: (o) =>
      masteringRepairTrimSilenceStereo({
        left: signal(),
        right: signal(),
        sampleRate: section.sample_rate,
        ...o,
      } as never),
    mastering_repair_detect_trim_range: (o) =>
      masteringRepairDetectTrimRange({
        samples: signal(),
        sampleRate: section.sample_rate,
        ...o,
      } as never),
    mastering_repair_detect_trim_range_stereo: (o) =>
      masteringRepairDetectTrimRangeStereo({
        left: signal(),
        right: signal(),
        sampleRate: section.sample_rate,
        ...o,
      } as never),
  };
  for (const entry of section.entries) {
    for (const testCase of section.cases) {
      const pending = testCase.pending?.[SURFACE];
      const test = pending ? it.fails : it;
      test(`${entry} ${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
        const key = camelKey(testCase.key);
        const call = () =>
          calls[entry]({ mode: section.mode[testCase.key], [key]: corpusValue(testCase.value) });
        if (testCase.accepted) {
          expect(call).not.toThrow();
          return;
        }
        const refusal = `${camelKey(entry)}: ${key} ${testCase.predicate}`;
        expect(call).toThrow(
          testCase.refused === 'range' ? new RangeError(refusal) : new TypeError(refusal),
        );
      });
    }
  }
});

type RoomMorphSection = { fn: string; room: Record<string, unknown>; cases: PendingCase[] };

describe('the room morph receiver spacing answers the shared corpus', () => {
  const section = (corpus as unknown as { room_morph_stereo: RoomMorphSection }).room_morph_stereo;
  const room = camelRecord(section.room);
  const impulse = () => {
    const x = new Float32Array(2000);
    x[0] = 1;
    return x;
  };
  for (const testCase of section.cases) {
    const pending = testCase.pending?.[SURFACE];
    const test = pending ? it.fails : it;
    test(`${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
      const key = camelKey(testCase.key);
      const call = () =>
        roomMorphStereo({
          left: impulse(),
          right: impulse(),
          ...room,
          [key]: corpusValue(testCase.value),
        } as never);
      if (testCase.accepted) {
        expect(call).not.toThrow();
        return;
      }
      const refusal = `${camelKey(section.fn)}: ${key} ${testCase.predicate}`;
      expect(call).toThrow(
        testCase.refused === 'range' ? new RangeError(refusal) : new TypeError(refusal),
      );
    });
  }
});

type SceneInsertCase = {
  id: string;
  param: string;
  value: unknown;
  accepted?: boolean;
  code_name?: string;
  message?: string;
  pending?: Record<string, string>;
};
type SceneInsertSection = {
  fn: string;
  options: Record<string, unknown>;
  scene: { strips: { inserts: { params: Record<string, unknown> }[] }[] };
  cases: SceneInsertCase[];
};

describe('a scene insert refusal reaches the caller with its document path untouched', () => {
  const section = (corpus as unknown as { scene_insert_refusal: SceneInsertSection })
    .scene_insert_refusal;
  const options = camelRecord(section.options);
  const fn = 'Mixer.fromJson';
  for (const testCase of section.cases) {
    const pending = testCase.pending?.[SURFACE];
    const test = pending ? it.fails : it;
    test(`${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
      const scene = structuredClone(section.scene);
      scene.strips[0].inserts[0].params[testCase.param] = testCase.value;
      const call = () => Mixer.fromJson(JSON.stringify(scene), options as never);
      if (testCase.accepted) {
        expect(call).not.toThrow();
        return;
      }
      let caught: unknown;
      try {
        call();
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(SonareError);
      expect((caught as SonareError).codeName).toBe(testCase.code_name);
      expect((caught as Error).message).toBe(`${fn}: ${testCase.message}`);
    });
  }
});

type InsertUnknownKeyCase = {
  id: string;
  processor: string;
  params: Record<string, number>;
  message: string;
  pending?: Record<string, string>;
};
type InsertUnknownKeySection = {
  entries: string[];
  sample_rate: number;
  samples: number;
  error: Record<string, string>;
  cases: InsertUnknownKeyCase[];
};

describe('an unknown insert key is refused with one class and text on every param-bag entry', () => {
  const section = (corpus as unknown as { insert_unknown_key: InsertUnknownKeySection })
    .insert_unknown_key;
  const sampleRate = section.sample_rate;
  const samples = new Float32Array(section.samples);
  const calls: Record<string, (processor: string, params: Record<string, number>) => unknown> = {
    mastering_insert_timing: (insert, params) =>
      masteringInsertTiming({ insert, params, sampleRate } as never),
    mastering_apply_processor: (processor, params) =>
      masteringApplyProcessor({ processor, samples, sampleRate, params } as never),
    mastering_apply_processor_stereo: (processor, params) =>
      masteringApplyProcessorStereo({
        processor,
        left: samples,
        right: samples,
        sampleRate,
        params,
      } as never),
  };
  for (const entry of section.entries) {
    for (const testCase of section.cases) {
      const pending = testCase.pending?.[SURFACE];
      const test = pending ? it.fails : it;
      test(`${entry} ${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
        let caught: unknown;
        try {
          calls[entry](testCase.processor, testCase.params);
        } catch (error) {
          caught = error;
        }
        expect((caught as Error | undefined)?.constructor.name).toBe(section.error[SURFACE]);
        expect((caught as Error).message).toBe(`${camelKey(entry)}: ${testCase.message}`);
      });
    }
  }
});

type CqtFamilyCase = PendingCase & {
  only?: string[];
  options?: Record<string, unknown>;
  message?: string;
  no_internal_text?: boolean;
};
/** A corpus text as this surface spells it: every snake_case identifier in camel case. */
const camelText = (text: string): string => text.replace(/\b[a-z]+(?:_[a-z0-9]+)+\b/g, camelKey);
type CqtFamilySection = {
  entries: string[];
  sample_rate: number;
  samples: number;
  fill: number;
  cases: CqtFamilyCase[];
};

describe('the constant-Q family answers the shared corpus with one text on every entry', () => {
  const section = (corpus as unknown as { cqt_family: CqtFamilySection }).cqt_family;
  const samples = new Float32Array(section.samples).fill(section.fill);
  const sampleRate = section.sample_rate;
  const calls: Record<string, (options: Record<string, unknown>) => unknown> = {
    cqt: (o) => cqt({ samples, sampleRate, ...o } as never),
    vqt: (o) => vqt({ samples, sampleRate, ...o } as never),
    pseudo_cqt: (o) => pseudoCqt({ samples, sampleRate, ...o } as never),
    hybrid_cqt: (o) => hybridCqt({ samples, sampleRate, ...o } as never),
  };
  for (const entry of section.entries) {
    for (const testCase of section.cases.filter((c) => !c.only || c.only.includes(entry))) {
      const pending = testCase.pending?.[SURFACE];
      const test = pending ? it.fails : it;
      test(`${entry} ${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
        const options = testCase.options
          ? Object.fromEntries(
              Object.entries(testCase.options).map(([k, v]) => [camelKey(k), corpusValue(v)]),
            )
          : { [camelKey(testCase.key)]: corpusValue(testCase.value) };
        const call = () => calls[entry](options);
        if (testCase.accepted) {
          expect(call).not.toThrow();
          return;
        }
        if (testCase.no_internal_text) {
          let caught: unknown;
          try {
            call();
          } catch (error) {
            caught = error;
          }
          if (caught === undefined) {
            return;
          }
          expect((caught as Error).message).not.toMatch(/std::|bad_alloc/);
          if (caught instanceof SonareError) {
            expect(caught.codeName).toBe('OutOfMemory');
          }
          return;
        }
        const refusal = testCase.message
          ? `${camelKey(entry)}: ${camelText(testCase.message)}`
          : `${camelKey(entry)}: ${camelKey(testCase.key)} ${testCase.predicate}`;
        expect(call).toThrow(
          testCase.refused === 'range' ? new RangeError(refusal) : new TypeError(refusal),
        );
      });
    }
  }
});

type EngineZeroCase = ReaderSplitCase & { entry: string; pending?: Record<string, string> };
/** `roots` holds the path to an entry's nested record; an object argument's keys are bare. */
type EngineZeroSection = { roots: Record<string, string | undefined>; cases: EngineZeroCase[] };

/** Each engine entry called with one field of its request replaced. */
const ENGINE_ZERO_CALLS: Record<
  string,
  (engine: RealtimeEngine, key: string, value: unknown) => void
> = {
  set_metronome: (engine, key, value) => engine.setMetronome({ enabled: true, [key]: value }),
  set_clips: (engine, key, value) =>
    engine.setClips([
      { id: 1, channels: [new Float32Array(256)], startPpq: 0, [key]: value } as never,
    ]),
  set_midi_clips: (engine, key, value) =>
    engine.setMidiClips([
      { events: [], lengthSamples: 100, loop: true, loopLengthSamples: 50, [key]: value } as never,
    ]),
  set_graph: (engine, key, value) =>
    engine.setGraph({
      nodes: [{ id: 'in', [key]: value } as never, { id: 'out' }],
      connections: [{ sourceNode: 'in', sourcePort: 0, destNode: 'out', destPort: 0 }],
      inputNode: 'in',
      outputNode: 'out',
    }),
  bounce_offline: (engine, key, value) =>
    engine.bounceOffline({ totalFrames: 128, numChannels: 2, dither: 'tpdf', [key]: value }),
  freeze_offline: (engine, key, value) =>
    engine.freezeOffline({ totalFrames: 128, numChannels: 2, [key]: value }),
};

describe('an engine field whose native 0 is a sentinel refuses 0 out of domain under the caller path', () => {
  const section = (corpus as unknown as { engine_zero_sentinel: EngineZeroSection })
    .engine_zero_sentinel;
  for (const testCase of section.cases) {
    const pending = testCase.pending?.[SURFACE];
    const test = pending ? it.fails : it;
    test(`${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        const key = camelKey(testCase.key);
        const call = () =>
          ENGINE_ZERO_CALLS[testCase.entry](engine, key, corpusValue(testCase.value));
        if (testCase.accepted) {
          expect(call).not.toThrow();
          return;
        }
        const root = section.roots[testCase.entry];
        const subject = root === undefined ? key : `${root}.${key}`;
        expect(call).toThrow(
          new RangeError(
            `RealtimeEngine.${camelKey(testCase.entry)}: ${subject} ${testCase.predicate}`,
          ),
        );
      } finally {
        engine.destroy();
      }
    });
  }
});

type InvertedBandCase = {
  id: string;
  entry: string;
  min_key: string;
  max_key: string;
  min?: number;
  max: number;
  pending?: Record<string, string>;
};
type InvertedBandSection = { sample_rate: number; predicate: string; cases: InvertedBandCase[] };

/** The entries whose frequency or tempo band the core orders, each taking a request object. */
const INVERTED_BAND_CALLS: Record<string, (request: Record<string, unknown>) => unknown> = {
  piptrack: (request) => piptrack(request as never),
  pitch_yin: (request) => pitchYin(request as never),
  pitch_pyin: (request) => pitchPyin(request as never),
  analyze_melody: (request) => analyzeMelody(request as never),
  detect_bpm: (request) => detectBpm(request as never),
  detect_beats: (request) => detectBeats(request as never),
  transcribe: (request) => transcribe(request as never),
};

describe('an inverted band is refused once, in the core text, under the request keys', () => {
  const section = (corpus as unknown as { inverted_band: InvertedBandSection }).inverted_band;
  const sampleRate = section.sample_rate;
  const samples = new Float32Array(sampleRate).map(
    (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
  );
  for (const testCase of section.cases) {
    const pending = testCase.pending?.[SURFACE];
    const test = pending ? it.fails : it;
    test(`${testCase.id}${pending ? ` (pending: ${pending})` : ''}`, () => {
      const minKey = camelKey(testCase.min_key);
      const maxKey = camelKey(testCase.max_key);
      const request: Record<string, unknown> = { samples, sampleRate, [maxKey]: testCase.max };
      if (testCase.min !== undefined) {
        request[minKey] = testCase.min;
      }
      const text = section.predicate.replace('{max}', maxKey).replace('{min}', minKey);
      expect(() => INVERTED_BAND_CALLS[testCase.entry](request)).toThrow(
        new RangeError(`${camelKey(testCase.entry)}: ${text}`),
      );
    });
  }
});

type MixerBusRoleSection = {
  fn: string;
  message: string;
  error: Record<string, string>;
  cases: { id: string; role: string | null; stored?: string }[];
};

describe('the mixer bus role answers the shared corpus', () => {
  const section = (corpus as unknown as { mixer_bus_role: MixerBusRoleSection }).mixer_bus_role;
  it.each(section.cases)('$id', ({ role, stored }) => {
    const mixer = new Mixer({ sampleRate: 48_000 });
    try {
      const call = () => mixer.addBus('corpus', (role ?? undefined) as never);
      if (stored !== undefined) {
        call();
        const buses = (JSON.parse(mixer.toJson()) as { buses: { id: string; role: string }[] })
          .buses;
        expect(buses.find((bus) => bus.id === 'corpus')?.role).toBe(stored);
        return;
      }
      expect(section.error[SURFACE]).toBe('RangeError');
      expect(call).toThrow(new RangeError(`${camelKey(section.fn)}: ${section.message}`));
    } finally {
      mixer[Symbol.dispose]();
    }
  });
});
