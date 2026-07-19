import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

type CorpusMarker = { id: number | 'uint32_max'; ppq: number | 'nan' | 'inf'; name: string };
type CorpusCase = {
  id: string;
  accepted: boolean;
  markers: CorpusMarker[];
};
type MarkerTransaction = { initial: CorpusMarker[]; cases: CorpusCase[] };

const corpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/public_input_corpus.json', import.meta.url),
    'utf8',
  ),
) as { marker_transaction: MarkerTransaction };

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
});
