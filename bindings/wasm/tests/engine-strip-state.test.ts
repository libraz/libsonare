import { describe, expect, it } from 'vitest';
import type { RealtimeEngine } from '../src/index';
import { SonareEngine } from '../src/worklet/engine';
import {
  applyFullStripJson,
  decodedInsertSignature,
  type EngineMixerContext,
  type StripJsonTarget,
  setBusStripJson,
  setMasterStripJson,
  syncMixer,
} from '../src/worklet/engine-mixer-facade';
import { type EngineStripContext, setTrackStripJson } from '../src/worklet/engine-strips';

const trackTarget: StripJsonTarget = { kind: 'track', trackId: 10 };
const busTarget: StripJsonTarget = { kind: 'bus', busId: 20 };
const masterTarget: StripJsonTarget = { kind: 'master' };

function scene(inserts: unknown[], extra: Record<string, unknown> = {}): string {
  return JSON.stringify({
    version: 1,
    strips: [{ id: 'track-10', inserts, ...extra }],
    buses: [],
    connections: [],
  });
}

function busScene(inserts: unknown[]): string {
  return JSON.stringify({
    version: 1,
    strips: [],
    buses: [{ id: 'bus-20', inserts }],
    connections: [],
  });
}

function masterScene(inserts: unknown[]): string {
  return JSON.stringify({
    version: 1,
    strips: [{ id: 'master', inserts }],
    buses: [],
    connections: [],
  });
}

function fakeEngine(calls: string[]): RealtimeEngine {
  return {
    applyCommandsDueNowPreservingFuture: () => undefined,
    setTrackStripJson: (_trackId: number, json: string) => calls.push(json),
    setBusStripJson: (_busId: number, json: string) => calls.push(json),
    setMasterStripJson: (json: string) => calls.push(json),
    setTrackBuses: () => calls.push('buses'),
    setTrackLanes: () => calls.push('lanes'),
    settleInsertParameters: () => calls.push('settle'),
    clearTrackInsertParameterBases: () => undefined,
    clearBusInsertParameterBases: () => undefined,
    clearMasterInsertParameterBases: () => undefined,
    resolveTrackInsertAutomationId: () => 101,
    resolveBusInsertAutomationId: () => 102,
    resolveMasterInsertAutomationId: () => 103,
    insertParameterConstructedValue: (id: number) => id / 100,
    restoreTrackStripInsertParamByName: () => undefined,
    restoreBusStripInsertParamByName: () => undefined,
    restoreMasterStripInsertParamByName: () => undefined,
  } as unknown as RealtimeEngine;
}

function fakeStripContext(
  engine: RealtimeEngine,
  trackStripJson: Map<number, string>,
): EngineStripContext {
  return {
    offlineEngine: engine,
    trackLaneIds: [10],
    postSync: () => undefined,
    postInstrumentSync: () => undefined,
    ensureTrackLane: () => 0,
    resolveTargetId: (target) => (typeof target === 'number' ? target : Number(target)),
    readStripJson: (target) =>
      target.kind === 'track' ? trackStripJson.get(target.trackId) : undefined,
    writeStripJson: (target, json) => {
      if (target.kind === 'track') {
        trackStripJson.set(target.trackId, json);
      }
    },
    insertParamOverrides: new Map(),
    clearInsertAutomationLanes: () => undefined,
  };
}

function fakeMixerContext(
  engine: RealtimeEngine,
  busStripJson: Map<number, string>,
  masterStripJson: string | undefined,
  syncCalls: unknown[][],
  trackLaneIds: number[] = [],
): EngineMixerContext {
  const buses = [{ busId: 20, gainDb: 0 }];
  return {
    offlineEngine: engine,
    trackLaneIds,
    trackSends: new Map(),
    trackOutputBus: new Map(),
    trackSourceChannelLayout: new Map(),
    laneSidechains: new Map(),
    busSidechains: new Map(),
    masterSidechains: new Map(),
    buses,
    trackStripJson: new Map(),
    busStripJson,
    insertParamOverrides: new Map(),
    clearInsertAutomationLanes: () => undefined,
    flushOfflineMirror: () => undefined,
    postSync: () => undefined,
    ensureTrackLane: () => 0,
    ensureBus: () => 0,
    mixerLanes: () => [],
    syncMixer: (...args: unknown[]) => syncCalls.push(args),
    sendSmoothedParam: () => true,
    getMasterStripJson: () => masterStripJson,
    cacheMasterStripJson: () => undefined,
  } as unknown as EngineMixerContext;
}

function fakeAudioContext(): BaseAudioContext {
  return { sampleRate: 48000 } as unknown as BaseAudioContext;
}

