import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { beforeAll, describe, expect, it } from 'vitest';
import type { Matrix2D, MixStereoStrip } from '../dist/index.js';
import {
  alignTakeToReference,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeMelody,
  analyzeRhythm,
  analyzeSections,
  analyzeTimbre,
  assignNoteTargets,
  autoTune,
  bassChroma,
  chordSymbolFromAnalysis,
  chroma,
  chromaCens,
  cqt,
  cyclicTempogram,
  decodeChannels,
  decompose,
  decomposeNotePitch,
  decomposeStems,
  decomposeStemsLinked,
  detectAcoustic,
  detectBeats,
  detectBoundaries,
  detectBpm,
  detectKeyCandidates,
  encodeAudio,
  estimateMeter,
  estimateRoom,
  extractNotes,
  extractPercussiveEvents,
  fourierTempogram,
  frameSignal,
  gm2DrumName,
  gm2DrumSetName,
  gm2InstrumentName,
  gmDrumName,
  gmFamilyFirstProgram,
  gmFamilyName,
  gmInstrumentName,
  hpss,
  hybridCqt,
  init,
  lufs,
  lufsMultichannel,
  lufsSeriesMultichannel,
  Mixer,
  masterAudio,
  masterAudioStereo,
  masteringAbMatchLoudness,
  masteringAbMatchLoudnessStereo,
  masteringAmpPresetCatalog,
  masteringAnalyzePair,
  masteringAnalyzeStereo,
  masteringApplyPairProcessor,
  masteringApplyPairProcessorStereo,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringChainConfigFromJson,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringInsertParamInfo,
  masteringInsertTiming,
  masteringNormalizeLoudness,
  masteringPresetParams,
  masteringRepairAnalyze,
  masteringRepairApply,
  masteringRepairDeclick,
  masteringRepairDeclickStereo,
  masteringRepairDeclip,
  masteringRepairDeclipStereo,
  masteringRepairDecrackle,
  masteringRepairDecrackleStereo,
  masteringRepairDehum,
  masteringRepairDehumStereo,
  masteringRepairDenoiseClassical,
  masteringRepairDenoiseClassicalLinked,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassical,
  masteringRepairDereverbClassicalLinked,
  masteringRepairDereverbClassicalStereo,
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairDetectReverb,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
  melDelta,
  melFilterbank,
  melFrequencies,
  melSpectrogram,
  melToStft,
  mergeNotes,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPhaseScope,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringVectorscope,
  mfcc,
  mfccToMel,
  midi2AssignableController,
  midi2AssignablePerNoteController,
  midi2Cc,
  midi2ChannelPressure,
  midi2NoteOff,
  midi2NoteOn,
  midi2PerNoteManagement,
  midi2PerNotePitchBend,
  midi2PitchBend,
  midi2PolyPressure,
  midi2Program,
  midi2RegisteredController,
  midi2RegisteredPerNoteController,
  midi2RelativeAssignableController,
  midi2RelativeRegisteredController,
  midiBankProgram,
  midiCc,
  midiCcLearn,
  midiCcName,
  midiCcToBreakpoint,
  midiChannelPressure,
  midiNoteOff,
  midiNoteOn,
  midiParamToCc,
  midiPitchBend,
  midiPolyPressure,
  midiProgram,
  midiRouteEvents,
  mixingAssistantSuggest,
  mixingScenePreset,
  mixStereo,
  nnFilter,
  nnlsChroma,
  normalizeStereo,
  noteSegments,
  noteTargetsFromSmf,
  onsetStrengthMulti,
  pcen,
  perNoteControllerName,
  piptrack,
  pitchPyin,
  pitchYin,
  polyFeatures,
  pseudoCqt,
  RealtimeEngine,
  RealtimeVoiceChanger,
  readAudioTags,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerValidatePresetJson,
  reassignedSpectrogram,
  renderPlayback,
  roomGeometryFromEstimate,
  roomMorph,
  roomMorphStereo,
  SonareError,
  StreamingRetune,
  segmentAgglomerative,
  segmentCrossSimilarity,
  segmentLagToRecurrence,
  segmentPathEnhance,
  segmentRecurrenceMatrix,
  segmentRecurrenceToLag,
  segmentSubsegment,
  spectralContrast,
  splitNote,
  splitSilenceCommonWithReport,
  stft,
  streamingLoudnessGain,
  streamingLoudnessGainStereo,
  synthEngineParamInfo,
  synthEnumTables,
  synthesizeRir,
  synthPatchParamInfo,
  synthPresetPatch,
  tempogram,
  tonnetz,
  transcribe,
  trimSilence,
  vqt,
  waveformPeakPyramid,
  waveformPeaks,
} from '../dist/index.js';
import { expectArgumentRefusal } from './_helpers';

type CorpusMarker = { id: number | 'uint32_max'; ppq: number | 'nan' | 'inf'; name: string };
type CorpusCase = { id: string; accepted: boolean; markers: CorpusMarker[] };
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

type StripOptions = Partial<Omit<MixStereoStrip, 'left' | 'right'>>;

const mixOptions = (testCase: MixOptionCase): StripOptions => {
  const value = mixOptionValue(testCase.value);
  switch (testCase.field) {
    case 'input_trim_db':
      return { inputTrimDb: value as number };
    case 'fader_db':
      return { faderDb: value as number };
    case 'pan':
      return { pan: value as number };
    case 'pan_mode':
      return { pan: 0, panMode: value as StripOptions['panMode'] };
    case 'width':
      return { width: value as number };
  }
};

describe('shared public-input conformance corpus (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('rejects MIDI CC learn movement outside 0-127', () => {
    const events = [60, 70].map((value) =>
      midiCc({ ppq: 0, group: 0, channel: 0, controller: 74, value }),
    );
    for (const minMovement of [-1, 128, 255, 256]) {
      let caught: unknown;
      try {
        midiCcLearn({ events, paramId: 3, minMovement });
      } catch (error) {
        caught = error;
      }
      expectArgumentRefusal(caught);
    }
    expect(midiCcLearn({ events, paramId: 3, minMovement: 10 })).not.toBeNull();
    expect(midiCcLearn({ events, paramId: 3, minMovement: 11 })).toBeNull();
    expect(midiCcLearn({ events, paramId: 3, minMovement: 127 })).toBeNull();
  });

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
        mixStereo({ strips: [{ left: channel, right: channel, ...options }], sampleRate: 48000 }),
      ).toThrow();
      // The control: the same strip without the refused field mixes, so the refusal is the field's.
      expect(() =>
        mixStereo({ strips: [{ left: channel, right: channel }], sampleRate: 48000 }),
      ).not.toThrow();
    });
  }

  it('drives both outcomes from the MIDI CC binding corpus', () => {
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
          expect(engine.midiCcBindings().length).toBe(1);
        } else {
          expect(() => engine.bindMidiCc(binding() as never)).toThrow(RangeError);
          expect(engine.midiCcBindings().length).toBe(0);
        }
      } finally {
        engine.destroy();
      }
    });

    it(`keeps midiCcToBreakpoint byte fields conformant: ${testCase.id}`, () => {
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
          expect(engine.midiCcBindings().length).toBe(0);
          engine.bindMidiCc(binding(accepted) as never);
          expect(engine.midiCcBindings().length).toBe(1);
        } finally {
          engine.destroy();
        }
      });
    }
  }
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
  beforeAll(async () => {
    await init();
  });

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

const SURFACE = 'wasm';

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
  beforeAll(async () => {
    await init();
  });

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

type RoomMorphCase = PendingCase & { room?: Record<string, unknown>; relation?: boolean };
type RoomMorphSection = { fn: string; room: Record<string, unknown>; cases: RoomMorphCase[] };

