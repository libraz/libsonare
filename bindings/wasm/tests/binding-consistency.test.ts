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
 *    decompose with init, the maxPoints form of meteringVectorscope and
 *    meteringPhaseScope, meteringSpectrumFrame.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { BuiltinSynthConfig } from '../dist/index.js';
import {
  decompose,
  init,
  Mixer,
  meteringPhaseScope,
  meteringSpectrumFrame,
  meteringVectorscope,
  midiNoteOff,
  midiNoteOn,
  mixingScenePreset,
  mixStereo,
  Project,
} from '../dist/index.js';
import { assertStripIndex } from './_helpers';

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
    midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
    midiNoteOff({ ppq: 3, group: 0, channel: 0, note: 60, velocity: 0 }),
  ]);
  return project;
}

describe('WASM cross-binding consistency', () => {
  beforeAll(async () => {
    await init();
  });

  describe('built-in synth waveform contract', () => {
    const bounceWaveform = (waveform: unknown, totalFrames = 4800): Float32Array => {
      const project = buildMidiOnlyProject();
      try {
        return project.bounceWithBuiltinInstruments(
          [{ instrument: { waveform: waveform as 'sine' } }],
          { totalFrames, numChannels: 1 },
        ).channels[0];
      } finally {
        project.destroy();
      }
    };

    it('refuses an unknown waveform name with RangeError (matching Node/Python)', () => {
      expect(() => bounceWaveform('bogus')).toThrow(RangeError);
    });

    it('refuses a numeric waveform with TypeError, in range or not', () => {
      // The waveform is a closed set of names; an ordinal is not one of its
      // spellings, so the in-domain 0..3 are refused exactly like 4, -1 or NaN.
      for (const waveform of [0, 3, 4, -1, 2 ** 31, Number.POSITIVE_INFINITY, Number.NaN]) {
        expect(() => bounceWaveform(waveform), `waveform ${waveform}`).toThrow(
          new TypeError('Project.bounceWithBuiltinInstruments: waveform must be a name'),
        );
      }
    });

    it('accepts every waveform name and they stay distinct', () => {
      // Vacuity guard for the refusals above: four names, four different renders,
      // and the 'sawtooth' alias renders as 'saw'.
      const hash = (audio: Float32Array): number =>
        audio.reduce((h, sample) => (h * 31 + Math.round(sample * 1e6)) | 0, 0);
      const renders = (['sine', 'saw', 'square', 'triangle'] as const).map((waveform) =>
        hash(bounceWaveform(waveform, SR)),
      );
      expect(new Set(renders).size, 'the four waveforms must render differently').toBe(4);
      expect(hash(bounceWaveform('sawtooth', SR))).toBe(renders[1]);
    });

    it('accepts "sawtooth" as an alias of "saw"', () => {
      const project = buildMidiOnlyProject();
      try {
        const [audio] = project.bounceWithBuiltinInstruments(
          [{ instrument: { waveform: 'sawtooth', gain: 0.5 } }],
          { totalFrames: SR, numChannels: 1 },
        ).channels;
        expect(audio).toBeInstanceOf(Float32Array);
        expect(maxAbs(audio)).toBeGreaterThan(0.01);
      } finally {
        project.destroy();
      }
    });

    it('accepts a patch typed with the shared BuiltinSynthConfig alias', () => {
      const project = buildMidiOnlyProject();
      try {
        // The Python binding names this concept BuiltinSynthConfig; the WASM
        // alias lets portable code share that name (tsc validates the alias
        // accepts the same shape as BuiltinSynthBinding).
        const patch: BuiltinSynthConfig = { waveform: 'square', gain: 0.3 };
        const [audio] = project.bounceWithBuiltinInstruments([{ instrument: patch }], {
          totalFrames: SR,
          numChannels: 1,
        }).channels;
        expect(audio).toBeInstanceOf(Float32Array);
        expect(maxAbs(audio)).toBeGreaterThan(0.01);
      } finally {
        project.destroy();
      }
    });

    it('bounces an explicitly empty instrument array to silence', () => {
      const project = buildMidiOnlyProject();
      try {
        const [audio] = project.bounceWithBuiltinInstruments([], {
          totalFrames: SR,
          numChannels: 1,
        }).channels;
        expect(audio).toBeInstanceOf(Float32Array);
        expect(maxAbs(audio)).toBe(0);
      } finally {
        project.destroy();
      }
    });
  });

  describe('mixer setPan keep-mode and stripMeter tap', () => {
    it('keeps the current pan mode when panMode is omitted', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: SR,
        maxBlockSize: BLOCK,
      });
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        // Force StereoPan, then nudge the position with no explicit mode.
        mixer.setPan(vocal, 0, 'stereoPan');
        mixer.setPan(vocal, 0.3);
        const scene = JSON.parse(mixer.toJson()) as {
          strips: Array<{ id: string; panMode?: string }>;
        };
        const strip = scene.strips.find((s) => s.id === 'vocal');
        expect(strip?.panMode).toBe('stereoPan'); // not reset to balance

        // An explicit mode still switches it.
        mixer.setPan(vocal, 0.3, 'balance');
        const scene2 = JSON.parse(mixer.toJson()) as {
          strips: Array<{ id: string; panMode?: string }>;
        };
        expect(scene2.strips.find((s) => s.id === 'vocal')?.panMode).toBe('balance');
      } finally {
        mixer.destroy();
      }
    });

    it('stripMeter accepts an optional tap argument', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: SR,
        maxBlockSize: BLOCK,
      });
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
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
        mixer.destroy();
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
      const result = mixStereo({
        strips: [
          { left: a, right: a },
          { left: b, right: b },
        ],
        sampleRate: SR,
      });
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(BLOCK);
      expect(maxAbs(result.left)).toBeGreaterThan(0);
    });
  });

  describe('functions matching the Node/Python surface', () => {
    it('decompose with an init returns W and H factor matrices', () => {
      const nFeatures = 8;
      const nFrames = 6;
      const s = new Float32Array(nFeatures * nFrames);
      for (let i = 0; i < s.length; i++) {
        s[i] = Math.abs(Math.sin(i * 0.7)) + 0.01;
      }
      const { w, h } = decompose({
        s: { data: s, rows: nFeatures, cols: nFrames },
        nComponents: 2,
        nIter: 20,
        beta: 2.0,
        init: 'nndsvd',
      });
      expect(w.data).toBeInstanceOf(Float32Array);
      expect(h.data).toBeInstanceOf(Float32Array);
      expect(w.data.length).toBe(nFeatures * 2);
      expect(h.data.length).toBe(2 * nFrames);
    });

    it('meteringVectorscope with maxPoints decimates the point series to maxPoints', () => {
      const n = 4096;
      const left = new Float32Array(n);
      const right = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        left[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
        right[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR + 0.3);
      }
      const report = meteringVectorscope({ left, right, sampleRate: SR, maxPoints: 64 });
      expect(report.mid).toBeInstanceOf(Float32Array);
      expect(report.side).toBeInstanceOf(Float32Array);
      expect(report.mid.length).toBeLessThanOrEqual(64);
      expect(report.mid.length).toBe(report.side.length);
    });

    it('meteringPhaseScope with maxPoints decimates points but keeps full-res stats', () => {
      const n = 4096;
      const left = new Float32Array(n);
      const right = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        left[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
        right[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR + 0.3);
      }
      const report = meteringPhaseScope({ left, right, sampleRate: SR, maxPoints: 32 });
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
      const report = meteringSpectrumFrame(samples, SR, { startSample: 0, nFft: 2048 });
      expect(report.frequencies).toBeInstanceOf(Float32Array);
      expect(report.magnitude).toBeInstanceOf(Float32Array);
      expect(report.db).toBeInstanceOf(Float32Array);
      expect(report.nFft).toBe(2048);
      expect(report.frequencies.length).toBe(report.magnitude.length);
    });
  });
});
