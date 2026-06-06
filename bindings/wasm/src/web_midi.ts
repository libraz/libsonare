import type { MidiCcBindOptions, RealtimeEngine } from './realtime_engine';

type MidiInputState = 'connected' | 'disconnected';

interface MidiPortLike {
  id: string;
  name?: string | null;
  manufacturer?: string | null;
  state?: MidiInputState;
}

interface MidiMessageEventLike {
  data: ArrayLike<number>;
  timeStamp?: number;
  receivedTime?: number;
  target?: MidiPortLike;
  currentTarget?: MidiPortLike;
}

interface MidiInputLike extends MidiPortLike {
  type?: 'input';
  onmidimessage: ((event: MidiMessageEventLike) => void) | null;
  addEventListener?: (type: 'midimessage', listener: (event: MidiMessageEventLike) => void) => void;
  removeEventListener?: (
    type: 'midimessage',
    listener: (event: MidiMessageEventLike) => void,
  ) => void;
}

interface MidiConnectionEventLike {
  port?: MidiPortLike | null;
}

interface MidiAccessLike {
  inputs: Map<string, MidiInputLike> | Iterable<[string, MidiInputLike]>;
  onstatechange: ((event: MidiConnectionEventLike) => void) | null;
  addEventListener?: (
    type: 'statechange',
    listener: (event: MidiConnectionEventLike) => void,
  ) => void;
  removeEventListener?: (
    type: 'statechange',
    listener: (event: MidiConnectionEventLike) => void,
  ) => void;
}

interface NavigatorWithMidi {
  requestMIDIAccess?: (options?: {
    sysex?: boolean;
    software?: boolean;
  }) => Promise<MidiAccessLike>;
}

export interface WebMidiCcBinding {
  channel: number;
  controller: number;
  paramId: number;
  options?: MidiCcBindOptions;
}

export interface WebMidiInputInfo {
  id: string;
  name: string;
  manufacturer: string;
  state: MidiInputState;
}

export interface BindWebMidiOptions {
  /** Realtime-engine MIDI destination receiving the live input source. Default `0`. */
  destinationId?: number;
  /** UMP group used for MIDI 1.0 channel voice events. Default `0`. */
  group?: number;
  /** Restrict binding to specific Web MIDI input ids. Omit or empty = all connected inputs. */
  inputIds?: readonly string[];
  /** Request SysEx-capable access from the browser. Default `false`. */
  sysex?: boolean;
  /** Request software ports from the browser where supported. Default `true`. */
  software?: boolean;
  /** Bind CC-to-parameter mappings before ports are connected. */
  ccBindings?: readonly WebMidiCcBinding[];
  /** Convert a Web MIDI event timestamp to engine port-time samples. */
  timestampToSamples?: (eventTimeMs: number) => number;
  /** Observe hot-plug updates after the helper rebinds matching inputs. */
  onInputsChanged?: (inputs: WebMidiInputInfo[]) => void;
}

export interface WebMidiBinding {
  access: MidiAccessLike;
  inputs(): WebMidiInputInfo[];
  close(): void;
}

type BoundInput = {
  input: MidiInputLike;
  listener: (event: MidiMessageEventLike) => void;
};

export function isWebMidiAvailable(): boolean {
  return (
    typeof (globalThis.navigator as NavigatorWithMidi | undefined)?.requestMIDIAccess === 'function'
  );
}

export async function bindWebMidi(
  engine: RealtimeEngine,
  options: BindWebMidiOptions = {},
): Promise<WebMidiBinding> {
  const navigatorWithMidi = globalThis.navigator as NavigatorWithMidi | undefined;
  if (typeof navigatorWithMidi?.requestMIDIAccess !== 'function') {
    throw new Error('Web MIDI is not available in this environment');
  }

  const group = options.group ?? 0;
  assertNibble('bindWebMidi', group, 'group');
  const destinationId = options.destinationId ?? 0;
  const selectedIds = new Set(options.inputIds ?? []);
  // Invoke through the navigator so the browser's native method keeps its
  // required `this` binding (detached calls throw "Illegal invocation").
  const access = await navigatorWithMidi.requestMIDIAccess({
    sysex: options.sysex ?? false,
    software: options.software ?? true,
  });

  for (const binding of options.ccBindings ?? []) {
    engine.bindMidiCc(binding.channel, binding.controller, binding.paramId, binding.options);
  }
  engine.setMidiInputSource(destinationId);

  const bound = new Map<string, BoundInput>();
  let closed = false;
  let runningStatus = 0;

  const shouldBind = (input: MidiInputLike) =>
    input.state !== 'disconnected' && (selectedIds.size === 0 || selectedIds.has(input.id));

  const snapshotInputs = (): WebMidiInputInfo[] =>
    Array.from(iterInputs(access), ([id, input]) => ({
      id,
      name: input.name ?? '',
      manufacturer: input.manufacturer ?? '',
      state: input.state ?? 'connected',
    }));

  const notify = () => options.onInputsChanged?.(snapshotInputs());

  const bindInput = (input: MidiInputLike) => {
    if (bound.has(input.id) || !shouldBind(input)) {
      return;
    }
    const listener = (event: MidiMessageEventLike) => {
      const status = dispatchMidiMessage(
        engine,
        event,
        group,
        runningStatus,
        options.timestampToSamples,
      );
      runningStatus = status;
    };
    if (input.addEventListener) {
      input.addEventListener('midimessage', listener);
    } else {
      input.onmidimessage = listener;
    }
    bound.set(input.id, { input, listener });
  };

  const unbindInput = (input: MidiInputLike) => {
    const entry = bound.get(input.id);
    if (!entry) {
      return;
    }
    if (entry.input.removeEventListener) {
      entry.input.removeEventListener('midimessage', entry.listener);
    } else if (entry.input.onmidimessage === entry.listener) {
      entry.input.onmidimessage = null;
    }
    bound.delete(input.id);
  };

  const refreshInputs = () => {
    for (const [, entry] of bound) {
      if (!shouldBind(entry.input)) {
        unbindInput(entry.input);
      }
    }
    for (const [, input] of iterInputs(access)) {
      bindInput(input);
    }
    notify();
  };

  const stateListener = (event: MidiConnectionEventLike) => {
    if (closed) {
      return;
    }
    if (event.port && 'onmidimessage' in event.port) {
      const input = event.port as MidiInputLike;
      if (shouldBind(input)) {
        bindInput(input);
      } else {
        unbindInput(input);
      }
    } else {
      refreshInputs();
    }
    notify();
  };

  refreshInputs();
  if (access.addEventListener) {
    access.addEventListener('statechange', stateListener);
  } else {
    access.onstatechange = stateListener;
  }

  return {
    access,
    inputs: snapshotInputs,
    close() {
      closed = true;
      if (access.removeEventListener) {
        access.removeEventListener('statechange', stateListener);
      } else if (access.onstatechange === stateListener) {
        access.onstatechange = null;
      }
      for (const [, entry] of Array.from(bound)) {
        unbindInput(entry.input);
      }
      engine.clearMidiInputSource();
    },
  };
}

