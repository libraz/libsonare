/**
 * Shared test signal helpers.
 *
 * `sine` is the canonical replacement for the per-file Math.sin loops. Argument
 * order and defaults are fixed here; each test keeps its own thin wrapper when
 * its historical call sites use a different arg order or sample-count shape.
 */

import { writeFileSync } from 'node:fs';
import { expect } from 'vitest';
import { type ErrorCode, isSonareError } from '../src/errors.js';

export interface SineOptions {
  /** Sample rate in Hz (default 44100). */
  sampleRate?: number;
  /** Peak amplitude (default 0.5). */
  amp?: number;
}

/**
 * Generate a mono sine wave.
 *
 * @param freqHz Frequency in Hz.
 * @param durationSec Duration in seconds; the sample count is floor(sampleRate * durationSec).
 * @param opts Optional sample rate and amplitude.
 * @returns The generated samples.
 */
export function sine(freqHz: number, durationSec: number, opts: SineOptions = {}): Float32Array {
  const { sampleRate = 44100, amp = 0.5 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / sampleRate);
  }
  return out;
}

export interface GlideOptions extends SineOptions {
  /** Interval swept over the whole duration, in semitones (default 11). */
  semitones?: number;
}

/**
 * Generate a continuous pitch glide, two partials deep.
 *
 * For anything that recovers a time correspondence between two signals: the
 * chroma moves every frame, so no two frames carry the same pitch class and one
 * alignment is strictly cheaper than the rest. A held tone leaves the path
 * unconstrained along its whole length and the recovered anchors say nothing.
 *
 * @param startHz Starting frequency in Hz; the sweep ends `semitones` above it.
 * @param durationSec Duration in seconds; the sample count is floor(sampleRate * durationSec).
 * @param opts Optional sample rate, amplitude (default 0.4) and interval.
 * @returns The generated samples.
 */
