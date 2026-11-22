/**
 * Articulation through the AudioWorklet engine: `SonareEngine.setArticulation`
 * reaches the live engine as a sync, the read-backs answer from the live engine
 * rather than from a mirror of the last value sent, and a mode set this way
 * reaches the sound the worklet renders.
 */

import { describe, expect, it } from 'vitest';
import type { Articulation, SynthEngineMode } from '../dist/index.js';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import {
  SonareEngine,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const BLOCK = 128;
const TRACK = 3;

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: 48000,
    audioWorklet: { addModule: () => Promise.resolve() },
  } as unknown as BaseAudioContext;
}

/** A facade whose port delivers each message to a live processor, and its replies back. */
async function bridgedEngine(): Promise<{
  engine: SonareEngine;
  live: SonareRealtimeEngineWorkletProcessor;
  posted: Array<{ type?: unknown }>;
  render: (blocks: number) => Float32Array;
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
        live.receiveSync(message as Parameters<typeof live.receiveSync>[0]);
      } else if (typed.type === 'articulationRequest') {
        live.receiveArticulationRequest(message as never);
      } else if (typeof typed.type === 'number') {
        live.receiveCommand(message as never);
      }
    },
  };
  const live: SonareRealtimeEngineWorkletProcessor = new SonareRealtimeEngineWorkletProcessor({
    sampleRate: 48000,
    maxBlockSize: BLOCK,
    numChannels: 2,
    transport: {
      postMessage: (reply) => queueMicrotask(() => port.onmessage?.({ data: reply } as never)),
    },
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
  const render = (blocks: number): Float32Array => {
    const out = new Float32Array(blocks * BLOCK);
    for (let block = 0; block < blocks; block++) {
      const left = new Float32Array(BLOCK);
      live.process([[]], [[left, new Float32Array(BLOCK)]]);
      out.set(left, block * BLOCK);
    }
    return out;
  };
  return { engine, live, posted, render };
}

describe('SonareEngine articulation', () => {
  setupWorklet();

  it('posts the articulation the offline engine accepted, and nothing it refused', async () => {
    const { engine, live, posted } = await bridgedEngine();
    try {
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setSynthInstrument(TRACK, { engineMode: 'reed' });
      posted.length = 0;
      engine.setArticulation(TRACK, 2, 'monoLegato');
      expect(posted.filter((message) => message.type === 'syncArticulation')).toEqual([
        expect.objectContaining({
          type: 'syncArticulation',
          destinationId: TRACK,
          channel: 2,
          articulation: 'monoLegato',
        }),
      ]);

      // The refusals are the main-thread engine's, raised before anything is posted.
      posted.length = 0;
      expect(() => engine.setArticulation(TRACK, 0, 2 as never)).toThrow(TypeError);
      expect(() => engine.setArticulation(TRACK, 16, 'poly')).toThrow(/out of range/);
      expect(() => engine.setArticulation(9, 0, 'poly')).toThrow(/no MIDI instrument is bound/);
      engine.setBuiltinInstrument(TRACK, {});
      posted.length = 0;
      expect(() => engine.setArticulation(TRACK, 0, 'monoLegato')).toThrow(
        /no articulation of its own/,
      );
      expect(posted.filter((message) => message.type === 'syncArticulation')).toEqual([]);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it('reads back what the live engine holds, which a later bind replaces', async () => {
    const { engine, live } = await bridgedEngine();
    try {
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setSynthInstrument(TRACK, { engineMode: 'reed' });
      engine.setArticulation(TRACK, 0, 'monoLegato');
      engine.setArticulation(TRACK, 1, 'monoRetrigger');
      await expect(engine.articulation(TRACK, 0)).resolves.toBe('monoLegato');
      await expect(engine.articulation(TRACK, 1)).resolves.toBe('monoRetrigger');
      await expect(engine.articulation(TRACK, 2)).resolves.toBe('poly');
      await expect(engine.legatoFallbackCount(TRACK)).resolves.toBe(0);

      // Binding another instrument installs that instrument's own articulation,
      // so a mirror of the last value sent would now answer wrongly.
      engine.setSynthInstrument(TRACK, { engineMode: 'reed' });
      await expect(engine.articulation(TRACK, 0)).resolves.toBe('poly');
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it('rejects a read-back with the class and cause the main-thread engine throws', async () => {
    const { engine, live } = await bridgedEngine();
    try {
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setSynthInstrument(TRACK, { engineMode: 'reed' });
      await expect(engine.articulation(TRACK, 16)).rejects.toThrow(RangeError);
      await expect(engine.articulation(TRACK, 16)).rejects.toThrow(/out of range/);
      await expect(engine.articulation(9, 0)).rejects.toThrow(/no MIDI instrument is bound/);
      await expect(engine.legatoFallbackCount(9)).rejects.toThrow(/no MIDI instrument is bound/);
      engine.setBuiltinInstrument(TRACK, {});
      await expect(engine.legatoFallbackCount(TRACK)).rejects.toThrow(/no articulation of its own/);
    } finally {
      engine.destroy();
      live.destroy();
    }
  });

  it('rejects a pending read-back when the engine is destroyed', async () => {
    const { engine, live } = await bridgedEngine();
    engine.destroy();
    await expect(engine.articulation(TRACK, 0)).rejects.toMatchObject({ codeName: 'InvalidState' });
    live.destroy();
  });

  /** C4 held, then G4 over it, then C4's late note-off, rendered by the live processor. */
  async function renderSlur(
    engineMode: SynthEngineMode,
    articulation: Articulation,
  ): Promise<{ audio: Float32Array; fallbacks: number }> {
    const { engine, live, render } = await bridgedEngine();
    try {
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setSynthInstrument(TRACK, { engineMode });
      engine.setArticulation(TRACK, 0, articulation);
      engine.pushMidiNoteOn(TRACK, 0, 0, 60, 100);
      const head = render(96);
      engine.pushMidiNoteOn(TRACK, 0, 0, 67, 100);
      engine.pushMidiNoteOff(TRACK, 0, 0, 60, 0);
      const tail = render(128);
      const audio = new Float32Array(head.length + tail.length);
      audio.set(head, 0);
      audio.set(tail, head.length);
      return { audio, fallbacks: await engine.legatoFallbackCount(TRACK) };
    } finally {
      engine.destroy();
      live.destroy();
    }
  }

  it('renders a slur differently from a retrigger in the worklet', async () => {
    const slurred = await renderSlur('reed', 'monoLegato');
    const retriggered = await renderSlur('reed', 'monoRetrigger');
    expect(Math.max(...retriggered.audio.map(Math.abs))).toBeGreaterThan(0);
    let maxDelta = 0;
    for (let i = 0; i < slurred.audio.length; i++) {
      maxDelta = Math.max(maxDelta, Math.abs(slurred.audio[i] - retriggered.audio[i]));
    }
    expect(maxDelta).toBeGreaterThan(1e-4);
    expect(slurred.fallbacks).toBe(0);
  });

  it('counts a declined slur in the live engine', async () => {
    const declined = await renderSlur('piano', 'monoLegato');
    expect(declined.fallbacks).toBeGreaterThanOrEqual(1);
    expect((await renderSlur('piano', 'poly')).fallbacks).toBe(0);
  });
});
