import { describe, expect, it } from 'vitest';
import {
  capabilities,
  HrtfSet,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  renderPlayback,
} from '../src/index.js';
import { sine } from './_helpers.js';

function stereoBlock(frames: number): [Float32Array, Float32Array] {
  const tone = sine(440, frames / 48000, { sampleRate: 48000, amp: 0.2 });
  return [tone.slice(0, frames), tone.slice(0, frames)];
}

describe('capabilities', () => {
  it('reports the playback feature flag', () => {
    expect(typeof capabilities().features.playback).toBe('boolean');
  });
});

describe('HrtfSet', () => {
  it('default() builds the built-in set and destroy() is idempotent', () => {
    const hrtf = HrtfSet.default();
    expect(() => hrtf.destroy()).not.toThrow();
    expect(() => hrtf.destroy()).not.toThrow();
  });

  it('fromBytes() rejects malformed SHRF data', () => {
    expect(() => HrtfSet.fromBytes(new Uint8Array([1, 2, 3, 4]))).toThrow();
  });
});

describe('PlaybackRenderer construction and validation', () => {
  it('defaults a headphones target without an explicit HrtfSet (native built-in)', () => {
    using renderer = new PlaybackRenderer({ config: {} });
    expect(renderer.outputChannels()).toBe(2);
    renderer.destroy();
  });

  it('rejects an unknown top-level config key', () => {
    expect(
      () => new PlaybackRenderer({ config: { nope: true } as never, sampleRate: 48000 }),
    ).toThrow();
  });

  it('rejects input.channel_map combined with input.layout "auto"', () => {
    expect(
      () =>
        new PlaybackRenderer({
          config: { input: { layout: 'auto', channel_map: ['L', 'R'] } },
        }),
    ).toThrow();
  });

  it('rejects a speakers target with no target.layout', () => {
    expect(() => new PlaybackRenderer({ config: { target: { kind: 'speakers' } } })).toThrow();
  });
});

describe('PlaybackRenderer processing', () => {
  it('processes a stereo planar block to a stereo-speakers target', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
      sampleRate: 48000,
      maxBlockSize: 256,
    });
    const [l, r] = stereoBlock(128);
    const out = renderer.processPlanar([l, r]);
    expect(out).toHaveLength(2);
    expect(out[0]).toHaveLength(128);
    expect(Array.from(out[0]).every((s) => Number.isFinite(s))).toBe(true);
    expect(renderer.inputChannels()).toBe(2);
    expect(renderer.outputChannels()).toBe(2);
  });

  it('processes a stereo interleaved block to a 5.1-speakers target', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: '5.1' } },
      sampleRate: 48000,
      maxBlockSize: 256,
    });
    const [l, r] = stereoBlock(128);
    const interleaved = new Float32Array(256);
    for (let i = 0; i < 128; i++) {
      interleaved[i * 2] = l[i];
      interleaved[i * 2 + 1] = r[i];
    }
    const out = renderer.processInterleaved(interleaved, 2);
    expect(renderer.outputChannels()).toBe(6);
    expect(out).toHaveLength(128 * 6);
    expect(Array.from(out).every((s) => Number.isFinite(s))).toBe(true);
  });

  it('rejects a non-number inChannels argument to processInterleaved and stays usable', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    expect(() =>
      renderer.processInterleaved(new Float32Array(4), 'two' as unknown as number),
    ).toThrow();
    // The rejected call must not have advanced state or left the addon unusable.
    expect(renderer.outputChannels()).toBe(2);
  });

  it('rejects a planar call whose planes have mismatched lengths', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const l = new Float32Array(64);
    const r = new Float32Array(32);
    expect(() => renderer.processPlanar([l, r])).toThrow();
  });

  it('rejects an unsupported input channel count under input.layout "auto"', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const planes = [new Float32Array(32), new Float32Array(32), new Float32Array(32)];
    expect(() => renderer.processPlanar(planes)).toThrow();
  });
});

