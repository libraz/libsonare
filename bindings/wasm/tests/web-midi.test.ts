import { afterEach, describe, expect, it } from 'vitest';
import { bindWebMidi, isWebMidiAvailable } from '../dist/index.js';

type MidiMessage = {
  kind: 'on' | 'off' | 'cc';
  group: number;
  channel: number;
  a: number;
  b: number;
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

  it('throws when Web MIDI is unavailable', async () => {
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {} });
    await expect(bindWebMidi(new FakeEngine() as never)).rejects.toThrow(
      'Web MIDI is not available',
    );
  });
});
