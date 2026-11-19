import { refuse, refuseType, withEntry } from './internal/entry.js';
import { assertMidiCcBindingListBytes } from './internal/midi_cc_binding.js';
import {
  midi2Byte,
  midi2Event,
  midi2I32,
  midi2U16,
  midi2U32,
  programArg,
} from './internal/ump_events.js';
import { addon } from './native.js';
import type { RequestFn } from './request_keys.js';
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
} from './types_midi_messages.js';
import type {
  ProjectAutomationPoint,
  ProjectMidiCcBinding,
  ProjectMidiEvent,
  ProjectMidiRouteResult,
  ProjectProgramBank,
} from './types_project.js';
import { assertBoundedInteger, midi1Event, requestObject } from './validation.js';
import { projectAutomationPointFromNative } from './value_coercion.js';

const isAbsent = (value: unknown): value is undefined | null =>
  value === undefined || value === null;

function requireNumber(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  return value;
}

/** An integer in `[0, max]`: a non-number is a `TypeError`, anything else outside a `RangeError`. */
function boundedInt(value: unknown, argName: string, max: number): number {
  assertBoundedInteger(value, argName, 0, max);
  return value as number;
}

function requireString(value: unknown, argName: string): string {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
  return value;
}

function optionalFlag(value: unknown, argName: string): boolean {
  if (isAbsent(value)) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw refuseType(`${argName} must be a boolean`);
  }
  return value;
}

/** The shared `{ ppq, group, channel }` prefix of a packer request. */
function readRequest<
  T extends { ppq: number; group: number; channel: number },
  N extends RequestFn,
>(
  requestName: N,
  request: T & Parameters<typeof requestObject<T, N>>[1],
): { record: T; ppq: number; group: number; channel: number } {
  const record = requestObject<T, N>(requestName, request, 'request', true);
  return {
    record,
    ppq: requireNumber(record.ppq, 'ppq'),
    group: boundedInt(record.group, 'group', 15),
    channel: boundedInt(record.channel, 'channel', 15),
  };
}

function readBank(bank: unknown): { msb: number; lsb: number } {
  if (typeof bank !== 'object' || bank === null) {
    throw refuseType(`bank must be an object`);
  }
  const { msb, lsb } = bank as ProjectProgramBank;
  return {
    msb: boundedInt(msb, 'bank.msb', 127),
    lsb: boundedInt(lsb, 'bank.lsb', 127),
  };
}

