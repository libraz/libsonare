import { beforeAll, describe, expect, it } from 'vitest';
import * as analysis from '../../dist/analysis.js';
import * as mod from '../../dist/index.js';

const abiProbes = [
  'abiVersion',
  'engineAbiVersion',
  'voiceChangerAbiVersion',
  'projectAbiVersion',
  'vocalEditAbiVersion',
] as const;

const removedNames = [
  'hasFfmpegSupport',
  'vocalEditAvailable',
  'vocalEditApiVersion',
  'engineCapabilities',
  'EXPECTED_ABI_VERSION',
  'EXPECTED_ENGINE_ABI_VERSION',
  'EXPECTED_PROJECT_ABI_VERSION',
] as const;

describe('ABI and capability probes', () => {
  beforeAll(async () => {
    await mod.init();
    await analysis.init();
  });

  it.each(abiProbes)('%s is a function returning a positive integer', (name) => {
    const probe = (mod as Record<string, unknown>)[name];
    expect(typeof probe).toBe('function');
    const value = (probe as () => unknown)();
    expect(Number.isInteger(value)).toBe(true);
    expect(value as number).toBeGreaterThan(0);
  });

  it('reports ffmpeg and vocalEdit as capability features', () => {
    const { features } = mod.capabilities();
    expect(typeof features.ffmpeg).toBe('boolean');
    expect(typeof features.vocalEdit).toBe('boolean');
  });

  it.each(removedNames)('does not export %s from the package root', (name) => {
    expect(name in mod).toBe(false);
  });

  it('does not export the removed probes from the analysis entry', () => {
    for (const name of ['hasFfmpegSupport', 'EXPECTED_ABI_VERSION'] as const) {
      expect(name in analysis).toBe(false);
    }
  });

  it('analysis entry answers the library ABI probes as functions', () => {
    for (const value of [
      analysis.abiVersion(),
      analysis.engineAbiVersion(),
      analysis.voiceChangerAbiVersion(),
    ]) {
      expect(Number.isInteger(value)).toBe(true);
      expect(value).toBeGreaterThan(0);
    }
    expect(typeof analysis.capabilities().features.ffmpeg).toBe('boolean');
  });
});
