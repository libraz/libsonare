/**
 * WASM input-validation guards that mirror the C ABI. The C-ABI TU is not
 * linked into the WASM build, so these wrappers must re-implement the guards
 * the C ABI performs — otherwise WASM would silently accept invalid input that
 * every other surface rejects.
 */

import { readFileSync } from 'node:fs';
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
  masterAudio,
  meteringDetectClipping,
  mixStereo,
  padCenter,
  pitchCorrectTimevarying,
  preemphasis,
  RealtimeEngine,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetConfig,
  StreamAnalyzer,
  splitSilence,
  trimSilence,
} from '../dist/index.js';
import { expectRefusalOf } from './_helpers';

beforeAll(async () => {
  await init();
});

/** Asserts that @p action throws a RangeError: an argument the binding refused itself. */
function expectRangeRefusal(action: () => void): void {
  expectRefusalOf(action);
}

/** Asserts that @p action throws a TypeError: a wrong-typed argument. */
function expectTypeRefusal(action: () => void): void {
  expectRefusalOf(action, TypeError);
}

describe('RealtimeEngine prepare/time-signature/loop guards', () => {
  it('rejects invalid sample rates or block sizes in the constructor', () => {
    expect(() => new RealtimeEngine({ sampleRate: 0, maxBlockSize: 128 })).toThrow();
    expect(() => new RealtimeEngine({ sampleRate: Number.NaN, maxBlockSize: 128 })).toThrow();
    expect(() => new RealtimeEngine({ sampleRate: 7999, maxBlockSize: 128 })).toThrow();
    expect(() => new RealtimeEngine({ sampleRate: 384001, maxBlockSize: 128 })).toThrow();
    expect(() => new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 0 })).toThrow();
    expect(
      () =>
        new RealtimeEngine({
          sampleRate: 48000,
          maxBlockSize: 128,
          commandCapacity: 1024,
          telemetryCapacity: 1024,
          maxChannels: 0,
        }),
    ).toThrow();
    expect(
      () =>
        new RealtimeEngine({
          sampleRate: 48000,
          maxBlockSize: 128,
          commandCapacity: 1024,
          telemetryCapacity: 1024,
          maxChannels: 65,
        }),
    ).toThrow();
  });

  it('rejects invalid sample rates or block sizes in prepare()', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.prepare({ sampleRate: 0, maxBlockSize: 0 })).toThrow();
    expect(() => engine.prepare({ sampleRate: Number.NaN, maxBlockSize: 128 })).toThrow();
    expect(() => engine.prepare({ sampleRate: 7999, maxBlockSize: 128 })).toThrow();
    expect(() => engine.prepare({ sampleRate: 384001, maxBlockSize: 128 })).toThrow();
    expect(() => engine.prepare({ sampleRate: 48000, maxBlockSize: 0 })).toThrow();
    expect(() =>
      engine.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 1024,
        telemetryCapacity: 1024,
        maxChannels: 0,
      }),
    ).toThrow();
    expect(() =>
      engine.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 1024,
        telemetryCapacity: 1024,
        maxChannels: 65,
      }),
    ).toThrow();
    expect(() =>
      engine.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 1024,
        telemetryCapacity: 1024,
        maxChannels: 2,
      }),
    ).not.toThrow();
  });

  it('puts the channel ceiling exactly where the engine constant puts it', () => {
    // The two cases above spell 65, which keeps passing if the ceiling moves
    // down and only ever asserts "something above 64 is refused". Derive the
    // boundary from kMaxAudioChannels instead, on both the constructor and the
    // prepare() path, so lowering the constant fails here rather than leaving a
    // green test pinning a value the engine no longer uses.
    const header = readFileSync(
      new URL('../../../src/engine/realtime_engine.h', import.meta.url).pathname,
      'utf8',
    );
    const declared = header.match(/kMaxAudioChannels\s*=\s*(\d+);/)?.[1];
    // Self-check: the assertions below are vacuous if the constant moved or the
    // regex stopped matching.
    expect(declared, 'kMaxAudioChannels in src/engine/realtime_engine.h').toBeDefined();
    const maxChannels = Number(declared);
    expect(maxChannels).toBeGreaterThan(2);

    const atLimit = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 128,
      commandCapacity: 1024,
      telemetryCapacity: 1024,
      maxChannels: maxChannels,
    });
    atLimit.destroy();
    expect(
      () =>
        new RealtimeEngine({
          sampleRate: 48000,
          maxBlockSize: 128,
          commandCapacity: 1024,
          telemetryCapacity: 1024,
          maxChannels: maxChannels + 1,
        }),
    ).toThrow();

    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 128,
      commandCapacity: 1024,
      telemetryCapacity: 1024,
      maxChannels: 2,
    });
    try {
      expect(() =>
        engine.prepare({
          sampleRate: 48000,
          maxBlockSize: 128,
          commandCapacity: 1024,
          telemetryCapacity: 1024,
          maxChannels: maxChannels,
        }),
      ).not.toThrow();
      expect(() =>
        engine.prepare({
          sampleRate: 48000,
          maxBlockSize: 128,
          commandCapacity: 1024,
          telemetryCapacity: 1024,
          maxChannels: maxChannels + 1,
        }),
      ).toThrow();
      // The prepared scratch rows share the same ceiling and the same constant.
      expect(() => engine.prepareChannels(maxChannels, 128)).not.toThrow();
      expect(() => engine.prepareChannels(maxChannels + 1, 128)).toThrow();
      expect(() => engine.prepareMonitorChannels(maxChannels, 128)).not.toThrow();
      expect(() => engine.prepareMonitorChannels(maxChannels + 1, 128)).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('forwards prepare() maxChannels and reports an over-limit process block', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 1024,
        telemetryCapacity: 1024,
        maxChannels: 2,
      });
      expect(() =>
        engine.process([new Float32Array(128), new Float32Array(128), new Float32Array(128)]),
      ).not.toThrow();

      expect(engine.drainTelemetry()).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            error: 'maxChannelsExceeded',
            value: 3,
          }),
        ]),
      );
    } finally {
      engine.destroy();
    }
  });

  it('rejects a non-positive time signature', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setTimeSignature(0, 4)).toThrow();
    expect(() => engine.setTimeSignature(4, 0)).toThrow();
    // A valid signature does not throw.
    expect(() => engine.setTimeSignature(3, 4)).not.toThrow();
  });

  it('rejects tempos above the realtime-safe public range', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setTempo(100000)).not.toThrow();
    expect(() => engine.setTempo(100000.1)).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 100000.1 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: 100000.1 }])).toThrow();
  });

  it('rejects an invalid loop range', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setLoop({ startPpq: -1, endPpq: 4, enabled: true })).toThrow();
    expect(() => engine.setLoop({ startPpq: 4, endPpq: 4, enabled: true })).toThrow(); // empty
    expect(() => engine.setLoop({ startPpq: 8, endPpq: 4, enabled: true })).toThrow(); // inverted
    expect(() => engine.setLoop({ startPpq: 0, endPpq: 4, enabled: true })).not.toThrow();
  });
});

