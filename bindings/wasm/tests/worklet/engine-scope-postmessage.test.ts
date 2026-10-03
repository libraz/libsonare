import { beforeAll, describe, expect, it, vi } from 'vitest';
import { init as initSource } from '../../src/index';
import { RealtimeEngine } from '../../src/realtime_engine';
import { SonareRealtimeEngineWorkletProcessor as SourceSonareRealtimeEngineWorkletProcessor } from '../../src/worklet/engine-processor';
import { isScopeSnapshot } from '../../src/worklet/guards';
import type { SonareWorkletScopeSnapshot } from '../../src/worklet/protocol';
import { SonareEngineCommandType as SourceSonareEngineCommandType } from '../../src/worklet/protocol';
import {
  SonareEngineCommandType,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from '../_worklet_helpers';

describe('realtime engine scope postMessage transport', () => {
  setupWorklet();
  beforeAll(async () => initSource());

  it('forwards bounded scope settings and delivers one onScope event', async () => {
    let capturedOptions: AudioWorkletNodeOptions | undefined;
    let port:
      | {
          onmessage?: ((event: MessageEvent<unknown>) => void) | null;
          postMessage(message: unknown): void;
        }
      | undefined;
    const node = await SonareRealtimeEngineNode.create(
      { sampleRate: 48000 } as unknown as BaseAudioContext,
      {
        mode: 'postMessage',
        scopeIntervalFrames: 256,
        scopeBands: 80,
        nodeFactory: (_context, _name, options) => {
          capturedOptions = options;
          port = {
            onmessage: undefined,
            postMessage: () => undefined,
          };
          return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
        },
      },
    );
    try {
      expect(node.scopeRing).toBeUndefined();
      expect(capturedOptions?.processorOptions).toMatchObject({
        scopeIntervalFrames: 256,
        scopeBands: 64,
      });

      const seen: SonareWorkletScopeSnapshot[] = [];
      const unsubscribe = node.onScope((scope) => seen.push(scope));
      const scope: SonareWorkletScopeSnapshot = {
        type: 'scope',
        targetId: 0,
        frame: 256,
        bands: new Float32Array([-12, -18, -24]),
        points: new Float32Array([0.25, 0.25, -0.25, -0.25]),
      };
      port?.onmessage?.({ data: scope } as MessageEvent<unknown>);
      expect(seen).toEqual([scope]);
      expect(node.pollScope()).toEqual([]);
      unsubscribe();
      port?.onmessage?.({ data: scope } as MessageEvent<unknown>);
      expect(seen).toHaveLength(1);
      const lateHandler = port?.onmessage;
      node.destroy();
      lateHandler?.({ data: scope } as MessageEvent<unknown>);
      expect(seen).toHaveLength(1);
    } finally {
      node.destroy();
    }
  });

  it('rejects malformed scope records before they reach listeners', () => {
    const valid: SonareWorkletScopeSnapshot = {
      type: 'scope',
      targetId: 0,
      frame: 0,
      bands: new Float32Array([0]),
      points: new Float32Array([0, 0]),
    };
    expect(isScopeSnapshot(valid)).toBe(true);
    expect(isScopeSnapshot({ ...valid, targetId: Number.NaN })).toBe(false);
    expect(isScopeSnapshot({ ...valid, frame: 1.5 })).toBe(false);
    expect(isScopeSnapshot({ ...valid, bands: new Float32Array(65) })).toBe(false);
    expect(isScopeSnapshot({ ...valid, points: new Float32Array(63) })).toBe(false);
    expect(isScopeSnapshot({ ...valid, bands: new Float32Array([Number.POSITIVE_INFINITY]) })).toBe(
      false,
    );
    expect(
      isScopeSnapshot({
        ...valid,
        bands: new Float32Array(new SharedArrayBuffer(Float32Array.BYTES_PER_ELEMENT)),
      }),
    ).toBe(false);
  });

  it('publishes real native scope records over postMessage with transferred buffers', () => {
    const posted: Array<{ message: unknown; transfer?: Transferable[] }> = [];
    const blockSize = 256;
    const processor = new SonareRealtimeEngineWorkletProcessor(
      {
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
        scopeIntervalFrames: blockSize,
        scopeBands: 32,
      },
      {
        postMessage: (message, transfer) => posted.push({ message, transfer }),
      },
    );
    const control = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: 48000,
      blockSize,
      channelCount: 2,
      scopeIntervalFrames: 0,
    });
    try {
      processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
      control.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
      let phase = 0;
      let audiblePeak = 0;
      for (let block = 0; block < 12; block++) {
        const inL = new Float32Array(blockSize);
        const inR = new Float32Array(blockSize);
        for (let i = 0; i < blockSize; i++) {
          const sample = 0.5 * Math.sin((2 * Math.PI * 1000 * phase) / 48000);
          inL[i] = sample;
          inR[i] = sample;
          phase++;
        }
        const output = [new Float32Array(blockSize), new Float32Array(blockSize)];
        const controlOutput = [new Float32Array(blockSize), new Float32Array(blockSize)];
        expect(processor.process([[inL, inR]], [output])).toBe(true);
        expect(control.process([[inL, inR]], [controlOutput])).toBe(true);
        expect(output).toEqual(controlOutput);
        for (const sample of output[0]) {
          audiblePeak = Math.max(audiblePeak, Math.abs(sample));
        }
      }
      expect(audiblePeak).toBeGreaterThan(0.1);

      const scopes = posted
        .map(({ message }) => message)
        .filter((message): message is SonareWorkletScopeSnapshot => isScopeSnapshot(message));
      expect(scopes.length).toBeGreaterThan(0);
      expect(new Set(scopes.map((scope) => `${scope.targetId}:${scope.frame}`)).size).toBe(
        scopes.length,
      );
      const master = scopes.filter((scope) => scope.targetId === 0).pop();
      expect(master).toBeDefined();
      if (master) {
        expect(master.bands.length).toBe(32);
        expect(master.points.length).toBeGreaterThan(0);
        expect(Math.max(...master.bands.slice(0, 3))).toBeGreaterThan(master.bands[24] + 20);
        expect(master.points.some((sample) => Math.abs(sample) > 0.1)).toBe(true);
        for (let point = 0; point < master.points.length; point += 2) {
          expect(master.points[point]).toBe(master.points[point + 1]);
        }
      }
      expect(scopes.map((scope) => scope.frame)).toEqual(
        scopes.map((scope) => scope.frame).sort((a, b) => a - b),
      );
      const scopePost = posted.find(({ message }) => isScopeSnapshot(message));
      expect(scopePost?.transfer).toHaveLength(2);
      expect(scopePost?.transfer?.every((buffer) => buffer instanceof ArrayBuffer)).toBe(true);
      if (scopePost && isScopeSnapshot(scopePost.message)) {
        expect(scopePost.transfer).toEqual(
          expect.arrayContaining([scopePost.message.bands.buffer, scopePost.message.points.buffer]),
        );
      }
    } finally {
      processor.destroy();
      control.destroy();
    }
  });

  it('does not configure or publish scope when the interval is zero', () => {
    const posted: unknown[] = [];
    const configureScopeTelemetry = vi.spyOn(RealtimeEngine.prototype, 'configureScopeTelemetry');
    const processor = new SourceSonareRealtimeEngineWorkletProcessor(
      { sampleRate: 48000, blockSize: 128, channelCount: 2, scopeIntervalFrames: 0 },
      { postMessage: (message) => posted.push(message) },
    );
    const engine = (processor as unknown as { engine: RealtimeEngine }).engine;
    const popScopeTelemetryToScratch = vi.spyOn(engine, 'popScopeTelemetryToScratch');
    try {
      expect(configureScopeTelemetry).not.toHaveBeenCalled();
      processor.receiveCommand({
        type: SourceSonareEngineCommandType.TransportPlay,
        sampleTime: -1,
      });
      for (let block = 0; block < 8; block++) {
        expect(
          processor.process(
            [[new Float32Array(128).fill(0.25), new Float32Array(128).fill(0.25)]],
            [[new Float32Array(128), new Float32Array(128)]],
          ),
        ).toBe(true);
      }
      expect(popScopeTelemetryToScratch).not.toHaveBeenCalled();
      expect(posted.some((message) => isScopeSnapshot(message))).toBe(false);
    } finally {
      popScopeTelemetryToScratch.mockRestore();
      configureScopeTelemetry.mockRestore();
      processor.destroy();
    }
  });

  it('does not configure or probe scope when no transport is available', () => {
    const configureScopeTelemetry = vi.spyOn(RealtimeEngine.prototype, 'configureScopeTelemetry');
    const processor = new SourceSonareRealtimeEngineWorkletProcessor({
      sampleRate: 48000,
      blockSize: 128,
      channelCount: 2,
      scopeIntervalFrames: 128,
    });
    const engine = (processor as unknown as { engine: RealtimeEngine }).engine;
    const popScopeTelemetryToScratch = vi.spyOn(engine, 'popScopeTelemetryToScratch');
    try {
      expect(configureScopeTelemetry).not.toHaveBeenCalled();
      processor.receiveCommand({
        type: SourceSonareEngineCommandType.TransportPlay,
        sampleTime: -1,
      });
      for (let block = 0; block < 4; block++) {
        expect(
          processor.process(
            [[new Float32Array(128).fill(0.25), new Float32Array(128).fill(0.25)]],
            [[new Float32Array(128), new Float32Array(128)]],
          ),
        ).toBe(true);
      }
      expect(popScopeTelemetryToScratch).not.toHaveBeenCalled();
    } finally {
      popScopeTelemetryToScratch.mockRestore();
      configureScopeTelemetry.mockRestore();
      processor.destroy();
    }
  });
});
