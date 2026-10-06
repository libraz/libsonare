import { afterEach, describe, expect, it } from 'vitest';
import { bindWebMidi, isWebMidiAvailable } from '../dist/index.js';

type MidiMessage =
  | {
      kind: 'on' | 'off' | 'cc';
      group: number;
      channel: number;
      a: number;
      b: number;
      time: number;
    }
  | {
      kind: 'poly-pressure';
      group: number;
      channel: number;
      note: number;
      pressure: number;
      time: number;
    }
  | {
      kind: 'channel-pressure';
      group: number;
      channel: number;
      pressure: number;
      time: number;
    }
  | {
      kind: 'pitch-bend';
      group: number;
      channel: number;
      bend14: number;
      time: number;
    };

class FakeInput {
  id: string;
  name: string;
  manufacturer = 'test';
  state: 'connected' | 'disconnected' = 'connected';
  onmidimessage: ((event: { data: Uint8Array | Uint32Array; timeStamp?: number }) => void) | null =
    null;
  private listeners = new Set<
    (event: { data: Uint8Array | Uint32Array; timeStamp?: number }) => void
  >();

  constructor(id: string, name = id) {
    this.id = id;
    this.name = name;
  }

  addEventListener(
    type: 'midimessage',
    listener: (event: { data: Uint8Array | Uint32Array; timeStamp?: number }) => void,
  ) {
    if (type === 'midimessage') {
      this.listeners.add(listener);
    }
  }

  removeEventListener(
    type: 'midimessage',
    listener: (event: { data: Uint8Array | Uint32Array; timeStamp?: number }) => void,
  ) {
    if (type === 'midimessage') {
      this.listeners.delete(listener);
    }
  }

  emit(data: number[], timeStamp = 0) {
    const event = {
      data: data.some((value) => value > 0xff) ? Uint32Array.from(data) : Uint8Array.from(data),
      timeStamp,
    };
    for (const listener of this.listeners) {
      listener(event);
    }
    this.onmidimessage?.(event);
  }
}

class FakeAccess {
  inputs = new Map<string, FakeInput>();
  onstatechange: ((event: { port?: FakeInput }) => void) | null = null;
  private listeners = new Set<(event: { port?: FakeInput }) => void>();

  addEventListener(type: 'statechange', listener: (event: { port?: FakeInput }) => void) {
    if (type === 'statechange') {
      this.listeners.add(listener);
    }
  }

  removeEventListener(type: 'statechange', listener: (event: { port?: FakeInput }) => void) {
    if (type === 'statechange') {
      this.listeners.delete(listener);
    }
  }

  stateChange(port?: FakeInput) {
    const event = { port };
    for (const listener of this.listeners) {
      listener(event);
    }
    this.onstatechange?.(event);
  }
}

class FakeEngine {
  messages: MidiMessage[] = [];
  ccBindings: unknown[] = [];
  sourceDestination: number | null = null;

  setMidiInputSource(destinationId = 0) {
    this.sourceDestination = destinationId;
  }

  clearMidiInputSource() {
    this.sourceDestination = null;
  }

  bindMidiCc(channel: number, controller: number, paramId: number, options?: unknown) {
    this.ccBindings.push({ channel, controller, paramId, options });
  }

  pushMidiInputNoteOn(group: number, channel: number, note: number, velocity: number, time = 0) {
    this.messages.push({ kind: 'on', group, channel, a: note, b: velocity, time });
  }

  pushMidiInputNoteOff(group: number, channel: number, note: number, velocity = 0, time = 0) {
    this.messages.push({ kind: 'off', group, channel, a: note, b: velocity, time });
  }

  pushMidiInputCc(group: number, channel: number, controller: number, value: number, time = 0) {
    this.messages.push({ kind: 'cc', group, channel, a: controller, b: value, time });
  }

  pushMidiInputPolyPressure(
    group: number,
    channel: number,
    note: number,
    pressure: number,
    time = 0,
  ) {
    this.messages.push({ kind: 'poly-pressure', group, channel, note, pressure, time });
  }

  pushMidiInputChannelPressure(group: number, channel: number, pressure: number, time = 0) {
    this.messages.push({ kind: 'channel-pressure', group, channel, pressure, time });
  }

  pushMidiInputPitchBend(group: number, channel: number, bend14: number, time = 0) {
    this.messages.push({ kind: 'pitch-bend', group, channel, bend14, time });
  }
}

class RawUmpEngine extends FakeEngine {
  rawUmp: Array<{ words: Uint32Array; time: number }> = [];

  pushMidiInputUmp(words: Uint32Array | readonly number[], time = 0) {
    this.rawUmp.push({ words: Uint32Array.from(words), time });
  }
}