describe('RealtimeEngine track monitor mode guard', () => {
  it('rejects non-canonical mode values before embind', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    // A declared track keeps the refusals below about the mode, not an unknown track.
    engine.setTrackLanes([{ trackId: 10 }]);
    for (const mode of [true, false]) {
      expect(() => engine.setTrackMonitorMode(10, mode as never)).toThrow(TypeError);
    }
    // A number never selects a mode, whatever its value.
    for (const mode of [
      0.5,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      -1,
      3,
    ]) {
      expect(() => engine.setTrackMonitorMode(10, mode as never)).toThrow(TypeError);
    }
    for (const mode of ['PFL', 'post-fader']) {
      expect(() => engine.setTrackMonitorMode(10, mode as never)).toThrow(RangeError);
    }
    engine.destroy();
  });

  it('accepts only the documented strings', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setTrackLanes([{ trackId: 10 }]);
    for (const mode of ['off', 'pfl', 'afl'] as const) {
      expect(() => engine.setTrackMonitorMode(10, mode)).not.toThrow();
    }
    for (const mode of [0, 1, 2]) {
      expect(() => engine.setTrackMonitorMode(10, mode as never)).toThrow(TypeError);
    }
    engine.destroy();
  });
});

describe('WASM mixStereo input guards', () => {
  it('rejects an empty strip list with a RangeError', () => {
    expect(() => mixStereo({ strips: [], sampleRate: 48000 })).toThrow(RangeError);
  });
});

