/**
 * An analysis measure taken from nothing is an absent key, never NaN, 0 or null: silence has no
 * tempo, a clip with two or three beats has no rhythm regularity, and a signal with no voiced frame
 * has no pitch summary. The same entries keep the key as a finite number on musical input.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  analyze,
  analyzeBpm,
  analyzeMelody,
  analyzeRhythm,
  analyzeTimbre,
  detectBpm,
  extractPercussiveEvents,
  init,
  masteringAssistantSuggest,
  masteringAudioProfile,
  pitchPyin,
  pitchYin,
} from '../src/index';

beforeAll(async () => {
  await init();
});

const sampleRate = 22050;
const silence = new Float32Array(sampleRate);

/** A click track at `bpm` over `seconds`, each click a short decaying noise burst. */
function clickTrack(bpm: number, seconds: number): Float32Array {
  const out = new Float32Array(Math.floor(sampleRate * seconds));
  const period = Math.round((sampleRate * 60) / bpm);
  let state = 11;
  for (let start = 0; start < out.length; start += period) {
    for (let i = 0; i < 400 && start + i < out.length; i++) {
      state = (state * 1103515245 + 12345) & 0x7fffffff;
      out[start + i] = ((state / 0x7fffffff) * 2 - 1) * Math.exp(-i / 60);
    }
  }
  return out;
}

/** `count` clicks about half a second apart: audible, but with too few beats for the rhythm measures. */
function clicks(starts: readonly number[]): Float32Array {
  const out = new Float32Array(sampleRate * 2);
  for (const at of starts) {
    for (let i = 0; i < 300; i++) {
      out[at + i] = Math.exp(-i / 40) * (i % 2 === 0 ? 1 : -1);
    }
  }
  return out;
}

/** Two intervals, so regularity and stability exist but syncopation (needs four beats) does not. */
const threeClicks = clicks([2000, 13025, 24050]);
/** One interval: below the two every interval-based measure needs. */
const twoClicks = clicks([2000, 24050]);

function tone(hz: number, seconds: number): Float32Array {
  return new Float32Array(Math.floor(sampleRate * seconds)).map(
    (_, i) => 0.5 * Math.sin((2 * Math.PI * hz * i) / sampleRate),
  );
}

const music = clickTrack(120, 8);

function expectAbsent(result: object, keys: readonly string[]): void {
  for (const key of keys) {
    expect(key in result, key).toBe(false);
  }
}

function expectFinite(result: object, keys: readonly string[]): void {
  for (const key of keys) {
    const value = (result as Record<string, unknown>)[key];
    expect(typeof value, key).toBe('number');
    expect(Number.isFinite(value as number), key).toBe(true);
  }
}

const TIMBRE_KEYS = ['brightness', 'warmth', 'density', 'roughness', 'complexity'] as const;
const RHYTHM_KEYS = ['syncopation', 'patternRegularity', 'tempoStability'] as const;
const MELODY_KEYS = ['pitchRangeOctaves', 'pitchStability', 'meanFrequencyHz', 'vibratoRateHz'];

describe('tempo of silence is not measured', () => {
  it('detectBpm returns undefined on silence and a number on music', () => {
    expect(detectBpm(silence, sampleRate)).toBeUndefined();
    const bpm = detectBpm(music, sampleRate);
    expect(Number.isFinite(bpm)).toBe(true);
  });

  it('analyzeBpm omits bpm on silence and keeps it on music', () => {
    expectAbsent(analyzeBpm(silence, sampleRate), ['bpm']);
    expectFinite(analyzeBpm(music, sampleRate), ['bpm']);
  });

  it('analyzeRhythm omits bpm on silence and keeps it on music', () => {
    expectAbsent(analyzeRhythm(silence, sampleRate), ['bpm']);
    expectFinite(analyzeRhythm(music, sampleRate), ['bpm']);
  });

  it('analyze omits bpm on silence and keeps it on music', () => {
    expectAbsent(analyze(silence, sampleRate), ['bpm']);
    expectFinite(analyze(music, sampleRate), ['bpm']);
  });

  it('the mastering profile and the assistant profile omit bpm on silence', () => {
    expectAbsent(masteringAudioProfile(silence, sampleRate), ['bpm']);
    expectFinite(masteringAudioProfile(music, sampleRate), ['bpm']);
    expectAbsent(masteringAssistantSuggest(silence, sampleRate).profile, ['bpm']);
    expectFinite(masteringAssistantSuggest(music, sampleRate).profile, ['bpm']);
  });
});