function dispatchMidiMessage(
  engine: RealtimeEngine,
  event: MidiMessageEventLike,
  group: number,
  runningStatus: number,
  timestampToSamples?: (eventTimeMs: number) => number,
): number {
  const data = event.data;
  if (data.length === 0) {
    return 0;
  }
  const first = data[0];
  if (first > 0xff) {
    dispatchUmpMessage(
      engine,
      data,
      timestampToSamples?.(event.receivedTime ?? event.timeStamp ?? 0) ?? 0,
    );
    return 0;
  }

  let offset = 0;
  let status = first & 0xff;
  if (status < 0x80) {
    if (runningStatus === 0) {
      return 0;
    }
    status = runningStatus;
  } else {
    offset = 1;
  }

  const message = status & 0xf0;
  const channel = status & 0x0f;
  if (message < 0x80 || message > 0xe0) {
    return status >= 0xf8 ? runningStatus : 0;
  }

  const a = readU7(data, offset);
  const b = readU7(data, offset + 1);
  if (a < 0 || b < 0) {
    return status;
  }

  const portTimeSamples = timestampToSamples
    ? timestampToSamples(event.receivedTime ?? event.timeStamp ?? 0)
    : 0;

  if (message === 0x80) {
    engine.pushMidiInputNoteOff(group, channel, a, b, portTimeSamples);
  } else if (message === 0x90) {
    if (b === 0) {
      engine.pushMidiInputNoteOff(group, channel, a, 0, portTimeSamples);
    } else {
      engine.pushMidiInputNoteOn(group, channel, a, b, portTimeSamples);
    }
  } else if (message === 0xb0 && b >= 0) {
    engine.pushMidiInputCc(group, channel, a, b, portTimeSamples);
  }

  return status;
}

function dispatchUmpMessage(
  engine: RealtimeEngine,
  words: ArrayLike<number>,
  portTimeSamples: number,
): void {
  const word0 = words[0] >>> 0;
  const messageType = word0 >>> 28;
  const group = (word0 >>> 24) & 0x0f;

  if (messageType === 0x2) {
    const status = (word0 >>> 16) & 0xff;
    const message = status & 0xf0;
    const channel = status & 0x0f;
    const a = (word0 >>> 8) & 0x7f;
    const b = word0 & 0x7f;
    if (message === 0x80) {
      engine.pushMidiInputNoteOff(group, channel, a, b, portTimeSamples);
    } else if (message === 0x90) {
      if (b === 0) {
        engine.pushMidiInputNoteOff(group, channel, a, 0, portTimeSamples);
      } else {
        engine.pushMidiInputNoteOn(group, channel, a, b, portTimeSamples);
      }
    } else if (message === 0xb0) {
      engine.pushMidiInputCc(group, channel, a, b, portTimeSamples);
    }
    return;
  }

  if (messageType === 0x4 && words.length >= 2) {
    const status = (word0 >>> 20) & 0x0f;
    const channel = (word0 >>> 16) & 0x0f;
    const data1 = (word0 >>> 8) & 0x7f;
    const word1 = words[1] >>> 0;
    if (status === 0x8) {
      engine.pushMidiInputNoteOff(group, channel, data1, (word1 >>> 25) & 0x7f, portTimeSamples);
    } else if (status === 0x9) {
      const velocity = (word1 >>> 25) & 0x7f;
      if (velocity === 0) {
        engine.pushMidiInputNoteOff(group, channel, data1, 0, portTimeSamples);
      } else {
        engine.pushMidiInputNoteOn(group, channel, data1, velocity, portTimeSamples);
      }
    } else if (status === 0xb) {
      engine.pushMidiInputCc(group, channel, data1, (word1 >>> 25) & 0x7f, portTimeSamples);
    }
  }
}

function readU7(data: ArrayLike<number>, index: number): number {
  if (index >= data.length) {
    return -1;
  }
  const value = data[index];
  if (!Number.isInteger(value) || value < 0 || value > 127) {
    return -1;
  }
  return value;
}

function assertNibble(fnName: string, value: number, field: string): void {
  if (!Number.isInteger(value) || value < 0 || value > 15) {
    throw new RangeError(`${fnName}: ${field} must be an integer in [0, 15]`);
  }
}

function iterInputs(access: MidiAccessLike): Iterable<[string, MidiInputLike]> {
  return access.inputs instanceof Map ? access.inputs.entries() : access.inputs;
}