describe('detectKey / detectKeyCandidates reject unknown enum spellings and out-of-range ordinals', () => {
  // keyProfileValue/keyModeValues used
  // to resolve an unmapped spelling to `undefined` (silently reaching the
  // WASM boundary as a mistyped argument, e.g. a typo like "edm" silently
  // selecting a different profile, and an unknown mode name silently
  // becoming Major) and passed a raw numeric ordinal through unvalidated.
  // Both now reject through resolveEnumOrdinal, the same primitive
  // panModeCode already used.
  const samples = new Float32Array(4096).fill(0.05);

  it("rejects a typo'd key profile name instead of silently selecting a different profile", () => {
    expect(() => detectKey(samples, 22050, { profile: 'edm' as never })).toThrow(RangeError);
  });

  it('rejects a numeric key profile ordinal', () => {
    expect(() => detectKey(samples, 22050, { profile: 99 as never })).toThrow(TypeError);
    expect(() => detectKey(samples, 22050, { profile: 0 as never })).toThrow(TypeError);
  });

  it("rejects a typo'd key mode name instead of silently becoming Major", () => {
    expect(() => detectKey(samples, 22050, { modes: ['typo' as never] })).toThrow(RangeError);
  });

  it('rejects a numeric key mode ordinal', () => {
    expect(() => detectKeyCandidates(samples, 22050, { modes: [99 as never] })).toThrow(TypeError);
    expect(() => detectKeyCandidates(samples, 22050, { modes: [0 as never] })).toThrow(TypeError);
  });

  it('rejects the synonym spellings of a key profile', () => {
    for (const profile of [
      'ks',
      'krumhansl',
      'keyfinder',
      'edmt',
      'faraldo-edmt',
      'bellman-budge',
    ]) {
      expect(() => detectKey(samples, 22050, { profile: profile as never })).toThrow(RangeError);
    }
  });

  it('accepts every documented key profile and key mode spelling', () => {
    for (const profile of [
      'krumhanslSchmuckler',
      'temperley',
      'shaath',
      'faraldoEdmt',
      'faraldoEdma',
      'faraldoEdmm',
      'bellmanBudge',
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

describe('StreamAnalyzer rejects a window that is not a known name instead of silently defaulting to Hann', () => {
  it('rejects a non-string window before the Embind constructor', () => {
    for (const window of [
      -1,
      0,
      4,
      0.5,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      null,
      true,
    ]) {
      expect(() => new StreamAnalyzer({ sampleRate: 22050, window: window as never })).toThrow(
        TypeError,
      );
    }
  });

  it('rejects an unknown window name', () => {
    for (const window of ['0', 'Hann', 'triangle', '']) {
      expect(() => new StreamAnalyzer({ sampleRate: 22050, window: window as never })).toThrow(
        RangeError,
      );
    }
  });

  it('accepts every valid window name', () => {
    for (const window of ['hann', 'hamming', 'blackman', 'rectangular'] as const) {
      const analyzer = new StreamAnalyzer({ sampleRate: 22050, window });
      analyzer.destroy();
    }
  });
});

describe('RealtimeVoiceChanger.setConfig rejects a partial POD instead of zero-filling it', () => {
  // Priority fix: a partial POD used to silently zero-fill missing fields —
  // a missing limiterEnableIspLimiter read as JS `false`, which can turn the
  // ISP limiter off and let the DAC clip. Every field is now required.
  function fullPod() {
    return { ...realtimeVoiceChangerPresetConfig('neutralMonitor') };
  }

  it('rejects a POD missing limiterEnableIspLimiter instead of silently turning the ISP limiter off', () => {
    const vc = new RealtimeVoiceChanger({
      sampleRate: 48000,
      numChannels: 1,
      config: 'neutralMonitor',
    });
    const { limiterEnableIspLimiter: _omitted, ...partial } = fullPod();
    expect(() => vc.setConfig(partial as never)).toThrow(/limiterEnableIspLimiter/);
  });

  it('rejects a POD missing any other required numeric field', () => {
    const vc = new RealtimeVoiceChanger({
      sampleRate: 48000,
      numChannels: 1,
      config: 'neutralMonitor',
    });
    const { wetMix: _omitted, ...partial } = fullPod();
    expect(() => vc.setConfig(partial as never)).toThrow(/wetMix/);
  });

  it('rejects a POD with a wrong-typed field instead of coercing it', () => {
    const vc = new RealtimeVoiceChanger({
      sampleRate: 48000,
      numChannels: 1,
      config: 'neutralMonitor',
    });
    expect(() => vc.setConfig({ ...fullPod(), wetMix: 'loud' as unknown as number })).toThrow(
      /wetMix/,
    );
  });

  it('accepts a fully specified POD', () => {
    const vc = new RealtimeVoiceChanger({
      sampleRate: 48000,
      numChannels: 1,
      config: 'neutralMonitor',
    });
    expect(() => vc.setConfig(fullPod())).not.toThrow();
  });

  it('reports a refused preset-or-config document as InvalidFormat on setConfig', () => {
    const vc = new RealtimeVoiceChanger({
      sampleRate: 48000,
      numChannels: 1,
      config: 'neutralMonitor',
    });
    let caught: unknown;
    try {
      vc.setConfig({ schemaVersion: 3, dsp: { retune: { pitchSemitones: 3 } } } as never);
    } catch (error) {
      caught = error;
    }
    expect(caught).toMatchObject({ codeName: 'InvalidFormat' });
  });
});

describe('RealtimeVoiceChanger legacy block-size guard', () => {
  it('rejects a block larger than the prepared max block size', () => {
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, maxBlockSize: 128 });
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setClips([{ ...validClip(), gain: Number.NaN }])).toThrow();
    expect(() => engine.setClips([{ ...validClip(), gain: -1 }])).toThrow();
    // A valid non-negative gain is accepted.
    expect(() => engine.setClips([{ ...validClip(), gain: 0.5 }])).not.toThrow();
  });

  it('rejects negative clip fade lengths', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setClips([{ ...validClip(), fadeInSamples: -1 }])).toThrow();
    expect(() => engine.setClips([{ ...validClip(), fadeOutSamples: -1 }])).toThrow();
  });

  it('treats zero clip length as the auto-derived source length', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setClips([{ ...validClip(), lengthSamples: 0 }])).not.toThrow();
  });

  it('rejects a non-finite setParameter / setParameterSmoothed value', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setParameter(7, Number.NaN, 0)).toThrow();
    expect(() => engine.setParameter(7, Number.POSITIVE_INFINITY, 0)).toThrow();
    expect(() => engine.setParameterSmoothed(7, Number.NaN, 0)).toThrow();
  });

  it('rejects an inverted addParameter range', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() =>
      engine.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: 1,
        maxValue: 0,
        defaultValue: 0,
        rtSafe: true,
        // @ts-expect-error a numeric curve is refused; curves are named tokens
        defaultCurve: 1,
      }),
    ).toThrow();
  });

  it('rejects invalid metronome gains or click length', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setMetronome({ enabled: true, beatGain: -1 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSamples: -1 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSamples: 2_000_000_000 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSec: 2 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSec: Number.NaN })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, beatGain: 0.3 })).not.toThrow();
  });

  it('rejects a non-finite MIDI clip startPpq', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setBuiltinInstrument(5, { gain: 0.5 });
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setBuiltinInstrument(5, { gain: 0.5 });

    // These C++ entry points used to read `.length` before checking for null,
    // letting an unwrapped JavaScript TypeError escape the binding boundary.
    expect(() => engine.setClips(null as never)).toThrow();
    expect(() => engine.process(null as never)).toThrow();
    expect(() => engine.setMarkers(null as never)).toThrow();
    expect(() => engine.setMidiClips(null as never)).toThrow();
  });

  it('rejects a non-positive renderOffline block size', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() =>
      engine.renderOffline({ channels: [new Float32Array(128)], blockSize: 0 }),
    ).toThrow();
    expect(() =>
      engine.renderOffline({ channels: [new Float32Array(128)], blockSize: -1 }),
    ).toThrow();
  });
});

