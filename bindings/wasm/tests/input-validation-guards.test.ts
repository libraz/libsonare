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
  ErrorCode,
  fixLength,
  frameSignal,
  init,
  isSonareError,
  Mixer,
  masteringChain,
  meteringDetectClipping,
  mixStereo,
  padCenter,
  pitchCorrectTimevarying,
  preemphasis,
  RealtimeEngine,
  RealtimeVoiceChanger,
  StreamAnalyzer,
  splitSilence,
  trimSilence,
} from '../dist/index.js';
import { SonareEngineTelemetryError } from '../dist/worklet.js';

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

  it('forwards prepare() maxChannels and reports an over-limit process block', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.prepare(48000, 128, 1024, 1024, 2);
      expect(() =>
        engine.process([new Float32Array(128), new Float32Array(128), new Float32Array(128)]),
      ).not.toThrow();

      expect(engine.drainTelemetry()).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            error: SonareEngineTelemetryError.MaxChannelsExceeded,
            value: 3,
          }),
        ]),
      );
    } finally {
      engine.destroy();
    }
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

describe('RealtimeEngine track monitor mode guard', () => {
  it('rejects non-canonical mode values before embind', () => {
    const engine = new RealtimeEngine(48000, 128);
    for (const mode of [
      true,
      false,
      0.5,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      -1,
      3,
      'PFL',
      'post-fader',
    ]) {
      expect(() => engine.setTrackMonitorMode(0, mode as never)).toThrow(RangeError);
    }
    engine.destroy();
  });

  it('accepts only the documented strings and ordinals', () => {
    const engine = new RealtimeEngine(48000, 128);
    for (const mode of ['off', 'pfl', 'afl', 0, 1, 2] as const) {
      expect(() => engine.setTrackMonitorMode(0, mode)).not.toThrow();
    }
    engine.destroy();
  });
});

describe('WASM mixStereo input guards', () => {
  it('rejects an empty channel list with a specific validation error', () => {
    expect(() => mixStereo([], [], 48000)).toThrow(/non-zero length/);
  });
});

describe('detectKey / detectKeyCandidates reject unknown enum spellings and out-of-range ordinals', () => {
  // Input-validation consolidation (P2-2): keyProfileValue/keyModeValues used
  // to resolve an unmapped spelling to `undefined` (silently reaching the
  // WASM boundary as a mistyped argument, e.g. a typo like "edm" silently
  // selecting a different profile, and an unknown mode name silently
  // becoming Major) and passed a raw numeric ordinal through unvalidated.
  // Both now reject through resolveEnumOrdinal, the same primitive
  // panModeCode already used (H-18).
  const samples = new Float32Array(4096).fill(0.05);

  it("rejects a typo'd key profile name instead of silently selecting a different profile", () => {
    expect(() => detectKey(samples, 22050, { profile: 'edm' as never })).toThrow(RangeError);
  });

  it('rejects an out-of-range numeric key profile ordinal', () => {
    expect(() => detectKey(samples, 22050, { profile: 99 as never })).toThrow(RangeError);
  });

  it("rejects a typo'd key mode name instead of silently becoming Major", () => {
    expect(() => detectKey(samples, 22050, { modes: ['typo' as never] })).toThrow(RangeError);
  });

  it('rejects an out-of-range numeric key mode ordinal', () => {
    expect(() => detectKeyCandidates(samples, 22050, { modes: [99 as never] })).toThrow(RangeError);
  });

  it('accepts every documented key profile and key mode spelling', () => {
    for (const profile of [
      'ks',
      'krumhansl',
      'temperley',
      'shaath',
      'keyfinder',
      'faraldo-edmt',
      'edmt',
      'faraldo-edma',
      'edma',
      'faraldo-edmm',
      'edmm',
      'bellman-budge',
      'bellman',
    ] as const) {
      expect(() => detectKey(samples, 22050, { profile })).not.toThrow();
    }
    for (const mode of [
      'major',
      'minor',
      'dorian',
      'phrygian',
      'lydian',
      'mixolydian',
      'locrian',
    ] as const) {
      expect(() => detectKey(samples, 22050, { modes: [mode] })).not.toThrow();
    }
  });
});

describe('StreamAnalyzer rejects an out-of-range window ordinal instead of silently defaulting to Hann', () => {
  it('rejects non-safe-integer window values before the Embind constructor', () => {
    for (const window of [
      -1,
      4,
      99,
      0.5,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      '0',
      null,
      true,
    ]) {
      expect(() => new StreamAnalyzer({ window: window as never })).toThrow(RangeError);
    }
  });

  it('accepts every valid window ordinal', () => {
    for (const window of [0, 1, 2, 3] as const) {
      const analyzer = new StreamAnalyzer({ window });
      analyzer.delete();
    }
  });
});

