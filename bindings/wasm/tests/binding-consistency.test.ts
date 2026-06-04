/**
 * WASM coverage for cross-binding consistency with Node/Python:
 *  - unknown built-in synth waveform names throw (matching Node/Python) and
 *    "sawtooth" is accepted as an alias of "saw";
 *  - an explicitly empty instrument array bounces to silence (not a default
 *    sine patch);
 *  - setPan with no panMode keeps the strip's current pan mode;
 *  - stripMeter accepts an optional tap argument;
 *  - one-shot mixStereo routes through the real graph (master output);
 *  - the four functions that round out WASM parity with Node/Python:
 *    decomposeWithInit, meteringVectorscopeDecimated,
 *    meteringPhaseScopeDecimated, meteringSpectrumFrame.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { BuiltinSynthConfig } from '../dist/index.js';
import {
  decomposeWithInit,
  init,
  Mixer,
  meteringPhaseScopeDecimated,
  meteringSpectrumFrame,
  meteringVectorscopeDecimated,
  mixingScenePresetJson,
  mixStereo,
  Project,
} from '../dist/index.js';

const SR = 48000;
const BLOCK = 512;

function maxAbs(buffer: Float32Array): number {
  let peak = 0;
  for (let i = 0; i < buffer.length; i++) {
    const v = Math.abs(buffer[i]);
    if (v > peak) {
      peak = v;
    }
  }
  return peak;
}

function buildMidiOnlyProject(): Project {
  const project = new Project();
  project.setSampleRate(SR);
  const { clipId } = project.addMidiClip(0, 4);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(3, 0, 0, 60, 0),
  ]);
  return project;
}

describe('WASM cross-binding consistency', () => {
  beforeAll(async () => {
    await init();
  });

  describe('built-in synth waveform contract', () => {
    it('throws on an unknown waveform name (matching Node/Python)', () => {
      const project = buildMidiOnlyProject();
      try {
        expect(() =>
          project.bounceWithBuiltinInstrument(
            { waveform: 'bogus' as unknown as 'sine' },
            { totalFrames: 4800, numChannels: 1 },
          ),
        ).toThrow();
      } finally {
        project.delete();
      }
    });

    it('accepts "sawtooth" as an alias of "saw"', () => {
      const project = buildMidiOnlyProject();
      try {
        const audio = project.bounceWithBuiltinInstrument(
          { waveform: 'sawtooth', gain: 0.5 },
          { totalFrames: SR, numChannels: 1 },
        );
        expect(audio).toBeInstanceOf(Float32Array);
        expect(maxAbs(audio)).toBeGreaterThan(0.01);
      } finally {
        project.delete();
      }
    });

    it('accepts a patch typed with the shared BuiltinSynthConfig alias', () => {
      const project = buildMidiOnlyProject();
      try {
        // The Python binding names this concept BuiltinSynthConfig; the WASM
        // alias lets portable code share that name (tsc validates the alias
        // accepts the same shape as BuiltinSynthBinding).
        const patch: BuiltinSynthConfig = { waveform: 'square', gain: 0.3 };
        const audio = project.bounceWithBuiltinInstrument(patch, {
          totalFrames: SR,
          numChannels: 1,
        });
        expect(audio).toBeInstanceOf(Float32Array);
        expect(maxAbs(audio)).toBeGreaterThan(0.01);
      } finally {
        project.delete();
      }
    });

    it('bounces an explicitly empty instrument array to silence', () => {
      const project = buildMidiOnlyProject();
      try {
        const audio = project.bounceWithBuiltinInstrument([], {
          totalFrames: SR,
          numChannels: 1,
        });
        expect(audio).toBeInstanceOf(Float32Array);
        expect(maxAbs(audio)).toBe(0);
      } finally {
        project.delete();
      }
    });
  });

  describe('mixer setPan keep-mode and stripMeter tap', () => {
    it('keeps the current pan mode when panMode is omitted', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        // Force StereoPan, then nudge the position with no explicit mode.
        mixer.setPan(vocal, 0, 'stereoPan');
        mixer.setPan(vocal, 0.3);
        const scene = JSON.parse(mixer.toSceneJson()) as {
          strips: Array<{ id: string; panMode?: number }>;
        };
        const strip = scene.strips.find((s) => s.id === 'vocal');
        expect(strip?.panMode).toBe(1); // 1 = StereoPan, not reset to 0 (Balance)

        // An explicit mode still switches it.
        mixer.setPan(vocal, 0.3, 'balance');
        const scene2 = JSON.parse(mixer.toSceneJson()) as {
          strips: Array<{ id: string; panMode?: number }>;
        };
        expect(scene2.strips.find((s) => s.id === 'vocal')?.panMode).toBe(0);
      } finally {
        mixer.delete();
      }
    });

    it('stripMeter accepts an optional tap argument', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        mixer.compile();
        const block = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
          block[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
        }
        mixer.processStereo([block, new Float32Array(BLOCK)], [block, new Float32Array(BLOCK)]);
        const post = mixer.stripMeter(vocal);
        const postTap = mixer.stripMeter(vocal, 'postFader');
        const preTap = mixer.stripMeter(vocal, 'preFader');
        expect(Number.isFinite(post.peakDbL)).toBe(true);
        expect(Number.isFinite(postTap.peakDbL)).toBe(true);
        expect(Number.isFinite(preTap.peakDbL)).toBe(true);
      } finally {
        mixer.delete();
      }
    });
  });

  describe('one-shot mixStereo routes through the real graph', () => {
    it('sums inputs into a non-silent master', () => {
      const a = new Float32Array(BLOCK);
      const b = new Float32Array(BLOCK);
      for (let i = 0; i < BLOCK; i++) {
        a[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / SR);
        b[i] = 0.4 * Math.sin((2 * Math.PI * 330 * i) / SR);
      }
      const result = mixStereo([a, b], [a, b], SR);
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(BLOCK);
      expect(maxAbs(result.left)).toBeGreaterThan(0);
    });
  });

  describe('functions matching the Node/Python surface', () => {
    it('decomposeWithInit returns W and H factor matrices', () => {
      const nFeatures = 8;
      const nFrames = 6;
      const s = new Float32Array(nFeatures * nFrames);
      for (let i = 0; i < s.length; i++) {
        s[i] = Math.abs(Math.sin(i * 0.7)) + 0.01;
      }
      const { w, h } = decomposeWithInit(s, nFeatures, nFrames, 2, 20, 2.0, 'nndsvd');
      expect(w).toBeInstanceOf(Float32Array);
      expect(h).toBeInstanceOf(Float32Array);
      expect(w.length).toBe(nFeatures * 2);
      expect(h.length).toBe(2 * nFrames);
    });

    it('meteringVectorscopeDecimated decimates the point series to maxPoints', () => {
      const n = 4096;
      const left = new Float32Array(n);
      const right = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        left[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
        right[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR + 0.3);
      }
      const report = meteringVectorscopeDecimated(left, right, SR, 64);
      expect(report.mid).toBeInstanceOf(Float32Array);
      expect(report.side).toBeInstanceOf(Float32Array);
      expect(report.mid.length).toBeLessThanOrEqual(64);
      expect(report.mid.length).toBe(report.side.length);
    });

    it('meteringPhaseScopeDecimated decimates points but keeps full-res stats', () => {
      const n = 4096;
      const left = new Float32Array(n);
      const right = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        left[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
        right[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR + 0.3);
      }
      const report = meteringPhaseScopeDecimated(left, right, SR, 32);
      expect(report.mid.length).toBeLessThanOrEqual(32);
      expect(Number.isFinite(report.correlation)).toBe(true);
      expect(Number.isFinite(report.maxRadius)).toBe(true);
    });

    it('meteringSpectrumFrame returns a single-frame spectrum snapshot', () => {
      const n = 4096;
      const samples = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        samples[i] = 0.5 * Math.sin((2 * Math.PI * 1000 * i) / SR);
      }
      const report = meteringSpectrumFrame(samples, SR, 0, { nFft: 2048 });
      expect(report.frequencies).toBeInstanceOf(Float32Array);
      expect(report.magnitude).toBeInstanceOf(Float32Array);
      expect(report.db).toBeInstanceOf(Float32Array);
      expect(report.nFft).toBe(2048);
      expect(report.frequencies.length).toBe(report.magnitude.length);
    });
  });
});
