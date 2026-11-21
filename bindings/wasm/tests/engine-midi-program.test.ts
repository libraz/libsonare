import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, midi2Program, RealtimeEngine } from '../dist/index.js';
import { SonareEngine, setupWorklet } from './_worklet_helpers';

setupWorklet();

const DEST = 7;
const BLOCK = 128;
const BLOCKS = 24;

/** Run `setup`, strike middle C on channel 0 and return the left channel. */
function render(
  setup: (engine: RealtimeEngine) => void,
  strike: (engine: RealtimeEngine) => void = (engine) => engine.pushMidiNoteOn(DEST, 0, 0, 60, 100),
): Float32Array {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
  try {
    engine.setSf2Instrument(DEST, {});
    setup(engine);
    strike(engine);
    const out = new Float32Array(BLOCKS * BLOCK);
    for (let block = 0; block < BLOCKS; block++) {
      const [left] = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      out.set(left, block * BLOCK);
    }
    return out;
  } finally {
    engine.destroy();
  }
}

function peak(samples: Float32Array): number {
  return samples.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
}

function errorCode(call: () => void): ErrorCode | undefined {
  try {
    call();
  } catch (error) {
    return isSonareError(error) ? error.code : undefined;
  }
  return undefined;
}

describe('RealtimeEngine.pushMidiProgram', () => {
  it('renders bit-identically to the raw MIDI 2.0 Program Change and differently from bank 0 and none', () => {
    const typed = render((e) => e.pushMidiProgram(DEST, 0, 0, 0, { msb: 8, lsb: 0 }));
    const [w0, w1] = [0x40c00001, 0x00000800];
    const raw = render((e) => e.pushMidiUmp(DEST, [w0, w1]));
    const bank0 = render((e) => e.pushMidiProgram(DEST, 0, 0, 0, { msb: 0, lsb: 0 }));
    const none = render(() => undefined);
    expect(peak(typed)).toBeGreaterThan(0);
    expect(typed).toEqual(raw);
    expect(typed).not.toEqual(bank0);
    expect(typed).not.toEqual(none);
  });

  it('refuses out-of-range arguments with a range error naming the function', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      const calls: Array<[() => void, string]> = [
        [() => engine.pushMidiProgram(DEST, 16, 0, 0), 'group must be an integer in [0, 15]'],
        [() => engine.pushMidiProgram(DEST, -1, 0, 0), 'group must be an integer in [0, 15]'],
        [() => engine.pushMidiProgram(DEST, 0, 16, 0), 'channel must be an integer in [0, 15]'],
        [() => engine.pushMidiProgram(DEST, 0, 0, 128), 'program must be an integer in [0, 127]'],
        [
          () => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 128, lsb: 0 }),
          'bank.msb must be an integer in [0, 127]',
        ],
        [
          () => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 0, lsb: 128 }),
          'bank.lsb must be an integer in [0, 127]',
        ],
      ];
      for (const [call, message] of calls) {
        expect(call).toThrow(new RangeError(`RealtimeEngine.pushMidiProgram: ${message}`));
      }
    } finally {
      engine.destroy();
    }
  });

  it('reports a full queue as OutOfMemory', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      let code: ErrorCode | undefined;
      for (let i = 0; i < 100000 && code === undefined; i++) {
        code = errorCode(() => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 8, lsb: 0 }));
      }
      expect(code).toBe(ErrorCode.OutOfMemory);
    } finally {
      engine.destroy();
    }
  });
});

