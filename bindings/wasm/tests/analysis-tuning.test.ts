import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  chordFunctionalAnalysis,
  detectChords,
  detectKey,
  detectKeyCandidates,
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
    expect(estimate).toBeLessThan(-40);
    expect(estimate).toBeGreaterThan(-50);
  });

  it('recovers plain triads in detectChords, positional and request forms alike', () => {
    // The STFT front end with the full vocabulary is the one a detuned triad misleads.
    const front = { chromaMethod: 'stft', useTriadsOnly: false } as const;
    const off = new Set(
      detectChords(detuned, sampleRate, { ...front, tuningCents: 0 }).chords.map((c) => c.name),
    );
    const on = detectChords({ samples: detuned, sampleRate, ...front, tuningCents: estimate });
    expect(on).toEqual(detectChords(detuned, sampleRate, { ...front, tuningCents: estimate }));
    const onNames = new Set(on.chords.map((c) => c.name));
    for (const triad of ['C', 'G', 'Am', 'F']) {
      expect(onNames.has(triad)).toBe(true);
      expect(off.has(triad)).toBe(false);
    }
  });

  it('measures the tuning by default and reads the triads and key from it', () => {
    const front = { chromaMethod: 'stft', useTriadsOnly: false } as const;
    const byDefault = detectChords(detuned, sampleRate, front);
    expect(byDefault.tuningCents).toBeCloseTo(-45, -1);
    const names = new Set(byDefault.chords.map((c) => c.name));
    for (const triad of ['C', 'G', 'Am', 'F']) {
      expect(names.has(triad)).toBe(true);
    }
    const key = detectKey(detuned, sampleRate);
    expect(key.tuningCents).toBeCloseTo(-45, -1);
    expect([key.root, key.mode]).toEqual([0, 'major']);
    const candidates = detectKeyCandidates(detuned, sampleRate);
    expect([candidates[0].key.root, candidates[0].key.mode]).toEqual([0, 'major']);
    const labels = chordFunctionalAnalysis({
      samples: detuned,
      sampleRate,
      keyRoot: 0,
      keyMode: 'major',
    });
    expect(labels.map((c) => c.name)).toEqual(
      detectChords(detuned, sampleRate).chords.map((c) => c.name),
    );
  });

  it('re-centres analyze on C major and numbers its chords like chordFunctionalAnalysis', () => {
    // On the constant-Q front end the off key can still read C major; what is
    // pinned is that the given tuning is the one reported.
    const off = analyze(detuned, sampleRate, { tuningCents: 0 });
    const on = analyze({ samples: detuned, sampleRate, tuningCents: estimate });
    const byDefault = analyze(detuned, sampleRate);
    expect(off.tuningCents).toBe(0);
    expect(on.tuningCents).toBeCloseTo(estimate, 4);
    expect(byDefault.tuningCents).toBeCloseTo(-45, -1);
    expect([byDefault.key.root, byDefault.key.mode]).toEqual([0, 'major']);
    expect([on.key.root, on.key.mode]).toEqual([0, 'major']);
    const numerals = new Set(on.chords.filter((c) => c.name !== 'N.C.').map((c) => c.romanNumeral));
    for (const numeral of ['I', 'V', 'vi', 'IV']) {
      expect(numerals.has(numeral)).toBe(true);
    }

    const labels = chordFunctionalAnalysis({
      samples: detuned,
      sampleRate,
      keyRoot: 0,
      keyMode: 'major',
      tuningCents: estimate,
    });
    const detected = detectChords({ samples: detuned, sampleRate, tuningCents: estimate }).chords;
    expect(labels.map((c) => c.name)).toEqual(detected.map((c) => c.name));
    const labelByName = new Map(labels.map((c) => [c.name, c.romanNumeral]));
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
    expect(() => analyze(short, sampleRate, { tuningCents: 50 })).toThrow();
    expect(() => analyze({ samples: short, sampleRate, tuningCents: -60 })).toThrow();
    expect(() => detectChords(short, sampleRate, { tuningCents: 50 })).toThrow();
    expect(() =>
      chordFunctionalAnalysis({
        samples: short,
        sampleRate,
        keyRoot: 0,
        keyMode: 'major',
        tuningCents: 50,
      }),
    ).toThrow();
  });
});

describe('analyze progress options', () => {
  const samples = detuned.subarray(0, 4 * sampleRate);
  const options = { computeTempoCurve: true, tuningCents: -45 };

  it('returns what analyze returns under the same options, in both forms', () => {
    const stages: string[] = [];
    const request = analyze({
      samples,
      sampleRate,
      ...options,
      onProgress: (_p, stage) => {
        stages.push(stage);
      },
    });
    let positionalCalls = 0;
    const positional = analyze(samples, sampleRate, {
      ...options,
      onProgress: () => {
        positionalCalls++;
      },
    });
    const plain = analyze(samples, sampleRate, options);
    expect(request).toEqual(plain);
    expect(positional).toEqual(plain);
    expect(stages.length).toBe(positionalCalls);
    expect(stages.length).toBeGreaterThan(0);
    expect(plain.beatLocalBpm.length).toBeGreaterThan(0);
  });

  it('keeps the defaults when options are omitted', () => {
    expect(analyze({ samples, sampleRate })).toEqual(analyze(samples, sampleRate));
    expect(analyze({ samples, sampleRate }).beatLocalBpm).toEqual([]);
  });

  it('still honours cancellation with options', () => {
    expect(() => analyze({ samples, sampleRate, ...options, cancel: () => true })).toThrow();
  });
});
