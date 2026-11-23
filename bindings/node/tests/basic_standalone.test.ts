import { describe, expect, it } from 'vitest';
import {
  amplitudeToDb,
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeMelody,
  analyzeRhythm,
  analyzeSections,
  analyzeTimbre,
  capabilities,
  capabilityCatalog,
  chordFunctionalAnalysis,
  chordFunctions,
  cqt,
  dbToAmplitude,
  dbToPower,
  deemphasis,
  detectBeats,
  detectBpm,
  detectChords,
  detectKey,
  detectOnsets,
  ErrorCode,
  fixFrames,
  fixLength,
  frameSignal,
  framesToSamples,
  hybridCqt,
  masteringAnalyzePair,
  masteringAnalyzeStereo,
  masteringApplyPairProcessor,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringPairAnalysisNames,
  masteringPairProcessorNames,
  masteringStereoAnalysisNames,
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
  meteringCrestFactorDb,
  meteringCrestFactorDbStereo,
  onsetStrengthMulti,
  padCenter,
  pcen,
  peakPick,
  plp,
  powerToDb,
  preemphasis,
  pseudoCqt,
  samplesToFrames,
  splitSilence,
  tempogram,
  timeStretch,
  tonnetz,
  trimSilence,
  vectorNormalize,
  version,
  vqt,
} from '../src/index.js';
import { EXPECTED_PROJECT_ABI_VERSION } from '../src/types_project.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

const CHORD_PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const CHORD_QUALITY_UNKNOWN = 'unknown';

