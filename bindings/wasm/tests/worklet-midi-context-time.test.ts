/**
 * Live MIDI input placed on the AudioContext clock: the processor maps `contextTimeSec`
 * through the last quantum head's render frame and context frame, so a timed event
 * renders exactly as the same event pushed with its block offset in the block it lands
 * in. A time already rendered plays at the next block's start and is counted, and the
 * SonareEngine facade refuses an offset given together with a context time.
 */

import { afterEach, describe, expect, it, vi } from 'vitest';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import {
  SonareEngine,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const SAMPLE_RATE = 48000;
const BLOCK = 128;
const NOTE_ON = 'syncMidiInputNoteOn' as const;

interface LiveEngine {
  setSynthInstrument: (destinationId: number, config: Record<string, unknown>) => void;
  seekSample: (sample: number) => void;
  resetProcessorState: () => void;
  midiInputPendingCount: number;
}

/** A processor with a synth on destination 0 fed by the live input source, rendering from `contextStartFrame`. */
function liveProcessor(contextStartFrame: number) {
  const replies: Array<Record<string, unknown>> = [];
  const processor = new SonareRealtimeEngineWorkletProcessor({
    sampleRate: SAMPLE_RATE,
    maxBlockSize: BLOCK,
    numChannels: 2,
    transport: { postMessage: (reply) => replies.push(reply as Record<string, unknown>) },
  });
  const engine = (processor as unknown as { engine: LiveEngine }).engine;
  engine.setSynthInstrument(0, {});
  processor.receiveSync({ type: 'syncMidiInputSource', destinationId: 0 });
  let contextFrame = contextStartFrame;
  const scope = globalThis as { currentFrame?: number };
  scope.currentFrame = contextFrame;
  const audio: number[] = [];
  const render = (blocks: number) => {
    for (let block = 0; block < blocks; block++) {
      scope.currentFrame = contextFrame;
      const output = [new Float32Array(BLOCK), new Float32Array(BLOCK)];
      processor.process([[new Float32Array(BLOCK), new Float32Array(BLOCK)]], [output]);
      audio.push(...output[0]);
      contextFrame += BLOCK;
    }
  };
  const noteAt = (contextTimeSec: number) =>
    processor.receiveSync({
      type: NOTE_ON,
      group: 0,
      channel: 0,
      data0: 60,
      data1: 100,
      offsetInBlockSamples: 0,
      contextTimeSec,
    });
  const noteAtOffset = (offsetInBlockSamples: number) =>
    processor.receiveSync({
      type: NOTE_ON,
      group: 0,
      channel: 0,
      data0: 60,
      data1: 100,
      offsetInBlockSamples,
    });
  const lateCount = () => {
    processor.receiveReadRequest({
      type: 'engineReadRequest',
      requestId: replies.length,
      op: 'lateMidiInputCount',
      destinationId: 0,
      channel: 0,
    });
    return (replies.at(-1) as { value: number }).value;
  };
  return { processor, engine, audio, render, noteAt, noteAtOffset, lateCount };
}

const firstSound = (audio: readonly number[]) => audio.findIndex((sample) => sample !== 0);

describe('live MIDI input at an AudioContext time', () => {
  setupWorklet();
  afterEach(() => {
    delete (globalThis as { currentFrame?: number }).currentFrame;
  });

  it('lands a timed event on the frame the offset push gives it in its own block', () => {
    const contextStart = 1000;
    const timed = liveProcessor(contextStart);
    const reference = liveProcessor(0);
    try {
      timed.render(1);
      // Sent between quanta, two blocks before it is due, 37 frames into its block.
      timed.noteAt((contextStart + 3 * BLOCK + 37) / SAMPLE_RATE);
      expect(timed.engine.midiInputPendingCount).toBe(1);
      timed.render(5);

      reference.render(3);
      reference.noteAtOffset(37);
      reference.render(3);

      expect(firstSound(reference.audio)).toBeGreaterThanOrEqual(3 * BLOCK + 37);
      expect(timed.audio).toEqual(reference.audio);
      expect(timed.lateCount()).toBe(0);
    } finally {
      timed.processor.destroy();
      reference.processor.destroy();
    }
  });

  it('reads both clocks as they stand before the first quantum', () => {
    const timed = liveProcessor(500);
    // Sent before any quantum; the scope's currentFrame is the context clock it reads.
    timed.noteAt((500 + 37) / SAMPLE_RATE);
    const reference = liveProcessor(0);
    try {
      reference.noteAtOffset(37);
      reference.render(2);
      timed.render(2);
      expect(timed.audio).toEqual(reference.audio);
    } finally {
      timed.processor.destroy();
      reference.processor.destroy();
    }
  });

  it('plays a time already rendered at the next block start and counts it', () => {
    const timed = liveProcessor(0);
    const reference = liveProcessor(0);
    try {
      timed.render(2);
      timed.noteAt((BLOCK + 5) / SAMPLE_RATE);
      timed.noteAt(0);
      timed.render(2);

      reference.render(2);
      reference.noteAtOffset(0);
      reference.noteAtOffset(0);
      reference.render(2);

      expect(timed.audio).toEqual(reference.audio);
      expect(timed.lateCount()).toBe(2);
      // Cumulative: an on-time event leaves the count where it was.
      timed.noteAt((4 * BLOCK + 3) / SAMPLE_RATE);
      timed.render(1);
      expect(timed.lateCount()).toBe(2);
    } finally {
      timed.processor.destroy();
      reference.processor.destroy();
    }
  });

  it('keeps a deferred event on its render frame across a seek', () => {
    const timed = liveProcessor(0);
    const reference = liveProcessor(0);
    try {
      timed.render(1);
      timed.noteAt((4 * BLOCK + 11) / SAMPLE_RATE);
      timed.engine.seekSample(9600);
      timed.render(1);
      // The live input queue is keyed on the render frame, which a seek does not move.
      expect(timed.engine.midiInputPendingCount).toBe(1);
      timed.render(4);

      reference.render(1);
      reference.engine.seekSample(9600);
      reference.render(3);
      reference.noteAtOffset(11);
      reference.render(2);

      expect(timed.audio).toEqual(reference.audio);
    } finally {
      timed.processor.destroy();
      reference.processor.destroy();
    }
  });

  it('keeps a deferred event across a processor-state reset', () => {
    const timed = liveProcessor(0);
    const reference = liveProcessor(0);
    try {
      timed.render(1);
      timed.noteAt((3 * BLOCK + 20) / SAMPLE_RATE);
      timed.engine.resetProcessorState();
      timed.render(1);
      expect(timed.engine.midiInputPendingCount).toBe(1);
      timed.render(3);

      reference.render(1);
      reference.engine.resetProcessorState();
      reference.render(2);
      reference.noteAtOffset(20);
      reference.render(2);

      expect(firstSound(reference.audio)).toBeGreaterThanOrEqual(3 * BLOCK + 20);
      expect(timed.audio).toEqual(reference.audio);
    } finally {
      timed.processor.destroy();
      reference.processor.destroy();
    }
  });
});

/** A facade whose port delivers each sync to a live processor. */
async function bridgedEngine() {
  const posted: Array<Record<string, unknown>> = [];
  const port: {
    postMessage: (message: unknown) => void;
    onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  } = {
    postMessage: (message) => {
      const typed = message as Record<string, unknown>;
      posted.push(typed);
      if (typeof typed.type === 'string' && typed.type in ENGINE_SYNC_MESSAGE_TYPES) {
        live.receiveSync(message as Parameters<typeof live.receiveSync>[0]);
      } else if (typed.type === 'engineReadRequest') {
        live.receiveReadRequest(message as Parameters<typeof live.receiveReadRequest>[0]);
      }
    },
  };
  const live: SonareRealtimeEngineWorkletProcessor = new SonareRealtimeEngineWorkletProcessor({
    sampleRate: SAMPLE_RATE,
    maxBlockSize: BLOCK,
    numChannels: 2,
    transport: {
      postMessage: (reply) => queueMicrotask(() => port.onmessage?.({ data: reply } as never)),
    },
  });
  const engine = await SonareEngine.create(
    { sampleRate: SAMPLE_RATE, audioWorklet: { addModule: () => Promise.resolve() } } as never,
    {
      numChannels: 2,
      mode: 'postMessage',
      nodeFactory: () => {
        queueMicrotask(() =>
          port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as never),
        );
        return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      },
    },
  );
  engine.setMidiInputSource(0);
  return { engine, live, posted };
}