describe('RealtimeEngine.pushMidiInputProgram', () => {
  it('needs an enabled input source, refuses like the destination op and reaches the synth', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      expect(errorCode(() => engine.pushMidiInputProgram(0, 0, 0, { msb: 8, lsb: 0 }))).toBe(
        ErrorCode.InvalidState,
      );
      engine.setMidiInputSource(DEST);
      expect(() => engine.pushMidiInputProgram(0, 0, 5, { msb: 1, lsb: 2 }, 64)).not.toThrow();
      expect(() => engine.pushMidiInputProgram(0, 0, 5)).not.toThrow();
      expect(() =>
        engine.pushMidiInputProgram(0, 0, 5, { msb: 1, lsb: 2, valid: false } as never),
      ).toThrow(TypeError);
      expect(() => engine.pushMidiInputProgram(0, 16, 0)).toThrow(
        new RangeError(
          'RealtimeEngine.pushMidiInputProgram: channel must be an integer in [0, 15]',
        ),
      );
      expect(() => engine.pushMidiInputProgram(0, 0, 128)).toThrow(
        new RangeError(
          'RealtimeEngine.pushMidiInputProgram: program must be an integer in [0, 127]',
        ),
      );
    } finally {
      engine.destroy();
    }

    // Input events and destination commands drain in different phases, so the
    // note is struck through the input source as well.
    const inputNote = (e: RealtimeEngine) => e.pushMidiInputNoteOn(0, 0, 60, 100);
    const enable = (e: RealtimeEngine) => e.setMidiInputSource(DEST);
    const viaInput = render((e) => {
      enable(e);
      e.pushMidiInputProgram(0, 0, 0, { msb: 8, lsb: 0 });
    }, inputNote);
    const viaRaw = render((e) => {
      enable(e);
      e.pushMidiInputUmp([0x40c00001, 0x00000800]);
    }, inputNote);
    const none = render(enable, inputNote);
    expect(peak(viaInput)).toBeGreaterThan(0);
    expect(viaInput).toEqual(viaRaw);
    expect(viaInput).not.toEqual(none);
  });
});

describe('midi2Program bank', () => {
  it('packs the bank when given, leaves it out otherwise and refuses a bad one', () => {
    const none = midi2Program({ ppq: 0, group: 0, channel: 0, program: 5 });
    expect([none.word0, none.word1]).toEqual([0x40c00000, 0x05000000]);
    expect(() =>
      midi2Program({ ppq: 0, group: 0, channel: 0, program: 5, bank: { msb: 128, lsb: 0 } }),
    ).toThrow(RangeError);
    expect(() =>
      midi2Program({ ppq: 0, group: 0, channel: 0, program: 5, bank: { msb: 1 } as never }),
    ).toThrow(TypeError);
    const e = midi2Program({ ppq: 3, group: 1, channel: 2, program: 5, bank: { msb: 1, lsb: 2 } });
    expect([e.ppq, e.word0, e.word1]).toEqual([3, 0x41c20001, 0x05000102]);
  });
});

describe('SonareEngine.pushMidiProgram (worklet facade)', () => {
  const fakeContext = {
    sampleRate: 48000,
    audioWorklet: { addModule: () => Promise.resolve() },
  } as unknown as BaseAudioContext;

  it('posts syncMidiUmp with the builder words and refuses like the packer', async () => {
    const posted: unknown[] = [];
    const engine = await SonareEngine.create(fakeContext, {
      numChannels: 2,
      mode: 'postMessage',
      nodeFactory: () => {
        const port = {
          postMessage: (message: unknown) => posted.push(message),
          onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
        };
        queueMicrotask(() => {
          port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent);
        });
        return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      },
    });
    try {
      const built = midi2Program({
        ppq: 0,
        group: 1,
        channel: 2,
        program: 5,
        bank: { msb: 1, lsb: 2 },
      });
      engine.pushMidiProgram(DEST, 1, 2, 5, { msb: 1, lsb: 2 }, 256);
      expect(posted).toContainEqual({
        type: 'syncMidiUmp',
        destinationId: DEST,
        words: Uint32Array.from([built.word0, built.word1 ?? 0]),
        renderFrame: 256,
        entry: 'SonareEngine.pushMidiProgram',
      });
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 5)).not.toThrow();
      expect(() => engine.pushMidiProgram(DEST, 0, 16, 0)).toThrow(RangeError);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 128, lsb: 0 })).toThrow(
        new RangeError('SonareEngine.pushMidiProgram: bank.msb must be an integer in [0, 127]'),
      );
      expect(() => engine.pushMidiProgram(DEST, 16, 0, 0)).toThrow(
        new RangeError('SonareEngine.pushMidiProgram: group must be an integer in [0, 15]'),
      );
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 1.5)).toThrow(
        new RangeError('SonareEngine.pushMidiProgram: program must be an integer in [0, 127]'),
      );
    } finally {
      engine.destroy();
    }
  });
});
