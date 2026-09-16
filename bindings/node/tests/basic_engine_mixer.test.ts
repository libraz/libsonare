import { describe, expect, it } from 'vitest';
import { Mixer, mixingScenePresetJson, RealtimeEngine } from '../src/index.js';

/** One strip feeding a master bus that carries a peaking band at `gainDb`. */
function sceneWithBusEq(gainDb: number): string {
  return JSON.stringify({
    version: 1,
    buses: [
      {
        id: 'master',
        role: 'master',
        inserts: [
          {
            slot: 'pre',
            processor: 'eq.parametric',
            params: JSON.stringify({
              'band0.frequencyHz': 1000,
              'band0.gainDb': gainDb,
              'band0.q': 2,
            }),
          },
        ],
      },
    ],
    strips: [{ id: 'a' }],
    connections: [{ source: 'a', destination: 'master' }],
  });
}

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

  it('schedules a PFL monitor tap for a mono lane without changing main output', () => {
    const blockSize = 16;
    const engine = new RealtimeEngine(48000, blockSize);
    try {
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(blockSize * 2).fill(0.5)],
          startPpq: 0,
          lengthSamples: blockSize * 2,
        },
      ]);
      engine.setTrackLanes([10]);
      engine.play();
      engine.setTrackMonitorMode(0, 'pfl', blockSize / 2);

      const result = engine.processWithMonitor([new Float32Array(blockSize)]);
      expect(result.output).toHaveLength(1);
      expect(result.monitor).toHaveLength(1);
      for (let i = 0; i < blockSize; i += 1) {
        expect(result.output[0][i]).toBeCloseTo(0.5, 4);
      }
      for (let i = 0; i < blockSize / 2; i += 1) {
        expect(result.monitor[0][i]).toBeCloseTo(0, 5);
      }
      for (let i = blockSize / 2; i < blockSize; i += 1) {
        expect(result.monitor[0][i]).toBeCloseTo(0.5, 4);
      }
    } finally {
      engine.destroy();
    }
  });

  it('reports an unknown monitor lane through asynchronous telemetry', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      expect(() => engine.setTrackMonitorMode(99, 'pfl')).not.toThrow();
      engine.process([new Float32Array(16)]);
      expect(engine.drainTelemetry().some((record) => record.error === 7)).toBe(true);
    } finally {
      engine.destroy();
    }
  });

  it('exposes live MIDI CC bindings', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      expect(engine.midiCcBindingCount()).toBe(0);
      engine.bindMidiCc(0, 74, 7, { minValue: -60, maxValue: 0 });
      engine.bindMidiCcBinding({
        ccNumber: 1,
        ccLsbNumber: 33,
        channel: 0,
        kind: 1,
        paramId: 8,
        minValue: 0,
        maxValue: 1,
      });
      expect(engine.midiCcBindingCount()).toBe(2);
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

  it('reports a non-zero discard count once a strip processes a near-FLT_MAX input', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();

      const block = 512;
      const silentL = new Float32Array(block);
      const silentR = new Float32Array(block);
      const vocalL = new Float32Array(block).fill(0.25);
      const vocalR = new Float32Array(block).fill(0.25);

      // A clean, ordinary-level block through strip 0 ("vocal") discards nothing.
      mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
      expect(mixer.stripNonFiniteDiscardCount(0)).toBe(0);

      // A finite but near-FLT_MAX input is still accepted by processStereo, which
      // rejects non-finite rather than large. Strip 0's pre-fader eq.parametric
      // insert then accumulates several such samples into one biquad output, and
      // that sum leaves float range, poisoning the insert's recursive state. The
      // guard under test returns that state to its post-reset value and counts
      // the block.
      const hotL = new Float32Array(block).fill(1.0e38);
      const hotR = new Float32Array(block).fill(1.0e38);
      mixer.processStereo([hotL, silentL], [hotR, silentR]);
      expect(mixer.stripNonFiniteDiscardCount(0)).toBeGreaterThan(0);
    } finally {
      mixer.destroy();
    }
  });

  it('reports a rising bus discard count once a bus insert overflows', () => {
    const mixer = Mixer.fromSceneJson(sceneWithBusEq(200.0), 48000, 64);
    try {
      const block = new Float32Array(64);
      for (let i = 0; i < block.length; i += 1) {
        block[i] = 0.3 * Math.sin((2 * Math.PI * 440 * i) / 48000);
      }
      // Control: an ordinary block leaves the count at zero, so the rise below
      // is attributable to the poison rather than to processing at all.
      mixer.processStereo([block], [block]);
      expect(mixer.busNonFiniteDiscardCount('master')).toBe(0);

      const poison = new Float32Array(64).fill(1.0e38);
      mixer.processStereo([poison], [poison]);
      expect(mixer.busNonFiniteDiscardCount('master')).toBeGreaterThan(0);
    } finally {
      mixer.destroy();
    }
  });

  it('rejects an uncompiled or unknown bus', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();
      // Baseline: a freshly compiled bus has discarded nothing.
      expect(mixer.busNonFiniteDiscardCount('master')).toBe(0);

      // A bus declared with addBus has no DSP record until the next compile,
      // so reading it before that throws rather than reading as a clean zero
      // (a zero here would be indistinguishable from an actually-clean bus).
      mixer.addBus('extra-aux', 'aux');
      expect(() => mixer.busNonFiniteDiscardCount('extra-aux')).toThrow();

      // An id with no matching bus at all also throws.
      expect(() => mixer.busNonFiniteDiscardCount('does-not-exist')).toThrow();
    } finally {
      mixer.destroy();
    }
  });
});
