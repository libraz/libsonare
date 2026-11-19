import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError } from '../src/errors.js';
import {
  capabilities,
  HrtfSet,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  renderPlayback,
} from '../src/index.js';
import type { PlaybackRendererConfig } from '../src/types_playback.js';
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
  it('requires sampleRate, reports the default block size when maxBlockSize is omitted and refuses 0', () => {
    const config = { target: { kind: 'speakers', layout: 'stereo' } } as const;
    expect(() => new PlaybackRenderer({ config } as never)).toThrow(TypeError);
    expect(() => new PlaybackRenderer({ sampleRate: 48000, config, maxBlockSize: 0 })).toThrow(
      RangeError,
    );
    const renderer = new PlaybackRenderer({ sampleRate: 48000, config });
    try {
      const max = renderer.maxBlockSize;
      expect(max).toBeGreaterThan(0);
      const block = new Float32Array(max);
      expect(renderer.processPlanar([block, block])).toHaveLength(2);
      const over = new Float32Array(max + 1);
      expect(() => renderer.processPlanar([over, over])).toThrow(RangeError);
    } finally {
      renderer.destroy();
    }
  });

  it('reports the given block size', () => {
    const config = { target: { kind: 'speakers', layout: 'stereo' } } as const;
    const renderer = new PlaybackRenderer({ sampleRate: 48000, config, maxBlockSize: 256 });
    try {
      expect(renderer.maxBlockSize).toBe(256);
    } finally {
      renderer.destroy();
    }
  });

  it('defaults a headphones target without an explicit HrtfSet (native built-in)', () => {
    using renderer = new PlaybackRenderer({ sampleRate: 48000, config: {} });
    expect(renderer.numOutputChannels).toBe(2);
    renderer.destroy();
  });

  it('refuses an earlier key, an earlier role token or an unknown key at any depth as InvalidFormat', () => {
    const earlier = [
      { night_mode: { amount: 0.5 } },
      { nope: true },
      { target: { kind: 'speakers', layout: 'stereo', bass_management: { enabled: false } } },
      { target: { kind: 'speakers', layout: 'stereo', speakers: { L: { trimDb: 0 } } } },
    ];
    for (const config of earlier) {
      expect(() => new PlaybackRenderer({ sampleRate: 48000, config: config as never })).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidFormat }),
      );
    }
  });

  it('rejects input.channelMap combined with input.layout "auto"', () => {
    expect(
      () =>
        new PlaybackRenderer({
          sampleRate: 48000,
          config: { input: { layout: 'auto', channelMap: ['left', 'right'] } },
        }),
    ).toThrow();
  });

  it('rejects a speakers target with no target.layout', () => {
    expect(
      () => new PlaybackRenderer({ sampleRate: 48000, config: { target: { kind: 'speakers' } } }),
    ).toThrow();
  });
});

