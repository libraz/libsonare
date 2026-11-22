/**
 * Call order between the command channel and the worklet port.
 *
 * Over the SAB ring a command is shared memory the audio thread reads at the next quantum, while
 * a sync or a request is a port message delivered whenever the host gets to it. The worklet sees
 * the calls in the order they were made either way: a port message applies every command sent
 * before it, and a ring command sent after one waits until it has been handled. Every case here
 * runs a live processor behind a port that delivers only when flushed.
 */

import { vi } from 'vitest';
import type { EngineCaptureStatus, EngineTransportState } from '../src/public_types_engine';
import {
  describe,
  expect,
  it,
  SonareEngine,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const BLOCK = 128;
type Mode = 'sab' | 'postMessage';
type Processor = SonareRealtimeEngineWorkletProcessor;

interface LiveEngine {
  transportState(): EngineTransportState;
  captureStatus(): EngineCaptureStatus;
}

/** A SonareEngine whose port queues every message until `flush()` hands it to a live processor. */
async function bridgedEngine(mode: Mode) {
  let processor: Processor | undefined;
  const queued: unknown[] = [];
  const replies: { type?: unknown }[] = [];
  const bridge = { dropReplies: false, failReplies: false };
  const escaped: unknown[] = [];
  const port = {
    postMessage: (message: unknown) => queued.push(message),
    onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
  };
  const engine = await SonareEngine.create({ sampleRate: 48000 } as BaseAudioContext, {
    mode,
    offlineNumChannels: 2,
    maxBlockSize: BLOCK,
    numChannels: 2,
    nodeFactory: (_context, _name, options) => {
      processor = new SonareRealtimeEngineWorkletProcessor({
        ...(options.processorOptions as ConstructorParameters<
          typeof SonareRealtimeEngineWorkletProcessor
        >[0]),
        transport: {
          postMessage: (message) => {
            if (bridge.failReplies) {
              throw new Error('reply port closed');
            }
            replies.push(message as { type?: unknown });
            if (!bridge.dropReplies) {
              port.onmessage?.({ data: message } as MessageEvent<unknown>);
            }
          },
        },
      });
      queueMicrotask(() => port.onmessage?.({ data: { type: 'ready' } } as MessageEvent));
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    },
  });
  if (!processor) {
    throw new Error('expected a worklet processor');
  }
  const live = processor;
  const flush = () => {
    for (const message of queued.splice(0)) {
      // A handler that throws ends its own event, never the delivery of the next message.
      try {
        deliver(message);
      } catch (error) {
        escaped.push(error);
      }
    }
  };
  const deliver = (message: unknown) => {
    const type = (message as { type?: unknown }).type;
    if (typeof type === 'number') {
      live.receiveCommand(message as Parameters<Processor['receiveCommand']>[0]);
    } else if (type === 'captureRequest') {
      live.receiveCaptureRequest(message as Parameters<Processor['receiveCaptureRequest']>[0]);
    } else if (type === 'transportRequest') {
      live.receiveTransportRequest(message as Parameters<Processor['receiveTransportRequest']>[0]);
    } else {
      live.receiveSync(message as Parameters<Processor['receiveSync']>[0]);
    }
  };
  const quantum = () => {
    live.process([[]], [[new Float32Array(BLOCK), new Float32Array(BLOCK)]]);
  };
  const liveEngine = () => (live as unknown as { engine: LiveEngine }).engine;
  const syncErrors = () => replies.filter((reply) => reply.type === 'syncError');
  return { engine, processor: live, bridge, escaped, flush, quantum, liveEngine, syncErrors };
}

describe('the worklet sees SonareEngine calls in call order', () => {
  setupWorklet();

  describe.each<Mode>(['sab', 'postMessage'])('over %s', (mode) => {
    it('replaces the capture buffer right after a disarm', async () => {
      const { engine, flush, quantum, syncErrors } = await bridgedEngine(mode);
      try {
        engine.configureCapture({ capacityFrames: 256 });
        engine.armCapture({ armed: true });
        flush();
        quantum();
        engine.armCapture({ armed: false });
        engine.configureCapture({ capacityFrames: 512 });
        flush();
        expect(syncErrors()).toEqual([]);
      } finally {
        engine.destroy();
      }
    });

    it('answers a capture status with the arm sent before it', async () => {
      const { engine, flush } = await bridgedEngine(mode);
      try {
        engine.configureCapture({ capacityFrames: 256 });
        engine.armCapture({ armed: true });
        const status = engine.captureStatus();
        flush();
        expect((await status).armed).toBe(true);
      } finally {
        engine.destroy();
      }
    });

    it('applies a stop sent after an unanswered capture read in the next quantum', async () => {
      const { engine, bridge, flush, quantum, liveEngine } = await bridgedEngine(mode);
      try {
        engine.configureCapture({ capacityFrames: 256 });
        engine.transport.play();
        flush();
        quantum();
        expect(liveEngine().transportState().playing).toBe(true);
        bridge.dropReplies = true;
        engine.capturedAudio().catch(() => undefined);
        engine.transport.stop();
        flush();
        quantum();
        expect(liveEngine().transportState().playing).toBe(false);
      } finally {
        engine.destroy();
      }
    });

    it('keeps a scheduled stop at its frame across an intervening port message', async () => {
      const { engine, flush, quantum, liveEngine } = await bridgedEngine(mode);
      try {
        engine.transport.play();
        flush();
        quantum();
        const stopFrame = liveEngine().transportState().renderFrame + 2 * BLOCK + 5;
        engine.transport.stop(stopFrame);
        engine.addMarker(0, 'between');
        flush();
        while (liveEngine().transportState().renderFrame + BLOCK <= stopFrame) {
          quantum();
          expect(liveEngine().transportState().playing).toBe(true);
        }
        quantum();
        expect(liveEngine().transportState().playing).toBe(false);
      } finally {
        engine.destroy();
      }
    });
  });

  describe('over the SAB ring', () => {
    it('holds a command sent after a request until the request is handled', async () => {
      const { engine, flush, quantum, liveEngine } = await bridgedEngine('sab');
      try {
        const state = engine.transportState();
        engine.transport.play();
        quantum();
        expect(liveEngine().transportState().playing).toBe(false);
        flush();
        expect((await state).playing).toBe(false);
        quantum();
        expect(liveEngine().transportState().playing).toBe(true);
      } finally {
        engine.destroy();
      }
    });

    // Each handler reports its own refusal, so what escapes one is a reply the port cannot send.
    it('releases the command after a request whose reply could not be posted', async () => {
      const { engine, bridge, escaped, flush, quantum, liveEngine } = await bridgedEngine('sab');
      try {
        engine.transportState().catch(() => undefined);
        engine.transport.play();
        bridge.failReplies = true;
        flush();
        bridge.failReplies = false;
        expect(escaped).toHaveLength(1);
        quantum();
        expect(liveEngine().transportState().playing).toBe(true);
      } finally {
        engine.destroy();
      }
    });

    it('releases the command after a refused sync whose report could not be posted', async () => {
      const { engine, processor, bridge, escaped, flush, quantum, liveEngine } =
        await bridgedEngine('sab');
      try {
        vi.spyOn(
          processor as unknown as { applySync(message: unknown): void },
          'applySync',
        ).mockImplementationOnce(() => {
          throw new Error('sync failed');
        });
        engine.addMarker(0, 'refused');
        engine.transport.play();
        bridge.failReplies = true;
        flush();
        bridge.failReplies = false;
        expect(escaped).toHaveLength(1);
        quantum();
        expect(liveEngine().transportState().playing).toBe(true);
      } finally {
        engine.destroy();
      }
    });
  });
});