/**
 * Packs a MIDI 1.0 note-on event accepted by `Project.setMidiEvents`.
 *
 * @param request - Position, group, channel, note and velocity
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiNoteOn(request: MidiNoteOnRequest): ProjectMidiEvent {
  return withEntry('midiNoteOn', () => {
    const fn = 'midiNoteOn';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const velocity = boundedInt(record.velocity, 'velocity', 127);
    return midi1Event(ppq, group, 0x9, channel, note, velocity);
  });
}

/**
 * Packs a MIDI 1.0 note-off event.
 *
 * @param request - Position, group, channel, note and optional release velocity
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiNoteOff(request: MidiNoteOffRequest): ProjectMidiEvent {
  return withEntry('midiNoteOff', () => {
    const fn = 'midiNoteOff';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const velocity = isAbsent(record.velocity) ? 0 : boundedInt(record.velocity, 'velocity', 127);
    return midi1Event(ppq, group, 0x8, channel, note, velocity);
  });
}

/**
 * Packs a MIDI 1.0 control-change event.
 *
 * @param request - Position, group, channel, controller and value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiCc(request: MidiCcRequest): ProjectMidiEvent {
  return withEntry('midiCc', () => {
    const fn = 'midiCc';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const controller = boundedInt(record.controller, 'controller', 127);
    const value = boundedInt(record.value, 'value', 127);
    return midi1Event(ppq, group, 0xb, channel, controller, value);
  });
}

/**
 * Packs a MIDI 1.0 poly-pressure event.
 *
 * @param request - Position, group, channel, note and pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiPolyPressure(request: MidiPolyPressureRequest): ProjectMidiEvent {
  return withEntry('midiPolyPressure', () => {
    const fn = 'midiPolyPressure';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const pressure = boundedInt(record.pressure, 'pressure', 127);
    return midi1Event(ppq, group, 0xa, channel, note, pressure);
  });
}

/**
 * Packs a MIDI 1.0 program-change event.
 *
 * @param request - Position, group, channel and program
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiProgram(request: MidiProgramRequest): ProjectMidiEvent {
  return withEntry('midiProgram', () => {
    const fn = 'midiProgram';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const program = boundedInt(record.program, 'program', 127);
    return midi1Event(ppq, group, 0xc, channel, program, 0);
  });
}

/**
 * Packs a MIDI 1.0 channel-pressure event.
 *
 * @param request - Position, group, channel and pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiChannelPressure(request: MidiChannelPressureRequest): ProjectMidiEvent {
  return withEntry('midiChannelPressure', () => {
    const fn = 'midiChannelPressure';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const pressure = boundedInt(record.pressure, 'pressure', 127);
    return midi1Event(ppq, group, 0xd, channel, pressure, 0);
  });
}

/**
 * Packs a MIDI 1.0 pitch-bend event.
 *
 * @param request - Position, group, channel and the unsigned 14-bit bend (center 8192)
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiPitchBend(request: MidiPitchBendRequest): ProjectMidiEvent {
  return withEntry('midiPitchBend', () => {
    const fn = 'midiPitchBend';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const bend = boundedInt(record.bend, 'bend', 16383);
    assertBoundedInteger(bend, 'bend', 0, 0x3fff);
    return midi1Event(ppq, group, 0xe, channel, bend & 0x7f, bend >> 7);
  });
}

/**
 * Packs a MIDI 2.0 note-on event.
 *
 * @param request - Position, group, channel, note, 16-bit velocity and optional attribute
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2NoteOn(request: Midi2NoteOnRequest): ProjectMidiEvent {
  return withEntry('midi2NoteOn', () => {
    const fn = 'midi2NoteOn';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const vel = midi2U16(record.velocity16, 'velocity16');
    const type = isAbsent(record.attributeType)
      ? 0
      : midi2Byte(record.attributeType, 'attributeType');
    const data = isAbsent(record.attributeData)
      ? 0
      : midi2U16(record.attributeData, 'attributeData');
    return midi2Event(ppq, group, 0x9, channel, note, type, ((vel << 16) | data) >>> 0);
  });
}

/**
 * Packs a MIDI 2.0 note-off event.
 *
 * @param request - Position, group, channel, note and optional 16-bit release velocity
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2NoteOff(request: Midi2NoteOffRequest): ProjectMidiEvent {
  return withEntry('midi2NoteOff', () => {
    const fn = 'midi2NoteOff';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const vel = isAbsent(record.velocity16) ? 0 : midi2U16(record.velocity16, 'velocity16');
    return midi2Event(ppq, group, 0x8, channel, note, 0, (vel << 16) >>> 0);
  });
}

/**
 * Packs a MIDI 2.0 control-change event.
 *
 * @param request - Position, group, channel, controller and the full 32-bit value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2Cc(request: Midi2CcRequest): ProjectMidiEvent {
  return withEntry('midi2Cc', () => {
    const fn = 'midi2Cc';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const controller = boundedInt(record.controller, 'controller', 127);
    const value = midi2U32(record.value32, 'value32');
    return midi2Event(ppq, group, 0xb, channel, controller, 0, value);
  });
}

/**
 * Packs a MIDI 2.0 poly-pressure event.
 *
 * @param request - Position, group, channel, note and the full 32-bit pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PolyPressure(request: Midi2PolyPressureRequest): ProjectMidiEvent {
  return withEntry('midi2PolyPressure', () => {
    const fn = 'midi2PolyPressure';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const pressure = midi2U32(record.pressure32, 'pressure32');
    return midi2Event(ppq, group, 0xa, channel, note, 0, pressure);
  });
}

/**
 * Packs a MIDI 2.0 channel-pressure event.
 *
 * @param request - Position, group, channel and the full 32-bit pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2ChannelPressure(request: Midi2ChannelPressureRequest): ProjectMidiEvent {
  return withEntry('midi2ChannelPressure', () => {
    const fn = 'midi2ChannelPressure';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const pressure = midi2U32(record.pressure32, 'pressure32');
    return midi2Event(ppq, group, 0xd, channel, 0, 0, pressure);
  });
}

/**
 * Packs a MIDI 2.0 pitch-bend event.
 *
 * @param request - Position, group, channel and the unsigned 32-bit bend (center 0x80000000)
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PitchBend(request: Midi2PitchBendRequest): ProjectMidiEvent {
  return withEntry('midi2PitchBend', () => {
    const fn = 'midi2PitchBend';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const bend = midi2U32(record.bend32, 'bend32');
    return midi2Event(ppq, group, 0xe, channel, 0, 0, bend);
  });
}

/**
 * Packs a MIDI 2.0 program-change event. The bank travels in the same message
 * when `bank` is given.
 *
 * @param request - Position, group, channel, program and optional bank
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2Program(request: Midi2ProgramRequest): ProjectMidiEvent {
  return withEntry('midi2Program', () => {
    const fn = 'midi2Program';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const program = programArg(record.program, 'program', 127);
    const bank = isAbsent(record.bank) ? { msb: 0, lsb: 0 } : readBank(record.bank);
    const { msb, lsb } = bank;
    const word1 = ((program << 24) | (msb << 8) | lsb) >>> 0;
    return midi2Event(ppq, group, 0xc, channel, 0, isAbsent(record.bank) ? 0 : 1, word1);
  });
}

function midi2Controller(
  requestName: 'midi2RegisteredController' | 'midi2AssignableController',
  request: Midi2RegisteredControllerRequest,
  opcode: number,
): ProjectMidiEvent {
  const { record, ppq, group, channel } = readRequest(requestName, request);
  const bank = boundedInt(record.bank, 'bank', 127);
  const index = boundedInt(record.index, 'index', 127);
  const value = midi2U32(record.value32, 'value32');
  return midi2Event(ppq, group, opcode, channel, bank, index, value);
}

/**
 * Packs a MIDI 2.0 registered controller (RPN) event.
 *
 * @param request - Position, group, channel, bank, index and the full 32-bit value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2RegisteredController(
  request: Midi2RegisteredControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RegisteredController', () => {
    return midi2Controller('midi2RegisteredController', request, 0x2);
  });
}

/**
 * Packs a MIDI 2.0 assignable controller (NRPN) event.
 *
 * @param request - Position, group, channel, bank, index and the full 32-bit value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2AssignableController(
  request: Midi2AssignableControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2AssignableController', () => {
    return midi2Controller('midi2AssignableController', request, 0x3);
  });
}

function midi2RelativeController(
  requestName: 'midi2RelativeRegisteredController' | 'midi2RelativeAssignableController',
  request: Midi2RelativeRegisteredControllerRequest,
  opcode: number,
): ProjectMidiEvent {
  const { record, ppq, group, channel } = readRequest(requestName, request);
  const bank = boundedInt(record.bank, 'bank', 127);
  const index = boundedInt(record.index, 'index', 127);
  const delta = midi2I32(record.delta32, 'delta32');
  return midi2Event(ppq, group, opcode, channel, bank, index, delta);
}

/**
 * Packs a MIDI 2.0 relative registered controller event.
 *
 * @param request - Position, group, channel, bank, index and the signed 32-bit change
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2RelativeRegisteredController(
  request: Midi2RelativeRegisteredControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RelativeRegisteredController', () => {
    return midi2RelativeController('midi2RelativeRegisteredController', request, 0x4);
  });
}

/**
 * Packs a MIDI 2.0 relative assignable controller event.
 *
 * @param request - Position, group, channel, bank, index and the signed 32-bit change
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2RelativeAssignableController(
  request: Midi2RelativeAssignableControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RelativeAssignableController', () => {
    return midi2RelativeController('midi2RelativeAssignableController', request, 0x5);
  });
}

function midi2PerNoteController(
  requestName: 'midi2RegisteredPerNoteController' | 'midi2AssignablePerNoteController',
  request: Midi2RegisteredPerNoteControllerRequest,
  opcode: number,
): ProjectMidiEvent {
  const { record, ppq, group, channel } = readRequest(requestName, request);
  const note = boundedInt(record.note, 'note', 127);
  const index = midi2Byte(record.index, 'index');
  const value = midi2U32(record.value32, 'value32');
  return midi2Event(ppq, group, opcode, channel, note, index, value);
}

/**
 * Packs a MIDI 2.0 registered per-note controller event.
 *
 * @param request - Position, group, channel, note, index (`0`..`255`) and the full 32-bit value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2RegisteredPerNoteController(
  request: Midi2RegisteredPerNoteControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2RegisteredPerNoteController', () => {
    return midi2PerNoteController('midi2RegisteredPerNoteController', request, 0x0);
  });
}

/**
 * Packs a MIDI 2.0 assignable per-note controller event.
 *
 * @param request - Position, group, channel, note, index (`0`..`255`) and the full 32-bit value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2AssignablePerNoteController(
  request: Midi2AssignablePerNoteControllerRequest,
): ProjectMidiEvent {
  return withEntry('midi2AssignablePerNoteController', () => {
    return midi2PerNoteController('midi2AssignablePerNoteController', request, 0x1);
  });
}

/**
 * Packs a MIDI 2.0 per-note pitch-bend event.
 *
 * @param request - Position, group, channel, note and the unsigned 32-bit bend (center 0x80000000)
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PerNotePitchBend(request: Midi2PerNotePitchBendRequest): ProjectMidiEvent {
  return withEntry('midi2PerNotePitchBend', () => {
    const fn = 'midi2PerNotePitchBend';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const bend = midi2U32(record.bend32, 'bend32');
    return midi2Event(ppq, group, 0x6, channel, note, 0, bend);
  });
}

/**
 * Packs a MIDI 2.0 per-note management event. `detach` sets the D flag (detach
 * per-note controllers from voices already sounding on `note`); `reset` sets
 * the S flag (reset the note's per-note controllers).
 *
 * @param request - Position, group, channel, note and the optional flags
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PerNoteManagement(request: Midi2PerNoteManagementRequest): ProjectMidiEvent {
  return withEntry('midi2PerNoteManagement', () => {
    const fn = 'midi2PerNoteManagement';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const note = boundedInt(record.note, 'note', 127);
    const detach = optionalFlag(record.detach, 'detach');
    const reset = optionalFlag(record.reset, 'reset');
    const flags = (detach ? 0x02 : 0) | (reset ? 0x01 : 0);
    return midi2Event(ppq, group, 0xf, channel, note, flags, 0);
  });
}

/**
 * Expands bank-select plus program-change into MIDI events accepted by `Project.setMidiEvents`.
 *
 * @param request - Position, group, channel, program and bank
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiBankProgram(request: MidiBankProgramRequest): ProjectMidiEvent[] {
  return withEntry('midiBankProgram', () => {
    const fn = 'midiBankProgram';
    const { record, ppq, group, channel } = readRequest(fn, request);
    const program = boundedInt(record.program, 'program', 127);
    const bank = readBank(record.bank);
    return addon.midiBankProgram(ppq, group, channel, bank.msb, bank.lsb, program);
  });
}

/**
 * Converts one automation value back to a CC UMP event using the native CcMap.
 *
 * @param request - Bindings, parameter id, value, group and optional position
 * @returns The event, or `null` when no binding maps the parameter
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiParamToCc(request: MidiParamToCcRequest): ProjectMidiEvent | null {
  return withEntry('midiParamToCc', () => {
    const fn = 'midiParamToCc';
    const record = requestObject(fn, request, 'request', true);
    assertMidiCcBindingListBytes(record.bindings);
    return addon.midiParamToCc(
      record.bindings,
      boundedInt(record.paramId, 'paramId', 0xffffffff),
      requireNumber(record.unitValue, 'unitValue'),
      boundedInt(record.group, 'group', 15),
      isAbsent(record.ppq) ? 0 : requireNumber(record.ppq, 'ppq'),
    );
  });
}

function found(value: string | null, what: string): string {
  if (value === null) {
    throw refuse(`${what}`);
  }
  return value;
}

function foundIndex(value: number, what: string): number {
  if (value < 0) {
    throw refuse(`${what}`);
  }
  return value;
}

/**
 * Returns the General MIDI instrument name for `program`.
 *
 * @param program - Program number, `0`..`127`
 * @throws TypeError when `program` is not a number
 * @throws RangeError when `program` is out of range
 */
