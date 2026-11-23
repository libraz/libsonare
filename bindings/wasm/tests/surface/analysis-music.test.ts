import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import * as sonare from '../../dist/index.js';
import {
  Audio,
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeMelody,
  analyzePolyphonic,
  CHORD_QUALITIES,
  chordFunctions,
  chordSymbolFromAnalysis,
  cqt,
  cqtToAudio,
  detectBeats,
  detectBoundaries,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  estimateMeter,
  fourierTempogram,
  init,
  nnlsChroma,
  onsetStrength,
  onsetStrengthMulti,
  tempogramRatio,
  transcribe,
  vqt,
  vqtToAudio,
} from '../../dist/index.js';
import { init as initSource } from '../../src/index';
import { getSonareModule } from '../../src/module_state';
import { unknownKeyText } from '../_helpers';

const sampleRate = 22050;

/** A C major triad, half a second long. */
function triad(seconds = 0.5): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (const f of [261.63, 329.63, 392.0]) {
    for (let i = 0; i < out.length; i++) {
      out[i] += 0.2 * Math.sin((2 * Math.PI * f * i) / sampleRate);
    }
  }
  return out;
}

/** The module's functions by name, for table-driven cases. */
const api = sonare as unknown as Record<string, (...args: unknown[]) => unknown>;