describe('PlaybackRenderer processing', () => {
  it('takes a room preset as a lowerCamel token and refuses the snake_case spelling', () => {
    const render = (preset: string): number[] => {
      const renderer = new PlaybackRenderer({
        sampleRate: 48000,
        config: { room: { preset } } as PlaybackRendererConfig,
      });
      try {
        let state = 1;
        const out: number[] = [];
        for (let block = 0; block < 4; block++) {
          const planes = [new Float32Array(1024), new Float32Array(1024)];
          for (const plane of planes) {
            for (let i = 0; i < plane.length; i++) {
              state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
              plane[i] = 0.1 * (state / 2 ** 31 - 1);
            }
          }
          out.push(...renderer.processPlanar(planes)[0]);
        }
        return out;
      } finally {
        renderer.destroy();
      }
    };
    const renders = ['none', 'livingRoom', 'homeTheater', 'screeningRoom'].map(render);
    for (let a = 0; a < renders.length; a++) {
      for (let b = a + 1; b < renders.length; b++) {
        expect(renders[a]).not.toEqual(renders[b]);
      }
    }
    // The earlier snake_case spelling is a document content defect.
    expect(() => render('living_room')).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidFormat }),
    );
  });

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
    expect(renderer.numInputChannels).toBe(2);
    expect(renderer.numOutputChannels).toBe(2);
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
    expect(renderer.numOutputChannels).toBe(6);
    expect(out).toHaveLength(128 * 6);
    expect(Array.from(out).every((s) => Number.isFinite(s))).toBe(true);
  });

  it('rejects a non-number inChannels argument to processInterleaved and stays usable', () => {
    using renderer = new PlaybackRenderer({
      sampleRate: 48000,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    expect(() =>
      renderer.processInterleaved(new Float32Array(4), 'two' as unknown as number),
    ).toThrow();
    // The rejected call must not have advanced state or left the addon unusable.
    expect(renderer.numOutputChannels).toBe(2);
  });

  it('rejects a planar call whose planes have mismatched lengths', () => {
    using renderer = new PlaybackRenderer({
      sampleRate: 48000,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const l = new Float32Array(64);
    const r = new Float32Array(32);
    expect(() => renderer.processPlanar([l, r])).toThrow();
  });

  it('rejects an unsupported input channel count under input.layout "auto"', () => {
    using renderer = new PlaybackRenderer({
      sampleRate: 48000,
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
    expect(speakers.latencySamples).toBe(288);

    using surround = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: '5.1' } },
      sampleRate: 48000,
    });
    expect(surround.latencySamples).toBe(1312);

    using headphones = new PlaybackRenderer({ config: {}, sampleRate: 48000 });
    expect(headphones.latencySamples).toBe(1312);
  });

  it('round-trips a realtime key through setConfig/config and rejects a changed prepare key', () => {
    using renderer = new PlaybackRenderer({
      sampleRate: 48000,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const config = renderer.config();
    expect(config.nightMode?.amount ?? 0).toBe(0);

    renderer.setConfig({ ...config, nightMode: { amount: 0.5 } });
    expect(renderer.config().nightMode?.amount).toBeCloseTo(0.5, 5);

    expect(() =>
      renderer.setConfig({ ...config, target: { kind: 'speakers', layout: '5.1' } }),
    ).toThrow();
  });

  it('accepts setHeadOrientation and reset without throwing on both target kinds', () => {
    using headphones = new PlaybackRenderer({ sampleRate: 48000, config: {} });
    expect(() => headphones.setHeadOrientation(30, 0, 0)).not.toThrow();
    expect(() => headphones.reset()).not.toThrow();

    using speakers = new PlaybackRenderer({
      sampleRate: 48000,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    // Ignored by a speakers target, but the call itself must not throw.
    expect(() => speakers.setHeadOrientation(30)).not.toThrow();
  });

  it('rejects wrong orientation types and stays usable', () => {
    using renderer = new PlaybackRenderer({ sampleRate: 48000, config: {} });
    const expectWrongType = (axis: string, invoke: () => void) => {
      expect(invoke).toThrow(TypeError);
      expect(invoke).toThrow(new RegExp(axis));
    };
    for (const value of [null, 'wrong', true, {}] as const) {
      expectWrongType('yawDeg', () =>
        renderer.setHeadOrientation(value as unknown as number, 0, 0),
      );
      expectWrongType('pitchDeg', () =>
        renderer.setHeadOrientation(0, value as unknown as number, 0),
      );
      expectWrongType('rollDeg', () =>
        renderer.setHeadOrientation(0, 0, value as unknown as number),
      );
    }
    expect(() => renderer.setHeadOrientation(0)).not.toThrow();
    expect(() => renderer.setHeadOrientation(0, undefined, undefined)).not.toThrow();
  });

  it('rejects non-finite and float32-overflow orientation values in yaw-pitch-roll order', () => {
    using renderer = new PlaybackRenderer({ sampleRate: 48000, config: {} });
    const float32Max = 3.4028234663852886e38;
    const float32Overflow = float32Max * 2;

    const invalidValues = [
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      float32Overflow,
      -float32Overflow,
    ];
    for (const value of invalidValues) {
      expect(() => renderer.setHeadOrientation(value, 0, 0)).toThrow(RangeError);
      expect(() => renderer.setHeadOrientation(0, value, 0)).toThrow(RangeError);
      expect(() => renderer.setHeadOrientation(0, 0, value)).toThrow(RangeError);
      expect(() => renderer.setHeadOrientation(value, 0, 0)).toThrow(/yawDeg/);
      expect(() => renderer.setHeadOrientation(0, value, 0)).toThrow(/pitchDeg/);
      expect(() => renderer.setHeadOrientation(0, 0, value)).toThrow(/rollDeg/);
    }

    // Exact float32 limits, signed zero, and multiple turns remain valid and
    // are passed through without normalization.
    for (const value of [float32Max, -float32Max, 0, -0, 720, -1080, 360]) {
      expect(() => renderer.setHeadOrientation(value, 0, 0)).not.toThrow();
      expect(() => renderer.setHeadOrientation(0, value, 0)).not.toThrow();
      expect(() => renderer.setHeadOrientation(0, 0, value)).not.toThrow();
    }

    // All three values are invalid, so the leftmost axis owns the error.
    expect(() =>
      renderer.setHeadOrientation(
        Number.NaN,
        Number.POSITIVE_INFINITY,
        'roll' as unknown as number,
      ),
    ).toThrow(/yawDeg/);
    expect(() => renderer.setHeadOrientation(0, Number.NaN, Number.POSITIVE_INFINITY)).toThrow(
      /pitchDeg/,
    );
  });

  it('reports a diagnostics document with the documented shape', () => {
    using renderer = new PlaybackRenderer({
      sampleRate: 48000,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    const diagnostics = renderer.diagnostics();
    expect(diagnostics.activeInputLayout).toBe('stereo');
    expect(diagnostics.layoutSwitches).toBe(0);
    expect(diagnostics.truncatedDrains).toBe(0);
    expect(Array.isArray(diagnostics.inactiveStages)).toBe(true);
    expect(diagnostics.inactiveStages).toContain('roomEarly');
    expect(typeof diagnostics.latency.samples).toBe('number');
    expect(typeof diagnostics.loudnessGainDb).toBe('number');
    expect(typeof diagnostics.hrtfIgnored).toBe('boolean');
    expect(renderer.nonFiniteDiscardCount).toBe(0);
  });

  it('rejects use after destroy', () => {
    const renderer = new PlaybackRenderer({
      sampleRate: 48000,
      config: { target: { kind: 'speakers', layout: 'stereo' } },
    });
    renderer.destroy();
    expect(() => renderer.destroy()).not.toThrow();
    expect(() => renderer.reset()).toThrow('destroyed');
    expect(() =>
      renderer.setHeadOrientation(Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY),
    ).toThrow('destroyed');
  });
});

describe('PlaybackLoudnessMeter', () => {
  it('measures a finite integrated LUFS after feeding interleaved stereo audio', () => {
    using meter = new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 });
    const [l, r] = stereoBlock(48000);
    const interleaved = new Float32Array(l.length * 2);
    for (let i = 0; i < l.length; i++) {
      interleaved[i * 2] = l[i];
      interleaved[i * 2 + 1] = r[i];
    }
    meter.pushInterleaved(interleaved);
    expect(Number.isFinite(meter.integratedLufs)).toBe(true);
  });

  it('reads only the frames the array holds, not its element count as frames', () => {
    // A view over a larger, louder buffer: counting samples as frames reads twice
    // the view for stereo and measures the loud tail too.
    const [l, r] = stereoBlock(48000);
    const backing = new Float32Array(l.length * 4).fill(0.9);
    const view = backing.subarray(0, l.length * 2);
    for (let i = 0; i < l.length; i++) {
      view[i * 2] = l[i];
      view[i * 2 + 1] = r[i];
    }
    using fromView = new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 });
    fromView.pushInterleaved(view);
    using fromCopy = new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 });
    fromCopy.pushInterleaved(view.slice());
    expect(fromView.integratedLufs).toBeCloseTo(fromCopy.integratedLufs, 6);
  });

  it('refuses an interleaved array that is not a whole number of frames', () => {
    using meter = new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 });
    expect(() => meter.pushInterleaved(new Float32Array(3))).toThrow(RangeError);
  });

  it('rejects a non-number numChannels or sampleRate argument', () => {
    expect(
      () =>
        new PlaybackLoudnessMeter({ numChannels: 'two' as unknown as number, sampleRate: 48000 }),
    ).toThrow();
    expect(
      () => new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 'fast' as unknown as number }),
    ).toThrow();
    // The addon must still be usable after the rejected construction.
    using meter = new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 });
    expect(Number.isFinite(meter.integratedLufs)).toBe(true);
  });
});

