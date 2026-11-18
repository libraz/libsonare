import { ErrorCode, SonareError } from './errors.js';
import { OwnerEpoch } from './owner_epoch.js';
import type { ProjectMidiCcBinding } from './project_types.js';
import { assertNibble } from './validation.js';

/** The slice of {@link RealtimeEngine} that {@link bindWebMidi} drives. */
export interface WebMidiEngine {
  /** Maps a MIDI controller to an engine parameter. */
  bindMidiCc(binding: ProjectMidiCcBinding): void;
  /** Enables the engine-owned live MIDI input source for a MIDI destination (omitted: 0). */
  setMidiInputSource(destinationId?: number): void;
  /** Disables the engine-owned live MIDI input source. */
  clearMidiInputSource(): void;
  /** Pushes a live note-on to the input source. `group` and `channel` are 0 to 15; `note` and `velocity` are 0 to 127; `offsetInBlockSamples` is samples into the next drained block (omitted: 0). */
  pushMidiInputNoteOn(
    group: number,
    channel: number,
    note: number,
    velocity: number,
    offsetInBlockSamples?: number,
  ): void;
  /** Pushes a live note-off to the input source. `group` and `channel` are 0 to 15; `note` is 0 to 127; `velocity` is 0 to 127 (omitted: 0); `offsetInBlockSamples` is samples into the next drained block (omitted: 0). */
  pushMidiInputNoteOff(
    group: number,
    channel: number,
    note: number,
    velocity?: number,
    offsetInBlockSamples?: number,
  ): void;
  /** Pushes a live control change to the input source. `group` and `channel` are 0 to 15; `controller` and `value` are 0 to 127; `offsetInBlockSamples` is samples into the next drained block (omitted: 0). */
  pushMidiInputCc(
    group: number,
    channel: number,
    controller: number,
    value: number,
    offsetInBlockSamples?: number,
  ): void;
  /** Pushes a live pitch bend to the input source; `bend14` is unsigned 14-bit with centre 8192 (0 to 16383). Pitch-bend input is dropped when the engine lacks this method. */
  pushMidiInputPitchBend?(
    group: number,
    channel: number,
    bend14: number,
    offsetInBlockSamples?: number,
  ): void;
  /** Pushes a live channel pressure to the input source; `pressure` is 0 to 127. Dropped when the engine lacks this method. */
  pushMidiInputChannelPressure?(
    group: number,
    channel: number,
    pressure: number,
    offsetInBlockSamples?: number,
  ): void;
  /** Pushes a live polyphonic key pressure to the input source; `note` and `pressure` are 0 to 127. Dropped when the engine lacks this method. */
  pushMidiInputPolyPressure?(
    group: number,
    channel: number,
    note: number,
    pressure: number,
    offsetInBlockSamples?: number,
  ): void;
  /** Pushes one raw UMP message (1 to 4 words) to the input source. When the engine lacks this method, UMP input is lowered to the individual push methods. */
  pushMidiInputUmp?(words: Uint32Array | readonly number[], offsetInBlockSamples?: number): void;
}

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

/** Snapshot of one Web MIDI input port. */
export interface WebMidiInputInfo {
  /** Browser-assigned port id; pass it in `inputIds` to restrict binding. */
  id: string;
  /** Port name; empty when the browser reports none. */
  name: string;
  /** Port manufacturer; empty when the browser reports none. */
  manufacturer: string;
  /** Port connection state; ports reported without a state read as `'connected'`. */
  state: MidiInputState;
}

/** Options for {@link bindWebMidi}. */
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
  ccBindings?: readonly ProjectMidiCcBinding[];
  /**
   * Convert a Web MIDI event timestamp to an offset in samples into the next block drained (clamped
   * to its last frame, negative reads as 0). Return a block-relative offset, not the timestamp
   * scaled to samples: an absolute value lands every event on one frame. Omit for offset 0.
   */
  timestampToOffsetInBlockSamples?: (eventTimeMs: number) => number;
  /** Observe hot-plug updates after the helper rebinds matching inputs. */
  onInputsChanged?: (inputs: WebMidiInputInfo[]) => void;
}

/** Handle returned by {@link bindWebMidi}; call `destroy` to release the inputs. */
export interface WebMidiBinding {
  /** The browser's MIDIAccess object. */
  access: MidiAccessLike;
  /** Current snapshot of every input port, bound or not. */
  inputs(): WebMidiInputInfo[];
  /** Stops listening on every bound input and for hot-plug changes, and clears the engine's MIDI input source unless a newer binding has replaced it. Safe to call more than once. */
  destroy(): void;
  /** Same as {@link destroy}, so the binding works with `using`. */
  [Symbol.dispose](): void;
}

type BoundInput = {
  input: MidiInputLike;
  listener: (event: MidiMessageEventLike) => void;
  runningStatus: number;
};

// Owner of each engine's single MIDI input source: the binding that installed it
// last. A replaced binding's destroy leaves its successor's source in place.
const midiInputSourceOwners = new WeakMap<WebMidiEngine, OwnerEpoch>();

