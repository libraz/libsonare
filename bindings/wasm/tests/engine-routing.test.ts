/**
 * WASM + worklet bindings for bus-to-bus routing, bus/master sidechain keys,
 * and MIDI clip gain/fade. Mirrors tests/api/sonare_c_engine_routing_test.cpp,
 * the C ABI oracle this surface wraps.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine, type SidechainSourceKind } from '../dist/index.js';
import { isEngineSyncMessage } from '../src/worklet/guards';
import { SonareEngine, setupWorklet } from './_worklet_helpers';

/** The engine `SonareEngine.create` accepts as its offline mirror. */
type OfflineEngineOption = NonNullable<
  NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
>;

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: 48000,
    audioWorklet: {
      added: [] as (string | URL)[],
      addModule(moduleUrl: string | URL): Promise<void> {
        this.added.push(moduleUrl);
        return Promise.resolve();
      },
    },
  } as unknown as BaseAudioContext;
}

function readyWorkletNode(port: {
  onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  [member: string]: unknown;
}): AudioWorkletNode {
  queueMicrotask(() => {
    port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
  });
  return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
}

describe('engine bus routing, sidechain and MIDI clip gain (WASM)', () => {
  const dbToLinear = (db: number): number => 10 ** (db / 20);
  const kBlock = 256;
  const kBlocks = 40;
  const frames = kBlock * kBlocks;

  const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

  const closeRel = (actual: number, expected: number, rel = 1e-3): void => {
    const tolerance = rel * Math.max(1, Math.abs(expected));
    expect(Math.abs(actual - expected)).toBeLessThanOrEqual(tolerance);
  };

  // index.js and worklet.js are separate self-contained bundles, so both
  // module singletons must be initialized (setupWorklet does both).
  setupWorklet();

  function makeRoutingEngine(tracks: Array<{ trackId: number; gain: number }>): RealtimeEngine {
    const engine = new RealtimeEngine(48000, kBlock);
    engine.setClips(
      tracks.map(({ trackId, gain }) => ({
        trackId,
        channels: [new Float32Array(frames).fill(1)],
        startPpq: 0,
        lengthSamples: frames,
        gain,
      })),
    );
    return engine;
  }

  // Plays from the top and returns the last rendered sample of a mono render
  // (num_channels 1, matching the C ABI test's direct engine.process(io, 1,...)).
  function settled(engine: RealtimeEngine, blocks = 30): number {
    engine.seekSample(0);
    engine.play();
    let last = 0;
    for (let block = 0; block < blocks; block += 1) {
      const [chunk] = engine.process([new Float32Array(kBlock)]);
      last = chunk[chunk.length - 1];
    }
    return last;
  }

  function directLevel(): number {
    const engine = makeRoutingEngine([{ trackId: 10, gain: 1 }]);
    engine.setTrackLanes([{ trackId: 10 }]);
    const level = settled(engine);
    engine.destroy();
    expect(level).toBeGreaterThan(0.5);
    return level;
  }

  describe('bus output and sends', () => {
    it('routes a bus output into another bus', () => {
      const x = directLevel();
      const half = dbToLinear(-6);
      const run = (bus1Output: number): number => {
        const engine = makeRoutingEngine([{ trackId: 10, gain: 1 }]);
        engine.setTrackBuses([
          { busId: 1, gainDb: 0, outputBusId: bus1Output },
          { busId: 2, gainDb: -6 },
        ]);
        engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
        const level = settled(engine);
        engine.destroy();
        return level;
      };
      closeRel(run(0), x);
      // Through bus 2, its -6 dB gainDb applies on top.
      closeRel(run(2), x * half);
    });

    it('taps a bus send before or after the bus gain', () => {
      const x = directLevel();
      const g1 = dbToLinear(-12);
      const run = (sendTiming: number, levelDb: number, enabled: boolean): number => {
        const engine = makeRoutingEngine([{ trackId: 10, gain: 1 }]);
        engine.setTrackBuses([
          { busId: 1, gainDb: -12, sends: [{ busId: 2, levelDb, enabled, sendTiming }] },
          { busId: 2, gainDb: 0 },
        ]);
        engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
        const level = settled(engine);
        engine.destroy();
        return level;
      };
      // sendTiming 1 = pre-fader (taps before bus 1's gainDb).
      closeRel(run(1, 0, true), x * (g1 + 1), 1e-2);
      // sendTiming 0 = post-fader (taps after bus 1's gainDb, same as output).
      closeRel(run(0, 0, true), x * 2 * g1, 1e-2);
      const half = dbToLinear(-6);
      closeRel(run(1, -6, true), x * (g1 + half), 1e-2);
      closeRel(run(0, 0, false), x * g1, 1e-2);
    });
  });

  describe('per-lane sends and output bus', () => {
    const send = (busId: number, levelDb = -6) => ({ busId, levelDb, enabled: true });
    const buses = [
      { busId: 1, gainDb: -3 },
      { busId: 2, gainDb: 0 },
    ];

    function rig(): RealtimeEngine {
      const engine = makeRoutingEngine([
        { trackId: 10, gain: 1 },
        { trackId: 20, gain: 0.5 },
      ]);
      engine.setTrackBuses(buses);
      return engine;
    }

    // Every sample of a mono render, so two engines compare block by block.
    function render(engine: RealtimeEngine, blocks = 8): Float32Array {
      engine.seekSample(0);
      engine.play();
      const out = new Float32Array(blocks * kBlock);
      for (let block = 0; block < blocks; block += 1) {
        const [chunk] = engine.process([new Float32Array(kBlock)]);
        out.set(chunk, block * kBlock);
      }
      return out;
    }

    function expectSame(actual: Float32Array, expected: Float32Array): void {
      expect(actual.length).toBe(expected.length);
      let peak = 0;
      for (let i = 0; i < actual.length; i += 1) {
        expect(Math.abs(actual[i] - expected[i])).toBeLessThanOrEqual(1e-6);
        peak = Math.max(peak, Math.abs(actual[i]));
      }
      // A comparison of two silences proves nothing.
      expect(peak).toBeGreaterThan(0.1);
    }

    it('setTrackSends equals setTrackLanes with the same final lanes', () => {
      const edited = rig();
      edited.setTrackLanes([{ trackId: 10, outputBusId: 1 }, { trackId: 20 }]);
      edited.setTrackSends(10, [send(2)]);
      const replaced = rig();
      replaced.setTrackLanes([{ trackId: 10, outputBusId: 1, sends: [send(2)] }, { trackId: 20 }]);
      expectSame(render(edited), render(replaced));
      edited.destroy();
      replaced.destroy();
    });

    it('setTrackOutputBus equals setTrackLanes with the same final lanes', () => {
      const edited = rig();
      edited.setTrackLanes([{ trackId: 10, sends: [send(2)] }, { trackId: 20 }]);
      edited.setTrackOutputBus(10, 1);
      const replaced = rig();
      replaced.setTrackLanes([{ trackId: 10, outputBusId: 1, sends: [send(2)] }, { trackId: 20 }]);
      expectSame(render(edited), render(replaced));
      // Output 0 returns the lane to the master mix.
      edited.setTrackOutputBus(10, 0);
      const master = rig();
      master.setTrackLanes([{ trackId: 10, sends: [send(2)] }, { trackId: 20 }]);
      expectSame(render(edited), render(master));
      edited.destroy();
      replaced.destroy();
      master.destroy();
    });

    it('clears a lane sends with an empty array', () => {
      const edited = rig();
      edited.setTrackLanes([{ trackId: 10, sends: [send(2, 0)] }, { trackId: 20 }]);
      const withSend = render(edited);
      edited.setTrackSends(10, []);
      const cleared = render(edited);
      expect(Math.abs(cleared[cleared.length - 1])).toBeLessThan(
        Math.abs(withSend[withSend.length - 1]),
      );
      expect(Math.abs(cleared[cleared.length - 1])).toBeGreaterThan(0.1);
      edited.destroy();
    });

    it('refuses an invalid edit and the lane renders as before', () => {
      const engine = rig();
      engine.setTrackLanes([{ trackId: 10, sends: [send(2)] }, { trackId: 20 }]);
      const baseline = render(engine);
      expect(() => engine.setTrackSends(99, [send(2)])).toThrow();
      expect(() => engine.setTrackOutputBus(99, 1)).toThrow();
      expect(() => engine.setTrackSends(10, [send(77)])).toThrow();
      expect(() => engine.setTrackOutputBus(10, 77)).toThrow();
      expect(() => engine.setTrackSends(10, [send(2), send(2)])).toThrow();
      expect(() => engine.setTrackSends(10, [send(2, 25)])).toThrow();
      expectSame(render(engine), baseline);
      engine.destroy();
    });

    it('refuses the positional ops on an engine with no lanes and on a missing argument', () => {
      const engine = rig();
      expect(() => engine.setTrackSends(10, [send(2)])).toThrow();
      engine.setTrackLanes([10]);
      // @ts-expect-error sends is required; undefined is not "clear"
      expect(() => engine.setTrackSends(10)).toThrow();
      engine.destroy();
    });

    it('setTrackLanes omitting sends replaces them (the send bus goes silent)', () => {
      const engine = rig();
      engine.setTrackLanes([{ trackId: 10, sends: [send(2, 0)] }, { trackId: 20 }]);
      const withSend = render(engine);
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
      const omitted = render(engine);
      // The send adds a second copy of the lane; omitting it leaves the bare level.
      expect(Math.abs(withSend[withSend.length - 1])).toBeGreaterThan(
        Math.abs(omitted[omitted.length - 1]) + 0.1,
      );
      expect(Math.abs(omitted[omitted.length - 1])).toBeGreaterThan(0.1);
      engine.destroy();
    });
  });

  describe('bus and master sidechain', () => {
    // Track 10 is quiet program (-26 dB, under the duckers' -20 dB threshold)
    // into bus 2, which carries a ducker keyed from bus 1. Track 30 is a loud
    // key-only source into bus 1, whose -60 dB gainDb keeps it out of the mix
    // while its key (tapped before gainDb) stays loud. Bus 1 carries a
    // pass-through limiter so it has an insert 0 to key; the master carries a
    // ducker. JSON strings match kDuckerBusJson / kLimiterBusJson /
    // kDuckerMasterJson in the C ABI test byte for byte.
    const duckerParams = JSON.stringify({
      thresholdDb: -20,
      ratio: 20,
      attackMs: 0.05,
      releaseMs: 80,
      rangeDb: 30,
    });
    const duckerBusJson = JSON.stringify({
      version: 1,
      strips: [],
      buses: [
        {
          id: '1',
          inserts: [{ slot: 'pre', processor: 'dynamics.duckingProcessor', params: duckerParams }],
        },
      ],
      connections: [],
    });
    const limiterBusJson = JSON.stringify({
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
    const duckerMasterJson = JSON.stringify({
      version: 1,
      strips: [
        {
          id: 'master',
          inserts: [{ slot: 'pre', processor: 'dynamics.duckingProcessor', params: duckerParams }],
        },
      ],
      buses: [],
      connections: [],
    });

    function makeKeyedRig(): RealtimeEngine {
      const engine = makeRoutingEngine([
        { trackId: 10, gain: 0.05 },
        { trackId: 30, gain: 1 },
      ]);
      engine.setTrackBuses([
        { busId: 1, gainDb: -60 },
        { busId: 2, gainDb: 0 },
      ]);
      engine.setTrackLanes([
        { trackId: 10, outputBusId: 2 },
        { trackId: 30, outputBusId: 1 },
      ]);
      engine.setBusStripJson(1, limiterBusJson);
      engine.setBusStripJson(2, duckerBusJson);
      engine.setMasterStripJson(duckerMasterJson);
      // Bus 2 insert 0 keyed from bus 1 (sourceKind 1 = bus).
      engine.setBusSidechain(2, 0, 1, 1);
      return engine;
    }

    it('keys and clears bus and master sidechain', () => {
      const reference = makeKeyedRig();
      reference.setBusSidechain(2, 0, 1, 0);
      const unkeyed = settled(reference);
      reference.destroy();
      expect(unkeyed).toBeGreaterThan(0.02);

      const engine = makeKeyedRig();
      // Bus 1's loud key ducks bus 2, which carries nearly all of the mix.
      expect(Math.abs(settled(engine))).toBeLessThan(unkeyed * 0.3);

      // A track-sourced key on bus 2 ducks it the same way.
      engine.setBusSidechain(2, 0, 'track', 30);
      expect(Math.abs(settled(engine))).toBeLessThan(unkeyed * 0.3);

      // The master ducker keyed from a track or a bus pulls the whole mix down.
      engine.setMasterSidechain(0, 0, 30);
      expect(Math.abs(settled(engine))).toBeLessThan(unkeyed * 0.3);
      engine.setMasterSidechain(0, 'bus', 1);
      expect(Math.abs(settled(engine))).toBeLessThan(unkeyed * 0.3);

      // sourceId 0 clears a binding: the master one leaves bus 2 keyed alone,
      // and clearing that too returns the render to the unkeyed level.
      engine.setMasterSidechain(0, 1, 0);
      expect(Math.abs(settled(engine))).toBeLessThan(unkeyed * 0.3);
      engine.setBusSidechain(2, 0, 0, 0);
      // Replay until the duckers' 80 ms release has run out.
      for (let pass = 0; pass < 8; pass += 1) {
        settled(engine);
      }
      closeRel(settled(engine), unkeyed, 1e-4);
      engine.destroy();
    });

    it('rejects an invalid sidechain source kind', () => {
      const engine = makeKeyedRig();
      expect(() => engine.setBusSidechain(2, 0, 2, 30)).toThrow();
      expect(() => engine.setBusSidechain(2, 0, -1, 30)).toThrow();
      expect(() => engine.setMasterSidechain(0, 2, 30)).toThrow();
      expect(() => engine.setMasterSidechain(0, -1, 30)).toThrow();
      expect(() =>
        engine.setBusSidechain(2, 0, 'lane' as unknown as SidechainSourceKind, 30),
      ).toThrow();
      engine.destroy();
    });
  });

  describe('MIDI clip gain and fade', () => {
    const kMidiDestination = 9;
    const heldNoteEvents = [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 }];

    function renderMidiClip(clip: Record<string, unknown>): Float32Array {
      const engine = new RealtimeEngine(48000, kBlock);
      engine.setBuiltinInstrument({ gain: 0.5 }, kMidiDestination);
      engine.setMidiClips([
        {
          id: 42,
          trackId: kMidiDestination,
          destinationId: kMidiDestination,
          lengthSamples: frames,
          events: heldNoteEvents,
          ...clip,
        },
      ]);
      engine.play();
      const out = new Float32Array(kBlocks * kBlock);
      for (let block = 0; block < kBlocks; block += 1) {
        const [left] = engine.process([new Float32Array(kBlock), new Float32Array(kBlock)]);
        out.set(left, block * kBlock);
      }
      engine.destroy();
      return out;
    }

    it('scales the destination render by clip gain, and omitted gain is unity', () => {
      const unity = renderMidiClip({ gain: 1 });
      const half = renderMidiClip({ gain: 0.5 });
      const omitted = renderMidiClip({});
      let peak = 0;
      for (const v of unity) {
        peak = Math.max(peak, Math.abs(v));
      }
      expect(peak).toBeGreaterThan(0.01);
      for (let i = 0; i < unity.length; i += 1) {
        expect(half[i]).toBeCloseTo(0.5 * unity[i], 5);
        expect(omitted[i]).toBeCloseTo(unity[i], 5);
      }
    });

    it('rejects a bad gain or fade, and an open-ended fade-out', () => {
      const engine = new RealtimeEngine(48000, kBlock);
      const good = {
        id: 1,
        trackId: kMidiDestination,
        destinationId: kMidiDestination,
        lengthSamples: frames,
        events: heldNoteEvents,
      };
      expect(() =>
        engine.setMidiClips([{ ...good, fadeInSamples: 256, fadeOutSamples: 256 }]),
      ).not.toThrow();
      expect(() => engine.setMidiClips([{ ...good, gain: 0 }])).not.toThrow();
      expect(() => engine.setMidiClips([{ ...good, gain: -0.5 }])).toThrow();
      expect(() => engine.setMidiClips([{ ...good, gain: Number.NaN }])).toThrow();
      expect(() => engine.setMidiClips([{ ...good, gain: Number.POSITIVE_INFINITY }])).toThrow();
      expect(() => engine.setMidiClips([{ ...good, fadeInSamples: -1 }])).toThrow();
      expect(() => engine.setMidiClips([{ ...good, fadeOutSamples: -1 }])).toThrow();
      // An open-ended clip (no lengthSamples) has no end to fade out towards; a
      // fade-in is still fine.
      const { lengthSamples: _unused, ...openEnded } = good;
      expect(() => engine.setMidiClips([{ ...openEnded, fadeOutSamples: 256 }])).toThrow();
      expect(() => engine.setMidiClips([{ ...openEnded, fadeInSamples: 256 }])).not.toThrow();
      engine.destroy();
    });
  });

  describe('worklet message guards', () => {
    it('accepts a syncMixer message carrying the new bus/master sidechain fields', () => {
      expect(
        isEngineSyncMessage({
          type: 'syncMixer',
          lanes: [],
          busSidechains: [{ busId: 2, insertIndex: 0, sourceKind: 1, sourceId: 1 }],
          masterSidechains: [{ insertIndex: 0, sourceKind: 0, sourceId: 30 }],
        }),
      ).toBe(true);
    });

    it('rejects a malformed sync message', () => {
      expect(isEngineSyncMessage({ lanes: [] })).toBe(false);
      expect(isEngineSyncMessage({ type: 'syncMixerTypo', lanes: [] })).toBe(false);
      expect(isEngineSyncMessage(null)).toBe(false);
      expect(isEngineSyncMessage(42)).toBe(false);
    });
  });

  describe('worklet syncMixer replay', () => {
    it('replays bus and master sidechain bindings across a syncMixer re-post', async () => {
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine(
        48000,
        128,
      ) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        mode: 'postMessage',
        offlineEngine: offline,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      // A key insert needs to exist on the keyed bus/master strip: neither
      // setter creates one, so setBusSidechain/setMasterSidechain's insert
      // index 0 is out of range without it.
      const oneInsertJson = (id: string): string =>
        JSON.stringify({
          version: 1,
          strips: [],
          buses: [
            {
              id,
              inserts: [
                {
                  slot: 'pre',
                  processor: 'dynamics.limiter',
                  params: JSON.stringify({ thresholdDb: 0, lookaheadMs: 0, releaseMs: 50 }),
                },
              ],
            },
          ],
          connections: [],
        });
      try {
        engine.setTrackBuses([
          { busId: 1, gainDb: 0 },
          { busId: 2, gainDb: 0 },
        ]);
        engine.setBusStripJson(2, oneInsertJson('2'));
        engine.setMasterStripJson(
          JSON.stringify({
            version: 1,
            strips: [
              {
                id: 'master',
                inserts: [
                  {
                    slot: 'pre',
                    processor: 'dynamics.limiter',
                    params: JSON.stringify({ thresholdDb: 0, lookaheadMs: 0, releaseMs: 50 }),
                  },
                ],
              },
            ],
            buses: [],
            connections: [],
          }),
        );
        engine.setBusSidechain(2, 0, 1, 1);
        engine.setMasterSidechain(0, 1, 2);
        // A later routing change re-posts every cached binding via syncMixer.
        engine.setTrackBuses([
          { busId: 1, gainDb: -3 },
          { busId: 2, gainDb: 0 },
        ]);
        const mixerSyncs = posted.filter(
          (message) => (message as { type?: unknown }).type === 'syncMixer',
        );
        const last = mixerSyncs[mixerSyncs.length - 1] as {
          busSidechains?: Array<{
            busId: number;
            insertIndex: number;
            sourceKind: number;
            sourceId: number;
          }>;
          masterSidechains?: Array<{ insertIndex: number; sourceKind: number; sourceId: number }>;
        };
        expect(last.busSidechains).toEqual([
          { busId: 2, insertIndex: 0, sourceKind: 1, sourceId: 1 },
        ]);
        expect(last.masterSidechains).toEqual([{ insertIndex: 0, sourceKind: 1, sourceId: 2 }]);

        // Clearing a binding removes it from the next full replay.
        engine.setMasterSidechain(0, 1, 0);
        engine.setTrackBuses([
          { busId: 1, gainDb: -3 },
          { busId: 2, gainDb: -1 },
        ]);
        const mixerSyncs2 = posted.filter(
          (message) => (message as { type?: unknown }).type === 'syncMixer',
        );
        const last2 = mixerSyncs2[mixerSyncs2.length - 1] as {
          masterSidechains?: unknown[];
        };
        expect(last2.masterSidechains).toEqual([]);
      } finally {
        engine.destroy();
      }
    });
  });
});