describe('RealtimeEngine.bindMidiCc defaults omitted descriptor fields', () => {
  // `ccNumber` and `paramId` are the only required fields; the rest fall back
  // to the CcBinding defaults (channel = any, kind = 7-bit CC, range 0..1),
  // matching the Node addon reader and the Project-side descriptor reader.
  // There is no engine-side getter for a bound descriptor, so the defaults are
  // pinned through the range guard instead: it rejects `maxValue < minValue`,
  // which brackets each default from both sides.
  //
  // The public descriptor type marks the defaulted fields as required, so the
  // partial literals below are cast; what is under test is the runtime reader.
  type CcBindingDescriptor = Parameters<RealtimeEngine['bindMidiCc']>[0];

  it('accepts a descriptor carrying only ccNumber and paramId', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() =>
      engine.bindMidiCc({ ccNumber: 7, paramId: 1 } as CcBindingDescriptor),
    ).not.toThrow();
    expect(engine.midiCcBindingCount).toBe(1);
  });

  it('defaults an omitted kind to 7-bit Control Change', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    // A kind of 'controlChange14' additionally requires ccLsbNumber === ccNumber + 32,
    // so the descriptor below is accepted only while the default kind is 'controlChange7'.
    expect(() =>
      engine.bindMidiCc({ ccNumber: 7, paramId: 1 } as CcBindingDescriptor),
    ).not.toThrow();
    // The explicit spelling of the same default behaves identically. A distinct
    // ccNumber is used because binding an existing (ccNumber, channel) replaces
    // it rather than appending.
    expect(() =>
      engine.bindMidiCc({ ccNumber: 8, kind: 'controlChange7', paramId: 2 } as CcBindingDescriptor),
    ).not.toThrow();
    expect(engine.midiCcBindingCount).toBe(2);
  });

  it('defaults an omitted minValue to 0 and an omitted maxValue to 1', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    // maxValue defaults to exactly 1: a minValue of 1 is still a valid
    // (degenerate) range, a minValue of 2 inverts it.
    expect(() =>
      engine.bindMidiCc({ ccNumber: 7, paramId: 1, minValue: 1 } as CcBindingDescriptor),
    ).not.toThrow();
    expectRangeRefusal(() =>
      engine.bindMidiCc({ ccNumber: 7, paramId: 1, minValue: 2 } as CcBindingDescriptor),
    );
    // minValue defaults to exactly 0, bracketed the same way.
    expect(() =>
      engine.bindMidiCc({ ccNumber: 7, paramId: 1, maxValue: 0 } as CcBindingDescriptor),
    ).not.toThrow();
    expectRangeRefusal(() =>
      engine.bindMidiCc({ ccNumber: 7, paramId: 1, maxValue: -1 } as CcBindingDescriptor),
    );
  });

  it('still rejects a supplied non-finite range instead of defaulting it', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expectRangeRefusal(() =>
      engine.bindMidiCc({
        ccNumber: 7,
        paramId: 1,
        minValue: Number.NaN,
      } as CcBindingDescriptor),
    );
    expectRangeRefusal(() =>
      engine.bindMidiCc({
        ccNumber: 7,
        paramId: 1,
        maxValue: Number.POSITIVE_INFINITY,
      } as CcBindingDescriptor),
    );
    expectRangeRefusal(() =>
      engine.bindMidiCc({
        ccNumber: 7,
        paramId: 1,
        minValue: Number.NEGATIVE_INFINITY,
        maxValue: 1,
      } as CcBindingDescriptor),
    );
    expect(engine.midiCcBindingCount).toBe(0);
  });

  it('still requires paramId and refuses a numeric kind', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expectTypeRefusal(() => engine.bindMidiCc({ ccNumber: 7 } as unknown as CcBindingDescriptor));
    expectTypeRefusal(() =>
      engine.bindMidiCc({
        ccNumber: 7,
        kind: 9,
        paramId: 1,
      } as unknown as CcBindingDescriptor),
    );
    expect(engine.midiCcBindingCount).toBe(0);
  });

  it('refuses an unknown kind name and a reserved paramId as RangeError', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    const unknownKind = expectRefusalOf(() =>
      engine.bindMidiCc({
        ccNumber: 7,
        kind: 'pitchBend',
        paramId: 1,
      } as unknown as CcBindingDescriptor),
    );
    expect(unknownKind.message).toMatch(/'controlChange7', 'controlChange14', 'rpn', 'nrpn'/);
    // 0xffffffff lies in the insert-parameter namespace the engine reserves.
    const reserved = expectRefusalOf(() => engine.bindMidiCc({ ccNumber: 7, paramId: 0xffffffff }));
    expect(reserved.message).toMatch(/paramId 4294967295 is reserved/);
    expect(engine.midiCcBindingCount).toBe(0);
  });
});

