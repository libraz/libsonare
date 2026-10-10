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

/** Renders `blocks` blocks from the top and returns the first channel's samples. */
function renderBlocks(engine: RealtimeEngine, blocks = 12): Float32Array {
  engine.seekSample(0);
  engine.play();
  const out = new Float32Array(blocks * BLOCK);
  for (let b = 0; b < blocks; b += 1) {
    out.set(engine.process([new Float32Array(BLOCK)])[0], b * BLOCK);
  }
  return out;
}

function expectSameRender(a: Float32Array, b: Float32Array): void {
  expect(a.length).toBe(b.length);
  for (let i = 0; i < a.length; i += 1) {
    expect(Math.abs(a[i] - b[i])).toBeLessThanOrEqual(1e-6);
  }
}

/** Two declared buses and one DC track (10) on the master mix, no sends. */
function partialOpsEngine(): RealtimeEngine {
  const engine = new RealtimeEngine(48000, BLOCK);
  engine.setClips([
    {
      id: 1,
      trackId: 10,
      channels: [new Float32Array(FRAMES).fill(0.5)],
      startPpq: 0,
      lengthSamples: FRAMES,
    },
    {
      id: 2,
      trackId: 11,
      channels: [new Float32Array(FRAMES).fill(0.25)],
      startPpq: 0,
      lengthSamples: FRAMES,
    },
  ]);
  engine.setTrackBuses([
    { busId: 1, gainDb: 0 },
    { busId: 2, gainDb: -6 },
  ]);
  engine.setTrackLanes([{ trackId: 10 }, { trackId: 11 }]);
  return engine;
}

const peak = (samples: Float32Array): number =>
  samples.reduce((m, v) => Math.max(m, Math.abs(v)), 0);

