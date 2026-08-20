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
  analyzeWithProgress,
  capabilities,
  capabilityCatalog,
  chordFunctionalAnalysis,
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
  hasFfmpegSupport,
  hybridCqt,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
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

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

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
    expect(report.abi).toEqual({ project: 1, engine: expect.any(Number) });
    expect(report.platform).toEqual(expect.any(String));
    expect(report.features).toEqual({
      mastering: expect.any(Boolean),
      mixing: expect.any(Boolean),
      fx: expect.any(Boolean),
      ffmpeg: hasFfmpegSupport(),
      instrumentParamAutomation: expect.any(Boolean),
    });
    expect(report.decode.builtin).toEqual(['wav', 'mp3']);
    expect(Array.isArray(report.decode.ffmpeg)).toBe(true);
    expect(report.simd).toEqual(expect.any(String));
    expect(report.hardwareConcurrency).toBeGreaterThan(0);
  });

  it('capabilityCatalog aggregates processors and built-in presets', () => {
    const catalog = capabilityCatalog();
    expect(catalog.version).toBe(version());
    expect(catalog.abi.project).toBe(1);
    expect(catalog.processors.length).toBeGreaterThan(0);
    expect(catalog.presets.mastering).toContain('pop');
    const compressor = catalog.processors.find(({ id }) => id === 'dynamics.compressor');
    expect(compressor).toMatchObject({ category: 'dynamics', realtimeInsertable: true });
    expect(compressor?.params[0]).toEqual(
      expect.objectContaining({ name: expect.any(String), type: expect.any(String) }),
    );
  });

  it('detectBpm returns a number', () => {
    const bpm = detectBpm(new Float32Array(SR), SR);
    expect(typeof bpm).toBe('number');
    expect(bpm).toBeGreaterThanOrEqual(0);
  });

  it('detectKey returns key object', () => {
    const key = detectKey(new Float32Array(SR), SR);
    expect(typeof key.root).toBe('string');
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
    const result = analyze(new Float32Array(SR), SR);
    expect(result).toHaveProperty('bpm');
    expect(result).toHaveProperty('key');
    expect(result).toHaveProperty('timeSignature');
    expect(result).toHaveProperty('beatTimes');
    expect(result).toHaveProperty('beats');
    expect(result.beatTimes).toBeInstanceOf(Float32Array);
    expect(Array.isArray(result.beats)).toBe(true);
  });

  it('analyzeWithProgress reports progress and matches analyze shape', () => {
    const samples = generateSine(220, SR, 2);
    const progress: number[] = [];
    const result = analyzeWithProgress(samples, SR, (p, _stage) => {
      progress.push(p);
    });
    expect(result).toHaveProperty('bpm');
    expect(result).toHaveProperty('key');
    expect(result).toHaveProperty('timeSignature');
    expect(result.beatTimes).toBeInstanceOf(Float32Array);
    expect(progress.length).toBeGreaterThan(0);
    for (const p of progress) {
      expect(p).toBeGreaterThanOrEqual(0);
      expect(p).toBeLessThanOrEqual(1);
    }
  });

  it('analyzeWithProgress ignores a progress callback false result', () => {
    const progress: number[] = [];
    const result = analyzeWithProgress(generateSine(220, SR, 2), SR, (value) => {
      progress.push(value);
      return false;
    });

    expect(progress.some((value) => value > 0.5)).toBe(true);
    expect(result.beatTimes).toBeInstanceOf(Float32Array);
  });

  it('analyzeWithProgress cancels through the request cancel callback', () => {
    expect(() =>
      analyzeWithProgress({
        samples: generateSine(220, SR, 2),
        sampleRate: SR,
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });

  it('analyzeSections returns an array of sections', () => {
    const samples = generateSine(220, SR, 4);
    const sections = analyzeSections(samples, SR);
    expect(Array.isArray(sections)).toBe(true);
    for (const section of sections) {
      expect(typeof section.type).toBe('number');
      expect(typeof section.name).toBe('string');
      expect(section.end).toBeGreaterThanOrEqual(section.start);
    }
  });

  it('analyzeMelody returns a melody contour', () => {
    const samples = generateSine(220, SR, 1);
    const melody = analyzeMelody(samples, SR);
    expect(Array.isArray(melody.points)).toBe(true);
    expect(typeof melody.meanFrequency).toBe('number');
    expect(typeof melody.pitchStability).toBe('number');
    for (const point of melody.points) {
      expect(typeof point.time).toBe('number');
      expect(typeof point.frequency).toBe('number');
    }
  });

  it('cqt and vqt return magnitude matrices', () => {
    const samples = generateSine(220, SR, 1);
    const cqtResult = cqt(samples, SR);
    expect(cqtResult.nBins).toBe(84);
    expect(cqtResult.nFrames).toBeGreaterThan(0);
    expect(cqtResult.magnitude.length).toBe(cqtResult.nBins * cqtResult.nFrames);
    expect(cqtResult.frequencies.length).toBe(cqtResult.nBins);

    const vqtResult = vqt(samples, SR);
    expect(vqtResult.nBins).toBe(84);
    expect(vqtResult.magnitude.length).toBe(vqtResult.nBins * vqtResult.nFrames);
    expect(vqtResult.frequencies.length).toBe(vqtResult.nBins);
  });

  it('pseudoCqt and hybridCqt return magnitude matrices', () => {
    const samples = generateSine(220, SR, 1);
    for (const result of [
      pseudoCqt(samples, SR, 512, 32.70319566257483, 24, 12),
      hybridCqt(samples, SR, 512, 32.70319566257483, 24, 12),
    ]) {
      expect(result.nBins).toBe(24);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.magnitude.length).toBe(result.nBins * result.nFrames);
      expect(result.frequencies.length).toBe(result.nBins);
    }
  });

  it('onsetStrengthMulti returns a band matrix', () => {
    const samples = generateSine(440, SR, 1);
    const result = onsetStrengthMulti(samples, SR, 2048, 512, 64, 4);
    expect(result.nBands).toBe(4);
    expect(result.nFrames).toBeGreaterThan(0);
    expect(result.data.length).toBe(result.nBands * result.nFrames);
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
    expect(dynamics.loudnessTimes).toBeInstanceOf(Float32Array);
    expect(dynamics.loudnessRmsDb.length).toBe(dynamics.loudnessTimes.length);

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

    const chords = detectChords(tone, SR, 0.3, 2.0, 0.5, false, 2048, 512, false);
    expect(Array.isArray(chords.chords)).toBe(true);
    // The per-chord assertions below say nothing on an empty progression, and
    // this input is a sustained triad, so it must detect at least one chord.
    expect(chords.chords.length).toBeGreaterThan(0);
    for (const chord of chords.chords) {
      expect(chord.rootName).toBe(chord.root);
      expect(chord.bassName).toBe(chord.bass);
      // `name` is the core's rendered symbol: root spelling, then the quality
      // suffix, then a slash bass only when the bass differs from the root.
      expect(typeof chord.name).toBe('string');
      expect(chord.name.length).toBeGreaterThan(0);
      if (chord.quality === 'unknown') {
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

    // Roman-numeral labels: one per detected chord, all non-empty strings.
    const romans = chordFunctionalAnalysis(tone, 0, 0, SR, 0.3, 2.0, 0.5, false, 2048, 512, false);
    expect(Array.isArray(romans)).toBe(true);
    expect(romans.length).toBe(chords.chords.length);
    for (const label of romans) {
      expect(typeof label).toBe('string');
      expect(label.length).toBeGreaterThan(0);
    }
  });

  it('detectChords options-object form matches the positional form', () => {
    const tone = new Float32Array(SR);
    for (let i = 0; i < tone.length; i++) {
      tone[i] =
        0.25 *
        (Math.sin((2 * Math.PI * 261.63 * i) / SR) +
          Math.sin((2 * Math.PI * 329.63 * i) / SR) +
          Math.sin((2 * Math.PI * 392.0 * i) / SR));
    }

    const positional = detectChords(tone, SR, 0.3, 2.0, 0.5, false, 2048, 512, false);
    const optionForm = detectChords(tone, SR, {
      minDuration: 0.3,
      smoothingWindow: 2.0,
      threshold: 0.5,
      useTriadsOnly: false,
      nFft: 2048,
      hopLength: 512,
      useBeatSync: false,
    });
    expect(Array.isArray(optionForm.chords)).toBe(true);
    expect(optionForm.chords.length).toBe(positional.chords.length);
    if (positional.chords.length > 0) {
      expect(optionForm.chords[0].root).toBe(positional.chords[0].root);
      expect(optionForm.chords[0].quality).toBe(positional.chords[0].quality);
    }

    // chordFunctionalAnalysis options form: key from the dedicated args, the
    // detection knobs from the options object.
    const romansPositional = chordFunctionalAnalysis(
      tone,
      0,
      0,
      SR,
      0.3,
      2.0,
      0.5,
      false,
      2048,
      512,
      false,
    );
    const romansOptions = chordFunctionalAnalysis(tone, 0, 0, SR, {
      minDuration: 0.3,
      smoothingWindow: 2.0,
      threshold: 0.5,
      useTriadsOnly: false,
      nFft: 2048,
      hopLength: 512,
      useBeatSync: false,
    });
    expect(Array.isArray(romansOptions)).toBe(true);
    expect(romansOptions.length).toBe(romansPositional.length);
    expect(romansOptions).toEqual(romansPositional);
  });

  it('detectChords threshold emits unknown for ambiguous input', () => {
    const silence = new Float32Array(4096);
    const common = {
      minDuration: 0,
      smoothingWindow: 0.01,
      useTriadsOnly: true,
      nFft: 2048,
      hopLength: 512,
      useBeatSync: false,
    } as const;
    const low = detectChords(silence, SR, { ...common, threshold: 0 });
    const high = detectChords(silence, SR, { ...common, threshold: 0.99 });
    expect(low.chords).toHaveLength(1);
    expect(low.chords[0]?.quality).not.toBe('unknown');
    expect(high.chords).toHaveLength(1);
    expect(high.chords[0]?.quality).toBe('unknown');
    expect(high.chords[0]?.start).toBe(0);
    expect(high.chords[0]?.end).toBeGreaterThan(0);
  });

  it('chordFunctionalAnalysis rejects non-Float32Array input', () => {
    expect(() =>
      chordFunctionalAnalysis(new Float64Array(SR) as unknown as Float32Array, 0, 0, SR),
    ).toThrow(/Float32Array/);
  });

  it('rejects non-Float32Array input', () => {
    expect(() => detectBpm(new Float64Array(SR) as unknown as Float32Array, SR)).toThrow(
      /Float32Array/,
    );
  });

  it('converts invalid native arguments into JS exceptions', () => {
    expect(() => timeStretch(new Float32Array(SR), -1, 2.0)).toThrow(
      /sample_rate.*supported range/,
    );
  });

  it('exposes compatibility numeric and signal utilities', () => {
    expect(framesToSamples(4, 512, 0)).toBe(2048);
    expect(samplesToFrames(2048, 512, 0)).toBe(4);

    const powerDb = powerToDb(new Float32Array([1, 0.01]), 1, 1e-10, 80);
    expect(powerDb[0]).toBeCloseTo(0, 5);
    expect(powerDb[1]).toBeCloseTo(-20, 4);
    expect(dbToPower(powerDb, 1)[1]).toBeCloseTo(0.01, 5);

    const ampDb = amplitudeToDb(new Float32Array([1, 0.5]), 1, 1e-5, 80);
    expect(ampDb[0]).toBeCloseTo(0, 5);
    expect(dbToAmplitude(ampDb, 1)[1]).toBeCloseTo(0.5, 5);

    const emphasized = preemphasis(new Float32Array([1, 1, 1]), 0.5, 0);
    expect(Array.from(emphasized)).toEqual([1, 0.5, 0.5]);
    const deemphasized = deemphasis(emphasized, 0.5, 0);
    expect(deemphasized[2]).toBeCloseTo(1, 5);

    const framed = frameSignal(new Float32Array([1, 2, 3, 4]), 2, 1);
    expect(framed.nFrames).toBe(3);
    expect(Array.from(framed.frames)).toEqual([1, 2, 2, 3, 3, 4]);
    expect(Array.from(fixLength(new Float32Array([1, 2]), 4, -1))).toEqual([1, 2, -1, -1]);
    expect(() => padCenter(new Float32Array([1, 2]), -1)).toThrow(RangeError);
    expect(() => fixLength(new Float32Array([1, 2]), -1)).toThrow(RangeError);
    expect(Array.from(fixFrames(new Int32Array([2, 4]), 0, 5, true))).toEqual([0, 2, 4, 5]);
    // Matches librosa.util.peak_pick exactly (index 0 is a peak under its
    // first-frame rule: x[0] >= max/mean of the leading window).
    expect(Array.from(peakPick(new Float32Array([0, 1, 0, 2, 0]), 1, 1, 1, 1, 0, 0))).toEqual([
      0, 1, 3,
    ]);

    const normalized = vectorNormalize(new Float32Array([3, 4]), 2, 1e-12);
    expect(normalized[0]).toBeCloseTo(0.6, 5);
    expect(normalized[1]).toBeCloseTo(0.8, 5);
  });

  it('exposes silence and rhythm compatibility utilities', () => {
    const samples = new Float32Array([0, 0, 1, 1, 0, 0]);
    const trimmed = trimSilence(samples, 20, 2, 1);
    expect(trimmed.audio.length).toBeGreaterThan(0);
    expect(trimmed.startSample).toBeGreaterThanOrEqual(0);
    expect(trimmed.endSample).toBeGreaterThan(trimmed.startSample);
    expect(splitSilence(samples, 20, 2, 1)).toBeInstanceOf(Int32Array);

    const pcenValues = pcen(new Float32Array([1, 2, 3, 4]), 2, 2);
    expect(pcenValues).toBeInstanceOf(Float32Array);
    expect(pcenValues.length).toBe(4);

    const chromaValues = new Float32Array(12 * 2);
    chromaValues[0] = 1;
    chromaValues[12] = 1;
    const tonnetzValues = tonnetz(chromaValues, 12, 2);
    expect(tonnetzValues).toBeInstanceOf(Float32Array);
    expect(tonnetzValues.length).toBe(12);

    const onset = new Float32Array([0, 1, 0, 1, 0, 1, 0, 1]);
    const temp = tempogram(onset, SR, 512, 4);
    expect(temp.data).toBeInstanceOf(Float32Array);
    expect(temp.winLength).toBe(4);
    expect(plp(onset, SR, 512, 30, 300, 4)).toBeInstanceOf(Float32Array);
  });

  it('exposes pair and stereo mastering APIs', () => {
    const sampleRate = 44100;
    const source = generateSine(440, sampleRate, 0.25);
    const reference = generateSine(880, sampleRate, 0.25);

    expect(masteringPairProcessorNames()).toContain('match.abCrossfade');
    expect(masteringPairAnalysisNames()).toContain('match.referenceLoudness');
    expect(masteringStereoAnalysisNames()).toContain('stereo.monoCompatCheck');

    const paired = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
      mix: 0.25,
    });
    expect(paired.samples).toBeInstanceOf(Float32Array);
    expect(paired.samples.length).toBe(source.length);

    const pairJson = masteringPairAnalyze('match.referenceLoudness', source, reference, sampleRate);
    expect(pairJson).toContain('"sourceLufs"');
    expect(pairJson).toContain('"referenceLufs"');

    const stereoJson = masteringStereoAnalyze(
      'stereo.monoCompatCheck',
      source,
      reference,
      sampleRate,
    );
    expect(stereoJson).toContain('"correlation"');
  });

  it('accepts independent source/reference lengths for pair mastering', () => {
    const sampleRate = 44100;
    const source = generateSine(440, sampleRate, 0.25);
    const reference = generateSine(880, sampleRate, 0.6); // different duration

    const paired = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
      mix: 0.25,
    });
    expect(paired.samples).toBeInstanceOf(Float32Array);
    expect(paired.samples.length).toBe(source.length);

    const pairJson = masteringPairAnalyze('match.referenceLoudness', source, reference, sampleRate);
    expect(pairJson).toContain('"sourceLufs"');
    expect(pairJson).toContain('"referenceLufs"');
  });

  it('exposes mastering assistant suggestions', () => {
    const sampleRate = 22050;
    const samples = generateSine(220, sampleRate, 3);
    const json = masteringAssistantSuggest(samples, sampleRate, {
      targetLufs: -13,
      ceilingDb: -0.8,
      enableRepair: true,
    });
    const result = JSON.parse(json);

    expect(result).toHaveProperty('chainConfig');
    expect(result).toHaveProperty('profile');
    expect(Array.isArray(result.explanation)).toBe(true);
    expect(Array.isArray(result.genreCandidates)).toBe(true);
    expect(result.chainConfig.params['loudness.targetLufs']).toBe(-13);
    expect(result.chainConfig.params['loudness.ceilingDb']).toBeCloseTo(-0.8, 6);
    // After chain_json.cpp moved to util::json::dump, booleans serialize as
    // JSON `true`/`false` (RFC 8259) instead of `1`/`0`. Same semantics.
    expect(result.chainConfig.params['repair.declick.enabled']).toBe(true);
  });

  it('exposes mastering audio profiles', () => {
    const sampleRate = 22050;
    const samples = generateSine(330, sampleRate, 2);
    const json = masteringAudioProfile(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
    });
    const result = JSON.parse(json);

    expect(typeof result.durationSec).toBe('number');
    expect(result.durationSec).toBeGreaterThan(1.9);
    expect(result).toHaveProperty('loudness.integratedLufs');
    expect(result).toHaveProperty('spectral.centroidHz');
    expect(result).toHaveProperty('dynamics.attackDensity');
    expect(Array.isArray(result.genreCandidates)).toBe(true);
  });

  it('exposes streaming platform loudness previews', () => {
    const sampleRate = 22050;
    const samples = generateSine(440, sampleRate, 1);
    const json = masteringStreamingPreview(samples, sampleRate, [
      { name: 'Unit Test', targetLufs: -12, ceilingDb: -1 },
    ]);
    const result = JSON.parse(json);

    expect(result.platforms).toHaveLength(1);
    expect(result.platforms[0].name).toBe('Unit Test');
    expect(typeof result.platforms[0].integratedLufs).toBe('number');
    expect(typeof result.platforms[0].truePeakDb).toBe('number');
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

    const stereo = JSON.parse(masteringStreamingPreviewStereo({ left, right, sampleRate }));
    const mono = JSON.parse(masteringStreamingPreview(downmix, sampleRate));
    const stereoLufs = stereo.platforms[0].integratedLufs;
    const monoLufs = mono.platforms[0].integratedLufs;

    expect(stereo.platforms[0].name).toBe('Spotify');
    // BS.1770 sums the channel powers while the 0.5*(L+R) downmix quarters
    // them, so a decorrelated pair reads 6.02 dB above what a mono caller can
    // measure, and the normalization gain built on it is off by the same.
    expect(stereoLufs - monoLufs).toBeCloseTo(6.02, 1);
    expect(stereo.platforms[0].normalizationGainDb).toBeCloseTo(-14 - stereoLufs, 3);

    const profile = JSON.parse(masteringAudioProfileStereo({ left, right, sampleRate }));
    expect(profile.loudness.integratedLufs).toBeCloseTo(stereoLufs, 2);
    expect(profile).toHaveProperty('spectral.centroidHz');

    const suggestion = JSON.parse(masteringAssistantSuggestStereo({ left, right, sampleRate }));
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
    // The anti-phase pair cancels to silence in the downmix, which reports the
    // 0 dB neutral rather than the pair's real 3 dB crest factor.
    expect(meteringCrestFactorDb(downmix, sampleRate)).toBeCloseTo(0, 5);
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