// A non-finite control or sample is REFUSED here, not repaired. Substituting a
// default for a NaN control made a caller error indistinguishable from an
// omitted key, and zeroing a non-finite sample kept the grain history clean
// while silently turning the block into a different block -- with nothing in the
// return value to say so, and with the substituted sample persisting through the
// overlap-add into every later block. Finite-but-out-of-range controls stay
// clamped: that is the documented contract and it is unchanged.
describe('StreamingRetune refuses non-finite input and clamps finite ranges', () => {
  it('requires prepare and rejects blocks above the prepared maximum', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({ pitchSemitones: 2 });
    try {
      expect(() => retune.processMono(new Float32Array(64))).toThrow(/prepared/);
      retune.prepare(48000, 64);
      expect(() => retune.processMono(new Float32Array(65))).toThrow(/maxBlockSize/);
      expect(retune.processMono(new Float32Array(64))).toHaveLength(64);
    } finally {
      retune.destroy();
    }
  });

  it('rejects a non-finite input sample and leaves the caller buffer untouched', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({ pitchSemitones: 2 });
    try {
      retune.prepare(48000, 256);
      const bad = new Float32Array(256).fill(0.1);
      bad[10] = Number.NaN;
      bad[20] = Number.POSITIVE_INFINITY;
      expectRangeRefusal(() => retune.processMono(bad));

      // Refused means untouched: the block the caller still holds is the block
      // it passed, not a repaired one it never asked for.
      expect(Number.isNaN(bad[10])).toBe(true);
      expect(bad[20]).toBe(Number.POSITIVE_INFINITY);
      expect(bad[0]).toBeCloseTo(0.1, 6);

      // And the refusal happened before any of it reached the overlap-add, so a
      // following clean block is still finite -- the property the old
      // zero-substitution bought by rewriting the caller's audio.
      const clean = retune.processMono(new Float32Array(256).fill(0.1));
      expect(Array.from(clean).every((v) => Number.isFinite(v))).toBe(true);
    } finally {
      retune.destroy();
    }
  });

  it('rejects a non-finite control and keeps the value it already had', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    // Construction refuses each of the three controls on its own.
    expectRangeRefusal(() => new StreamingRetune({ pitchSemitones: Number.NaN }));
    expectRangeRefusal(() => new StreamingRetune({ mix: Number.POSITIVE_INFINITY }));
    expectRangeRefusal(() => new StreamingRetune({ grainSizeSamples: Number.NaN }));

    const retune = new StreamingRetune({ pitchSemitones: 5, mix: 0.25, grainSizeSamples: 512 });
    try {
      retune.prepare(48000, 128);
      const before = retune.config();
      expectRangeRefusal(() => retune.setConfig({ pitchSemitones: Number.NaN }));
      expectRangeRefusal(() => retune.setConfig({ mix: Number.NEGATIVE_INFINITY }));
      // A rejected update leaves the whole config as it was: the refusal is not
      // a partial write that applied the keys it read before the bad one.
      expect(retune.config()).toEqual(before);
    } finally {
      retune.destroy();
    }
  });

  it('still clamps a finite out-of-range control', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({
      pitchSemitones: 500,
      mix: 4,
      grainSizeSamples: 1_000_000,
    });
    try {
      retune.prepare(48000, 128);
      const config = retune.config();
      expect(config.pitchSemitones).toBe(24);
      expect(config.mix).toBe(1);
      expect(retune.grainSizeSamples).toBeLessThanOrEqual(8192);
      expect(
        Array.from(retune.processMono(new Float32Array(128).fill(0.1))).every(Number.isFinite),
      ).toBe(true);
    } finally {
      retune.destroy();
    }
  });
});

