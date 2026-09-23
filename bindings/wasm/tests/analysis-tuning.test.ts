import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  detectChords,
  estimateTuning,
  init,
} from '../dist/index.js';

const sampleRate = 22050;

/** I-V-vi-IV in C major at 120 BPM, re-struck every beat, every partial detuned. */
function detunedPopLoop(detune: number, loops = 1): Float32Array {
  const chords = [
    [48, 60, 64, 67],
    [43, 59, 62, 67],
    [45, 60, 64, 69],
    [41, 60, 65, 69],
  ];
  const beat = Math.floor(0.5 * sampleRate);
  const out = new Float32Array(loops * chords.length * 4 * beat);
  let i = 0;
  for (let loop = 0; loop < loops; loop++) {
    for (const chord of chords) {
      const freqs = chord.map((m) => 440 * 2 ** ((m - 69 + detune) / 12));
      for (let b = 0; b < 4; b++) {
        for (let n = 0; n < beat; n++) {
          const t = n / sampleRate;
          const env = Math.exp(-3 * t) * Math.min(1, t * 200);
          let value = 0;
          for (const f of freqs) {
            for (let h = 1; h <= 4; h++) {
              value += Math.sin(2 * Math.PI * f * h * t) / h;
            }
          }
          out[i++] = 0.08 * env * value;
        }
      }
    }
  }
  return out;
}

const detuned = detunedPopLoop(-0.45, 2);
let estimate = 0;

beforeAll(async () => {
  await init();
  estimate = estimateTuning(detuned, sampleRate);
});

describe('tuning on batch analysis', () => {
  it('reads the flat recording as a negative offset', () => {
    expect(estimate).toBeLessThan(-0.4);
    expect(estimate).toBeGreaterThan(-0.5);
  });

  it('recovers plain triads in detectChords, positional and request forms alike', () => {
    const off = new Set(detectChords(detuned, sampleRate).chords.map((c) => c.name));
    const on = detectChords({ samples: detuned, sampleRate, tuning: estimate });
    expect(on).toEqual(detectChords(detuned, sampleRate, { tuning: estimate }));
    const onNames = new Set(on.chords.map((c) => c.name));
    for (const triad of ['C', 'G', 'Am', 'F']) {
      expect(onNames.has(triad)).toBe(true);
      expect(off.has(triad)).toBe(false);
    }
  });

  it('re-centres analyze on C major and numbers its chords like chordFunctionalAnalysis', () => {
    const off = analyze(detuned, sampleRate);
    const on = analyze({ samples: detuned, sampleRate, tuning: estimate });
    expect([off.key.root, off.key.mode]).not.toEqual([0, 0]);
    expect([on.key.root, on.key.mode]).toEqual([0, 0]);
    const numerals = new Set(on.chords.filter((c) => c.name !== 'N.C.').map((c) => c.romanNumeral));
    for (const numeral of ['I', 'V', 'vi', 'IV']) {
      expect(numerals.has(numeral)).toBe(true);
    }

    const labels = chordFunctionalAnalysis({
      samples: detuned,
      sampleRate,
      keyRoot: 0,
      keyMode: 0,
      tuning: estimate,
    });
    const detected = detectChords({ samples: detuned, sampleRate, tuning: estimate }).chords;
    const labelByName = new Map(detected.map((c, i) => [c.name, labels[i]]));
    let compared = 0;
    for (const chord of on.chords) {
      if (chord.name === 'N.C.' || !labelByName.has(chord.name)) {
        continue;
      }
      compared++;
      expect(chord.romanNumeral).toBe(labelByName.get(chord.name));
    }
    expect(compared).toBeGreaterThanOrEqual(4);
    for (const chord of on.chords) {
      if (chord.name === 'N.C.') {
        expect(chord.romanNumeral).toBe('');
      }
    }
  });

  it('rejects an out-of-range tuning on every entry point', () => {
    const short = detuned.subarray(0, sampleRate);
    expect(() => analyze(short, sampleRate, { tuning: 0.5 })).toThrow();
    expect(() =>
      analyzeWithProgress({ samples: short, sampleRate, options: { tuning: -0.6 } }),
    ).toThrow();
    expect(() => detectChords(short, sampleRate, { tuning: 0.5 })).toThrow();
    expect(() => chordFunctionalAnalysis(short, 0, 0, sampleRate, { tuning: 0.5 })).toThrow();
  });
});

describe('analyzeWithProgress options', () => {
  const samples = detuned.subarray(0, 4 * sampleRate);
  const options = { computeTempoCurve: true, tuning: -0.45 };

  it('returns what analyze returns under the same options, in both forms', () => {
    const stages: string[] = [];
    const request = analyzeWithProgress({
      samples,
      sampleRate,
      options,
      onProgress: (_p, stage) => {
        stages.push(stage);
      },
    });
    let positionalCalls = 0;
    const positional = analyzeWithProgress(
      samples,
      sampleRate,
      () => {
        positionalCalls++;
      },
      options,
    );
    const plain = analyze(samples, sampleRate, options);
    expect(request).toEqual(plain);
    expect(positional).toEqual(plain);
    expect(stages.length).toBe(positionalCalls);
    expect(stages.length).toBeGreaterThan(0);
    expect(plain.beatLocalBpm.length).toBeGreaterThan(0);
  });

  it('keeps the defaults when options are omitted', () => {
    expect(analyzeWithProgress({ samples, sampleRate })).toEqual(analyze(samples, sampleRate));
    expect(analyzeWithProgress({ samples, sampleRate }).beatLocalBpm).toEqual([]);
  });

  it('still honours cancellation with options', () => {
    expect(() =>
      analyzeWithProgress({ samples, sampleRate, options, cancel: () => true }),
    ).toThrow();
  });
});
