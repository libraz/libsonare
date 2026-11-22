import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';
import { beforeAll, describe, expect, it } from 'vitest';
import * as analysis from '../dist/analysis.js';
import { analysisRegistrations, declarationText } from '../scripts/write-analysis-module-types.mjs';

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
      // Only the pitch editor is linked into the analysis bundle; the other
      // gated subsystems are absent even where their options stay on.
      arrangement: false,
      acousticSim: false,
      pitchEditor: true,
      voiceChanger: false,
      playback: false,
    });
    expect(analysis.meteringPeakDb(new Float32Array([0, 0.5, -0.25]), 48000)).toBeCloseTo(
      -6.0206,
      3,
    );
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

  /** Exported names that are a JS-side rename over a differently named native registration. */
  const NATIVE_NAME: Readonly<Record<string, string>> = {
    onsetStrength: 'onsetEnvelope',
    lufsMultichannel: 'lufsInterleaved',
    lufsSeriesMultichannel: 'lufsSeriesInterleaved',
    ebur128LoudnessRangeLu: 'ebur128LoudnessRangeLu',
    meteringTruePeakDbtp: 'meteringTruePeakDb',
    splitSilenceCommonWithReport: 'splitSilenceCommon',
  };

  const entryFunctions = (): string[] =>
    Object.entries(analysis as Record<string, unknown>)
      .filter(([name, value]) => typeof value === 'function' && !JS_ONLY.has(name))
      .map(([name]) => NATIVE_NAME[name] ?? name);

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
    expect(analysis.zeroCrossingRate(samples, 22050).length).toBeGreaterThan(0);
    // feature_spectrogram, re-exported by name. These answer a shaped object
    // rather than an array, so assert a field the native call has to fill.
    expect(analysis.stft(samples, 22050).magnitude.cols).toBeGreaterThan(0);
    expect(analysis.melSpectrogram(samples, 22050).power.rows).toBeGreaterThan(0);
    expect(analysis.chroma(samples, 22050).features.rows).toBeGreaterThan(0);
  });
});

describe('the analysis module declaration', () => {
  const DECLARATION = fileURLToPath(new URL('../src/sonare-analysis.js.d.ts', import.meta.url));

  /** Property names of the module type the declared factory resolves to. */
  function declaredModuleKeys(): string[] {
    // The package's own resolution: `./sonare.js` reaches `sonare.js.d.ts` only under it,
    // and an unresolved import reads as `any`, which has no properties to compare.
    const root = fileURLToPath(new URL('..', import.meta.url));
    const config = ts.parseJsonConfigFileContent(
      ts.readConfigFile(`${root}/tsconfig.json`, ts.sys.readFile).config,
      ts.sys,
      root,
    );
    const program = ts.createProgram([DECLARATION], { ...config.options, noEmit: true });
    const checker = program.getTypeChecker();
    const file = program.getSourceFile(DECLARATION);
    const moduleSymbol = file && checker.getSymbolAtLocation(file);
    const factory = moduleSymbol
      ? checker.getExportsOfModule(moduleSymbol).find((s) => s.name === 'default')
      : undefined;
    if (!factory) throw new Error(`${DECLARATION} has no default export`);
    const [signature] = checker.getTypeOfSymbol(factory).getCallSignatures();
    const promised = checker.getAwaitedType(signature.getReturnType());
    if (!promised || promised.flags & ts.TypeFlags.Any) {
      throw new Error('the factory does not resolve to a declared module type');
    }
    return checker
      .getPropertiesOfType(promised)
      .map((p) => p.name)
      .sort();
  }

  it('names exactly what the analysis-only binary registers', async () => {
    const registered = await analysisRegistrations();
    expect(registered.length).toBeGreaterThan(100);
    expect(declaredModuleKeys()).toEqual(registered);
  });

  it('is the text the generator writes for those registrations', async () => {
    expect(readFileSync(DECLARATION, 'utf8')).toBe(declarationText(await analysisRegistrations()));
  });
});
