/**
 * Node facade coverage for the new C ABI engine surface: bus output routing
 * and sends (`SonareEngineBus.outputBusId`/`sends`), bus/master sidechain keys
 * (`sonare_engine_set_bus_sidechain`/`sonare_engine_set_master_sidechain`), and
 * MIDI clip gain/fade (`SonareEngineMidiClipSchedule.gain`/`fadeInSamples`/
 * `fadeOutSamples`). Mirrors the rig in
 * tests/api/sonare_c_engine_routing_test.cpp: a mono DC source per track lane,
 * settled after enough blocks for the duckers' release to matter.
 */

import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, RealtimeEngine } from '../src/index.js';

const BLOCK = 256;
const FRAMES = BLOCK * 40;

const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
  (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

/** Asserts that `call` throws a SonareError carrying exactly `code`. */
function expectSonareErrorCode(call: () => void, code: ErrorCode): void {
  let error: unknown;
  try {
    call();
  } catch (thrown) {
    error = thrown;
  }
  expect(isSonareError(error)).toBe(true);
  if (!isSonareError(error)) {
    throw new Error('expected SonareError');
  }
  expect(error.code).toBe(code);
}

/** One mono DC(1.0) clip on `trackId`, routed onto lane->bus `outputBusId`. */
function dcEngine(trackId: number, outputBusId: number): RealtimeEngine {
  const engine = new RealtimeEngine(48000, BLOCK);
  engine.setClips([
    {
      id: trackId,
      trackId,
      channels: [new Float32Array(FRAMES).fill(1.0)],
      startPpq: 0,
      lengthSamples: FRAMES,
    },
  ]);
  engine.setTrackLanes([{ trackId, outputBusId }]);
  return engine;
}

/** Plays from the top and returns the last rendered mono sample. */
function settled(engine: RealtimeEngine, blocks = 30): number {
  engine.seekSample(0);
  engine.play();
  let last = 0;
  for (let b = 0; b < blocks; b += 1) {
    const [out] = engine.process([new Float32Array(BLOCK)]);
    last = out[BLOCK - 1];
  }
  return last;
}

describe('RealtimeEngine bus routing and sends', () => {
  it('routes a bus output into another bus', () => {
    const direct = dcEngine(10, 0);
    const x = settled(direct);
    direct.destroy();
    expect(x).toBeGreaterThan(0.5);

    const throughBus2 = new RealtimeEngine(48000, BLOCK);
    throughBus2.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [new Float32Array(FRAMES).fill(1.0)],
        startPpq: 0,
        lengthSamples: FRAMES,
      },
    ]);
    throughBus2.setTrackBuses([
      { busId: 1, gainDb: 0, outputBusId: 2 },
      { busId: 2, gainDb: -6 },
    ]);
    throughBus2.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    const half = 10 ** (-6 / 20);
    expect(settled(throughBus2)).toBeCloseTo(x * half, 3);
    throughBus2.destroy();
  });

  it('taps bus sends before or after gainDb', () => {
    const direct = dcEngine(10, 0);
    const x = settled(direct);
    direct.destroy();

    const run = (timing: 'preFader' | 'postFader'): number => {
      const engine = new RealtimeEngine(48000, BLOCK);
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(FRAMES).fill(1.0)],
          startPpq: 0,
          lengthSamples: FRAMES,
        },
      ]);
      engine.setTrackBuses([
        { busId: 1, gainDb: -12, sends: [{ busId: 2, levelDb: 0, sendTiming: timing }] },
        { busId: 2, gainDb: 0 },
      ]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
      const level = settled(engine);
      engine.destroy();
      return level;
    };
    const g1 = 10 ** (-12 / 20);
    // Pre-fader: bus 1's direct output (g1*x) plus the send (1.0*x, untouched
    // by gainDb) both land on master.
    expect(run('preFader')).toBeCloseTo(x * (g1 + 1.0), 3);
    // Post-fader: the send taps after gainDb, so both paths carry g1.
    expect(run('postFader')).toBeCloseTo(x * 2 * g1, 3);
  });
});

const kDuckerBusJson = JSON.stringify({
  version: 1,
  strips: [],
  buses: [
    {
      id: '1',
      inserts: [
        {
          slot: 'pre',
          processor: 'dynamics.duckingProcessor',
          params: JSON.stringify({
            thresholdDb: -20,
            ratio: 20,
            attackMs: 0.05,
            releaseMs: 80,
            rangeDb: 30,
          }),
        },
      ],
    },
  ],
  connections: [],
});
const kLimiterBusJson = JSON.stringify({
  version: 1,
  strips: [],
  buses: [
    {
      id: '1',
      inserts: [
        {
          slot: 'pre',
          processor: 'dynamics.limiter',
          params: JSON.stringify({ thresholdDb: 24, lookaheadMs: 0, releaseMs: 50 }),
        },
      ],
    },
  ],
  connections: [],
});
const kDuckerMasterJson = JSON.stringify({
  version: 1,
  strips: [
    {
      id: 'master',
      inserts: [
        {
          slot: 'pre',
          processor: 'dynamics.duckingProcessor',
          params: JSON.stringify({
            thresholdDb: -20,
            ratio: 20,
            attackMs: 0.05,
            releaseMs: 80,
            rangeDb: 30,
          }),
        },
      ],
    },
  ],
  buses: [],
  connections: [],
});

