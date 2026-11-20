/**
 * Tests for the v1.2 feature additions exposed in WASM:
 * onset envelope, Fourier tempogram, tempogram ratio, NNLS chroma,
 * and EBU R128 LUFS metering.
 */

import { beforeAll, describe, expect, it, vi } from 'vitest';
import type { PitchCorrectOptions } from '../dist/index.js';
import {
  Audio,
  analyzeMelody,
  analyzeSections,
  bassChroma,
  chromaCens,
  chromaCqt,
  cqt,
  cqtToAudio,
  ErrorCode,
  fourierTempogram,
  hybridCqt,
  init,
  lufs,
  MAX_TRACK_LANES,
  Mixer,
  melSpectrogram,
  melToAudio,
  melToStft,
  meteringSilenceRatio,
  mfcc,
  mfccToAudio,
  mfccToMel,
  mixingScenePreset,
  momentaryLufs,
  nnlsChroma,
  noteMove,
  noteStretch,
  onsetStrength,
  onsetStrengthMulti,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchPyin,
  plp,
  pseudoCqt,
  RealtimeEngine,
  StreamingEqualizer,
  shortTermLufs,
  stft,
  tempogram,
  tempogramRatio,
  voiceChange,
  vqt,
  vqtToAudio,
} from '../dist/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number, amp = 0.5): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = amp * Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

function allFinite(arr: Float32Array | number[]): boolean {
  for (const x of arr) {
    if (!Number.isFinite(x)) {
      return false;
    }
  }
  return true;
}

function expectVoicedTypeError(operation: () => unknown, fnName: string): void {
  expect(operation).toThrowError(TypeError);
  expect(operation).toThrowError(new RegExp(`^${fnName}: voiced`));
}

