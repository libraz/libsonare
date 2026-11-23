/**
 * An analysis measure taken from nothing is an absent key, never NaN and never 0; the same measure
 * on musical input is a finite number.
 */

import { describe, expect, it } from 'vitest';
import {
  Audio,
  analyze,
  analyzeBpm,
  analyzeMelody,
  analyzeRhythm,
  analyzeTimbre,
  detectBpm,
  extractPercussiveEvents,
  masteringAssistantSuggest,
  masteringAudioProfile,
  pitchPyin,
  pitchYin,
} from '../src/index.js';

const SR = 22050;

const SILENCE = new Float32Array(SR);

/** Three impulses: audible, but too few beats to measure syncopation. */
const IMPULSES = (() => {
  const out = new Float32Array(SR * 2);
  for (const t of [0.2, 0.7, 1.2]) {
    out[Math.floor(t * SR)] = 1;
  }
  return out;
})();

/** Sixteen decaying transients at 120 BPM. */
const PULSED = (() => {
  const out = new Float32Array(SR * 8);
  for (let beat = 0; beat < 16; beat++) {
    const start = Math.round(beat * SR * 0.5);
    for (let i = 0; i < 800; i++) {
      out[start + i] = Math.exp(-i / 200) * Math.sin((2 * Math.PI * 900 * i) / SR);
    }
  }
  return out;
})();

const TONE = Float32Array.from(
  { length: SR * 2 },
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR),
);

function expectAbsent(result: object, keys: readonly string[]): void {
  for (const key of keys) {
    expect(key in result, key).toBe(false);
  }
}

function expectFinite(result: object, keys: readonly string[]): void {
  for (const key of keys) {
    expect(key in result, key).toBe(true);
    expect(Number.isFinite((result as Record<string, number>)[key]), key).toBe(true);
  }
}

const TIMBRE_KEYS = ['brightness', 'warmth', 'density', 'roughness', 'complexity'] as const;
const MELODY_KEYS = [
  'pitchRangeOctaves',
  'pitchStability',
  'meanFrequencyHz',
  'vibratoRateHz',
] as const;
const RHYTHM_KEYS = ['syncopation', 'patternRegularity', 'tempoStability'] as const;

describe('tempo that was not found', () => {
  it('detectBpm returns undefined on silence and a number on a pulse', () => {
    expect(detectBpm(SILENCE, SR)).toBeUndefined();
    expect(Number.isFinite(detectBpm(PULSED, SR))).toBe(true);
  });

  it('Audio.detectBpm returns undefined on silence', () => {
    expect(Audio.fromBuffer(SILENCE, SR).detectBpm()).toBeUndefined();
    expect(Number.isFinite(Audio.fromBuffer(PULSED, SR).detectBpm())).toBe(true);
  });

  it('analyzeBpm omits bpm on silence', () => {
    const result = analyzeBpm(SILENCE, SR);
    expectAbsent(result, ['bpm']);
    expect(result.candidates).toEqual([]);
    expectFinite(analyzeBpm(PULSED, SR), ['bpm']);
  });

  it('analyzeRhythm omits bpm on silence', () => {
    expectAbsent(analyzeRhythm(SILENCE, SR), ['bpm']);
    expectFinite(analyzeRhythm(PULSED, SR), ['bpm']);
  });

  it('analyze omits bpm on silence', () => {
    expectAbsent(analyze(SILENCE, SR), ['bpm']);
    expectFinite(analyze(PULSED, SR), ['bpm']);
  });

  it('the mastering audio profile and the assistant profile omit bpm on silence', () => {
    expectAbsent(masteringAudioProfile(SILENCE, SR), ['bpm']);
    expectAbsent(masteringAssistantSuggest(SILENCE, SR).profile, ['bpm']);
    expectFinite(masteringAudioProfile(PULSED, SR), ['bpm']);
    expectFinite(masteringAssistantSuggest(PULSED, SR).profile, ['bpm']);
  });
});

describe('rhythm measures that were not taken', () => {
  it('analyzeRhythm omits all three on silence', () => {
    expectAbsent(analyzeRhythm(SILENCE, SR), RHYTHM_KEYS);
  });

  it('analyzeRhythm omits syncopation on a three-impulse clip but keeps the interval measures', () => {
    const result = analyzeRhythm(IMPULSES, SR);
    expectAbsent(result, ['syncopation']);
    expectFinite(result, ['patternRegularity', 'tempoStability']);
  });

  it('analyzeRhythm measures all three on a pulse', () => {
    expectFinite(analyzeRhythm(PULSED, SR), RHYTHM_KEYS);
  });

  it('analyze omits the rhythm measures on silence and syncopation on a three-impulse clip', () => {
    expectAbsent(analyze(SILENCE, SR).rhythm, RHYTHM_KEYS);
    expectAbsent(analyze(IMPULSES, SR).rhythm, ['syncopation']);
    expectFinite(analyze(PULSED, SR).rhythm, RHYTHM_KEYS);
  });
});

describe('timbre measures that were not taken', () => {
  it('analyzeTimbre omits every overall measure and every per-window measure on silence', () => {
    const result = analyzeTimbre(SILENCE, SR);
    expectAbsent(result, TIMBRE_KEYS);
    expect(result.timbreOverTime.length).toBeGreaterThan(0);
    for (const frame of result.timbreOverTime) {
      expectAbsent(frame, TIMBRE_KEYS);
    }
  });

  it('analyzeTimbre omits the measures of a window with no energy and keeps the others', () => {
    const result = analyzeTimbre(IMPULSES, SR);
    expectFinite(result, TIMBRE_KEYS);
    for (const key of TIMBRE_KEYS) {
      expect(
        result.timbreOverTime.some((frame) => key in frame),
        key,
      ).toBe(true);
      expect(
        result.timbreOverTime.some((frame) => !(key in frame)),
        key,
      ).toBe(true);
    }
  });

  it('analyze omits the timbre measures on silence', () => {
    expectAbsent(analyze(SILENCE, SR).timbre, TIMBRE_KEYS);
    expectFinite(analyze(PULSED, SR).timbre, TIMBRE_KEYS);
  });
});

describe('melody measures that were not taken', () => {
  it('analyzeMelody omits all four on silence and measures them on a tone', () => {
    expectAbsent(analyzeMelody(SILENCE, SR), MELODY_KEYS);
    expectFinite(analyzeMelody(TONE, SR), MELODY_KEYS);
  });

  it('analyze omits the melody measures on silence', () => {
    expectAbsent(analyze(SILENCE, SR).melody, MELODY_KEYS);
    expectFinite(analyze(TONE, SR).melody, MELODY_KEYS);
  });
});

describe('pitch summaries with no voiced frame', () => {
  it.each([
    ['pitchYin', pitchYin],
    ['pitchPyin', pitchPyin],
  ] as const)('%s omits medianF0 and meanF0 on silence and reports them on a tone', (_n, fn) => {
    expectAbsent(fn(SILENCE, SR), ['medianF0', 'meanF0']);
    expectFinite(fn(TONE, SR), ['medianF0', 'meanF0']);
  });
});

describe('percussive events', () => {
  it('report percussiveRatio as a finite number for a measured span', () => {
    const events = extractPercussiveEvents(PULSED, SR);
    expect(events.length).toBeGreaterThan(0);
    for (const event of events) {
      expectFinite(event, ['percussiveRatio']);
    }
  });
});
