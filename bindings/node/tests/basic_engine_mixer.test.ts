import { describe, expect, it } from 'vitest';
import { Mixer, mixingScenePresetJson, RealtimeEngine } from '../src/index.js';

describe('RealtimeEngine', () => {
  it('processWithMonitor returns output and monitor buses', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      const result = engine.processWithMonitor([
        new Float32Array(16).fill(0.25),
        new Float32Array(16).fill(-0.25),
      ]);
      expect(result.output).toHaveLength(2);
      expect(result.monitor).toHaveLength(2);
      expect(result.output[0][0]).toBeCloseTo(0.25);
      expect(result.output[1][0]).toBeCloseTo(-0.25);
      expect(result.monitor[0][0]).toBeCloseTo(0);
      expect(result.monitor[1][0]).toBeCloseTo(0);
    } finally {
      engine.destroy();
    }
  });

  it('exposes live MIDI CC bindings', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      expect(engine.midiCcBindingCount()).toBe(0);
      engine.bindMidiCc(0, 74, 7, { minValue: -60, maxValue: 0 });
      expect(engine.midiCcBindingCount()).toBe(1);
      engine.clearMidiCcBindings();
      expect(engine.midiCcBindingCount()).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  it('exposes live non-destructive MIDI FX inserts', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      expect(() => engine.setMidiFx(0, '{"transpose_semitones":12}')).not.toThrow();
      expect(() => engine.clearMidiFx(0)).not.toThrow();
      expect(() => engine.setMidiFx(0, '{bad json')).toThrow();
      expect(() => engine.setMidiFx(0, '{"quantize_ppq":0}')).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('exposes an owned live MIDI input source', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      engine.setMidiInputSource(0);
      expect(engine.midiInputPendingCount()).toBe(0);
      engine.pushMidiInputNoteOn(0, 0, 60, 100, 3);
      expect(engine.midiInputPendingCount()).toBe(1);
      engine.process([new Float32Array(16), new Float32Array(16)]);
      expect(engine.midiInputPendingCount()).toBe(0);
      engine.clearMidiInputSource();
      expect(() => engine.pushMidiInputNoteOff(0, 0, 60, 0, 0)).toThrow();
    } finally {
      engine.destroy();
    }
  });
});

describe('Mixer (scene-based routing)', () => {
  it('routes a preset scene and schedules insert automation', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();
      expect(mixer.stripCount()).toBeGreaterThan(0);

      // Strip 0 (vocal) carries pre-fader inserts; schedule a no-throw event.
      expect(() => mixer.scheduleInsertAutomation(0, 0, 0, 0, 0.0)).not.toThrow();
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, 0, 48000, 1.0, 'exponential'),
      ).not.toThrow();

      // Out-of-range strip index must throw.
      expect(() => mixer.scheduleInsertAutomation(999, 0, 0, 0, 0.0)).toThrow();

      const block = 512;
      const vocalL = new Float32Array(block);
      const vocalR = new Float32Array(block);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(block);
      const silentR = new Float32Array(block);
      const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
      expect(out.left.length).toBe(block);
      expect(out.sampleRate).toBe(48000);

      const scene = mixer.toSceneJson();
      expect(scene).toContain('vocal-verb');
    } finally {
      mixer.destroy();
    }
  });
});
