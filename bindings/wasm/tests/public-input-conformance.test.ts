import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import type { MixStereoStrip } from '../dist/index.js';
import { init, midiCc, midiCcLearn, mixStereo, RealtimeEngine } from '../dist/index.js';
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
});
