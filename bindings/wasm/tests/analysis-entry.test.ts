import { beforeAll, describe, expect, it } from 'vitest';
import * as analysis from '../dist/analysis.js';

describe('analysis-only WASM entry', () => {
  beforeAll(async () => {
    await analysis.init();
  });

  it('loads the dedicated binary and exposes analysis capabilities', () => {
    expect(analysis.isInitialized()).toBe(true);
    expect(analysis.capabilities().features).toMatchObject({
      mastering: false,
      mixing: false,
      // The analysis-only configuration forces BUILD_MIXING_ASSISTANT off, and
      // the assistant's entry points stay registered in a build without it, so
      // this flag is the only way a host can tell the two bundles apart.
      mixingAssistant: false,
      fx: false,
    });
    expect(analysis.meteringPeakDb(new Float32Array([0, 0.5, -0.25]))).toBeCloseTo(-6.0206, 3);
  });

  it('does not expose non-analysis APIs', () => {
    const entry = analysis as Record<string, unknown>;
    for (const name of [
      'masterAudio',
      'mixStereo',
      'Mixer',
      'Project',
      'RealtimeEngine',
      'synthesizeRir',
      'estimateRoom',
      'roomMorph',
    ]) {
      expect(entry[name]).toBeUndefined();
    }
  });
});

/**
 * The entry re-exported three modules wholesale, and each held functions the
 * analysis-only embind source set never registers. Those names imported fine
 * and threw "… is not a function" on the first call, against a binary that
 * never had them — an absence that only a *call* reveals, since the import and
 * the symbol both exist as far as the bundler is concerned.
 *
 * Note what this cannot be: a no-argument call over every export does NOT find
 * them. Each wrapper validates its arguments in TypeScript before reaching the
 * native call, so `decompose()` throws on the arguments and never gets far
 * enough to discover the missing symbol. Measured: that version of the check
 * reported 0 unresolved against a bundle with all 10 defects present. So the
 * exhaustive half has to compare names, and the calling half has to use real
 * arguments and therefore cannot be exhaustive. Both are below.
 */
describe('the analysis entry exports only what its binary registers', () => {
  /** Defined in analysis.ts itself, so they back onto no native registration. */
  const JS_ONLY = new Set([
    'init',
    'isInitialized',
    'version',
    'capabilities',
    'abiVersion',
    'engineAbiVersion',
    'voiceChangerAbiVersion',
    'isSonareError',
    'SonareError',
  ]);

  const entryFunctions = (): string[] =>
    Object.entries(analysis as Record<string, unknown>)
      .filter(([name, value]) => typeof value === 'function' && !JS_ONLY.has(name))
      .map(([name]) => name);

  const registrations = async (): Promise<Set<string>> => {
    const createModule = (await import('../dist/sonare-analysis.js')).default;
    return new Set(Object.keys(await createModule()));
  };

  it('resolves every exported function against the analysis-only module', async () => {
    const registered = await registrations();
    const exported = entryFunctions();
    // Self-check: a mismatch between the two naming schemes would make the
    // assertion below fail for every name at once rather than for a defect, so
    // pin that the direct match still resolves the bulk of the surface. If a
    // wrapper convention ever diverges from its native name, this fails loudly
    // and a human decides, instead of the check quietly tolerating it.
    expect(exported.length).toBeGreaterThan(100);
    expect(exported.filter((name) => registered.has(name)).length).toBeGreaterThan(100);

    expect(exported.filter((name) => !registered.has(name))).toEqual([]);
  });

  it('runs a representative call from each re-exported module', async () => {
    // The name comparison proves the lists agree; only a call proves a symbol
    // resolves. One per module that the entry re-exports by name, driven with
    // arguments valid enough to reach the native call.
    await analysis.init();
    const samples = new Float32Array(2048).map((_, i) => Math.sin((2 * Math.PI * 440 * i) / 22050));
    // feature_spectral, re-exported by name.
    expect(analysis.spectralCentroid(samples, 22050).length).toBeGreaterThan(0);
    expect(analysis.zeroCrossingRate(samples).length).toBeGreaterThan(0);
    // feature_spectrogram, re-exported by name. These answer a shaped object
    // rather than an array, so assert a field the native call has to fill.
    expect(analysis.stft(samples).nFrames).toBeGreaterThan(0);
    expect(analysis.melSpectrogram(samples, 22050).nMels).toBeGreaterThan(0);
    expect(analysis.chroma(samples, 22050).nChroma).toBeGreaterThan(0);
  });
});