describe('v1.2 feature additions (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const signal = generateSine(220, SR, 3.0);

  describe('onset strength', () => {
    it('returns a finite per-frame envelope sized to the audio length', () => {
      const env = onsetStrength(signal, SR);
      // ~ ceil(signal.length / hop_length=512). Allow ±2 for librosa-style padding/centering.
      const expectedFrames = Math.ceil(signal.length / 512);
      expect(env.length).toBeGreaterThanOrEqual(expectedFrames - 2);
      expect(env.length).toBeLessThanOrEqual(expectedFrames + 2);
      expect(allFinite(env)).toBe(true);
    });

    it('returns finite multi-band onset strength aligned to frames', () => {
      const nBands = 4;
      const result = onsetStrengthMulti(signal, SR, {
        nFft: 1024,
        hopLength: 256,
        nMels: 48,
        nBands,
      });
      expect(result.rows).toBe(nBands);
      expect(result.cols).toBeGreaterThan(0);
      expect(result.data.length).toBe(result.rows * result.cols);
      expect(allFinite(result.data)).toBe(true);
      for (const v of result.data) {
        expect(v).toBeGreaterThanOrEqual(0);
      }
    });
  });

  describe('tempogram family', () => {
    it('Fourier tempogram returns an [nBins x nFrames] matrix of non-negative magnitudes', () => {
      const env = onsetStrength(signal, SR);
      const ft = fourierTempogram({ onsetEnvelope: env, sampleRate: SR });
      // Default win_length is 384 → n_bins = win_length / 2 + 1 = 193.
      expect(ft.rows).toBe(193);
      // Fourier tempogram frames align to the onset-envelope length.
      expect(ft.cols).toBe(env.length);
      expect(ft.data.length).toBe(ft.rows * ft.cols);
      expect(allFinite(ft.data)).toBe(true);
      // Magnitudes must be non-negative.
      for (const v of ft.data) {
        expect(v).toBeGreaterThanOrEqual(0);
      }
    });

    it('tempogram ratio returns one finite value per default factor', () => {
      const env = onsetStrength(signal, SR);
      const tg = tempogram({ onsetEnvelope: env, sampleRate: SR });
      const ratios = tempogramRatio({ tempogram: tg, sampleRate: SR });
      // Defaults to {0.5, 1, 2, 3, 4}.
      expect(ratios.length).toBe(5);
      expect(allFinite(ratios)).toBe(true);
      // Ratio values are normalized magnitudes — non-negative.
      for (const v of ratios) {
        expect(v).toBeGreaterThanOrEqual(0);
      }
    });

    it('tempogram ratio honors custom factors instead of silently defaulting', () => {
      const env = onsetStrength(signal, SR);
      const tg = tempogram({ onsetEnvelope: env, sampleRate: SR });
      const factors = new Float32Array([1, 2, 3]);
      const ratios = tempogramRatio({ tempogram: tg, sampleRate: SR, hopLength: 512, factors });
      // One value per requested factor — proves the argument reaches the core
      // instead of being dropped (which would yield the 5-value default).
      expect(ratios.length).toBe(3);
      expect(allFinite(ratios)).toBe(true);
      const defaults = tempogramRatio({ tempogram: tg, sampleRate: SR });
      expect(defaults.length).toBe(5);
    });

    it('plp returns a pulse curve aligned to the envelope', () => {
      const env = onsetStrength(signal, SR);
      const pulse = plp({ onsetEnvelope: env, sampleRate: SR });
      expect(pulse.length).toBe(env.length);
      expect(allFinite(pulse)).toBe(true);
    });
  });

  describe('NNLS chroma', () => {
    it('returns a 12 x nFrames matrix with normalized non-negative entries', () => {
      const result = nnlsChroma(signal, SR);
      expect(result.rows).toBe(12);
      // Aligns to the same hop grid as the onset strength (~ signal.length / 512).
      const expectedFrames = Math.ceil(signal.length / 512);
      expect(result.cols).toBeGreaterThanOrEqual(expectedFrames - 2);
      expect(result.cols).toBeLessThanOrEqual(expectedFrames + 2);
      expect(result.data.length).toBe(result.rows * result.cols);
      expect(allFinite(result.data)).toBe(true);
      // NNLS chroma is non-negative and normalized to [0, 1] per librosa convention.
      for (const v of result.data) {
        expect(v).toBeGreaterThanOrEqual(0);
        expect(v).toBeLessThanOrEqual(1.0 + 1e-5);
      }
    });

    it('computes CENS and bass chroma matrices', () => {
      const cens = chromaCens(signal, SR, { hopLength: 512, nChroma: 12 });
      expect(cens.features.rows).toBe(12);
      expect(cens.features.cols).toBeGreaterThan(0);
      expect(cens.features.data.length).toBe(cens.features.rows * cens.features.cols);
      expect(cens.sampleRate).toBe(SR);
      expect(cens.hopLength).toBe(512);
      expect(allFinite(cens.features.data)).toBe(true);
      expect(allFinite(cens.meanEnergy)).toBe(true);

      const bass = bassChroma(signal, SR, { hopLength: 512, nChroma: 12 });
      expect(bass.features.rows).toBe(12);
      expect(bass.features.cols).toBeGreaterThan(0);
      expect(bass.features.data.length).toBe(bass.features.rows * bass.features.cols);
      expect(bass.sampleRate).toBe(SR);
      expect(bass.hopLength).toBe(512);
      expect(allFinite(bass.features.data)).toBe(true);
      expect(allFinite(bass.meanEnergy)).toBe(true);
    });

    it('forwards binsPerOctave on the constant-Q chroma variants', () => {
      // These two keep the option because the entry point honours it. The bass
      // chroma does not take it at all: its bin count and lowest frequency are
      // fixed together, so there is nothing to forward.
      for (const variant of [chromaCens, chromaCqt]) {
        const base = variant({ samples: signal, sampleRate: SR });
        const coarser = variant({ samples: signal, sampleRate: SR, binsPerOctave: 12 });
        expect(coarser.features.data.length).toBe(base.features.data.length);
        expect(Array.from(coarser.features.data)).not.toEqual(Array.from(base.features.data));
      }
    });
  });

  describe('LUFS metering', () => {
    it('returns finite integrated/momentary/short-term/range values', () => {
      const result = lufs(signal, SR);
      expect(Number.isFinite(result.integratedLufs)).toBe(true);
      expect(Number.isFinite(result.momentaryLufs)).toBe(true);
      expect(Number.isFinite(result.shortTermLufs)).toBe(true);
      expect(Number.isFinite(result.loudnessRangeLu)).toBe(true);
    });

    it('reports a louder signal as higher integrated LUFS', () => {
      const quiet = lufs(generateSine(220, SR, 3.0, 0.1), SR);
      const loud = lufs(generateSine(220, SR, 3.0, 0.8), SR);
      expect(loud.integratedLufs).toBeGreaterThan(quiet.integratedLufs);
    });

    it('momentary/short-term series are non-empty, finite, and within audible LUFS range', () => {
      const mom = momentaryLufs(signal, SR);
      const st = shortTermLufs(signal, SR);
      // 3-second 0.5-amplitude tone -> at least a couple of 400ms momentary windows
      // and at least one 3s short-term window.
      expect(mom.length).toBeGreaterThan(2);
      expect(st.length).toBeGreaterThanOrEqual(1);
      expect(allFinite(mom)).toBe(true);
      expect(allFinite(st)).toBe(true);
      // Every value should land in a sensible loudness range; a 0.5-amp tone is
      // ~-9 LUFS, and -Infinity (a previous silent-input bug we want to catch)
      // would fail the > -100 check.
      for (const v of mom) {
        expect(v).toBeGreaterThan(-100);
        expect(v).toBeLessThan(0);
      }
      for (const v of st) {
        expect(v).toBeGreaterThan(-100);
        expect(v).toBeLessThan(0);
      }
    });
  });

  describe('editing DSP (pitch correct / note stretch / voice change)', () => {
    function peakAmplitude(arr: Float32Array): number {
      let p = 0;
      for (const v of arr) {
        const a = Math.abs(v);
        if (a > p) {
          p = a;
        }
      }
      return p;
    }

    function rms(arr: Float32Array): number {
      let acc = 0;
      for (const v of arr) {
        acc += v * v;
      }
      return Math.sqrt(acc / arr.length);
    }

    it('pitchCorrectToMidi preserves length and stays within audio range', () => {
      const out = pitchCorrectToMidi(signal, SR, { currentMidi: 57, targetMidi: 60 });
      expect(out).toBeInstanceOf(Float32Array);
      // Phase-vocoder pitch correction returns the input length verbatim.
      expect(out.length).toBe(signal.length);
      expect(allFinite(out)).toBe(true);
      const peak = peakAmplitude(out);
      // Input peak is 0.5; allow up to ~1.0 for pitch-correction overshoot but
      // catch obvious blow-ups / silence.
      expect(peak).toBeGreaterThan(0.05);
      expect(peak).toBeLessThanOrEqual(1.0);

      const detected = pitchPyin(out, SR, {
        frameLength: 2048,
        hopLength: 512,
        f0MinHz: 65,
        f0MaxHz: 1000,
      });
      const expectedHz = 220 * 2 ** (3 / 12);
      const centsError = 1200 * Math.log2(detected.medianF0 / expectedHz);
      expect(Math.abs(centsError)).toBeLessThan(5);
    });

    it('rejects out-of-range MIDI targets', () => {
      expect(() => pitchCorrectToMidi(signal, SR, { currentMidi: 57, targetMidi: 200 })).toThrow();
      expect(() => pitchCorrectToMidi(signal, SR, { currentMidi: 57, targetMidi: -1 })).toThrow();
      expect(() => pitchCorrectToMidi(signal, SR, { currentMidi: -1, targetMidi: 60 })).toThrow();
      expect(() => pitchCorrectToMidi(signal, SR, { currentMidi: 200, targetMidi: 60 })).toThrow();
    });

    /** Positional form over a caller-supplied F0 contour. */
    const timevarying = (
      f0Hz: Float32Array,
      hopLength: number,
      options: Partial<PitchCorrectOptions> = {},
    ) => pitchCorrectTimevarying(signal, SR, { f0Hz, hopLength, ...options });
    /** Request form of the same call. */
    const timevaryingRequest = (
      f0Hz: Float32Array,
      hopLength: number,
      options: Partial<PitchCorrectOptions> = {},
    ) => pitchCorrectTimevarying({ samples: signal, sampleRate: SR, f0Hz, hopLength, ...options });

    it('pitchCorrectTimevarying follows a caller-supplied F0 contour', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const out = timevarying(f0, hop, { targetMidi: 60 });
      expect(out).toBeInstanceOf(Float32Array);
      expect(out.length).toBe(signal.length);
      expect(allFinite(out)).toBe(true);

      // Optional voiced / voicedProb arrays are accepted.
      const voiced = new Int32Array(nFrames).fill(1);
      const voicedProb = new Float32Array(nFrames).fill(1);
      const out2 = timevarying(f0, hop, { targetMidi: 60, voiced, voicedProb });
      expect(out2.length).toBe(signal.length);

      // pYIN emits NaN F0 for unvoiced frames by default.
      f0[0] = Number.NaN;
      voiced[0] = 0;
      const pyinOut = timevarying(f0, hop, { targetMidi: 60, voiced, voicedProb });
      expect(allFinite(pyinOut)).toBe(true);
    });

    it('pitchCorrectTimevarying takes a PitchResult voicedFlag array directly', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);

      // pitchPyin reports voicing as boolean[]; feeding it straight back in
      // must behave exactly like the equivalent Int32Array.
      const flags = Array.from({ length: nFrames }, (_, i) => i % 3 !== 0);
      const numeric = Int32Array.from(flags, (voiced) => (voiced ? 1 : 0));

      const fromBooleans = timevarying(f0, hop, { targetMidi: 60, voiced: flags });
      const fromInt32 = timevarying(f0, hop, { targetMidi: 60, voiced: numeric });
      expect(Array.from(fromBooleans)).toEqual(Array.from(fromInt32));

      // The flags must actually reach the core rather than being dropped.
      const allVoiced = timevarying(f0, hop, { targetMidi: 60 });
      expect(fromBooleans.some((x, i) => Math.abs(x - allVoiced[i]) > 1e-6)).toBe(true);

      // The request form agrees with the positional form.
      const requestForm = timevaryingRequest(f0, hop, { targetMidi: 60, voiced: flags });
      const requestNumeric = timevaryingRequest(f0, hop, { targetMidi: 60, voiced: numeric });
      expect(Array.from(requestForm)).toEqual(Array.from(fromBooleans));
      expect(Array.from(requestNumeric)).toEqual(Array.from(fromInt32));
    });

    it('pitchCorrectTimevarying supports scale mode and retune-strength knobs', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);

      const base = timevarying(f0, hop);
      expect(base.length).toBe(signal.length);
      expect(allFinite(base)).toBe(true);

      const scaled = timevarying(f0, hop, { targetMode: 'scale', scaleRoot: 0 });
      expect(scaled.length).toBe(signal.length);
      expect(allFinite(scaled)).toBe(true);

      const full = timevarying(f0, hop, { targetMidi: 59, retuneAmount: 1 });
      const gentle = timevarying(f0, hop, { targetMidi: 59, retuneAmount: 0.25 });
      expect(full.some((x, i) => Math.abs(x - gentle[i]) > 1e-6)).toBe(true);

      expect(() => timevarying(f0, hop, { targetMidi: 59, retuneAmount: 2 })).toThrow();
      expect(() => timevarying(f0, hop, { targetMidi: 200 })).toThrow();
      expect(() => timevarying(f0, hop, { targetMode: 'Scale' as never })).toThrow(RangeError);
    });

    it('pitchCorrectTimevarying rejects mismatched companion arrays', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const tooShortVoiced = new Int32Array(nFrames - 1).fill(1);
      const tooShortProb = new Float32Array(nFrames - 1).fill(1);
      expect(() => timevarying(f0, hop, { targetMidi: 60, voiced: tooShortVoiced })).toThrow(
        'pitchCorrectTimevarying: voiced must have the same length as f0Hz',
      );
      expect(() => timevarying(f0, hop, { targetMidi: 60, voicedProb: tooShortProb })).toThrow(
        'pitchCorrectTimevarying: voicedProb must have the same length as f0Hz',
      );
      expect(() => timevaryingRequest(f0, hop, { voiced: tooShortVoiced })).toThrow(
        'pitchCorrectTimevarying: voiced must have the same length as f0Hz',
      );
      expect(() => timevaryingRequest(f0, hop, { voicedProb: tooShortProb })).toThrow(
        'pitchCorrectTimevarying: voicedProb must have the same length as f0Hz',
      );
    });

    it('pitchCorrectTimevarying derives voicing from voicedProb when voiced is omitted', () => {
      // voicedProb below 0.5 must behave exactly like an explicit all-unvoiced
      // array, and at/above 0.5 exactly like an explicit all-voiced one --
      // never like "every frame voiced" regardless of what voicedProb says.
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const lowProb = new Float32Array(nFrames).fill(0.1);
      const highProb = new Float32Array(nFrames).fill(0.9);
      const allUnvoiced = new Int32Array(nFrames).fill(0);
      const allVoiced = new Int32Array(nFrames).fill(1);
      const midi = { targetMidi: 60 };

      const viaLowProb = timevarying(f0, hop, { ...midi, voicedProb: lowProb });
      const viaExplicitUnvoiced = timevarying(f0, hop, { ...midi, voiced: allUnvoiced });
      expect(Array.from(viaLowProb)).toEqual(Array.from(viaExplicitUnvoiced));

      const viaHighProb = timevarying(f0, hop, { ...midi, voicedProb: highProb });
      const viaExplicitVoiced = timevarying(f0, hop, { ...midi, voiced: allVoiced });
      expect(Array.from(viaHighProb)).toEqual(Array.from(viaExplicitVoiced));

      // The two derived answers must actually differ, or this proves nothing.
      expect(viaLowProb.some((x, i) => Math.abs(x - viaHighProb[i]) > 1e-6)).toBe(true);

      // The request form must agree.
      const optLow = timevaryingRequest(f0, hop, { ...midi, voicedProb: lowProb });
      const optUnvoiced = timevaryingRequest(f0, hop, { ...midi, voiced: allUnvoiced });
      expect(Array.from(optLow)).toEqual(Array.from(optUnvoiced));
      expect(Array.from(optLow)).toEqual(Array.from(viaLowProb));
    });

    it('treats a null voiced field as omitted while using voicedProb', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const voicedProb = new Float32Array(nFrames).fill(0.9);

      const positionalOmitted = timevarying(f0, hop, { targetMidi: 60, voicedProb });
      // @ts-expect-error null is not in the type; the runtime still reads it as omitted.
      const positionalNull = timevarying(f0, hop, { targetMidi: 60, voiced: null, voicedProb });
      expect(Array.from(positionalNull)).toEqual(Array.from(positionalOmitted));

      const optionsOmitted = timevaryingRequest(f0, hop, { targetMidi: 60, voicedProb });
      const optionsNull = timevaryingRequest(f0, hop, {
        targetMidi: 60,
        // @ts-expect-error null is not in the type; the runtime still reads it as omitted.
        voiced: null,
        voicedProb,
      });
      expect(Array.from(optionsNull)).toEqual(Array.from(optionsOmitted));
    });

    it('explicit voiced flags ignore invalid voicedProb shape and values', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const voiced = new Int32Array(nFrames).fill(1);
      const withoutProb = timevarying(f0, hop, { targetMidi: 60, voiced });
      const generalWithoutProb = timevaryingRequest(f0, hop, { voiced });

      // Once voiced is supplied, voicedProb is ignored completely, including
      // its shape and value-domain checks.
      const invalidProbabilities = [
        new Float32Array(nFrames + 1).fill(Number.NaN),
        new Float32Array(nFrames).fill(-0.1),
        new Float32Array(nFrames).fill(1.1),
      ];
      for (const invalidProb of invalidProbabilities) {
        const withInvalidProb = timevarying(f0, hop, {
          targetMidi: 60,
          voiced,
          voicedProb: invalidProb,
        });
        const generalWithInvalidProb = timevaryingRequest(f0, hop, {
          voiced,
          voicedProb: invalidProb,
        });
        expect(Array.from(withInvalidProb)).toEqual(Array.from(withoutProb));
        expect(Array.from(generalWithInvalidProb)).toEqual(Array.from(generalWithoutProb));
      }

      // With no explicit flags, invalid probabilities remain rejected by the
      // probability-only path.
      for (const invalid of [Number.NaN, -0.1, 1.1]) {
        expect(() =>
          timevarying(f0, hop, {
            targetMidi: 60,
            voicedProb: new Float32Array(nFrames).fill(invalid),
          }),
        ).toThrow();
        expect(() =>
          timevaryingRequest(f0, hop, { voicedProb: new Float32Array(nFrames).fill(invalid) }),
        ).toThrow();
      }
    });

    it('rejects unsupported voiced containers before native pitch correction', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const invalid = [
        '1'.repeat(nFrames),
        new Float64Array(nFrames).fill(1),
        { length: nFrames, 0: 1 },
        Array<string>(nFrames).fill('0'),
        Array<object>(nFrames).fill({}),
      ];

      for (const voiced of invalid) {
        expectVoicedTypeError(
          () => timevarying(f0, hop, { targetMidi: 60, voiced: voiced as never }),
          'pitchCorrectTimevarying',
        );
        expectVoicedTypeError(
          () => timevaryingRequest(f0, hop, { voiced: voiced as never }),
          'pitchCorrectTimevarying',
        );
      }
    });

    it('rejects unsupported voiced containers before checking their length', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const invalid = ['1', new Float64Array(nFrames - 1).fill(1), {}];

      for (const voiced of invalid) {
        expectVoicedTypeError(
          () => timevarying(f0, hop, { targetMidi: 60, voiced: voiced as never }),
          'pitchCorrectTimevarying',
        );
        expectVoicedTypeError(
          () => timevaryingRequest(f0, hop, { voiced: voiced as never }),
          'pitchCorrectTimevarying',
        );
      }
    });

    it('noteStretch lengthens the buffer by the stretch ratio', () => {
      const out = noteStretch(signal, SR, {
        onsetSample: 0,
        offsetSample: signal.length,
        stretchRatio: 1.5,
      });
      expect(out).toBeInstanceOf(Float32Array);
      expect(allFinite(out)).toBe(true);
      // Stretching by 1.5 must lengthen the output (allow ±2% for windowing).
      const expected = Math.round(signal.length * 1.5);
      expect(out.length).toBeGreaterThanOrEqual(Math.floor(expected * 0.98));
      expect(out.length).toBeLessThanOrEqual(Math.ceil(expected * 1.02));
      const peak = peakAmplitude(out);
      expect(peak).toBeGreaterThan(0.05);
      expect(peak).toBeLessThanOrEqual(1.0);
    });

    it('noteStretch and noteMove default offset to the input length', () => {
      const stretched = noteStretch(signal, SR, { stretchRatio: 1.5 });
      const moved = noteMove(signal, SR, { targetOnsetSample: 500 });
      expect(stretched).toBeInstanceOf(Float32Array);
      expect(stretched.length).toBeGreaterThan(0);
      expect(moved).toBeInstanceOf(Float32Array);
      expect(moved.length).toBe(signal.length);
    });

    it('noteMove preserves length while relocating a note region', () => {
      const out = noteMove(signal, SR, {
        onsetSample: 100,
        offsetSample: 1000,
        targetOnsetSample: 500,
      });
      expect(out).toBeInstanceOf(Float32Array);
      expect(out.length).toBe(signal.length);
      expect(allFinite(out)).toBe(true);
    });

    it('voiceChange formantMode defaults to relative and absolute changes the result', () => {
      const options = { pitchSemitones: 3, formantFactor: 1.1 };
      const byDefault = voiceChange(signal, SR, options);
      expect(voiceChange(signal, SR, { ...options, formantMode: 'relative' })).toEqual(byDefault);
      const absolute = voiceChange(signal, SR, { ...options, formantMode: 'absolute' });
      expect(absolute.length).toBe(byDefault.length);
      expect(absolute).not.toEqual(byDefault);
      expect(voiceChange(signal, SR, { formantMode: 'absolute' })).toEqual(signal);
    });

    it('voiceChange absolute mode refuses a factor the warp cannot reach', () => {
      // 2^(4/12) = 1.2599, so the warp range [0.55, 1.65] reaches [0.693, 2.079], printed inward.
      const request = { pitchSemitones: 4, formantFactor: 2.5, formantMode: 'absolute' as const };
      expect(() => voiceChange(signal, SR, request)).toThrow(RangeError);
      expect(() => voiceChange(signal, SR, request)).toThrow(/\[0\.693, 2\.078\]$/);
      expect(() =>
        voiceChange(signal, SR, { formantMode: 'sideways' as unknown as 'relative' }),
      ).toThrow(RangeError);
    });

    it('voiceChange refuses factors 0, -2, 10 and NaN as RangeError in both formant modes', () => {
      for (const formantMode of ['relative', 'absolute'] as const) {
        // The control: an in-range factor is accepted in this mode.
        expect(voiceChange(signal, SR, { formantFactor: 1.1, formantMode }).length).toBe(
          signal.length,
        );
        for (const formantFactor of [0, -2, 10, Number.NaN]) {
          expect(() => voiceChange(signal, SR, { formantFactor, formantMode })).toThrow(RangeError);
        }
      }
    });

    it('voiceChange preserves length and level', () => {
      const out = voiceChange(signal, SR, { pitchSemitones: 2, formantFactor: 1.1 });
      expect(out).toBeInstanceOf(Float32Array);
      expect(allFinite(out)).toBe(true);
      // Voice changer is duration-preserving; allow a tiny boundary slack.
      expect(out.length).toBeGreaterThanOrEqual(signal.length - 4);
      expect(out.length).toBeLessThanOrEqual(signal.length + 4);
      // Level is an RMS property here, not a peak one: the formant stage
      // recolours the residual with a zero-phase envelope, which reshapes the
      // waveform and raises the crest factor of a tone well above the 1.41 of a
      // sine, so its peak legitimately passes the input peak. The stage is not a
      // limiter and does not clamp. What must hold is that the energy tracks the
      // input rather than collapsing by the LPC prediction gain.
      expect(peakAmplitude(out)).toBeGreaterThan(0.05);
      const deltaDb = 20 * Math.log10(rms(out) / rms(signal));
      expect(Math.abs(deltaDb)).toBeLessThan(1.5);
    });

    it('exposes the editing methods on the Audio class with the same shapes', () => {
      const audio = Audio.fromBuffer(signal, SR);
      const corrected = audio.pitchCorrectToMidi({ currentMidi: 57, targetMidi: 60 });
      const stretched = audio.noteStretch({
        onsetSample: 0,
        offsetSample: signal.length,
        stretchRatio: 1.5,
      });
      const voiced = audio.voiceChange({ pitchSemitones: 2, formantFactor: 1.1 });
      expect(corrected.length).toBe(signal.length);
      expect(stretched.length).toBeGreaterThan(signal.length);
      expect(voiced.length).toBeGreaterThanOrEqual(signal.length - 4);
      expect(allFinite(corrected)).toBe(true);
      expect(allFinite(stretched)).toBe(true);
      expect(allFinite(voiced)).toBe(true);
    });
  });

  describe('Mixer (scene-based routing)', () => {
    const BLOCK = 512;

    function blockEnergy(r: { left: Float32Array; right: Float32Array }): number {
      let sum = 0;
      for (let i = 0; i < r.left.length; i++) {
        sum += r.left[i] * r.left[i] + r.right[i] * r.right[i];
      }
      return sum;
    }

    it('routes a send through reverb and back to master', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: 48000,
        maxBlockSize: BLOCK,
      });
      mixer.compile();

      // Strip 0 = vocal, strip 1 = reverb return. Impulse into vocal, silence into return.
      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(BLOCK);
      const silentR = new Float32Array(BLOCK);

      const energies: number[] = [];
      for (let block = 0; block < 16; block++) {
        const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
        energies.push(blockEnergy(out));
        vocalL[0] = 0.0;
        vocalR[0] = 0.0;
      }

      // Block 0 carries the dry hit; later blocks carry the reverb tail.
      expect(energies[0]).toBeGreaterThan(1e-6);
      const tail = energies.slice(4).reduce((a, b) => a + b, 0);
      expect(tail).toBeGreaterThan(1e-6);
      const master = mixer.busMeter('master');
      expect(master.numChannels).toBe(2);
      expect(Number.isFinite(master.peakDb[0])).toBe(true);

      mixer.destroy();
    });

    it('schedules insert-parameter automation without throwing', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: 48000,
        maxBlockSize: BLOCK,
      });
      mixer.compile();

      expect(mixer.stripCount).toBeGreaterThan(0);

      // Strip 0 (vocal) has pre-fader inserts (insert 0 = eq.parametric).
      // Schedule a linear ramp on param 0 over the first second of audio.
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, {
          paramId: 0,
          samplePos: 0,
          value: 0.0,
          curve: 'linear',
        }),
      ).not.toThrow();
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, {
          paramId: 0,
          samplePos: 48000,
          value: 1.0,
          curve: 'exponential',
        }),
      ).not.toThrow();

      // Out-of-range strip index must throw.
      expect(() =>
        mixer.scheduleInsertAutomation(999, 0, { paramId: 0, samplePos: 0, value: 0.0 }),
      ).toThrow();

      // Processing after scheduling still produces output.
      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(BLOCK);
      const silentR = new Float32Array(BLOCK);
      const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      mixer.destroy();
    });

    it('round-trips the scene topology to JSON', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: 48000,
        maxBlockSize: BLOCK,
      });
      const scene = mixer.toJson();
      expect(scene).toContain('"vocal-verb"');
      expect(scene).toContain('"vocal-verb-return"');
      expect(scene).toContain('"destinationBusId":"vocal-verb"');

      const restored = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: BLOCK });
      restored.compile();
      restored.destroy();
      mixer.destroy();
    });

    it('processes 128-sample render quanta through the single WASM module', () => {
      const quantum = 128;
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: 48000,
        maxBlockSize: quantum,
      });
      mixer.compile();

      const vocalL = new Float32Array(quantum);
      const vocalR = new Float32Array(quantum);
      const returnL = new Float32Array(quantum);
      const returnR = new Float32Array(quantum);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const out = mixer.processStereo([vocalL, returnL], [vocalR, returnR]);
      expect(out.left.length).toBe(quantum);
      expect(out.right.length).toBe(quantum);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      mixer.destroy();
    });

    it('can render a block into caller-owned output arrays', () => {
      const quantum = 128;
      const scene = JSON.stringify(mixingScenePreset('vocalReverbSend'));
      const offline = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: quantum });
      const inplace = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: quantum });
      offline.compile();
      inplace.compile();

      const vocalL = new Float32Array(quantum);
      const vocalR = new Float32Array(quantum);
      const returnL = new Float32Array(quantum);
      const returnR = new Float32Array(quantum);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const outL = new Float32Array(quantum);
      const outR = new Float32Array(quantum);
      inplace.processStereoInto([vocalL, returnL], [vocalR, returnR], outL, outR);

      expect(Array.from(outL)).toEqual(Array.from(expected.left));
      expect(Array.from(outR)).toEqual(Array.from(expected.right));

      offline.destroy();
      inplace.destroy();
    });

    it('bulk-copies processStereoInto with frame-independent boundary calls', () => {
      const quantum = 128;
      const scene = JSON.stringify(mixingScenePreset('vocalReverbSend'));
      const reference = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: quantum });
      const inplace = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: quantum });
      reference.compile();
      inplace.compile();

      const makeInputs = (length: number) => {
        const vocalL = new Float32Array(length);
        const vocalR = new Float32Array(length);
        const returnL = new Float32Array(length);
        const returnR = new Float32Array(length);
        if (length > 0) {
          vocalL[0] = 1.0;
          vocalR[0] = 1.0;
        }
        return { left: [vocalL, returnL], right: [vocalR, returnR] };
      };

      try {
        // Establish the same DSP state on both mixers before spying on the
        // native TypedArray method. processStereo's own result copies happen
        // before the spy, so only processStereoInto's bridge calls are counted.
        const empty = makeInputs(0);
        const short = makeInputs(1);
        const long = makeInputs(quantum);
        const expectedShort = reference.processStereo(short.left, short.right);
        const expectedLong = reference.processStereo(long.left, long.right);
        const emptyOutL = new Float32Array(0);
        const emptyOutR = new Float32Array(0);
        const setSpy = vi.spyOn(Float32Array.prototype, 'set');
        try {
          inplace.processStereoInto(empty.left, empty.right, emptyOutL, emptyOutR);
          expect(setSpy).not.toHaveBeenCalled();
          setSpy.mockClear();

          const shortOutL = new Float32Array(1);
          const shortOutR = new Float32Array(1);
          inplace.processStereoInto(short.left, short.right, shortOutL, shortOutR);
          expect(Array.from(shortOutL)).toEqual(Array.from(expectedShort.left));
          expect(Array.from(shortOutR)).toEqual(Array.from(expectedShort.right));
          expect(setSpy).toHaveBeenCalledTimes(2 * inplace.stripCount + 2);
          setSpy.mockClear();

          const longOutL = new Float32Array(quantum);
          const longOutR = new Float32Array(quantum);
          inplace.processStereoInto(long.left, long.right, longOutL, longOutR);
          expect(Array.from(longOutL)).toEqual(Array.from(expectedLong.left));
          expect(Array.from(longOutR)).toEqual(Array.from(expectedLong.right));
          expect(setSpy).toHaveBeenCalledTimes(2 * inplace.stripCount + 2);
        } finally {
          // Always restore the native method, even when a parity assertion
          // fails, so later tests cannot inherit a spied TypedArray prototype.
          setSpy.mockRestore();
        }
      } finally {
        reference.destroy();
        inplace.destroy();
      }
    });

    it('can render through reusable WASM-heap realtime buffers', () => {
      const quantum = 128;
      const scene = JSON.stringify(mixingScenePreset('vocalReverbSend'));
      const offline = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: quantum });
      const realtime = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: quantum });
      offline.compile();
      realtime.compile();

      const vocalL = new Float32Array(quantum);
      const vocalR = new Float32Array(quantum);
      const returnL = new Float32Array(quantum);
      const returnR = new Float32Array(quantum);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const buffer = realtime.createRealtimeBuffer();
      buffer.leftInputs[0].set(vocalL);
      buffer.rightInputs[0].set(vocalR);
      buffer.leftInputs[1].set(returnL);
      buffer.rightInputs[1].set(returnR);
      buffer.process();

      expect(Array.from(buffer.outLeft)).toEqual(Array.from(expected.left));
      expect(Array.from(buffer.outRight)).toEqual(Array.from(expected.right));

      offline.destroy();
      realtime.destroy();
    });

    it('exposes live mixer controls and meter readers', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: 48000,
        maxBlockSize: BLOCK,
      });
      const vocalIndex = mixer.stripById('vocal');
      expect(vocalIndex).toBe(0);
      // An unknown id is refused rather than resolved to a sentinel.
      expect(() => mixer.stripById('definitely-not-a-strip')).toThrow(RangeError);
      const vocal = vocalIndex as number;

      mixer.setSoloSafe(vocal, true);
      mixer.setSoloed(vocal, false);
      mixer.setPolarityInvert(vocal, { left: false, right: true });
      mixer.setPanLaw(vocal, 'linear0Db');
      mixer.setChannelDelaySamples(vocal, 0);
      mixer.setVcaOffsetDb(vocal, -1);
      mixer.setDualPan(vocal, -0.2, 0.2);
      const sendIndex = mixer.addSend(vocal, {
        id: 'wasm-extra-send',
        destinationBusId: 'vocal-verb',
        sendDb: -24,
        timing: 'postFader',
      });
      mixer.setSendDb(vocal, sendIndex, -18);
      mixer.scheduleFaderAutomation(vocal, { samplePos: 0, faderDb: -6, curve: 'linear' });
      mixer.schedulePanAutomation(vocal, { samplePos: 0, pan: 0, curve: 'linear' });
      mixer.scheduleWidthAutomation(vocal, { samplePos: 0, width: 1, curve: 'linear' });
      mixer.scheduleSendAutomation(vocal, sendIndex, {
        samplePos: 0,
        sendDb: -12,
        curve: 'linear',
      });
      mixer.compile();

      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      const returnL = new Float32Array(BLOCK);
      const returnR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const out = mixer.processStereo([vocalL, returnL], [vocalR, returnR]);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      expect(Number.isFinite(mixer.stripMeter(vocal, 'preFader').peakDbL)).toBe(true);
      expect(Number.isFinite(mixer.stripMeter(vocal, 'postFader').rmsDbL)).toBe(true);
      expect(mixer.readGoniometerLatest(vocal, 8).length).toBeGreaterThan(0);

      mixer.destroy();
    });
  });

  describe('Audio class methods', () => {
    it('exposes onsetStrength/nnlsChroma/lufs/momentaryLufs/shortTermLufs with finite values', () => {
      const audio = Audio.fromBuffer(signal, SR);
      const env = audio.onsetStrength();
      expect(env.length).toBeGreaterThan(0);
      expect(allFinite(env)).toBe(true);

      const nc = audio.nnlsChroma();
      expect(nc.rows).toBe(12);
      expect(nc.data.length).toBe(nc.rows * nc.cols);
      expect(allFinite(nc.data)).toBe(true);

      const integrated = audio.lufs().integratedLufs;
      expect(Number.isFinite(integrated)).toBe(true);
      expect(integrated).toBeGreaterThan(-100);
      expect(integrated).toBeLessThan(0);

      const mom = audio.momentaryLufs();
      const st = audio.shortTermLufs();
      expect(mom.length).toBeGreaterThan(0);
      expect(st.length).toBeGreaterThan(0);
      expect(allFinite(mom)).toBe(true);
      expect(allFinite(st)).toBe(true);
    });
  });

  describe('RealtimeEngine parameter/transport parity', () => {
    it('honors tunable queue capacities and reads back transport state', () => {
      const engine = new RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 32,
        telemetryCapacity: 32,
      });
      engine.setTempo(120);
      engine.addParameter({
        id: 5,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 'linear', // canonical AutomationCurve::Linear
      });

      // Sample-accurate + smoothed parameter changes queue without throwing.
      expect(() => engine.setParameter(5, 6)).not.toThrow();
      expect(() => engine.setParameterSmoothed(5, -3)).not.toThrow();
      expect(engine.midiCcBindingCount).toBe(0);
      expect(() =>
        engine.bindMidiCc({ channel: 0, ccNumber: 74, paramId: 5, minValue: -60, maxValue: 12 }),
      ).not.toThrow();
      expect(engine.midiCcBindingCount).toBe(1);
      expect(() =>
        engine.bindMidiCc({
          ccNumber: 1,
          ccLsbNumber: 33,
          channel: 0,
          kind: 'controlChange14',
          paramId: 6,
          minValue: 0,
          maxValue: 1,
        }),
      ).not.toThrow();
      expect(engine.midiCcBindingCount).toBe(2);
      engine.clearMidiCcBindings();
      expect(engine.midiCcBindingCount).toBe(0);
      expect(() => engine.setMidiFx(0, { transposeSemitones: 12 })).not.toThrow();
      expect(() =>
        engine.setMidiFx(0, {
          arpeggiatorIntervals: [0, 12],
          arpeggiatorStepPpq: 0.25,
          arpeggiatorGatePpq: 0.125,
        }),
      ).not.toThrow();
      expect(() => engine.clearMidiFx(0)).not.toThrow();
      expect(() => engine.setMidiFx(0, '{}' as never)).toThrow(TypeError);
      const refusals: [Record<string, unknown>, string][] = [
        [{ quantizePpq: 0 }, 'quantizePpq must be a positive PPQ length in range'],
        [{ quantizePpq: 1e300 }, 'quantizePpq must be a positive PPQ length in range'],
        [{ transposeSemitones: 1e100 }, 'transposeSemitones must be a whole number in range'],
        [{ chordIntervals: [0, 7.5] }, 'chordIntervals[1] must be a whole number'],
        [
          { arpeggiatorIntervals: [], arpeggiatorStepPpq: 0.25 },
          'arpeggiatorIntervals must hold 1 to 16 intervals',
        ],
        [
          { arpeggiatorIntervals: [0], arpeggiatorStepPpq: 0 },
          'arpeggiatorStepPpq must be a positive PPQ length in range',
        ],
      ];
      for (const [config, message] of refusals) {
        expect(() => engine.setMidiFx(0, config as never)).toThrow(
          expect.objectContaining({
            code: ErrorCode.InvalidFormat,
            message: `RealtimeEngine.setMidiFx: ${message}`,
          }),
        );
      }
      engine.setMidiInputSource(0);
      expect(engine.midiInputPendingCount).toBe(0);
      engine.pushMidiInputNoteOn(0, 0, 60, 100, 3);
      expect(engine.midiInputPendingCount).toBe(1);

      engine.play();
      engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(engine.midiInputPendingCount).toBe(0);
      engine.clearMidiInputSource();
      expect(() => engine.pushMidiInputNoteOff(0, 0, 60, 0, 0)).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );

      const state = engine.transportState();
      expect(state.playing).toBe(true);
      expect(state.sampleRate).toBe(48000);
      expect(state.bpm).toBe(120);
      expect(state.samplePosition).toBe(128);
      // Musical beat readout: `beat` is one-based, `beatFraction` in [0, 1).
      expect(Number.isInteger(state.beat)).toBe(true);
      expect(state.beat).toBeGreaterThanOrEqual(1);
      expect(state.beatFraction).toBeGreaterThanOrEqual(0);
      expect(state.beatFraction).toBeLessThan(1);
      engine.destroy();
    });

    it('reports external-destination table overflow and validates smoothing ms', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      for (let id = 0; id < 16; id++) {
        engine.setMidiDestinationExternal(id, true);
      }
      expect(() => engine.setMidiDestinationExternal(3, true)).not.toThrow();
      expect(() => engine.setMidiDestinationExternal(99, true)).toThrow();
      engine.setMidiDestinationExternal(3, false);
      expect(() => engine.setMidiDestinationExternal(99, true)).not.toThrow();

      expect(() => engine.setParamSmoothingMs(0)).not.toThrow();
      expect(() => engine.setParamSmoothingMs(75)).not.toThrow();
      expect(() => engine.setParamSmoothingMs(-1)).toThrow();
      expect(() => engine.setParamSmoothingMs(Number.NaN)).toThrow();
      engine.destroy();
    });

    it('accepts only a stereo lane sourceChannelLayout and rejects an out-of-range one', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      // Stereo and an omitted layout are accepted; mono, 5.1 and 7.1 are refused.
      expect(() =>
        engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 'stereo' }]),
      ).not.toThrow();
      for (const layout of ['mono', '5.1', '7.1'] as const) {
        expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: layout }])).toThrow(
          expect.objectContaining({
            code: ErrorCode.NotSupported,
            message: expect.stringMatching(
              /: lanes\[0\]\.sourceChannelLayout other than stereo is not supported$/,
            ),
          }),
        );
      }
      expect(() => engine.setTrackLanes([{ trackId: 10 }])).not.toThrow();
      // The lane cap is exported and its refusal names it.
      const lanes = Array.from({ length: MAX_TRACK_LANES + 1 }, (_, i) => ({ trackId: i + 1 }));
      expect(MAX_TRACK_LANES).toBe(32);
      expect(() => engine.setTrackLanes(lanes.slice(0, MAX_TRACK_LANES))).not.toThrow();
      expect(() => engine.setTrackLanes(lanes)).toThrow(RangeError);
      expect(() => engine.setTrackLanes(lanes)).toThrow(
        /: laneCount must be an integer in \[0, 32\]$/,
      );
      expect(() => engine.setTrackLanes([{ trackId: 10 }])).not.toThrow();
      // An unknown name is a RangeError and an ordinal, wrapping ones included
      // (257 narrows to 1 if the bound is checked after truncating to uint8_t),
      // is a TypeError.
      expect(() =>
        engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 'quad' as never }]),
      ).toThrow(RangeError);
      for (const ordinal of [1, 9, 257, -255]) {
        expect(() =>
          engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: ordinal as never }]),
        ).toThrow(TypeError);
      }
      engine.destroy();
    });

    it('accepts a bus channelLayout and rejects an out-of-range or wrapping one', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: '5.1' }])).not.toThrow();
      expect(() => engine.setTrackBuses([{ busId: 1 }])).not.toThrow();
      expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: 'quad' as never }])).toThrow(
        RangeError,
      );
      for (const ordinal of [2, 9, 257, -255]) {
        expect(() => engine.setTrackBuses([{ busId: 1, channelLayout: ordinal as never }])).toThrow(
          TypeError,
        );
      }
      engine.destroy();
    });

    it('settles smoothed lane faders before an offline render starts', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      engine.setTempo(120);
      engine.setTrackLanes([{ trackId: 10 }]);
      const frames = 128 * 8;
      const ones = new Float32Array(frames).fill(1);
      engine.setClips([
        { trackId: 10, channels: [ones, ones], startPpq: 0, lengthSamples: frames },
      ]);
      // Track 10's fader held at -60 dB from ppq 0.
      engine.setAutomationLane(engine.resolveStripAutomationId(10, 'faderDb'), [
        { ppq: 0, value: -60, curveToNext: 'hold' },
      ]);
      engine.seekSample(0);
      // Priming block with the transport stopped applies the automation target;
      // settleParameters snaps the lane fader smoother to it so the first
      // audible frame renders at -60 dB instead of ramping down from 0 dB.
      engine.process([new Float32Array(128), new Float32Array(128)]);
      engine.settleParameters();
      engine.play();
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(Math.abs(out[0][0])).toBeLessThan(0.01);
      expect(Math.abs(out[0][127])).toBeLessThan(0.01);
      engine.destroy();
    });

    it('rejects non-finite automation breakpoints like the C ABI oracle', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      // Seed a valid lane so failed replacements must leave published state intact.
      engine.setAutomationLane(0x4d58ff01, [{ ppq: 0, value: -6, curveToNext: 'linear' }]);
      expect(engine.automationLaneCount).toBe(1);
      // Non-finite ppq is rejected.
      expect(() =>
        engine.setAutomationLane(0x4d58ff01, [
          { ppq: Number.NaN, value: -6, curveToNext: 'linear' },
        ]),
      ).toThrow();
      // Non-finite value is rejected.
      expect(() =>
        engine.setAutomationLane(0x4d58ff01, [
          { ppq: 0, value: Number.POSITIVE_INFINITY, curveToNext: 'linear' },
        ]),
      ).toThrow();
      expect(engine.automationLaneCount).toBe(1);
      engine.destroy();
    });

    it('rejects an out-of-range automation curve ordinal instead of clamping it', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      // An unknown curve name is a RangeError and an ordinal a TypeError; neither
      // is clamped to Linear.
      expect(() =>
        engine.setAutomationLane(0x4d58ff01, [
          { ppq: 0, value: -6, curveToNext: 'bounce' as never },
        ]),
      ).toThrow(RangeError);
      expect(() =>
        engine.setAutomationLane(0x4d58ff01, [{ ppq: 0, value: -6, curveToNext: 4 as never }]),
      ).toThrow(TypeError);
      // The same refusals apply to a defaultCurve at addParameter.
      expect(() =>
        engine.addParameter({
          id: 9,
          name: 'gain',
          unit: 'dB',
          minValue: -60,
          maxValue: 12,
          defaultValue: 0,
          rtSafe: true,
          defaultCurve: 5 as never,
        }),
      ).toThrow(TypeError);
      engine.destroy();
    });

    it('preserves parameters and markers after transactional setter failures', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      const parameter = {
        id: 17,
        name: 'original gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: -3,
        rtSafe: true,
        defaultCurve: 'linear' as const,
      };
      engine.addParameter(parameter);
      expect(() =>
        engine.addParameter({
          ...parameter,
          name: 'replacement mode',
          unit: 'steps',
          minValue: 0,
          maxValue: 3,
          defaultValue: 2,
          rtSafe: false,
          defaultCurve: 'hold',
        }),
      ).toThrow();
      expect(engine.parameterInfo(17)).toEqual(parameter);

      engine.setMarkers([
        { id: 11, ppq: 1, name: 'intro' },
        { id: 12, ppq: 2, name: 'chorus' },
      ]);
      expect(() =>
        engine.setMarkers([
          { id: 21, ppq: 0.5, name: 'verse' },
          { id: 22, ppq: Number.NaN, name: 'bridge' },
        ]),
      ).toThrow();
      expect(engine.markers()).toHaveLength(2);
      expect(engine.markers()[0]).toEqual({
        id: 11,
        ppq: 1,
        name: 'intro',
        kind: 'marker',
        keyFifths: 0,
        keyMinor: false,
      });
      expect(engine.marker(12).name).toBe('chorus');
      expect(() => engine.setMarkers([{ id: 0, ppq: 3, name: 'invalid id' }])).toThrow();
      expect(() =>
        engine.setMarkers([
          { id: 31, ppq: 3, name: 'duplicate a' },
          { id: 31, ppq: 4, name: 'duplicate b' },
        ]),
      ).toThrow();
      expect(engine.markers()).toHaveLength(2);
      expect(engine.marker(11).name).toBe('intro');
      engine.destroy();
    });

    it('routes a lane through its group bus and ducks a lane from a sidechain key', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      engine.setTempo(120);
      const frames = 128 * 64;
      const pad = new Float32Array(frames).fill(0.5);
      const key = new Float32Array(frames).fill(1.0);
      engine.setTrackBuses([{ busId: 5, gainDb: -6.0206 }]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 5 }, { trackId: 20 }]);
      engine.setClips([
        { trackId: 10, channels: [pad, pad], startPpq: 0, lengthSamples: frames },
        { trackId: 20, channels: [key, key], startPpq: 0, lengthSamples: frames },
      ]);
      // Ducking insert on the grouped lane keyed from the other lane.
      engine.setStrip(10, {
        id: 'track-10',
        inserts: [
          {
            slot: 'post',
            processor: 'dynamics.duckingProcessor',
            params: { thresholdDb: -30, ratio: 20, attackMs: 1, releaseMs: 50, rangeDb: 24 },
          },
        ],
      } as never);
      engine.setStripSidechain(10, 0, { kind: 'track', id: 20 });
      // Keep the key lane out of the measured mix; the key snapshot is
      // pre-fader, so the binding still sees it at full level.
      engine.setParameter(engine.resolveStripAutomationId(20, 'faderDb'), -120);
      engine.seekSample(0);
      engine.process([new Float32Array(128), new Float32Array(128)]);
      engine.settleParameters();
      engine.play();
      let out: Float32Array[] = [];
      for (let block = 0; block < 40; block++) {
        out = engine.process([new Float32Array(128), new Float32Array(128)]);
      }
      const ducked = Math.abs(out[0][127]);
      // Pad 0.5 through the -6 dB group bus would sit at ~0.25 unducked; the
      // hot key must push it far below that.
      expect(ducked).toBeLessThan(0.07);
      engine.setStripSidechain(10, 0, null);
      engine.destroy();
    });

    it('seeks markers at a scheduled render frame', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      engine.setTempo(60);
      engine.setMarkers([{ id: 9, ppq: 4, name: 'verse' }]);
      // Seek to marker 9 (4 quarter notes at 60bpm = 4s = 192000 samples),
      // scheduled at the head of the block.
      expect(() => engine.seekMarker(9)).not.toThrow();
      engine.play();
      engine.process([new Float32Array(128), new Float32Array(128)]);
      const state = engine.transportState();
      expect(state.samplePosition).toBe(192000 + 128);
      engine.destroy();
    });

    it('rejects invalid tempo without replacing the active tempo map', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      engine.setTempo(60);
      expect(engine.sampleAtPpq(1)).toBe(48000);
      for (const invalid of [0, -1, Number.NaN, Number.POSITIVE_INFINITY]) {
        expect(() => engine.setTempo(invalid)).toThrow();
        expect(engine.sampleAtPpq(1)).toBe(48000);
      }
      expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: Number.NaN }])).toThrow();
      expect(engine.sampleAtPpq(1)).toBe(48000);
      engine.destroy();
    });

    it('rejects an oversized segment/marker list before allocating', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      // A crafted length past the shared buffer cap (500e6) must be rejected up
      // front, matching the C ABI, instead of driving an unbounded reserve. Use a
      // fake array-like so no real 500M-element allocation is attempted.
      const oversized = { length: 500_000_001 } as unknown as [];
      expect(() => engine.setTempoSegments(oversized)).toThrow();
      expect(() => engine.setTimeSignatureSegments(oversized)).toThrow();
      expect(() => engine.setMarkers(oversized)).toThrow();
      engine.destroy();
    });

    it('round-trips marker kind and key signature through setMarkers/markers', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      engine.setMarkers([
        { id: 1, ppq: 0, name: 'intro' },
        { id: 2, ppq: 4, name: 'Bb minor', kind: 'keySignature', keyFifths: -2, keyMinor: true },
      ]);
      expect(engine.markers()).toHaveLength(2);
      const [plain, key] = engine.markers();
      expect(plain.kind).toBe('marker');
      expect(plain.keyFifths).toBe(0);
      expect(plain.keyMinor).toBe(false);
      expect(key.name).toBe('Bb minor');
      expect(key.kind).toBe('keySignature');
      expect(key.keyFifths).toBe(-2);
      expect(key.keyMinor).toBe(true);
      engine.destroy();
    });
  });

  describe('StreamingEqualizer sidechain parity', () => {
    it('accepts mono/stereo sidechain and clears it', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        const key = new Float32Array(128).fill(0.5);
        expect(() => eq.setSidechainMono(key)).not.toThrow();
        expect(() => eq.setSidechainStereo(key, key)).not.toThrow();
        expect(() => eq.clearSidechain()).not.toThrow();
        // Mismatched stereo lengths must throw before reaching WASM.
        expect(() => eq.setSidechainStereo(key, new Float32Array(64))).toThrow();
        // Processing still works after a sidechain round-trip.
        const out = eq.processMono(new Float32Array(128).fill(0.25));
        expect(out.length).toBe(128);
      } finally {
        eq.destroy();
      }
    });
  });

  describe('Mixer imperative topology', () => {
    it('adds/removes buses and VCA groups and reports counts', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: 48000,
        maxBlockSize: 128,
      });
      try {
        const busesBefore = mixer.busCount;
        mixer.addBus('parallel-comp', 'aux');
        expect(mixer.busCount).toBe(busesBefore + 1);
        mixer.removeBus('parallel-comp');
        expect(mixer.busCount).toBe(busesBefore);

        const vcasBefore = mixer.vcaGroupCount;
        mixer.addVcaGroup('all', -3, ['vocal']);
        expect(mixer.vcaGroupCount).toBe(vcasBefore + 1);
        mixer.setVcaGroupGainDb('all', -8);
        mixer.setVcaGroupMembers('all', []);
        const scene = JSON.parse(mixer.toJson());
        const group = scene.vcaGroups.find((entry: { id: string }) => entry.id === 'all');
        expect(group.gainDb).toBe(-8);
        expect(group.members).toEqual([]);
        mixer.removeVcaGroup('all');
        expect(mixer.vcaGroupCount).toBe(vcasBefore);

        mixer.compile();
      } finally {
        mixer.destroy();
      }
    });
  });

  describe('sections / melody / CQT / VQT parity', () => {
    it('passes fminHz/fmaxHz through inverse Mel and MFCC paths', () => {
      const tone = generateSine(440, SR, 0.5);
      const nFft = 1024;
      const hop = 256;
      const nMels = 40;
      const fminHz = 80;
      const fmaxHz = 4000;

      const mel = melSpectrogram(tone, SR, { nFft, hopLength: hop, nMels });
      const stft = melToStft({ power: mel.power, sampleRate: SR, nFft, fminHz, fmaxHz });
      expect(stft.rows).toBe(nFft / 2 + 1);
      expect(stft.cols).toBe(mel.power.cols);
      expect(allFinite(stft.data)).toBe(true);

      const audio = melToAudio({
        power: mel.power,
        sampleRate: SR,
        nFft,
        hopLength: hop,
        fminHz,
        fmaxHz,
        nIter: 2,
      });
      expect(audio.length).toBeGreaterThan(0);
      expect(allFinite(audio)).toBe(true);

      const coeffs = mfcc(tone, SR, { nFft, hopLength: hop, nMels, nMfcc: 13 });
      const mfccAudio = mfccToAudio({
        coefficients: coeffs.coefficients,
        nMels,
        sampleRate: SR,
        nFft,
        hopLength: hop,
        fminHz,
        fmaxHz,
        nIter: 2,
      });
      expect(mfccAudio.length).toBeGreaterThan(0);
      expect(allFinite(mfccAudio)).toBe(true);
    });

    it('passes htk through inverse Mel paths', () => {
      const tone = generateSine(440, SR, 0.25);
      const nFft = 1024;
      const hop = 256;
      const nMels = 40;
      const mel = melSpectrogram(tone, SR, { nFft, hopLength: hop, nMels, htk: true });

      const slaney = melToStft({ power: mel.power, sampleRate: SR, nFft, htk: false });
      const htk = melToStft({ power: mel.power, sampleRate: SR, nFft, htk: true });
      expect(htk.rows).toBe(nFft / 2 + 1);
      expect(htk.cols).toBe(mel.power.cols);
      expect(allFinite(htk.data)).toBe(true);

      let diff = 0;
      for (let i = 0; i < htk.data.length; i++) {
        diff += (slaney.data[i] - htk.data[i]) ** 2;
      }
      expect(diff).toBeGreaterThan(1e-6);

      const audio = melToAudio({
        power: mel.power,
        sampleRate: SR,
        nFft,
        hopLength: hop,
        nIter: 2,
        htk: true,
      });
      expect(audio.length).toBeGreaterThan(0);
      expect(allFinite(audio)).toBe(true);
    });

    it('reconstructs Mel and MFCC data from request objects', () => {
      const tone = generateSine(440, SR, 0.25);
      const nFft = 1024;
      const hopLength = 256;
      const nMels = 40;
      const nMfcc = 13;
      const fminHz = 80;
      const fmaxHz = 4000;
      const mel = melSpectrogram(tone, SR, { nFft, hopLength, nMels, fminHz, fmaxHz, htk: true });
      const coefficients = mfcc(tone, SR, {
        nFft,
        hopLength,
        nMels,
        nMfcc,
        fminHz,
        fmaxHz,
        htk: true,
      });
      expect(coefficients.coefficients.rows).toBe(nMfcc);
      expect(coefficients.coefficients.cols).toBe(mel.power.cols);

      const stft = melToStft({ power: mel.power, sampleRate: SR, nFft, fminHz, fmaxHz, htk: true });
      expect([stft.rows, stft.cols]).toEqual([nFft / 2 + 1, mel.power.cols]);
      expect(allFinite(stft.data)).toBe(true);

      const rebuilt = mfccToMel({ coefficients: coefficients.coefficients, nMels });
      expect([rebuilt.rows, rebuilt.cols]).toEqual([nMels, coefficients.coefficients.cols]);
      expect(allFinite(rebuilt.data)).toBe(true);

      const lifted = new Float32Array(5 * 4);
      for (let coefficient = 0; coefficient < 5; coefficient++) {
        const lift = 1 + 11 * Math.sin((Math.PI * coefficient) / 22);
        lifted.fill((0.2 + 0.1 * coefficient) * lift, coefficient * 4, coefficient * 4 + 4);
      }
      const liftedMatrix = { data: lifted, rows: 5, cols: 4 };
      const restored = mfccToMel({ coefficients: liftedMatrix, nMels: 8, lifter: 22 });
      const untreated = mfccToMel({ coefficients: liftedMatrix, nMels: 8 });
      expect(
        restored.data.some((value, index) => Math.abs(value - untreated.data[index]) > 1e-5),
      ).toBe(true);

      const audioRequest = {
        power: mel.power,
        sampleRate: SR,
        nFft,
        hopLength,
        fminHz,
        fmaxHz,
        nIter: 2,
        htk: true,
      };
      const audio = melToAudio(audioRequest);
      expect(audio.length).toBeGreaterThan(0);
      expect(allFinite(audio)).toBe(true);
      expect(melToAudio(audioRequest)).toEqual(audio);

      const mfccAudio = mfccToAudio({
        coefficients: coefficients.coefficients,
        nMels,
        sampleRate: SR,
        nFft,
        hopLength,
        fminHz,
        fmaxHz,
        nIter: 2,
        htk: true,
      });
      expect(mfccAudio.length).toBeGreaterThan(0);
      expect(allFinite(mfccAudio)).toBe(true);
    });

    it('computes CQT and VQT magnitude grids', () => {
      const grid = { hopLength: 512, fminHz: 32.7, nBins: 24, binsPerOctave: 12 };
      const cqtResult = cqt(signal, SR, grid);
      expect(cqtResult.magnitude.rows).toBe(24);
      expect(cqtResult.magnitude.cols).toBeGreaterThan(0);
      expect(cqtResult.frequencies.length).toBe(24);
      expect(cqtResult.magnitude.data.length).toBe(
        cqtResult.magnitude.rows * cqtResult.magnitude.cols,
      );
      expect(cqtResult.sampleRate).toBe(SR);
      expect(allFinite(cqtResult.magnitude.data)).toBe(true);

      const vqtResult = vqt(signal, SR, { ...grid, gamma: 10 });
      expect(vqtResult.magnitude.rows).toBe(24);
      expect(vqtResult.magnitude.data.length).toBe(
        vqtResult.magnitude.rows * vqtResult.magnitude.cols,
      );
      expect(allFinite(vqtResult.magnitude.data)).toBe(true);

      // Shared deterministic oracle fixture with Node/Python.
      const inverseSource = generateSine(261.6256, 8000, 0.256, 0.5);
      const inverseGrid = { hopLength: 128, fminHz: 130.8128, nBins: 12, binsPerOctave: 12 };
      const inverseCqt = cqt(inverseSource, 8000, inverseGrid);
      const inverseVqt = vqt(inverseSource, 8000, { ...inverseGrid, gamma: 0 });
      const cqtAudio = cqtToAudio({
        magnitude: inverseCqt.magnitude,
        sampleRate: 8000,
        hopLength: 128,
        fminHz: 130.8128,
        binsPerOctave: 12,
        nIter: 2,
      });
      const vqtAudio = vqtToAudio({
        magnitude: inverseVqt.magnitude,
        sampleRate: 8000,
        gamma: 0,
        hopLength: 128,
        fminHz: 130.8128,
        binsPerOctave: 12,
        nIter: 2,
      });
      expect(cqtAudio.length).toBe(inverseSource.length);
      expect(allFinite(cqtAudio)).toBe(true);
      expect(Math.max(...cqtAudio.map(Math.abs))).toBeGreaterThan(1e-5);
      expect(vqtAudio.length).toBe(inverseSource.length);
      expect(allFinite(vqtAudio)).toBe(true);
      expect(() =>
        cqtToAudio({
          magnitude: {
            data: cqtResult.magnitude.data.subarray(1),
            rows: cqtResult.magnitude.rows,
            cols: cqtResult.magnitude.cols,
          },
          sampleRate: SR,
        }),
      ).toThrow(/length/);
      expect(() =>
        vqtToAudio({
          magnitude: vqtResult.magnitude,
          sampleRate: SR,
          hopLength: 512,
          fminHz: 32.7,
          binsPerOctave: 12,
          gamma: 10,
          nIter: 257,
        }),
      ).toThrow(/nIter/);

      const pseudo = pseudoCqt(signal, SR, grid);
      expect(pseudo.magnitude.rows).toBe(24);
      expect(pseudo.frequencies.length).toBe(24);
      expect(pseudo.magnitude.data.length).toBe(pseudo.magnitude.rows * pseudo.magnitude.cols);
      expect(pseudo.sampleRate).toBe(SR);
      expect(allFinite(pseudo.magnitude.data)).toBe(true);

      const hybrid = hybridCqt(signal, SR, grid);
      expect(hybrid.magnitude.rows).toBe(24);
      expect(hybrid.frequencies.length).toBe(24);
      expect(hybrid.magnitude.data.length).toBe(hybrid.magnitude.rows * hybrid.magnitude.cols);
      expect(hybrid.sampleRate).toBe(SR);
      expect(allFinite(hybrid.magnitude.data)).toBe(true);
    });

    it('guards music feature wrappers before native analysis', () => {
      expect(() => nnlsChroma(new Float32Array([Number.NaN]), SR)).toThrow(/non-finite/);
      expect(() => cqt(signal, 7999)).toThrow(/sampleRate/);
      expect(() => cqt(signal, SR, { hopLength: 0 })).toThrow(/hopLength/);
      // An omitted gamma is the automatic ERB-derived bandwidth; 0 is the
      // constant-Q transform. Negative, NaN and infinite gamma are refused.
      const grid = { hopLength: 512, fminHz: 32.7, nBins: 24, binsPerOctave: 12 };
      for (const gamma of [Number.POSITIVE_INFINITY, Number.NaN, -1]) {
        expect(() => vqt(signal, SR, { ...grid, gamma })).toThrow(/gamma/);
      }
      expect(Array.from(vqt(signal, SR, grid).magnitude.data)).not.toEqual(
        Array.from(vqt(signal, SR, { ...grid, gamma: 0 }).magnitude.data),
      );
      expect(() => analyzeSections(signal, SR, { minSectionSec: -1 })).toThrow(/minSectionSec/);
      expect(() => analyzeSections(signal, SR, { minSectionSec: 0 })).not.toThrow();
      expect(() => analyzeSections(signal, SR, { nFft: 0 })).toThrow(/nFft/);
      expect(() => analyzeMelody(signal, SR, { f0MinHz: 400, f0MaxHz: 200 })).toThrow(/f0MaxHz/);
      // Strictly-positive parity with the C ABI (sonare_analyze_melody): a zero
      // f0MinHz or threshold is rejected, not silently replaced with the default.
      expect(() => analyzeMelody(signal, SR, { f0MinHz: 0 })).toThrow(/f0MinHz/);
      expect(() => analyzeMelody(signal, SR, { threshold: 0 })).toThrow(/threshold/);
      expect(() => onsetStrength(new Float32Array(), SR)).toThrow(/empty/);
      expect(() =>
        fourierTempogram({
          onsetEnvelope: new Float32Array([1, Number.POSITIVE_INFINITY]),
          sampleRate: SR,
        }),
      ).toThrow(/non-finite/);
      expect(() =>
        tempogramRatio({
          tempogram: { data: new Float32Array([1, 2]), rows: 0, cols: 2 },
          sampleRate: SR,
        }),
      ).toThrow(/rows/);
    });

    it('guards spectrogram and inverse wrapper shape inputs', () => {
      expect(() => stft(new Float32Array([0, Number.NaN]), SR)).toThrow(/non-finite/);
      expect(() => melSpectrogram(signal, 0)).toThrow(/sampleRate/);
      expect(() =>
        melSpectrogram(signal, SR, {
          nFft: 1024,
          hopLength: 256,
          nMels: 40,
          fminHz: 5000,
          fmaxHz: 4000,
        }),
      ).toThrow(/fmax/);

      expect(() =>
        melToStft({
          power: { data: new Float32Array(5), rows: 2, cols: 3 },
          sampleRate: SR,
          nFft: 1024,
        }),
      ).toThrow(/length/);
      expect(() =>
        melToAudio({
          power: { data: new Float32Array([1, Number.NaN, 2, 3]), rows: 2, cols: 2 },
          sampleRate: SR,
          nFft: 1024,
        }),
      ).toThrow(/non-finite/);
      expect(() =>
        mfccToMel({ coefficients: { data: new Float32Array(6), rows: 0, cols: 3 }, nMels: 40 }),
      ).toThrow(/rows/);
      expect(() =>
        mfccToAudio({
          coefficients: { data: new Float32Array(6), rows: 2, cols: 3 },
          nMels: 40,
          sampleRate: 7999,
        }),
      ).toThrow(/sampleRate/);
    });

    it('analyzes melody contour with summary stats', () => {
      // 440 Hz tone is squarely within the default melody f0 range.
      const tone = generateSine(440, SR, 1.0);
      const melody = analyzeMelody(tone, SR);
      expect(Array.isArray(melody.points)).toBe(true);
      expect(melody.points.length).toBeGreaterThan(0);
      expect(Number.isFinite(melody.meanFrequencyHz)).toBe(true);
      expect(Number.isFinite(melody.pitchRangeOctaves)).toBe(true);
      expect(Number.isFinite(melody.pitchStability)).toBe(true);
      expect(Number.isFinite(melody.vibratoRateHz)).toBe(true);
      const first = melody.points[0];
      expect(Number.isFinite(first.timeSec)).toBe(true);
      expect(Number.isFinite(first.confidence)).toBe(true);
      // frequencyHz is absent on an unvoiced frame; a steady tone has voiced ones.
      const voiced = melody.points.find((point) => point.frequencyHz !== undefined);
      expect(Number.isFinite(voiced?.frequencyHz)).toBe(true);
    });

    it('analyzes song-structure sections', () => {
      const sections = analyzeSections(signal, SR);
      expect(Array.isArray(sections)).toBe(true);
      for (const section of sections) {
        expect(typeof section.type).toBe('string');
        expect(Number.isFinite(section.startSec)).toBe(true);
        expect(Number.isFinite(section.endSec)).toBe(true);
        expect(section.endSec).toBeGreaterThanOrEqual(section.startSec);
        expect(Number.isFinite(section.energyLevel)).toBe(true);
        expect(Number.isFinite(section.confidence)).toBe(true);
      }
    });
  });

  it('exposes silence ratio metering and optional NNLS STFT blending', () => {
    const samples = new Float32Array(2048);
    samples.fill(1, 1024);
    expect(
      meteringSilenceRatio(samples, SR, { thresholdDb: -45, frameLength: 1024, hopLength: 1024 }),
    ).toBeCloseTo(0.5, 6);
    // A non-finite threshold is a question the meter cannot evaluate: every
    // frame comparison is false against NaN, so it used to answer "0 % silence"
    // here while the C ABI and Python refused. The `?? -45` default cannot
    // catch it (NaN is not nullish); the guard is in the core meter, which this
    // surface calls directly.
    for (const invalid of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expect(() =>
        meteringSilenceRatio(samples, SR, {
          thresholdDb: invalid,
          frameLength: 1024,
          hopLength: 1024,
        }),
      ).toThrow();
    }
    const chroma = nnlsChroma(generateSine(440, SR, 0.5), SR, {
      enableStftBlend: false,
      stftBlendWeight: 0,
      stftBlendNFft: 2048,
    });
    expect(chroma.data.length).toBe(12 * chroma.cols);
  });
});
