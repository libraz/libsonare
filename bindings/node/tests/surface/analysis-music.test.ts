import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  AnalysisBpmCandidate,
  AnalysisResult,
  AnalyzeAsyncRequest,
  AnalyzeRequest,
  Beat,
  BpmCandidate,
  BpmCandidateRelation,
  Chord,
  ChordQuality,
  GenreHint,
  GrooveType,
  KeyMode,
  KeyProfile,
  MelodyPoint,
  PitchClass,
  Section,
  SectionType,
  TempogramMode,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import {
  Audio,
  analyze,
  analyzeAsync,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeMelody,
  analyzePolyphonic,
  analyzeRhythm,
  analyzeSections,
  analyzeTimbre,
  CHORD_QUALITIES,
  chordFunctionalAnalysis,
  chordFunctions,
  detectAcoustic,
  detectBeats,
  detectBoundaries,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  estimateMeter,
  noteSegments,
  resample,
  transcribe,
} from '../../dist/index.js';
import { unknownKeyError } from '../_helpers.js';

const sampleRate = 22050;

/** A C major triad re-struck every half second, 2 s long. */
function triad(seconds = 2): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (const f of [261.63, 329.63, 392.0]) {
    for (let i = 0; i < out.length; i++) {
      const gate = 0.5 + 0.5 * Math.sin((2 * Math.PI * 2 * i) / sampleRate);
      out[i] += 0.2 * gate * Math.sin((2 * Math.PI * f * i) / sampleRate);
    }
  }
  return out;
}

/** A clicked pulse train, 120 BPM. */
function clicks(seconds = 4): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (let beat = 0; beat * 0.5 < seconds; beat++) {
    const start = Math.round(beat * 0.5 * sampleRate);
    for (let i = 0; i < 200 && start + i < out.length; i++) {
      out[start + i] = Math.exp(-i / 40) * Math.sin(i * 0.9);
    }
  }
  return out;
}

const samples = triad();

