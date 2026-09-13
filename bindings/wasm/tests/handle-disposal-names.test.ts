/**
 * Disposal-method naming across the WASM handle classes.
 *
 * embind names its release method `delete()`, the Node binding names it
 * `destroy()`, and a host that cleans up both surfaces with one code path needs
 * every class to answer to the same name. Each handle class therefore accepts
 * both spellings; `StreamAnalyzer` additionally keeps its historical
 * `dispose()`.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzePolyphonic,
  init,
  Mixer,
  mixingScenePresetJson,
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
  ];

  for (const [name, create] of handles) {
    it(`${name} accepts both delete() and destroy()`, () => {
      const handle = create();
      expect(typeof handle.delete).toBe('function');
      expect(typeof handle.destroy).toBe('function');
      // The generic cross-surface cleanup path: one `destroy()` call frees the
      // native object, so a following `delete()` would double-free.
      handle.destroy();
    });
  }

  it('keeps the historical StreamAnalyzer.dispose() alias', () => {
    const analyzer = new StreamAnalyzer({ sampleRate: 22050 });
    expect(typeof analyzer.dispose).toBe('function');
    analyzer.dispose();
  });
});
