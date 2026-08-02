/**
 * WASM input-validation guards that mirror the C ABI. The C-ABI TU is not
 * linked into the WASM build, so these wrappers must re-implement the guards
 * the C ABI performs — otherwise WASM would silently accept invalid input that
 * every other surface rejects.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  deemphasis,
  detectBeats,
  detectBpm,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  fixLength,
  frameSignal,
  init,
  Mixer,
  masteringChain,
  meteringDetectClipping,
  mixStereo,
  padCenter,
  pitchCorrectTimevarying,
  preemphasis,
  RealtimeEngine,
  RealtimeVoiceChanger,
  splitSilence,
  trimSilence,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('RealtimeEngine prepare/time-signature/loop guards', () => {
  it('rejects invalid sample rates or block sizes in the constructor', () => {
    expect(() => new RealtimeEngine(0, 128)).toThrow();
    expect(() => new RealtimeEngine(Number.NaN, 128)).toThrow();
    expect(() => new RealtimeEngine(7999, 128)).toThrow();
    expect(() => new RealtimeEngine(384001, 128)).toThrow();
    expect(() => new RealtimeEngine(48000, 0)).toThrow();
    expect(() => new RealtimeEngine(48000, 128, 1024, 1024, 0)).toThrow();
    expect(() => new RealtimeEngine(48000, 128, 1024, 1024, 65)).toThrow();
  });

  it('rejects invalid sample rates or block sizes in prepare()', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.prepare(0, 0)).toThrow();
    expect(() => engine.prepare(Number.NaN, 128)).toThrow();
    expect(() => engine.prepare(7999, 128)).toThrow();
    expect(() => engine.prepare(384001, 128)).toThrow();
    expect(() => engine.prepare(48000, 0)).toThrow();
    expect(() => engine.prepare(48000, 128, 1024, 1024, 0)).toThrow();
    expect(() => engine.prepare(48000, 128, 1024, 1024, 65)).toThrow();
    expect(() => engine.prepare(48000, 128, 1024, 1024, 2)).not.toThrow();
  });

  it('rejects a non-positive time signature', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setTimeSignature(0, 4)).toThrow();
    expect(() => engine.setTimeSignature(4, 0)).toThrow();
    // A valid signature does not throw.
    expect(() => engine.setTimeSignature(3, 4)).not.toThrow();
  });

  it('rejects tempos above the realtime-safe public range', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setTempo(100000)).not.toThrow();
    expect(() => engine.setTempo(100000.1)).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 100000.1 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: 100000.1 }])).toThrow();
  });

  it('rejects an invalid loop range', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setLoop(-1, 4, true)).toThrow();
    expect(() => engine.setLoop(4, 4, true)).toThrow(); // empty
    expect(() => engine.setLoop(8, 4, true)).toThrow(); // inverted
    expect(() => engine.setLoop(0, 4, true)).not.toThrow();
  });
});

describe('WASM mixStereo input guards', () => {
  it('rejects an empty channel list with a specific validation error', () => {
    expect(() => mixStereo([], [], 48000)).toThrow(/non-zero length/);
  });
});

describe('RealtimeVoiceChanger legacy block-size guard', () => {
  it('rejects a block larger than the prepared max block size', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    vc.prepare(48000, 128, 1);
    // Within the prepared max: fine.
    expect(() => vc.processMono(new Float32Array(128))).not.toThrow();
    // Larger than the prepared max: must throw, not emit stale/garbage scratch.
    expect(() => vc.processMono(new Float32Array(256))).toThrow();
  });
});

describe('RealtimeEngine clip/parameter/metronome guards mirror the C ABI', () => {
  const validClip = () => ({
    id: 1,
    channels: [new Float32Array(128).fill(0.1)],
    startPpq: 0,
    lengthSamples: 128,
  });

  it('rejects a non-finite or negative clip gain', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ ...validClip(), gain: Number.NaN }])).toThrow();
    expect(() => engine.setClips([{ ...validClip(), gain: -1 }])).toThrow();
    // A valid non-negative gain is accepted.
    expect(() => engine.setClips([{ ...validClip(), gain: 0.5 }])).not.toThrow();
  });

  it('rejects negative clip fade lengths', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ ...validClip(), fadeInSamples: -1 }])).toThrow();
    expect(() => engine.setClips([{ ...validClip(), fadeOutSamples: -1 }])).toThrow();
  });

  it('treats zero clip length as the auto-derived source length', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ ...validClip(), lengthSamples: 0 }])).not.toThrow();
  });

  it('rejects a non-finite setParameter / setParameterSmoothed value', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setParameter(7, Number.NaN, 0)).toThrow();
    expect(() => engine.setParameter(7, Number.POSITIVE_INFINITY, 0)).toThrow();
    expect(() => engine.setParameterSmoothed(7, Number.NaN, 0)).toThrow();
  });

  it('rejects an inverted addParameter range', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() =>
      engine.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: 1,
        maxValue: 0,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 1,
      }),
    ).toThrow();
  });

  it('rejects invalid metronome gains or click length', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setMetronome({ enabled: true, beatGain: -1 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSamples: -1 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSamples: 2_000_000_000 })).toThrow();
    expect(() =>
      engine.setMetronome({ enabled: true, clickSamples: 0, clickSeconds: 2 }),
    ).toThrow();
    expect(() =>
      engine.setMetronome({ enabled: true, clickSamples: 0, clickSeconds: Number.NaN }),
    ).toThrow();
    expect(() => engine.setMetronome({ enabled: true, beatGain: 0.3 })).not.toThrow();
  });

  it('rejects a non-finite MIDI clip startPpq', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);
    expect(() =>
      engine.setMidiClips([
        {
          id: 1,
          trackId: 5,
          destinationId: 5,
          lengthSamples: 8192,
          startPpq: Number.NaN,
          events: [],
        },
      ]),
    ).toThrow();
  });

  it('rejects null array-like realtime inputs with a binding error', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);

    // These C++ entry points used to read `.length` before checking for null,
    // letting an unwrapped JavaScript TypeError escape the binding boundary.
    expect(() => engine.setClips(null as never)).toThrow();
    expect(() => engine.process(null as never)).toThrow();
    expect(() => engine.setMarkers(null as never)).toThrow();
    expect(() => engine.setMidiClips(null as never)).toThrow();
  });

  it('rejects a non-positive renderOffline block size', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.renderOffline([new Float32Array(128)], 0)).toThrow();
    expect(() => engine.renderOffline([new Float32Array(128)], -1)).toThrow();
  });
});

describe('StreamingRetune sanitizes non-finite input', () => {
  it('requires prepare and rejects blocks above the prepared maximum', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({ semitones: 2 });
    try {
      expect(() => retune.processMono(new Float32Array(64))).toThrow(/prepared/);
      retune.prepare(48000, 64);
      expect(() => retune.processMono(new Float32Array(65))).toThrow(/maxBlockSize/);
      expect(retune.processMono(new Float32Array(64))).toHaveLength(64);
    } finally {
      retune.delete();
    }
  });

  it('does not propagate NaN into the grain history', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({ semitones: 2 });
    retune.prepare(48000, 256);
    const bad = new Float32Array(256).fill(0.1);
    bad[10] = Number.NaN;
    bad[20] = Number.POSITIVE_INFINITY;
    const first = retune.processMono(bad);
    expect(Array.from(first).every((v) => Number.isFinite(v))).toBe(true);
    // A subsequent clean block must also stay finite (no poisoned ring state).
    const clean = retune.processMono(new Float32Array(256).fill(0.1));
    expect(Array.from(clean).every((v) => Number.isFinite(v))).toBe(true);
  });

  it('sanitizes non-finite and out-of-range configuration before it reaches DSP', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({
      semitones: Number.NaN,
      mix: Number.POSITIVE_INFINITY,
      grainSize: 1_000_000,
    });
    try {
      retune.prepare(48000, 128);
      const config = retune.config();
      expect(config.semitones).toBe(0);
      expect(config.mix).toBe(1);
      expect(retune.grainSize()).toBeLessThanOrEqual(8192);
      expect(
        Array.from(retune.processMono(new Float32Array(128).fill(0.1))).every(Number.isFinite),
      ).toBe(true);
    } finally {
      retune.delete();
    }
  });
});

describe('offline boundary validation is enforced by the native WASM layer', () => {
  it('rejects NaN/Inf in every quick-analysis entry even when the JS scan is disabled', () => {
    const bad = new Float32Array(2048);
    bad[17] = Number.NaN;
    const calls = [
      () => detectBpm(bad, 22050, { validate: false }),
      () => detectKey({ samples: bad, sampleRate: 22050, validate: false }),
      () => detectKeyCandidates({ samples: bad, sampleRate: 22050, validate: false }),
      () => detectOnsets(bad, 22050, { validate: false }),
      () => detectBeats(bad, 22050, { validate: false }),
      () => detectDownbeats(bad, 22050, { validate: false }),
      () => analyze(bad, 22050, { validate: false }),
    ];
    for (const call of calls) {
      expect(call).toThrow();
    }
  });

  it('rejects non-finite values in all signal transforms', () => {
    const bad = new Float32Array([0, Number.POSITIVE_INFINITY, 1]);
    const calls = [
      () => preemphasis(bad),
      () => deemphasis(bad),
      () => trimSilence(bad, 20, 2, 1),
      () => splitSilence(bad, 20, 2, 1),
      () => frameSignal(bad, 2, 1),
      () => padCenter(bad, 4),
      () => fixLength(bad, 4),
    ];
    for (const call of calls) {
      expect(call).toThrow();
    }
  });

  it('rejects negative target sizes without attempting an allocation', () => {
    const values = new Float32Array([1, 2]);
    expect(() => padCenter(values, -1)).toThrow();
    expect(() => fixLength(values, -1)).toThrow();
  });

  it('rejects invalid time-varying pitch tracks and configuration', () => {
    const samples = new Float32Array(2048).fill(0.1);
    expect(() => pitchCorrectTimevarying(samples, new Float32Array(), 22050, 256)).toThrow();
    expect(() => pitchCorrectTimevarying(samples, new Float32Array([440]), 22050, 0)).toThrow();
    expect(() =>
      pitchCorrectTimevarying(samples, new Float32Array([440]), 22050, 256, {
        maxCorrectionSemitones: -1,
      }),
    ).toThrow();
    expect(() =>
      pitchCorrectTimevarying(samples, new Float32Array([440]), 22050, 256, {
        retuneSpeedMs: Number.NaN,
      }),
    ).toThrow();
    expect(() =>
      pitchCorrectTimevarying(samples, new Float32Array([440]), 22050, 256, {
        scaleModeMask: 0,
      }),
    ).toThrow();
  });

  it('rejects negative clipping-region lengths and mastering oversampling', () => {
    const samples = new Float32Array(2048).fill(0.1);
    expect(() =>
      meteringDetectClipping({ samples, sampleRate: 22050, minRegionSamples: -1 }),
    ).toThrow();
    expect(() =>
      masteringChain(samples, 22050, {
        loudness: { enabled: false, truePeakOversample: 3 },
      }),
    ).toThrow();
  });

  it('accepts a zero-strip processStereo call and returns an empty master', () => {
    const mixer = Mixer.fromSceneJson(
      '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
      48000,
      128,
    );
    const result = mixer.processStereo([], []);
    expect(result.left).toHaveLength(0);
    expect(result.right).toHaveLength(0);
  });
});
