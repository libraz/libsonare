import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import type { MixStereoStrip } from '../dist/index.js';
import {
  gm2DrumName,
  gm2DrumSetName,
  gm2InstrumentName,
  gmDrumName,
  gmFamilyFirstProgram,
  gmFamilyName,
  gmInstrumentName,
  init,
  midiCc,
  midiCcLearn,
  midiCcName,
  midiCcToBreakpoint,
  midiParamToCc,
  mixStereo,
  perNoteControllerName,
  RealtimeEngine,
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