export function gmInstrumentName(program: number): string {
  return withEntry('gmInstrumentName', () => {
    boundedInt(program, 'program', 127);
    return found(addon.midiGmInstrumentName(program), 'program must be an integer in [0, 127]');
  });
}

/**
 * Returns the General MIDI program number for a canonical instrument name.
 *
 * @param name - Canonical instrument name, e.g. `'Violin'`
 * @throws TypeError when `name` is not a string
 * @throws RangeError when the name is unknown
 */
export function gmProgramForName(name: string): number {
  return withEntry('gmProgramForName', () => {
    requireString(name, 'name');
    return foundIndex(addon.midiGmProgramForName(name), 'name must be a GM instrument name');
  });
}

/**
 * Returns the General MIDI family name for `family`.
 *
 * @param family - Family index, `0`..`15`
 * @throws TypeError when `family` is not a number
 * @throws RangeError when `family` is out of range
 */
export function gmFamilyName(family: number): string {
  return withEntry('gmFamilyName', () => {
    boundedInt(family, 'family', 15);
    return found(addon.midiGmFamilyName(family), 'family must be an integer in [0, 15]');
  });
}

/**
 * Returns the first General MIDI program number in `family`.
 *
 * @param family - Family index, `0`..`15`
 * @throws TypeError when `family` is not a number
 * @throws RangeError when `family` is out of range
 */
