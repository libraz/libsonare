import { describe, expect, it } from 'vitest';
import { SonareRealtimeEngineNode } from '../../dist/worklet.js';
import { buildCaptureConfig } from '../../src/worklet/engine-offline';
import {
  isEngineCaptureResponseForOperation,
  isEngineCaptureResponseMessage,
} from '../../src/worklet/guards';

const status = {
  capturedFrames: 128,
  overflowCount: 0,
  armed: true,
  punchEnabled: false,
  source: 'input' as const,
  recordOffsetSamples: -12,
};

function validResponse(response: unknown): boolean {
  return isEngineCaptureResponseMessage(response);
}

function matchesOperation(response: unknown, op: 'status' | 'read' | 'reset'): boolean {
  return isEngineCaptureResponseMessage(response)
    ? isEngineCaptureResponseForOperation(response, op)
    : false;
}

describe('worklet capture response protocol', () => {
  it('accepts only strict typed-array channel responses', () => {
    expect(
      validResponse({
        type: 'captureResponse',
        requestId: 1,
        ok: true,
        channels: [new Float32Array([0.25])],
      }),
    ).toBe(true);
    expect(
      validResponse({
        type: 'captureResponse',
        requestId: 2,
        ok: true,
        channels: [[0.25]],
      }),
    ).toBe(false);
    expect(
      validResponse({
        type: 'captureResponse',
        requestId: 3,
        ok: true,
        status,
        channels: [new Float32Array([0.25])],
      }),
    ).toBe(false);
  });

  it('matches strict success variants to their requested operation', () => {
    const statusResponse = { type: 'captureResponse', requestId: 1, ok: true, status };
    const readResponse = {
      type: 'captureResponse',
      requestId: 2,
      ok: true,
      channels: [new Float32Array([0.25])],
    };
    const resetResponse = { type: 'captureResponse', requestId: 3, ok: true };
    const failureResponse = {
      type: 'captureResponse',
      requestId: 4,
      ok: false,
      error: 'capture failed',
    };

    expect(matchesOperation(statusResponse, 'status')).toBe(true);
    expect(matchesOperation(statusResponse, 'read')).toBe(false);
    expect(matchesOperation(readResponse, 'read')).toBe(true);
    expect(matchesOperation(readResponse, 'reset')).toBe(false);
    expect(matchesOperation(resetResponse, 'reset')).toBe(true);
    expect(matchesOperation(resetResponse, 'status')).toBe(false);
    for (const op of ['status', 'read', 'reset'] as const) {
      expect(matchesOperation(failureResponse, op)).toBe(true);
    }
  });

  it('rejects a mismatched response and does not reuse the cleaned pending entry', async () => {
    const posted: unknown[] = [];
    const port = {
      onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      postMessage(message: unknown) {
        posted.push(message);
      },
    };
    const node = await SonareRealtimeEngineNode.create(
      { sampleRate: 48000 } as unknown as BaseAudioContext,
      {
        mode: 'postMessage',
        engineAbiVersion: 1,
        nodeFactory: () => ({ port, disconnect: () => undefined }) as unknown as AudioWorkletNode,
      },
    );
    try {
      const pending = node.requestCapturedAudio();
      const request = posted.at(-1) as { requestId: number; op: string };
      expect(request.op).toBe('read');
      port.onmessage?.({
        data: {
          type: 'captureResponse',
          requestId: request.requestId,
          ok: true,
          status,
        },
      } as MessageEvent<unknown>);
      await expect(pending).rejects.toThrow('does not match request operation');

      // A second response with the same id is ignored after the mismatch was
      // rejected and removed from the pending map.
      port.onmessage?.({
        data: {
          type: 'captureResponse',
          requestId: request.requestId,
          ok: true,
          channels: [new Float32Array([0.25])],
        },
      } as MessageEvent<unknown>);

      const resetPending = node.requestCaptureReset();
      const resetRequest = posted.at(-1) as { requestId: number; op: string };
      expect(resetRequest.op).toBe('reset');
      port.onmessage?.({
        data: {
          type: 'captureResponse',
          requestId: resetRequest.requestId,
          ok: true,
          channels: [new Float32Array([0.25])],
        },
      } as MessageEvent<unknown>);
      await expect(resetPending).rejects.toThrow('does not match request operation');

      // The mismatched reset response also clears its pending entry.
      port.onmessage?.({
        data: {
          type: 'captureResponse',
          requestId: resetRequest.requestId,
          ok: true,
        },
      } as MessageEvent<unknown>);
    } finally {
      node.destroy();
    }
  });
});

describe('buildCaptureConfig', () => {
  const base = { bufferFrames: 1024 };

  it('carries the resolved counts through rather than a rounded stand-in', () => {
    expect(buildCaptureConfig({ ...base, channels: 2 }, 4).channels).toBe(2);
    expect(buildCaptureConfig({ ...base, channels: 3 }, 4).channels).toBe(3);
    // An absent channel count falls back to the engine's own.
    expect(buildCaptureConfig(base, 4).channels).toBe(4);
    expect(buildCaptureConfig({ bufferFrames: 512 }, 2).bufferFrames).toBe(512);
    expect(buildCaptureConfig({ bufferFrames: 2048 }, 2).bufferFrames).toBe(2048);
    expect(buildCaptureConfig({ ...base, recordOffsetSamples: -12 }, 2).recordOffsetSamples).toBe(
      -12,
    );
    expect(buildCaptureConfig({ ...base, recordOffsetSamples: 24 }, 2).recordOffsetSamples).toBe(
      24,
    );
  });

  it.each([1024.5, 0, -1, Number.NaN, Number.POSITIVE_INFINITY])(
    'refuses a bufferFrames of %p',
    (bufferFrames) => {
      expect(() => buildCaptureConfig({ bufferFrames }, 2)).toThrow(RangeError);
      expect(() => buildCaptureConfig({ bufferFrames }, 2)).toThrow(
        /bufferFrames must be an integer of at least 1/,
      );
    },
  );

  it('refuses an absent bufferFrames, which has no default', () => {
    expect(() => buildCaptureConfig({ bufferFrames: undefined as unknown as number }, 2)).toThrow(
      /bufferFrames must be an integer of at least 1/,
    );
  });

  it.each([2.5, 0, -2, Number.NaN])('refuses a capture channel count of %p', (channels) => {
    expect(() => buildCaptureConfig({ ...base, channels }, 2)).toThrow(
      /channelCount must be an integer of at least 1/,
    );
  });

  it.each([-12.5, Number.NaN, Number.NEGATIVE_INFINITY])(
    'refuses a recordOffsetSamples of %p while keeping both signs legal',
    (recordOffsetSamples) => {
      expect(() => buildCaptureConfig({ ...base, recordOffsetSamples }, 2)).toThrow(
        /recordOffsetSamples must be an integer/,
      );
    },
  );
});
