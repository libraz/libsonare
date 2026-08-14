import { describe, expect, it } from 'vitest';
import { SonareRealtimeEngineNode } from '../../dist/worklet.js';
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