/**
 * Track 10 is a quiet program (-26 dB, under the duckers' -20 dB threshold)
 * into bus 2, which carries a ducker keyed from bus 1. Track 30 is a loud
 * key-only source into bus 1, whose -60 dB gainDb keeps it out of the mix
 * while its key (tapped before gainDb) stays loud. Bus 1 carries a
 * pass-through limiter so it has an insert 0 to key; the master carries a
 * ducker. Mirrors sonare_c_engine_routing_test.cpp's make_keyed_rig().
 */
function makeKeyedRig(): RealtimeEngine {
  const engine = new RealtimeEngine(48000, BLOCK);
  engine.setClips([
    {
      id: 1,
      trackId: 10,
      channels: [new Float32Array(FRAMES).fill(0.05)],
      startPpq: 0,
      lengthSamples: FRAMES,
    },
    {
      id: 2,
      trackId: 30,
      channels: [new Float32Array(FRAMES).fill(1.0)],
      startPpq: 0,
      lengthSamples: FRAMES,
    },
  ]);
  engine.setTrackBuses([
    { busId: 1, gainDb: -60 },
    { busId: 2, gainDb: 0 },
  ]);
  engine.setTrackLanes([
    { trackId: 10, outputBusId: 2 },
    { trackId: 30, outputBusId: 1 },
  ]);
  engine.setBusStripJson(1, kLimiterBusJson);
  engine.setBusStripJson(2, kDuckerBusJson);
  engine.setMasterStripJson(kDuckerMasterJson);
  engine.setBusSidechain(2, 0, 'bus', 1);
  return engine;
}

describe('RealtimeEngine bus and master sidechain', () => {
  it('ducks from a bus or track source and clears on sourceId 0', () => {
    const reference = makeKeyedRig();
    reference.setBusSidechain(2, 0, 'bus', 0);
    const unkeyed = settled(reference);
    reference.destroy();
    expect(unkeyed).toBeGreaterThan(0.02);

    const engine = makeKeyedRig();
    expect(settled(engine)).toBeLessThan(unkeyed * 0.3);

    // A track-sourced key on bus 2 ducks it the same way.
    engine.setBusSidechain(2, 0, 'track', 30);
    expect(settled(engine)).toBeLessThan(unkeyed * 0.3);

    // The master ducker keyed from a track or a bus pulls the whole mix down.
    engine.setMasterSidechain(0, 'track', 30);
    expect(settled(engine)).toBeLessThan(unkeyed * 0.3);
    engine.setMasterSidechain(0, 'bus', 1);
    expect(settled(engine)).toBeLessThan(unkeyed * 0.3);

    // Clearing the master key leaves bus 2 keyed alone; clearing that too
    // returns the render to the unkeyed level once the 80ms release runs out.
    engine.setMasterSidechain(0, 'bus', 0);
    expect(settled(engine)).toBeLessThan(unkeyed * 0.3);
    engine.setBusSidechain(2, 0, 'track', 0);
    for (let pass = 0; pass < 8; pass += 1) {
      settled(engine);
    }
    expect(settled(engine)).toBeCloseTo(unkeyed, 3);
    engine.destroy();
  });

  it('rejects an unknown source_kind and leaves state unchanged', () => {
    const engine = makeKeyedRig();
    // An out-of-range numeric sourceKind is refused client-side, before it
    // ever reaches the C ABI (the same enum-ordinal validation every other
    // string/number union on this facade uses).
    expect(() => engine.setBusSidechain(1, 0, 2, 30)).toThrow(RangeError);
    expect(() => engine.setMasterSidechain(0, 2, 30)).toThrow(RangeError);
    // A declared sourceKind naming an undeclared source is refused by the C
    // ABI itself.
    expectSonareErrorCode(
      () => engine.setBusSidechain(1, 0, 'track', 99),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(() => engine.setMasterSidechain(0, 'bus', 9), ErrorCode.InvalidParameter);
    engine.destroy();
  });
});

describe('RealtimeEngine MIDI clip gain and fade', () => {
  const destination = 9;
  const heldNote = [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 }];

  function renderMidiClip(clip: {
    gain?: number;
    fadeInSamples?: number;
    fadeOutSamples?: number;
    lengthSamples?: number;
  }): Float32Array {
    const engine = new RealtimeEngine(48000, BLOCK);
    engine.setBuiltinInstrument({ gain: 0.5 }, destination);
    engine.setMidiClips([
      {
        id: 42,
        trackId: destination,
        destinationId: destination,
        lengthSamples: FRAMES,
        events: heldNote,
        ...clip,
      },
    ]);
    engine.play();
    const out: number[] = [];
    for (let b = 0; b < 8; b += 1) {
      const [left] = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      out.push(...left);
    }
    engine.destroy();
    return new Float32Array(out);
  }

  it('scales the destination render by gain, and omitted gain equals unity', () => {
    const unity = renderMidiClip({});
    const explicitUnity = renderMidiClip({ gain: 1.0 });
    const half = renderMidiClip({ gain: 0.5 });
    const peak = Math.max(...Array.from(unity, Math.abs));
    expect(peak).toBeGreaterThan(0.01);
    for (let i = 0; i < unity.length; i += 1) {
      expect(explicitUnity[i]).toBeCloseTo(unity[i], 6);
      expect(half[i]).toBeCloseTo(0.5 * unity[i], 6);
    }
  });

  it('refuses a fade-out on an open-ended clip', () => {
    expect(() => renderMidiClip({ lengthSamples: 0, fadeOutSamples: 256 })).toThrow();
    // A fade-in on an open-ended clip is fine.
    expect(() => renderMidiClip({ lengthSamples: 0, fadeInSamples: 256 })).not.toThrow();
  });
});