describe('renderPlayback', () => {
  it('refuses a serialized JSON string as the configuration', () => {
    expect(() =>
      renderPlayback({
        samples: new Float32Array(8),
        numChannels: 2,
        sampleRate: 48000,
        config: '{}' as never,
      }),
    ).toThrow(new TypeError('renderPlayback: config must be a configuration object'));
  });

  it('scans the samples for NaN and refuses a validate key', () => {
    const samples = new Float32Array(8);
    samples[3] = Number.NaN;
    const request = {
      samples,
      numChannels: 2,
      sampleRate: 48000,
      config: { target: { kind: 'speakers' as const, layout: 'stereo' as const } },
    };
    expect(() => renderPlayback(request)).toThrow(
      new RangeError('renderPlayback: samples contains a non-finite sample at index 3'),
    );
    expect(() => renderPlayback({ ...request, validate: false } as never)).toThrow(
      new TypeError("renderPlayback: unknown request key 'validate'"),
    );
  });

  it('renders a whole stereo buffer offline to a headphones target', () => {
    const [l, r] = stereoBlock(4096);
    const interleaved = new Float32Array(l.length * 2);
    for (let i = 0; i < l.length; i++) {
      interleaved[i * 2] = l[i];
      interleaved[i * 2 + 1] = r[i];
    }
    const result = renderPlayback({
      samples: interleaved,
      numChannels: 2,
      sampleRate: 48000,
      config: {},
    });
    expect(result.numChannels).toBe(2);
    expect(result.samples).toHaveLength(l.length * result.numChannels);
    expect(Array.from(result.samples).every((s) => Number.isFinite(s))).toBe(true);
  });

  it('rejects a non-number numChannels or sampleRate argument and stays usable', () => {
    const samples = new Float32Array(8);
    expect(() =>
      renderPlayback({
        samples,
        numChannels: 'two' as unknown as number,
        sampleRate: 48000,
        config: {},
      }),
    ).toThrow();
    expect(() =>
      renderPlayback({
        samples,
        numChannels: 2,
        sampleRate: 'fast' as unknown as number,
        config: {},
      }),
    ).toThrow();
    const result = renderPlayback({ samples, numChannels: 2, sampleRate: 48000, config: {} });
    expect(result.numChannels).toBe(2);
  });
});