describe('PlaybackRenderer latency, config and diagnostics', () => {
  it('reports the documented fixed latency at 48 kHz (stereo speakers vs. every other target)', () => {
    using speakers = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
      sampleRate: 48000,
    });
    expect(speakers.latencySamples()).toBe(288);

    using surround = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: '5.1' } },
      sampleRate: 48000,
    });
    expect(surround.latencySamples()).toBe(1312);

    using headphones = new PlaybackRenderer({ config: {}, sampleRate: 48000 });
    expect(headphones.latencySamples()).toBe(1312);
  });

  it('round-trips a realtime key through setConfig/config and rejects a changed prepare key', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const config = renderer.config();
    expect(config.night_mode?.amount ?? 0).toBe(0);

    renderer.setConfig({ ...config, night_mode: { amount: 0.5 } });
    expect(renderer.config().night_mode?.amount).toBeCloseTo(0.5, 5);

    expect(() =>
      renderer.setConfig({ ...config, target: { kind: 'speakers', layout: '5.1' } }),
    ).toThrow();
  });

  it('accepts setHeadOrientation and reset without throwing on both target kinds', () => {
    using headphones = new PlaybackRenderer({ config: {} });
    expect(() => headphones.setHeadOrientation(30, 0, 0)).not.toThrow();
    expect(() => headphones.reset()).not.toThrow();

    using speakers = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    // Ignored by a speakers target, but the call itself must not throw.
    expect(() => speakers.setHeadOrientation(30)).not.toThrow();
  });

  it('rejects a non-number pitchDeg/rollDeg and stays usable', () => {
    using renderer = new PlaybackRenderer({ config: {} });
    expect(() => renderer.setHeadOrientation(0, 'up' as unknown as number, 0)).toThrow();
    expect(() => renderer.setHeadOrientation(0, 0, 'level' as unknown as number)).toThrow();
    expect(() => renderer.setHeadOrientation(0, 0, 0)).not.toThrow();
  });

  it('reports a diagnostics document with the documented shape', () => {
    using renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const diagnostics = renderer.diagnostics();
    expect(diagnostics.active_input_layout).toBe('stereo');
    expect(diagnostics.layout_switches).toBe(0);
    expect(diagnostics.truncated_drains).toBe(0);
    expect(Array.isArray(diagnostics.inactive_stages)).toBe(true);
    expect(diagnostics.inactive_stages).toContain('room_early');
    expect(typeof diagnostics.latency.samples).toBe('number');
    expect(typeof diagnostics.loudness_gain_db).toBe('number');
    expect(typeof diagnostics.hrtf_ignored).toBe('boolean');
    expect(renderer.nonFiniteDiscardCount()).toBe(0);
  });

  it('rejects use after destroy', () => {
    const renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    renderer.destroy();
    expect(() => renderer.destroy()).not.toThrow();
    expect(() => renderer.reset()).toThrow('destroyed');
  });
});

describe('PlaybackLoudnessMeter', () => {
  it('measures a finite integrated LUFS after feeding interleaved stereo audio', () => {
    using meter = new PlaybackLoudnessMeter(2, 48000);
    const [l, r] = stereoBlock(48000);
    const interleaved = new Float32Array(l.length * 2);
    for (let i = 0; i < l.length; i++) {
      interleaved[i * 2] = l[i];
      interleaved[i * 2 + 1] = r[i];
    }
    meter.pushInterleaved(interleaved);
    expect(Number.isFinite(meter.integratedLufs())).toBe(true);
  });

  it('rejects a non-number channels or sampleRate argument', () => {
    expect(() => new PlaybackLoudnessMeter('two' as unknown as number, 48000)).toThrow();
    expect(() => new PlaybackLoudnessMeter(2, 'fast' as unknown as number)).toThrow();
    // The addon must still be usable after the rejected construction.
    using meter = new PlaybackLoudnessMeter(2, 48000);
    expect(Number.isFinite(meter.integratedLufs())).toBe(true);
  });
});

describe('renderPlayback', () => {
  it('renders a whole stereo buffer offline to a headphones target', () => {
    const [l, r] = stereoBlock(4096);
    const interleaved = new Float32Array(l.length * 2);
    for (let i = 0; i < l.length; i++) {
      interleaved[i * 2] = l[i];
      interleaved[i * 2 + 1] = r[i];
    }
    const result = renderPlayback({
      samples: interleaved,
      channels: 2,
      sampleRate: 48000,
      config: {},
    });
    expect(result.channels).toBe(2);
    expect(result.samples).toHaveLength(l.length * result.channels);
    expect(Array.from(result.samples).every((s) => Number.isFinite(s))).toBe(true);
  });

  it('rejects a non-number channels or sampleRate argument and stays usable', () => {
    const samples = new Float32Array(8);
    expect(() =>
      renderPlayback({
        samples,
        channels: 'two' as unknown as number,
        sampleRate: 48000,
        config: {},
      }),
    ).toThrow();
    expect(() =>
      renderPlayback({
        samples,
        channels: 2,
        sampleRate: 'fast' as unknown as number,
        config: {},
      }),
    ).toThrow();
    const result = renderPlayback({ samples, channels: 2, sampleRate: 48000, config: {} });
    expect(result.channels).toBe(2);
  });
});