describe('Audio tempo and percussive events', () => {
  it('Audio.detectBpm returns undefined on silence and a number on music', () => {
    expect(Audio.fromBuffer(silence, sampleRate).detectBpm()).toBeUndefined();
    expect(Number.isFinite(Audio.fromBuffer(music, sampleRate).detectBpm())).toBe(true);
  });

  it('extractPercussiveEvents keeps percussiveRatio a finite number on music', () => {
    const events = extractPercussiveEvents(music, sampleRate);
    expect(events.length).toBeGreaterThan(0);
    for (const event of events) {
      expectFinite(event, ['percussiveRatio']);
    }
    expect(extractPercussiveEvents(silence, sampleRate)).toEqual([]);
  });
});

describe('rhythm measures without enough beats are not measured', () => {
  it('analyzeRhythm omits all three on silence and keeps them on music', () => {
    expectAbsent(analyzeRhythm(silence, sampleRate), RHYTHM_KEYS);
    expectFinite(analyzeRhythm(music, sampleRate), RHYTHM_KEYS);
  });

  it('three clicks measure the interval-based pair but not syncopation', () => {
    const rhythm = analyzeRhythm(threeClicks, sampleRate);
    expectAbsent(rhythm, ['syncopation']);
    expectFinite(rhythm, ['patternRegularity', 'tempoStability']);
    expectAbsent(analyze(threeClicks, sampleRate).rhythm, ['syncopation']);
  });

  it('two clicks measure none of the three, in analyzeRhythm and in analyze', () => {
    expectAbsent(analyzeRhythm(twoClicks, sampleRate), RHYTHM_KEYS);
    expectAbsent(analyze(twoClicks, sampleRate).rhythm, RHYTHM_KEYS);
  });

  it('analyze omits all three on silence and keeps them on music', () => {
    expectAbsent(analyze(silence, sampleRate).rhythm, RHYTHM_KEYS);
    expectFinite(analyze(music, sampleRate).rhythm, RHYTHM_KEYS);
  });
});

describe('timbre of nothing is not measured', () => {
  it('analyzeTimbre omits every overall measure and every window entry field on silence', () => {
    const result = analyzeTimbre(silence, sampleRate);
    expectAbsent(result, TIMBRE_KEYS);
    for (const frame of result.timbreOverTime) {
      expectAbsent(frame, TIMBRE_KEYS);
    }
  });

  it('analyzeTimbre keeps the measures on music', () => {
    const result = analyzeTimbre(music, sampleRate);
    expectFinite(result, TIMBRE_KEYS);
    expect(result.timbreOverTime.length).toBeGreaterThan(0);
    expectFinite(result.timbreOverTime[0], TIMBRE_KEYS);
  });

  it('omits the fields of a window with no energy while its neighbour keeps them', () => {
    const frames = analyzeTimbre(twoClicks, sampleRate).timbreOverTime;
    expectFinite(frames[0], TIMBRE_KEYS);
    expectAbsent(frames[frames.length - 1], TIMBRE_KEYS);
  });

  it('analyze omits the timbre measures on silence and keeps them on music', () => {
    expectAbsent(analyze(silence, sampleRate).timbre, TIMBRE_KEYS);
    expectFinite(analyze(music, sampleRate).timbre, TIMBRE_KEYS);
  });
});

describe('a melody and pitch summary with no voiced frame is not measured', () => {
  it('analyzeMelody omits the summary on silence and keeps it on a tone', () => {
    expectAbsent(analyzeMelody(silence, sampleRate), MELODY_KEYS);
    expectFinite(analyzeMelody(tone(440, 2), sampleRate), [
      'pitchRangeOctaves',
      'pitchStability',
      'meanFrequencyHz',
    ]);
  });

  it('analyze omits the melody summary on silence', () => {
    expectAbsent(analyze(silence, sampleRate).melody, MELODY_KEYS);
  });

  it('pitchYin and pitchPyin omit medianF0 and meanF0 on silence and keep them on a tone', () => {
    for (const track of [pitchYin, pitchPyin]) {
      expectAbsent(track(silence, sampleRate), ['medianF0', 'meanF0']);
      expectFinite(track(tone(440, 1), sampleRate), ['medianF0', 'meanF0']);
    }
  });
});
