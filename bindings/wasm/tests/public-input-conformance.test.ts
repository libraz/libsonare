import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, mixStereo, RealtimeEngine } from '../dist/index.js';
import type { MixOptions } from '../src/public_types_mixing';

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

describe('shared public-input conformance corpus (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

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