describe('RealtimeVoiceChanger.setPodConfig rejects a partial POD instead of zero-filling it', () => {
  // Priority fix: a partial POD used to silently zero-fill missing fields —
  // a missing limiterEnableIspLimiter read as JS `false`, which can turn the
  // ISP limiter off and let the DAC clip. Every field is now required.
  function fullPod() {
    return {
      inputGainDb: 0,
      outputGainDb: 0,
      wetMix: 1,
      retuneSemitones: 0,
      retuneMix: 0,
      retuneGrainSize: 0,
      formantFactor: 1,
      formantAmount: 0,
      formantBody: 0,
      formantBrightness: 0,
      formantNasal: 0,
      eqHighpassHz: 80,
      eqBodyDb: 0,
      eqPresenceDb: 0,
      eqAirDb: 0,
      gateThresholdDb: -55,
      gateAttackMs: 2,
      gateReleaseMs: 100,
      gateRangeDb: 18,
      compressorThresholdDb: -22,
      compressorRatio: 2,
      compressorAttackMs: 6,
      compressorReleaseMs: 90,
      compressorMakeupGainDb: 0,
      deesserFrequencyHz: 7200,
      deesserThresholdDb: -28,
      deesserRatio: 4,
      deesserRangeDb: 8,
      reverbMix: 0,
      reverbTimeMs: 100,
      reverbDamping: 0.5,
      reverbSeed: 1,
      limiterCeilingDb: -1,
      limiterReleaseMs: 50,
      limiterEnableIspLimiter: true,
      limiterIspCeilingDbtp: -1,
    };
  }

  it('rejects a POD missing limiterEnableIspLimiter instead of silently turning the ISP limiter off', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    const { limiterEnableIspLimiter: _omitted, ...partial } = fullPod();
    expect(() => vc.setPodConfig(partial as never)).toThrow(/limiterEnableIspLimiter/);
  });

  it('rejects a POD missing any other required numeric field', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    const { wetMix: _omitted, ...partial } = fullPod();
    expect(() => vc.setPodConfig(partial as never)).toThrow(/wetMix/);
  });

  it('rejects a POD with a wrong-typed field instead of coercing it', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    expect(() => vc.setPodConfig({ ...fullPod(), wetMix: 'loud' as unknown as number })).toThrow(
      /wetMix/,
    );
  });

  it('accepts a fully specified POD', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    expect(() => vc.setPodConfig(fullPod())).not.toThrow();
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

describe('RealtimeEngine.bindMidiCcBinding defaults omitted descriptor fields', () => {
  // `ccNumber` and `paramId` are the only required fields; the rest fall back
  // to the CcBinding defaults (channel = any, kind = 7-bit CC, range 0..1),
  // matching the Node addon reader and the Project-side descriptor reader.
  // There is no engine-side getter for a bound descriptor, so the defaults are
  // pinned through the range guard instead: it rejects `maxValue < minValue`,
  // which brackets each default from both sides.
  //
  // The public descriptor type marks the defaulted fields as required, so the
  // partial literals below are cast; what is under test is the runtime reader.
  type CcBindingDescriptor = Parameters<RealtimeEngine['bindMidiCcBinding']>[0];

  const expectInvalidParameter = (action: () => void) => {
    let caught: unknown;
    try {
      action();
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    if (isSonareError(caught)) {
      expect(caught.code).toBe(ErrorCode.InvalidParameter);
    }
  };

  it('accepts a descriptor carrying only ccNumber and paramId', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() =>
      engine.bindMidiCcBinding({ ccNumber: 7, paramId: 1 } as CcBindingDescriptor),
    ).not.toThrow();
    expect(engine.midiCcBindingCount()).toBe(1);
  });

  it('defaults an omitted kind to 7-bit Control Change', () => {
    const engine = new RealtimeEngine(48000, 128);
    // A kind of 1 (14-bit CC) additionally requires ccLsbNumber === ccNumber + 32,
    // so the descriptor below is accepted only while the default kind is 0.
    expect(() =>
      engine.bindMidiCcBinding({ ccNumber: 7, paramId: 1 } as CcBindingDescriptor),
    ).not.toThrow();
    // The explicit spelling of the same default behaves identically. A distinct
    // ccNumber is used because binding an existing (ccNumber, channel) replaces
    // it rather than appending.
    expect(() =>
      engine.bindMidiCcBinding({ ccNumber: 8, kind: 0, paramId: 2 } as CcBindingDescriptor),
    ).not.toThrow();
    expect(engine.midiCcBindingCount()).toBe(2);
  });

  it('defaults an omitted minValue to 0 and an omitted maxValue to 1', () => {
    const engine = new RealtimeEngine(48000, 128);
    // maxValue defaults to exactly 1: a minValue of 1 is still a valid
    // (degenerate) range, a minValue of 2 inverts it.
    expect(() =>
      engine.bindMidiCcBinding({ ccNumber: 7, paramId: 1, minValue: 1 } as CcBindingDescriptor),
    ).not.toThrow();
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({ ccNumber: 7, paramId: 1, minValue: 2 } as CcBindingDescriptor),
    );
    // minValue defaults to exactly 0, bracketed the same way.
    expect(() =>
      engine.bindMidiCcBinding({ ccNumber: 7, paramId: 1, maxValue: 0 } as CcBindingDescriptor),
    ).not.toThrow();
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({ ccNumber: 7, paramId: 1, maxValue: -1 } as CcBindingDescriptor),
    );
  });

  it('still rejects a supplied non-finite range instead of defaulting it', () => {
    const engine = new RealtimeEngine(48000, 128);
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({
        ccNumber: 7,
        paramId: 1,
        minValue: Number.NaN,
      } as CcBindingDescriptor),
    );
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({
        ccNumber: 7,
        paramId: 1,
        maxValue: Number.POSITIVE_INFINITY,
      } as CcBindingDescriptor),
    );
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({
        ccNumber: 7,
        paramId: 1,
        minValue: Number.NEGATIVE_INFINITY,
        maxValue: 1,
      } as CcBindingDescriptor),
    );
    expect(engine.midiCcBindingCount()).toBe(0);
  });

  it('still requires paramId and range-checks a supplied kind', () => {
    const engine = new RealtimeEngine(48000, 128);
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({ ccNumber: 7 } as unknown as CcBindingDescriptor),
    );
    expectInvalidParameter(() =>
      engine.bindMidiCcBinding({
        ccNumber: 7,
        kind: 9,
        paramId: 1,
      } as unknown as CcBindingDescriptor),
    );
    expect(engine.midiCcBindingCount()).toBe(0);
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