function midiInputSourceOwner(engine: WebMidiEngine): OwnerEpoch {
  let owner = midiInputSourceOwners.get(engine);
  if (!owner) {
    owner = new OwnerEpoch('Web MIDI binding');
    midiInputSourceOwners.set(engine, owner);
  }
  return owner;
}

/** Whether the environment exposes `navigator.requestMIDIAccess`. */
export function isWebMidiAvailable(): boolean {
  return (
    typeof (globalThis.navigator as NavigatorWithMidi | undefined)?.requestMIDIAccess === 'function'
  );
}

/**
 * Routes Web MIDI input ports into a realtime engine's live MIDI input source and tracks hot-plugged
 * ports until the binding is destroyed. Only one binding per engine owns the input source; a later one
 * replaces it.
 *
 * @throws SonareError `NotSupported` when Web MIDI is unavailable.
 * @throws RangeError when `options.group` is outside 0 to 15.
 */
export async function bindWebMidi(
  engine: WebMidiEngine,
  options: BindWebMidiOptions = {},
): Promise<WebMidiBinding> {
  const navigatorWithMidi = globalThis.navigator as NavigatorWithMidi | undefined;
  if (typeof navigatorWithMidi?.requestMIDIAccess !== 'function') {
    throw new SonareError(ErrorCode.NotSupported, 'Web MIDI is not available in this environment');
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
    engine.bindMidiCc(binding);
  }
  engine.setMidiInputSource(destinationId);
  const sourceOwner = midiInputSourceOwner(engine);
  const sourceToken = sourceOwner.advance();

  const bound = new Map<string, BoundInput>();
  let closed = false;

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
    const entry: BoundInput = {
      input,
      listener: (event: MidiMessageEventLike) => {
        entry.runningStatus = dispatchMidiMessage(
          engine,
          event,
          group,
          entry.runningStatus,
          options.timestampToOffsetInBlockSamples,
        );
      },
      runningStatus: 0,
    };
    const listener = entry.listener;
    if (input.addEventListener) {
      input.addEventListener('midimessage', listener);
    } else {
      input.onmidimessage = listener;
    }
    bound.set(input.id, entry);
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

  // Does not call notify() itself: both call sites below (the initial sync
  // and stateListener's fallback branch) already notify exactly once after
  // calling this, so notifying here too would fire onInputsChanged twice per
  // hotplug event that goes through the fallback branch.
  const refreshInputs = () => {
    for (const [, entry] of bound) {
      if (!shouldBind(entry.input)) {
        unbindInput(entry.input);
      }
    }
    for (const [, input] of iterInputs(access)) {
      bindInput(input);
    }
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
  notify();
  if (access.addEventListener) {
    access.addEventListener('statechange', stateListener);
  } else {
    access.onstatechange = stateListener;
  }

  const binding: WebMidiBinding = {
    access,
    inputs: snapshotInputs,
    destroy() {
      if (closed) {
        return;
      }
      closed = true;
      if (access.removeEventListener) {
        access.removeEventListener('statechange', stateListener);
      } else if (access.onstatechange === stateListener) {
        access.onstatechange = null;
      }
      for (const [, entry] of Array.from(bound)) {
        unbindInput(entry.input);
      }
      if (sourceOwner.isCurrent(sourceToken)) {
        engine.clearMidiInputSource();
      }
    },
    [Symbol.dispose]() {
      this.destroy();
    },
  };
  return binding;
}

function dispatchMidiMessage(
  engine: WebMidiEngine,
  event: MidiMessageEventLike,
  group: number,
  runningStatus: number,
  timestampToOffsetInBlockSamples?: (eventTimeMs: number) => number,
): number {
  const data = event.data;
  if (data.length === 0) {
    return 0;
  }
  const first = data[0];
  if (first > 0xff || data instanceof Uint32Array) {
    dispatchUmpMessages(
      engine,
      data,
      timestampToOffsetInBlockSamples?.(event.receivedTime ?? event.timeStamp ?? 0) ?? 0,
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
  if (a < 0) {
    return status;
  }
  const oneDataByteMessage = message === 0xc0 || message === 0xd0;
  const b = oneDataByteMessage ? -1 : readU7(data, offset + 1);
  if (!oneDataByteMessage && b < 0) {
    return status;
  }

  const offsetInBlockSamples = timestampToOffsetInBlockSamples
    ? timestampToOffsetInBlockSamples(event.receivedTime ?? event.timeStamp ?? 0)
    : 0;

  if (message === 0x80) {
    engine.pushMidiInputNoteOff(group, channel, a, b, offsetInBlockSamples);
  } else if (message === 0x90) {
    if (b === 0) {
      engine.pushMidiInputNoteOff(group, channel, a, 0, offsetInBlockSamples);
    } else {
      engine.pushMidiInputNoteOn(group, channel, a, b, offsetInBlockSamples);
    }
  } else if (message === 0xb0 && b >= 0) {
    engine.pushMidiInputCc(group, channel, a, b, offsetInBlockSamples);
  } else if (message === 0xa0 && b >= 0) {
    engine.pushMidiInputPolyPressure?.(group, channel, a, b, offsetInBlockSamples);
  } else if (message === 0xd0) {
    engine.pushMidiInputChannelPressure?.(group, channel, a, offsetInBlockSamples);
  } else if (message === 0xe0 && b >= 0) {
    engine.pushMidiInputPitchBend?.(group, channel, a | (b << 7), offsetInBlockSamples);
  }

  return status;
}

function dispatchUmpMessages(
  engine: WebMidiEngine,
  words: ArrayLike<number>,
  offsetInBlockSamples: number,
): void {
  let offset = 0;
  while (offset < words.length) {
    const word0 = words[offset] >>> 0;
    const wordCount = umpWordCount(word0 >>> 28);
    if (words.length - offset < wordCount) {
      return;
    }
    const packet = new Uint32Array(wordCount);
    for (let index = 0; index < wordCount; index++) {
      packet[index] = words[offset + index] >>> 0;
    }
    dispatchUmpMessage(engine, packet, offsetInBlockSamples);
    offset += wordCount;
  }
}

function dispatchUmpMessage(
  engine: WebMidiEngine,
  words: Uint32Array,
  offsetInBlockSamples: number,
): void {
  const word0 = words[0] >>> 0;
  const messageType = word0 >>> 28;
  const group = (word0 >>> 24) & 0x0f;

  // Data packets (MT 0x3/0x5) need the owned SysEx path; raw input refuses them.
  if (messageType === 0x3 || messageType === 0x5) {
    return;
  }
  if (engine.pushMidiInputUmp) {
    engine.pushMidiInputUmp(words, offsetInBlockSamples);
    return;
  }

  if (messageType === 0x2) {
    const status = (word0 >>> 16) & 0xff;
    const message = status & 0xf0;
    const channel = status & 0x0f;
    const a = (word0 >>> 8) & 0x7f;
    const b = word0 & 0x7f;
    if (message === 0x80) {
      engine.pushMidiInputNoteOff(group, channel, a, b, offsetInBlockSamples);
    } else if (message === 0x90) {
      if (b === 0) {
        engine.pushMidiInputNoteOff(group, channel, a, 0, offsetInBlockSamples);
      } else {
        engine.pushMidiInputNoteOn(group, channel, a, b, offsetInBlockSamples);
      }
    } else if (message === 0xb0) {
      engine.pushMidiInputCc(group, channel, a, b, offsetInBlockSamples);
    } else if (message === 0xa0) {
      engine.pushMidiInputPolyPressure?.(group, channel, a, b, offsetInBlockSamples);
    } else if (message === 0xd0) {
      engine.pushMidiInputChannelPressure?.(group, channel, a, offsetInBlockSamples);
    } else if (message === 0xe0) {
      engine.pushMidiInputPitchBend?.(group, channel, a | (b << 7), offsetInBlockSamples);
    }
    return;
  }

  if (messageType === 0x4 && words.length >= 2) {
    const status = (word0 >>> 20) & 0x0f;
    const channel = (word0 >>> 16) & 0x0f;
    const data1 = (word0 >>> 8) & 0x7f;
    const word1 = words[1] >>> 0;
    if (status === 0x8) {
      engine.pushMidiInputNoteOff(
        group,
        channel,
        data1,
        (word1 >>> 25) & 0x7f,
        offsetInBlockSamples,
      );
    } else if (status === 0x9) {
      // MIDI 2.0 note-on is distinct from note-off (status 0x8). Downsample
      // its 16-bit velocity to the engine's 7-bit API, retaining a nonzero
      // note-on even when the top seven bits are all zero.
      const velocity = Math.max(1, (word1 >>> 25) & 0x7f);
      engine.pushMidiInputNoteOn(group, channel, data1, velocity, offsetInBlockSamples);
    } else if (status === 0xb) {
      engine.pushMidiInputCc(group, channel, data1, (word1 >>> 25) & 0x7f, offsetInBlockSamples);
    } else if (status === 0xa) {
      engine.pushMidiInputPolyPressure?.(
        group,
        channel,
        data1,
        (word1 >>> 25) & 0x7f,
        offsetInBlockSamples,
      );
    } else if (status === 0xd) {
      engine.pushMidiInputChannelPressure?.(
        group,
        channel,
        (word1 >>> 25) & 0x7f,
        offsetInBlockSamples,
      );
    } else if (status === 0xe) {
      engine.pushMidiInputPitchBend?.(group, channel, word1 >>> 18, offsetInBlockSamples);
    }
  }
}

function umpWordCount(messageType: number): number {
  switch (messageType & 0x0f) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x6:
    case 0x7:
      return 1;
    case 0x3:
    case 0x4:
    case 0x8:
    case 0x9:
    case 0xa:
      return 2;
    case 0xb:
    case 0xc:
      return 3;
    default:
      return 4;
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

function iterInputs(access: MidiAccessLike): Iterable<[string, MidiInputLike]> {
  return access.inputs instanceof Map ? access.inputs.entries() : access.inputs;
}