describe('SonareEngine live MIDI input timing options', () => {
  setupWorklet();

  it('sends a context time with offset 0 and pushes the offline mirror at its block head', async () => {
    const { engine, live, posted } = await bridgedEngine();
    const offline = (engine as unknown as { offlineEngine: { pushMidiInputCc: () => void } })
      .offlineEngine;
    const mirrored = vi.spyOn(offline, 'pushMidiInputCc');
    try {
      posted.length = 0;
      engine.pushMidiInputCc(0, 1, 7, 90, undefined, { contextTimeSec: 0.5 });
      engine.pushMidiInputUmp([0x20903c64], undefined, { contextTimeSec: 0.25 });
      engine.pushMidiInputNoteOn(0, 0, 60, 100, 12, {});
      // null is absent, for the bag and for the field.
      engine.pushMidiInputNoteOff(0, 0, 60, 0, 4, null as never);
      engine.pushMidiInputNoteOff(0, 0, 60, 0, 5, { contextTimeSec: null as never });
      expect(posted).toEqual([
        expect.objectContaining({
          type: 'syncMidiInputCc',
          data0: 7,
          data1: 90,
          offsetInBlockSamples: 0,
          contextTimeSec: 0.5,
        }),
        expect.objectContaining({ type: 'syncMidiInputUmp', contextTimeSec: 0.25 }),
        expect.objectContaining({ type: 'syncMidiInputNoteOn', offsetInBlockSamples: 12 }),
        expect.objectContaining({ type: 'syncMidiInputNoteOff', offsetInBlockSamples: 4 }),
        expect.objectContaining({ type: 'syncMidiInputNoteOff', offsetInBlockSamples: 5 }),
      ]);
      expect(posted[3]).not.toHaveProperty('contextTimeSec');
      expect(posted[4]).not.toHaveProperty('contextTimeSec');
      expect(posted[2]).not.toHaveProperty('contextTimeSec');
      expect(mirrored.mock.calls).toEqual([[0, 1, 7, 90, 0]]);
      expect(await engine.lateMidiInputCount()).toBe(0);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it('refuses an offset beside a context time, and a context time out of domain', async () => {
    const { engine, live, posted } = await bridgedEngine();
    try {
      posted.length = 0;
      expect(() => engine.pushMidiInputNoteOn(0, 0, 60, 100, 0, { contextTimeSec: 1 })).toThrow(
        TypeError,
      );
      expect(() => engine.pushMidiInputUmp([0x20903c64], 3, { contextTimeSec: 1 })).toThrow(
        /offsetInBlockSamples and contextTimeSec must not both be given/,
      );
      expect(() =>
        engine.pushMidiInputNoteOff(0, 0, 60, 0, undefined, { contextTimeSec: -0.1 }),
      ).toThrow(RangeError);
      expect(() =>
        engine.pushMidiInputPitchBend(0, 0, 8192, undefined, { contextTimeSec: Number.NaN }),
      ).toThrow(RangeError);
      expect(() =>
        engine.pushMidiInputCc(0, 0, 7, 1, undefined, { contextTimeSec: '1' as never }),
      ).toThrow(TypeError);
      expect(() =>
        engine.pushMidiInputPolyPressure(0, 0, 60, 1, undefined, { when: 1 } as never),
      ).toThrow(TypeError);
      expect(posted).toEqual([]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });
});
