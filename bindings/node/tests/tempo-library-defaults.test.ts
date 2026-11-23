/**
 * An omitted tempo bound takes the library's own default rather than a facade
 * literal, so omitting it must match spelling the documented default out, and a
 * lone bound outside the default range is refused by the library's pair check.
 */

import { describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeRhythm,
  cyclicTempogram,
  detectBeats,
  detectBpm,
  onsetStrength,
  plp,
} from '../src/index.js';

const SR = 22050;

function clicks(bpm: number, seconds: number): Float32Array {
  const out = new Float32Array(SR * seconds);
  const period = Math.round((60 / bpm) * SR);
  for (let start = 0; start < out.length; start += period) {
    for (let i = 0; i < 200 && start + i < out.length; i++) {
      out[start + i] = Math.exp(-i / 30) * Math.sin((2 * Math.PI * 1000 * i) / SR);
    }
  }
  return out;
}

describe('tempo bounds default in the library', () => {
  const x = clicks(120, 6);
  it('detectBpm and detectBeats search 60 to 200 BPM when the bounds are omitted', () => {
    expect(detectBpm(x, SR)).toBe(detectBpm(x, SR, { bpmMin: 60, bpmMax: 200 }));
    expect(Array.from(detectBeats(x, SR))).toEqual(
      Array.from(detectBeats(x, SR, { bpmMin: 60, bpmMax: 200 })),
    );
  });

  it('analyzeBpm fills omitted bounds and analyzeRhythm starts tempo from the library', () => {
    expect(analyzeBpm(x, SR)).toEqual(analyzeBpm(x, SR, { bpmMin: 60, bpmMax: 200 }));
    expect(analyzeRhythm(x, SR)).toEqual(
      analyzeRhythm(x, SR, { bpmMin: 60, bpmMax: 200, startBpm: 120 }),
    );
  });

  it('refuses a lone lower bound above the default upper bound', () => {
    expect(() => detectBpm(x, SR, { bpmMin: 250 })).toThrow(RangeError);
    expect(() => detectBpm(x, SR, { bpmMax: 40 })).toThrow(RangeError);
  });

  it('cyclicTempogram and plp take the library tempo defaults', () => {
    const onsetEnvelope = onsetStrength({ samples: x, sampleRate: SR });
    const folded = cyclicTempogram({ onsetEnvelope, sampleRate: SR });
    expect(folded.rows).toBe(60);
    expect(folded).toEqual(
      cyclicTempogram({ onsetEnvelope, sampleRate: SR, bpmMin: 60, nBins: 60 }),
    );
    expect(Array.from(plp({ onsetEnvelope, sampleRate: SR }))).toEqual(
      Array.from(plp({ onsetEnvelope, sampleRate: SR, bpmMin: 60, bpmMax: 200 })),
    );
  });
});
