import { assertDeclaredKeys, refuse, refuseType, withEntry } from './internal/entry.js';
import {
  assertMidiCcBindingListBytes,
  assertMidiCcEventPayload,
  assertMidiCcEventPayloads,
} from './internal/midi_cc_binding.js';
import { packMidi2Program } from './internal/midi_program_pack.js';
import {
  assertMidiBooleanShape,
  assertMidiCcBindingListShapes,
  assertMidiNumberShapes,
} from './internal/midi_shape.js';
import {
  assertEventPpq,
  midi2Byte,
  midi2Event,
  midi2I32,
  midi2U16,
  midi2U32,
} from './internal/ump_events.js';
import { isAbsent, isNonArrayObject } from './internal/value_guards.js';
import {
  projectAutomationPointFromNative,
  projectMidi1Event,
  projectModule,
} from './project_internal.js';
import type {
  ProjectAutomationPoint,
  ProjectMidiCcBinding,
  ProjectMidiEvent,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
} from './project_types.js';
import type {
  Gm2DrumNameRequest,
  Gm2InstrumentNameRequest,
  Midi2AssignableControllerRequest,
  Midi2AssignablePerNoteControllerRequest,
  Midi2CcRequest,
  Midi2ChannelPressureRequest,
  Midi2NoteOffRequest,
  Midi2NoteOnRequest,
  Midi2PerNoteManagementRequest,
  Midi2PerNotePitchBendRequest,
  Midi2PitchBendRequest,
  Midi2PolyPressureRequest,
  Midi2ProgramRequest,
  Midi2RegisteredControllerRequest,
  Midi2RegisteredPerNoteControllerRequest,
  Midi2RelativeAssignableControllerRequest,
  Midi2RelativeRegisteredControllerRequest,
  MidiBank,
  MidiBankProgramRequest,
  MidiCcLearnRequest,
  MidiCcRequest,
  MidiCcToBreakpointRequest,
  MidiChannelPressureRequest,
  MidiNoteOffRequest,
  MidiNoteOnRequest,
  MidiParamToCcRequest,
  MidiPitchBendRequest,
  MidiPolyPressureRequest,
  MidiProgramRequest,
  MidiRouteEventsRequest,
} from './public_types_midi_messages.js';
import { assertBoundedInteger, optionalBoolean, requestObject } from './validation.js';

