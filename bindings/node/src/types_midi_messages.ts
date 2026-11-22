import type {
  MidiBank,
  ProjectMidiCcBinding,
  ProjectMidiEvent,
  ProjectMidiRouteConfig,
} from './types_project.js';

/** Request of {@link midiNoteOn}. */
export interface MidiNoteOnRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Velocity, `0`..`127`. */
  velocity: number;
}

/** Request of {@link midiNoteOff}. */
export interface MidiNoteOffRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Release velocity, `0`..`127`. Default `0`. */
  velocity?: number;
}

/** Request of {@link midiCc}. */
export interface MidiCcRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Controller number, `0`..`127`. */
  controller: number;
  /** Controller value, `0`..`127`. */
  value: number;
}

/** Request of {@link midiPolyPressure}. */
export interface MidiPolyPressureRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Pressure, `0`..`127`. */
  pressure: number;
}

/** Request of {@link midiProgram}. */
export interface MidiProgramRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Program number, `0`..`127`. */
  program: number;
}

/** Request of {@link midiChannelPressure}. */
export interface MidiChannelPressureRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Pressure, `0`..`127`. */
  pressure: number;
}

/** Request of {@link midiPitchBend}. */
export interface MidiPitchBendRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Unsigned 14-bit bend, `0`..`16383`; center is `8192`. */
  bend: number;
}

/** Request of {@link midi2NoteOn}. */
export interface Midi2NoteOnRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Full 16-bit velocity, `0`..`65535`; `0` is a sounding note-on. */
  velocity16: number;
  /** Attribute type, `0`..`255`: `0` none, `3` pitch 7.9. Default `0`. */
  attributeType?: number;
  /** Attribute data, `0`..`65535`. Default `0`. */
  attributeData?: number;
}

/** Request of {@link midi2NoteOff}. */
export interface Midi2NoteOffRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Full 16-bit release velocity, `0`..`65535`. Default `0`. */
  velocity16?: number;
}

/** Request of {@link midi2Cc}. */
export interface Midi2CcRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Controller number, `0`..`127`. */
  controller: number;
  /** Full 32-bit value, `0`..`4294967295`. */
  value32: number;
}

/** Request of {@link midi2PolyPressure}. */
export interface Midi2PolyPressureRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Full 32-bit pressure, `0`..`4294967295`. */
  pressure32: number;
}

/** Request of {@link midi2ChannelPressure}. */
export interface Midi2ChannelPressureRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Full 32-bit pressure, `0`..`4294967295`. */
  pressure32: number;
}

/** Request of {@link midi2PitchBend}. */
export interface Midi2PitchBendRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Unsigned 32-bit bend, `0`..`4294967295`; center is `0x80000000`. */
  bend32: number;
}

/** Request of {@link midi2Program}. */
export interface Midi2ProgramRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Program number, `0`..`127`. */
  program: number;
  /** Bank Select applied with the program. Omitted, the message carries no bank. */
  bank?: MidiBank;
}

/** Request of {@link midi2RegisteredController}. */
export interface Midi2RegisteredControllerRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Controller bank (MSB), `0`..`127`. */
  bank: number;
  /** Controller index (LSB), `0`..`127`. */
  index: number;
  /** Full 32-bit value, `0`..`4294967295`. */
  value32: number;
}

/** Request of {@link midi2AssignableController}. */
export interface Midi2AssignableControllerRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Controller bank (MSB), `0`..`127`. */
  bank: number;
  /** Controller index (LSB), `0`..`127`. */
  index: number;
  /** Full 32-bit value, `0`..`4294967295`. */
  value32: number;
}

/** Request of {@link midi2RelativeRegisteredController}. */
export interface Midi2RelativeRegisteredControllerRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Controller bank (MSB), `0`..`127`. */
  bank: number;
  /** Controller index (LSB), `0`..`127`. */
  index: number;
  /** Signed 32-bit change. */
  delta32: number;
}

/** Request of {@link midi2RelativeAssignableController}. */
export interface Midi2RelativeAssignableControllerRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Controller bank (MSB), `0`..`127`. */
  bank: number;
  /** Controller index (LSB), `0`..`127`. */
  index: number;
  /** Signed 32-bit change. */
  delta32: number;
}

/** Request of {@link midi2RegisteredPerNoteController}. */
export interface Midi2RegisteredPerNoteControllerRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Controller index, `0`..`255`. */
  index: number;
  /** Full 32-bit value, `0`..`4294967295`. */
  value32: number;
}

/** Request of {@link midi2AssignablePerNoteController}. */
export interface Midi2AssignablePerNoteControllerRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Controller index, `0`..`255`. */
  index: number;
  /** Full 32-bit value, `0`..`4294967295`. */
  value32: number;
}

/** Request of {@link midi2PerNotePitchBend}. */
export interface Midi2PerNotePitchBendRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Unsigned 32-bit bend, `0`..`4294967295`; center is `0x80000000`. */
  bend32: number;
}

/** Request of {@link midi2PerNoteManagement}. */
export interface Midi2PerNoteManagementRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Note number, `0`..`127`. */
  note: number;
  /** Detach per-note controllers from voices already sounding on `note`. Default `false`. */
  detach?: boolean;
  /** Reset the note's per-note controllers. Default `false`. */
  reset?: boolean;
}

/** Request of {@link midiBankProgram}. */
export interface MidiBankProgramRequest {
  /** Position in quarter notes, in [0, 1e12]. */
  ppq: number;
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Program number, `0`..`127`. */
  program: number;
  /** Bank Select sent before the program. */
  bank: MidiBank;
}

/** Request of {@link midiParamToCc}. */
export interface MidiParamToCcRequest {
  /** Bindings to search for the one that maps `paramId`. */
  bindings: ReadonlyArray<ProjectMidiCcBinding>;
  /** Parameter id to convert. */
  paramId: number;
  /** Parameter value in the binding's range. */
  unitValue: number;
  /** UMP group of the produced event, `0`..`15`. */
  group: number;
  /** Position of the produced event in quarter notes, in [0, 1e12]. Default `0`. */
  ppq?: number;
}

/** Request of {@link midiRouteEvents}: the events plus the route configuration. */
export interface MidiRouteEventsRequest extends ProjectMidiRouteConfig {
  /** Events to route. */
  events: ReadonlyArray<ProjectMidiEvent>;
}

/** Request of {@link midiCcLearn}. */
export interface MidiCcLearnRequest {
  /** Events to learn from. */
  events: ReadonlyArray<ProjectMidiEvent>;
  /** Parameter id the learned controller binds to. */
  paramId: number;
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
  /** Activity threshold in 7-bit CC units (integer 0-127) a controller must move to be learned. Default `0`. */
  minMovement?: number;
}

/** Request of {@link midiCcToBreakpoint}. */
export interface MidiCcToBreakpointRequest {
  /** Bindings to search for the one that maps the event's controller. */
  bindings: ReadonlyArray<ProjectMidiCcBinding>;
  /** CC event to convert. */
  event: ProjectMidiEvent;
}

/** Request of {@link gm2InstrumentName}. */
export interface Gm2InstrumentNameRequest {
  /** GM2 bank LSB (variation bank). */
  bankLsb: number;
  /** Program number, `0`..`127`. */
  program: number;
}

/** Request of {@link gm2DrumName}. */
export interface Gm2DrumNameRequest {
  /** GM2 drum-set bank LSB. */
  bankLsb: number;
  /** Drum note number. */
  note: number;
}