function errorOf(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

// biome-ignore lint/suspicious/noExplicitAny: table of heterogeneous entry points
const sampleTaking: [string, (...args: any[]) => unknown][] = [
  ['detectBpm', detectBpm],
  ['detectBeats', detectBeats],
  ['detectDownbeats', detectDownbeats],
  ['detectKey', detectKey],
  ['detectKeyCandidates', detectKeyCandidates],
  ['detectOnsets', detectOnsets],
  ['analyze', analyze],
  ['analyzeSections', analyzeSections],
  ['detectBoundaries', detectBoundaries],
  ['analyzeMelody', analyzeMelody],
  ['analyzeBpm', analyzeBpm],
  ['analyzeRhythm', analyzeRhythm],
  ['analyzeDynamics', analyzeDynamics],
  ['analyzeTimbre', analyzeTimbre],
  ['detectChords', detectChords],
  ['detectAcoustic', detectAcoustic],
];

describe('one positional form, sampleRate required', () => {
  it.each(sampleTaking)('%s refuses an omitted sampleRate', (_name, fn) => {
    expect(() => fn(samples)).toThrow(TypeError);
    expect(() => fn({ samples })).toThrow(TypeError);
  });

  it.each(sampleTaking)('%s refuses a non-integer sampleRate with RangeError', (_name, fn) => {
    expect(errorOf(() => fn(samples, 22050.5))).toBeInstanceOf(RangeError);
    expect(errorOf(() => fn({ samples, sampleRate: 22050.5 }))).toBeInstanceOf(RangeError);
  });

  it.each(sampleTaking)('%s refuses an argument after a request object', (_name, fn) => {
    expect(() => fn({ samples, sampleRate }, 22050)).toThrow(TypeError);
  });

  it.each(sampleTaking)('%s scans for NaN and refuses validate', (name, fn) => {
    const bad = Float32Array.from(samples);
    bad[10] = Number.NaN;
    expect(errorOf(() => fn({ samples: bad, sampleRate }))).toBeInstanceOf(RangeError);
    expect(errorOf(() => fn({ samples, sampleRate, validate: false }))).toEqual(
      unknownKeyError(`${name}: unknown key validate`),
    );
  });

  it('analyzeImpulseResponse and chordFunctionalAnalysis require sampleRate too', () => {
    expect(() => (analyzeImpulseResponse as (...a: unknown[]) => unknown)(samples)).toThrow(
      TypeError,
    );
    expect(() =>
      (chordFunctionalAnalysis as (...a: unknown[]) => unknown)(samples, undefined, { keyRoot: 0 }),
    ).toThrow(TypeError);
  });

  it.each([
    ['detectKey', (r: object) => detectKey({ samples, sampleRate, ...r }), { nFft: 2048 }],
    ['analyzeBpm', (r: object) => analyzeBpm({ samples, sampleRate, ...r }), { startBpm: 100 }],
    ['detectOnsets', (r: object) => detectOnsets({ samples, sampleRate, ...r }), { wait: 2 }],
  ])('%s: request and positional forms agree', (name, viaRequest, options) => {
    const fn = (sonare as unknown as Record<string, (...a: unknown[]) => unknown>)[name];
    expect(fn(samples, sampleRate, options)).toEqual(viaRequest(options));
  });

  it('refuses the removed nested and positional spellings', () => {
    expect(() => detectChords(samples, sampleRate, 0.3 as never)).toThrow(TypeError);
    expect(() => analyzeBpm({ samples, sampleRate, tempoMin: 60 } as never)).toThrow(TypeError);
    expect(() =>
      (analyzeSections as (...args: unknown[]) => unknown)(samples, sampleRate, { nFft: 2048 }, 1),
    ).toThrow(TypeError);
  });

  it('keeps analyzeAsync rejecting rather than throwing', async () => {
    await expect(
      (analyzeAsync as (...a: unknown[]) => Promise<unknown>)(samples),
    ).rejects.toBeInstanceOf(TypeError);
  });
});

describe('renamed result fields', () => {
  const result = analyze({ samples, sampleRate });

  it('reports beats and downbeats as Beat records', () => {
    expect(result.beats.length).toBeGreaterThan(0);
    for (const beat of result.beats) {
      expect(typeof beat.timeSec).toBe('number');
      expect(typeof beat.strength).toBe('number');
      expect('time' in beat).toBe(false);
    }
    for (const downbeat of result.downbeats) {
      expect(result.beats).toContainEqual(downbeat);
    }
    expect('beatTimes' in result).toBe(false);
    expect('downbeatIndices' in result).toBe(false);
  });

  it('reports tuning in cents and drops the semitone-fraction name', () => {
    expect(typeof result.tuningCents).toBe('number');
    expect(Math.abs(result.tuningCents)).toBeLessThanOrEqual(50);
    expect('tuning' in result).toBe(false);
  });

  it('uses *Sec for chord and section spans', () => {
    for (const chord of result.chords) {
      expect(chord.endSec).toBeGreaterThanOrEqual(chord.startSec);
      expect(chord.durationSec).toBeCloseTo(chord.endSec - chord.startSec, 5);
      expect('start' in chord || 'end' in chord || 'duration' in chord).toBe(false);
    }
    for (const section of result.sections) {
      expect(section.endSec).toBeGreaterThanOrEqual(section.startSec);
      expect(typeof section.type).toBe('string');
      expect('start' in section || 'end' in section).toBe(false);
    }
  });

  it('names the melody and dynamics fields with their units', () => {
    expect(typeof result.melody.meanFrequencyHz).toBe('number');
    expect(typeof result.melody.vibratoRateHz).toBe('number');
    // The range is absent on a signal too short to measure it.
    expect('loudnessRangeLu' in result.dynamics).toBe(false);
    const longDynamics = analyzeDynamics(triad(6), sampleRate);
    expect(typeof longDynamics.loudnessRangeLu).toBe('number');
    expect('loudnessRangeDb' in result.dynamics).toBe(false);
    expect('loudnessRangeDb' in longDynamics).toBe(false);
    for (const point of result.melody.pitches) {
      expect(typeof point.timeSec).toBe('number');
      expect('time' in point).toBe(false);
    }
  });

  it('gives tempo candidates bpm and relation', () => {
    for (const candidate of result.bpmCandidates) {
      expect(typeof candidate.bpm).toBe('number');
      expect(['primary', 'half', 'double', 'other']).toContain(candidate.relation);
      expect('value' in candidate).toBe(false);
    }
  });

  it('omits frequencyHz on an unvoiced melody frame', () => {
    const melody = analyzeMelody({ samples: new Float32Array(sampleRate / 2), sampleRate });
    expect(melody.points.length).toBeGreaterThan(0);
    expect(melody.points.every((p) => p.frequencyHz === undefined)).toBe(true);
    expect(typeof melody.meanFrequencyHz).toBe('number');
    expect('meanFrequency' in melody).toBe(false);
  });

  it('names the f0 bounds by their request keys when a bound only narrows out of range', () => {
    const samples = new Float32Array(sampleRate / 2);
    // 1e-46 passes the facade as positive and narrows to a float 0; 100.000001 narrows to 100.
    expect(() => analyzeMelody({ samples, sampleRate, f0MinHz: 1e-46 })).toThrow(
      new RangeError('analyzeMelody: f0MinHz must be finite and greater than 0'),
    );
    expect(() => analyzeMelody({ samples, sampleRate, f0MinHz: 100, f0MaxHz: 100.000001 })).toThrow(
      new RangeError('analyzeMelody: f0MaxHz must be finite and greater than f0MinHz'),
    );
  });

  it('uses the renamed fields on boundary, rhythm and dynamics results', () => {
    const boundaries = detectBoundaries({ samples, sampleRate, peakDistanceSec: 1 });
    for (const boundary of boundaries.boundaries) {
      expect(typeof boundary.timeSec).toBe('number');
    }
    expect('loudnessRangeLu' in analyzeDynamics(triad(6), sampleRate)).toBe(true);
    const dynamics = analyzeDynamics(samples, sampleRate);
    expect(dynamics.loudnessTimesSec).toBeInstanceOf(Float32Array);
    expect(dynamics.loudnessTimesSec.length).toBe(dynamics.loudnessRmsDb.length);
    expect('loudnessTimes' in dynamics).toBe(false);
    expectTypeOf<sonare.DynamicsResult>().toHaveProperty('loudnessTimesSec');
    expect(['straight', 'shuffle', 'swing']).toContain(
      analyzeRhythm(samples, sampleRate).grooveType,
    );
  });

  it('reports tuning in cents from key and chord detection', () => {
    const key = detectKey({ samples, sampleRate });
    expect(typeof key.tuningCents).toBe('number');
    expect('tuning' in key).toBe(false);
    const chords = detectChords({ samples, sampleRate });
    expect(typeof chords.tuningCents).toBe('number');
    expect('tuning' in chords).toBe(false);
  });
});

describe('tuningCents', () => {
  it('round-trips a given offset in cents', () => {
    const key = detectKey({ samples, sampleRate, tuningCents: 20 });
    expect(key.tuningCents).toBeCloseTo(20, 3);
    const analysis = analyze({ samples, sampleRate, tuningCents: -30 });
    expect(analysis.tuningCents).toBeCloseTo(-30, 3);
    expect(detectChords({ samples, sampleRate, tuningCents: 10 }).tuningCents).toBeCloseTo(10, 3);
  });

  it('refuses the old spellings and a wrong type', () => {
    expect(() => detectKey({ samples, sampleRate, tuning: 0.2 } as never)).toThrow(TypeError);
    expect(() => detectKey({ samples, sampleRate, tuningCents: '20' as never })).toThrow(TypeError);
    expect(errorOf(() => detectKey({ samples, sampleRate, tuningCents: 60 }))).toBeInstanceOf(
      RangeError,
    );
  });

  it('accepts auto', () => {
    const key = detectKey({ samples, sampleRate, tuningCents: 'auto' });
    expect(Math.abs(key.tuningCents)).toBeLessThanOrEqual(50);
  });
});

describe('closed sets refuse numbers and synonyms', () => {
  it.each([
    ['profile number', { profile: 0 }, TypeError],
    ['profile synonym', { profile: 'ks' }, RangeError],
    ['profile kebab case', { profile: 'faraldo-edmt' }, RangeError],
    ['genreHint unknown', { genreHint: 'metal' }, RangeError],
    ['genreHint number', { genreHint: 1 }, TypeError],
    ['modes kebab case', { modes: 'major-minor' }, RangeError],
    ['modes number', { modes: 1 }, TypeError],
    ['modes list with a number', { modes: ['major', 1] }, TypeError],
  ])('detectKey refuses %s', (_name, options, errorClass) => {
    expect(errorOf(() => detectKey(samples, sampleRate, options as never))).toBeInstanceOf(
      errorClass,
    );
  });

  it.each([
    ['krumhanslSchmuckler'],
    ['temperley'],
    ['shaath'],
    ['faraldoEdmt'],
    ['faraldoEdma'],
    ['faraldoEdmm'],
    ['bellmanBudge'],
  ])('detectKey accepts the lowerCamel profile %s', (profile) => {
    expect(() => detectKey(samples, sampleRate, { profile: profile as KeyProfile })).not.toThrow();
  });

  it('accepts majorMinor and the full genre-hint set', () => {
    expect(() => detectKey(samples, sampleRate, { modes: 'majorMinor' })).not.toThrow();
    for (const genreHint of ['auto', 'edm', 'electronic', 'dance', 'pop', 'classical', 'jazz']) {
      expect(() =>
        detectKey(samples, sampleRate, { genreHint: genreHint as GenreHint }),
      ).not.toThrow();
    }
  });

  it('detectKeyCandidates applies the same sets', () => {
    expect(() => detectKeyCandidates(samples, sampleRate, { profile: 'ks' as never })).toThrow(
      RangeError,
    );
    expect(() => detectKeyCandidates(samples, sampleRate, { genreHint: 'metal' as never })).toThrow(
      RangeError,
    );
  });

  it('refuses a key given as a name or a number', () => {
    expect(
      errorOf(() => detectChords(samples, sampleRate, { keyRoot: 'C' as never })),
    ).toBeInstanceOf(TypeError);
    expect(
      errorOf(() => detectChords(samples, sampleRate, { keyRoot: 12 as never })),
    ).toBeInstanceOf(RangeError);
    expect(
      errorOf(() => detectChords(samples, sampleRate, { keyMode: 1 as never })),
    ).toBeInstanceOf(TypeError);
    expect(
      errorOf(() => detectChords(samples, sampleRate, { keyMode: 'minorish' as never })),
    ).toBeInstanceOf(RangeError);
    expect(
      errorOf(() => chordFunctionalAnalysis(samples, sampleRate, { keyRoot: 'C' as never })),
    ).toBeInstanceOf(TypeError);
  });
});

describe('chord quality tokens', () => {
  it('is a readonly tuple of 25 distinct tokens indexed by native ordinal', () => {
    expect(CHORD_QUALITIES).toHaveLength(25);
    expect(new Set(CHORD_QUALITIES).size).toBe(25);
    expect(CHORD_QUALITIES[0]).toBe('major');
    expect(CHORD_QUALITIES[1]).toBe('minor');
    expect(CHORD_QUALITIES[9]).toBe('unknown');
    expect(CHORD_QUALITIES[24]).toBe('dominant7Sharp9');
    expectTypeOf<ChordQuality>().toEqualTypeOf<(typeof CHORD_QUALITIES)[number]>();
  });

  it('is exported as a type only, with no const object beside it', () => {
    expect(typeof (sonare as Record<string, unknown>).ChordQuality).toBe('undefined');
  });

  it('decodes every chord the detector emits through the tuple', () => {
    const analysis = analyze({ samples, sampleRate });
    const detected = detectChords({ samples, sampleRate }).chords;
    for (const chord of [...analysis.chords, ...detected]) {
      expect(CHORD_QUALITIES).toContain(chord.quality);
      expect(Number.isInteger(chord.root) && chord.root >= 0 && chord.root <= 11).toBe(true);
    }
  });

  it('chordFunctions takes tokens and refuses ordinals and names', () => {
    const chords = [{ root: 0 as PitchClass, quality: 'major' as ChordQuality }];
    const labelled = chordFunctions({ chords, key: { root: 0, mode: 'major' } });
    expect(labelled[0].romanNumeral).toBe('I');
    expect(labelled[0].function).toBe('tonic');
    expect('roman' in labelled[0]).toBe(false);
    const bad = (chord: object, key: object = { root: 0, mode: 'major' }) =>
      errorOf(() => chordFunctions({ chords: [chord], key } as never));
    expect(bad({ root: 0, quality: 0 })).toBeInstanceOf(TypeError);
    expect(bad({ root: 'C', quality: 'major' })).toBeInstanceOf(TypeError);
    expect(bad({ root: 0, quality: 'majestic' })).toBeInstanceOf(RangeError);
    expect(bad({ root: 0, quality: 'major' }, { root: 'C', mode: 'major' })).toBeInstanceOf(
      TypeError,
    );
    expect(bad({ root: 0, quality: 'major' }, { root: 0, mode: 0 })).toBeInstanceOf(TypeError);
  });

  it('chordFunctionalAnalysis reports romanNumeral and *Sec spans', () => {
    const labelled = chordFunctionalAnalysis(samples, sampleRate, { keyRoot: 0 });
    for (const chord of labelled) {
      expect(typeof chord.romanNumeral).toBe('string');
      expect(typeof chord.startSec).toBe('number');
    }
    expect(() => chordFunctionalAnalysis(samples, sampleRate, {} as never)).toThrow(TypeError);
  });
});

describe('analyze progress and async', () => {
  it('has no analyzeWithProgress', () => {
    expect((sonare as Record<string, unknown>).analyzeWithProgress).toBeUndefined();
  });

  it('reports progress through the request', () => {
    const stages: string[] = [];
    let last = -1;
    const result = analyze({
      samples,
      sampleRate,
      onProgress: (progress, stage) => {
        expect(progress).toBeGreaterThanOrEqual(last);
        last = progress;
        stages.push(stage);
      },
    });
    expect(stages.length).toBeGreaterThan(0);
    expect(result.beats).toBeDefined();
  });

  it('reports progress through the positional options', () => {
    let calls = 0;
    analyze(samples, sampleRate, {
      onProgress: () => {
        calls++;
      },
    });
    expect(calls).toBeGreaterThan(0);
  });

  it('refuses a non-function callback with TypeError', () => {
    expect(() => analyze({ samples, sampleRate, onProgress: 1 as never })).toThrow(TypeError);
    expect(() => analyze({ samples, sampleRate, cancel: 'x' as never })).toThrow(TypeError);
  });

  it('async resolves the same shape as the synchronous call', async () => {
    const sync = analyze({ samples, sampleRate });
    const asynchronous = await analyzeAsync(samples, sampleRate);
    expect(asynchronous.beats).toEqual(sync.beats);
    expect(asynchronous.downbeats).toEqual(sync.downbeats);
    expect(asynchronous.tuningCents).toBeCloseTo(sync.tuningCents, 5);
  });

  it('async refuses the synchronous callbacks', async () => {
    await expect(
      analyzeAsync({ samples, sampleRate, onProgress: () => {} } as never),
    ).rejects.toBeInstanceOf(TypeError);
  });

  it('moves chromaHighPassHz and tuningCents onto the request', () => {
    expect(() => analyze({ samples, sampleRate, chromaHighpassHz: 80 } as never)).toThrow(
      TypeError,
    );
    expect(() => analyze({ samples, sampleRate, chromaHighPassHz: 80 })).not.toThrow();
  });
});

describe('analyzeBpm tempo range', () => {
  it('searches 60-200 BPM by default', () => {
    const pulse = clicks();
    const byDefault = analyzeBpm(pulse, sampleRate);
    const explicit = analyzeBpm(pulse, sampleRate, { bpmMin: 60, bpmMax: 200 });
    expect(byDefault.bpm).toBe(explicit.bpm);
    expect(byDefault.confidence).toBe(explicit.confidence);
  });
});

describe('estimateMeter takes beats', () => {
  const beats: Beat[] = Array.from({ length: 16 }, (_, i) => ({
    timeSec: i * 0.5,
    strength: i % 4 === 0 ? 1 : 0.4,
  }));

  it('scores a Beat series', () => {
    const meter = estimateMeter({ beats });
    expect(meter.timeSignature.numerator).toBe(4);
  });

  it('refuses the old parallel arrays', () => {
    expect(() =>
      estimateMeter({
        beatTimes: new Float32Array(8),
        beatStrengths: new Float32Array(8),
      } as never),
    ).toThrow(TypeError);
  });

  it('refuses a malformed beat', () => {
    expect(() => estimateMeter({ beats: [{ timeSec: 0 }] } as never)).toThrow(TypeError);
    expect(() => estimateMeter({ beats: 3 } as never)).toThrow(TypeError);
  });
});

describe('resample takes two named rates', () => {
  const input = new Float32Array(2205);

  it('accepts the positional form and the request form', () => {
    const positional = resample(input, 22050, 11025);
    const viaRequest = resample({
      samples: input,
      sampleRate: 22050,
      targetSampleRate: 11025,
    });
    expect(viaRequest).toEqual(positional);
  });

  it('refuses the old field names and trailing arguments', () => {
    expect(() => resample({ samples: input, srcSr: 22050, targetSr: 11025 } as never)).toThrow(
      TypeError,
    );
    expect(() =>
      resample({ samples: input, sourceSampleRate: 22050, targetSampleRate: 11025 } as never),
    ).toThrow(TypeError);
    expect(() =>
      (resample as (...a: unknown[]) => unknown)(
        { samples: input, sampleRate: 22050, targetSampleRate: 11025 },
        22050,
      ),
    ).toThrow(TypeError);
    expect(() => resample(input, 22050.5, 11025)).toThrow(RangeError);
  });
});

describe('transcribe', () => {
  const take = triad(1);

  it('takes the positional form and the request form', () => {
    const positional = transcribe(take, sampleRate, { tempoBpm: 120, f0MinHz: 80 });
    const viaRequest = transcribe({ samples: take, sampleRate, tempoBpm: 120, f0MinHz: 80 });
    expect(positional.noteCount).toBe(viaRequest.noteCount);
  });

  it('refuses the old frequency names and an omitted rate', () => {
    expect(() => transcribe({ samples: take, sampleRate, fmin: 80 } as never)).toThrow(TypeError);
    expect(() => transcribe({ samples: take, sampleRate, fmax: 800 } as never)).toThrow(TypeError);
    expect(() => (transcribe as (...a: unknown[]) => unknown)(take)).toThrow(TypeError);
  });

  it('refuses an explicit reattackRatio of 0', () => {
    expect(
      errorOf(() => transcribe({ samples: take, sampleRate, polyphonic: true, reattackRatio: 0 })),
    ).toBeInstanceOf(RangeError);
  });
});

describe('analyzePolyphonic', () => {
  const chord = triad(1);

  it('takes the positional form', () => {
    using_(analyzePolyphonic(chord, sampleRate, { maxPolyphony: 3 }), (analysis) => {
      expect(analysis.noteCount).toBeGreaterThanOrEqual(0);
    });
  });

  it('refuses an omitted sampleRate and a trailing argument', () => {
    expect(() => (analyzePolyphonic as (...a: unknown[]) => unknown)(chord)).toThrow(TypeError);
    expect(() =>
      (analyzePolyphonic as (...a: unknown[]) => unknown)({ samples: chord, sampleRate }, 1),
    ).toThrow(TypeError);
  });

  it('declares tonality in place of tonalityOff', () => {
    expect(() =>
      analyzePolyphonic({ samples: chord, sampleRate, tonalityOff: true } as never),
    ).toThrow(TypeError);
    using_(analyzePolyphonic({ samples: chord, sampleRate, tonality: false }), (analysis) => {
      expect(analysis.frameCount).toBeGreaterThan(0);
    });
  });

  it.each(['minFramePeakRatio', 'minSeparationCents', 'minRidgePeakRatio', 'minRidgeDurationMs'])(
    '%s: an explicit 0 is accepted and a negative is a RangeError',
    (field) => {
      using_(analyzePolyphonic({ samples: chord, sampleRate, [field]: 0 }), (analysis) => {
        expect(analysis.frameCount).toBeGreaterThan(0);
      });
      expect(
        errorOf(() => analyzePolyphonic({ samples: chord, sampleRate, [field]: -1 })),
      ).toBeInstanceOf(RangeError);
    },
  );

  it('refuses an explicit reattackRatio of 0', () => {
    expect(
      errorOf(() => analyzePolyphonic({ samples: chord, sampleRate, reattackRatio: 0 })),
    ).toBeInstanceOf(RangeError);
  });

  it('spells a note edit pitchSemitones and formantSemitones', () => {
    using_(analyzePolyphonic({ samples: chord, sampleRate }), (analysis) => {
      for (const note of analysis.notes()) {
        expect(note.edit.pitchSemitones).toBe(0);
        expect(note.edit.formantSemitones).toBe(0);
        expect('pitchShiftSemitones' in note.edit).toBe(false);
      }
      if (analysis.noteCount > 0) {
        analysis.setNoteEdit(0, { pitchSemitones: 1 });
        expect(analysis.notes()[0].edit.pitchSemitones).toBe(1);
      }
    });
  });
});

function using_<T extends { destroy(): void }>(handle: T, body: (handle: T) => void): void {
  try {
    body(handle);
  } finally {
    handle.destroy();
  }
}

describe('type surface', () => {
  it('exposes the closed sets and records as unions and shapes', () => {
    expectTypeOf<SectionType>().toEqualTypeOf<
      'intro' | 'verse' | 'preChorus' | 'chorus' | 'bridge' | 'instrumental' | 'outro' | 'unknown'
    >();
    expectTypeOf<GrooveType>().toEqualTypeOf<'straight' | 'shuffle' | 'swing'>();
    expectTypeOf<BpmCandidateRelation>().toEqualTypeOf<'primary' | 'half' | 'double' | 'other'>();
    expectTypeOf<TempogramMode>().toEqualTypeOf<'autocorrelation' | 'cosine'>();
    expectTypeOf<GenreHint>().toEqualTypeOf<
      'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz'
    >();
    expectTypeOf<KeyProfile>().toEqualTypeOf<
      | 'krumhanslSchmuckler'
      | 'temperley'
      | 'shaath'
      | 'faraldoEdmt'
      | 'faraldoEdma'
      | 'faraldoEdmm'
      | 'bellmanBudge'
    >();
    expectTypeOf<PitchClass>().toEqualTypeOf<0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11>();
    expectTypeOf<Beat>().toEqualTypeOf<{ timeSec: number; strength: number }>();
    expectTypeOf<BpmCandidate>().toEqualTypeOf<{ bpm: number; confidence: number }>();
    expectTypeOf<AnalysisBpmCandidate['relation']>().toEqualTypeOf<BpmCandidateRelation>();
    expectTypeOf<AnalysisResult['bpmCandidates']>().toEqualTypeOf<AnalysisBpmCandidate[]>();
    expectTypeOf<MelodyPoint['frequencyHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<Section['type']>().toEqualTypeOf<SectionType>();
    expectTypeOf<Chord['quality']>().toEqualTypeOf<ChordQuality>();
    expectTypeOf<Chord['root']>().toEqualTypeOf<PitchClass>();
    expectTypeOf<AnalysisResult['beats']>().toEqualTypeOf<Beat[]>();
    expectTypeOf<AnalysisResult['downbeats']>().toEqualTypeOf<Beat[]>();
    expectTypeOf<AnalysisResult['tuningCents']>().toEqualTypeOf<number>();
    expectTypeOf<KeyMode>().toBeString();
    expectTypeOf<AnalyzeRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<AnalyzeRequest['onProgress']>().not.toBeUndefined();
    expectTypeOf<AnalyzeAsyncRequest>().not.toHaveProperty('onProgress');
    expectTypeOf<AnalyzeAsyncRequest>().not.toHaveProperty('cancel');
    expect(true).toBe(true);
  });

  it('no longer exports the removed names', () => {
    // @ts-expect-error removed
    type A = import('../../dist/index.js').AnalysisBeat;
    // @ts-expect-error removed
    type B = import('../../dist/index.js').AnalysisSection;
    // @ts-expect-error removed
    type C = import('../../dist/index.js').BpmHypothesis;
    // @ts-expect-error removed
    type D = import('../../dist/index.js').ChordQualityName;
    // @ts-expect-error removed
    type E = import('../../dist/index.js').DetectKeyOptions;
    // @ts-expect-error removed
    type F = import('../../dist/index.js').DynamicsAnalysisResult;
    // @ts-expect-error removed
    type G = import('../../dist/index.js').SectionTypeOrdinal;
    // @ts-expect-error removed
    type H = import('../../dist/index.js').ScaleName;
    // @ts-expect-error removed
    type I = import('../../dist/index.js').MusicAnalyzeRequest;
    // @ts-expect-error removed
    type J = import('../../dist/index.js').AnalyzeWithProgressRequest;
    // @ts-expect-error removed
    type K = import('../../dist/index.js').SamplesRequest;
    expectTypeOf<[A, B, C, D, E, F, G, H, I, J, K]>().not.toBeNever();
    expect(true).toBe(true);
  });
});

/** Clicks at 40 BPM (1.5 s apart) plus a faint 120 BPM pulse. */
function slowPulse(): Float32Array {
  const out = new Float32Array(sampleRate * 12);
  for (let t = 0; t < 12; t += 1.5) {
    const start = Math.round(t * sampleRate);
    for (let i = 0; i < 300 && start + i < out.length; i++) {
      out[start + i] = Math.exp(-i / 50) * Math.sin(i * 0.9);
    }
  }
  return out;
}

describe('tempo range on detectBpm and detectBeats', () => {
  const samples = slowPulse();

  it('detectBpm honours bpmMin and bpmMax', () => {
    // The 60-200 estimate lies above 59 BPM; a 30-59 search must answer inside its own range.
    const narrow = detectBpm(samples, sampleRate, { bpmMin: 60, bpmMax: 200 });
    const below = detectBpm(samples, sampleRate, { bpmMin: 30, bpmMax: 59 });
    expect(narrow).toBeGreaterThan(59);
    expect(narrow).toBeLessThanOrEqual(200);
    expect(below).toBeGreaterThanOrEqual(30);
    expect(below).toBeLessThanOrEqual(59);
  });

  it('detectBpm defaults to 60-200 BPM', () => {
    expect(detectBpm(samples, sampleRate)).toBe(
      detectBpm(samples, sampleRate, { bpmMin: 60, bpmMax: 200 }),
    );
  });

  it('detectBeats honours bpmMin and bpmMax and defaults to 60-200', () => {
    expect(detectBeats(samples, sampleRate)).toEqual(
      detectBeats({ samples, sampleRate, bpmMin: 60, bpmMax: 200 }),
    );
  });

  it('refuses an inverted range with a coded error', () => {
    expect(() => detectBpm({ samples, sampleRate, bpmMin: 200, bpmMax: 60 })).toThrow();
  });

  it('Audio tempo methods search 60-200 BPM by default', () => {
    const audio = Audio.fromBuffer(samples, sampleRate);
    const range = { bpmMin: 60, bpmMax: 200 };
    expect(audio.detectBpm()).toBe(detectBpm(samples, sampleRate, range));
    // The default estimate lies above 59 BPM, so a 30-59 search answers inside that range only
    // if the bounds reach the core.
    const below = { bpmMin: 30, bpmMax: 59 };
    const constrained = detectBpm(samples, sampleRate, below);
    expect(audio.detectBpm()).toBeGreaterThan(below.bpmMax);
    expect(constrained).toBeGreaterThanOrEqual(below.bpmMin);
    expect(constrained).toBeLessThanOrEqual(below.bpmMax);
    expect(audio.detectBpm(below)).toBe(constrained);
    expect(audio.detectBeats()).toEqual(detectBeats({ samples, sampleRate, ...range }));
    expect(audio.analyzeBpm().bpm).toBe(analyzeBpm({ samples, sampleRate, ...range }).bpm);
    expect(audio.detectDownbeats()).toEqual(detectDownbeats(samples, sampleRate));
  });
});

describe('noteSegments voiced', () => {
  it('takes voiced in place of voicedProb, and voiced wins when both are given', () => {
    const f0Hz = Float32Array.from({ length: 50 }, () => 440);
    const voiced = Uint8Array.from({ length: 50 }, () => 1);
    const frameRate = 100;
    const result = noteSegments({ f0Hz, frameRate, voiced });
    expect(result.length).toBeGreaterThan(0);
    expect(noteSegments({ f0Hz, frameRate, voiced, voicedProb: new Float32Array(50) })).toEqual(
      result,
    );
  });
});