describe('the room morph receiver spacing answers the shared corpus', () => {
  const section = (corpus as unknown as { room_morph_stereo: RoomMorphSection }).room_morph_stereo;
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
      // A case's own room overrides the section's, so one value can fit one room and not another.
      const room = camelRecord({ ...section.room, ...(testCase.room ?? {}) });
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
      if (testCase.relation) {
        // A relation names its other subjects after "for"; only the prefix is shared until they settle.
        const escaped = refusal.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        expect(call).toThrow(new RegExp(`^${escaped} for .+$`));
        expect(call).toThrow(testCase.refused === 'range' ? RangeError : TypeError);
        return;
      }
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
  beforeAll(async () => {
    await init();
  });
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
  beforeAll(async () => {
    await init();
  });
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
  beforeAll(async () => {
    await init();
  });
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

type MelFamilyCase = {
  id: string;
  args: Record<string, unknown>;
  only?: string[];
  accepted?: boolean;
  expect?: Record<string, { shape?: number[]; values?: number[] }>;
  refused?: 'range' | 'type';
  predicate?: string;
};
type MelFamilySection = {
  entries: string[];
  base: Record<string, Record<string, unknown>>;
  cases: MelFamilyCase[];
};

describe('the mel filterbank and mel frequencies answer the shared corpus', () => {
  beforeAll(async () => {
    await init();
  });
  const section = (corpus as unknown as { mel_family: MelFamilySection }).mel_family;
  const calls: Record<string, (request: Record<string, unknown>) => Matrix2D | Float32Array> = {
    mel_filterbank: (r) => melFilterbank(r as never),
    mel_frequencies: (r) => melFrequencies(r as never),
  };
  for (const entry of section.entries) {
    for (const testCase of section.cases.filter((c) => !c.only || c.only.includes(entry))) {
      it(`${entry} ${testCase.id}`, () => {
        const request: Record<string, unknown> = {};
        for (const [k, v] of Object.entries({ ...section.base[entry], ...testCase.args })) {
          request[camelKey(k)] = corpusValue(v);
        }
        const call = () => calls[entry](request);
        if (testCase.accepted) {
          const out = call();
          const want = testCase.expect?.[entry];
          if (want?.shape) {
            const matrix = out as Matrix2D;
            expect([matrix.rows, matrix.cols]).toEqual(want.shape);
          }
          if (want?.values) {
            expect(Array.from(out as Float32Array)).toEqual(want.values);
          }
          return;
        }
        const refusal = `${camelKey(entry)}: ${(testCase.predicate ?? '').replace(
          /\{([a-z_]+)\}/g,
          (_, key: string) => camelKey(key),
        )}`;
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
  beforeAll(async () => {
    await init();
  });
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
  beforeAll(async () => {
    await init();
  });
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
  beforeAll(async () => {
    await init();
  });

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

type DeclipLinkSection = {
  sample_rate: number;
  samples: number;
  error: Record<string, string>;
  cases: { id: string; entry: string; link_channels: unknown; accepted: boolean }[];
};

describe('the declip linkChannels flag answers the shared corpus', () => {
  beforeAll(async () => {
    await init();
  });

  const section = (corpus as unknown as { declip_link_channels: DeclipLinkSection })
    .declip_link_channels;
  const signal = Float32Array.from(
    { length: section.samples },
    (_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / section.sample_rate),
  );
  it.each(section.cases)('$id', ({ entry, link_channels, accepted }) => {
    const call = () =>
      entry === 'mastering_repair_declip'
        ? masteringRepairDeclip({
            samples: signal,
            sampleRate: section.sample_rate,
            linkChannels: link_channels,
          } as never)
        : masteringRepairDeclipStereo({
            left: signal,
            right: signal,
            sampleRate: section.sample_rate,
            linkChannels: link_channels,
          } as never);
    if (accepted) {
      expect(call).not.toThrow();
      return;
    }
    expect(section.error[SURFACE]).toBe('TypeError');
    expect(call).toThrow(TypeError);
  });
});

type PublicResultValue =
  | null
  | boolean
  | number
  | string
  | PublicResultValue[]
  | { [key: string]: PublicResultValue };

type PublicResultFixture =
  | 'historical_excerpt'
  | 'impulsive'
  | 'silence_excerpt'
  | 'tone'
  | 'silence'
  | 'clipped'
  | 'gapped'
  | 'empty'
  | 'historical_left'
  | 'historical_reference_right'
  | 'historical_rir'
  | 'historical_frame'
  | 'meter_accents'
  | 'decay_envelope'
  | 'lone_impulse'
  | 'profile_silence'
  | 'midi'
  | 'tagged_wav';
type PublicResultEntry =
  | 'sonare_master_audio'
  | 'sonare_master_audio_stereo'
  | 'sonare_mastering_assistant_suggest'
  | 'sonare_mastering_assistant_suggest_stereo'
  | 'sonare_assign_note_targets'
  | 'sonare_extract_notes'
  | 'sonare_merge_notes'
  | 'sonare_split_note'
  | 'sonare_extract_percussive_events'
  | 'sonare_mastering_repair_dereverb_classical_linked'
  | 'sonare_mastering_repair_dereverb_classical_stereo'
  | 'sonare_mastering_repair_dereverb_classical'
  | 'sonare_mastering_repair_denoise_classical_linked'
  | 'sonare_mastering_repair_denoise_classical_stereo'
  | 'sonare_mastering_repair_denoise_classical'
  | 'sonare_mastering_repair_dehum_stereo'
  | 'sonare_mastering_repair_dehum'
  | 'sonare_mastering_repair_apply'
  | 'sonare_piptrack'
  | 'sonare_reassigned_spectrogram'
  | 'sonare_mastering_repair_decrackle'
  | 'sonare_mastering_repair_decrackle_stereo'
  | 'sonare_mastering_repair_declip'
  | 'sonare_mastering_repair_declip_stereo'
  | 'sonare_mastering_repair_declick'
  | 'sonare_mastering_repair_declick_stereo'
  | 'sonare_realtime_voice_changer_preset_config'
  | 'sonare_realtime_voice_changer_preset_json'
  | 'sonare_realtime_voice_changer_validate_preset_json'
  | 'sonare_note_segments'
  | 'sonare_transcribe'
  | 'sonare_nnls_chroma'
  | 'sonare_onset_strength_multi'
  | 'sonare_spectral_contrast'
  | 'sonare_poly_features'
  | 'sonare_cyclic_tempogram'
  | 'sonare_fourier_tempogram'
  | 'sonare_tempogram'
  | 'sonare_nn_filter'
  | 'sonare_tonnetz'
  | 'sonare_mel_delta'
  | 'sonare_pcen'
  | 'sonare_mel_filterbank'
  | 'sonare_pitch_pyin'
  | 'sonare_pitch_yin'
  | 'sonare_detect_key_candidates'
  | 'sonare_vqt'
  | 'sonare_hybrid_cqt'
  | 'sonare_pseudo_cqt'
  | 'sonare_cqt'
  | 'sonare_chroma_cens'
  | 'sonare_bass_chroma'
  | 'sonare_decompose_stems_linked'
  | 'sonare_decompose_stems'
  | 'sonare_decompose'
  | 'sonare_hpss'
  | 'sonare_auto_tune'
  | 'sonare_note_targets_from_smf'
  | 'sonare_decompose_note_pitch'
  | 'sonare_lufs_series_interleaved'
  | 'sonare_room_morph_stereo'
  | 'sonare_room_morph'
  | 'sonare_synthesize_rir'
  | 'sonare_room_geometry_from_estimate'
  | 'sonare_align_take_to_reference'
  | 'sonare_split_silence_common'
  | 'sonare_midi_param_to_cc'
  | 'sonare_midi_route_events'
  | 'sonare_midi_cc_to_breakpoint'
  | 'sonare_midi_cc_learn'
  | 'sonare_midi_bank_program'
  | 'sonare_analyze_bpm'
  | 'sonare_analyze_dynamics'
  | 'sonare_analyze_impulse_response'
  | 'sonare_analyze_melody'
  | 'sonare_analyze_rhythm'
  | 'sonare_analyze_sections'
  | 'sonare_analyze_timbre'
  | 'sonare_chroma'
  | 'sonare_detect_acoustic'
  | 'sonare_detect_boundaries'
  | 'sonare_estimate_meter_json'
  | 'sonare_estimate_room'
  | 'sonare_frame_signal'
  | 'sonare_lufs'
  | 'sonare_lufs_interleaved'
  | 'sonare_mastering_repair_analyze'
  | 'sonare_mixer_process_stereo'
  | 'sonare_playback_render_interleaved'
  | 'sonare_mastering_streaming_preview'
  | 'sonare_mastering_streaming_preview_stereo'
  | 'sonare_mastering_repair_trim_silence'
  | 'sonare_mastering_repair_trim_silence_stereo'
  | 'sonare_normalize_stereo'
  | 'sonare_normalize_rms_stereo'
  | 'sonare_mastering_process'
  | 'sonare_mastering_apply_processor'
  | 'sonare_mastering_apply_processor_stereo'
  | 'sonare_mastering_apply_pair_processor'
  | 'sonare_mastering_apply_pair_processor_stereo'
  | 'sonare_mastering_ab_match_loudness'
  | 'sonare_mastering_ab_match_loudness_stereo'
  | 'sonare_mastering_analyze_pair'
  | 'sonare_mastering_analyze_pair_stereo'
  | 'sonare_mastering_analyze_stereo'
  | 'sonare_mastering_audio_profile'
  | 'sonare_mastering_audio_profile_stereo'
  | 'sonare_mastering_dynamics_compressor'
  | 'sonare_mastering_dynamics_gate'
  | 'sonare_mastering_dynamics_transient_shaper'
  | 'sonare_mastering_insert_timing'
  | 'sonare_mastering_repair_detect_clicks'
  | 'sonare_mastering_repair_detect_clipping'
  | 'sonare_mastering_repair_detect_crackle'
  | 'sonare_mastering_repair_detect_hum'
  | 'sonare_mastering_repair_detect_noise_floor'
  | 'sonare_mastering_repair_detect_reverb'
  | 'sonare_mastering_repair_detect_trim_range'
  | 'sonare_mastering_repair_detect_trim_range_stereo'
  | 'sonare_mel_spectrogram'
  | 'sonare_metering_detect_clipping'
  | 'sonare_metering_dynamic_range'
  | 'sonare_metering_phase_scope'
  | 'sonare_metering_spectrum'
  | 'sonare_metering_spectrum_frame'
  | 'sonare_metering_vectorscope'
  | 'sonare_mfcc'
  | 'sonare_mixing_assistant_suggest'
  | 'sonare_stft'
  | 'sonare_mel_to_stft'
  | 'sonare_mfcc_to_mel'
  | 'sonare_segment_cross_similarity'
  | 'sonare_segment_recurrence_matrix'
  | 'sonare_segment_recurrence_to_lag'
  | 'sonare_segment_lag_to_recurrence'
  | 'sonare_segment_subsegment'
  | 'sonare_segment_agglomerative'
  | 'sonare_segment_path_enhance'
  | 'sonare_streaming_loudness_gain'
  | 'sonare_streaming_loudness_gain_stereo'
  | 'sonare_trim_silence'
  | 'sonare_waveform_peaks'
  | 'sonare_midi2_assignable_controller'
  | 'sonare_midi2_assignable_per_note_controller'
  | 'sonare_midi2_channel_pressure'
  | 'sonare_midi2_cc'
  | 'sonare_midi2_note_off'
  | 'sonare_midi2_note_on'
  | 'sonare_midi2_per_note_management'
  | 'sonare_midi2_per_note_pitch_bend'
  | 'sonare_midi2_pitch_bend'
  | 'sonare_midi2_poly_pressure'
  | 'sonare_midi2_program'
  | 'sonare_midi2_registered_controller'
  | 'sonare_midi2_registered_per_note_controller'
  | 'sonare_midi2_relative_assignable_controller'
  | 'sonare_midi2_relative_registered_controller'
  | 'sonare_midi_channel_pressure'
  | 'sonare_midi_cc'
  | 'sonare_midi_note_off'
  | 'sonare_midi_note_on'
  | 'sonare_midi_pitch_bend'
  | 'sonare_midi_poly_pressure'
  | 'sonare_midi_program'
  | 'sonare_mastering_amp_preset_catalog'
  | 'sonare_mastering_preset_params_json'
  | 'sonare_mastering_chain_config_from_json'
  | 'sonare_mastering_insert_param_info'
  | 'sonare_mixing_scene_preset_json'
  | 'sonare_synth_engine_param_info'
  | 'sonare_synth_enum_names'
  | 'sonare_synth_patch_param_info'
  | 'sonare_synth_preset_patch'
  | 'sonare_waveform_peak_pyramid'
  | 'sonare_decode_channels'
  | 'sonare_read_audio_tags'
  | 'sonare_chord_symbol_from_analysis';

type PublicResultCase = {
  id: string;
  entry: PublicResultEntry;
  fixture: PublicResultFixture;
  result: PublicResultValue;
  tolerances?: Record<string, number>;
  surfaces?: string[];
  midi?: Record<string, number>;
  input?: SegmentCorpusInput;
};

type PublicResultRefusal = {
  id: string;
  entry: PublicResultEntry;
  fixture: PublicResultFixture;
  code: number | null;
  js_class: 'RangeError' | 'TypeError' | 'SonareError';
  python_class: string;
  midi?: Record<string, number>;
};

type PublicResultSection = {
  sample_rate: number;
  frames: number;
  relative_tolerance: number;
  cases: PublicResultCase[];
  refusals: PublicResultRefusal[];
};

type PublicResultSignals = Record<
  | 'historical_left'
  | 'historical_right'
  | 'historical_reference'
  | 'historical_reference_right'
  | 'historical_rir'
  | 'historical_frame'
  | 'meter_accents'
  | 'decay_envelope'
  | 'lone_impulse',
  number[]
>;

const publicResultSignals = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/public_result_signals.json', import.meta.url),
    'utf8',
  ),
) as PublicResultSignals;

const publicResultSection = (
  JSON.parse(
    readFileSync(
      new URL('../../../tests/conformance/public_result_corpus.json', import.meta.url),
      'utf8',
    ),
  ) as { one_shot_results: PublicResultSection }
).one_shot_results;

function corpusFeatureMatrix(rows: number, cols: number) {
  return {
    rows,
    cols,
    data: Float32Array.of(3, 2, 1, 2, 3, 2, 1, 2, 4, 3, 2, 1, 2, 4, 3, 1, 2, 4, 4, 1, 3, 2, 1, 3),
  };
}

function corpusCcEvents() {
  return [
    midiCc({ ppq: 0.5, group: 1, channel: 2, controller: 74, value: 17 }),
    midiCc({ ppq: 1, group: 1, channel: 2, controller: 74, value: 101 }),
  ] as const;
}

type SegmentCorpusMatrix = { rows: number; cols: number; data: number[] };
type SegmentCorpusInput = {
  x: SegmentCorpusMatrix;
  y: SegmentCorpusMatrix;
  data: SegmentCorpusMatrix;
  recurrence: SegmentCorpusMatrix;
  lag: SegmentCorpusMatrix;
  mel: SegmentCorpusMatrix;
  coefficients: SegmentCorpusMatrix;
  boundaries: number[];
  k: number;
  win: number;
};
function requiredSegmentInput(input: SegmentCorpusInput | undefined): SegmentCorpusInput {
  if (input === undefined) {
    throw new Error('matrix result case needs input');
  }
  return input;
}

function segmentMatrix(m: SegmentCorpusMatrix): Matrix2D {
  return { rows: m.rows, cols: m.cols, data: Float32Array.from(m.data) };
}
function corpusNoteTrack(sampleRate: number, paired: boolean) {
  const f0Hz = new Float32Array(32).fill(440);
  if (paired) {
    f0Hz.fill(660, 16);
  }
  return {
    f0Hz,
    voiced: new Int32Array(32).fill(1),
    frameRate: sampleRate / 256,
  };
}

function corpusNoteSet(samples: Float32Array, sampleRate: number, paired: boolean) {
  const track = corpusNoteTrack(sampleRate, paired);
  const extracted = extractNotes({ samples, sampleRate, ...track });
  return {
    ...track,
    notes: extracted.map((note, index) => ({
      frameStart: note.frameStart,
      frameEnd: note.frameEnd,
      edit: index === 0 ? { amplitudeEnvelope: Float32Array.of(0.25, 0.5, 1) } : {},
    })),
  };
}

const publicResultCalls: Record<
  PublicResultEntry,
  (
    samples: Float32Array,
    sampleRate: number,
    fixture: PublicResultFixture,
    midi?: Record<string, number>,
    input?: SegmentCorpusInput,
  ) => unknown
> = {
  sonare_midi2_assignable_controller: (_samples, _sampleRate, _fixture, midi) =>
    midi2AssignableController({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      bank: midiNumber(midi, 'bank'),
      index: midiNumber(midi, 'index'),
      value32: midiNumber(midi, 'value32'),
    }),
  sonare_midi2_assignable_per_note_controller: (_samples, _sampleRate, _fixture, midi) =>
    midi2AssignablePerNoteController({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      index: midiNumber(midi, 'index'),
      value32: midiNumber(midi, 'value32'),
    }),
  sonare_midi2_channel_pressure: (_samples, _sampleRate, _fixture, midi) =>
    midi2ChannelPressure({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      pressure32: midiNumber(midi, 'pressure32'),
    }),
  sonare_midi2_cc: (_samples, _sampleRate, _fixture, midi) =>
    midi2Cc({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      controller: midiNumber(midi, 'controller'),
      value32: midiNumber(midi, 'value32'),
    }),
  sonare_midi2_note_off: (_samples, _sampleRate, _fixture, midi) =>
    midi2NoteOff({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
    }),
  sonare_midi2_note_on: (_samples, _sampleRate, _fixture, midi) =>
    midi2NoteOn({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      velocity16: midiNumber(midi, 'velocity16'),
    }),
  sonare_midi2_per_note_management: (_samples, _sampleRate, _fixture, midi) =>
    midi2PerNoteManagement({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
    }),
  sonare_midi2_per_note_pitch_bend: (_samples, _sampleRate, _fixture, midi) =>
    midi2PerNotePitchBend({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      bend32: midiNumber(midi, 'bend32'),
    }),
  sonare_midi2_pitch_bend: (_samples, _sampleRate, _fixture, midi) =>
    midi2PitchBend({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      bend32: midiNumber(midi, 'bend32'),
    }),
  sonare_midi2_poly_pressure: (_samples, _sampleRate, _fixture, midi) =>
    midi2PolyPressure({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      pressure32: midiNumber(midi, 'pressure32'),
    }),
  sonare_midi2_program: (_samples, _sampleRate, _fixture, midi) =>
    midi2Program({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      program: midiNumber(midi, 'program'),
    }),
  sonare_midi2_registered_controller: (_samples, _sampleRate, _fixture, midi) =>
    midi2RegisteredController({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      bank: midiNumber(midi, 'bank'),
      index: midiNumber(midi, 'index'),
      value32: midiNumber(midi, 'value32'),
    }),
  sonare_midi2_registered_per_note_controller: (_samples, _sampleRate, _fixture, midi) =>
    midi2RegisteredPerNoteController({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      index: midiNumber(midi, 'index'),
      value32: midiNumber(midi, 'value32'),
    }),
  sonare_midi2_relative_assignable_controller: (_samples, _sampleRate, _fixture, midi) =>
    midi2RelativeAssignableController({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      bank: midiNumber(midi, 'bank'),
      index: midiNumber(midi, 'index'),
      delta32: midiNumber(midi, 'delta32'),
    }),
  sonare_midi2_relative_registered_controller: (_samples, _sampleRate, _fixture, midi) =>
    midi2RelativeRegisteredController({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      bank: midiNumber(midi, 'bank'),
      index: midiNumber(midi, 'index'),
      delta32: midiNumber(midi, 'delta32'),
    }),
  sonare_midi_channel_pressure: (_samples, _sampleRate, _fixture, midi) =>
    midiChannelPressure({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      pressure: midiNumber(midi, 'pressure'),
    }),
  sonare_midi_cc: (_samples, _sampleRate, _fixture, midi) =>
    midiCc({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      controller: midiNumber(midi, 'controller'),
      value: midiNumber(midi, 'value'),
    }),
  sonare_midi_note_off: (_samples, _sampleRate, _fixture, midi) =>
    midiNoteOff({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
    }),
  sonare_midi_note_on: (_samples, _sampleRate, _fixture, midi) =>
    midiNoteOn({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      velocity: midiNumber(midi, 'velocity'),
    }),
  sonare_midi_pitch_bend: (_samples, _sampleRate, _fixture, midi) =>
    midiPitchBend({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      bend: midiNumber(midi, 'bend'),
    }),
  sonare_midi_poly_pressure: (_samples, _sampleRate, _fixture, midi) =>
    midiPolyPressure({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      note: midiNumber(midi, 'note'),
      pressure: midiNumber(midi, 'pressure'),
    }),
  sonare_midi_program: (_samples, _sampleRate, _fixture, midi) =>
    midiProgram({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      program: midiNumber(midi, 'program'),
    }),
  sonare_midi_bank_program: (_samples, _rate, _fixture, midi) =>
    midiBankProgram({
      ppq: midiNumber(midi, 'ppq'),
      group: midiNumber(midi, 'group'),
      channel: midiNumber(midi, 'channel'),
      program: midiNumber(midi, 'program'),
      bank: { msb: midiNumber(midi, 'bank_msb'), lsb: midiNumber(midi, 'bank_lsb') },
    }),
  sonare_midi_cc_learn: () => midiCcLearn({ events: corpusCcEvents(), paramId: 9 }),
  sonare_midi_cc_to_breakpoint: () =>
    midiCcToBreakpoint({
      bindings: [{ ccNumber: 74, channel: 2, paramId: 9 }],
      event: corpusCcEvents()[1],
    }),
  sonare_midi_route_events: () => midiRouteEvents({ events: corpusCcEvents() }),
  sonare_midi_param_to_cc: () =>
    midiParamToCc({
      bindings: [{ ccNumber: 74, channel: 2, paramId: 9 }],
      paramId: 9,
      unitValue: 0.75,
      group: 1,
    }),
  sonare_split_silence_common: (samples) =>
    splitSilenceCommonWithReport({
      signals: [samples, samples.subarray(0, samples.length / 2)],
    }),
  sonare_align_take_to_reference: (samples, sampleRate) =>
    alignTakeToReference({
      reference: samples,
      take: samples,
      sampleRate,
    }),
  sonare_room_geometry_from_estimate: () =>
    roomGeometryFromEstimate({
      estimate: { lengthM: 14, widthM: 5, heightM: 6, bandAbsorption: [0.25, 0.5, 0.75] },
    }),
  sonare_synthesize_rir: (_samples, sampleRate) => synthesizeRir({ sampleRate, maxSec: 0.12 }),
  sonare_room_morph: (samples, sampleRate) =>
    roomMorph({
      samples,
      sampleRate,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      maxSec: 0.12,
    }),
  sonare_room_morph_stereo: (samples, sampleRate) =>
    roomMorphStereo({
      left: samples,
      right: samples.slice().reverse(),
      sampleRate,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      maxSec: 0.12,
    }),
  sonare_lufs_series_interleaved: (samples, sampleRate, fixture) =>
    lufsSeriesMultichannel({
      channels: [samples, resultPlane('historical_right', samples.length, fixture)],
      sampleRate,
    }),
  sonare_decompose_note_pitch: (_samples, _rate, fixture) => {
    const result = decomposeNotePitch({
      f0Hz: Float32Array.of(
        ...(fixture === 'empty' ? [0, 0, 0, 0, 0, 0, 0] : [440, 441, 442, 0, 438, 437, 440]),
      ),
      frameRate: 100,
      medianHz: fixture === 'empty' ? 0 : 440,
    });
    if (fixture !== 'empty') {
      return result;
    }
    const { centerHz, ...curves } = result;
    expect(centerHz).toBe(0);
    return curves;
  },
  sonare_note_targets_from_smf: () =>
    noteTargetsFromSmf({
      data: Uint8Array.of(
        0x4d,
        0x54,
        0x68,
        0x64,
        0,
        0,
        0,
        6,
        0,
        0,
        0,
        1,
        0,
        96,
        0x4d,
        0x54,
        0x72,
        0x6b,
        0,
        0,
        0,
        12,
        0,
        0x90,
        60,
        80,
        96,
        0x80,
        60,
        0,
        0,
        0xff,
        0x2f,
        0,
      ),
    }),
  sonare_auto_tune: (samples, sampleRate) => autoTune({ samples, sampleRate }),
  sonare_hpss: (samples, sampleRate) => hpss({ samples, sampleRate }),
  sonare_decompose: () =>
    decompose({
      s: {
        rows: 4,
        cols: 6,
        data: Float32Array.of(
          3,
          2,
          1,
          2,
          3,
          2,
          1,
          2,
          4,
          3,
          2,
          1,
          2,
          4,
          3,
          1,
          2,
          4,
          4,
          1,
          3,
          2,
          1,
          3,
        ),
      },
      nComponents: 2,
    }),
  sonare_decompose_stems: (samples, sampleRate) => decomposeStems({ samples, sampleRate }),
  sonare_decompose_stems_linked: (samples, sampleRate) =>
    decomposeStemsLinked({
      channels: [samples, resultSignal('historical_right')],
      sampleRate,
    }),
  sonare_bass_chroma: (samples, sampleRate) => bassChroma({ samples, sampleRate }),
  sonare_chroma_cens: (samples, sampleRate) => chromaCens({ samples, sampleRate }),
  sonare_cqt: (samples, sampleRate) => cqt({ samples, sampleRate }),
  sonare_pseudo_cqt: (samples, sampleRate) => pseudoCqt({ samples, sampleRate }),
  sonare_hybrid_cqt: (samples, sampleRate) => hybridCqt({ samples, sampleRate }),
  sonare_vqt: (samples, sampleRate) => vqt({ samples, sampleRate }),
  sonare_detect_key_candidates: (samples, sampleRate) =>
    detectKeyCandidates({ samples, sampleRate }),
  sonare_pitch_yin: (samples, sampleRate) => pitchYin({ samples, sampleRate }),
  sonare_pitch_pyin: (samples, sampleRate) => pitchPyin({ samples, sampleRate }),
  sonare_mel_filterbank: (_samples, sampleRate) =>
    melFilterbank({ sampleRate, nFft: 128, nMels: 6 }),
  sonare_pcen: () => pcen({ values: corpusFeatureMatrix(2, 12) }),
  sonare_mel_delta: () => melDelta({ features: corpusFeatureMatrix(2, 12) }),
  sonare_tonnetz: () => tonnetz({ chromagram: corpusFeatureMatrix(12, 2) }),
  sonare_nn_filter: () => nnFilter({ s: corpusFeatureMatrix(2, 12) }),
  sonare_tempogram: (samples) => tempogram({ onsetEnvelope: samples }),
  sonare_fourier_tempogram: (samples) => fourierTempogram({ onsetEnvelope: samples }),
  sonare_cyclic_tempogram: (samples, sampleRate) =>
    cyclicTempogram({ onsetEnvelope: samples, sampleRate }),
  sonare_poly_features: (samples, sampleRate) => polyFeatures({ samples, sampleRate }),
  sonare_spectral_contrast: (samples, sampleRate) => spectralContrast({ samples, sampleRate }),
  sonare_onset_strength_multi: (samples, sampleRate) => onsetStrengthMulti({ samples, sampleRate }),
  sonare_nnls_chroma: (samples, sampleRate) => nnlsChroma({ samples, sampleRate }),
  sonare_transcribe: (samples, sampleRate) => transcribe({ samples, sampleRate }),
  sonare_note_segments: () =>
    noteSegments({
      f0Hz: Float32Array.of(440, 440, 440, 440, 0, 880, 880, 880, 880, 0),
      voiced: [true, true, true, true, false, true, true, true, true, false],
      frameRate: 100,
    }),
  sonare_realtime_voice_changer_preset_config: (_samples, sampleRate) => {
    const changer = new RealtimeVoiceChanger({
      config: realtimeVoiceChangerPresetConfig('neutralMonitor'),
      sampleRate,
      maxBlockSize: 128,
      numChannels: 1,
    });
    try {
      return JSON.parse(changer.configJson());
    } finally {
      changer.destroy();
    }
  },
  sonare_realtime_voice_changer_preset_json: () =>
    JSON.parse(realtimeVoiceChangerPresetJson('neutralMonitor')),
  sonare_realtime_voice_changer_validate_preset_json: () => {
    const result = realtimeVoiceChangerValidatePresetJson(
      realtimeVoiceChangerPresetJson('neutralMonitor'),
    );
    expect(result.normalizedJson).toBeDefined();
    return { ...result, normalizedJson: JSON.parse(result.normalizedJson ?? '') };
  },

  sonare_mastering_amp_preset_catalog: () => masteringAmpPresetCatalog(),
  sonare_mastering_preset_params_json: () => masteringPresetParams('pop'),
  sonare_mastering_chain_config_from_json: () => masteringChainConfigFromJson('{"params":{}}'),
  sonare_mastering_insert_param_info: (_samples, sampleRate) =>
    masteringInsertParamInfo({ insert: 'eq.parametric', sampleRate }),
  sonare_mixing_scene_preset_json: () => mixingScenePreset('vocalReverbSend'),
  sonare_synth_engine_param_info: () => synthEngineParamInfo('subtractive'),
  sonare_synth_enum_names: () => synthEnumTables(),
  sonare_synth_patch_param_info: () => synthPatchParamInfo(),
  sonare_synth_preset_patch: () => synthPresetPatch('sine'),
  sonare_waveform_peak_pyramid: (samples) =>
    waveformPeakPyramid({ channels: [samples], samplesPerBucketLevels: [1, 2, 4] }),
  sonare_decode_channels: () => decodeChannels(taggedWavResultFixture()),
  sonare_read_audio_tags: () => readAudioTags(taggedWavResultFixture()),
  sonare_chord_symbol_from_analysis: () =>
    chordSymbolFromAnalysis({ root: 0, bass: 0, quality: 'major' }),
  sonare_mastering_repair_declick: (samples, sampleRate) =>
    masteringRepairDeclick({ samples, sampleRate }),
  sonare_mastering_repair_declick_stereo: (samples, sampleRate) =>
    masteringRepairDeclickStereo({
      left: samples,
      right: samples.slice().reverse(),
      sampleRate,
    }),
  sonare_mastering_repair_declip: (samples, sampleRate) =>
    masteringRepairDeclip({ samples, sampleRate }),
  sonare_mastering_repair_declip_stereo: (samples, sampleRate) =>
    masteringRepairDeclipStereo({
      left: samples,
      right: samples.slice().reverse(),
      sampleRate,
    }),
  sonare_mastering_repair_decrackle: (samples, sampleRate) =>
    masteringRepairDecrackle({ samples, sampleRate }),
  sonare_mastering_repair_decrackle_stereo: (samples, sampleRate) =>
    masteringRepairDecrackleStereo({
      left: samples,
      right: samples.slice().reverse(),
      sampleRate,
    }),
  sonare_piptrack: (samples, sampleRate) =>
    piptrack({ samples, sampleRate, nFft: 512, hopLength: 128 }),
  sonare_reassigned_spectrogram: (samples, sampleRate) =>
    reassignedSpectrogram({ samples, sampleRate, nFft: 512, hopLength: 128 }),
  sonare_mastering_repair_apply: (samples, sampleRate) =>
    masteringRepairApply({ channels: [samples], sampleRate, stages: [{ stage: 'declick' }] }),
  sonare_mastering_repair_dehum: (samples, sampleRate) =>
    masteringRepairDehum({ samples, sampleRate }),
  sonare_mastering_repair_dehum_stereo: (samples, sampleRate) =>
    masteringRepairDehumStereo({ left: samples, right: samples, sampleRate }),
  sonare_mastering_repair_denoise_classical: (samples, sampleRate) =>
    masteringRepairDenoiseClassical({ samples, sampleRate }),
  sonare_mastering_repair_denoise_classical_stereo: (samples, sampleRate) =>
    masteringRepairDenoiseClassicalStereo({ left: samples, right: samples, sampleRate }),
  sonare_mastering_repair_denoise_classical_linked: (samples, sampleRate) =>
    masteringRepairDenoiseClassicalLinked({ channels: [samples, samples], sampleRate }),
  sonare_mastering_repair_dereverb_classical: (samples, sampleRate) =>
    masteringRepairDereverbClassical({ samples, sampleRate }),
  sonare_mastering_repair_dereverb_classical_stereo: (samples, sampleRate) =>
    masteringRepairDereverbClassicalStereo({ left: samples, right: samples, sampleRate }),
  sonare_mastering_repair_dereverb_classical_linked: (samples, sampleRate) =>
    masteringRepairDereverbClassicalLinked({ channels: [samples, samples], sampleRate }),
  sonare_assign_note_targets: (samples, sampleRate) => {
    const track = corpusNoteTrack(sampleRate, false);
    const notes = extractNotes({ samples, sampleRate, ...track });
    return assignNoteTargets({
      notes,
      sampleRate,
      targets: [{ startSec: 0, endSec: 1, targetMidi: 60 }],
    });
  },
  sonare_merge_notes: (samples, sampleRate) => {
    const input = corpusNoteSet(samples, sampleRate, true);
    return mergeNotes({
      samples,
      sampleRate,
      f0Hz: input.f0Hz,
      frameRate: input.frameRate,
      voiced: input.voiced,
      notes: input.notes,
      first: 0,
      last: 1,
    });
  },
  sonare_split_note: (samples, sampleRate) => {
    const input = corpusNoteSet(samples, sampleRate, false);
    return splitNote({
      samples,
      sampleRate,
      f0Hz: input.f0Hz,
      frameRate: input.frameRate,
      voiced: input.voiced,
      notes: input.notes,
      index: 0,
      frame: 16,
    });
  },
  sonare_extract_notes: (samples, sampleRate) =>
    extractNotes({
      samples,
      sampleRate,
      f0Hz: new Float32Array(16),
      voiced: new Int32Array(16),
      frameRate: 100,
    }),
  sonare_extract_percussive_events: (samples, sampleRate) =>
    extractPercussiveEvents({ samples, sampleRate }),
  sonare_mastering_assistant_suggest: (samples, sampleRate) =>
    masteringAssistantSuggest({ samples, sampleRate }),
  sonare_mastering_assistant_suggest_stereo: (samples, sampleRate) =>
    masteringAssistantSuggestStereo({ left: samples, right: samples, sampleRate }),
  sonare_analyze_bpm: (samples, sampleRate) => analyzeBpm({ samples, sampleRate }),
  sonare_analyze_dynamics: (samples, sampleRate) => analyzeDynamics({ samples, sampleRate }),
  sonare_analyze_impulse_response: (samples, sampleRate) =>
    analyzeImpulseResponse({ samples, sampleRate }),
  sonare_analyze_melody: (samples, sampleRate) => analyzeMelody({ samples, sampleRate }),
  sonare_analyze_rhythm: (samples, sampleRate) => analyzeRhythm({ samples, sampleRate }),
  sonare_analyze_sections: (samples, sampleRate) => analyzeSections({ samples, sampleRate }),
  sonare_analyze_timbre: (samples, sampleRate) => analyzeTimbre({ samples, sampleRate }),
  sonare_chroma: (samples, sampleRate) => chroma({ samples, sampleRate }),
  sonare_detect_acoustic: (samples, sampleRate) => detectAcoustic({ samples, sampleRate }),
  sonare_detect_boundaries: (samples, sampleRate) => detectBoundaries({ samples, sampleRate }),
  sonare_estimate_meter_json: (samples) =>
    estimateMeter({
      observations: {
        accent: Array.from(samples),
        hasAttack: [],
        lowFrequencyEnergy: [],
        subdivisionAccent: [],
        chordChange: [],
      },
    }),
  sonare_estimate_room: (samples, sampleRate) => estimateRoom({ samples, sampleRate }),
  sonare_frame_signal: (_samples, sampleRate) =>
    frameSignal({
      samples: resultSignal('historical_frame'),
      sampleRate,
      frameLength: 4,
      hopLength: 2,
    }),
  sonare_lufs: (samples, sampleRate) => lufs({ samples, sampleRate }),
  sonare_lufs_interleaved: (samples, sampleRate, fixture) =>
    lufsMultichannel({
      channels: [samples, resultPlane('historical_right', samples.length, fixture)],
      sampleRate,
    }),
  sonare_mastering_analyze_pair: (samples, sampleRate, fixture) =>
    masteringAnalyzePair({
      analysis: 'match.tonalBalance',
      source: samples,
      reference: resultPlane('historical_reference', samples.length, fixture),
      sampleRate,
    }),
  sonare_mastering_analyze_pair_stereo: (samples, sampleRate, fixture) =>
    masteringAnalyzePair({
      analysis: 'match.tonalBalance',
      source: [samples, resultPlane('historical_right', samples.length, fixture)],
      reference: [
        resultPlane('historical_reference', samples.length, fixture),
        resultPlane('historical_reference_right', samples.length, fixture),
      ],
      sampleRate,
    }),
  sonare_mastering_analyze_stereo: (samples, sampleRate, fixture) =>
    masteringAnalyzeStereo({
      analysis: 'stereo.monoCompatCheck',
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),
  sonare_mastering_audio_profile: (samples, sampleRate, fixture) =>
    masteringAudioProfile({
      samples,
      sampleRate,
      ...(fixture === 'profile_silence' ||
      fixture === 'clipped' ||
      fixture === 'historical_reference_right'
        ? { detectDefects: true }
        : {}),
    }),
  sonare_mastering_audio_profile_stereo: (samples, sampleRate, fixture) =>
    masteringAudioProfileStereo({
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),
  sonare_mastering_dynamics_compressor: (samples, sampleRate) =>
    masteringDynamicsCompressor({ samples, sampleRate }),
  sonare_mastering_dynamics_gate: (samples, sampleRate) =>
    masteringDynamicsGate({ samples, sampleRate }),
  sonare_mastering_dynamics_transient_shaper: (samples, sampleRate) =>
    masteringDynamicsTransientShaper({ samples, sampleRate }),
  sonare_mastering_insert_timing: (_samples, sampleRate) =>
    masteringInsertTiming({ insert: 'eq.parametric', sampleRate }),
  sonare_mastering_repair_detect_clicks: (samples, sampleRate) =>
    masteringRepairDetectClicks({ samples, sampleRate }),
  sonare_mastering_repair_detect_clipping: (samples, sampleRate) =>
    masteringRepairDetectClipping({ samples, sampleRate }),
  sonare_mastering_repair_detect_crackle: (samples, sampleRate) =>
    masteringRepairDetectCrackle({ samples, sampleRate }),
  sonare_mastering_repair_detect_hum: (samples, sampleRate) =>
    masteringRepairDetectHum({ samples, sampleRate }),
  sonare_mastering_repair_detect_noise_floor: (samples, sampleRate) =>
    masteringRepairDetectNoiseFloor({ samples, sampleRate }),
  sonare_mastering_repair_detect_reverb: (samples, sampleRate) =>
    masteringRepairDetectReverb({ samples, sampleRate }),
  sonare_mastering_repair_detect_trim_range: (samples, sampleRate) =>
    masteringRepairDetectTrimRange({ samples, sampleRate }),
  sonare_mastering_repair_detect_trim_range_stereo: (samples, sampleRate, fixture) =>
    masteringRepairDetectTrimRangeStereo({
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),
  sonare_mel_spectrogram: (samples, sampleRate) => melSpectrogram({ samples, sampleRate }),
  sonare_metering_detect_clipping: (samples, sampleRate) =>
    meteringDetectClipping({ samples, sampleRate }),
  sonare_metering_dynamic_range: (samples, sampleRate) =>
    meteringDynamicRange({ samples, sampleRate }),
  sonare_metering_phase_scope: (samples, sampleRate, fixture) =>
    meteringPhaseScope({
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),
  sonare_metering_spectrum: (samples, sampleRate) => meteringSpectrum({ samples, sampleRate }),
  sonare_metering_spectrum_frame: (samples, sampleRate) =>
    meteringSpectrumFrame({ samples, sampleRate }),
  sonare_metering_vectorscope: (samples, sampleRate, fixture) =>
    meteringVectorscope({
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),
  sonare_mfcc: (samples, sampleRate) => mfcc({ samples, sampleRate }),
  sonare_mixing_assistant_suggest: (samples, sampleRate) =>
    mixingAssistantSuggest({
      tracks: [
        { id: 'tone', channels: [samples] },
        { id: 'silent', channels: [new Float32Array(samples.length)] },
      ],
      sampleRate,
    }),
  sonare_segment_cross_similarity: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentCrossSimilarity({ x: segmentMatrix(i.x), y: segmentMatrix(i.y) });
  },
  sonare_segment_recurrence_matrix: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentRecurrenceMatrix({ data: segmentMatrix(i.data) });
  },
  sonare_segment_recurrence_to_lag: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentRecurrenceToLag({ recurrence: segmentMatrix(i.recurrence) });
  },
  sonare_segment_lag_to_recurrence: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentLagToRecurrence({ lag: segmentMatrix(i.lag) });
  },
  sonare_segment_subsegment: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentSubsegment({ data: segmentMatrix(i.data), boundaries: i.boundaries });
  },
  sonare_segment_agglomerative: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentAgglomerative({ data: segmentMatrix(i.data), k: i.k });
  },
  sonare_segment_path_enhance: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return segmentPathEnhance({ recurrence: segmentMatrix(i.recurrence), win: i.win });
  },
  sonare_mel_to_stft: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return melToStft({ power: segmentMatrix(i.mel), sampleRate: 22050, nFft: 64 });
  },
  sonare_mfcc_to_mel: (_s, _r, _f, _m, raw) => {
    const i = requiredSegmentInput(raw);
    return mfccToMel({ coefficients: segmentMatrix(i.coefficients) });
  },
  sonare_stft: (samples, sampleRate) => stft({ samples, sampleRate }),
  sonare_streaming_loudness_gain: (samples, sampleRate) =>
    streamingLoudnessGain({ samples, sampleRate }),
  sonare_streaming_loudness_gain_stereo: (samples, sampleRate, fixture) =>
    streamingLoudnessGainStereo({
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),
  sonare_trim_silence: (samples, sampleRate) => trimSilence({ samples, sampleRate }),
  sonare_waveform_peaks: (samples, _sampleRate, fixture) =>
    waveformPeaks({
      channels: [samples, resultPlane('historical_right', samples.length, fixture)],
    }),
  sonare_mixer_process_stereo: (samples, sampleRate) =>
    mixStereo({
      strips: [
        {
          left: samples,
          right: samples
            .slice()
            .reverse()
            .map((sample) => sample * 0.5),
        },
      ],
      sampleRate,
    }),
  sonare_playback_render_interleaved: (samples, sampleRate) =>
    renderPlayback({
      samples,
      sampleRate,
      numChannels: 2,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    }),
  sonare_mastering_repair_analyze: (samples, sampleRate, fixture) =>
    masteringRepairAnalyze({
      channels: [samples, resultPlane('historical_right', samples.length, fixture)],
      sampleRate,
    }),
  sonare_mastering_streaming_preview: (samples, sampleRate) =>
    masteringStreamingPreview(samples, sampleRate),
  sonare_mastering_streaming_preview_stereo: (samples, sampleRate, fixture) =>
    masteringStreamingPreviewStereo({
      left: samples,
      right: resultPlane('historical_right', samples.length, fixture),
      sampleRate,
    }),

  sonare_mastering_repair_trim_silence: (samples, sampleRate) =>
    masteringRepairTrimSilence({ samples, sampleRate }),
  sonare_mastering_repair_trim_silence_stereo: (samples, sampleRate) =>
    masteringRepairTrimSilenceStereo({ left: samples, right: samples, sampleRate }),
  sonare_normalize_stereo: (samples, sampleRate) =>
    normalizeStereo({ left: samples, right: samples, sampleRate }),
  sonare_normalize_rms_stereo: (samples, sampleRate) =>
    normalizeStereo({ left: samples, right: samples, sampleRate, mode: 'rms' }),

  sonare_master_audio: (samples, sampleRate) => masterAudio({ samples, sampleRate, overrides: {} }),
  sonare_master_audio_stereo: (samples, sampleRate) =>
    masterAudioStereo({ left: samples, right: samples, sampleRate, overrides: {} }),
  sonare_mastering_process: (samples, sampleRate) =>
    masteringNormalizeLoudness({ samples, sampleRate }),
  sonare_mastering_apply_processor: (samples, sampleRate) =>
    masteringApplyProcessor({ processor: 'utility.gain', samples, sampleRate }),
  sonare_mastering_apply_processor_stereo: (samples, sampleRate) =>
    masteringApplyProcessorStereo({
      processor: 'utility.gain',
      left: samples,
      right: samples,
      sampleRate,
    }),
  sonare_mastering_apply_pair_processor: (samples, sampleRate) =>
    masteringApplyPairProcessor({
      processor: 'match.abCrossfade',
      source: samples,
      reference: samples.slice().reverse(),
      sampleRate,
    }),
  sonare_mastering_apply_pair_processor_stereo: (samples, sampleRate) =>
    masteringApplyPairProcessorStereo({
      processor: 'match.abCrossfade',
      sourceLeft: samples,
      sourceRight: samples.slice().reverse(),
      referenceLeft: samples.slice().reverse(),
      referenceRight: samples
        .slice()
        .reverse()
        .map((sample) => sample * 0.5),
      sampleRate,
    }),
  sonare_mastering_ab_match_loudness: (samples, sampleRate, fixture) =>
    masteringAbMatchLoudness({
      source: samples,
      reference: resultPlane('historical_reference', samples.length, fixture),
      sampleRate,
    }),
  sonare_mastering_ab_match_loudness_stereo: (samples, sampleRate, fixture) =>
    masteringAbMatchLoudnessStereo({
      sourceLeft: samples,
      sourceRight: resultPlane('historical_right', samples.length, fixture),
      referenceLeft: resultPlane('historical_reference', samples.length, fixture),
      referenceRight: resultPlane('historical_reference_right', samples.length, fixture),
      sampleRate,
    }),
};

function isExactRecord(
  value: PublicResultValue | undefined,
): value is Record<string, PublicResultValue> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function expandChainParams(
  params: Record<string, PublicResultValue>,
): Record<string, PublicResultValue> {
  const expanded: Record<string, PublicResultValue> = {};
  for (const [path, value] of Object.entries(params)) {
    const keys = path.split('.');
    const leaf = keys.pop();
    if (leaf === undefined) {
      throw new Error('expected a chain parameter path');
    }
    let parent = expanded;
    for (const key of keys) {
      const child = parent[key];
      if (isExactRecord(child)) {
        parent = child;
      } else {
        const record: Record<string, PublicResultValue> = {};
        parent[key] = record;
        parent = record;
      }
    }
    parent[leaf] = value;
  }
  return expanded;
}

function catalogWithoutNullFields(value: PublicResultValue): PublicResultValue {
  if (Array.isArray(value)) {
    return value.map(catalogWithoutNullFields);
  }
  if (isExactRecord(value)) {
    return Object.fromEntries(
      Object.entries(value)
        .filter(([, field]) => field !== null)
        .map(([key, field]) => [key, catalogWithoutNullFields(field)]),
    );
  }
  return value;
}

function facadeResultOracle(entry: PublicResultEntry, value: PublicResultValue): PublicResultValue {
  switch (entry) {
    case 'sonare_mastering_insert_param_info':
      // Catalog JSON represents omitted bounds and choices as null.
      return catalogWithoutNullFields(value);
    case 'sonare_mastering_assistant_suggest':
    case 'sonare_mastering_assistant_suggest_stereo': {
      if (
        !isExactRecord(value) ||
        !isExactRecord(value.chainConfig) ||
        !isExactRecord(value.chainConfig.params)
      ) {
        throw new Error(`expected an assistant result for ${entry}`);
      }
      return { ...value, chainConfig: expandChainParams(value.chainConfig.params) };
    }
    case 'sonare_mastering_preset_params_json':
    case 'sonare_mastering_chain_config_from_json':
      // Facades expose the parameters, without the chain document envelope.
      if (!isExactRecord(value) || !isExactRecord(value.params)) {
        throw new Error(`expected a chain configuration document for ${entry}`);
      }
      return expandChainParams(value.params);
    case 'sonare_mastering_apply_pair_processor': {
      // The JS mono pair result omits the loudness target flag for a crossfade.
      if (!isExactRecord(value)) {
        throw new Error('expected a mastering result');
      }
      const { loudnessTargetLimited, ...result } = value;
      expect(loudnessTargetLimited).toBe(false);
      return result;
    }
    default:
      return value;
  }
}

function taggedWavResultFixture(): Uint8Array {
  return encodeAudio({
    channels: [
      Float32Array.from([0.25, -0.25, 0.75, -0.5]),
      Float32Array.from([-0.125, 0.375, 0.5, -0.75]),
    ],
    sampleRate: 22050,
    format: 'wav',
    tags: { title: 'Morning', artist: 'Ada' },
  });
}

function resultSignal(name: keyof PublicResultSignals): Float32Array {
  return Float32Array.from(publicResultSignals[name]);
}

function resultPlane(
  name: keyof PublicResultSignals,
  sampleCount: number,
  fixture: PublicResultFixture,
): Float32Array {
  if (fixture === 'silence') {
    return new Float32Array(sampleCount);
  }
  const values = publicResultSignals[name];
  expect(values, `${name} fixture`).toHaveLength(sampleCount);
  return Float32Array.from(values);
}

function midiNumber(midi: Record<string, number> | undefined, key: string): number {
  const value = midi?.[key] ?? midi?.[camelKey(key)];
  if (typeof value !== 'number') {
    throw new Error(`MIDI corpus input is missing ${key}`);
  }
  return value;
}

function publicResultSamples(fixture: PublicResultFixture): Float32Array {
  const { frames } = publicResultSection;
  if (fixture === 'impulsive') {
    return Float32Array.from({ length: 512 }, (_, i) =>
      i === 64 || (i >= 256 && i < 260) ? 1 : ((i % 23) - 11) / 128,
    );
  }
  if (fixture === 'silence_excerpt') {
    return new Float32Array(8192);
  }
  if (fixture === 'historical_excerpt') {
    return resultSignal('historical_left').subarray(0, 8192);
  }
  if (fixture in publicResultSignals) {
    return Float32Array.from(publicResultSignals[fixture as keyof PublicResultSignals]);
  }
  if (fixture === 'empty' || fixture === 'tagged_wav') {
    return new Float32Array();
  }
  if (fixture === 'silence' || fixture === 'profile_silence') {
    return new Float32Array(frames);
  }
  return Float32Array.from({ length: frames }, (_, i) => {
    if (fixture === 'gapped' && (i < frames / 2 || i >= frames / 2 + 256)) {
      return 0;
    }
    if (fixture === 'clipped' && i % 109 < 8) {
      return 1;
    }
    return ((i % 109) - 54 + ((i % 47) - 23) * 0.5) / (i < frames / 2 ? 512 : 256);
  });
}

function normalizePublicResult(value: unknown): unknown {
  if (ArrayBuffer.isView(value)) {
    return Array.from(value as unknown as ArrayLike<unknown>, normalizePublicResult);
  }
  if (Array.isArray(value)) {
    return value.map(normalizePublicResult);
  }
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value)
        .filter(([, field]) => field !== undefined)
        .map(([key, field]) => [key, normalizePublicResult(field)]),
    );
  }
  return value;
}

function isExactPublicResultNumber(key: string): boolean {
  if (key === 'samples') {
    return false;
  }
  return /(count|samples$|sample$|frames$|^frame$|^frameStart$|^frameEnd$|^onsetSample$|^offsetSample$|^rows$|^cols$|^nFft$|^nMels$|^first$|^lastExclusive$|^numerator$|^denominator$|^harmonics$|^bandFloorBand$|^group$|^numChannels$|^unison$|^polyphony$|^sampleSet$|stride$|rate$|length$|index$|hopLength$|word\d+$|ppq$)/i.test(
    key,
  );
}

function publicResultTolerance(
  path: string,
  relativeTolerance: number,
  scale: number,
  tolerances?: Record<string, number>,
): number {
  if (tolerances === undefined) {
    return relativeTolerance * scale;
  }
  let candidate = path.replace(/\[\d+\]/g, '[]');
  for (;;) {
    if (Object.hasOwn(tolerances, candidate)) {
      return tolerances[candidate] as number;
    }
    if (!candidate.endsWith('[]')) {
      break;
    }
    candidate = candidate.slice(0, -2);
  }
  return (tolerances['*'] ?? 0) * scale;
}

function comparePublicResult(
  actual: unknown,
  expected: PublicResultValue,
  tolerance: number,
  path = 'result',
  arrayScale?: number,
  tolerances?: Record<string, number>,
): void {
  if (expected === 'Infinity' || expected === '-Infinity' || expected === 'NaN') {
    const sentinel =
      expected === 'Infinity'
        ? Number.POSITIVE_INFINITY
        : expected === '-Infinity'
          ? Number.NEGATIVE_INFINITY
          : Number.NaN;
    expect(typeof actual, path).toBe('number');
    expect(Object.is(actual, sentinel), path).toBe(true);
    return;
  }
  if (typeof expected === 'number') {
    if (typeof actual !== 'number') {
      expect(typeof actual, path).toBe('number');
      return;
    }
    const field = path.slice(path.lastIndexOf('.') + 1).split('[')[0] ?? '';
    const exactField = isExactPublicResultNumber(field);
    if (exactField) {
      expect(actual, path).toBe(expected);
      return;
    }
    const scale = arrayScale ?? Math.abs(expected);
    const bound = publicResultTolerance(path, tolerance, scale, tolerances);
    const actualNumber = actual;
    const difference = Math.abs(actualNumber - expected);
    if (!Number.isFinite(actualNumber) || difference > bound) {
      expect(Number.isFinite(actualNumber), path).toBe(true);
      expect(difference, path).toBeLessThanOrEqual(bound);
    }
    return;
  }
  if (expected === null || typeof expected !== 'object') {
    expect(actual, path).toBe(expected);
    return;
  }
  if (Array.isArray(expected)) {
    expect(Array.isArray(actual), path).toBe(true);
    const actualArray = actual as unknown[];
    expect(actualArray, `${path} length`).toHaveLength(expected.length);
    let scale = 0;
    let hasNumericValues = false;
    for (const value of expected) {
      if (typeof value === 'number') {
        scale = Math.max(scale, Math.abs(value));
        hasNumericValues = true;
      }
    }
    const arrayScale = hasNumericValues ? scale : undefined;
    for (const [index, value] of expected.entries()) {
      comparePublicResult(
        actualArray[index],
        value,
        tolerance,
        `${path}[${index}]`,
        arrayScale,
        tolerances,
      );
    }
    return;
  }
  expect(actual !== null && typeof actual === 'object' && !Array.isArray(actual), path).toBe(true);
  const actualObject = actual as Record<string, unknown>;
  const expectedKeys = Object.keys(expected).sort();
  expect(Object.keys(actualObject).sort(), `${path} keys`).toEqual(expectedKeys);
  for (const key of expectedKeys) {
    comparePublicResult(
      actualObject[key],
      expected[key],
      tolerance,
      `${path}.${key}`,
      undefined,
      tolerances,
    );
  }
}

function expectPublicResultRefusal(run: () => unknown, refusal: PublicResultRefusal): void {
  let caught: unknown;
  try {
    run();
  } catch (error) {
    caught = error;
  }
  expect(caught, `${refusal.id}: expected refusal`).toBeInstanceOf(Error);
  const constructors = { RangeError, TypeError, SonareError };
  expect(caught, `${refusal.id}: class`).toBeInstanceOf(constructors[refusal.js_class]);
  const error = caught as Error & { code?: number };
  if (refusal.js_class === 'SonareError') {
    expect(error.name, `${refusal.id}: coded error name`).toBe('SonareError');
  } else {
    expect(error.constructor, `${refusal.id}: exact built-in class`).toBe(
      constructors[refusal.js_class],
    );
  }
  if (refusal.js_class === 'SonareError') {
    expect(error.code, `${refusal.id}: C error code`).toBe(refusal.code);
  } else {
    expect('code' in error, `${refusal.id}: built-in errors have no code`).toBe(false);
  }
}

describe('one-shot public result corpus', () => {
  beforeAll(async () => {
    await init();
  });

  it('has complete entry coverage', () => {
    execFileSync(
      'python3',
      [
        fileURLToPath(
          new URL('../../../tests/conformance/check_result_corpus_coverage.py', import.meta.url),
        ),
        'wasm',
      ],
      { encoding: 'utf8', timeout: 30_000 },
    );
  });

  for (const testCase of publicResultSection.cases) {
    if (testCase.surfaces && !testCase.surfaces.includes('wasm')) {
      continue;
    }
    it(testCase.id, () => {
      const actual = normalizePublicResult(
        publicResultCalls[testCase.entry](
          publicResultSamples(testCase.fixture),
          publicResultSection.sample_rate,
          testCase.fixture,
          testCase.midi,
          testCase.input,
        ),
      );
      const expected = facadeResultOracle(testCase.entry, testCase.result);
      comparePublicResult(
        actual,
        expected,
        publicResultSection.relative_tolerance,
        'result',
        undefined,
        testCase.tolerances,
      );
    });
  }

  for (const refusal of publicResultSection.refusals) {
    it(refusal.id, () => {
      expectPublicResultRefusal(
        () =>
          publicResultCalls[refusal.entry](
            publicResultSamples(refusal.fixture),
            publicResultSection.sample_rate,
            refusal.fixture,
            refusal.midi,
          ),
        refusal,
      );
    });
  }
});
