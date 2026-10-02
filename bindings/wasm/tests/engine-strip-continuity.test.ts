/**
 * Mixer strip regressions against the real WASM RealtimeEngine.
 *
 * A scalar strip edit is allowed to change fader/pan/width state, but it must
 * not reconstruct an equal insert chain.  Reconstructing a delay/reverb loses
 * the feedback tail, and reconstructing an insert while a by-id ramp is live
 * snaps the parameter back to its construction value.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';
import type {
  EngineBus,
  EngineTrackLane,
  EngineTrackSend,
  RealtimeEngine as RealtimeEngineType,
} from '../src/realtime_engine';
import type { StripJsonTarget } from '../src/worklet/engine-mixer-facade';
import {
  decodedInsertSignature,
  type InsertParamOverrideMap,
  type EngineMixerContext as MixerContext,
  syncMixer as syncMixerFacade,
} from '../src/worklet/engine-mixer-facade';
import { type EngineStripContext, setTrackStripJson } from '../src/worklet/engine-strips';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const TRACK_ID = 10;

type Fixture = {
  engine: RealtimeEngineType;
  scene: string;
  editedScene: string;
  mixer: MixerContext;
  strips: EngineStripContext;
  resetCount: () => number;
};

function stripScene(width: number, legacyParams = false, includeTail = true): string {
  const params = {
    decaySec: 1.2,
    dryWet: 0.75,
  };
  const gain = { levelDb: 0 };
  const inserts = includeTail
    ? legacyParams
      ? [
          {
            slot: 'post',
            processor: 'effects.reverb.fdn',
            params_json: '{ "dryWet": 0.750000, "decaySec": 1.200000 }',
          },
          { processor_name: 'utility.gain', params_json: '{"levelDb":0.000000}' },
        ]
      : [
          { slot: 'post', processor: 'effects.reverb.fdn', params },
          { slot: 'pre', processor: 'utility.gain', params: gain },
        ]
    : legacyParams
      ? [{ processor_name: 'utility.gain', params_json: '{"levelDb":0.000000}' }]
      : [{ slot: 'pre', processor: 'utility.gain', params: gain }];
  return JSON.stringify({
    version: 1,
    strips: [{ id: 'track-10', width, inserts }],
    buses: [],
    connections: [],
  });
}

function makeSource(constant = false, symmetric = false): [Float32Array, Float32Array] {
  const frames = BLOCK_SIZE * 48;
  const left = new Float32Array(frames);
  const right = new Float32Array(frames);
  // An asymmetric impulse makes the width stage observable while the FDN
  // feedback makes a scalar edit's state loss observable well after the edit.
  left[0] = 1;
  right[0] = symmetric ? 1 : 0.25;
  if (constant) {
    left.fill(1);
    right.fill(symmetric ? 1 : 0.25);
  }
  return [left, right];
}

function makeFixture(constantSource = false, includeTail = true, symmetricSource = false): Fixture {
  const scene = stripScene(1, false, includeTail);
  const editedScene = stripScene(1.25, true, includeTail);
  // The checked-in dist can lag the source declarations between a core and JS
  // build.  The focused runtime build supplies the complete source surface.
  const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE) as unknown as RealtimeEngineType;
  const [left, right] = makeSource(constantSource, symmetricSource);
  engine.setTrackLanes([{ trackId: TRACK_ID }]);
  engine.setTrackStripJson(TRACK_ID, scene);
  engine.setClips([
    {
      id: 1,
      trackId: TRACK_ID,
      channels: [left, right],
      startPpq: 0,
      lengthSamples: left.length,
    },
  ]);
  engine.play();

  const trackLaneIds = [TRACK_ID];
  const trackSends = new Map<number, EngineTrackSend[]>();
  const trackOutputBus = new Map<number, number>();
  const trackSourceChannelLayout = new Map<number, number>();
  const laneSidechains = new Map<
    string,
    { trackId: number; insertIndex: number; sourceTrackId: number }
  >();
  const busSidechains = new Map<
    string,
    { busId: number; insertIndex: number; sourceKind: number; sourceId: number }
  >();
  const masterSidechains = new Map<
    number,
    { insertIndex: number; sourceKind: number; sourceId: number }
  >();
  const buses: EngineBus[] = [];
  const trackStripJson = new Map([[TRACK_ID, scene]]);
  const busStripJson = new Map<number, string>();
  const insertParamOverrides: InsertParamOverrideMap = new Map();
  let resetCount = 0;
  let mixer!: MixerContext;

  const ensureTrackLane = (target: string | number): number => {
    const trackId = typeof target === 'number' ? target : Number(target);
    const existing = trackLaneIds.indexOf(trackId);
    if (existing >= 0) {
      return existing;
    }
    trackLaneIds.push(trackId);
    engine.setTrackLanes(trackLaneIds);
    return trackLaneIds.length - 1;
  };
  const clearInsertAutomationLanes = (_target: StripJsonTarget): void => {
    resetCount += 1;
  };

  mixer = {
    offlineEngine: engine,
    trackLaneIds,
    trackSends,
    trackOutputBus,
    trackSourceChannelLayout,
    laneSidechains,
    busSidechains,
    masterSidechains,
    buses,
    trackStripJson,
    busStripJson,
    insertParamOverrides,
    clearInsertAutomationLanes,
    flushOfflineMirror: () => engine.applyCommandsDueNowPreservingFuture(),
    postSync: () => undefined,
    ensureTrackLane,
    ensureBus: () => 0,
    mixerLanes: (): EngineTrackLane[] => trackLaneIds.map((trackId) => ({ trackId })),
    syncMixer: (insertBaseResets, oneShotInsertParamOverrides) =>
      syncMixerFacade(mixer, false, insertBaseResets, oneShotInsertParamOverrides),
    sendSmoothedParam: (paramId, value) => {
      engine.setParameterSmoothed(paramId, value);
      return true;
    },
    getMasterStripJson: () => undefined,
    cacheMasterStripJson: () => undefined,
  };

  const strips: EngineStripContext = {
    offlineEngine: engine,
    trackLaneIds,
    postSync: () => undefined,
    postInstrumentSync: () => undefined,
    ensureTrackLane,
    resolveTargetId: (target) => (typeof target === 'number' ? target : Number(target)),
    readStripJson: (target) =>
      target.kind === 'track' ? trackStripJson.get(target.trackId) : undefined,
    writeStripJson: (target, sceneJson) => {
      if (target.kind === 'track') {
        trackStripJson.set(target.trackId, sceneJson);
      }
    },
    insertParamOverrides,
    clearInsertAutomationLanes,
  };

  return {
    engine,
    scene,
    editedScene,
    mixer,
    strips,
    resetCount: () => resetCount,
  };
}

function processBlock(engine: RealtimeEngineType): [Float32Array, Float32Array] {
  const output = engine.process([new Float32Array(BLOCK_SIZE), new Float32Array(BLOCK_SIZE)]);
  return [new Float32Array(output[0]), new Float32Array(output[1] ?? output[0])];
}

function energy(channel: Float32Array): number {
  let sum = 0;
  for (const sample of channel) {
    sum += sample * sample;
  }
  return sum;
}

function maxDifference(left: Float32Array, right: Float32Array): number {
  let maximum = 0;
  for (let i = 0; i < Math.min(left.length, right.length); i += 1) {
    maximum = Math.max(maximum, Math.abs(left[i] - right[i]));
  }
  return maximum;
}

function rms(channel: Float32Array): number {
  return Math.sqrt(energy(channel) / channel.length);
}

describe('strip continuity (real WASM engine)', () => {
  beforeAll(async () => {
    await init();
  });

  it('retains a real reverb tail across a scalar width edit and preserves pre/post identity', () => {
    const edited = makeFixture();
    const control = makeFixture();
    try {
      expect(decodedInsertSignature(edited.scene, { kind: 'track', trackId: TRACK_ID })).toBe(
        decodedInsertSignature(edited.editedScene, { kind: 'track', trackId: TRACK_ID }),
      );

      const editedBlocks: Array<[Float32Array, Float32Array]> = [];
      const controlBlocks: Array<[Float32Array, Float32Array]> = [];
      for (let block = 0; block < 36; block += 1) {
        if (block === 4) {
          setTrackStripJson(
            edited.strips,
            TRACK_ID,
            edited.editedScene,
            edited.mixer.trackStripJson,
          );
          syncMixerFacade(edited.mixer);
        }
        editedBlocks.push(processBlock(edited.engine));
        controlBlocks.push(processBlock(control.engine));
      }

      const editedTail = editedBlocks.slice(12).reduce((sum, block) => sum + energy(block[0]), 0);
      const controlTail = controlBlocks.slice(12).reduce((sum, block) => sum + energy(block[0]), 0);
      expect(controlTail).toBeGreaterThan(1e-8);
      // A scalar scene edit must leave the stateful feedback tail alive.  A
      // rebuilt chain loses the impulse and is many orders of magnitude lower.
      expect(editedTail).toBeGreaterThan(controlTail * 0.2);
      expect(edited.resetCount()).toBe(0);
    } finally {
      edited.engine.destroy();
      control.engine.destroy();
    }
  });

  it('keeps a live utility.gain ramp running through the same scalar sync path', () => {
    const edited = makeFixture(true, false, true);
    const control = makeFixture(true, false, true);
    try {
      const gainId = edited.engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'levelDb');
      expect(gainId).toBeGreaterThan(0);
      edited.engine.setParameterSmoothed(gainId, -12);
      control.engine.setParameterSmoothed(gainId, -12);

      const editedBlocks: Array<[Float32Array, Float32Array]> = [];
      const controlBlocks: Array<[Float32Array, Float32Array]> = [];
      for (let block = 0; block < 8; block += 1) {
        if (block === 2) {
          setTrackStripJson(
            edited.strips,
            TRACK_ID,
            edited.editedScene,
            edited.mixer.trackStripJson,
          );
          syncMixerFacade(edited.mixer);
          // Symmetric constant input is invariant under the width edit. Leave
          // the control untouched so native routing resets cannot affect both
          // engines and hide a discontinuity.
        }
        editedBlocks.push(processBlock(edited.engine));
        controlBlocks.push(processBlock(control.engine));
      }

      expect(edited.resetCount()).toBe(0);
      expect(maxDifference(editedBlocks[2][0], controlBlocks[2][0])).toBeLessThan(1e-4);
      expect(rms(editedBlocks[2][0])).toBeGreaterThan(rms(editedBlocks[1][0]) * 0.5);
      expect(rms(editedBlocks[2][0])).toBeLessThan(rms(editedBlocks[0][0] ?? editedBlocks[2][0]));
    } finally {
      edited.engine.destroy();
      control.engine.destroy();
    }
  });
});
