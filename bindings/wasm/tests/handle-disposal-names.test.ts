/**
 * Disposal-method naming across the WASM handle classes.
 *
 * embind names its release method `delete()`, the Node binding names it
 * `destroy()`, and a host that cleans up both surfaces with one code path needs
 * every class to answer to the same name. Each handle class therefore accepts
 * both spellings; `StreamAnalyzer` additionally keeps its historical
 * `dispose()`.
 *
 * Every class is also idempotent under repeated disposal: embind's own
 * `delete()` throws a raw BindingError on a second call, and the generic
 * cross-surface cleanup path (`finally { h.destroy() }` running twice, a React
 * effect cleanup firing twice) must not surface that raw error on any class.
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
  delete(): void;
  destroy(): void;
}

describe('WASM handle disposal names', () => {
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
    it(`${name} accepts both delete() and destroy()`, () => {
      const handle = create();
      expect(typeof handle.delete).toBe('function');
      expect(typeof handle.destroy).toBe('function');
      handle.destroy();
    });

    it(`${name} is idempotent under a second delete()/destroy() in either order`, () => {
      // Two independent handles, driven through both name orderings: embind's
      // own delete() throws a raw BindingError on a second call, which is
      // exactly the defect this test is written to catch.
      const first = create();
      first.delete();
      expect(() => first.delete()).not.toThrow();

      const second = create();
      second.destroy();
      expect(() => second.destroy()).not.toThrow();
      expect(() => second.delete()).not.toThrow();
    });
  }

  it('keeps the historical StreamAnalyzer.dispose() alias', () => {
    const analyzer = new StreamAnalyzer({ sampleRate: 22050 });
    expect(typeof analyzer.dispose).toBe('function');
    analyzer.dispose();
    // dispose() is also delete(), so it must be idempotent through every name.
    expect(() => analyzer.dispose()).not.toThrow();
    expect(() => analyzer.destroy()).not.toThrow();
  });
});