export function gmFamilyFirstProgram(family: number): number {
  return withEntry('gmFamilyFirstProgram', () => {
    boundedInt(family, 'family', 15);
    return foundIndex(
      addon.midiGmFamilyFirstProgram(family),
      'family must be an integer in [0, 15]',
    );
  });
}

/**
 * Returns the GM2 instrument variation name for a bank LSB and program.
 *
 * @param request - Bank LSB and program number, each `0`..`127`
 * @returns The name, or `undefined` when the number names nothing in the table
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError when a field is outside `0`..`127`
 */
export function gm2InstrumentName(request: Gm2InstrumentNameRequest): string | undefined {
  return withEntry('gm2InstrumentName', () => {
    const fn = 'gm2InstrumentName';
    const record = requestObject(fn, request, 'request', true);
    const bankLsb = boundedInt(record.bankLsb, 'bankLsb', 127);
    const program = boundedInt(record.program, 'program', 127);
    return addon.midiGm2InstrumentName(bankLsb, program) ?? undefined;
  });
}

/**
 * Returns the General MIDI drum name for `note`.
 *
 * @param note - Drum note number, `0`..`127`
 * @returns The name, or `undefined` when the number names nothing in the table
 * @throws TypeError when `note` is not a number
 * @throws RangeError when `note` is outside `0`..`127`
 */
