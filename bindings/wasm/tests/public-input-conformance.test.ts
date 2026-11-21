import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import type { MixStereoStrip } from '../dist/index.js';
import {
  cqt,
  gm2DrumName,
  gm2DrumSetName,
  gm2InstrumentName,
  gmDrumName,
  gmFamilyFirstProgram,
  gmFamilyName,
  gmInstrumentName,
  hybridCqt,
  init,
  Mixer,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringInsertTiming,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
  midiCc,
  midiCcLearn,
  midiCcName,
  midiCcToBreakpoint,
  midiNoteOn,
  midiParamToCc,
  mixStereo,
  perNoteControllerName,
  pseudoCqt,
  RealtimeEngine,
  roomMorphStereo,
  SonareError,
  StreamingRetune,
  vqt,
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
          expect(engine.midiCcBindingCount).toBe(1);
        } else {
          expect(() => engine.bindMidiCc(binding() as never)).toThrow(RangeError);
          expect(engine.midiCcBindingCount).toBe(0);
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
          expect(engine.midiCcBindingCount).toBe(0);
          engine.bindMidiCc(binding(accepted) as never);
          expect(engine.midiCcBindingCount).toBe(1);
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

type EngineZeroCase = ReaderSplitCase & { entry: string; pending?: Record<string, string> };
type EngineZeroSection = { roots: Record<string, string>; cases: EngineZeroCase[] };

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
        expect(call).toThrow(
          new RangeError(
            `RealtimeEngine.${camelKey(testCase.entry)}: ${root}.${key} ${testCase.predicate}`,
          ),
        );
      } finally {
        engine.destroy();
      }
    });
  }
});