export function glide(startHz: number, durationSec: number, opts: GlideOptions = {}): Float32Array {
  const { sampleRate = 44100, amp = 0.4, semitones = 11 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  let phase = 0;
  let octavePhase = 0;
  for (let i = 0; i < n; i++) {
    const hz = startHz * 2 ** ((semitones * (i / n)) / 12);
    phase += (2 * Math.PI * hz) / sampleRate;
    octavePhase += (2 * Math.PI * hz * 2) / sampleRate;
    out[i] = amp * (Math.sin(phase) + 0.5 * Math.sin(octavePhase));
  }
  return out;
}

/**
 * Write the given planes as one interleaved 16-bit PCM WAV.
 *
 * Fixtures are written by the test rather than committed, so a decoded channel
 * is compared against the exact samples it was written from.
 */
export function writePcm16Wav(filePath: string, planes: Float32Array[], sampleRate: number): void {
  const channels = planes.length;
  const frameCount = planes[0].length;
  const bytesPerSample = 2;
  const dataSize = frameCount * channels * bytesPerSample;
  const wav = Buffer.alloc(44 + dataSize);
  wav.write('RIFF', 0, 4, 'ascii');
  wav.writeUInt32LE(36 + dataSize, 4);
  wav.write('WAVE', 8, 4, 'ascii');
  wav.write('fmt ', 12, 4, 'ascii');
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(channels, 22);
  wav.writeUInt32LE(sampleRate, 24);
  wav.writeUInt32LE(sampleRate * channels * bytesPerSample, 28);
  wav.writeUInt16LE(channels * bytesPerSample, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36, 4, 'ascii');
  wav.writeUInt32LE(dataSize, 40);
  for (let frame = 0; frame < frameCount; frame++) {
    for (let channel = 0; channel < channels; channel++) {
      const offset = 44 + (frame * channels + channel) * bytesPerSample;
      const quantized = Math.max(
        -32768,
        Math.min(32767, Math.round(planes[channel][frame] * 32767)),
      );
      wav.writeInt16LE(quantized, offset);
    }
  }
  writeFileSync(filePath, wav);
}

/**
 * Assert that `run` throws the refusal a cause maps to, checking class and code together.
 *
 * A refused argument is a plain `TypeError` / `RangeError` with no `SonareError` code; every other
 * failure is a coded `SonareError`. Pass the error class for the first and the {@link ErrorCode}
 * for the second, so a wrong class or a wrong code fails the test and a message alone never passes.
 * `message` additionally pins which refusal of that class fired.
 */
export function expectRefusal(
  run: () => unknown,
  expected: ErrorConstructor | ErrorCode,
  message?: RegExp,
): void {
  let caught: unknown;
  try {
    run();
  } catch (error) {
    caught = error;
  }
  expect(caught, 'expected the call to throw').toBeDefined();
  if (message !== undefined) {
    expect((caught as Error).message).toMatch(message);
  }
  if (typeof expected === 'number') {
    expect(isSonareError(caught)).toBe(true);
    expect((caught as { code: number }).code).toBe(expected);
    return;
  }
  expect(caught).toBeInstanceOf(expected);
  expect(isSonareError(caught)).toBe(false);
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * Pattern for an unknown-key refusal whose message starts with `head`
 * (`"<entry>: unknown key <path>"`); `also` is the nested path a misplaced key belongs under.
 */
export function unknownKeyText(head: string, also?: string): RegExp {
  const tail = also === undefined ? '' : `, or ${escapeRegExp(also)}`;
  return new RegExp(`${escapeRegExp(head)} \\(expected [^)]*${tail}\\)`);
}

/** Matcher for the TypeError carrying exactly that refusal, for `toThrow` / `rejects.toThrow`. */
export function unknownKeyError(head: string, also?: string) {
  const pattern = new RegExp(`^${unknownKeyText(head, also).source}$`);
  return {
    asymmetricMatch: (actual: unknown) =>
      actual instanceof TypeError && pattern.test(actual.message),
    toString: () => `TypeError matching ${pattern}`,
    toAsymmetricMatcher: () => `TypeError matching ${pattern}`,
  };
}

/** The keys a mastering chain reader accepts, in the order its unknown-key refusal lists them. */
const CHAIN_ACCEPTED_KEYS = [
  'dynamics.compressor.attackMs',
  'dynamics.compressor.autoMakeup',
  'dynamics.compressor.detector',
  'dynamics.compressor.enabled',
  'dynamics.compressor.kneeDb',
  'dynamics.compressor.makeupGainDb',
  'dynamics.compressor.pdrReleaseScale',
  'dynamics.compressor.pdrTimeMs',
  'dynamics.compressor.ratio',
  'dynamics.compressor.releaseMs',
  'dynamics.compressor.sidechainHighPassHz',
  'dynamics.compressor.thresholdDb',
  'dynamics.deesser.attackMs',
  'dynamics.deesser.bandPassQ',
  'dynamics.deesser.enabled',
  'dynamics.deesser.frequencyHz',
  'dynamics.deesser.rangeDb',
  'dynamics.deesser.ratio',
  'dynamics.deesser.releaseMs',
  'dynamics.deesser.thresholdDb',
  'dynamics.multibandComp.bands.{i}.attackMs',
  'dynamics.multibandComp.bands.{i}.autoMakeup',
  'dynamics.multibandComp.bands.{i}.detector',
  'dynamics.multibandComp.bands.{i}.kneeDb',
  'dynamics.multibandComp.bands.{i}.makeupGainDb',
  'dynamics.multibandComp.bands.{i}.pdrReleaseScale',
  'dynamics.multibandComp.bands.{i}.pdrTimeMs',
  'dynamics.multibandComp.bands.{i}.ratio',
  'dynamics.multibandComp.bands.{i}.releaseMs',
  'dynamics.multibandComp.bands.{i}.sidechainHighPassHz',
  'dynamics.multibandComp.bands.{i}.thresholdDb',
  'dynamics.multibandComp.crossover.cutoffsHz.{i}',
  'dynamics.multibandComp.crossover.firKernelSize',
  'dynamics.multibandComp.crossover.mode',
  'dynamics.multibandComp.crossover.slope',
  'dynamics.multibandComp.cutoff{i}Hz',
  'dynamics.multibandComp.enabled',
  'dynamics.transientShaper.attackGainDb',
  'dynamics.transientShaper.enabled',
  'dynamics.transientShaper.fastAttackMs',
  'dynamics.transientShaper.fastReleaseMs',
  'dynamics.transientShaper.gainSmoothingMs',
  'dynamics.transientShaper.lookaheadMs',
  'dynamics.transientShaper.maxGainDb',
  'dynamics.transientShaper.sensitivity',
  'dynamics.transientShaper.slowAttackMs',
  'dynamics.transientShaper.slowReleaseMs',
  'dynamics.transientShaper.sustainGainDb',
  'eq.tilt.enabled',
  'eq.tilt.pivotHz',
  'eq.tilt.tiltDb',
  'loudness.applyGainAtInputRate',
  'loudness.ceilingDbtp',
  'loudness.enabled',
  'loudness.maxLimiterGainReductionDb',
  'loudness.releaseMs',
  'loudness.targetLufs',
  'loudness.truePeakOversample',
  'maximizer.truePeakLimiter.applyGainAtInputRate',
  'maximizer.truePeakLimiter.ceilingDbtp',
  'maximizer.truePeakLimiter.enabled',
  'maximizer.truePeakLimiter.lookaheadMs',
  'maximizer.truePeakLimiter.oversampleFactor',
  'maximizer.truePeakLimiter.releaseMs',
  'repair.declick.enabled',
  'repair.declick.lpcOrder',
  'repair.declick.maxClickSamples',
  'repair.declick.neighborRatio',
  'repair.declick.residualRatio',
  'repair.declick.threshold',
  'repair.declip.clipThresholdDb',
  'repair.declip.enabled',
  'repair.declip.iterations',
  'repair.declip.linkChannels',
  'repair.declip.lpcOrder',
  'repair.decrackle.enabled',
  'repair.decrackle.levels',
  'repair.decrackle.mode',
  'repair.decrackle.threshold',
  'repair.dehum.adaptation',
  'repair.dehum.adaptive',
  'repair.dehum.enabled',
  'repair.dehum.frameSize',
  'repair.dehum.fundamentalHz',
  'repair.dehum.harmonics',
  'repair.dehum.mode',
  'repair.dehum.pllBandwidth',
  'repair.dehum.q',
  'repair.dehum.searchRangeHz',
  'repair.denoise.ddAlpha',
  'repair.denoise.enabled',
  'repair.denoise.gainSmoothing',
  'repair.denoise.hopLength',
  'repair.denoise.mode',
  'repair.denoise.nFft',
  'repair.denoise.noiseEstimationQuantile',
  'repair.denoise.noiseEstimator',
  'repair.denoise.overSubtraction',
  'repair.denoise.reductionDb',
  'repair.denoise.spectralFloor',
  'repair.denoise.speechPresenceGain',
  'repair.dereverb.attenuation',
  'repair.dereverb.enabled',
  'repair.dereverb.hopLength',
  'repair.dereverb.lateDelayMs',
  'repair.dereverb.nFft',
  'repair.dereverb.overSubtraction',
  'repair.dereverb.spectralFloor',
  'repair.dereverb.t60Sec',
  'repair.dereverb.threshold',
  'repair.dereverb.wpeEnabled',
  'repair.dereverb.wpeIterations',
  'repair.dereverb.wpeStrength',
  'repair.dereverb.wpeTaps',
  'saturation.exciter.aliasing',
  'saturation.exciter.amount',
  'saturation.exciter.driveDb',
  'saturation.exciter.enabled',
  'saturation.exciter.evenOddMix',
  'saturation.exciter.frequencyHz',
  'saturation.exciter.q',
  'saturation.tape.bias',
  'saturation.tape.driveDb',
  'saturation.tape.enabled',
  'saturation.tape.gapLoss',
  'saturation.tape.headBumpDb',
  'saturation.tape.hysteresis',
  'saturation.tape.outputGainDb',
  'saturation.tape.oversampleFactor',
  'saturation.tape.saturation',
  'saturation.tape.speedIps',
  'spectral.airBand.amount',
  'spectral.airBand.dynamicRangeDb',
  'spectral.airBand.dynamicThresholdDb',
  'spectral.airBand.enabled',
  'spectral.airBand.shelfFrequencyHz',
  'stereo.imager.decorrelationAmount',
  'stereo.imager.enabled',
  'stereo.imager.outputGainDb',
  'stereo.imager.preserveEnergy',
  'stereo.imager.width',
  'stereo.monoMaker.amount',
  'stereo.monoMaker.enabled',
  'stereo.monoMaker.frequencyHz',
];

/** The whole unknown-key refusal of a mastering chain reader whose message starts with `head`. */
export function chainUnknownKeyText(head: string): string {
  return `${head} (expected ${CHAIN_ACCEPTED_KEYS.join(', ')})`;
}

/** A literal message compares equal; a pattern from {@link unknownKeyText} compares by match. */
export function textMatcher(message: string | RegExp): unknown {
  return typeof message === 'string' ? message : expect.stringMatching(message);
}

/** Returns `value`, failing the test when it is absent; for a level the call must have measured. */
export function present<T>(value: T | undefined, what = 'value'): T {
  expect(value, `${what} should be present`).toBeDefined();
  return value as T;
}
