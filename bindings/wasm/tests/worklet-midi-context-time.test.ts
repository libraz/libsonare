/**
 * MIDI placed on the AudioContext clock: the processor maps `contextTimeSec` through the
 * last quantum head's render frame and context frame, for live input and track-addressed
 * pushes alike, so a timed event renders exactly as the same event pushed with its block
 * offset in the block it lands in. A time already rendered plays at the next block's start
 * and is counted once for both families, and the SonareEngine facade refuses an offset or
 * render frame given together with a context time.
 */

import { afterEach, describe, expect, it, vi } from 'vitest';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import { unknownKeyError } from './_helpers';
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
  processor.receiveMessage({ type: 'syncMidiInputSource', destinationId: 0 });
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
    processor.receiveMessage({
      type: NOTE_ON,
      group: 0,
      channel: 0,
      data0: 60,
      data1: 100,
      offsetInBlockSamples: 0,
      contextTimeSec,
    });
  const noteAtOffset = (offsetInBlockSamples: number) =>
    processor.receiveMessage({
      type: NOTE_ON,
      group: 0,
      channel: 0,
      data0: 60,
      data1: 100,
      offsetInBlockSamples,
    });
  const lateCount = () => {
    processor.receiveMessage({
      type: 'engineReadRequest',
      requestId: replies.length,
      op: 'lateMidiEventCount',
      destinationId: 0,
      channel: 0,
    });
    return (replies.at(-1) as { value: number }).value;
  };
  const trackNoteAt = (contextTimeSec: number) =>
    processor.receiveMessage({
      type: 'syncMidiNoteOn',
      destinationId: 0,
      group: 0,
      channel: 0,
      note: 60,
      velocity: 100,
      renderFrame: -1,
      contextTimeSec,
    });
  return { processor, engine, audio, render, noteAt, noteAtOffset, trackNoteAt, lateCount };
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

  it('lands a timed track push on the frame the timed live input push lands on', () => {
    const contextStart = 1000;
    const track = liveProcessor(contextStart);
    const input = liveProcessor(contextStart);
    try {
      track.render(1);
      input.render(1);
      const due = (contextStart + 3 * BLOCK + 37) / SAMPLE_RATE;
      track.trackNoteAt(due);
      input.noteAt(due);
      track.render(5);
      input.render(5);

      expect(firstSound(input.audio)).toBeGreaterThanOrEqual(3 * BLOCK + 37);
      expect(track.audio).toEqual(input.audio);
      expect(track.lateCount()).toBe(0);
    } finally {
      track.processor.destroy();
      input.processor.destroy();
    }
  });

  it('counts a late track push and a late live input push on one counter', () => {
    const timed = liveProcessor(0);
    try {
      timed.render(2);
      timed.trackNoteAt(0);
      timed.noteAt(BLOCK / SAMPLE_RATE);
      timed.render(1);
      expect(timed.lateCount()).toBe(2);
    } finally {
      timed.processor.destroy();
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
        live.receiveMessage(message);
      } else if (typed.type === 'engineReadRequest') {
        live.receiveMessage(message);
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
      expect(await engine.lateMidiEventCount()).toBe(0);
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

describe('SonareEngine track MIDI timing options', () => {
  setupWorklet();

  it('sends a context time instead of a render frame and pushes the offline mirror now', async () => {
    const { engine, live, posted } = await bridgedEngine();
    const offline = (engine as unknown as { offlineEngine: { pushMidiCc: () => void } })
      .offlineEngine;
    const mirrored = vi.spyOn(offline, 'pushMidiCc');
    try {
      const sysex = new Uint8Array([0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]);
      posted.length = 0;
      engine.pushMidiNoteOn(0, 0, 0, 60, 100, undefined, { contextTimeSec: 0.5 });
      engine.pushMidiNoteOff(0, 0, 0, 60, 0, undefined, { contextTimeSec: 0.5 });
      engine.pushMidiCc(0, 0, 1, 7, 90, undefined, { contextTimeSec: 0.25 });
      engine.pushMidiPitchBend(0, 0, 0, 8192, undefined, { contextTimeSec: 0.25 });
      engine.pushMidiChannelPressure(0, 0, 0, 30, undefined, { contextTimeSec: 0.25 });
      engine.pushMidiPolyPressure(0, 0, 0, 60, 30, undefined, { contextTimeSec: 0.25 });
      engine.pushMidiUmp(0, [0x20903c64], undefined, { contextTimeSec: 0.125 });
      engine.pushMidiProgram(0, 0, 0, 5, undefined, undefined, { contextTimeSec: 0.125 });
      engine.pushMidiSysex(0, sysex, undefined, { contextTimeSec: 0.125 });
      // null is absent, for the bag and for the field.
      engine.pushMidiCc(0, 0, 1, 7, 91, 64, null as never);
      engine.pushMidiCc(0, 0, 1, 7, 92, 65, { contextTimeSec: null as never });
      const timed = (type: string, contextTimeSec: number) =>
        expect.objectContaining({ type, renderFrame: -1, contextTimeSec });
      expect(posted).toEqual([
        timed('syncMidiNoteOn', 0.5),
        timed('syncMidiNoteOff', 0.5),
        timed('syncMidiCc', 0.25),
        timed('syncMidiPitchBend', 0.25),
        timed('syncMidiChannelPressure', 0.25),
        timed('syncMidiPolyPressure', 0.25),
        timed('syncMidiUmp', 0.125),
        timed('syncMidiUmp', 0.125),
        timed('syncMidiSysex', 0.125),
        expect.objectContaining({ type: 'syncMidiCc', renderFrame: 64 }),
        expect.objectContaining({ type: 'syncMidiCc', renderFrame: 65 }),
      ]);
      expect(posted[9]).not.toHaveProperty('contextTimeSec');
      expect(posted[10]).not.toHaveProperty('contextTimeSec');
      expect(mirrored.mock.calls).toEqual([
        [0, 0, 1, 7, 90, undefined],
        [0, 0, 1, 7, 91, 64],
        [0, 0, 1, 7, 92, 65],
      ]);
      expect(await engine.lateMidiEventCount()).toBe(0);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it('refuses a render frame beside a context time, and a context time out of domain', async () => {
    const { engine, live, posted } = await bridgedEngine();
    try {
      posted.length = 0;
      expect(() => engine.pushMidiNoteOn(0, 0, 0, 60, 100, 0, { contextTimeSec: 1 })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.pushMidiSysex(0, new Uint8Array([0xf0, 0xf7]), 3, { contextTimeSec: 1 }),
      ).toThrow(/renderFrame and contextTimeSec must not both be given/);
      expect(() => engine.pushMidiProgram(0, 0, 0, 5, undefined, 3, { contextTimeSec: 1 })).toThrow(
        TypeError,
      );
      expect(() =>
        engine.pushMidiNoteOff(0, 0, 0, 60, 0, undefined, { contextTimeSec: -0.1 }),
      ).toThrow(RangeError);
      expect(() =>
        engine.pushMidiPitchBend(0, 0, 0, 8192, undefined, { contextTimeSec: Number.NaN }),
      ).toThrow(RangeError);
      expect(() =>
        engine.pushMidiCc(0, 0, 0, 7, 1, undefined, { contextTimeSec: '1' as never }),
      ).toThrow(TypeError);
      const bogus = { when: 1 } as never;
      const sysex = new Uint8Array([0xf0, 0xf7]);
      const undeclared: Array<[string, () => void]> = [
        ['pushMidiNoteOn', () => engine.pushMidiNoteOn(0, 0, 0, 60, 1, undefined, bogus)],
        ['pushMidiNoteOff', () => engine.pushMidiNoteOff(0, 0, 0, 60, 0, undefined, bogus)],
        ['pushMidiCc', () => engine.pushMidiCc(0, 0, 0, 7, 1, undefined, bogus)],
        ['pushMidiPitchBend', () => engine.pushMidiPitchBend(0, 0, 0, 8192, undefined, bogus)],
        [
          'pushMidiChannelPressure',
          () => engine.pushMidiChannelPressure(0, 0, 0, 1, undefined, bogus),
        ],
        [
          'pushMidiPolyPressure',
          () => engine.pushMidiPolyPressure(0, 0, 0, 60, 1, undefined, bogus),
        ],
        ['pushMidiUmp', () => engine.pushMidiUmp(0, [0x20903c64], undefined, bogus)],
        ['pushMidiProgram', () => engine.pushMidiProgram(0, 0, 0, 5, undefined, undefined, bogus)],
        ['pushMidiSysex', () => engine.pushMidiSysex(0, sysex, undefined, bogus)],
      ];
      for (const [name, call] of undeclared) {
        expect(call).toThrow(unknownKeyError(`SonareEngine.${name}: unknown key when`));
      }
      expect(posted).toEqual([]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });
});

describe('SonareEngine.pushMidiInputProgram', () => {
  setupWorklet();

  it('posts a MIDI 2.0 program change through the live input with the input timing', async () => {
    const { engine, live, posted } = await bridgedEngine();
    const offline = (
      engine as unknown as {
        offlineEngine: { pushMidiInputUmp: (words: unknown, offset: number) => void };
      }
    ).offlineEngine;
    const mirrored = vi.spyOn(offline, 'pushMidiInputUmp');
    try {
      posted.length = 0;
      engine.pushMidiInputProgram(2, 3, 9);
      engine.pushMidiInputProgram(2, 3, 13, { msb: 8, lsb: 2 }, 17);
      engine.pushMidiInputProgram(0, 0, 1, undefined, undefined, { contextTimeSec: 0.5 });
      const word0 = (group: number, channel: number, bankValid: number) =>
        ((0x4 << 28) | (group << 24) | (0xc << 20) | (channel << 16) | bankValid) >>> 0;
      expect(posted).toEqual([
        expect.objectContaining({
          type: 'syncMidiInputUmp',
          words: Uint32Array.from([word0(2, 3, 0), 9 << 24]),
          offsetInBlockSamples: 0,
        }),
        expect.objectContaining({
          type: 'syncMidiInputUmp',
          words: Uint32Array.from([word0(2, 3, 1), (13 << 24) | (8 << 8) | 2]),
          offsetInBlockSamples: 17,
        }),
        expect.objectContaining({
          type: 'syncMidiInputUmp',
          words: Uint32Array.from([word0(0, 0, 0), 1 << 24]),
          offsetInBlockSamples: 0,
          contextTimeSec: 0.5,
        }),
      ]);
      expect(posted[0]).not.toHaveProperty('contextTimeSec');
      expect(posted[1]).not.toHaveProperty('contextTimeSec');
      expect(mirrored.mock.calls.map((call) => call[1])).toEqual([0, 17, 0]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it('refuses what pushMidiProgram refuses, and an offset beside a context time', async () => {
    const { engine, live, posted } = await bridgedEngine();
    try {
      posted.length = 0;
      expect(() => engine.pushMidiInputProgram(0, 16, 0)).toThrow(RangeError);
      expect(() => engine.pushMidiInputProgram(0, 0, 128)).toThrow(RangeError);
      expect(() => engine.pushMidiInputProgram(0, 0, 0, { msb: 128, lsb: 0 })).toThrow(RangeError);
      expect(() =>
        engine.pushMidiInputProgram(0, 0, 0, { msb: 1, lsb: 0, valid: 1 } as never),
      ).toThrow(unknownKeyError('SonareEngine.pushMidiInputProgram: unknown key valid'));
      expect(() =>
        engine.pushMidiInputProgram(0, 0, 0, undefined, 3, { contextTimeSec: 1 }),
      ).toThrow(/offsetInBlockSamples and contextTimeSec must not both be given/);
      expect(() =>
        engine.pushMidiInputProgram(0, 0, 0, undefined, undefined, { when: 1 } as never),
      ).toThrow(unknownKeyError('SonareEngine.pushMidiInputProgram: unknown key when'));
      expect(posted).toEqual([]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });
});
