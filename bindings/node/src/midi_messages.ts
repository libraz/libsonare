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
import { assertBoundedInteger, assertU7, midi1Event, requestObject } from './validation.js';
import { projectAutomationPointFromNative } from './value_coercion.js';

const isAbsent = (value: unknown): value is undefined | null =>
  value === undefined || value === null;

function requireNumber(fnName: string, value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a number`);
  }
  return value;
}

/** A lookup index: `TypeError` unless a number, `RangeError` unless an integer in `[0, max]`. */
function lookupIndex(fnName: string, value: unknown, argName: string, max: number): number {
  const n = requireNumber(fnName, value, argName);
  assertBoundedInteger(fnName, n, argName, 0, max);
  return n;
}

function requireString(fnName: string, value: unknown, argName: string): string {
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${argName} must be a string`);
  }
  return value;
}

function optionalFlag(fnName: string, value: unknown, argName: string): boolean {
  if (isAbsent(value)) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: ${argName} must be a boolean`);
  }
  return value;
}

/** The shared `{ ppq, group, channel }` prefix of a packer request. */
function readRequest<
  T extends { ppq: number; group: number; channel: number },
  N extends RequestFn,
>(
  fnName: N,
  request: T & Parameters<typeof requestObject<T, N>>[1],
): { record: T; ppq: number; group: number; channel: number } {
  const record = requestObject<T, N>(fnName, request, 'request', true);
  return {
    record,
    ppq: requireNumber(fnName, record.ppq, 'ppq'),
    group: requireNumber(fnName, record.group, 'group'),
    channel: requireNumber(fnName, record.channel, 'channel'),
  };
}

function readBank(fnName: string, bank: unknown): { msb: number; lsb: number } {
  if (typeof bank !== 'object' || bank === null) {
    throw new TypeError(`${fnName}: bank must be an object`);
  }
  const { msb, lsb } = bank as ProjectProgramBank;
  return {
    msb: requireNumber(fnName, msb, 'bank.msb'),
    lsb: requireNumber(fnName, lsb, 'bank.lsb'),
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
  const fn = 'midiNoteOn';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = requireNumber(fn, record.note, 'note');
  const velocity = requireNumber(fn, record.velocity, 'velocity');
  return midi1Event(fn, ppq, group, 0x9, channel, note, velocity);
}

/**
 * Packs a MIDI 1.0 note-off event.
 *
 * @param request - Position, group, channel, note and optional release velocity
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiNoteOff(request: MidiNoteOffRequest): ProjectMidiEvent {
  const fn = 'midiNoteOff';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = requireNumber(fn, record.note, 'note');
  const velocity = isAbsent(record.velocity) ? 0 : requireNumber(fn, record.velocity, 'velocity');
  return midi1Event(fn, ppq, group, 0x8, channel, note, velocity);
}

/**
 * Packs a MIDI 1.0 control-change event.
 *
 * @param request - Position, group, channel, controller and value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiCc(request: MidiCcRequest): ProjectMidiEvent {
  const fn = 'midiCc';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const controller = requireNumber(fn, record.controller, 'controller');
  const value = requireNumber(fn, record.value, 'value');
  return midi1Event(fn, ppq, group, 0xb, channel, controller, value);
}

/**
 * Packs a MIDI 1.0 poly-pressure event.
 *
 * @param request - Position, group, channel, note and pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiPolyPressure(request: MidiPolyPressureRequest): ProjectMidiEvent {
  const fn = 'midiPolyPressure';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = requireNumber(fn, record.note, 'note');
  const pressure = requireNumber(fn, record.pressure, 'pressure');
  return midi1Event(fn, ppq, group, 0xa, channel, note, pressure);
}

/**
 * Packs a MIDI 1.0 program-change event.
 *
 * @param request - Position, group, channel and program
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiProgram(request: MidiProgramRequest): ProjectMidiEvent {
  const fn = 'midiProgram';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const program = requireNumber(fn, record.program, 'program');
  return midi1Event(fn, ppq, group, 0xc, channel, program, 0);
}

/**
 * Packs a MIDI 1.0 channel-pressure event.
 *
 * @param request - Position, group, channel and pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiChannelPressure(request: MidiChannelPressureRequest): ProjectMidiEvent {
  const fn = 'midiChannelPressure';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const pressure = requireNumber(fn, record.pressure, 'pressure');
  return midi1Event(fn, ppq, group, 0xd, channel, pressure, 0);
}

/**
 * Packs a MIDI 1.0 pitch-bend event.
 *
 * @param request - Position, group, channel and the unsigned 14-bit bend (center 8192)
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiPitchBend(request: MidiPitchBendRequest): ProjectMidiEvent {
  const fn = 'midiPitchBend';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const bend = requireNumber(fn, record.bend, 'bend');
  assertBoundedInteger(fn, bend, 'bend', 0, 0x3fff);
  return midi1Event(fn, ppq, group, 0xe, channel, bend & 0x7f, bend >> 7);
}

/**
 * Packs a MIDI 2.0 note-on event.
 *
 * @param request - Position, group, channel, note, 16-bit velocity and optional attribute
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2NoteOn(request: Midi2NoteOnRequest): ProjectMidiEvent {
  const fn = 'midi2NoteOn';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = assertU7(fn, requireNumber(fn, record.note, 'note'), 'note');
  const vel = midi2U16(fn, requireNumber(fn, record.velocity16, 'velocity16'), 'velocity16');
  const type = isAbsent(record.attributeType)
    ? 0
    : midi2Byte(fn, requireNumber(fn, record.attributeType, 'attributeType'), 'attributeType');
  const data = isAbsent(record.attributeData)
    ? 0
    : midi2U16(fn, requireNumber(fn, record.attributeData, 'attributeData'), 'attributeData');
  return midi2Event(fn, ppq, group, 0x9, channel, note, type, ((vel << 16) | data) >>> 0);
}

/**
 * Packs a MIDI 2.0 note-off event.
 *
 * @param request - Position, group, channel, note and optional 16-bit release velocity
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2NoteOff(request: Midi2NoteOffRequest): ProjectMidiEvent {
  const fn = 'midi2NoteOff';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = assertU7(fn, requireNumber(fn, record.note, 'note'), 'note');
  const vel = isAbsent(record.velocity16)
    ? 0
    : midi2U16(fn, requireNumber(fn, record.velocity16, 'velocity16'), 'velocity16');
  return midi2Event(fn, ppq, group, 0x8, channel, note, 0, (vel << 16) >>> 0);
}

/**
 * Packs a MIDI 2.0 control-change event.
 *
 * @param request - Position, group, channel, controller and the full 32-bit value
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2Cc(request: Midi2CcRequest): ProjectMidiEvent {
  const fn = 'midi2Cc';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const controller = assertU7(fn, requireNumber(fn, record.controller, 'controller'), 'controller');
  const value = midi2U32(fn, requireNumber(fn, record.value32, 'value32'), 'value32');
  return midi2Event(fn, ppq, group, 0xb, channel, controller, 0, value);
}

/**
 * Packs a MIDI 2.0 poly-pressure event.
 *
 * @param request - Position, group, channel, note and the full 32-bit pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PolyPressure(request: Midi2PolyPressureRequest): ProjectMidiEvent {
  const fn = 'midi2PolyPressure';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = assertU7(fn, requireNumber(fn, record.note, 'note'), 'note');
  const pressure = midi2U32(fn, requireNumber(fn, record.pressure32, 'pressure32'), 'pressure32');
  return midi2Event(fn, ppq, group, 0xa, channel, note, 0, pressure);
}

/**
 * Packs a MIDI 2.0 channel-pressure event.
 *
 * @param request - Position, group, channel and the full 32-bit pressure
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2ChannelPressure(request: Midi2ChannelPressureRequest): ProjectMidiEvent {
  const fn = 'midi2ChannelPressure';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const pressure = midi2U32(fn, requireNumber(fn, record.pressure32, 'pressure32'), 'pressure32');
  return midi2Event(fn, ppq, group, 0xd, channel, 0, 0, pressure);
}

/**
 * Packs a MIDI 2.0 pitch-bend event.
 *
 * @param request - Position, group, channel and the unsigned 32-bit bend (center 0x80000000)
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PitchBend(request: Midi2PitchBendRequest): ProjectMidiEvent {
  const fn = 'midi2PitchBend';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const bend = midi2U32(fn, requireNumber(fn, record.bend32, 'bend32'), 'bend32');
  return midi2Event(fn, ppq, group, 0xe, channel, 0, 0, bend);
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
  const fn = 'midi2Program';
  const { record, ppq, group, channel } = readRequest(fn, request);
  programArg(fn, group, 'group', 15);
  programArg(fn, channel, 'channel', 15);
  const program = programArg(fn, requireNumber(fn, record.program, 'program'), 'program', 127);
  let msb = 0;
  let lsb = 0;
  if (!isAbsent(record.bank)) {
    const bank = readBank(fn, record.bank);
    msb = programArg(fn, bank.msb, 'bank.msb', 127);
    lsb = programArg(fn, bank.lsb, 'bank.lsb', 127);
  }
  const word1 = ((program << 24) | (msb << 8) | lsb) >>> 0;
  return midi2Event(fn, ppq, group, 0xc, channel, 0, isAbsent(record.bank) ? 0 : 1, word1);
}

function midi2Controller(
  fn: 'midi2RegisteredController' | 'midi2AssignableController',
  request: Midi2RegisteredControllerRequest,
  opcode: number,
): ProjectMidiEvent {
  const { record, ppq, group, channel } = readRequest(fn, request);
  const bank = assertU7(fn, requireNumber(fn, record.bank, 'bank'), 'bank');
  const index = assertU7(fn, requireNumber(fn, record.index, 'index'), 'index');
  const value = midi2U32(fn, requireNumber(fn, record.value32, 'value32'), 'value32');
  return midi2Event(fn, ppq, group, opcode, channel, bank, index, value);
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
  return midi2Controller('midi2RegisteredController', request, 0x2);
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
  return midi2Controller('midi2AssignableController', request, 0x3);
}

function midi2RelativeController(
  fn: 'midi2RelativeRegisteredController' | 'midi2RelativeAssignableController',
  request: Midi2RelativeRegisteredControllerRequest,
  opcode: number,
): ProjectMidiEvent {
  const { record, ppq, group, channel } = readRequest(fn, request);
  const bank = assertU7(fn, requireNumber(fn, record.bank, 'bank'), 'bank');
  const index = assertU7(fn, requireNumber(fn, record.index, 'index'), 'index');
  const delta = midi2I32(fn, requireNumber(fn, record.delta32, 'delta32'), 'delta32');
  return midi2Event(fn, ppq, group, opcode, channel, bank, index, delta);
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
  return midi2RelativeController('midi2RelativeRegisteredController', request, 0x4);
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
  return midi2RelativeController('midi2RelativeAssignableController', request, 0x5);
}

function midi2PerNoteController(
  fn: 'midi2RegisteredPerNoteController' | 'midi2AssignablePerNoteController',
  request: Midi2RegisteredPerNoteControllerRequest,
  opcode: number,
): ProjectMidiEvent {
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = assertU7(fn, requireNumber(fn, record.note, 'note'), 'note');
  const index = midi2Byte(fn, requireNumber(fn, record.index, 'index'), 'index');
  const value = midi2U32(fn, requireNumber(fn, record.value32, 'value32'), 'value32');
  return midi2Event(fn, ppq, group, opcode, channel, note, index, value);
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
  return midi2PerNoteController('midi2RegisteredPerNoteController', request, 0x0);
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
  return midi2PerNoteController('midi2AssignablePerNoteController', request, 0x1);
}

/**
 * Packs a MIDI 2.0 per-note pitch-bend event.
 *
 * @param request - Position, group, channel, note and the unsigned 32-bit bend (center 0x80000000)
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midi2PerNotePitchBend(request: Midi2PerNotePitchBendRequest): ProjectMidiEvent {
  const fn = 'midi2PerNotePitchBend';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = assertU7(fn, requireNumber(fn, record.note, 'note'), 'note');
  const bend = midi2U32(fn, requireNumber(fn, record.bend32, 'bend32'), 'bend32');
  return midi2Event(fn, ppq, group, 0x6, channel, note, 0, bend);
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
  const fn = 'midi2PerNoteManagement';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const note = assertU7(fn, requireNumber(fn, record.note, 'note'), 'note');
  const detach = optionalFlag(fn, record.detach, 'detach');
  const reset = optionalFlag(fn, record.reset, 'reset');
  const flags = (detach ? 0x02 : 0) | (reset ? 0x01 : 0);
  return midi2Event(fn, ppq, group, 0xf, channel, note, flags, 0);
}

/**
 * Expands bank-select plus program-change into MIDI events accepted by `Project.setMidiEvents`.
 *
 * @param request - Position, group, channel, program and bank
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError for an out-of-range value
 */
export function midiBankProgram(request: MidiBankProgramRequest): ProjectMidiEvent[] {
  const fn = 'midiBankProgram';
  const { record, ppq, group, channel } = readRequest(fn, request);
  const program = requireNumber(fn, record.program, 'program');
  const bank = readBank(fn, record.bank);
  return addon.midiBankProgram(ppq, group, channel, bank.msb, bank.lsb, program);
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
  const fn = 'midiParamToCc';
  const record = requestObject(fn, request, 'request', true);
  return addon.midiParamToCc(
    record.bindings,
    requireNumber(fn, record.paramId, 'paramId'),
    requireNumber(fn, record.unitValue, 'unitValue'),
    requireNumber(fn, record.group, 'group'),
    isAbsent(record.ppq) ? 0 : requireNumber(fn, record.ppq, 'ppq'),
  );
}

function found(fn: string, value: string | null, what: string): string {
  if (value === null) {
    throw new RangeError(`${fn}: ${what}`);
  }
  return value;
}

function foundIndex(fn: string, value: number, what: string): number {
  if (value < 0) {
    throw new RangeError(`${fn}: ${what}`);
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
  lookupIndex('gmInstrumentName', program, 'program', 127);
  return found(
    'gmInstrumentName',
    addon.midiGmInstrumentName(program),
    `program ${program} is out of range`,
  );
}

/**
 * Returns the General MIDI program number for a canonical instrument name.
 *
 * @param name - Canonical instrument name, e.g. `'Violin'`
 * @throws TypeError when `name` is not a string
 * @throws RangeError when the name is unknown
 */
export function gmProgramForName(name: string): number {
  requireString('gmProgramForName', name, 'name');
  return foundIndex(
    'gmProgramForName',
    addon.midiGmProgramForName(name),
    `unknown instrument name '${name}'`,
  );
}

/**
 * Returns the General MIDI family name for `family`.
 *
 * @param family - Family index, `0`..`15`
 * @throws TypeError when `family` is not a number
 * @throws RangeError when `family` is out of range
 */
export function gmFamilyName(family: number): string {
  lookupIndex('gmFamilyName', family, 'family', 15);
  return found('gmFamilyName', addon.midiGmFamilyName(family), `family ${family} is out of range`);
}

/**
 * Returns the first General MIDI program number in `family`.
 *
 * @param family - Family index, `0`..`15`
 * @throws TypeError when `family` is not a number
 * @throws RangeError when `family` is out of range
 */
export function gmFamilyFirstProgram(family: number): number {
  lookupIndex('gmFamilyFirstProgram', family, 'family', 15);
  return foundIndex(
    'gmFamilyFirstProgram',
    addon.midiGmFamilyFirstProgram(family),
    `family ${family} is out of range`,
  );
}

/**
 * Returns the GM2 instrument variation name for a bank LSB and program.
 *
 * @param request - Bank LSB and program number
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError when the bank/program pair names no instrument
 */
export function gm2InstrumentName(request: Gm2InstrumentNameRequest): string {
  const fn = 'gm2InstrumentName';
  const record = requestObject(fn, request, 'request', true);
  const bankLsb = lookupIndex(fn, record.bankLsb, 'bankLsb', 127);
  const program = lookupIndex(fn, record.program, 'program', 127);
  return found(
    fn,
    addon.midiGm2InstrumentName(bankLsb, program),
    `no instrument for bankLsb ${bankLsb}, program ${program}`,
  );
}

/**
 * Returns the General MIDI drum name for `note`.
 *
 * @param note - Drum note number
 * @throws TypeError when `note` is not a number
 * @throws RangeError when `note` names no drum
 */
export function gmDrumName(note: number): string {
  lookupIndex('gmDrumName', note, 'note', 127);
  return found('gmDrumName', addon.midiGmDrumName(note), `note ${note} is out of range`);
}

/**
 * Returns the General MIDI drum note for a canonical drum name.
 *
 * @param name - Canonical drum name, e.g. `'Acoustic Snare'`
 * @throws TypeError when `name` is not a string
 * @throws RangeError when the name is unknown
 */
export function gmDrumNoteForName(name: string): number {
  requireString('gmDrumNoteForName', name, 'name');
  return foundIndex(
    'gmDrumNoteForName',
    addon.midiGmDrumNoteForName(name),
    `unknown drum name '${name}'`,
  );
}

/**
 * Returns the GM2 drum-set name for a bank LSB.
 *
 * @param bankLsb - GM2 drum-set bank LSB
 * @throws TypeError when `bankLsb` is not a number
 * @throws RangeError when no drum set uses that bank
 */
export function gm2DrumSetName(bankLsb: number): string {
  lookupIndex('gm2DrumSetName', bankLsb, 'bankLsb', 127);
  return found(
    'gm2DrumSetName',
    addon.midiGm2DrumSetName(bankLsb),
    `bankLsb ${bankLsb} names no drum set`,
  );
}

/**
 * Returns the GM2 drum name for a bank LSB and drum note.
 *
 * @param request - Drum-set bank LSB and note number
 * @throws TypeError for a wrong-typed or unknown field
 * @throws RangeError when the bank/note pair names no drum
 */
export function gm2DrumName(request: Gm2DrumNameRequest): string {
  const fn = 'gm2DrumName';
  const record = requestObject(fn, request, 'request', true);
  const bankLsb = lookupIndex(fn, record.bankLsb, 'bankLsb', 127);
  const note = lookupIndex(fn, record.note, 'note', 127);
  return found(
    fn,
    addon.midiGm2DrumName(bankLsb, note),
    `no drum for bankLsb ${bankLsb}, note ${note}`,
  );
}

/**
 * Returns the MIDI CC name for `controller`.
 *
 * @param controller - Controller number, `0`..`127`
 * @throws TypeError when `controller` is not a number
 * @throws RangeError when `controller` is out of range
 */
export function midiCcName(controller: number): string {
  lookupIndex('midiCcName', controller, 'controller', 127);
  return found(
    'midiCcName',
    addon.midiCcName(controller),
    `controller ${controller} is out of range`,
  );
}

/**
 * Returns the MIDI CC number for a canonical controller name.
 *
 * @param name - Canonical controller name, e.g. `'Brightness'`
 * @throws TypeError when `name` is not a string
 * @throws RangeError when the name is unknown
 */
export function midiCcIndexForName(name: string): number {
  requireString('midiCcIndexForName', name, 'name');
  return foundIndex(
    'midiCcIndexForName',
    addon.midiCcIndexForName(name),
    `unknown controller name '${name}'`,
  );
}

/**
 * Returns the MIDI 2.0 per-note controller name for `index`.
 *
 * @param index - Per-note controller index
 * @throws TypeError when `index` is not a number
 * @throws RangeError when `index` names no per-note controller
 */
export function perNoteControllerName(index: number): string {
  lookupIndex('perNoteControllerName', index, 'index', 255);
  return found(
    'perNoteControllerName',
    addon.midiPerNoteControllerName(index),
    `index ${index} names no per-note controller`,
  );
}

/**
 * Routes MIDI events through the native MidiRouter filter/remap/thru logic.
 *
 * @param request - Events to route plus the group/channel filter, channel remap
 *   and pass-through; absent or `null` route fields mean any/none
 * @throws TypeError for a non-object request or an unknown field
 */
export function midiRouteEvents(request: MidiRouteEventsRequest): ProjectMidiRouteResult {
  const { events, ...config } = requestObject('midiRouteEvents', request, 'request', true);
  return addon.midiRouteEvents(events, config);
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
  const r = requestObject('midiCcLearn', request, 'request', true);
  return addon.midiCcLearn(
    r.events,
    r.paramId,
    r.minValue ?? 0,
    r.maxValue ?? 1,
    r.minMovement ?? 0,
  );
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
  const r = requestObject('midiCcToBreakpoint', request, 'request', true);
  const native = addon.midiCcToBreakpoint(r.bindings, r.event) as {
    ppq: number;
    value: number;
    curveToNext: number;
  } | null;
  return native === null ? null : projectAutomationPointFromNative(native);
}