describe('PlaybackRenderer.renderOffline', () => {
  const unequalSpeakers = {
    target: {
      kind: 'speakers' as const,
      layout: 'stereo' as const,
      speakers: { left: { distanceM: 1 }, right: { distanceM: 5 } },
    },
    outputLimiter: { enabled: false },
  };

  function interleavedStereo(frames: number): Float32Array {
    const [l, r] = stereoBlock(frames);
    const interleaved = new Float32Array(frames * 2);
    for (let i = 0; i < frames; i++) {
      interleaved[i * 2] = l[i];
      interleaved[i * 2 + 1] = r[i] * 0.5;
    }
    return interleaved;
  }

  it('matches renderPlayback after the handle has processed a block', () => {
    const samples = interleavedStereo(4096);
    const expected = renderPlayback({
      samples,
      numChannels: 2,
      sampleRate: 48000,
      config: unequalSpeakers,
    });
    using renderer = new PlaybackRenderer({ config: unequalSpeakers, sampleRate: 48000 });
    renderer.processInterleaved(interleavedStereo(256), 2);
    const out = renderer.renderOffline(samples, 2);
    expect(out).toHaveLength(expected.samples.length);
    expect(Array.from(out)).toEqual(Array.from(expected.samples));
  });

  it('refuses a mismatched channel count and non-finite input as a RangeError', () => {
    using renderer = new PlaybackRenderer({ config: unequalSpeakers, sampleRate: 48000 });
    let caught: unknown;
    try {
      renderer.renderOffline(new Float32Array(12), 3);
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
    expect((caught as Error).message).toMatch(
      /^PlaybackRenderer\.renderOffline: in_channels 3 does not match the input layout/,
    );

    const bad = interleavedStereo(64);
    bad[10] = Number.NaN;
    caught = undefined;
    try {
      renderer.renderOffline(bad, 2);
    } catch (error) {
      caught = error;
    }
    // Non-finite samples are an argument refusal, not a library failure.
    expect(caught).toBeInstanceOf(RangeError);
  });

  it('reports a destroyed handle as InvalidState', () => {
    const renderer = new PlaybackRenderer({ config: unequalSpeakers, sampleRate: 48000 });
    renderer.destroy();
    let caught: unknown;
    try {
      renderer.renderOffline(interleavedStereo(64), 2);
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as { code: number }).code).toBe(ErrorCode.InvalidState);
  });
});

