import { ErrorCode } from './errors.js';
import { codedError, refuse, refuseType, withEntry } from './internal/entry.js';
import { OwnerEpoch } from './owner_epoch.js';
import type { ProjectMidiCcBinding } from './project_types.js';
import {
  assertFiniteScalar,
  assertNibble,
  assertNonNegativeScalar,
  requestObject,
} from './validation.js';
import { AUDIO_WORKLET_RENDER_QUANTUM } from './worklet/render_quantum.js';

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
    options?: WebMidiEngineInputOptions,
  ): void;
  /** Pushes a live note-off to the input source. `group` and `channel` are 0 to 15; `note` is 0 to 127; `velocity` is 0 to 127 (omitted: 0); `offsetInBlockSamples` is samples into the next drained block (omitted: 0). */
  pushMidiInputNoteOff(
    group: number,
    channel: number,
    note: number,
    velocity?: number,
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /** Pushes a live control change to the input source. `group` and `channel` are 0 to 15; `controller` and `value` are 0 to 127; `offsetInBlockSamples` is samples into the next drained block (omitted: 0). */
  pushMidiInputCc(
    group: number,
    channel: number,
    controller: number,
    value: number,
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /** Pushes a live pitch bend to the input source; `bend14` is unsigned 14-bit with centre 8192 (0 to 16383). Pitch-bend input is dropped when the engine lacks this method. */
  pushMidiInputPitchBend?(
    group: number,
    channel: number,
    bend14: number,
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /** Pushes a live channel pressure to the input source; `pressure` is 0 to 127. Dropped when the engine lacks this method. */
  pushMidiInputChannelPressure?(
    group: number,
    channel: number,
    pressure: number,
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /** Pushes a live polyphonic key pressure to the input source; `note` and `pressure` are 0 to 127. Dropped when the engine lacks this method. */
  pushMidiInputPolyPressure?(
    group: number,
    channel: number,
    note: number,
    pressure: number,
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /**
   * Pushes a live program change to the input source; `program` is 0 to 127 and `bank` is
   * `{ msb, lsb }` (omitted: the bank the receiver latched from CC 0 / CC 32). Dropped when the
   * engine lacks this method.
   */
  pushMidiInputProgram?(
    group: number,
    channel: number,
    program: number,
    bank?: { msb: number; lsb: number },
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /** Pushes one raw UMP message (1 to 4 words) to the input source. When the engine lacks this method, UMP input is lowered to the individual push methods. */
  pushMidiInputUmp?(
    words: Uint32Array | readonly number[],
    offsetInBlockSamples?: number,
    options?: WebMidiEngineInputOptions,
  ): void;
  /**
   * Present on an engine whose pushes take `options.contextTimeSec` (the AudioWorklet
   * `SonareEngine`); {@link BindWebMidiOptions.context} requires it.
   */
  lateMidiEventCount?(): Promise<number>;
}

/** Trailing options of a {@link WebMidiEngine} push; only an engine with `lateMidiEventCount` reads them. */
export interface WebMidiEngineInputOptions {
  /** AudioContext time in seconds the event is due at; exclusive with `offsetInBlockSamples`. */
  contextTimeSec?: number;
}

/**
 * The part of an `AudioContext` that {@link bindWebMidi} reads to place events on its clock.
 * @mirrors AudioContext
 */
export interface WebMidiAudioContext {
  /** Context and performance clocks of the sample currently leaving the output. */
  getOutputTimestamp(): { contextTime?: number; performanceTime?: number };
  /** Seconds the context adds between the render thread and the output; read per event. */
  baseLatency?: number;
  /** Seconds the output device adds; read per event. */
  outputLatency?: number;
  /** Context sample rate in Hz. */
  sampleRate: number;
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
   * Exclusive with {@link context}.
   */
  timestampToOffsetInBlockSamples?: (eventTimeMs: number) => number;
  /**
   * The AudioContext the engine renders in. Each event's timestamp is placed on its clock by
   * {@link webMidiEventContextTime}, delayed by {@link scheduleAheadSec}, and pushed with `contextTimeSec`,
   * so events keep their spacing to the sample instead of bunching at block heads. Requires an
   * engine whose pushes take `contextTimeSec` (the AudioWorklet `SonareEngine`). Exclusive with
   * {@link timestampToOffsetInBlockSamples}.
   */
  context?: WebMidiAudioContext;
  /**
   * Seconds each event is delayed on the context clock in {@link context} mode. Default: the
   * context's `baseLatency + outputLatency` (a missing or non-finite value reads as 0) plus two
   * render quanta, read per event. A smaller value answers sooner and plays more events late, at
   * the next block's start; a larger one keeps every event on its sample at the cost of that delay.
   * `0` is allowed.
   */
  scheduleAheadSec?: number;
  /** Observe hot-plug updates after the helper rebinds matching inputs. */
  onInputsChanged?: (inputs: WebMidiInputInfo[]) => void;
  /**
   * Called with each message the binding is about to push, before it reaches the engine;
   * returning a falsy value drops that message (a MIDI-learn mode, for instance). System messages
   * the binding never pushes are not offered.
   */
  filter?: (message: WebMidiMessage) => boolean;
}

/** One message {@link BindWebMidiOptions.filter} is offered. */
export interface WebMidiMessage {
  /** The input port the message arrived on. */
  input: WebMidiInputInfo;
  /**
   * The message: a complete MIDI 1.0 channel voice message with running status resolved (status
   * byte first), or one UMP packet.
   */
  data: Uint8Array | Uint32Array;
  /** The event's timestamp on the `performance.now()` clock, in milliseconds. */
  eventTimeMs: number;
}

/** Options for {@link webMidiEventContextTime}. */
export interface WebMidiEventContextTimeOptions {
  /** Seconds added on the context clock; default as {@link BindWebMidiOptions.scheduleAheadSec}. */
  scheduleAheadSec?: number;
}

/** Handle returned by {@link bindWebMidi}; call `destroy` to release the inputs. */
export interface WebMidiBinding {
  /** The browser's MIDIAccess object. */
  access: MidiAccessLike;
  /** Current snapshot of every input port, bound or not. */
  inputs(): WebMidiInputInfo[];
  /** Stops listening on every bound input and for hot-plug changes, releases what each input still held, and clears the engine's MIDI input source; both last steps are skipped when a newer binding has replaced the source. Safe to call more than once. */
  destroy(): void;
  /** Same as {@link destroy}, so the binding works with `using`. */
  [Symbol.dispose](): void;
}

type BoundInput = {
  input: MidiInputLike;
  listener: (event: MidiMessageEventLike) => void;
  runningStatus: number;
  held: HeldState;
};

const RESET_ALL_CONTROLLERS = 121;
/** All Notes Off, and the four mode messages that imply it. */
const FIRST_ALL_NOTES_OFF = 123;
const SWITCH_ON_VALUE = 64;
const BEND14_CENTRE = 8192;

/**
 * The controls a channel holds until they are written back to rest, as the engine's release of a
 * replaced MIDI input source reads them: damper, sostenuto and soft pedal at 64 or above, pitch
 * bend off centre. Each entry's `controller` is the CC it is lifted by; the bend has none.
 */
const HELD_CONTROLS = [
  { controller: 64 },
  { controller: 66 },
  { controller: 67 },
  { controller: undefined },
] as const;
const HELD_BEND = HELD_CONTROLS.length - 1;

/** Notes a port started and has not ended, and the controls it holds, per group and channel. */
class HeldState {
  private readonly notes = new Set<number>();
  private readonly controls = new Set<number>();

  noteOn(group: number, channel: number, note: number): void {
    this.notes.add(heldKey(group, channel, note));
  }

  noteOff(group: number, channel: number, note: number): void {
    this.notes.delete(heldKey(group, channel, note));
  }

  controller(group: number, channel: number, controller: number, value7: number): void {
    const control = HELD_CONTROLS.findIndex((entry) => entry.controller === controller);
    if (control >= 0) {
      this.hold(group, channel, control, value7 >= SWITCH_ON_VALUE);
    } else if (controller >= FIRST_ALL_NOTES_OFF) {
      forgetChannel(this.notes, group, channel);
    } else if (controller === RESET_ALL_CONTROLLERS) {
      forgetChannel(this.controls, group, channel);
    }
  }

  bend(group: number, channel: number, offCentre: boolean): void {
    this.hold(group, channel, HELD_BEND, offCentre);
  }

  /** Ends every held note, then returns every held control to rest, and forgets them. */
  release(engine: WebMidiEngine): void {
    for (const key of this.notes) {
      engine.pushMidiInputNoteOff(key >>> 11, (key >>> 7) & 0x0f, key & 0x7f, 0);
    }
    for (const key of this.controls) {
      const group = key >>> 11;
      const channel = (key >>> 7) & 0x0f;
      const controller = HELD_CONTROLS[key & 0x7f].controller;
      if (controller !== undefined) {
        engine.pushMidiInputCc(group, channel, controller, 0);
      } else if (engine.pushMidiInputPitchBend) {
        engine.pushMidiInputPitchBend(group, channel, BEND14_CENTRE);
      } else {
        // A bend reached this engine only as raw UMP; return it to centre the same way.
        engine.pushMidiInputUmp?.([(0x20e00040 | (group << 24) | (channel << 16)) >>> 0]);
      }
    }
    this.notes.clear();
    this.controls.clear();
  }

  private hold(group: number, channel: number, control: number, held: boolean): void {
    const key = heldKey(group, channel, control);
    if (held) {
      this.controls.add(key);
    } else {
      this.controls.delete(key);
    }
  }
}

function heldKey(group: number, channel: number, index: number): number {
  return (group << 11) | (channel << 7) | index;
}

function forgetChannel(keys: Set<number>, group: number, channel: number): void {
  for (const key of keys) {
    if (key >>> 7 === ((group << 4) | channel)) {
      keys.delete(key);
    }
  }
}

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
 * Each input's held notes and its held sustain and sostenuto pedals are tracked, so when the input
 * disconnects or the binding is destroyed, the binding sends that input's note-offs and returns its
 * held controls to rest (damper, sostenuto and soft pedal up, pitch bend to centre), and only that
 * input's.
 *
 * Channel voice messages are forwarded; MIDI 1.0 system messages (0xF0 to 0xFF: SysEx, time code,
 * song position and select, tune request, real-time bytes) and UMP data packets are not.
 *
 * @throws SonareError `NotSupported` when Web MIDI is unavailable.
 * @throws TypeError when `options` is not an object or holds an undeclared key.
 * @throws TypeError when `options.filter` is given and is not a function.
 * @throws RangeError when `options.group` is outside 0 to 15.
 * @throws RangeError when `options.inputIds` is an empty array.
 * @throws TypeError when `options.context` is given with `timestampToOffsetInBlockSamples`, lacks
 *   `getOutputTimestamp`, or is given for an engine without `lateMidiEventCount`; when
 *   `options.scheduleAheadSec` is given without `context` or is not a number.
 * @throws RangeError when `options.scheduleAheadSec` is negative or not finite.
 *
 * @example
 * ```ts
 * // Events keep their device timing to the sample through the AudioWorklet engine.
 * const engine = await SonareEngine.create(context, { numChannels: 2 });
 * const midi = await bindWebMidi(engine, { context });
 * // ...
 * console.log('late events', await engine.lateMidiEventCount());
 * midi.destroy();
 * ```
 */
export async function bindWebMidi(
  engine: WebMidiEngine,
  options: BindWebMidiOptions = {},
): Promise<WebMidiBinding> {
  return withEntry('bindWebMidi', async () => {
    const navigatorWithMidi = globalThis.navigator as NavigatorWithMidi | undefined;
    if (typeof navigatorWithMidi?.requestMIDIAccess !== 'function') {
      throw codedError(ErrorCode.NotSupported, 'Web MIDI is not available in this environment');
    }

    requestObject('bindWebMidi', options, 'options', true);
    const group = options.group ?? 0;
    assertNibble(group, 'group');
    const destinationId = options.destinationId ?? 0;
    if (options.inputIds !== undefined && options.inputIds.length === 0) {
      throw refuse('inputIds must not be empty; omit it to bind every input');
    }
    const selectedIds = new Set(options.inputIds ?? []);
    const timing = resolveEventTiming(engine, options);
    const filter = options.filter;
    if (filter !== undefined && typeof filter !== 'function') {
      throw refuseType('filter must be a function');
    }
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
      Array.from(iterInputs(access), ([id, input]) => inputInfo(input, id));

    const notify = () => options.onInputsChanged?.(snapshotInputs());

    const bindInput = (input: MidiInputLike) => {
      if (bound.has(input.id) || !shouldBind(input)) {
        return;
      }
      const offer: MessageOffer | undefined =
        filter &&
        ((data, eventTimeMs) => Boolean(filter({ input: inputInfo(input), data, eventTimeMs })));
      const entry: BoundInput = {
        input,
        listener: (event: MidiMessageEventLike) => {
          entry.runningStatus = dispatchMidiMessage(
            engine,
            event,
            group,
            entry.runningStatus,
            timing,
            offer,
            entry.held,
          );
        },
        runningStatus: 0,
        held: new HeldState(),
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
      // A replaced input source is its new owner's; the engine releases live notes on a change.
      if (sourceOwner.isCurrent(sourceToken)) {
        entry.held.release(engine);
      }
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
  });
}

/** Snapshot of one port; `id` defaults to the port's own. */
function inputInfo(input: MidiPortLike, id = input.id): WebMidiInputInfo {
  return {
    id,
    name: input.name ?? '',
    manufacturer: input.manufacturer ?? '',
    state: input.state ?? 'connected',
  };
}

/** Offers one message about to be pushed to the binding's filter; false drops it. */
type MessageOffer = (data: Uint8Array | Uint32Array, eventTimeMs: number) => boolean;

/** The trailing push arguments placing one event: a block offset, or a context time the engine maps itself. */
type EventAt = [offsetInBlockSamples?: number, options?: WebMidiEngineInputOptions];

type EventTiming = (eventTimeMs: number) => EventAt;

const AT_BLOCK_HEAD: EventAt = [0];

/** A latency a context reports, read as 0 when missing or not finite. */
function reportedLatency(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0;
}

/** Validates the timing options of {@link bindWebMidi} and returns how each event is placed. */
function resolveEventTiming(engine: WebMidiEngine, options: BindWebMidiOptions): EventTiming {
  const { context, scheduleAheadSec, timestampToOffsetInBlockSamples } = options;
  if (context === undefined) {
    if (scheduleAheadSec !== undefined) {
      throw refuseType('scheduleAheadSec is read only with context');
    }
    if (!timestampToOffsetInBlockSamples) {
      return () => AT_BLOCK_HEAD;
    }
    return (eventTimeMs) => [timestampToOffsetInBlockSamples(eventTimeMs)];
  }
  if (timestampToOffsetInBlockSamples !== undefined) {
    throw refuseType('context and timestampToOffsetInBlockSamples must not both be given');
  }
  if (typeof context?.getOutputTimestamp !== 'function') {
    throw refuseType('context must be an AudioContext with getOutputTimestamp');
  }
  if (typeof engine.lateMidiEventCount !== 'function') {
    throw refuseType(
      'context needs an engine whose live MIDI input pushes take contextTimeSec (the AudioWorklet SonareEngine)',
    );
  }
  if (scheduleAheadSec !== undefined) {
    assertNonNegativeScalar(scheduleAheadSec, 'scheduleAheadSec');
  }
  return (eventTimeMs) => [
    undefined,
    { contextTimeSec: eventContextTime(context, eventTimeMs, scheduleAheadSec) },
  ];
}

/** The one timestamp-to-context-time mapping, over arguments already validated. */
function eventContextTime(
  context: WebMidiAudioContext,
  eventTimeMs: number,
  scheduleAheadSec: number | undefined,
): number {
  const stamp = context.getOutputTimestamp();
  const ahead =
    scheduleAheadSec ??
    reportedLatency(context.baseLatency) +
      reportedLatency(context.outputLatency) +
      (2 * AUDIO_WORKLET_RENDER_QUANTUM) / context.sampleRate;
  const contextTime =
    (stamp.contextTime ?? 0) + (eventTimeMs - (stamp.performanceTime ?? 0)) / 1000;
  return Math.max(0, contextTime + ahead);
}

/**
 * Maps a Web MIDI event timestamp to the AudioContext time {@link bindWebMidi} pushes it at, for a
 * caller that owns its own MIDI listeners and pushes with `{ contextTimeSec }` itself.
 *
 * The timestamp is placed on the context clock through `context.getOutputTimestamp()`, then
 * delayed by `scheduleAheadSec` (default: the context's `baseLatency + outputLatency`, a missing
 * or non-finite value read as 0, plus two render quanta); a time before the context started reads
 * as 0. The clock pair and the latencies are read on every call.
 *
 * @param eventTimeMs The event's `timeStamp` (or `receivedTime`), on the `performance.now()` clock.
 * @returns AudioContext time in seconds.
 * @throws TypeError when `context` has no `getOutputTimestamp`, `eventTimeMs` is not a number, or
 *   `options` is not an object or holds an undeclared key.
 * @throws RangeError when `eventTimeMs` is not finite, or `scheduleAheadSec` is negative or not
 *   finite.
 *
 * @example
 * ```ts
 * input.onmidimessage = (event) => {
 *   const contextTimeSec = webMidiEventContextTime(context, event.timeStamp);
 *   engine.pushMidiInputNoteOn(0, 0, event.data[1], event.data[2], undefined, { contextTimeSec });
 * };
 * ```
 */
export function webMidiEventContextTime(
  context: WebMidiAudioContext,
  eventTimeMs: number,
  options?: WebMidiEventContextTimeOptions,
): number {
  return withEntry('webMidiEventContextTime', () => {
    if (typeof context?.getOutputTimestamp !== 'function') {
      throw refuseType('context must be an AudioContext with getOutputTimestamp');
    }
    assertFiniteScalar(eventTimeMs, 'eventTimeMs');
    const scheduleAheadSec =
      options === undefined
        ? undefined
        : requestObject('webMidiEventContextTime', options, 'options', true).scheduleAheadSec;
    if (scheduleAheadSec !== undefined) {
      assertNonNegativeScalar(scheduleAheadSec, 'scheduleAheadSec');
    }
    return eventContextTime(context, eventTimeMs, scheduleAheadSec);
  });
}

function dispatchMidiMessage(
  engine: WebMidiEngine,
  event: MidiMessageEventLike,
  group: number,
  runningStatus: number,
  timing: EventTiming,
  offer: MessageOffer | undefined,
  held: HeldState,
): number {
  const data = event.data;
  if (data.length === 0) {
    return 0;
  }
  const eventTimeMs = event.receivedTime ?? event.timeStamp ?? 0;
  const first = data[0];
  if (first > 0xff || data instanceof Uint32Array) {
    dispatchUmpMessages(engine, data, eventTimeMs, timing, offer, held);
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

  if (
    offer &&
    !offer(oneDataByteMessage ? Uint8Array.of(status, a) : Uint8Array.of(status, a, b), eventTimeMs)
  ) {
    return status;
  }
  const at = timing(eventTimeMs);

  if (message === 0x80) {
    engine.pushMidiInputNoteOff(group, channel, a, b, ...at);
    held.noteOff(group, channel, a);
  } else if (message === 0x90) {
    if (b === 0) {
      engine.pushMidiInputNoteOff(group, channel, a, 0, ...at);
      held.noteOff(group, channel, a);
    } else {
      engine.pushMidiInputNoteOn(group, channel, a, b, ...at);
      held.noteOn(group, channel, a);
    }
  } else if (message === 0xb0 && b >= 0) {
    engine.pushMidiInputCc(group, channel, a, b, ...at);
    held.controller(group, channel, a, b);
  } else if (message === 0xa0 && b >= 0) {
    engine.pushMidiInputPolyPressure?.(group, channel, a, b, ...at);
  } else if (message === 0xd0) {
    engine.pushMidiInputChannelPressure?.(group, channel, a, ...at);
  } else if (message === 0xe0 && b >= 0 && engine.pushMidiInputPitchBend) {
    engine.pushMidiInputPitchBend(group, channel, a | (b << 7), ...at);
    held.bend(group, channel, (a | (b << 7)) !== BEND14_CENTRE);
  } else if (message === 0xc0) {
    engine.pushMidiInputProgram?.(group, channel, a, undefined, ...at);
  }

  return status;
}

function dispatchUmpMessages(
  engine: WebMidiEngine,
  words: ArrayLike<number>,
  eventTimeMs: number,
  timing: EventTiming,
  offer: MessageOffer | undefined,
  held: HeldState,
): void {
  let at: EventAt | undefined;
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
    // Data packets (MT 0x3/0x5) need the owned SysEx path; raw input refuses them.
    const messageType = packet[0] >>> 28;
    if (messageType !== 0x3 && messageType !== 0x5 && (!offer || offer(packet, eventTimeMs))) {
      at ??= timing(eventTimeMs);
      dispatchUmpMessage(engine, packet, at);
      trackUmpMessage(
        held,
        packet,
        Boolean(engine.pushMidiInputUmp || engine.pushMidiInputPitchBend),
      );
    }
    offset += wordCount;
  }
}

function dispatchUmpMessage(engine: WebMidiEngine, words: Uint32Array, at: EventAt): void {
  const word0 = words[0] >>> 0;
  const messageType = word0 >>> 28;
  const group = (word0 >>> 24) & 0x0f;

  if (engine.pushMidiInputUmp) {
    engine.pushMidiInputUmp(words, ...at);
    return;
  }

  if (messageType === 0x2) {
    const status = (word0 >>> 16) & 0xff;
    const message = status & 0xf0;
    const channel = status & 0x0f;
    const a = (word0 >>> 8) & 0x7f;
    const b = word0 & 0x7f;
    if (message === 0x80) {
      engine.pushMidiInputNoteOff(group, channel, a, b, ...at);
    } else if (message === 0x90) {
      if (b === 0) {
        engine.pushMidiInputNoteOff(group, channel, a, 0, ...at);
      } else {
        engine.pushMidiInputNoteOn(group, channel, a, b, ...at);
      }
    } else if (message === 0xb0) {
      engine.pushMidiInputCc(group, channel, a, b, ...at);
    } else if (message === 0xa0) {
      engine.pushMidiInputPolyPressure?.(group, channel, a, b, ...at);
    } else if (message === 0xd0) {
      engine.pushMidiInputChannelPressure?.(group, channel, a, ...at);
    } else if (message === 0xe0) {
      engine.pushMidiInputPitchBend?.(group, channel, a | (b << 7), ...at);
    } else if (message === 0xc0) {
      engine.pushMidiInputProgram?.(group, channel, a, undefined, ...at);
    }
    return;
  }

  if (messageType === 0x4 && words.length >= 2) {
    const status = (word0 >>> 20) & 0x0f;
    const channel = (word0 >>> 16) & 0x0f;
    const data1 = (word0 >>> 8) & 0x7f;
    const word1 = words[1] >>> 0;
    if (status === 0x8) {
      engine.pushMidiInputNoteOff(group, channel, data1, (word1 >>> 25) & 0x7f, ...at);
    } else if (status === 0x9) {
      // MIDI 2.0 note-on is distinct from note-off (status 0x8). Downsample
      // its 16-bit velocity to the engine's 7-bit API, retaining a nonzero
      // note-on even when the top seven bits are all zero.
      const velocity = Math.max(1, (word1 >>> 25) & 0x7f);
      engine.pushMidiInputNoteOn(group, channel, data1, velocity, ...at);
    } else if (status === 0xb) {
      engine.pushMidiInputCc(group, channel, data1, (word1 >>> 25) & 0x7f, ...at);
    } else if (status === 0xa) {
      engine.pushMidiInputPolyPressure?.(group, channel, data1, (word1 >>> 25) & 0x7f, ...at);
    } else if (status === 0xd) {
      engine.pushMidiInputChannelPressure?.(group, channel, (word1 >>> 25) & 0x7f, ...at);
    } else if (status === 0xe) {
      engine.pushMidiInputPitchBend?.(group, channel, word1 >>> 18, ...at);
    } else if (status === 0xc) {
      const bank = (word0 & 1) === 1 ? { msb: (word1 >>> 8) & 0x7f, lsb: word1 & 0x7f } : undefined;
      engine.pushMidiInputProgram?.(group, channel, (word1 >>> 24) & 0x7f, bank, ...at);
    }
  }
}

/** Records the notes and pedals one pushed UMP channel voice packet starts or ends. */
function trackUmpMessage(held: HeldState, words: Uint32Array, bendPushed: boolean): void {
  const word0 = words[0] >>> 0;
  const messageType = word0 >>> 28;
  const group = (word0 >>> 24) & 0x0f;
  const status = (word0 >>> 20) & 0x0f;
  const channel = (word0 >>> 16) & 0x0f;
  const index = (word0 >>> 8) & 0x7f;
  if (messageType === 0x2) {
    const value = word0 & 0x7f;
    if (status === 0x8 || (status === 0x9 && value === 0)) {
      held.noteOff(group, channel, index);
    } else if (status === 0x9) {
      held.noteOn(group, channel, index);
    } else if (status === 0xb) {
      held.controller(group, channel, index, value);
    } else if (status === 0xe && bendPushed) {
      held.bend(group, channel, (index | (value << 7)) !== BEND14_CENTRE);
    }
  } else if (messageType === 0x4 && words.length >= 2) {
    // A MIDI 2.0 note-on with velocity 0 is still a note-on.
    if (status === 0x8) {
      held.noteOff(group, channel, index);
    } else if (status === 0x9) {
      held.noteOn(group, channel, index);
    } else if (status === 0xb) {
      held.controller(group, channel, index, words[1] >>> 25);
    } else if (status === 0xe && bendPushed) {
      held.bend(group, channel, words[1] >>> 0 !== 0x80000000);
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