describe('offline boundary validation always scans the samples', () => {
  it('rejects NaN/Inf in every quick-analysis entry', () => {
    const bad = new Float32Array(2048);
    bad[17] = Number.NaN;
    const calls = [
      () => detectBpm(bad, 22050),
      () => detectKey({ samples: bad, sampleRate: 22050 }),
      () => detectKeyCandidates({ samples: bad, sampleRate: 22050 }),
      () => detectOnsets(bad, 22050),
      () => detectBeats(bad, 22050),
      () => detectDownbeats(bad, 22050),
      () => analyze(bad, 22050),
    ];
    for (const call of calls) {
      expect(call).toThrow(RangeError);
    }
  });

  it('rejects non-finite values in all signal transforms', () => {
    const bad = new Float32Array([0, Number.POSITIVE_INFINITY, 1]);
    const calls = [
      () => preemphasis(bad, 22050),
      () => deemphasis(bad, 22050),
      () => trimSilence(bad, 22050, { topDb: 20, frameLength: 2, hopLength: 1 }),
      () => splitSilence(bad, 22050, { topDb: 20, frameLength: 2, hopLength: 1 }),
      () => frameSignal(bad, 22050, { frameLength: 2, hopLength: 1 }),
      () => padCenter({ values: bad, targetSize: 4 }),
      () => fixLength({ values: bad, targetSize: 4 }),
    ];
    for (const call of calls) {
      expect(call).toThrow();
    }
  });

  it('rejects negative target sizes without attempting an allocation', () => {
    const values = new Float32Array([1, 2]);
    expect(() => padCenter({ values, targetSize: -1 })).toThrow();
    expect(() => fixLength({ values, targetSize: -1 })).toThrow();
  });

  it('rejects invalid time-varying pitch tracks and configuration', () => {
    const samples = new Float32Array(2048).fill(0.1);
    const f0Hz = new Float32Array([440]);
    expect(() =>
      pitchCorrectTimevarying(samples, 22050, { f0Hz: new Float32Array(), hopLength: 256 }),
    ).toThrow();
    expect(() => pitchCorrectTimevarying(samples, 22050, { f0Hz, hopLength: 0 })).toThrow();
    expect(() =>
      pitchCorrectTimevarying(samples, 22050, {
        f0Hz,
        hopLength: 256,
        maxCorrectionSemitones: -1,
      }),
    ).toThrow();
    expect(() =>
      pitchCorrectTimevarying(samples, 22050, {
        f0Hz,
        hopLength: 256,
        retuneSpeedMs: Number.NaN,
      }),
    ).toThrow();
    expect(() =>
      pitchCorrectTimevarying(samples, 22050, {
        f0Hz,
        hopLength: 256,
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
      masterAudio({
        samples,
        sampleRate: 22050,
        overrides: { loudness: { enabled: false, truePeakOversample: 3 } },
      }),
    ).toThrow();
  });

  it('refuses a fractional clipping-region length, the sign case naming the sign', () => {
    // Clipped runs three samples long, so the region list responds to the
    // threshold between 2 and 5 -- the control that makes the refusals below
    // measure a live argument rather than an ignored one.
    const samples = Float32Array.from({ length: 4096 }, (_, i) =>
      i % 200 < 3 ? 1 : Math.sin(i * 0.05) * 0.3,
    );
    const regionsAt = (minRegionSamples: number) =>
      meteringDetectClipping({ samples, sampleRate: 22050, minRegionSamples }).regions.length;
    expect(regionsAt(2)).toBeGreaterThan(0);
    expect(regionsAt(5)).toBe(0);

    for (const value of [2.5, -1]) {
      expect(() =>
        meteringDetectClipping({ samples, sampleRate: 22050, minRegionSamples: value }),
      ).toThrow(/minRegionSamples must be a non-negative integer/);
    }
  });

  it('accepts a zero-strip processStereo call and returns an empty master', () => {
    const mixer = Mixer.fromJson(
      '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
      { sampleRate: 48000, maxBlockSize: 128 },
    );
    const result = mixer.processStereo([], []);
    expect(result.left).toHaveLength(0);
    expect(result.right).toHaveLength(0);
  });
});
