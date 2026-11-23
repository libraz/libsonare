import { describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  decompose,
  decomposeStems,
  detectAcoustic,
  ebur128LoudnessRangeLu,
  estimateTuning,
  hpss,
  lufsMultichannel,
  lufsSeriesMultichannel,
  momentaryLufs,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchShift,
  pitchTuning,
  pitchYin,
  polyFeatures,
  realtimeVoiceChangerPresetConfig,
  remix,
  remixAlignedIntervals,
  scaleCorrectionSemitones,
  scaleMaskForMode,
  scalePitchClassEnabled,
  scaleQuantizeMidi,
  spectralContrast,
  timeStretch,
  voiceCharacterPresetId,
  zeroCrossings,
} from '../src/index.js';

const SR = 22050;

function sine(durationSec: number, freqHz: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freqHz * i) / SR);
  }
  return out;
}

/** Deterministic white noise; a different `seed` is an uncorrelated realization. */
function noise(durationSec: number, seed: number, amplitude = 0.25): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  let state = seed >>> 0;
  for (let i = 0; i < n; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = amplitude * ((2 * (state >>> 8)) / (1 << 24) - 1);
  }
  return out;
}

function allFinite(arr: ArrayLike<number>): boolean {
  for (let i = 0; i < arr.length; i++) {
    if (!Number.isFinite(arr[i])) {
      return false;
    }
  }
  return arr.length > 0;
}