describe('RealtimeEngine per-lane sends and output bus', () => {
  it('setTrackSends matches setTrackLanes with the same final configuration', () => {
    const partial = partialOpsEngine();
    const replaced = partialOpsEngine();
    partial.setTrackSends(10, [{ busId: 2, levelDb: -3 }]);
    replaced.setTrackLanes([{ trackId: 10, sends: [{ busId: 2, levelDb: -3 }] }, { trackId: 11 }]);
    const withSend = renderBlocks(partial);
    expectSameRender(withSend, renderBlocks(replaced));
    expect(peak(withSend)).toBeGreaterThan(0.1);

    const baseline = partialOpsEngine();
    expect(peak(withSend)).toBeGreaterThan(peak(renderBlocks(baseline)) * 1.1);
    partial.destroy();
    replaced.destroy();
    baseline.destroy();
  });

  it('setTrackSends leaves the output bus and the other lanes unchanged', () => {
    const partial = partialOpsEngine();
    const replaced = partialOpsEngine();
    partial.setTrackLanes([{ trackId: 10, outputBusId: 1 }, { trackId: 11 }]);
    replaced.setTrackLanes([{ trackId: 10, outputBusId: 1 }, { trackId: 11 }]);
    partial.setTrackSends(10, [{ busId: 2 }]);
    replaced.setTrackLanes([
      { trackId: 10, outputBusId: 1, sends: [{ busId: 2 }] },
      { trackId: 11 },
    ]);
    expectSameRender(renderBlocks(partial), renderBlocks(replaced));
    partial.destroy();
    replaced.destroy();
  });

  it('setTrackSends with an empty array clears the lane sends', () => {
    const partial = partialOpsEngine();
    const replaced = partialOpsEngine();
    partial.setTrackSends(10, [{ busId: 2 }]);
    replaced.setTrackLanes([{ trackId: 10, sends: [{ busId: 2 }] }, { trackId: 11 }]);
    partial.setTrackSends(10, []);
    replaced.setTrackLanes([{ trackId: 10 }, { trackId: 11 }]);
    expectSameRender(renderBlocks(partial), renderBlocks(replaced));
    partial.destroy();
    replaced.destroy();
  });

  it('setTrackOutputBus matches setTrackLanes and 0 returns to the master mix', () => {
    const partial = partialOpsEngine();
    const replaced = partialOpsEngine();
    partial.setTrackSends(10, [{ busId: 2 }]);
    replaced.setTrackLanes([{ trackId: 10, sends: [{ busId: 2 }] }, { trackId: 11 }]);
    partial.setTrackOutputBus(10, 1);
    replaced.setTrackLanes([
      { trackId: 10, outputBusId: 1, sends: [{ busId: 2 }] },
      { trackId: 11 },
    ]);
    expectSameRender(renderBlocks(partial), renderBlocks(replaced));

    partial.setTrackOutputBus(10, 0);
    replaced.setTrackLanes([{ trackId: 10, sends: [{ busId: 2 }] }, { trackId: 11 }]);
    expectSameRender(renderBlocks(partial), renderBlocks(replaced));
    partial.destroy();
    replaced.destroy();
  });

  it('refuses an unknown track with a coded error naming the id', () => {
    const engine = partialOpsEngine();
    const before = renderBlocks(engine);
    for (const call of [
      () => engine.setTrackSends(99, []),
      () => engine.setTrackOutputBus(99, 0),
    ]) {
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
      expect(error.code).toBe(ErrorCode.InvalidParameter);
      expect(error.message).toMatch(/unknown track id 99/);
    }
    expectSameRender(renderBlocks(engine), before);
    engine.destroy();
  });

  it('refuses an undeclared bus, a duplicate send bus and an out-of-range level', () => {
    const engine = partialOpsEngine();
    engine.setTrackSends(10, [{ busId: 2 }]);
    const before = renderBlocks(engine);
    expectSonareErrorCode(
      () => engine.setTrackSends(10, [{ busId: 7 }]),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(() => engine.setTrackOutputBus(10, 7), ErrorCode.InvalidParameter);
    expectSonareErrorCode(
      () => engine.setTrackSends(10, [{ busId: 1 }, { busId: 1 }]),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(
      () => engine.setTrackSends(10, [{ busId: 1, levelDb: 100 }]),
      ErrorCode.InvalidParameter,
    );
    expectSameRender(renderBlocks(engine), before);
    engine.destroy();
  });

  it('refuses a non-array sends instead of clearing', () => {
    const engine = partialOpsEngine();
    engine.setTrackSends(10, [{ busId: 2 }]);
    const before = renderBlocks(engine);
    const call = engine.setTrackSends as unknown as (t: number, s?: unknown) => void;
    expect(() => call.call(engine, 10, undefined)).toThrow(TypeError);
    expect(() => call.call(engine, 10, { busId: 2 })).toThrow(TypeError);
    expectSameRender(renderBlocks(engine), before);
    engine.destroy();
  });

  it('keeps setTrackLanes replace semantics: omitting sends clears them', () => {
    const engine = partialOpsEngine();
    engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 2 }] }, { trackId: 11 }]);
    const withSend = peak(renderBlocks(engine));
    expect(withSend).toBeGreaterThan(0.1);
    engine.setTrackLanes([{ trackId: 10 }, { trackId: 11 }]);
    const omitted = peak(renderBlocks(engine));
    expect(omitted).toBeGreaterThan(0.1);
    expect(withSend).toBeGreaterThan(omitted * 1.1);
    engine.destroy();
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
const kDuckerTrackJson = JSON.stringify({
  version: 1,
  strips: [
    {
      id: 'track-10',
      inserts: [
        {
          slot: 'pre',
          processor: 'dynamics.duckingProcessor',
          params: { thresholdDb: -20, ratio: 20, attackMs: 0.05, releaseMs: 80, rangeDb: 30 },
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

describe('RealtimeEngine sidechain pre-check', () => {
  it('reports ok for a binding the setter accepts', () => {
    const engine = makeKeyedRig();
    expect(engine.canSetBusSidechain(2, 0, 'track', 30)).toEqual({ ok: true, reason: null });
    expect(engine.canSetMasterSidechain(0, 'bus', 1)).toEqual({ ok: true, reason: null });
    // Unbinding is always acceptable.
    expect(engine.canSetBusSidechain(2, 0, 'bus', 0)).toEqual({ ok: true, reason: null });
    // The check changes nothing: the bus key set by the rig is still in force.
    expect(settled(engine)).toBeLessThan(0.02);
    engine.destroy();
  });

  it('names the refusal reason without changing state', () => {
    const engine = makeKeyedRig();
    expect(engine.canSetBusSidechain(9, 0, 'track', 30)).toEqual({
      ok: false,
      reason: 'invalidTarget',
    });
    expect(engine.canSetBusSidechain(2, 7, 'track', 30)).toEqual({
      ok: false,
      reason: 'insertOutOfRange',
    });
    expect(engine.canSetBusSidechain(2, 0, 'track', 99)).toEqual({
      ok: false,
      reason: 'undeclaredSource',
    });
    expect(engine.canSetBusSidechain(2, 0, 'bus', 2)).toEqual({ ok: false, reason: 'selfKey' });
    expect(engine.canSetMasterSidechain(5, 'bus', 1)).toEqual({
      ok: false,
      reason: 'insertOutOfRange',
    });
    expect(engine.canSetMasterSidechain(0, 'bus', 9)).toEqual({
      ok: false,
      reason: 'undeclaredSource',
    });
    expect(engine.canSetLaneSidechain(0, 0, 10)).toEqual({ ok: false, reason: 'invalidTarget' });
    // Bus 2 is keyed from bus 1; keying bus 1 from bus 2 would close a loop.
    expect(engine.canSetBusSidechain(1, 0, 'bus', 2)).toEqual({ ok: false, reason: 'cycle' });
    engine.destroy();
  });

  it('refuses an unknown sourceKind by name like the setters do', () => {
    const engine = makeKeyedRig();
    expect(() => engine.canSetBusSidechain(2, 0, 2, 30)).toThrow(RangeError);
    expect(() => engine.canSetMasterSidechain(0, 'sideways' as never, 30)).toThrow(RangeError);
    engine.destroy();
  });

  it('names an undeclared source and an insert the lane strip lacks on the lane form', () => {
    const engine = makeKeyedRig();
    expect(engine.canSetLaneSidechain(10, 0, 99)).toEqual({
      ok: false,
      reason: 'insertOutOfRange',
    });
    engine.setTrackStripJson(10, kDuckerTrackJson);
    expect(engine.canSetLaneSidechain(10, 0, 99)).toEqual({
      ok: false,
      reason: 'undeclaredSource',
    });
    expect(engine.canSetLaneSidechain(10, 3, 30)).toEqual({
      ok: false,
      reason: 'insertOutOfRange',
    });
    expect(engine.canSetLaneSidechain(10, 0, 30)).toEqual({ ok: true, reason: null });
    expectSonareErrorCode(() => engine.setLaneSidechain(10, 0, 99), ErrorCode.InvalidParameter);
    expectSonareErrorCode(() => engine.setLaneSidechain(10, 3, 30), ErrorCode.InvalidParameter);
    expect(() => engine.setLaneSidechain(10, 0, 30)).not.toThrow();
    engine.destroy();
  });

  it('agrees with the setter on the lane form', () => {
    const engine = makeKeyedRig();
    const check = engine.canSetLaneSidechain(10, 0, 30);
    if (check.ok) {
      expect(() => engine.setLaneSidechain(10, 0, 30)).not.toThrow();
    } else {
      expect(check.reason).not.toBeNull();
    }
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
