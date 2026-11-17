/**
 * Teardown naming across the WASM handle classes: every class releases through
 * `destroy()` (idempotent, so a cleanup path that runs twice never surfaces
 * embind's raw BindingError) and carries no other release name.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzePolyphonic,
  createVocalEditSession,
  HrtfSet,
  init,
  Mixer,
  mixingScenePresetJson,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  Project,
  RealtimeEngine,
  RealtimeVoiceChanger,
  SampleBank,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
} from '../dist/index.js';

/** A short tone, enough for an analysis handle to exist. */
function tone(): Float32Array {
  const out = new Float32Array(8192);
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * 220 * i) / 22050);
  }
  return out;
}

const packageRoot = new URL('../', import.meta.url);
function hrtfBytes(): Uint8Array {
  return new Uint8Array(readFileSync(new URL('dist/hrtf/default.shrf', packageRoot)));
}

interface Disposable {
  destroy(): void;
}

describe('WASM handle teardown names', () => {
  beforeAll(async () => {
    await init();
  });

  const handles: ReadonlyArray<[string, () => Disposable]> = [
    ['StreamingMasteringChain', () => new StreamingMasteringChain({ 'eq.tilt.tiltDb': 0.5 })],
    ['StreamingEqualizer', () => new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 })],
    ['StreamingRetune', () => new StreamingRetune({ semitones: 12, mix: 1, grainSize: 512 })],
    ['RealtimeVoiceChanger', () => new RealtimeVoiceChanger('neutral-monitor')],
    ['StreamAnalyzer', () => new StreamAnalyzer({ sampleRate: 22050 })],
    ['RealtimeEngine', () => new RealtimeEngine(48000, 128)],
    ['Project', () => new Project()],
    ['SampleBank', () => new SampleBank()],
    ['Mixer', () => Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512)],
    ['PolyphonicAnalysis', () => analyzePolyphonic({ samples: tone(), sampleRate: 22050 })],
    ['HrtfSet', () => HrtfSet.fromBytes(hrtfBytes())],
    [
      'PlaybackRenderer',
      () => new PlaybackRenderer({ config: {}, hrtf: HrtfSet.fromBytes(hrtfBytes()) }),
    ],
    ['PlaybackLoudnessMeter', () => new PlaybackLoudnessMeter(2, 48000)],
    ['VocalEditSession', () => createVocalEditSession({ samples: tone(), sampleRate: 22050 })],
  ];

  for (const [name, create] of handles) {
    it(`${name} releases through destroy() only`, () => {
      const handle = create();
      expect(typeof handle.destroy).toBe('function');
      expect('delete' in handle).toBe(false);
      expect('dispose' in handle).toBe(false);
      handle.destroy();
    });

    it(`${name} is idempotent under a second destroy()`, () => {
      const handle = create();
      handle.destroy();
      expect(() => handle.destroy()).not.toThrow();
    });
  }
});
