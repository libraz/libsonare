/**
 * Replacing the live MIDI input source releases what the previous source still
 * held, inside the engine: a second `bindWebMidi` on the same SonareEngine posts
 * only the new source, and the rendering worklet ends the old stream's notes and
 * lifts its damper on its own.
 */

import { describe, expect, it } from 'vitest';
import { bindWebMidi } from '../dist/index.js';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import {
  SonareEngine,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const BLOCK = 128;
const TRACK = 3;

class FakeInput {
  readonly manufacturer = 'test';
  readonly state = 'connected';
  readonly connection = 'open';
  private listeners = new Set<(event: { data: Uint8Array; timeStamp?: number }) => void>();

  constructor(
    readonly id: string,
    readonly name = id,
  ) {}

  addEventListener(type: string, listener: (event: { data: Uint8Array }) => void) {
    if (type === 'midimessage') {
      this.listeners.add(listener);
    }
  }

  removeEventListener(type: string, listener: (event: { data: Uint8Array }) => void) {
    if (type === 'midimessage') {
      this.listeners.delete(listener);
    }
  }

  emit(data: number[]) {
    for (const listener of this.listeners) {
      listener({ data: Uint8Array.from(data), timeStamp: 0 });
    }
  }
}

function installMidi(inputs: FakeInput[]) {
  const access = {
    inputs: new Map(inputs.map((input) => [input.id, input])),
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
  };
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: { requestMIDIAccess: async () => access },
  });
}

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: 48000,
    audioWorklet: { addModule: () => Promise.resolve() },
  } as unknown as BaseAudioContext;
}

/** A facade whose port delivers each message to a live processor. */
async function bridgedEngine(): Promise<{
  engine: SonareEngine;
  posted: Array<{ type?: unknown }>;
  peak: (blocks: number) => number;
  destroy: () => void;
}> {
  const posted: Array<{ type?: unknown }> = [];
  const port: {
    postMessage: (message: unknown) => void;
    onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  } = {
    postMessage: (message) => {
      const typed = message as { type?: unknown };
      posted.push(typed);
      if (typeof typed.type === 'string' && typed.type in ENGINE_SYNC_MESSAGE_TYPES) {
        live.receiveMessage(message);
      } else if (typeof typed.type === 'number') {
        live.receiveMessage(message as never);
      }
    },
  };
  const live: SonareRealtimeEngineWorkletProcessor = new SonareRealtimeEngineWorkletProcessor({
    sampleRate: 48000,
    maxBlockSize: BLOCK,
    numChannels: 2,
    transport: { postMessage: () => undefined },
  });
  const engine = await SonareEngine.create(fakeContext(), {
    numChannels: 2,
    mode: 'postMessage',
    nodeFactory: () => {
      queueMicrotask(() =>
        port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as never),
      );
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    },
  });
  // Largest absolute left-channel sample over the last of `blocks` rendered blocks.
  const peak = (blocks: number): number => {
    let last = 0;
    for (let block = 0; block < blocks; block++) {
      const left = new Float32Array(BLOCK);
      live.process([[]], [[left, new Float32Array(BLOCK)]]);
      last = left.reduce((max, sample) => Math.max(max, Math.abs(sample)), 0);
    }
    return last;
  };
  return {
    engine,
    posted,
    peak,
    destroy: () => {
      engine.destroy();
      live.destroy();
    },
  };
}

const RELEASE_MESSAGES = new Set([
  'syncMidiInputNoteOff',
  'syncMidiInputCc',
  'syncMidiInputPitchBend',
  'syncMidiInputUmp',
]);

describe('bindWebMidi source replacement', () => {
  setupWorklet();

  // Holds `held` through the first binding, then either replaces the source with
  // a second binding that listens to no input, or leaves it, and returns the
  // peak of the last block rendered afterwards.
  async function heldThenReplaced(held: number[][], replace: boolean): Promise<number> {
    const keys = new FakeInput('keys');
    installMidi([keys, new FakeInput('other')]);
    const { engine, posted, peak, destroy } = await bridgedEngine();
    try {
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setBuiltinInstrument(TRACK, {});
      engine.transport.play();
      const first = await bindWebMidi(engine, { destinationId: TRACK, inputIds: ['keys'] });
      for (const message of held) {
        keys.emit(message);
      }
      expect(peak(4)).toBeGreaterThan(0.01);
      posted.length = 0;
      const second = replace
        ? await bindWebMidi(engine, { destinationId: TRACK, inputIds: ['other'] })
        : undefined;
      // One second of rendering is far past the built-in instrument's release.
      const after = peak(375);
      if (second) {
        // Only the new source travelled: the release is the engine's, not a posted one.
        expect(posted.some((message) => message.type === 'syncMidiInputSource')).toBe(true);
        expect(posted.filter((message) => RELEASE_MESSAGES.has(String(message.type)))).toEqual([]);
        second.destroy();
      }
      first.destroy();
      return after;
    } finally {
      destroy();
    }
  }

  it("ends the previous source's held notes", async () => {
    const held = [
      [0x90, 60, 100],
      [0x90, 64, 100],
    ];
    expect(await heldThenReplaced(held, false)).toBeGreaterThan(0.01);
    expect(await heldThenReplaced(held, true)).toBe(0);
  });

  it("lifts the previous source's damper over a released note", async () => {
    const held = [
      [0xb0, 64, 127],
      [0x90, 60, 100],
      [0x80, 60, 0],
    ];
    expect(await heldThenReplaced(held, false)).toBeGreaterThan(0.01);
    expect(await heldThenReplaced(held, true)).toBe(0);
  });
});