describe('newly exposed Node functions', () => {
  it('spectralContrast returns (nBands+1) x nFrames', () => {
    const r = spectralContrast(sine(1, 440), SR, { nFft: 2048, hopLength: 512, nBands: 6 });
    expect(r.rows).toBe(7);
    expect(r.cols).toBeGreaterThan(0);
    expect(r.data.length).toBe(r.rows * r.cols);
    expect(allFinite(r.data)).toBe(true);
  });

  it('polyFeatures returns (order+1) x nFrames', () => {
    const r = polyFeatures(sine(1, 440), SR, { nFft: 2048, hopLength: 512, order: 1 });
    expect(r.rows).toBe(2);
    expect(allFinite(r.data)).toBe(true);
  });

  it('zeroCrossings returns sorted in-range indices', () => {
    const x = sine(0.05, 440);
    const idx = zeroCrossings(x, SR);
    expect(idx.length).toBeGreaterThan(0);
    for (let i = 1; i < idx.length; i++) {
      expect(idx[i]).toBeGreaterThan(idx[i - 1]);
      expect(idx[i]).toBeLessThan(x.length);
    }
  });

  it('pitchTuning is finite', () => {
    const t = pitchTuning({ frequencies: new Float32Array([440, 880, 660]) });
    expect(Number.isFinite(t)).toBe(true);
    expect(t).toBeGreaterThanOrEqual(-50);
    expect(t).toBeLessThan(50);
  });

  it('estimateTuning is finite', () => {
    expect(Number.isFinite(estimateTuning(sine(1, 440), SR))).toBe(true);
  });

  it('decompose factorizes a non-negative spectrogram', () => {
    const nF = 16;
    const nT = 24;
    const nC = 3;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.sin(i));
    }
    const r = decompose({
      s: { data: s, rows: nF, cols: nT },
      nComponents: nC,
      nIter: 20,
      beta: 2.0,
    });
    expect(r.w.data.length).toBe(nF * nC);
    expect(r.h.data.length).toBe(nC * nT);
    expect(allFinite(r.w.data) && allFinite(r.h.data)).toBe(true);
  });

  it('decompose accepts an nndsvd warm-start initialiser', () => {
    const nF = 16;
    const nT = 24;
    const nC = 3;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.sin(i));
    }
    const r = decompose({
      s: { data: s, rows: nF, cols: nT },
      nComponents: nC,
      nIter: 20,
      beta: 2.0,
      init: 'nndsvd',
    });
    expect(r.w.data.length).toBe(nF * nC);
    expect(r.h.data.length).toBe(nC * nT);
    expect(allFinite(r.w.data) && allFinite(r.h.data)).toBe(true);
  });

  it('nnFilter preserves shape', () => {
    const nF = 12;
    const nT = 20;
    const s = new Float32Array(nF * nT);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.cos(i));
    }
    const r = nnFilter({ s: { data: s, rows: nF, cols: nT } });
    expect(r.rows).toBe(nF);
    expect(r.cols).toBe(nT);
    expect(allFinite(r.data)).toBe(true);
  });

  it('decompose/nnFilter reject dims that exceed the input length (no OOB read)', () => {
    const small = new Float32Array(10);
    expect(() =>
      decompose({
        s: { data: small, rows: 1000, cols: 1000 },
        nComponents: 3,
        nIter: 20,
        beta: 2.0,
      }),
    ).toThrow();
    expect(() => nnFilter({ s: { data: small, rows: 1000, cols: 1000 } })).toThrow();
  });

  it('timeStretch/pitchShift reject a missing required argument instead of producing NaN', () => {
    const x = sine(0.1, 220);
    // sampleRate then the transform factor; both are required, so a bare call must throw.
    expect(() => (timeStretch as unknown as (s: Float32Array) => Float32Array)(x)).toThrow();
    expect(() => (pitchShift as unknown as (s: Float32Array) => Float32Array)(x)).toThrow();
    // Valid calls still work.
    expect(allFinite(timeStretch(x, SR, { speedRatio: 1.25 }))).toBe(true);
    expect(allFinite(pitchShift(x, SR, { pitchSemitones: 2 }))).toBe(true);
  });

  it('timeStretch rejects non-finite and oversized projected rates, then recovers', () => {
    const x = sine(0.1, 220);
    expect(() => timeStretch(x, SR, { speedRatio: Number.NaN })).toThrow();
    expect(() => timeStretch(x, SR, { speedRatio: Number.POSITIVE_INFINITY })).toThrow();
    expect(() => timeStretch(x, SR, { speedRatio: 1.1754943508222875e-38 })).toThrow();
    expect(allFinite(timeStretch(x, SR, { speedRatio: 1 }))).toBe(true);
  });

  it('realtimeVoiceChangerPresetConfig exposes the ISP true-peak limiter fields', () => {
    const cfg = realtimeVoiceChangerPresetConfig('neutralMonitor') as unknown as Record<
      string,
      unknown
    >;
    expect(typeof cfg.limiterEnableIspLimiter).toBe('boolean');
    expect(typeof cfg.limiterIspCeilingDbtp).toBe('number');
    expect(Number.isFinite(cfg.limiterIspCeilingDbtp as number)).toBe(true);
  });

  it('remix concatenates interval slices', () => {
    const x = sine(0.5, 440);
    const half = Math.floor(x.length / 2);
    const out = remix(x, SR, { intervals: new Int32Array([half, x.length, 0, half]) });
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('remix refuses a fractional plain-array interval instead of truncating it', () => {
    const x = sine(0.5, 440);
    // The control: a whole-number interval end is load-bearing for the output
    // length, so a silent fold onto the same length would hide behind it.
    expect(remix(x, SR, { intervals: [0, 1000] }).length).not.toBe(
      remix(x, SR, { intervals: [0, 2000] }).length,
    );
    expect(() => remix(x, SR, { intervals: [0, 1000.7] })).toThrow(/intervals\[1\]/);
  });

  it('remixAlignedIntervals resolves one clamped pair per interval', () => {
    const x = sine(0.5, 440);
    const pairs = remixAlignedIntervals({
      samples: x,
      sampleRate: SR,
      intervals: new Int32Array([0, 1000, 5000, 5500]),
    });
    expect(pairs.length).toBe(4);
    expect(pairs[0]).toBeGreaterThanOrEqual(0);
    expect(pairs[1]).toBeGreaterThan(pairs[0]);
    expect(pairs[3]).toBeGreaterThan(pairs[2]);
    expect(pairs[3]).toBeLessThanOrEqual(x.length);
  });

  it('remixAlignedIntervals leaves a signal with no sign change unsnapped', () => {
    const flat = new Float32Array(4096).fill(0.25);
    const pairs = remixAlignedIntervals({
      samples: flat,
      sampleRate: SR,
      intervals: new Int32Array([100, 200]),
    });
    expect(Array.from(pairs)).toEqual([100, 200]);
  });

  it('decomposeStems components carry phase and sum back to the input', () => {
    const n = 8192;
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      x[i] = 0.5 * Math.sin((2 * Math.PI * (i < n / 2 ? 220 : 880) * i) / SR);
    }
    const r = decomposeStems({
      samples: x,
      sampleRate: SR,
      nComponents: 2,
      nFft: 1024,
      hopLength: 256,
      nIter: 30,
    });
    expect(r.components.length).toBe(2);
    expect(r.components[0].length).toBe(n);
    expect(r.w.length).toBe((1024 / 2 + 1) * 2);
    expect(r.h.length % 2).toBe(0);
    expect(r.sampleRate).toBe(SR);
    let err = 0;
    let ref = 0;
    for (let i = 1024; i < n - 1024; i++) {
      const sum = r.components[0][i] + r.components[1][i];
      err += (sum - x[i]) ** 2;
      ref += x[i] ** 2;
    }
    expect(Math.sqrt(err / ref)).toBeLessThan(0.05);
  });

  it('decomposeStems reads every config field literally, so 0 is a value', () => {
    // Omitted keeps the library default; a given value is read as itself, so a 0
    // outside a count's domain is refused and beta 0 is the Itakura-Saito divergence.
    const n = 8192;
    const samples = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      samples[i] =
        0.5 * Math.sin((2 * Math.PI * 440 * i) / SR) + 0.3 * Math.sin((2 * Math.PI * 660 * i) / SR);
    }
    const run = (options: Record<string, number>) =>
      decomposeStems({ samples, sampleRate: SR, ...options });
    const fingerprint = (options: Record<string, number>): string => {
      const r = run(options);
      return JSON.stringify([
        r.components.length,
        r.components[0].length,
        r.w.length,
        r.h.length,
        Array.from(r.components[0].slice(0, 8)),
      ]);
    };

    for (const [field, documentedDefault] of [
      ['nComponents', 4],
      ['nFft', 2048],
      ['hopLength', 512],
      ['nIter', 100],
      ['beta', 2],
      ['maskPower', 1],
    ] as Array<[string, number]>) {
      expect(fingerprint({ [field]: documentedDefault })).toBe(fingerprint({}));
    }

    // Without these the case above would also pass on a surface that ignored
    // the option outright.
    expect(fingerprint({ nComponents: 3 })).not.toBe(fingerprint({}));
    expect(fingerprint({ beta: 0 })).not.toBe(fingerprint({}));
    expect(fingerprint({ maskPower: 2 })).not.toBe(fingerprint({}));

    // A count of 0 or below and a sub-unit mask power are refused rather than defaulted.
    for (const field of ['nComponents', 'nFft', 'hopLength', 'nIter']) {
      expect(() => run({ [field]: 0 })).toThrow(RangeError);
      expect(() => run({ [field]: -1 })).toThrow(RangeError);
    }
    for (const maskPower of [0, 0.5, -1, Number.NaN]) {
      expect(() => run({ maskPower })).toThrow(RangeError);
    }
    expect(() => run({ beta: Number.NaN })).toThrow(RangeError);
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = sine(0.5, 440);
    const out = phaseVocoder(x, SR, { speedRatio: 2.0 });
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('hpss with withResidual splits into three signals', () => {
    const r = hpss(sine(1, 440), SR, { withResidual: true });
    expect(r.harmonic.length).toBe(r.percussive.length);
    expect(r.percussive.length).toBe(r.residual?.length);
    expect(allFinite(r.harmonic)).toBe(true);
  });

  it('lufsMultichannel measures dual-mono loudness', () => {
    const x = sine(1, 440);
    const r = lufsMultichannel({ channels: [x, x], sampleRate: SR });
    expect(Number.isFinite(r.integratedLufs)).toBe(true);
    expect(r.integratedLufs).toBeLessThan(0);
  });

  it('lufsSeriesMultichannel sums channel energies rather than averaging loudness', () => {
    // BS.1770-4 sums the K-weighted per-channel block energies, so two
    // uncorrelated channels at one level read 10*log10(2) above their
    // per-channel mean. A facade that measured each channel and averaged the
    // results in dB would put this difference at exactly 0.
    const left = noise(4, 12345);
    const right = noise(4, 987654321);
    const { momentaryLufs: momentary, shortTermLufs: shortTerm } = lufsSeriesMultichannel({
      channels: [left, right],
      sampleRate: SR,
    });
    const monoLeft = momentaryLufs(left, SR);
    const monoRight = momentaryLufs(right, SR);

    expect(momentary.length).toBe(monoLeft.length);
    expect(momentary.length).toBe(monoRight.length);
    expect(allFinite(momentary)).toBe(true);
    for (let i = 0; i < momentary.length; i++) {
      expect(momentary[i] - 0.5 * (monoLeft[i] + monoRight[i])).toBeCloseTo(3.0103, 1);
    }
    expect(shortTerm.length).toBeGreaterThan(0);
    expect(allFinite(shortTerm)).toBe(true);
  });

  it('lufsSeriesMultichannel matches the mono meters for one channel', () => {
    const x = noise(4, 24680);
    const { momentaryLufs: momentary, shortTermLufs: shortTerm } = lufsSeriesMultichannel({
      channels: [x],
      sampleRate: SR,
    });
    expect(Array.from(momentary)).toEqual(Array.from(momentaryLufs(x, SR)));
    expect(shortTerm.length).toBeGreaterThan(0);
  });

  it('lufsSeriesMultichannel returns an empty series below a window', () => {
    // 2 s clears the 400 ms momentary window and not the 3 s short-term one.
    const x = noise(2, 13579);
    const { momentaryLufs: momentary, shortTermLufs: shortTerm } = lufsSeriesMultichannel({
      channels: [x, x],
      sampleRate: SR,
    });
    expect(momentary.length).toBeGreaterThan(0);
    expect(shortTerm.length).toBe(0);
  });

  it('lufsSeriesMultichannel rejects an empty plane list', () => {
    expect(() => lufsSeriesMultichannel({ channels: [], sampleRate: SR })).toThrow(RangeError);
  });

  it('ebur128LoudnessRangeLu is finite and non-negative, and absent under 3 s', () => {
    const lra = ebur128LoudnessRangeLu(sine(4, 440), SR);
    expect(Number.isFinite(lra)).toBe(true);
    expect(lra).toBeGreaterThanOrEqual(0);
    // No short-term window passes the gate in a signal shorter than one 3 s window.
    expect(ebur128LoudnessRangeLu(sine(1, 440), SR)).toBeUndefined();
  });

  it('pitchYin returns librosa-style estimates for unvoiced frames', () => {
    const silence = new Float32Array(SR); // fully unvoiced
    const nanRes = pitchYin(silence, SR, {
      frameLength: 2048,
      hopLength: 512,
      f0MinHz: 65,
      f0MaxHz: 2093,
      threshold: 0.3,
    });
    expect(nanRes.f0.every((v) => Number.isFinite(v))).toBe(true);
    expect(nanRes.voicedFlag.every((v) => !v)).toBe(true);
  });

  it('pitchYin refuses fillNa, which only pYIN reads', () => {
    const silence = new Float32Array(SR);
    expect(() => pitchYin({ samples: silence, sampleRate: SR, fillNa: true } as never)).toThrow(
      /fillNa/,
    );
    expect(() => pitchYin(silence, SR, { fillNa: false } as never)).toThrow(/fillNa/);
  });

  it('pitchPyin fillNa controls the unvoiced value', () => {
    const silence = new Float32Array(SR);
    const nanRes = pitchPyin(silence, SR, {
      frameLength: 2048,
      hopLength: 512,
      f0MinHz: 65,
      f0MaxHz: 2093,
      threshold: 0.3,
      fillNa: false,
    });
    expect(nanRes.f0.some((v) => Number.isNaN(v))).toBe(true);

    const filled = pitchPyin(silence, SR, {
      frameLength: 2048,
      hopLength: 512,
      f0MinHz: 65,
      f0MaxHz: 2093,
      threshold: 0.3,
      fillNa: true,
    });
    expect(filled.f0.every((v) => Number.isFinite(v))).toBe(true);
  });

  it('voiceCharacterPresetId maps a known ordinal to its id', () => {
    expect(voiceCharacterPresetId(1)).toBe('brightIdol');
    // A preset id is not an ordinal, and an out-of-range ordinal is refused.
    expect(() => voiceCharacterPresetId('neutralMonitor' as never)).toThrow(TypeError);
    expect(() => voiceCharacterPresetId(99)).toThrow(RangeError);
  });

  it('realtimeVoiceChangerPresetConfig returns a config object with expected fields', () => {
    const cfg = realtimeVoiceChangerPresetConfig('brightIdol') as unknown as Record<string, number>;
    for (const key of [
      'inputGainDb',
      'outputGainDb',
      'wetMix',
      'retunePitchSemitones',
      'formantFactor',
      'compressorThresholdDb',
      'reverbSeed',
      'limiterCeilingDb',
      'limiterReleaseMs',
    ]) {
      expect(typeof cfg[key]).toBe('number');
      expect(Number.isFinite(cfg[key])).toBe(true);
    }
  });

  it('scaleQuantizeMidi rejects an out-of-range modeMask', () => {
    expect(() => scaleQuantizeMidi(0, -1, 69)).toThrow();
    expect(() => scaleQuantizeMidi(0, 5000, 69)).toThrow();
  });

  it('refuses a wrong-typed scale root, mode mask or pitch class as a non-integer', () => {
    const bad = 'x' as unknown as number;
    const rows: Array<[string, () => unknown, RegExp]> = [
      [
        'scaleQuantizeMidi root',
        () => scaleQuantizeMidi(bad, 1, 60),
        /scaleQuantizeMidi: root must be an integer/,
      ],
      [
        'scaleQuantizeMidi modeMask',
        () => scaleQuantizeMidi(0, bad, 60),
        /scaleQuantizeMidi: modeMask must be an integer/,
      ],
      [
        'scaleMaskForMode root',
        () => scaleMaskForMode(bad as never, 'major'),
        /scaleMaskForMode: root must be an integer/,
      ],
      [
        'scaleCorrectionSemitones root',
        () => scaleCorrectionSemitones(bad, 1, 60),
        /scaleCorrectionSemitones: root must be an integer/,
      ],
      [
        'scaleCorrectionSemitones modeMask',
        () => scaleCorrectionSemitones(0, bad, 60),
        /scaleCorrectionSemitones: modeMask must be an integer/,
      ],
      [
        'scalePitchClassEnabled root',
        () => scalePitchClassEnabled(bad, 1, 0),
        /scalePitchClassEnabled: root must be an integer/,
      ],
      [
        'scalePitchClassEnabled modeMask',
        () => scalePitchClassEnabled(0, bad, 0),
        /scalePitchClassEnabled: modeMask must be an integer/,
      ],
      [
        'scalePitchClassEnabled pitchClass',
        () => scalePitchClassEnabled(0, 1, bad),
        /scalePitchClassEnabled: pitchClass must be an integer/,
      ],
    ];
    for (const [, call, message] of rows) {
      expect(call).toThrow(TypeError);
      expect(call).toThrow(message);
    }
  });

  it('phaseVocoder rejects a non-number sampleRate', () => {
    const x = sine(0.1, 440);
    expect(() => phaseVocoder(x, 'bad' as unknown as number, { speedRatio: 2.0 })).toThrow();
  });

  it('analyzeTimbre exposes timbreOverTime', () => {
    const r = analyzeTimbre(sine(2, 440), SR);
    expect(r.timbreOverTime.length).toBeGreaterThan(0);
    for (const frame of r.timbreOverTime) {
      expect(Number.isFinite(frame.brightness)).toBe(true);
      expect(Number.isFinite(frame.warmth)).toBe(true);
      expect(Number.isFinite(frame.density)).toBe(true);
      expect(Number.isFinite(frame.roughness)).toBe(true);
      expect(Number.isFinite(frame.complexity)).toBe(true);
    }
  });

  it('analysis functions accept options objects', () => {
    const x = sine(2, 440);
    // maxCandidates is honoured through the options bag.
    expect(analyzeBpm(x, SR, { maxCandidates: 2 }).candidates.length).toBeLessThanOrEqual(2);
    // nMfcc widens the MFCC matrix exposed by the timbre result.
    const timbre = analyzeTimbre(x, SR, { nFft: 1024, hopLength: 256, nMfcc: 20 });
    expect(timbre.spectralCentroid).toBeInstanceOf(Float32Array);
    // Remaining options-based entry points run without throwing on defaults+overrides.
    expect(analyzeRhythm(x, SR, { bpmMin: 80, bpmMax: 160 }).bpm).toBeGreaterThan(0);
    expect(analyzeDynamics(x, SR, { windowSec: 0.2 }).loudnessTimesSec).toBeInstanceOf(
      Float32Array,
    );
    expect(detectAcoustic(x, SR, { nOctaveBands: 4 }).rt60Bands).toBeInstanceOf(Float32Array);
  });
});