/** A required number field: anything else is a `TypeError`. */
function num(value: unknown, name: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${name} must be a number`);
  }
  return value;
}

/** An integer in `[0, max]`: a non-number is a `TypeError`, anything else outside a `RangeError`. */
function boundedInt(value: unknown, argName: string, max: number): number {
  assertBoundedInteger(value, argName, 0, max);
  return value as number;
}

/** An optional {@link boundedInt}: absent (`undefined`/`null`) takes the fallback. */
function boundedIntOr(value: unknown, argName: string, max: number, fallback: number): number {
  return isAbsent(value) ? fallback : boundedInt(value, argName, max);
}

/** Reads a bank object's two byte fields. */
function bankPair(
  value: unknown,
  readByte = (field: unknown, subject: string) => boundedInt(field, subject, 127),
): MidiBank {
  if (!isNonArrayObject(value)) {
    throw refuseType(`bank must be an object { msb, lsb }`);
  }
  const { msb, lsb } = value as Record<string, unknown>;
  assertDeclaredKeys(value, ['msb', 'lsb'], 'bank');
  return {
    msb: readByte(msb, 'bank.msb'),
    lsb: readByte(lsb, 'bank.lsb'),
  };
}

/** A MIDI 1.0 channel-voice event from a request carrying `ppq`, `group` and `channel`. */
function midi1(
  requestName: 'midiNoteOn' | 'midiCc' | 'midiPolyPressure' | 'midiProgram' | 'midiChannelPressure',
  request: unknown,
  status: number,
  data1: string,
  data2?: string,
): ProjectMidiEvent {
  const r = requestObject(requestName, request, 'request', true) as Record<string, unknown>;
  return projectMidi1Event(
    num(r.ppq, 'ppq'),
    boundedInt(r.group, 'group', 15),
    status,
    boundedInt(r.channel, 'channel', 15),
    boundedInt(r[data1], data1, 127),
    data2 === undefined ? 0 : boundedInt(r[data2], data2, 127),
  );
}

/**
 * Pack a MIDI 1.0 note-on event accepted by `Project.setMidiEvents`.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiNoteOn(request: MidiNoteOnRequest): ProjectMidiEvent {
  return withEntry('midiNoteOn', () => {
    return midi1('midiNoteOn', request, 0x9, 'note', 'velocity');
  });
}

/**
 * Pack a MIDI 1.0 note-off event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiNoteOff(request: MidiNoteOffRequest): ProjectMidiEvent {
  return withEntry('midiNoteOff', () => {
    const fn = 'midiNoteOff';
    const r = requestObject(fn, request, 'request', true);
    return projectMidi1Event(
      num(r.ppq, 'ppq'),
      boundedInt(r.group, 'group', 15),
      0x8,
      boundedInt(r.channel, 'channel', 15),
      boundedInt(r.note, 'note', 127),
      boundedIntOr(r.velocity, 'velocity', 127, 0),
    );
  });
}

/**
 * Pack a MIDI 1.0 control-change event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiCc(request: MidiCcRequest): ProjectMidiEvent {
  return withEntry('midiCc', () => {
    return midi1('midiCc', request, 0xb, 'controller', 'value');
  });
}

/**
 * Pack a MIDI 1.0 poly-pressure event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiPolyPressure(request: MidiPolyPressureRequest): ProjectMidiEvent {
  return withEntry('midiPolyPressure', () => {
    return midi1('midiPolyPressure', request, 0xa, 'note', 'pressure');
  });
}

/**
 * Pack a MIDI 1.0 program-change event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiProgram(request: MidiProgramRequest): ProjectMidiEvent {
  return withEntry('midiProgram', () => {
    return midi1('midiProgram', request, 0xc, 'program');
  });
}

/**
 * Pack a MIDI 1.0 channel-pressure event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiChannelPressure(request: MidiChannelPressureRequest): ProjectMidiEvent {
  return withEntry('midiChannelPressure', () => {
    return midi1('midiChannelPressure', request, 0xd, 'pressure');
  });
}

/**
 * Pack a MIDI 1.0 pitch-bend event (`bend` is unsigned 14-bit, center = 8192).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midiPitchBend(request: MidiPitchBendRequest): ProjectMidiEvent {
  return withEntry('midiPitchBend', () => {
    const fn = 'midiPitchBend';
    const r = requestObject(fn, request, 'request', true);
    const bend = boundedInt(r.bend, 'bend', 0x3fff);
    return projectMidi1Event(
      num(r.ppq, 'ppq'),
      boundedInt(r.group, 'group', 15),
      0xe,
      boundedInt(r.channel, 'channel', 15),
      bend & 0x7f,
      bend >> 7,
    );
  });
}

/**
 * Pack a MIDI 2.0 note-on event. `velocity16` is the full 16-bit velocity (0 is
 * a sounding note-on); `attributeType` 0 is none, 3 is pitch 7.9.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2NoteOn(request: Midi2NoteOnRequest): ProjectMidiEvent {
  return withEntry('midi2NoteOn', () => {
    const fn = 'midi2NoteOn';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const note = boundedInt(r.note, 'note', 127);
    const vel = midi2U16(r.velocity16, 'velocity16');
    const type = midi2Byte(r.attributeType ?? 0, 'attributeType');
    const data = midi2U16(r.attributeData ?? 0, 'attributeData');
    return midi2Event(ppq, group, 0x9, channel, note, type, ((vel << 16) | data) >>> 0);
  });
}

/**
 * Pack a MIDI 2.0 note-off event (`velocity16` is the full 16-bit release velocity).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2NoteOff(request: Midi2NoteOffRequest): ProjectMidiEvent {
  return withEntry('midi2NoteOff', () => {
    const fn = 'midi2NoteOff';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const note = boundedInt(r.note, 'note', 127);
    const vel = midi2U16(r.velocity16 ?? 0, 'velocity16');
    return midi2Event(ppq, group, 0x8, channel, note, 0, (vel << 16) >>> 0);
  });
}

/**
 * Pack a MIDI 2.0 control-change event (`value32` is the full 32-bit value).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2Cc(request: Midi2CcRequest): ProjectMidiEvent {
  return withEntry('midi2Cc', () => {
    const fn = 'midi2Cc';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const controller = boundedInt(r.controller, 'controller', 127);
    const value = midi2U32(r.value32, 'value32');
    return midi2Event(ppq, group, 0xb, channel, controller, 0, value);
  });
}

/**
 * Pack a MIDI 2.0 poly-pressure event (`pressure32` is the full 32-bit pressure).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2PolyPressure(request: Midi2PolyPressureRequest): ProjectMidiEvent {
  return withEntry('midi2PolyPressure', () => {
    const fn = 'midi2PolyPressure';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const note = boundedInt(r.note, 'note', 127);
    const pressure = midi2U32(r.pressure32, 'pressure32');
    return midi2Event(ppq, group, 0xa, channel, note, 0, pressure);
  });
}

/**
 * Pack a MIDI 2.0 channel-pressure event (`pressure32` is the full 32-bit pressure).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2ChannelPressure(request: Midi2ChannelPressureRequest): ProjectMidiEvent {
  return withEntry('midi2ChannelPressure', () => {
    const fn = 'midi2ChannelPressure';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const pressure = midi2U32(r.pressure32, 'pressure32');
    return midi2Event(ppq, group, 0xd, channel, 0, 0, pressure);
  });
}

/**
 * Pack a MIDI 2.0 pitch-bend event (`bend32` is unsigned 32-bit, center = 0x80000000).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2PitchBend(request: Midi2PitchBendRequest): ProjectMidiEvent {
  return withEntry('midi2PitchBend', () => {
    const fn = 'midi2PitchBend';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const bend = midi2U32(r.bend32, 'bend32');
    return midi2Event(ppq, group, 0xe, channel, 0, 0, bend);
  });
}

/**
 * Pack a MIDI 2.0 program-change event. The bank travels in the same message;
 * omit `bank` for none.
 * @throws {TypeError} A non-object request, an unknown key or a wrong-typed field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2Program(request: Midi2ProgramRequest): ProjectMidiEvent {
  return withEntry('midi2Program', () => {
    const fn = 'midi2Program';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    assertEventPpq(ppq);
    const bank = isAbsent(r.bank) ? undefined : bankPair(r.bank);
    const [word0, word1] = packMidi2Program(
      boundedInt(r.group, 'group', 15),
      boundedInt(r.channel, 'channel', 15),
      boundedInt(r.program, 'program', 127),
      bank !== undefined,
      bank?.msb ?? 0,
      bank?.lsb ?? 0,
    );
    return { ppq, word0, word1 };
  });
}

/** The shared reader of the four bank/index controller messages. */
function bankController(
  requestName:
    | 'midi2RegisteredController'
    | 'midi2AssignableController'
    | 'midi2RelativeRegisteredController'
    | 'midi2RelativeAssignableController',
  request: unknown,
  opcode: number,
  valueName: 'value32' | 'delta32',
): ProjectMidiEvent {
  const r = requestObject(requestName, request, 'request', true) as Record<string, unknown>;
  const ppq = num(r.ppq, 'ppq');
  const group = boundedInt(r.group, 'group', 15);
  const channel = boundedInt(r.channel, 'channel', 15);
  const bank = boundedInt(r.bank, 'bank', 127);
  const index = boundedInt(r.index, 'index', 127);
  const raw = r[valueName];
  const word = valueName === 'delta32' ? midi2I32(raw, valueName) : midi2U32(raw, valueName);
  return midi2Event(ppq, group, opcode, channel, bank, index, word);
}

/**
 * Pack a MIDI 2.0 registered controller (RPN) event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2RegisteredController(
  request: Midi2RegisteredControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RegisteredController', () => {
    return bankController('midi2RegisteredController', request, 0x2, 'value32');
  });
}

/**
 * Pack a MIDI 2.0 assignable controller (NRPN) event.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2AssignableController(
  request: Midi2AssignableControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2AssignableController', () => {
    return bankController('midi2AssignableController', request, 0x3, 'value32');
  });
}

/**
 * Pack a MIDI 2.0 relative registered controller event (`delta32` is a signed change).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2RelativeRegisteredController(
  request: Midi2RelativeRegisteredControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RelativeRegisteredController', () => {
    return bankController('midi2RelativeRegisteredController', request, 0x4, 'delta32');
  });
}

/**
 * Pack a MIDI 2.0 relative assignable controller event (`delta32` is a signed change).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2RelativeAssignableController(
  request: Midi2RelativeAssignableControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RelativeAssignableController', () => {
    return bankController('midi2RelativeAssignableController', request, 0x5, 'delta32');
  });
}

/** The shared reader of the two per-note controller messages. */
function perNoteController(
  requestName: 'midi2RegisteredPerNoteController' | 'midi2AssignablePerNoteController',
  request: unknown,
  opcode: number,
): ProjectMidiEvent {
  const r = requestObject(requestName, request, 'request', true) as Record<string, unknown>;
  const ppq = num(r.ppq, 'ppq');
  const group = boundedInt(r.group, 'group', 15);
  const channel = boundedInt(r.channel, 'channel', 15);
  const note = boundedInt(r.note, 'note', 127);
  const index = midi2Byte(r.index, 'index');
  const value = midi2U32(r.value32, 'value32');
  return midi2Event(ppq, group, opcode, channel, note, index, value);
}

/**
 * Pack a MIDI 2.0 registered per-note controller event (`index` is 0..255).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2RegisteredPerNoteController(
  request: Midi2RegisteredPerNoteControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RegisteredPerNoteController', () => {
    return perNoteController('midi2RegisteredPerNoteController', request, 0x0);
  });
}

/**
 * Pack a MIDI 2.0 assignable per-note controller event (`index` is 0..255).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2AssignablePerNoteController(
  request: Midi2AssignablePerNoteControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2AssignablePerNoteController', () => {
    return perNoteController('midi2AssignablePerNoteController', request, 0x1);
  });
}

/**
 * Pack a MIDI 2.0 per-note pitch-bend event (`bend32` is unsigned 32-bit, center = 0x80000000).
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2PerNotePitchBend(request: Midi2PerNotePitchBendRequest): ProjectMidiEvent {
  return withEntry('midi2PerNotePitchBend', () => {
    const fn = 'midi2PerNotePitchBend';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const note = boundedInt(r.note, 'note', 127);
    const bend = midi2U32(r.bend32, 'bend32');
    return midi2Event(ppq, group, 0x6, channel, note, 0, bend);
  });
}

/**
 * Pack a MIDI 2.0 per-note management event. `detach` sets the D flag (detach
 * per-note controllers from voices already sounding on `note`); `reset` sets
 * the S flag (reset the note's per-note controllers).
 * @throws {TypeError} A non-object request, an unknown key or a wrong-typed field.
 * @throws {RangeError} A field outside its range.
 */
export function midi2PerNoteManagement(request: Midi2PerNoteManagementRequest): ProjectMidiEvent {
  return withEntry('midi2PerNoteManagement', () => {
    const fn = 'midi2PerNoteManagement';
    const r = requestObject(fn, request, 'request', true);
    const ppq = num(r.ppq, 'ppq');
    const group = boundedInt(r.group, 'group', 15);
    const channel = boundedInt(r.channel, 'channel', 15);
    const note = boundedInt(r.note, 'note', 127);
    const detach = optionalBoolean(r.detach, 'detach', false);
    const reset = optionalBoolean(r.reset, 'reset', false);
    const flags = (detach ? 0x02 : 0) | (reset ? 0x01 : 0);
    return midi2Event(ppq, group, 0xf, channel, note, flags, 0);
  });
}

/** A string argument: anything else is a `TypeError`. */
function lookupName(value: unknown, argName: string): string {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
  return value;
}

/** The lookup result, or a `RangeError` when the native table has no entry. */
function found<T>(value: T | null, what: string): T {
  if (value === null) {
    throw refuse(`${what}`);
  }
  return value;
}

// MIDI 1.0 <-> MIDI 2.0 value scaling: the conversions the packers and Standard MIDI File import
// and export apply. Widening is min-center-max, so the center lands on exactly 0x8000 / 0x80000000
// and the maximum on all ones; narrowing keeps the top bits, so widening then narrowing returns the
// input exactly. Each calls the library's own conversion.

/**
 * Widens a MIDI 1.0 velocity to MIDI 2.0's 16 bits; `0` stays `0`.
 *
 * @param velocity - `0`..`127`
 * @throws TypeError when `velocity` is not a number
 * @throws RangeError when `velocity` is not an integer in its range
 */
export function midiVelocityToMidi2(velocity: number): number {
  return withEntry('midiVelocityToMidi2', () => projectModule().midiVelocityToMidi2(velocity));
}

/**
 * Widens a MIDI 1.0 note-on velocity to MIDI 2.0's 16 bits.
 *
 * @param velocity - `1`..`127`; `0` is refused, since a MIDI 1.0 note-on with velocity 0 is a note-off
 * @throws TypeError when `velocity` is not a number
 * @throws RangeError when `velocity` is not an integer in its range
 */
export function midiNoteOnVelocityToMidi2(velocity: number): number {
  return withEntry('midiNoteOnVelocityToMidi2', () =>
    projectModule().midiNoteOnVelocityToMidi2(velocity),
  );
}

/**
 * Narrows a MIDI 2.0 velocity to MIDI 1.0's 7 bits (its top 7 bits).
 *
 * @param velocity16 - `0`..`65535`
 * @throws TypeError when `velocity16` is not a number
 * @throws RangeError when `velocity16` is not an integer in its range
 */
export function midi2VelocityToMidi1(velocity16: number): number {
  return withEntry('midi2VelocityToMidi1', () => projectModule().midi2VelocityToMidi1(velocity16));
}

/**
 * Narrows a MIDI 2.0 note-on velocity to MIDI 1.0's 7 bits; a result of `0` reads `1`, because every MIDI 2.0 note-on sounds.
 *
 * @param velocity16 - `0`..`65535`
 * @throws TypeError when `velocity16` is not a number
 * @throws RangeError when `velocity16` is not an integer in its range
 */
export function midi2NoteOnVelocityToMidi1(velocity16: number): number {
  return withEntry('midi2NoteOnVelocityToMidi1', () =>
    projectModule().midi2NoteOnVelocityToMidi1(velocity16),
  );
}

/**
 * Widens a 7-bit controller value to MIDI 2.0's 32 bits; `64` lands on `0x80000000`.
 *
 * @param value - `0`..`127`
 * @throws TypeError when `value` is not a number
 * @throws RangeError when `value` is not an integer in its range
 */
export function midiControlToMidi2(value: number): number {
  return withEntry('midiControlToMidi2', () => projectModule().midiControlToMidi2(value));
}

/**
 * Widens a 14-bit controller value to MIDI 2.0's 32 bits; `8192` lands on `0x80000000`.
 *
 * @param value14 - `0`..`16383`
 * @throws TypeError when `value14` is not a number
 * @throws RangeError when `value14` is not an integer in its range
 */
export function midiControl14ToMidi2(value14: number): number {
  return withEntry('midiControl14ToMidi2', () => projectModule().midiControl14ToMidi2(value14));
}

/**
 * Narrows a MIDI 2.0 controller value to 7 bits (its top 7 bits).
 *
 * @param value32 - `0`..`0xFFFFFFFF`
 * @throws TypeError when `value32` is not a number
 * @throws RangeError when `value32` is not an integer in its range
 */
export function midi2ControlToMidi1(value32: number): number {
  return withEntry('midi2ControlToMidi1', () => projectModule().midi2ControlToMidi1(value32));
}

/**
 * Narrows a MIDI 2.0 controller value to a 14-bit controller value (its top 14 bits).
 *
 * @param value32 - `0`..`0xFFFFFFFF`
 * @throws TypeError when `value32` is not a number
 * @throws RangeError when `value32` is not an integer in its range
 */
export function midi2ControlToMidi1Control14(value32: number): number {
  return withEntry('midi2ControlToMidi1Control14', () =>
    projectModule().midi2ControlToMidi1Control14(value32),
  );
}

/**
 * Widens a 14-bit pitch bend to MIDI 2.0's 32 bits; the center `8192` lands on `0x80000000`.
 *
 * @param bend14 - `0`..`16383`
 * @throws TypeError when `bend14` is not a number
 * @throws RangeError when `bend14` is not an integer in its range
 */
export function midiPitchBendToMidi2(bend14: number): number {
  return withEntry('midiPitchBendToMidi2', () => projectModule().midiPitchBendToMidi2(bend14));
}

/**
 * Narrows a MIDI 2.0 pitch bend to 14 bits (its top 14 bits).
 *
 * @param bend32 - `0`..`0xFFFFFFFF`
 * @throws TypeError when `bend32` is not a number
 * @throws RangeError when `bend32` is not an integer in its range
 */
export function midi2PitchBendToMidi1(bend32: number): number {
  return withEntry('midi2PitchBendToMidi1', () => projectModule().midi2PitchBendToMidi1(bend32));
}

/**
 * Return the General MIDI instrument name for `program` (0..127).
 * @throws {TypeError} A non-number `program`.
 * @throws {RangeError} A `program` outside 0..127.
 */
export function gmInstrumentName(program: number): string {
  return withEntry('gmInstrumentName', () => {
    const n = boundedInt(program, 'program', 127);
    return found(projectModule().midiGmInstrumentName(n), 'program must be an integer in [0, 127]');
  });
}

/**
 * Return the General MIDI program number for a canonical instrument name.
 * @throws {TypeError} A non-string `name`.
 * @throws {RangeError} An unknown name.
 */
export function gmProgramForName(name: string): number {
  return withEntry('gmProgramForName', () => {
    const s = lookupName(name, 'name');
    const program = projectModule().midiGmProgramForName(s);
    if (program < 0) {
      throw refuse('name must be a GM instrument name');
    }
    return program;
  });
}

/**
 * Return the General MIDI family name for `family` (0..15).
 * @throws {TypeError} A non-number `family`.
 * @throws {RangeError} A `family` outside 0..15.
 */
export function gmFamilyName(family: number): string {
  return withEntry('gmFamilyName', () => {
    const n = boundedInt(family, 'family', 15);
    return found(projectModule().midiGmFamilyName(n), 'family must be an integer in [0, 15]');
  });
}

/**
 * Return the first General MIDI program number in `family` (0..15).
 * @throws {TypeError} A non-number `family`.
 * @throws {RangeError} A `family` outside 0..15.
 */
export function gmFamilyFirstProgram(family: number): number {
  return withEntry('gmFamilyFirstProgram', () => {
    const n = boundedInt(family, 'family', 15);
    const program = projectModule().midiGmFamilyFirstProgram(n);
    if (program < 0) {
      throw refuse('family must be an integer in [0, 15]');
    }
    return program;
  });
}

/**
 * Return the GM2 bank/program instrument variation name.
 * @returns The name, or `undefined` when the number names nothing in the table.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside 0..127.
 */
export function gm2InstrumentName(request: Gm2InstrumentNameRequest): string | undefined {
  return withEntry('gm2InstrumentName', () => {
    const fn = 'gm2InstrumentName';
    const r = requestObject(fn, request, 'request', true);
    const bankLsb = boundedInt(r.bankLsb, 'bankLsb', 127);
    const program = boundedInt(r.program, 'program', 127);
    return projectModule().midiGm2InstrumentName(bankLsb, program) ?? undefined;
  });
}

/**
 * Return the General MIDI drum name for `note` (0..127).
 * @returns The name, or `undefined` when the number names nothing in the table.
 * @throws {TypeError} A non-number `note`.
 * @throws {RangeError} A `note` outside 0..127.
 */
export function gmDrumName(note: number): string | undefined {
  return withEntry('gmDrumName', () => {
    const n = boundedInt(note, 'note', 127);
    return projectModule().midiGmDrumName(n) ?? undefined;
  });
}

/**
 * Return the General MIDI drum note for a canonical drum name.
 * @throws {TypeError} A non-string `name`.
 * @throws {RangeError} An unknown name.
 */
export function gmDrumNoteForName(name: string): number {
  return withEntry('gmDrumNoteForName', () => {
    const s = lookupName(name, 'name');
    const note = projectModule().midiGmDrumNoteForName(s);
    if (note < 0) {
      throw refuse('name must be a GM drum name');
    }
    return note;
  });
}

/**
 * Return the GM2 drum-set name for `bankLsb` (0..127).
 * @returns The name, or `undefined` when the number names nothing in the table.
 * @throws {TypeError} A non-number `bankLsb`.
 * @throws {RangeError} A `bankLsb` outside 0..127.
 */
export function gm2DrumSetName(bankLsb: number): string | undefined {
  return withEntry('gm2DrumSetName', () => {
    const n = boundedInt(bankLsb, 'bankLsb', 127);
    return projectModule().midiGm2DrumSetName(n) ?? undefined;
  });
}

/**
 * Return the GM2 drum name for `bankLsb`/`note`.
 * @returns The name, or `undefined` when the number names nothing in the table.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 * @throws {RangeError} A field outside 0..127.
 */
export function gm2DrumName(request: Gm2DrumNameRequest): string | undefined {
  return withEntry('gm2DrumName', () => {
    const fn = 'gm2DrumName';
    const r = requestObject(fn, request, 'request', true);
    const bankLsb = boundedInt(r.bankLsb, 'bankLsb', 127);
    const note = boundedInt(r.note, 'note', 127);
    return projectModule().midiGm2DrumName(bankLsb, note) ?? undefined;
  });
}

/**
 * Return the MIDI CC name for `controller` (0..127).
 * @returns The name, or `undefined` when the number names nothing in the table.
 * @throws {TypeError} A non-number `controller`.
 * @throws {RangeError} A `controller` outside 0..127.
 */
export function midiCcName(controller: number): string | undefined {
  return withEntry('midiCcName', () => {
    const n = boundedInt(controller, 'controller', 127);
    return projectModule().midiCcName(n) ?? undefined;
  });
}

/**
 * Return the MIDI CC number for a canonical controller name.
 * @throws {TypeError} A non-string `name`.
 * @throws {RangeError} An unknown name.
 */
export function midiCcIndexForName(name: string): number {
  return withEntry('midiCcIndexForName', () => {
    const s = lookupName(name, 'name');
    const index = projectModule().midiCcIndexForName(s);
    if (index < 0) {
      throw refuse('name must be a standard MIDI CC name');
    }
    return index;
  });
}

/**
 * Return the MIDI 2.0 per-note controller name for `index` (0..255).
 * @returns The name, or `undefined` when the number names nothing in the table.
 * @throws {TypeError} A non-number `index`.
 * @throws {RangeError} An `index` outside 0..255.
 */
export function perNoteControllerName(index: number): string | undefined {
  return withEntry('perNoteControllerName', () => {
    const n = boundedInt(index, 'index', 255);
    return projectModule().midiPerNoteControllerName(n) ?? undefined;
  });
}

/**
 * Expand bank-select + program-change into MIDI events accepted by `Project.setMidiEvents`.
 * @throws {TypeError} A non-object request, an unknown key or a wrong-typed field.
 * @throws {RangeError} A field outside its range.
 */
export function midiBankProgram(request: MidiBankProgramRequest): ProjectMidiEvent[] {
  return withEntry('midiBankProgram', () => {
    const fn = 'midiBankProgram';
    const r = requestObject(fn, request, 'request', true);
    const bank = bankPair(r.bank, num);
    return projectModule().midiBankProgram(
      num(r.ppq, 'ppq'),
      num(r.group, 'group'),
      num(r.channel, 'channel'),
      bank.msb,
      bank.lsb,
      num(r.program, 'program'),
    );
  });
}

/**
 * Route MIDI events through the native MidiRouter filter/remap/thru logic. A
 * `null` or omitted config field is absent.
 */
export function midiRouteEvents(request: MidiRouteEventsRequest): ProjectMidiRouteResult {
  return withEntry('midiRouteEvents', () => {
    const { events, ...config } = requestObject('midiRouteEvents', request, 'request', true);
    assertMidiCcEventPayloads(events);
    assertMidiNumberShapes([
      ['filterGroup', config.filterGroup, true],
      ['filterChannel', config.filterChannel, true],
      ['remapChannel', config.remapChannel, true],
    ]);
    assertMidiBooleanShape(config.thru, 'thru', true);
    const present: Record<string, unknown> = {};
    for (const [key, value] of Object.entries(config)) {
      if (!isAbsent(value)) {
        present[key] = value;
      }
    }
    return projectModule().midiRouteEvents(events, present as ProjectMidiRouteConfig);
  });
}

/** Run native MIDI learn over an event stream; returns `null` when nothing is learned. */
export function midiCcLearn(
  request: MidiCcLearnRequest,
): (ProjectMidiCcBinding & { consume: boolean }) | null {
  return withEntry('midiCcLearn', () => {
    const r = requestObject('midiCcLearn', request, 'request', true);
    assertMidiCcEventPayloads(r.events);
    assertMidiNumberShapes([
      ['paramId', r.paramId],
      ['minValue', r.minValue, true],
      ['maxValue', r.maxValue, true],
      ['minMovement', r.minMovement, true],
    ]);
    return projectModule().midiCcLearn(
      r.events,
      r.paramId,
      r.minValue ?? 0,
      r.maxValue ?? 1,
      r.minMovement ?? 0,
    );
  });
}

/** Convert a CC event to a breakpoint. Refuses valid SysEx with NotSupported. */
export function midiCcToBreakpoint(
  request: MidiCcToBreakpointRequest,
): ProjectAutomationPoint | null {
  return withEntry('midiCcToBreakpoint', () => {
    const { bindings, event } = requestObject('midiCcToBreakpoint', request, 'request', true);
    assertMidiCcBindingListShapes(bindings);
    assertMidiCcEventPayload(event, 'event');
    assertMidiCcBindingListBytes(bindings);
    const native = projectModule().midiCcToBreakpoint(bindings, event);
    return native === null ? null : projectAutomationPointFromNative(native);
  });
}

/**
 * Convert one automation value back to a CC UMP event using native CcMap.
 * Returns `null` when no binding carries `paramId`.
 * @throws {TypeError} A non-object request, an unknown key or a non-number field.
 */
export function midiParamToCc(request: MidiParamToCcRequest): ProjectMidiEvent | null {
  return withEntry('midiParamToCc', () => {
    const fn = 'midiParamToCc';
    const r = requestObject(fn, request, 'request', true);
    assertMidiCcBindingListShapes(r.bindings);
    assertMidiNumberShapes([
      ['paramId', r.paramId],
      ['unitValue', r.unitValue],
      ['group', r.group],
      ['ppq', r.ppq, true],
    ]);
    assertMidiCcBindingListBytes(r.bindings);
    return projectModule().midiParamToCc(
      r.bindings,
      num(r.paramId, 'paramId'),
      num(r.unitValue, 'unitValue'),
      num(r.group, 'group'),
      isAbsent(r.ppq) ? 0 : num(r.ppq, 'ppq'),
    );
  });
}