export function gmDrumName(note: number): string | undefined {
  return withEntry('gmDrumName', () => {
    boundedInt(note, 'note', 127);
    return addon.midiGmDrumName(note) ?? undefined;
  });
}

/**
 * Returns the General MIDI drum note for a canonical drum name.
 *
 * @param name - Canonical drum name, e.g. `'Acoustic Snare'`
 * @throws TypeError when `name` is not a string
 * @throws RangeError when the name is unknown
 */
export function gmDrumNoteForName(name: string): number {
  return withEntry('gmDrumNoteForName', () => {
    requireString(name, 'name');
    return foundIndex(addon.midiGmDrumNoteForName(name), 'name must be a GM drum name');
  });
}

/**
 * Returns the GM2 drum-set name for a bank LSB.
 *
 * @param bankLsb - GM2 drum-set bank LSB, `0`..`127`
 * @returns The name, or `undefined` when the number names nothing in the table
 * @throws TypeError when `bankLsb` is not a number
 * @throws RangeError when `bankLsb` is outside `0`..`127`
 */
export function gm2DrumSetName(bankLsb: number): string | undefined {
  return withEntry('gm2DrumSetName', () => {
    boundedInt(bankLsb, 'bankLsb', 127);
    return addon.midiGm2DrumSetName(bankLsb) ?? undefined;
  });
}