function errorOf(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

beforeAll(async () => {
  await init();
});

const sampleFunctions = [
  'detectBpm',
  'detectBeats',
  'detectDownbeats',
  'detectOnsets',
  'detectKey',
  'detectKeyCandidates',
  'detectChords',
  'analyze',
  'analyzeBpm',
  'analyzeRhythm',
  'analyzeDynamics',
  'analyzeTimbre',
  'analyzeSections',
  'analyzeMelody',
  'detectBoundaries',
  'detectAcoustic',
  'analyzeImpulseResponse',
  'cqt',
  'pseudoCqt',
  'hybridCqt',
  'vqt',
  'nnlsChroma',
  'onsetStrength',
  'onsetStrengthMulti',
  'lufs',
  'momentaryLufs',
  'shortTermLufs',
  'transcribe',
  'analyzePolyphonic',
] as const;

describe('sample rate is required on every sample-taking function', () => {
  it.each(sampleFunctions)('%s refuses a missing sampleRate', (name) => {
    const samples = triad(0.25);
    expect(errorOf(() => api[name](samples))).toBeInstanceOf(TypeError);
    expect(errorOf(() => api[name]({ samples }))).toBeInstanceOf(TypeError);
  });

  it('refuses a non-integer rate as out of domain', () => {
    expect(errorOf(() => api.detectBpm(triad(0.25), 22050.5))).toBeInstanceOf(RangeError);
  });

  it('refuses a request object followed by an extra positional argument', () => {
    const samples = triad(0.25);
    expect(errorOf(() => api.detectKey({ samples, sampleRate }, sampleRate))).toBeInstanceOf(
      TypeError,
    );
  });

  it('chordFunctionalAnalysis requires options carrying keyRoot', () => {
    const samples = triad(0.25);
    expect(errorOf(() => api.chordFunctionalAnalysis(samples, sampleRate))).toBeInstanceOf(
      TypeError,
    );
    expect(
      errorOf(() => api.chordFunctionalAnalysis(samples, sampleRate, { keyRoot: 0 })),
    ).toBeUndefined();
  });
});

describe('removed names and positional overloads', () => {
  it.each(['analyzeWithProgress', 'onsetEnvelope'])('%s is gone', (name) => {
    expect(name in sonare).toBe(false);
  });

  it('exposes onsetStrength', () => {
    expect(typeof sonare.onsetStrength).toBe('function');
  });

  it.each([
    ['cqt', () => api.cqt(triad(0.25), sampleRate, 512)],
    ['vqt', () => api.vqt(triad(0.25), sampleRate, 512)],
    ['analyzeImpulseResponse', () => api.analyzeImpulseResponse(triad(0.25), sampleRate, 6)],
  ])('%s refuses a positional scalar where options go', (_name, call) => {
    expect(errorOf(call)).toBeInstanceOf(TypeError);
  });

  it.each([
    ['fourierTempogram', () => api.fourierTempogram(new Float32Array(64))],
    ['tempogramRatio', () => api.tempogramRatio(new Float32Array(64))],
    ['cqtToAudio', () => api.cqtToAudio(new Float32Array(64))],
    ['vqtToAudio', () => api.vqtToAudio(new Float32Array(64))],
  ])('%s is request-only', (_name, call) => {
    expect(errorOf(call)).toBeInstanceOf(TypeError);
  });
});

describe('positional and request forms are one call', () => {
  it('detectKey agrees across the forms', () => {
    const samples = triad();
    expect(detectKey(samples, sampleRate, { nFft: 1024 })).toEqual(
      detectKey({ samples, sampleRate, nFft: 1024 }),
    );
  });

  it('analyzeSections, detectBoundaries and transcribe take the positional form', () => {
    const samples = triad();
    expect(sonare.analyzeSections(samples, sampleRate)).toEqual(
      sonare.analyzeSections({ samples, sampleRate }),
    );
    expect(detectBoundaries(samples, sampleRate).sampleRate).toBe(
      detectBoundaries({ samples, sampleRate }).sampleRate,
    );
    expect(transcribe(samples, sampleRate, { tempoBpm: 120 }).tempoBpm).toBe(
      transcribe({ samples, sampleRate, tempoBpm: 120 }).tempoBpm,
    );
  });
});

describe('unit and name changes in settings', () => {
  const samples = triad(0.25);

  it.each([
    ['detectChords', 'minDuration', 'minDurationSec', 0.3],
    ['detectChords', 'smoothingWindow', 'smoothingWindowSec', 2],
    ['detectChords', 'tuning', 'tuningCents', 0],
    ['detectKey', 'highPassHz', 'chromaHighPassHz', 80],
    ['detectKey', 'tuning', 'tuningCents', 0],
    ['detectKeyCandidates', 'highPassHz', 'chromaHighPassHz', 80],
    ['analyze', 'chromaHighpassHz', 'chromaHighPassHz', 80],
    ['analyze', 'tuning', 'tuningCents', 0],
    ['analyzeDynamics', 'compressionThreshold', 'compressionThresholdDb', 6],
    ['analyzeMelody', 'fmin', 'f0MinHz', 65],
    ['analyzeMelody', 'fmax', 'f0MaxHz', 2093],
    ['detectBoundaries', 'peakDistance', 'peakDistanceSec', 2],
    ['detectBoundaries', 'referenceWindow', 'referenceWindowSec', 60],
    ['cqt', 'fmin', 'fminHz', 32.7],
    ['vqt', 'fmin', 'fminHz', 32.7],
    ['transcribe', 'fmin', 'f0MinHz', 65],
    ['transcribe', 'fmax', 'f0MaxHz', 2093],
  ] as const)('%s takes %s*, not the old spelling', (name, oldKey, newKey, value) => {
    expect(errorOf(() => api[name]({ samples, sampleRate, [newKey]: value }))).toBeUndefined();
    expect(errorOf(() => api[name]({ samples, sampleRate, [oldKey]: value }))).toBeInstanceOf(
      TypeError,
    );
  });
});

describe('closed sets refuse numbers and synonyms', () => {
  const samples = triad(0.25);

  it.each([
    ['profile', 'ks', RangeError],
    ['profile', 'krumhansl-schmuckler', RangeError],
    ['profile', 0, TypeError],
    ['genreHint', 'techno', RangeError],
    ['modes', 'major-minor', RangeError],
    ['modes', [0, 1], TypeError],
  ] as const)('detectKey %s=%j', (field, value, errorClass) => {
    expect(
      errorOf(() => detectKey({ samples, sampleRate, [field]: value } as never)),
    ).toBeInstanceOf(errorClass);
  });

  it.each([
    ['keyRoot', 'C', TypeError],
    ['keyRoot', 12, RangeError],
    ['keyMode', 1, TypeError],
    ['keyMode', 'maj', RangeError],
    ['chromaMethod', 'foo', RangeError],
  ] as const)('detectChords %s=%j', (field, value, errorClass) => {
    expect(
      errorOf(() => detectChords({ samples, sampleRate, [field]: value } as never)),
    ).toBeInstanceOf(errorClass);
  });

  it.each([
    'krumhanslSchmuckler',
    'temperley',
    'shaath',
    'faraldoEdmt',
    'faraldoEdma',
    'faraldoEdmm',
    'bellmanBudge',
  ])('detectKey accepts profile %s', (profile) => {
    expect(errorOf(() => detectKey({ samples, sampleRate, profile } as never))).toBeUndefined();
  });

  it('accepts the lowerCamelCase mode-set tokens', () => {
    for (const modes of ['majorMinor', 'all', 'modal', ['major', 'minor']]) {
      expect(errorOf(() => detectKey({ samples, sampleRate, modes } as never))).toBeUndefined();
    }
  });
});

describe('tuning is reported in cents', () => {
  it('reads cents back as given', () => {
    const samples = triad();
    const key = detectKey({ samples, sampleRate, tuningCents: 0 });
    expect(key.tuningCents).toBeCloseTo(0, 6);
    expect('tuning' in key).toBe(false);
    const chords = detectChords({ samples, sampleRate, tuningCents: 20 });
    expect(chords.tuningCents).toBeCloseTo(20, 4);
    expect('tuning' in chords).toBe(false);
  });

  it('measures within +-50 cents under auto', () => {
    const { tuningCents } = detectKey({ samples: triad(), sampleRate, tuningCents: 'auto' });
    expect(tuningCents).toBeGreaterThanOrEqual(-50);
    expect(tuningCents).toBeLessThan(50);
  });
});

describe('analysis records', () => {
  it('analyze returns the unified, renamed shapes', () => {
    // Long enough for a loudness-range reading (short-term windows are 3 s).
    const result = analyze(triad(6), sampleRate, { tuningCents: 0 });
    expect(result).not.toHaveProperty('beatTimes');
    expect(result).not.toHaveProperty('downbeatIndices');
    expect(result).not.toHaveProperty('tuning');
    expect(result.tuningCents).toBeCloseTo(0, 6);
    for (const beat of [...result.beats, ...result.downbeats]) {
      expect(typeof beat.timeSec).toBe('number');
      expect(beat).not.toHaveProperty('time');
    }
    for (const beat of result.downbeats) {
      expect(result.beats.some((b) => b.timeSec === beat.timeSec)).toBe(true);
    }
    for (const candidate of result.bpmCandidates) {
      expect(candidate).toHaveProperty('bpm');
      expect(candidate).not.toHaveProperty('value');
      expect(['primary', 'half', 'double', 'other']).toContain(candidate.relation);
    }
    for (const chord of result.chords) {
      expect(CHORD_QUALITIES).toContain(chord.quality);
      expect(chord.durationSec).toBeCloseTo(chord.endSec - chord.startSec, 6);
      expect(chord).not.toHaveProperty('start');
    }
    for (const section of result.sections) {
      expect([
        'intro',
        'verse',
        'preChorus',
        'chorus',
        'bridge',
        'instrumental',
        'outro',
        'unknown',
      ]).toContain(section.type);
      expect(section).toHaveProperty('startSec');
      expect(section).not.toHaveProperty('start');
    }
    expect(result.dynamics).toHaveProperty('loudnessRangeLu');
    expect(result.dynamics).not.toHaveProperty('loudnessRangeDb');
    expect(result.melody).toHaveProperty('meanFrequencyHz');
    expect(result.melody).toHaveProperty('vibratoRateHz');
    expect(['straight', 'shuffle', 'swing']).toContain(result.rhythm.grooveType);
  });

  it('analyzeDynamics reports loudness range in LU', () => {
    const result = analyzeDynamics(triad(6), sampleRate);
    expect(typeof result.loudnessRangeLu).toBe('number');
    expect(Number.isFinite(result.loudnessRangeLu)).toBe(true);
    expect(result).not.toHaveProperty('loudnessRangeDb');
  });

  it('analyzeDynamics leaves the loudness range absent on a half-second signal', () => {
    const result = analyzeDynamics(triad(), sampleRate);
    expect('loudnessRangeLu' in result).toBe(false);
    expect(Number.isFinite(result.peakDb)).toBe(true);
  });

  it('analyzeDynamics reports the loudness curve timestamps in seconds', () => {
    const result = analyzeDynamics(triad(), sampleRate);
    expect(result.loudnessTimesSec).toBeInstanceOf(Float32Array);
    expect(result.loudnessTimesSec.length).toBe(result.loudnessRmsDb.length);
    expect(result).not.toHaveProperty('loudnessTimes');
    expectTypeOf<sonare.DynamicsResult>().toHaveProperty('loudnessTimesSec');
  });

  it('analyzeMelody reports seconds and Hz, and leaves an unvoiced frequency absent', () => {
    const result = analyzeMelody(new Float32Array(sampleRate / 2), sampleRate);
    // Silence has no voiced frame, so the mean frequency is unmeasured and absent.
    expect(result).not.toHaveProperty('meanFrequencyHz');
    if ('vibratoRateHz' in result) {
      expect(Number.isFinite(result.vibratoRateHz)).toBe(true);
    }
    expect(result.points.length).toBeGreaterThan(0);
    for (const point of result.points) {
      expect(typeof point.timeSec).toBe('number');
      expect(point).not.toHaveProperty('time');
      expect(point).not.toHaveProperty('frequency');
      if ('frequencyHz' in point) {
        expect(point.frequencyHz).toBeGreaterThan(0);
      }
    }
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

  it('detectBoundaries and detectChords report *Sec fields', () => {
    const boundaries = detectBoundaries({ samples: triad(2), sampleRate });
    for (const boundary of boundaries.boundaries) {
      expect(typeof boundary.timeSec).toBe('number');
      expect(boundary).not.toHaveProperty('time');
    }
    for (const chord of detectChords({ samples: triad(1), sampleRate }).chords) {
      expect(chord).toHaveProperty('startSec');
      expect(chord).toHaveProperty('endSec');
      expect(chord).toHaveProperty('durationSec');
      expect(chord).not.toHaveProperty('start');
      expect(CHORD_QUALITIES).toContain(chord.quality);
    }
  });
});

describe('default tempo range', () => {
  it('analyzeBpm searches 60-200 BPM unless told otherwise', () => {
    const samples = triad();
    expect(analyzeBpm(samples, sampleRate).bpm).toBe(
      analyzeBpm(samples, sampleRate, { bpmMin: 60, bpmMax: 200 }).bpm,
    );
  });
});

describe('analyze progress', () => {
  it('reports progress through the request and the options bag', () => {
    const samples = triad();
    const viaRequest: number[] = [];
    analyze({ samples, sampleRate, onProgress: (progress) => viaRequest.push(progress) });
    const viaOptions: number[] = [];
    analyze(samples, sampleRate, { onProgress: (progress) => viaOptions.push(progress) });
    expect(viaRequest.length).toBeGreaterThan(0);
    expect(viaOptions).toEqual(viaRequest);
  });

  it('refuses a non-function callback', () => {
    expect(
      errorOf(() => analyze({ samples: triad(0.25), sampleRate, onProgress: 1 as never })),
    ).toBeInstanceOf(TypeError);
  });
});

describe('2-D results are Matrix2D', () => {
  const samples = triad();

  it.each([
    ['nnlsChroma', () => nnlsChroma(samples, sampleRate), 12],
    ['onsetStrengthMulti', () => onsetStrengthMulti(samples, sampleRate), 3],
    ['onsetStrengthMulti nBands', () => onsetStrengthMulti({ samples, sampleRate, nBands: 2 }), 2],
  ] as const)('%s has rows = bands/bins and cols = frames', (_name, call, rows) => {
    const matrix = call();
    expect(matrix.rows).toBe(rows);
    expect(matrix.data.length).toBe(matrix.rows * matrix.cols);
    expect(Object.keys(matrix).sort()).toEqual(['cols', 'data', 'rows']);
  });

  it('fourierTempogram takes a request and returns Matrix2D', () => {
    const envelope = onsetStrength(samples, sampleRate);
    const matrix = fourierTempogram({ onsetEnvelope: envelope, sampleRate, winLength: 32 });
    expect(matrix.data.length).toBe(matrix.rows * matrix.cols);
    expect(
      errorOf(() => fourierTempogram({ onsetEnvelope: envelope, norm: true } as never)),
    ).toBeInstanceOf(TypeError);
  });

  it('tempogramRatio takes the tempogram as a matrix', () => {
    const rows = 16;
    const cols = 8;
    const ratios = tempogramRatio({
      tempogram: { data: new Float32Array(rows * cols).fill(0.5), rows, cols },
    });
    expect(ratios).toBeInstanceOf(Float32Array);
    expect(
      errorOf(() => tempogramRatio({ tempogramData: new Float32Array(8), winLength: 4 } as never)),
    ).toBeInstanceOf(TypeError);
  });

  it('cqt and vqt round-trip through the matrix inverses', () => {
    const forward = cqt({ samples, sampleRate, nBins: 24, fminHz: 65.4 });
    expect(forward.magnitude.data.length).toBe(forward.magnitude.rows * forward.magnitude.cols);
    const audio = cqtToAudio({
      magnitude: forward.magnitude,
      sampleRate,
      fminHz: 65.4,
      nIter: 2,
    });
    expect(audio).toBeInstanceOf(Float32Array);
    const variable = vqt({ samples, sampleRate, nBins: 24, fminHz: 65.4 });
    expect(
      vqtToAudio({ magnitude: variable.magnitude, sampleRate, fminHz: 65.4, nIter: 2 }),
    ).toBeInstanceOf(Float32Array);
  });

  it.each([
    [
      'cqtToAudio',
      'magnitude length',
      () => cqtToAudio({ magnitude: { data: new Float32Array(5), rows: 2, cols: 3 }, sampleRate }),
    ],
  ])('%s refuses a matrix whose data length is not rows * cols', (_name, _what, call) => {
    expect(errorOf(call)).toBeInstanceOf(RangeError);
  });

  it('refuses the old bare-array magnitude', () => {
    expect(
      errorOf(() =>
        cqtToAudio({ magnitude: new Float32Array(16), sampleRate, nBins: 4, nFrames: 4 } as never),
      ),
    ).toBeInstanceOf(TypeError);
  });
});

describe('meter and chord function inputs', () => {
  const beats = Array.from({ length: 16 }, (_, i) => ({
    timeSec: i * 0.5,
    strength: i % 4 === 0 ? 1 : 0.3,
  }));

  it('estimateMeter takes beats with timeSec and strength', () => {
    const meter = estimateMeter({ beats });
    expect(meter.timeSignature.numerator).toBeGreaterThan(0);
    expect(
      errorOf(() => estimateMeter({ beatTimes: [0, 1], beatStrengths: [1, 1] } as never)),
    ).toBeInstanceOf(TypeError);
    expect(errorOf(() => estimateMeter({ beats: 3 } as never))).toBeInstanceOf(TypeError);
  });

  it('chordFunctions labels with romanNumeral and reads quality tokens only', () => {
    const labelled = chordFunctions({
      chords: [{ root: 0, quality: 'major' }],
      key: { root: 0, mode: 'major' },
    });
    expect(labelled[0].romanNumeral).toBe('I');
    expect(labelled[0]).not.toHaveProperty('roman');
    expect(
      errorOf(() =>
        chordFunctions({
          chords: [{ root: 0, quality: 0 as never }],
          key: { root: 0, mode: 'major' },
        }),
      ),
    ).toBeInstanceOf(TypeError);
    expect(
      errorOf(() =>
        chordFunctions({
          chords: [{ root: 0, quality: 'major' }],
          key: { root: 'C' as never, mode: 'major' },
        }),
      ),
    ).toBeInstanceOf(TypeError);
  });

  it('chordSymbolFromAnalysis reads a quality token and refuses an ordinal', () => {
    const symbol = chordSymbolFromAnalysis({ root: 0, bass: 0, quality: 'major' });
    expect(symbol).toHaveProperty('rootPc', 0);
    expect(
      errorOf(() => chordSymbolFromAnalysis({ root: 0, bass: 0, quality: 0 as never })),
    ).toBeInstanceOf(TypeError);
  });
});

describe('chord quality tokens', () => {
  // The native enum is read through the source module, which keeps its own instance.
  beforeAll(async () => {
    await initSource();
  });

  it('has a token for every ordinal the native side can emit', () => {
    const native = (
      getSonareModule() as unknown as { ChordQuality: Record<string, { value: number }> }
    ).ChordQuality;
    // An embind enum also carries a `values` map beside its members.
    const ordinals = Object.values(native)
      .filter((entry) => typeof entry?.value === 'number')
      .map((entry) => entry.value);
    expect(ordinals.length).toBe(CHORD_QUALITIES.length);
    for (const ordinal of ordinals) {
      expect(typeof CHORD_QUALITIES[ordinal]).toBe('string');
    }
    expect(new Set(CHORD_QUALITIES).size).toBe(CHORD_QUALITIES.length);
    expect(CHORD_QUALITIES[0]).toBe('major');
    expect(CHORD_QUALITIES[9]).toBe('unknown');
  });
});

describe('polyphonic analysis and transcription', () => {
  it('analyzePolyphonic takes the positional form and a bare setNoteEdit clears the edit', () => {
    const samples = triad();
    const analysis = analyzePolyphonic(samples, sampleRate);
    try {
      expect(analysis.noteCount).toBe(analyzePolyphonic({ samples, sampleRate }).noteCount);
      if (analysis.noteCount > 0) {
        analysis.setNoteEdit(0, { gainDb: 3 });
        analysis.setNoteEdit(0);
        expect(analysis.notes()[0].edit.gainDb ?? 0).toBe(0);
      }
    } finally {
      analysis.destroy();
    }
  });

  it.each([
    [
      'analyzePolyphonic',
      (extra: object) => api.analyzePolyphonic({ samples: triad(), sampleRate, ...extra }),
    ],
    ['transcribe', (extra: object) => api.transcribe({ samples: triad(), sampleRate, ...extra })],
  ])('%s refuses reattackRatio 0 and a negative ratio', (_name, call) => {
    expect(errorOf(() => call({ reattackRatio: 0 }))).toBeInstanceOf(RangeError);
    expect(errorOf(() => call({ minFramePeakRatio: -0.1 }))).toBeInstanceOf(RangeError);
  });

  it('transcribe reports the requested tempo and refuses a validate key', () => {
    const result = transcribe({ samples: triad(), sampleRate, tempoBpm: 100 });
    expect(result.tempoBpm).toBe(100);
    expect(() =>
      transcribe({ samples: triad(), sampleRate, tempoBpm: 100, validate: false } as never),
    ).toThrow(unknownKeyText('transcribe: unknown key validate'));
  });
});

describe('types', () => {
  it('result and request shapes', () => {
    expectTypeOf(detectKey).returns.toHaveProperty('tuningCents');
    expectTypeOf<ReturnType<typeof analyze>['beats'][number]>().toHaveProperty('timeSec');
    expectTypeOf<ReturnType<typeof analyze>['downbeats']>().toEqualTypeOf<
      ReturnType<typeof analyze>['beats']
    >();
    expectTypeOf<ReturnType<typeof nnlsChroma>>().toEqualTypeOf<sonare.Matrix2D>();
    expectTypeOf<ReturnType<typeof onsetStrengthMulti>>().toEqualTypeOf<sonare.Matrix2D>();
    expectTypeOf<ReturnType<typeof fourierTempogram>>().toEqualTypeOf<sonare.Matrix2D>();
    expectTypeOf<sonare.ChordQuality>().toEqualTypeOf<(typeof CHORD_QUALITIES)[number]>();
    expectTypeOf<sonare.SectionType>().toEqualTypeOf<
      'intro' | 'verse' | 'preChorus' | 'chorus' | 'bridge' | 'instrumental' | 'outro' | 'unknown'
    >();
    expectTypeOf<sonare.KeyProfile>().toEqualTypeOf<
      | 'krumhanslSchmuckler'
      | 'temperley'
      | 'shaath'
      | 'faraldoEdmt'
      | 'faraldoEdma'
      | 'faraldoEdmm'
      | 'bellmanBudge'
    >();
    expectTypeOf<sonare.GenreHint>().toEqualTypeOf<
      'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz'
    >();
    expectTypeOf<sonare.TempogramMode>().toEqualTypeOf<'autocorrelation' | 'cosine'>();
    // @ts-expect-error removed alias
    type NoDetectKeyOptions = sonare.DetectKeyOptions;
    // @ts-expect-error removed alias
    type NoDynamicsAnalysisResult = sonare.DynamicsAnalysisResult;
    // @ts-expect-error removed alias
    type NoPitch = sonare.Pitch;
    // @ts-expect-error removed alias
    type NoScaleName = sonare.ScaleName;
    // @ts-expect-error pitch-class inputs take the number only
    type NoPitchClassName = sonare.PitchClassName;
    // @ts-expect-error profile synonyms live in no type any more
    type NoKeyProfileName = sonare.KeyProfileName;
    // @ts-expect-error renamed AnalysisBpmCandidate
    type NoBpmHypothesis = sonare.BpmHypothesis;
    expectTypeOf<sonare.BpmCandidate>().toEqualTypeOf<{ bpm: number; confidence: number }>();
    expectTypeOf<
      sonare.AnalysisBpmCandidate['relation']
    >().toEqualTypeOf<sonare.BpmCandidateRelation>();
    expectTypeOf<sonare.AnalysisResult['bpmCandidates']>().toEqualTypeOf<
      sonare.AnalysisBpmCandidate[]
    >();
    // @ts-expect-error split into per-function requests
    type NoSamplesRequest = sonare.SamplesRequest;
    // @ts-expect-error progress moved onto AnalyzeRequest
    type NoAnalyzeWithProgressRequest = sonare.AnalyzeWithProgressRequest;
    // @ts-expect-error renamed AnalyzeRequest
    type NoMusicAnalyzeRequest = sonare.MusicAnalyzeRequest;
    // @ts-expect-error renamed OnsetStrengthRequest
    type NoOnsetEnvelopeRequest = sonare.OnsetEnvelopeRequest;
    // @ts-expect-error renamed RhythmResult
    type NoRhythmAnalysisResult = sonare.RhythmAnalysisResult;
    // @ts-expect-error renamed TimbreResult
    type NoTimbreAnalysisResult = sonare.TimbreAnalysisResult;
    expectTypeOf<sonare.GrooveType>().toEqualTypeOf<'straight' | 'shuffle' | 'swing'>();
    expectTypeOf<sonare.BpmCandidateRelation>().toEqualTypeOf<
      'primary' | 'half' | 'double' | 'other'
    >();
    expectTypeOf<sonare.NormType>().toEqualTypeOf<'max' | 'l1' | 'l2' | 'power'>();
    expectTypeOf<sonare.ChordChromaMethod>().toEqualTypeOf<'stft' | 'nnls' | 'cqt'>();
    expectTypeOf<sonare.ChordSymbolFields>().toHaveProperty('rootPc');
    expectTypeOf<sonare.AnalysisMelody>().toHaveProperty('meanFrequencyHz');
    expectTypeOf<sonare.AnalysisDynamics>().toHaveProperty('loudnessRangeLu');
    expectTypeOf<sonare.AnalysisRhythm>().toHaveProperty('grooveType');
    expectTypeOf<sonare.MelodyPoint>().toEqualTypeOf<{
      timeSec: number;
      frequencyHz?: number;
      confidence: number;
    }>();
    void (undefined as unknown as [
      NoDetectKeyOptions,
      NoDynamicsAnalysisResult,
      NoPitch,
      NoScaleName,
      NoPitchClassName,
      NoKeyProfileName,
      NoBpmHypothesis,
      NoSamplesRequest,
      NoAnalyzeWithProgressRequest,
      NoMusicAnalyzeRequest,
      NoOnsetEnvelopeRequest,
      NoRhythmAnalysisResult,
      NoTimbreAnalysisResult,
    ]);
    // @ts-expect-error sampleRate is required on every sample-taking request
    const missingRate: sonare.AnalyzeRequest = { samples: new Float32Array(1) };
    void missingRate;
    const named: sonare.DetectChordsRequest = {
      samples: new Float32Array(1),
      sampleRate,
      // @ts-expect-error the pitch-class names are gone: keyRoot is a number
      keyRoot: 'C',
    };
    void named;
  });
});

/** A click train at 120 BPM, two seconds long. */
function clicks(): Float32Array {
  const out = new Float32Array(sampleRate * 2);
  for (let beat = 0; beat < 4; beat++) {
    out[Math.round(beat * 0.5 * sampleRate)] = 1;
  }
  return out;
}

describe('tempo range bounds on the one-shot detectors', () => {
  it.each([
    ['detectBpm', (extra: object) => detectBpm({ samples: clicks(), sampleRate, ...extra })],
    ['detectBeats', (extra: object) => detectBeats({ samples: clicks(), sampleRate, ...extra })],
  ])('%s takes bpmMin and bpmMax and refuses an inverted range', (_name, call) => {
    expect(() => call({ bpmMin: 60, bpmMax: 200 })).not.toThrow();
    expect(() => call({ bpmMin: 200, bpmMax: 60 })).toThrow(RangeError);
  });

  it('detectBpm searches 60-200 BPM by default', () => {
    const samples = clicks();
    expect(detectBpm({ samples, sampleRate })).toBe(
      detectBpm({ samples, sampleRate, bpmMin: 60, bpmMax: 200 }),
    );
  });

  it('detectBeats searches 60-200 BPM by default', () => {
    const samples = clicks();
    expect(detectBeats({ samples, sampleRate })).toEqual(
      detectBeats({ samples, sampleRate, bpmMin: 60, bpmMax: 200 }),
    );
  });
  it('Audio tempo methods search 60-200 BPM by default', () => {
    // One pulse every 1.5 s: 40 BPM, outside 60-200.
    const samples = new Float32Array(sampleRate * 12);
    for (let t = 0; t < 12; t += 1.5) {
      const start = Math.round(t * sampleRate);
      for (let i = 0; i < 300 && start + i < samples.length; i++) {
        samples[start + i] = Math.exp(-i / 50) * Math.sin(i * 0.9);
      }
    }
    const audio = Audio.fromBuffer(samples, sampleRate);
    const range = { bpmMin: 60, bpmMax: 200 };
    expect(audio.detectBpm()).toBe(detectBpm({ samples, sampleRate, ...range }));
    // The default estimate lies above 59 BPM, so a 30-59 search answers inside that range only
    // if the bounds reach the core.
    const below = { bpmMin: 30, bpmMax: 59 };
    const constrained = detectBpm({ samples, sampleRate, ...below });
    expect(audio.detectBpm()).toBeGreaterThan(below.bpmMax);
    expect(constrained).toBeGreaterThanOrEqual(below.bpmMin);
    expect(constrained).toBeLessThanOrEqual(below.bpmMax);
    expect(audio.detectBpm(below)).toBe(constrained);
    expect(audio.detectBeats()).toEqual(detectBeats({ samples, sampleRate, ...range }));
    expect(audio.analyzeBpm().bpm).toBe(analyzeBpm({ samples, sampleRate, ...range }).bpm);
    expect(audio.detectDownbeats()).toEqual(detectDownbeats({ samples, sampleRate }));
  });
});
