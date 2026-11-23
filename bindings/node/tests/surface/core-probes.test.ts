import { describe, expect, expectTypeOf, it } from 'vitest';
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
});

const FEATURE_KEYS = [
  'acousticSim',
  'arrangement',
  'assist',
  'ffmpeg',
  'fx',
  'graph',
  'instrumentParamAutomation',
  'liveDevices',
  'mastering',
  'mixing',
  'mixingAssistant',
  'pitchEditor',
  'playback',
  'project',
  'vocalEdit',
  'voiceChanger',
] as const;

describe('capabilities().features', () => {
  it('carries exactly the closed set of feature keys, each a boolean', () => {
    const { features } = mod.capabilities();
    expect(Object.keys(features).sort()).toEqual([...FEATURE_KEYS]);
    for (const key of FEATURE_KEYS) {
      expect(typeof features[key], key).toBe('boolean');
    }
  });

  it('reports live device support as unavailable in the native addon', () => {
    expect(mod.capabilities().features.liveDevices).toBe(false);
  });

  it('declares every key as a required boolean and refuses unknown keys', () => {
    expectTypeOf<keyof mod.Capabilities['features']>().toEqualTypeOf<
      (typeof FEATURE_KEYS)[number]
    >();
    expectTypeOf<mod.Capabilities['features']['project']>().toEqualTypeOf<boolean>();
    const typeOnly = (features: mod.Capabilities['features']) => {
      // @ts-expect-error the feature set is closed
      return features.hasFeature;
    };
    expect(typeof typeOnly).toBe('function');
  });
});

describe('VocalCapabilities', () => {
  it('reports the layout version as abiVersion', () => {
    const sampleRate = 16000;
    const samples = new Float32Array(sampleRate);
    for (let i = 0; i < samples.length; i++) {
      samples[i] = 0.35 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
    }
    const session = mod.createVocalEditSession({ samples, sampleRate });
    try {
      const caps = session.capabilities();
      expect(caps.abiVersion).toBe(mod.vocalEditAbiVersion());
      expect('apiVersion' in caps).toBe(false);
      expectTypeOf<mod.VocalCapabilities['abiVersion']>().toEqualTypeOf<number>();
    } finally {
      session.destroy();
    }
  });
});
