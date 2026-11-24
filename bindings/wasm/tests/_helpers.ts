/**
 * Shared test-signal generators and assertions for the WASM binding suites.
 */

import { expect } from 'vitest';
import { isSonareError } from '../dist/index.js';

/** Options for {@link sine}. */
export interface SineOptions {
  /** Peak amplitude of the generated tone. Defaults to 0.5. */
  amp?: number;
  /** Sample rate in Hz. Defaults to 48000. */
  sampleRate?: number;
}

/**
 * Generates a mono sine tone as a Float32Array.
 *
 * @param freqHz - Tone frequency in Hz.
 * @param durationSec - Duration in seconds; the sample count is
 *   `floor(sampleRate * durationSec)`.
 * @param opts - Optional amplitude and sample-rate overrides.
 * @returns The generated tone.
 */
export function sine(freqHz: number, durationSec: number, opts: SineOptions = {}): Float32Array {
  const { amp = 0.5, sampleRate = 48000 } = opts;
  const n = Math.floor(sampleRate * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / sampleRate);
  }
  return out;
}

/**
 * Asserts that a `Mixer.stripById()` lookup resolved, narrowing away the `null`
 * it returns for an unknown id so the index can be passed to the strip-indexed
 * setters and meter readers.
 *
 * @param index - The `stripById()` return value.
 * @param id - The scene strip id that was looked up, used in the failure message.
 * @throws Error when the lookup missed.
 */
export function assertStripIndex(index: number | null, id: string): asserts index is number {
  if (index === null) {
    throw new Error(`Mixer.stripById(${JSON.stringify(id)}) did not resolve to a strip index`);
  }
}

/** The native class a refused argument surfaces as. */
export type ArgumentRefusalClass = typeof RangeError | typeof TypeError;

/**
 * Asserts that a caught value is an argument the binding refused itself: a
 * native `RangeError` (out of domain, the default) or `TypeError` (wrong type),
 * and not a coded `SonareError`, which is what a library or object-state
 * failure stays.
 *
 * @param caught - What the call threw, or `undefined` when it did not throw.
 * @param kind - The class the refusal must be.
 * @param context - Names the case in each failure message.
 * @returns The refusal, narrowed to `Error`.
 */
export function expectArgumentRefusal(
  caught: unknown,
  kind: ArgumentRefusalClass = RangeError,
  context = 'refusal',
): Error {
  expect(caught, `${context}: expected a refusal, got no throw`).toBeDefined();
  expect(isSonareError(caught), `${context}: an argument refusal is not a coded SonareError`).toBe(
    false,
  );
  expect(caught, `${context}: expected ${kind.name}`).toBeInstanceOf(kind);
  return caught as Error;
}

/**
 * Runs `call` and asserts it throws an argument refusal of class `kind`.
 *
 * @param call - The call under test.
 * @param kind - The class the refusal must be; `RangeError` by default.
 * @returns The refusal, narrowed to `Error`.
 */
export function expectRefusalOf(
  call: () => unknown,
  kind: ArgumentRefusalClass = RangeError,
): Error {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  return expectArgumentRefusal(caught, kind);
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