class ThrowingRawUmpEngine extends RawUmpEngine {
  override pushMidiInputUmp(words: Uint32Array | readonly number[], time = 0) {
    const packet = Uint32Array.from(words);
    const messageType = packet[0] >>> 28;
    const wordCount = [1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4][messageType];
    if (packet.length !== wordCount || messageType === 0x3 || messageType === 0x5) {
      throw new Error('malformed UMP packet');
    }
    super.pushMidiInputUmp(packet, time);
  }
}

const originalNavigator = globalThis.navigator;

afterEach(() => {
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: originalNavigator,
  });
});

function installMidi(access: FakeAccess, calls: unknown[] = []) {
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: {
      requestMIDIAccess: async (options?: unknown) => {
        calls.push(options);
        return access;
      },
    },
  });
}

describe('Web MIDI helper', () => {
  it('reports availability from navigator.requestMIDIAccess', () => {
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {} });
    expect(isWebMidiAvailable()).toBe(false);

    installMidi(new FakeAccess());
    expect(isWebMidiAvailable()).toBe(true);
  });

  it('binds selected inputs and routes note, CC, velocity-zero note-off, and running-status messages', async () => {
    const access = new FakeAccess();
    const inputA = new FakeInput('a', 'Keys A');
    const inputB = new FakeInput('b', 'Keys B');
    access.inputs.set(inputA.id, inputA);
    access.inputs.set(inputB.id, inputB);
    const calls: unknown[] = [];
    installMidi(access, calls);

    const engine = new FakeEngine();
    const changed: string[][] = [];
    const binding = await bindWebMidi(engine as never, {
      destinationId: 7,
      group: 2,
      inputIds: ['a'],
      sysex: true,
      ccBindings: [
        { channel: 0, controller: 74, paramId: 5, options: { minValue: -60, maxValue: 12 } },
      ],
      timestampToSamples: (ms) => Math.round(ms * 48),
      onInputsChanged: (inputs) => changed.push(inputs.map((input) => input.id)),
    });

    expect(calls).toEqual([{ sysex: true, software: true }]);
    expect(engine.sourceDestination).toBe(7);
    expect(engine.ccBindings).toEqual([
      { channel: 0, controller: 74, paramId: 5, options: { minValue: -60, maxValue: 12 } },
    ]);
    expect(binding.inputs().map((input) => input.id)).toEqual(['a', 'b']);

    inputB.emit([0x90, 60, 100], 1);
    inputA.emit([0x90, 60, 100], 1);
    inputA.emit([64, 90], 2);
    inputA.emit([0xf1, 0x01], 2.5);
    inputA.emit([66, 91], 2.75);
    inputA.emit([0x90, 60, 0], 3);
    inputA.emit([0xb0, 74, 127], 4);
    inputA.emit([0x80, 64, 20], 5);

    expect(engine.messages).toEqual([
      { kind: 'on', group: 2, channel: 0, a: 60, b: 100, time: 48 },
      { kind: 'on', group: 2, channel: 0, a: 64, b: 90, time: 96 },
      { kind: 'off', group: 2, channel: 0, a: 60, b: 0, time: 144 },
      { kind: 'cc', group: 2, channel: 0, a: 74, b: 127, time: 192 },
      { kind: 'off', group: 2, channel: 0, a: 64, b: 20, time: 240 },
    ]);
    expect(changed.at(-1)).toEqual(['a', 'b']);

    binding.close();
    inputA.emit([0x90, 67, 100], 6);
    expect(engine.messages).toHaveLength(5);
    expect(engine.sourceDestination).toBeNull();
  });

  it('clears running status for system common messages and drops incomplete channel voice messages', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('keys');
    access.inputs.set(input.id, input);
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never);

    input.emit([0x90, 60, 100]);
    input.emit([0xf1, 0x01]);
    input.emit([64, 90]);
    input.emit([0x90, 62]);
    input.emit([0xb0, 74]);
    input.emit([0x90, 65, 70]);

    expect(engine.messages).toEqual([
      { kind: 'on', group: 0, channel: 0, a: 60, b: 100, time: 0 },
      { kind: 'on', group: 0, channel: 0, a: 65, b: 70, time: 0 },
    ]);
    binding.close();
  });

  it('routes MIDI 1 pressure and pitch bend, including one-byte running-status messages', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('expression');
    access.inputs.set(input.id, input);
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never, {
      timestampToSamples: (ms) => Math.round(ms * 48),
    });

    input.emit([0xa2, 60, 70], 1);
    input.emit([0xd2, 64], 2);
    input.emit([65], 2.5);
    input.emit([0xf8], 3);
    input.emit([66], 3.5);
    input.emit([0xe2, 1, 64], 4);
    input.emit([0xc2, 10], 5); // A valid one-data-byte program change is ignored.

    expect(engine.messages).toEqual([
      { kind: 'poly-pressure', group: 0, channel: 2, note: 60, pressure: 70, time: 48 },
      { kind: 'channel-pressure', group: 0, channel: 2, pressure: 64, time: 96 },
      { kind: 'channel-pressure', group: 0, channel: 2, pressure: 65, time: 120 },
      { kind: 'channel-pressure', group: 0, channel: 2, pressure: 66, time: 168 },
      { kind: 'pitch-bend', group: 0, channel: 2, bend14: 8193, time: 192 },
    ]);
    binding.close();
  });

  it('keeps MIDI running status isolated per bound input', async () => {
    const access = new FakeAccess();
    const inputA = new FakeInput('a');
    const inputB = new FakeInput('b');
    access.inputs.set(inputA.id, inputA);
    access.inputs.set(inputB.id, inputB);
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never);

    inputA.emit([0x90, 60, 100]);
    inputB.emit([64, 90]);
    inputB.emit([0x80, 64, 20]);
    inputB.emit([65, 30]);

    expect(engine.messages).toEqual([
      { kind: 'on', group: 0, channel: 0, a: 60, b: 100, time: 0 },
      { kind: 'off', group: 0, channel: 0, a: 64, b: 20, time: 0 },
      { kind: 'off', group: 0, channel: 0, a: 65, b: 30, time: 0 },
    ]);
    binding.close();
  });

  it('binds hot-plugged matching inputs and unbinds disconnected ports', async () => {
    const access = new FakeAccess();
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never, { inputIds: ['later'] });

    const later = new FakeInput('later');
    access.inputs.set(later.id, later);
    access.stateChange(later);
    later.emit([0x91, 62, 80]);
    expect(engine.messages).toEqual([{ kind: 'on', group: 0, channel: 1, a: 62, b: 80, time: 0 }]);

    later.state = 'disconnected';
    access.stateChange(later);
    later.emit([0x91, 64, 80]);
    expect(engine.messages).toHaveLength(1);
    binding.close();
  });

  it('routes MIDI 1.0 and MIDI 2.0 UMP channel voice words when browsers provide UMP data', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('ump');
    access.inputs.set(input.id, input);
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never);

    input.emit([0x21903c64]);
    input.emit([(0x4 << 28) | (3 << 24) | (0x9 << 20) | (2 << 16) | (65 << 8), 100 << 25]);
    input.emit([(0x4 << 28) | (3 << 24) | (0xb << 20) | (2 << 16) | (74 << 8), 127 << 25]);

    expect(engine.messages).toEqual([
      { kind: 'on', group: 1, channel: 0, a: 60, b: 100, time: 0 },
      { kind: 'on', group: 3, channel: 2, a: 65, b: 100, time: 0 },
      { kind: 'cc', group: 3, channel: 2, a: 74, b: 127, time: 0 },
    ]);
    binding.close();
  });

  it('falls back to optional pressure and bend methods for MIDI 2.0 UMP', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('ump-expression');
    access.inputs.set(input.id, input);
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never, {
      timestampToSamples: (ms) => Math.round(ms * 48),
    });

    input.emit([(0x2 << 28) | (4 << 24) | (0xd << 20) | (3 << 16) | (64 << 8)], 0.5);
    input.emit([(0x4 << 28) | (4 << 24) | (0xa << 20) | (3 << 16) | (60 << 8), 70 << 25], 1);
    input.emit([(0x4 << 28) | (4 << 24) | (0xd << 20) | (3 << 16), 64 << 25], 2);
    input.emit([(0x4 << 28) | (4 << 24) | (0xe << 20) | (3 << 16), 0x12345678], 3);

    expect(engine.messages).toEqual([
      { kind: 'channel-pressure', group: 4, channel: 3, pressure: 64, time: 24 },
      { kind: 'poly-pressure', group: 4, channel: 3, note: 60, pressure: 70, time: 48 },
      { kind: 'channel-pressure', group: 4, channel: 3, pressure: 64, time: 96 },
      { kind: 'pitch-bend', group: 4, channel: 3, bend14: 0x12345678 >>> 18, time: 144 },
    ]);
    binding.close();
  });

  it('routes each framed UMP packet losslessly when raw input is supported', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('raw-ump');
    access.inputs.set(input.id, input);
    const engine = new RawUmpEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never, {
      timestampToSamples: (ms) => Math.round(ms * 48),
    });

    const midi1NoteOn = 0x21903c64;
    const midi2Cc = [(0x4 << 28) | (3 << 24) | (0xb << 20) | (2 << 16) | (74 << 8), 0x12345678];
    input.emit([midi1NoteOn, ...midi2Cc], 1.5);

    expect(engine.messages).toEqual([]);
    expect(engine.rawUmp).toHaveLength(2);
    expect(engine.rawUmp[0]).toEqual({ words: Uint32Array.from([midi1NoteOn]), time: 72 });
    expect(engine.rawUmp[1]).toEqual({ words: Uint32Array.from(midi2Cc), time: 72 });
    binding.close();
  });

  it('does not pass malformed, truncated, or engine-refused UMP packets to raw input', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('raw-ump-validation');
    access.inputs.set(input.id, input);
    const engine = new ThrowingRawUmpEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never);

    const midi2CcWord0 = (0x4 << 28) | (1 << 24) | (0xb << 20) | (1 << 16) | (7 << 8);
    const data64Word0 = (0x3 << 28) | (1 << 24) | (0x16 << 16);
    const data128Word0 = (0x5 << 28) | (1 << 24);
    expect(() => input.emit([midi2CcWord0])).not.toThrow();
    expect(() => input.emit([data64Word0, 0, midi2CcWord0, 0x12345678])).not.toThrow();
    expect(() => input.emit([data128Word0, 0, 0])).not.toThrow();

    expect(engine.rawUmp).toEqual([
      { words: Uint32Array.from([midi2CcWord0, 0x12345678]), time: 0 },
    ]);
    binding.close();
  });

  it('MT=4 routes only status 0x8 as note-off and preserves zero note-on velocity', async () => {
    const access = new FakeAccess();
    const input = new FakeInput('ump-velocity-boundaries');
    access.inputs.set(input.id, input);
    const engine = new FakeEngine();
    installMidi(access);
    const binding = await bindWebMidi(engine as never);

    const velocities16 = [0, 1, 511, 512, 65535];
    for (const [index, velocity16] of velocities16.entries()) {
      input.emit([
        (0x4 << 28) | (0 << 24) | (0x9 << 20) | (0 << 16) | ((60 + index) << 8),
        velocity16 << 16,
      ]);
    }
    // MIDI 1.0 UMP keeps its existing velocity-zero note-off rule.
    input.emit([0x20903d00]);
    input.emit([(0x4 << 28) | (0 << 24) | (0x8 << 20) | (0 << 16) | (70 << 8), 0]);
    input.emit([(0x4 << 28) | (0 << 24) | (0x8 << 20) | (0 << 16) | (71 << 8), 65535 << 16]);

    expect(engine.messages).toEqual([
      ...velocities16.map((velocity16, index) => ({
        kind: 'on' as const,
        group: 0,
        channel: 0,
        a: 60 + index,
        b: Math.max(1, (velocity16 >>> 9) & 0x7f),
        time: 0,
      })),
      { kind: 'off', group: 0, channel: 0, a: 61, b: 0, time: 0 },
      { kind: 'off', group: 0, channel: 0, a: 70, b: 0, time: 0 },
      { kind: 'off', group: 0, channel: 0, a: 71, b: 127, time: 0 },
    ]);
    binding.close();
  });

  it('calls onInputsChanged exactly once per hotplug event', async () => {
    // Regression test: refreshInputs() used to call notify() internally, and
    // the statechange listener called notify() again unconditionally
    // afterwards, so any event routed through the "no event.port" fallback
    // branch (refreshInputs()) fired onInputsChanged twice.
    const access = new FakeAccess();
    const engine = new FakeEngine();
    installMidi(access);
    const notifications: string[][] = [];
    const binding = await bindWebMidi(engine as never, {
      onInputsChanged: (inputs) => notifications.push(inputs.map((input) => input.id)),
    });
    expect(notifications).toHaveLength(1); // Initial sync.

    const later = new FakeInput('later');
    access.inputs.set(later.id, later);
    access.stateChange(later); // event.port set: the per-port `if` branch.
    expect(notifications).toHaveLength(2);

    access.stateChange(); // No event.port: the refreshInputs() fallback branch.
    expect(notifications).toHaveLength(3);

    binding.close();
  });

  it('invokes requestMIDIAccess with the navigator as `this` (native methods throw otherwise)', async () => {
    const access = new FakeAccess();
    const navigatorValue = {
      async requestMIDIAccess(this: unknown) {
        if (this !== navigatorValue) {
          throw new TypeError(
            "Failed to execute 'requestMIDIAccess' on 'Navigator': Illegal invocation",
          );
        }
        return access;
      },
    };
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: navigatorValue });

    const binding = await bindWebMidi(new FakeEngine() as never);
    expect(binding.inputs()).toEqual([]);
    binding.close();
  });

  it('throws when Web MIDI is unavailable', async () => {
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {} });
    await expect(bindWebMidi(new FakeEngine() as never)).rejects.toThrow(
      'Web MIDI is not available',
    );
  });
});