describe('standalone functions', () => {
  it('version returns a string', () => {
    const v = version();
    expect(typeof v).toBe('string');
    expect(v.length).toBeGreaterThan(0);
  });

  it('capabilities returns the parsed native capability report', () => {
    const report = capabilities();
    expect(Object.keys(report)).toEqual([
      'version',
      'abi',
      'platform',
      'features',
      'decode',
      'simd',
      'hardwareConcurrency',
    ]);
    expect(report.version).toBe(version());
    expect(report.abi).toEqual({
      project: EXPECTED_PROJECT_ABI_VERSION,
      engine: expect.any(Number),
    });
    expect(report.platform).toEqual(expect.any(String));
    expect(report.features).toEqual({
      mastering: expect.any(Boolean),
      mixing: expect.any(Boolean),
      mixingAssistant: expect.any(Boolean),
      graph: expect.any(Boolean),
      fx: expect.any(Boolean),
      ffmpeg: expect.any(Boolean),
      instrumentParamAutomation: expect.any(Boolean),
      arrangement: expect.any(Boolean),
      project: expect.any(Boolean),
      acousticSim: expect.any(Boolean),
      playback: expect.any(Boolean),
      pitchEditor: expect.any(Boolean),
      vocalEdit: expect.any(Boolean),
      voiceChanger: expect.any(Boolean),
      assist: expect.any(Boolean),
    });
    expect(report.decode.builtin).toEqual(['wav', 'mp3']);
    expect(Array.isArray(report.decode.ffmpeg)).toBe(true);
    expect(report.simd).toEqual(expect.any(String));
    expect(report.hardwareConcurrency).toBeGreaterThan(0);
  });

  it('capabilityCatalog aggregates processors and built-in presets', () => {
    const catalog = capabilityCatalog();
    expect(catalog.version).toBe(version());
    expect(catalog.abi.project).toBe(EXPECTED_PROJECT_ABI_VERSION);
    expect(catalog.processors.length).toBeGreaterThan(0);
    expect(catalog.presets.mastering).toContain('pop');
    expect(Object.keys(catalog.presets).sort()).toEqual([
      'mastering',
      'mixingScene',
      'playbackRoom',
      'synth',
      'voiceChanger',
    ]);
    expect(catalog.presets.playbackRoom).toContain('none');
    const compressor = catalog.processors.find(({ id }) => id === 'dynamics.compressor');
    expect(compressor).toMatchObject({ category: 'dynamics', realtimeInsertable: true });
    expect(compressor?.params[0]).toEqual(
      expect.objectContaining({ name: expect.any(String), type: expect.any(String) }),
    );
  });

  it('detectBpm returns a number for audible input and undefined for silence', () => {
    const bpm = detectBpm(generateSine(220, SR, 2), SR);
    expect(typeof bpm).toBe('number');
    expect(bpm).toBeGreaterThanOrEqual(0);
    expect(detectBpm(new Float32Array(SR), SR)).toBeUndefined();
  });

  it('detectKey returns key object', () => {
    const key = detectKey(new Float32Array(SR), SR);
    expect(typeof key.root).toBe('number');
    expect(typeof key.mode).toBe('string');
    expect(typeof key.confidence).toBe('number');
    expect(typeof key.name).toBe('string');
    expect(typeof key.shortName).toBe('string');
  });

  it('detectBeats returns Float32Array', () => {
    expect(detectBeats(new Float32Array(SR), SR)).toBeInstanceOf(Float32Array);
  });

  it('detectOnsets returns Float32Array', () => {
    expect(detectOnsets(new Float32Array(SR), SR)).toBeInstanceOf(Float32Array);
  });

  it('analyze returns full result', () => {
    const result = analyze(generateSine(220, SR, 2), SR);
    expect(result).toHaveProperty('bpm');
    expect(result).toHaveProperty('key');
    expect(result).toHaveProperty('timeSignature');
    expect(result).toHaveProperty('beats');
    expect(result).toHaveProperty('downbeats');
    expect(Array.isArray(result.beats)).toBe(true);
    expect(Array.isArray(result.downbeats)).toBe(true);
  });

  it('analyze with onProgress reports progress and matches the plain shape', () => {
    const samples = generateSine(220, SR, 2);
    const progress: number[] = [];
    const result = analyze(samples, SR, {
      onProgress: (p, _stage) => {
        progress.push(p);
      },
    });
    expect(result).toHaveProperty('bpm');
    expect(result).toHaveProperty('key');
    expect(result).toHaveProperty('timeSignature');
    expect(Array.isArray(result.beats)).toBe(true);
    expect(progress.length).toBeGreaterThan(0);
    for (const p of progress) {
      expect(p).toBeGreaterThanOrEqual(0);
      expect(p).toBeLessThanOrEqual(1);
    }
  });

  it('analyze ignores an onProgress false result', () => {
    const progress: number[] = [];
    const result = analyze(generateSine(220, SR, 2), SR, {
      onProgress: (value) => {
        progress.push(value);
        return false;
      },
    });

    expect(progress.some((value) => value > 0.5)).toBe(true);
    expect(Array.isArray(result.beats)).toBe(true);
  });

  it('analyze cancels through the request cancel callback', () => {
    expect(() =>
      analyze({
        samples: generateSine(220, SR, 2),
        sampleRate: SR,
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });

  it('analyzeSections returns the sections and the form they spell', () => {
    const samples = generateSine(220, SR, 4);
    const { sections, form } = analyzeSections(samples, SR);
    expect(Array.isArray(sections)).toBe(true);
    expect(form).toHaveLength(sections.length);
    for (const section of sections) {
      expect(typeof section.type).toBe('string');
      expect(typeof section.name).toBe('string');
      expect(section.endSec).toBeGreaterThanOrEqual(section.startSec);
    }
  });

  it('analyzeMelody returns a melody contour', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR);
    expect(Array.isArray(melody.points)).toBe(true);
    expect(typeof melody.meanFrequencyHz).toBe('number');
    expect(typeof melody.pitchStability).toBe('number');
    for (const point of melody.points) {
      expect(typeof point.timeSec).toBe('number');
      expect(typeof point.frequencyHz).toBe('number');
    }
  });

  it('cqt and vqt return magnitude matrices', () => {
    const samples = generateSine(220, SR, 1);
    const cqtResult = cqt(samples, SR);
    expect(cqtResult.magnitude.rows).toBe(84);
    expect(cqtResult.magnitude.cols).toBeGreaterThan(0);
    expect(cqtResult.magnitude.data.length).toBe(
      cqtResult.magnitude.rows * cqtResult.magnitude.cols,
    );
    expect(cqtResult.frequencies.length).toBe(cqtResult.magnitude.rows);

    const vqtResult = vqt(samples, SR);
    expect(vqtResult.magnitude.rows).toBe(84);
    expect(vqtResult.magnitude.data.length).toBe(
      vqtResult.magnitude.rows * vqtResult.magnitude.cols,
    );
    expect(vqtResult.frequencies.length).toBe(vqtResult.magnitude.rows);
  });

  it('pseudoCqt and hybridCqt return magnitude matrices', () => {
    const samples = generateSine(220, SR, 1);
    const cqtOptions = { hopLength: 512, fminHz: 32.70319566257483, nBins: 24, binsPerOctave: 12 };
    for (const result of [pseudoCqt(samples, SR, cqtOptions), hybridCqt(samples, SR, cqtOptions)]) {
      expect(result.magnitude.rows).toBe(24);
      expect(result.magnitude.cols).toBeGreaterThan(0);
      expect(result.magnitude.data.length).toBe(result.magnitude.rows * result.magnitude.cols);
      expect(result.frequencies.length).toBe(result.magnitude.rows);
    }
  });

  it('onsetStrengthMulti returns a band matrix', () => {
    const samples = generateSine(440, SR, 1);
    const result = onsetStrengthMulti(samples, SR, {
      nFft: 2048,
      hopLength: 512,
      nMels: 64,
      nBands: 4,
    });
    expect(result.rows).toBe(4);
    expect(result.cols).toBeGreaterThan(0);
    expect(result.data.length).toBe(result.rows * result.cols);
  });

  it('analysis primitives expose detailed BPM and rhythm data', () => {
    const samples = new Float32Array(SR * 4);
    const samplesPerBeat = (SR * 60) / 120;
    for (let beat = 0; beat < 8; beat++) {
      const start = Math.floor(beat * samplesPerBeat);
      for (let i = start; i < Math.min(start + 200, samples.length); i++) {
        samples[i] = 1.0;
      }
    }

    const bpm = analyzeBpm(samples, SR);
    expect(bpm.bpm).toBeGreaterThan(0);
    expect(bpm.confidence).toBeGreaterThanOrEqual(0);
    expect(bpm.candidates.length).toBeLessThanOrEqual(5);
    expect(bpm.autocorrelation).toBeInstanceOf(Float32Array);
    expect(bpm.tempogram).toBeInstanceOf(Float32Array);

    const rhythm = analyzeRhythm(samples, SR);
    expect(rhythm.bpm).toBeGreaterThan(0);
    expect(rhythm.timeSignature.numerator).toBeGreaterThan(0);
    expect(['straight', 'shuffle', 'swing']).toContain(rhythm.grooveType);
    expect(rhythm.beatIntervals).toBeInstanceOf(Float32Array);

    const dynamics = analyzeDynamics(samples, SR);
    expect(dynamics.peakDb).toBeLessThanOrEqual(1);
    expect(dynamics.loudnessTimesSec).toBeInstanceOf(Float32Array);
    expect(dynamics.loudnessRmsDb.length).toBe(dynamics.loudnessTimesSec.length);
    // The addon names the range in LU itself; the facade passes it through unrenamed.
    expect(Number.isFinite(dynamics.loudnessRangeLu)).toBe(true);
    expect('loudnessRangeDb' in dynamics).toBe(false);

    const tone = new Float32Array(SR * 2);
    for (let i = 0; i < tone.length; i++) {
      tone[i] =
        0.25 *
        (Math.sin((2 * Math.PI * 261.63 * i) / SR) +
          Math.sin((2 * Math.PI * 329.63 * i) / SR) +
          Math.sin((2 * Math.PI * 392.0 * i) / SR));
    }
    const timbre = analyzeTimbre(tone, SR);
    expect(timbre.brightness).toBeGreaterThanOrEqual(0);
    expect(timbre.brightness).toBeLessThanOrEqual(1);
    expect(timbre.spectralCentroid).toBeInstanceOf(Float32Array);

    const detectOptions = {
      minDurationSec: 0.3,
      smoothingWindowSec: 2.0,
      threshold: 0.5,
      useTriadsOnly: false,
      nFft: 2048,
      hopLength: 512,
      useBeatSync: false,
    };
    const chords = detectChords(tone, SR, detectOptions);
    expect(Array.isArray(chords.chords)).toBe(true);
    // The per-chord assertions below say nothing on an empty progression, and
    // this input is a sustained triad, so it must detect at least one chord.
    expect(chords.chords.length).toBeGreaterThan(0);
    for (const chord of chords.chords) {
      expect(chord.rootName).toBe(CHORD_PITCH_NAMES[chord.root]);
      expect(chord.bassName).toBe(CHORD_PITCH_NAMES[chord.bass]);
      // `name` is the core's rendered symbol: root spelling, then the quality
      // suffix, then a slash bass only when the bass differs from the root.
      expect(typeof chord.name).toBe('string');
      expect(chord.name.length).toBeGreaterThan(0);
      if (chord.quality === CHORD_QUALITY_UNKNOWN) {
        expect(chord.name).toBe('N.C.');
      } else {
        expect(chord.name.startsWith(chord.rootName)).toBe(true);
        if (chord.bass === chord.root) {
          expect(chord.name).not.toContain('/');
        } else {
          expect(chord.name.endsWith(`/${chord.bassName}`)).toBe(true);
        }
      }
    }

    // Timed entries: one per detected chord, each carrying a numeral and a function,
    // and equal to detectChords followed by chordFunctions.
    const romans = chordFunctionalAnalysis(tone, SR, {
      ...detectOptions,
      keyRoot: 0,
      keyMode: 'major',
    });
    expect(Array.isArray(romans)).toBe(true);
    expect(romans.length).toBe(chords.chords.length);
    for (const entry of romans) {
      expect(entry.romanNumeral.length).toBeGreaterThan(0);
      expect(entry.function.length).toBeGreaterThan(0);
    }
    expect(romans).toEqual(chordFunctions({ chords, key: { root: 0, mode: 'major' } }));
  });

  it('detectChords and chordFunctionalAnalysis give one answer in the positional and request forms', () => {
    const tone = new Float32Array(SR);
    for (let i = 0; i < tone.length; i++) {
      tone[i] =
        0.25 *
        (Math.sin((2 * Math.PI * 261.63 * i) / SR) +
          Math.sin((2 * Math.PI * 329.63 * i) / SR) +
          Math.sin((2 * Math.PI * 392.0 * i) / SR));
    }
    const options = {
      minDurationSec: 0.3,
      smoothingWindowSec: 2.0,
      threshold: 0.5,
      useTriadsOnly: false,
      nFft: 2048,
      hopLength: 512,
      useBeatSync: false,
    };

    const positional = detectChords(tone, SR, options);
    const requestForm = detectChords({ samples: tone, sampleRate: SR, ...options });
    expect(Array.isArray(requestForm.chords)).toBe(true);
    expect(requestForm).toEqual(positional);
    if (positional.chords.length > 0) {
      expect(requestForm.chords[0]?.root).toBe(positional.chords[0]?.root);
      expect(requestForm.chords[0]?.quality).toBe(positional.chords[0]?.quality);
    }

    const keyed = { ...options, keyRoot: 0 as const, keyMode: 'major' as const };
    const romansPositional = chordFunctionalAnalysis(tone, SR, keyed);
    const romansRequest = chordFunctionalAnalysis({ samples: tone, sampleRate: SR, ...keyed });
    expect(Array.isArray(romansRequest)).toBe(true);
    expect(romansRequest).toEqual(romansPositional);
  });

  it('detectChords threshold emits unknown for ambiguous input', () => {
    const silence = new Float32Array(4096);
    const common = {
      minDurationSec: 0,
      smoothingWindowSec: 0.01,
      useTriadsOnly: true,
      nFft: 2048,
      hopLength: 512,
      useBeatSync: false,
    } as const;
    const low = detectChords(silence, SR, { ...common, threshold: 0 });
    const high = detectChords(silence, SR, { ...common, threshold: 0.99 });
    expect(low.chords).toHaveLength(1);
    expect(low.chords[0]?.quality).not.toBe(CHORD_QUALITY_UNKNOWN);
    expect(high.chords).toHaveLength(1);
    expect(high.chords[0]?.quality).toBe(CHORD_QUALITY_UNKNOWN);
    expect(high.chords[0]?.startSec).toBe(0);
    expect(high.chords[0]?.endSec).toBeGreaterThan(0);
  });

  it('chordFunctionalAnalysis rejects non-Float32Array input', () => {
    expect(() =>
      chordFunctionalAnalysis(new Float64Array(SR) as unknown as Float32Array, SR, { keyRoot: 0 }),
    ).toThrow(/Float32Array/);
  });

  it('rejects non-Float32Array input', () => {
    expect(() => detectBpm(new Float64Array(SR) as unknown as Float32Array, SR)).toThrow(
      /Float32Array/,
    );
  });

  it('converts invalid native arguments into JS exceptions', () => {
    expect(() => timeStretch(new Float32Array(SR), -1, { speedRatio: 2.0 })).toThrow(
      /sampleRate must be an integer in \[8000, 384000\]/,
    );
  });

  it('exposes compatibility numeric and signal utilities', () => {
    expect(framesToSamples(4, 512)).toBe(2048);
    expect(samplesToFrames(2048, 512)).toBe(4);

    const powerDb = powerToDb({
      values: new Float32Array([1, 0.01]),
      ref: 1,
      amin: 1e-10,
      topDb: 80,
    });
    expect(powerDb[0]).toBeCloseTo(0, 5);
    expect(powerDb[1]).toBeCloseTo(-20, 4);
    expect(dbToPower({ values: powerDb, ref: 1 })[1]).toBeCloseTo(0.01, 5);

    const ampDb = amplitudeToDb({
      values: new Float32Array([1, 0.5]),
      ref: 1,
      amin: 1e-5,
      topDb: 80,
    });
    expect(ampDb[0]).toBeCloseTo(0, 5);
    expect(dbToAmplitude({ values: ampDb, ref: 1 })[1]).toBeCloseTo(0.5, 5);

    const emphasized = preemphasis(new Float32Array([1, 1, 1]), SR, { coef: 0.5, zi: 0 });
    expect(Array.from(emphasized)).toEqual([1, 0.5, 0.5]);
    const deemphasized = deemphasis(emphasized, SR, { coef: 0.5, zi: 0 });
    expect(deemphasized[2]).toBeCloseTo(1, 5);

    const framed = frameSignal(new Float32Array([1, 2, 3, 4]), SR, {
      frameLength: 2,
      hopLength: 1,
    });
    expect(framed.rows).toBe(2);
    expect(framed.cols).toBe(3);
    expect(Array.from(framed.data)).toEqual([1, 2, 2, 3, 3, 4]);
    expect(
      Array.from(fixLength({ values: new Float32Array([1, 2]), targetSize: 4, padValue: -1 })),
    ).toEqual([1, 2, -1, -1]);
    expect(() => padCenter({ values: new Float32Array([1, 2]), targetSize: -1 })).toThrow(
      RangeError,
    );
    expect(() => fixLength({ values: new Float32Array([1, 2]), targetSize: -1 })).toThrow(
      RangeError,
    );
    expect(
      Array.from(fixFrames({ frames: new Int32Array([2, 4]), xMin: 0, xMax: 5, pad: true })),
    ).toEqual([0, 2, 4, 5]);
    // Matches librosa.util.peak_pick exactly (index 0 is a peak under its
    // first-frame rule: x[0] >= max/mean of the leading window).
    expect(
      Array.from(
        peakPick({
          values: new Float32Array([0, 1, 0, 2, 0]),
          preMax: 1,
          postMax: 1,
          preAvg: 1,
          postAvg: 1,
          delta: 0,
          wait: 0,
        }),
      ),
    ).toEqual([0, 1, 3]);

    const normalized = vectorNormalize({
      values: new Float32Array([3, 4]),
      norm: 'l2',
      threshold: 1e-12,
    });
    expect(normalized[0]).toBeCloseTo(0.6, 5);
    expect(normalized[1]).toBeCloseTo(0.8, 5);
  });

  it('exposes silence and rhythm compatibility utilities', () => {
    const samples = new Float32Array([0, 0, 1, 1, 0, 0]);
    const trimmed = trimSilence(samples, SR, { topDb: 20, frameLength: 2, hopLength: 1 });
    expect(trimmed.audio.length).toBeGreaterThan(0);
    expect(trimmed.startSample).toBeGreaterThanOrEqual(0);
    expect(trimmed.endSample).toBeGreaterThan(trimmed.startSample);
    expect(splitSilence(samples, SR, { topDb: 20, frameLength: 2, hopLength: 1 })).toBeInstanceOf(
      Int32Array,
    );

    const pcenValues = pcen({ values: { data: new Float32Array([1, 2, 3, 4]), rows: 2, cols: 2 } });
    expect(pcenValues.data).toBeInstanceOf(Float32Array);
    expect(pcenValues.data.length).toBe(4);

    const chromaValues = new Float32Array(12 * 2);
    chromaValues[0] = 1;
    chromaValues[12] = 1;
    const tonnetzValues = tonnetz({ chromagram: { data: chromaValues, rows: 12, cols: 2 } });
    expect(tonnetzValues.data).toBeInstanceOf(Float32Array);
    expect(tonnetzValues.rows).toBe(6);
    expect(tonnetzValues.data.length).toBe(12);

    const onset = new Float32Array([0, 1, 0, 1, 0, 1, 0, 1]);
    const temp = tempogram({ onsetEnvelope: onset, sampleRate: SR, hopLength: 512, winLength: 4 });
    expect(temp.data).toBeInstanceOf(Float32Array);
    expect(temp.rows).toBe(4);
    expect(
      plp({
        onsetEnvelope: onset,
        sampleRate: SR,
        hopLength: 512,
        bpmMin: 30,
        bpmMax: 300,
        winLength: 4,
      }),
    ).toBeInstanceOf(Float32Array);
  });

  it('exposes pair and stereo mastering APIs', () => {
    const sampleRate = 44100;
    const source = generateSine(440, sampleRate, 0.25);
    const reference = generateSine(880, sampleRate, 0.25);

    expect(masteringPairProcessorNames()).toContain('match.abCrossfade');
    expect(masteringPairAnalysisNames()).toContain('match.referenceLoudness');
    expect(masteringStereoAnalysisNames()).toContain('stereo.monoCompatCheck');

    const paired = masteringApplyPairProcessor({
      processor: 'match.abCrossfade',
      source,
      reference,
      sampleRate,
      params: { mix: 0.25 },
    });
    expect(paired.samples).toBeInstanceOf(Float32Array);
    expect(paired.samples.length).toBe(source.length);

    const pairResult = masteringAnalyzePair({
      analysis: 'match.referenceLoudness',
      source,
      reference,
      sampleRate,
    });
    expect(pairResult).toHaveProperty('sourceLufs');
    expect(pairResult).toHaveProperty('referenceLufs');

    const stereoResult = masteringAnalyzeStereo({
      analysis: 'stereo.monoCompatCheck',
      left: source,
      right: reference,
      sampleRate,
    });
    expect(stereoResult).toHaveProperty('correlation');
  });

  it('accepts independent source/reference lengths for pair mastering', () => {
    const sampleRate = 44100;
    const source = generateSine(440, sampleRate, 0.25);
    const reference = generateSine(880, sampleRate, 0.6); // different duration

    const paired = masteringApplyPairProcessor({
      processor: 'match.abCrossfade',
      source,
      reference,
      sampleRate,
      params: { mix: 0.25 },
    });
    expect(paired.samples).toBeInstanceOf(Float32Array);
    expect(paired.samples.length).toBe(source.length);

    const pairResult = masteringAnalyzePair({
      analysis: 'match.referenceLoudness',
      source,
      reference,
      sampleRate,
    });
    expect(pairResult).toHaveProperty('sourceLufs');
    expect(pairResult).toHaveProperty('referenceLufs');
  });

  it('exposes mastering assistant suggestions', () => {
    const sampleRate = 22050;
    const suggest = (samples: Float32Array) =>
      masteringAssistantSuggest(samples, sampleRate, {
        targetLufs: -13,
        ceilingDbtp: -0.8,
        enableRepair: true,
      });

    const clean = new Float32Array(sampleRate * 3);
    for (let i = 0; i < clean.length; i++) {
      clean[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
    }
    const clicked = Float32Array.from(clean);
    const step = Math.floor(clicked.length / 5);
    for (let k = 1; k <= 4; k++) {
      clicked[k * step] = 0.9;
    }

    const result = suggest(clean);
    expect(result).toHaveProperty('chainConfig');
    expect(result).toHaveProperty('profile');
    expect(Array.isArray(result.explanation)).toBe(true);
    expect(result).not.toHaveProperty('genreCandidates');
    expect(result.chainConfig.loudness?.targetLufs).toBe(-13);
    expect(result.chainConfig.loudness?.ceilingDbtp).toBeCloseTo(-0.8, 6);

    // enableRepair asks for repair; the measured defect profile picks which stages
    // run. Only the clean side would still pass with enableRepair ignored outright,
    // so both are driven. Booleans serialize as JSON true/false (RFC 8259).
    expect(result.chainConfig.repair?.declick?.enabled).toBe(false);
    expect(suggest(clicked).chainConfig.repair?.declick?.enabled).toBe(true);
  });

  it('exposes mastering audio profiles', () => {
    const sampleRate = 22050;
    const samples = generateSine(330, sampleRate, 2);
    const result = masteringAudioProfile(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
    });

    expect(typeof result.durationSec).toBe('number');
    expect(result.durationSec).toBeGreaterThan(1.9);
    expect(result).toHaveProperty('loudness.integratedLufs');
    expect(result).toHaveProperty('spectral.centroidHz');
    expect(result).toHaveProperty('dynamics.attackPerSecond');
    expect(result).not.toHaveProperty('genreCandidates');
  });

  it('exposes streaming platform loudness previews', () => {
    const sampleRate = 22050;
    const samples = generateSine(440, sampleRate, 1);
    const result = masteringStreamingPreview(samples, sampleRate, {
      platforms: [{ name: 'Unit Test', targetLufs: -12, ceilingDbtp: -1 }],
    });

    expect(result.platforms).toHaveLength(1);
    expect(result.platforms[0].name).toBe('Unit Test');
    expect(typeof result.platforms[0].integratedLufs).toBe('number');
    expect(typeof result.platforms[0].truePeakDbtp).toBe('number');
    expect(typeof result.platforms[0].normalizationGainDb).toBe('number');
    expect(typeof result.platforms[0].ceilingRisk).toBe('boolean');
  });

  it('measures a stereo pair with channel summing, not a downmix', () => {
    const sampleRate = 48000;
    const left = generateSine(1000, sampleRate, 4);
    const right = generateSine(1731, sampleRate, 4);
    const downmix = new Float32Array(left.length);
    for (let i = 0; i < left.length; i += 1) {
      downmix[i] = 0.5 * (left[i] + right[i]);
    }

    const stereo = masteringStreamingPreviewStereo({ left, right, sampleRate });
    const mono = masteringStreamingPreview(downmix, sampleRate);
    const stereoLufs = stereo.platforms[0].integratedLufs as number;
    const monoLufs = mono.platforms[0].integratedLufs as number;

    expect(stereo.platforms[0].name).toBe('Spotify');
    // BS.1770 sums the channel powers while the 0.5*(L+R) downmix quarters
    // them, so a decorrelated pair reads 6.02 dB above what a mono caller can
    // measure, and the normalization gain built on it is off by the same.
    expect(stereoLufs - monoLufs).toBeCloseTo(6.02, 1);
    expect(stereo.platforms[0].normalizationGainDb).toBeCloseTo(-14 - stereoLufs, 3);

    const profile = masteringAudioProfileStereo({ left, right, sampleRate });
    expect(profile.loudness.integratedLufs).toBeCloseTo(stereoLufs, 2);
    expect(profile).toHaveProperty('spectral.centroidHz');

    const suggestion = masteringAssistantSuggestStereo({ left, right, sampleRate });
    expect(suggestion).toHaveProperty('chainConfig');
    expect(Array.isArray(suggestion.explanation)).toBe(true);
  });

  it('keeps a crest factor where the downmix cancels', () => {
    const sampleRate = 48000;
    const left = generateSine(440, sampleRate, 1);
    const right = new Float32Array(left.length);
    const downmix = new Float32Array(left.length);
    for (let i = 0; i < left.length; i += 1) {
      right[i] = -left[i];
      downmix[i] = 0.5 * (left[i] + right[i]);
    }

    expect(meteringCrestFactorDbStereo({ left, right, sampleRate })).toBeCloseTo(3.01, 1);
    // The anti-phase pair cancels to silence in the downmix, which has no crest
    // factor at all rather than the pair's real 3 dB.
    expect(meteringCrestFactorDb(downmix, sampleRate)).toBeUndefined();
  });

  it('rejects a stereo pair whose channel lengths differ', () => {
    const sampleRate = 48000;
    const left = generateSine(440, sampleRate, 1);
    const right = left.subarray(0, left.length - 1);
    expect(() => masteringStreamingPreviewStereo({ left, right, sampleRate })).toThrow();
    expect(() => masteringAudioProfileStereo({ left, right, sampleRate })).toThrow();
    expect(() => masteringAssistantSuggestStereo({ left, right, sampleRate })).toThrow();
    expect(() => meteringCrestFactorDbStereo({ left, right, sampleRate })).toThrow();
  });
});

describe('chord entry shape', () => {
  // The same field set and types come from detectChords and analyze, on Node and WASM alike.
  const CHORD_FIELDS = [
    'bass',
    'bassName',
    'confidence',
    'durationSec',
    'endSec',
    'name',
    'quality',
    'root',
    'rootName',
    'startSec',
  ];

  it('gives detectChords and analyze chords one ordinal + name shape for a C major triad', () => {
    const sr = 22050;
    const triad = new Float32Array(sr * 4);
    for (let i = 0; i < triad.length; i++) {
      triad[i] =
        0.25 *
        (Math.sin((2 * Math.PI * 261.63 * i) / sr) +
          Math.sin((2 * Math.PI * 329.63 * i) / sr) +
          Math.sin((2 * Math.PI * 392.0 * i) / sr));
    }
    const detected = detectChords(triad, sr).chords;
    const analysed = analyze(triad, sr).chords;
    expect(detected.length).toBeGreaterThan(0);
    expect(analysed.length).toBeGreaterThan(0);
    for (const chord of detected) {
      expect(Object.keys(chord).sort()).toEqual(CHORD_FIELDS);
    }
    for (const chord of analysed) {
      expect(Object.keys(chord).sort()).toEqual([...CHORD_FIELDS, 'romanNumeral'].sort());
    }
    for (const chord of [...detected, ...analysed]) {
      expect(typeof chord.root).toBe('number');
      expect(typeof chord.bass).toBe('number');
      expect(typeof chord.quality).toBe('string');
      expect(chord.rootName).toBe(CHORD_PITCH_NAMES[chord.root]);
      expect(chord.bassName).toBe(CHORD_PITCH_NAMES[chord.bass]);
      expect(chord.durationSec).toBeCloseTo(chord.endSec - chord.startSec, 5);
    }
    const first = detected.find((chord) => chord.name !== 'N.C.');
    expect(first?.rootName).toBe('C');
    expect(first?.root).toBe(0);
  });
});