describe('playback scalar reads', () => {
  it('reads cheap scalar state as getters', () => {
    using renderer = new PlaybackRenderer({ sampleRate: 48000, config: {} });
    using meter = new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 });
    const getter = (target: object, name: string) =>
      Object.getOwnPropertyDescriptor(Object.getPrototypeOf(target), name)?.get;
    const reads = renderer as unknown as Record<string, unknown>;
    for (const name of [
      'latencySamples',
      'numInputChannels',
      'numOutputChannels',
      'nonFiniteDiscardCount',
    ]) {
      expect(typeof getter(renderer, name), name).toBe('function');
      expect(typeof reads[name], name).toBe('number');
    }
    expect(typeof getter(meter, 'integratedLufs')).toBe('function');
    expect(typeof meter.integratedLufs).toBe('number');
  });
});

describe('playback constructors take one options object', () => {
  it('refuse an undeclared key by name', () => {
    expect(
      () => new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000, bogus: 1 } as never),
    ).toThrow(new TypeError("PlaybackLoudnessMeter: unknown request key 'bogus'"));
    expect(
      () => new PlaybackRenderer({ sampleRate: 48000, config: {}, bogus: 1 } as never),
    ).toThrow(new TypeError("PlaybackRenderer: unknown request key 'bogus'"));
  });

  it('refuse an undeclared key inside an object config as InvalidFormat, and a JSON string config', () => {
    expect(
      () => new PlaybackRenderer({ sampleRate: 48000, config: { bogus: 1 } as never }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.InvalidFormat }));
    expect(
      () => new PlaybackRenderer({ sampleRate: 48000, config: '{"bogus":1}' as never }),
    ).toThrow(new TypeError('PlaybackRenderer: config must be a configuration object'));
    const renderer = new PlaybackRenderer({ sampleRate: 48000, config: {} });
    try {
      expect(() => renderer.setConfig('{}' as never)).toThrow(
        new TypeError('PlaybackRenderer.setConfig: config must be a configuration object'),
      );
    } finally {
      renderer.destroy();
    }
  });

  it('refuse a positional call', () => {
    const positional = PlaybackLoudnessMeter as unknown as new (
      numChannels: number,
      sampleRate: number,
    ) => PlaybackLoudnessMeter;
    expect(() => new positional(2, 48000)).toThrow(TypeError);
  });
});