/**
 * Returns the GM2 drum name for a bank LSB and drum note.
 *
 * @param request - Drum-set bank LSB and note number, each `0`..`127`
 * @returns The name, or `undefined` when the number names nothing in the table
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError when a field is outside `0`..`127`
 */
export function gm2DrumName(request: Gm2DrumNameRequest): string | undefined {
  return withEntry('gm2DrumName', () => {
    const fn = 'gm2DrumName';
    const record = requestObject(fn, request, 'request', true);
    const bankLsb = boundedInt(record.bankLsb, 'bankLsb', 127);
    const note = boundedInt(record.note, 'note', 127);
    return addon.midiGm2DrumName(bankLsb, note) ?? undefined;
  });
}

/**
 * Returns the MIDI CC name for `controller`.
 *
 * @param controller - Controller number, `0`..`127`
 * @returns The name, or `undefined` when the number names nothing in the table
 * @throws TypeError when `controller` is not a number
 * @throws RangeError when `controller` is outside `0`..`127`
 */
export function midiCcName(controller: number): string | undefined {
  return withEntry('midiCcName', () => {
    boundedInt(controller, 'controller', 127);
    return addon.midiCcName(controller) ?? undefined;
  });
}

/**
 * Returns the MIDI CC number for a canonical controller name.
 *
 * @param name - Canonical controller name, e.g. `'Brightness'`
 * @throws TypeError when `name` is not a string
 * @throws RangeError when the name is unknown
 */
export function midiCcIndexForName(name: string): number {
  return withEntry('midiCcIndexForName', () => {
    requireString(name, 'name');
    return foundIndex(addon.midiCcIndexForName(name), 'name must be a standard MIDI CC name');
  });
}

/**
 * Returns the MIDI 2.0 per-note controller name for `index`.
 *
 * @param index - Per-note controller index, `0`..`255`
 * @returns The name, or `undefined` when the number names nothing in the table
 * @throws TypeError when `index` is not a number
 * @throws RangeError when `index` is outside `0`..`255`
 */
export function perNoteControllerName(index: number): string | undefined {
  return withEntry('perNoteControllerName', () => {
    boundedInt(index, 'index', 255);
    return addon.midiPerNoteControllerName(index) ?? undefined;
  });
}

/**
 * Routes MIDI events through the native MidiRouter filter/remap/thru logic.
 *
 * @param request - Events to route plus the group/channel filter, channel remap
 *   and pass-through; absent or `null` route fields mean any/none
 * @throws TypeError for a non-object request or an unknown field
 */
export function midiRouteEvents(request: MidiRouteEventsRequest): ProjectMidiRouteResult {
  return withEntry('midiRouteEvents', () => {
    const { events, ...config } = requestObject('midiRouteEvents', request, 'request', true);
    return addon.midiRouteEvents(events, config);
  });
}

/**
 * Runs native MIDI learn over an event stream.
 *
 * @param request - Events to learn from, the parameter id the learned controller
 *   binds to, the mapped range and the activity threshold
 * @returns The learned binding, or `null` when nothing is learned
 * @throws TypeError for a non-object request or an unknown field
 */
export function midiCcLearn(request: MidiCcLearnRequest): ProjectMidiCcBinding | null {
  return withEntry('midiCcLearn', () => {
    const r = requestObject('midiCcLearn', request, 'request', true);
    return addon.midiCcLearn(
      r.events,
      r.paramId,
      r.minValue ?? 0,
      r.maxValue ?? 1,
      r.minMovement ?? 0,
    );
  });
}

/**
 * Converts one CC event to an automation breakpoint using the native CcMap.
 *
 * @param request - Bindings to search and the CC event to convert
 * @returns The breakpoint, or `null` when no binding matches
 * @throws TypeError for a non-object request or an unknown field
 */
export function midiCcToBreakpoint(
  request: MidiCcToBreakpointRequest,
): ProjectAutomationPoint | null {
  return withEntry('midiCcToBreakpoint', () => {
    const r = requestObject('midiCcToBreakpoint', request, 'request', true);
    assertMidiCcBindingListBytes(r.bindings);
    const native = addon.midiCcToBreakpoint(r.bindings, r.event) as {
      ppq: number;
      value: number;
      curveToNext: number;
    } | null;
    return native === null ? null : projectAutomationPointFromNative(native);
  });
}