function readyNode(posted: unknown[]): AudioWorkletNode {
  const port = {
    onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
    postMessage: (message: unknown) => posted.push(message),
  };
  queueMicrotask(() => {
    port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
  });
  return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
}

describe('strip state retention', () => {
  it('uses the native insert identity fields, independent of scalar JSON', () => {
    const before = scene([
      { slot: 'post', processor: 'delay', params: { timeMs: 240, feedback: 0.4 } },
      { processor: 'eq.parametric', params: { bands: [{ frequencyHz: 100 }] } },
    ]);
    const after = scene(
      [
        { processor_name: 'eq.parametric', params_json: '{"bands":[{"frequencyHz":100}]}' },
        { slot: 'post', processor: 'delay', params: { feedback: 0.4, timeMs: 240 } },
      ],
      { width: 1.25, faderDb: -3 },
    );
    expect(decodedInsertSignature(before, trackTarget)).toBe(
      decodedInsertSignature(after, trackTarget),
    );
  });

  it('keeps absent params distinct from an explicit empty params object', () => {
    const absent = scene([{ processor: 'delay' }]);
    const explicitEmpty = scene([{ processor: 'delay', params: {} }]);
    const legacyEmpty = scene([{ processor: 'delay', params_json: '' }]);
    expect(decodedInsertSignature(absent, trackTarget)).not.toBe(
      decodedInsertSignature(explicitEmpty, trackTarget),
    );
    expect(decodedInsertSignature(absent, trackTarget)).toBe(
      decodedInsertSignature(legacyEmpty, trackTarget),
    );
  });

  it('matches numeric params across object and legacy JSON representations', () => {
    const objectParams = scene([{ processor: 'delay', params: { feedback: 0.1 } }]);
    const legacyParams = scene([
      { processor: 'delay', params_json: '{ "feedback": 0.10000000000000001 }' },
    ]);
    expect(decodedInsertSignature(objectParams, trackTarget)).toBe(
      decodedInsertSignature(legacyParams, trackTarget),
    );
  });

  it('uses pre then post ordering for track and master, while buses keep scene order', () => {
    const pre = { processor: 'pre' };
    const post = { slot: 'post', processor: 'post' };
    expect(decodedInsertSignature(scene([post, pre]), trackTarget)).toBe(
      decodedInsertSignature(scene([pre, post]), trackTarget),
    );
    expect(decodedInsertSignature(masterScene([post, pre]), masterTarget)).toBe(
      decodedInsertSignature(masterScene([pre, post]), masterTarget),
    );
    expect(decodedInsertSignature(busScene([post, pre]), busTarget)).not.toBe(
      decodedInsertSignature(busScene([pre, post]), busTarget),
    );
  });

  it('matches scene_from_json by ignoring non-object strip and insert entries', () => {
    const withNoise = JSON.stringify({
      version: 1,
      strips: [null, { id: 'track-10', inserts: [null, { processor: 'delay' }, []] }],
      buses: [],
      connections: [],
    });
    expect(decodedInsertSignature(withNoise, trackTarget)).toBe(
      decodedInsertSignature(scene([{ processor: 'delay' }]), trackTarget),
    );
  });

  it('does not settle active insert ramps during a scalar mixer sync', () => {
    const calls: string[] = [];
    const syncCalls: unknown[][] = [];
    const ctx = fakeMixerContext(fakeEngine(calls), new Map(), undefined, syncCalls, [10]);

    syncMixer(ctx);

    expect(calls).toContain('lanes');
    expect(calls).not.toContain('settle');
  });

  it('applies one candidate scene exactly once even when the old chain changed', () => {
    const calls: string[] = [];
    const engine = fakeEngine(calls);
    applyFullStripJson(engine, trackTarget, scene([{ processor: 'new' }]));
    expect(calls).toHaveLength(1);
  });

  it('does not treat a scalar-only scene edit as an insert reset', () => {
    const calls: string[] = [];
    const oldJson = scene([{ processor: 'delay', params: { timeMs: 240 } }]);
    const newJson = scene([{ processor: 'delay', params: { timeMs: 240 } }], { width: 1.1 });
    const trackStripJson = new Map([[10, oldJson]]);
    const result = setTrackStripJson(
      fakeStripContext(fakeEngine(calls), trackStripJson),
      10,
      newJson,
      trackStripJson,
    );
    expect(result.insertBaseResets).toEqual([]);
    expect(calls).toEqual([newJson]);
  });

  it('restores a same-chain manual edit from the immutable construction value once', () => {
    const calls: string[] = [];
    const restoreCalls: unknown[][] = [];
    const oldJson = scene([{ processor: 'delay', params: { timeMs: 240 } }]);
    const trackStripJson = new Map([[10, oldJson]]);
    const engine = {
      ...fakeEngine(calls),
      restoreTrackStripInsertParamByName: (...args: unknown[]) => restoreCalls.push(args),
    } as unknown as RealtimeEngine;
    const ctx = fakeStripContext(engine, trackStripJson);
    ctx.insertParamOverrides.set(JSON.stringify(['track', 10, 0, 'timeMs']), {
      target: trackTarget,
      insertIndex: 0,
      paramName: 'timeMs',
      value: 31,
    });

    const result = setTrackStripJson(ctx, 10, oldJson, trackStripJson);

    expect(result.insertBaseResets).toEqual([trackTarget]);
    expect(result.constructionOverrides).toEqual([
      { target: trackTarget, insertIndex: 0, paramName: 'timeMs', value: 1.01 },
    ]);
    expect(restoreCalls).toEqual([[10, 0, 'timeMs', 1.01]]);
    expect(ctx.insertParamOverrides.size).toBe(0);
    expect(calls).toEqual([oldJson]);
  });

  it('applies equal bus and master chains once when only scalar JSON changes', () => {
    const busCalls: string[] = [];
    const busJson = busScene([{ processor: 'delay', params: { timeMs: 240 } }]);
    const busCache = new Map([[20, busJson]]);
    const busSync: unknown[][] = [];
    const busCtx = fakeMixerContext(fakeEngine(busCalls), busCache, undefined, busSync);
    const busNext = JSON.stringify({
      version: 1,
      strips: [],
      buses: [
        { id: 'bus-20', width: 1.2, inserts: [{ processor: 'delay', params: { timeMs: 240 } }] },
      ],
      connections: [],
    });
    setBusStripJson(busCtx, 20, busNext);
    expect(busCalls).toEqual([busNext]);
    expect(busSync).toEqual([[[], []]]);

    const masterCalls: string[] = [];
    const masterJson = masterScene([{ processor: 'delay', params: { timeMs: 240 } }]);
    const masterSync: unknown[][] = [];
    const masterCtx = fakeMixerContext(fakeEngine(masterCalls), new Map(), masterJson, masterSync);
    const masterNext = JSON.stringify({
      version: 1,
      strips: [
        { id: 'master', faderDb: -3, inserts: [{ processor: 'delay', params: { timeMs: 240 } }] },
      ],
      buses: [],
      connections: [],
    });
    setMasterStripJson(masterCtx, masterNext);
    expect(masterCalls).toEqual([masterNext]);
    expect(masterSync).toEqual([[[], []]]);
  });

  it('forwards the integrated-loudness reset to the offline and live mirrors', async () => {
    const offlineFrames: number[] = [];
    const posted: unknown[] = [];
    const offlineEngine = {
      parameterCount: () => 0,
      applyCommandsDueNowPreservingFuture: () => undefined,
      resetMasterLoudnessMeter: (renderFrame: number) => offlineFrames.push(renderFrame),
      destroy: () => undefined,
    } as unknown as RealtimeEngine;
    const engine = await SonareEngine.create(fakeAudioContext(), {
      mode: 'postMessage',
      engineAbiVersion: 1,
      offlineEngine,
      nodeFactory: () => readyNode(posted),
    });
    try {
      expect(engine.resetMasterLoudnessMeter(123)).toBe(true);
      expect(offlineFrames).toEqual([123]);
      expect(
        posted.some(
          (message) =>
            typeof message === 'object' &&
            message !== null &&
            'sampleTime' in message &&
            (message as { sampleTime: number }).sampleTime === 123,
        ),
      ).toBe(true);
    } finally {
      engine.destroy();
    }
  });

  it('rejects malformed scene text before declaring a lane or bus', async () => {
    const posted: unknown[] = [];
    const offlineEngine = {
      parameterCount: () => 0,
      destroy: () => undefined,
    } as unknown as RealtimeEngine;
    const engine = await SonareEngine.create(fakeAudioContext(), {
      mode: 'postMessage',
      engineAbiVersion: 1,
      offlineEngine,
      nodeFactory: () => readyNode(posted),
    });
    try {
      const before = posted.length;
      expect(() => engine.setTrackStripJson(10, '{')).toThrow();
      expect(() => engine.setBusStripJson(20, '{')).toThrow();
      const routing = engine as unknown as { trackLaneIds: number[]; buses: unknown[] };
      expect(posted.length).toBe(before);
      expect(routing.trackLaneIds).toEqual([]);
      expect(routing.buses).toEqual([]);
    } finally {
      engine.destroy();
    }
  });
});
