import { ErrorCode } from './errors.js';
import { libraryToken, unknownOrdinal } from './internal/core_tokens.js';
import {
  assertDeclaredKeys,
  codedError,
  entryClass,
  invalidState,
  type Respelling,
  refusalOf,
  refuse,
  refuseType,
  refuseUnknownKey,
  withEntry,
} from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { requireListBudget } from './internal/list_budget.js';
import { midiFxConfigJson } from './internal/midi_fx_config.js';
import { sceneArgumentJson, sceneArgumentRefusal } from './internal/mix_scene.js';
import { optionalId, optionalPositiveInt } from './internal/optional_ints.js';
import { deinterleave, interleavePlanar } from './internal/planar.js';
import { setTimelineHandle } from './internal/project_handles.js';
import {
  CHORD_QUALITY,
  FADE_CURVE,
  KEY_MODE,
  LOOP_MODE,
  MARKER_KIND,
  OVERLAP_POLICY,
  SOURCE_KIND,
  STEM_LAYOUT,
  TRACK_KIND,
  tokenName,
  tokenOrdinal,
  WARP_MODE,
} from './internal/project_tokens.js';
import { respellRefusal, withRespelled } from './internal/respell.js';
import { refuseBuiltinSynthOrdinals, refuseSynthPatchOrdinals } from './internal/synth_tokens.js';
import { isAbsent, isNonArrayObject, isPlainRecord } from './internal/value_guards.js';
import { addon } from './native.js';
import { synthPatchParamInfo } from './synth_catalog.js';
import type {
  ExternalSeparatedStemImportRequest,
  ExternalSeparatedStemImportResult,
  MixSceneDocument,
  MixSceneDocumentInput,
  PartRigEntry,
  PartRigKey,
  PartRigMode,
  PartRigRequest,
  ProjectAnalyzeTempoRequest,
  ProjectArticulationBinding,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAudioSourceMetadataRequest,
  ProjectAutomationLane,
  ProjectAutomationLaneDesc,
  ProjectAutomationLaneEditDesc,
  ProjectAutomationTargetKind,
  ProjectAutoTempoOptions,
  ProjectAutoTempoRequest,
  ProjectAutoTempoResult,
  ProjectBounceOptions,
  ProjectBounceResult,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFadeRequest,
  ProjectClipLoopRequest,
  ProjectClipMoveOptions,
  ProjectClipTake,
  ProjectClipTakesOptions,
  ProjectCompileOptions,
  ProjectCompileResult,
  ProjectControllerProfileBinding,
  ProjectDestinationInstrumentBinding,
  ProjectDestinationInstrumentOptions,
  ProjectDiagnostic,
  ProjectInstrument,
  ProjectInstrumentBinding,
  ProjectKeySegment,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMarkerInput,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxBakeResult,
  ProjectMidiFxPreviewRequest,
  ProjectNotePairValidation,
  ProjectOverlapPolicy,
  ProjectPosition,
  ProjectProgramOptions,
  ProjectSection,
  ProjectSoundFontOptions,
  ProjectSource,
  ProjectSourceAudioRequest,
  ProjectTempoCandidate,
  ProjectTempoOptions,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrack,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectTranscribeRequest,
  ProjectWarpMode,
  Sf2ProgramStatus,
  SynthPatch,
} from './types.js';
import {
  ARTICULATIONS,
  BUILTIN_SYNTH_WAVEFORMS,
  PART_RIG_MODES,
  SYNTH_ENGINE_MODES,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
} from './types_instrument.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalRehydrateItem,
  ProjectVocalRehydrateOptions,
} from './types_vocal_project.js';
import {
  assertAudioInput,
  assertBoundedInteger,
  assertClosedToken,
  assertFiniteScalar,
  assertNonNegativeFloatValue,
  assertProjectMidiEvents,
  assertSafeInteger,
  assertSampleRate,
  assertSamples,
  assertU7,
  assertU32,
  C_INT_MAX,
  requestObject,
  requireBoolean,
  sampleOneShot,
} from './validation.js';
import {
  type NativeAutomationLaneDesc,
  normalizePartRig,
  normalizeSynthInstrument,
  projectAutomationLaneValue,
  projectAutomationPointFromNative,
} from './value_coercion.js';
import {
  projectApplyVocalEdit,
  projectGetVocalEditDependencies,
  projectRehydrateVocalEdits,
} from './vocal_project.js';

export * from './synth_catalog.js';
export * from './take_alignment.js';

/**
 * ABI version of the flat project layout in the loaded native binding. `undefined` when the
 * build has no project subsystem; check `capabilities().features` for presence.
 */
export function projectAbiVersion(): number | undefined {
  const version = (addon.projectAbiVersion as (() => number) | undefined)?.() ?? 0;
  return version > 0 ? version : undefined;
}

/** Names of the persistent automation targets accepted by project lanes. */
export function automationTargetKindNames(): ProjectAutomationTargetKind[] {
  return withEntry(
    'automationTargetKindNames',
    () => addon.automationTargetKindNames() as ProjectAutomationTargetKind[],
  );
}

type NativeProject = InstanceType<typeof addon.Project>;

/** Carries a native handle the module adopts instead of allocating a fresh project. */
class AdoptedProject {
  constructor(readonly native: NativeProject) {}
}

/** Proves a {@link ProjectTimeline} is built by this module. */
const TIMELINE_TOKEN = Symbol('ProjectTimeline');

function requireNumber(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  return value;
}

function requireInteger(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  return value;
}

/** Reads a string handed to a C UTF-8 field without replacement or truncation. */
function requireProjectText(value: unknown, argName: string): string {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
  if (value.includes('\0')) {
    throw refuse(`${argName} must not contain NUL`);
  }
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff) {
      const next = value.charCodeAt(i + 1);
      if (!(next >= 0xdc00 && next <= 0xdfff)) {
        throw refuse(`${argName} must contain well-formed Unicode`);
      }
      i++;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      throw refuse(`${argName} must contain well-formed Unicode`);
    }
  }
  return value;
}

function assertSectionPpq(value: unknown, argName: string): asserts value is number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isFinite(value) || value < 0 || value > 1e12) {
    throw refuse(`${argName} must be finite and in [0, 1e12]`);
  }
}

function bounceMetronomeSettings(value: unknown): Record<string, unknown> | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (!isNonArrayObject(value)) {
    throw refuseType('metronome must be an object');
  }
  assertDeclaredKeys(
    value,
    ['enabled', 'beatGain', 'accentGain', 'clickSamples', 'clickSec'],
    'metronome',
  );
  const enabled = requireBoolean(value.enabled, 'metronome.enabled');
  const native: Record<string, unknown> = { enabled };
  const beatGain = value.beatGain;
  if (!isAbsent(beatGain)) {
    assertNonNegativeFloatValue(beatGain, 'metronome.beatGain');
    native.beatGain = beatGain;
  }
  const accentGain = value.accentGain;
  if (!isAbsent(accentGain)) {
    assertNonNegativeFloatValue(accentGain, 'metronome.accentGain');
    native.accentGain = accentGain;
  }
  const clickSamples = value.clickSamples;
  const clickSec = value.clickSec;
  if (!isAbsent(clickSamples)) {
    assertBoundedInteger(clickSamples, 'metronome.clickSamples', 1, 384000);
    native.clickSamples = clickSamples;
  }
  if (!isAbsent(clickSec)) {
    if (typeof clickSec !== 'number') {
      throw refuseType('metronome.clickSec must be a number');
    }
    assertFiniteScalar(clickSec, 'metronome.clickSec');
    if (clickSec <= 0 || clickSec > 1) {
      throw refuse('metronome.clickSec must be in (0, 1]');
    }
    native.clickSec = clickSec;
  }
  if (!isAbsent(clickSamples) && !isAbsent(clickSec)) {
    throw refuse('give metronome.clickSamples or metronome.clickSec, not both');
  }
  return native;
}

function bounceControllerProfiles(value: unknown): ProjectControllerProfileBinding[] | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (!Array.isArray(value)) {
    throw refuseType('controllerProfiles must be an array');
  }
  requireListBudget(value, 'controllerProfiles');
  return value.map((item: unknown, index): ProjectControllerProfileBinding => {
    const at = `controllerProfiles[${index}]`;
    if (!isNonArrayObject(item)) {
      throw refuseType(`${at} must be an object`);
    }
    assertDeclaredKeys(item, ['destinationId', 'presetName'], at);
    const destinationId = requireInteger(item.destinationId, `${at}.destinationId`);
    assertU32(destinationId, `${at}.destinationId`);
    return {
      destinationId,
      presetName: requireProjectText(item.presetName, `${at}.presetName`),
    };
  });
}

function bounceArticulations(value: unknown): ProjectArticulationBinding[] | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (!Array.isArray(value)) {
    throw refuseType('articulations must be an array');
  }
  requireListBudget(value, 'articulations');
  return value.map((item: unknown, index): ProjectArticulationBinding => {
    const at = `articulations[${index}]`;
    if (!isNonArrayObject(item)) {
      throw refuseType(`${at} must be an object`);
    }
    assertDeclaredKeys(item, ['destinationId', 'channel', 'articulation'], at);
    const destinationId = requireInteger(item.destinationId, `${at}.destinationId`);
    assertU32(destinationId, `${at}.destinationId`);
    assertBoundedInteger(item.channel, `${at}.channel`, 0, 15);
    assertClosedToken(item.articulation, ARTICULATIONS, `${at}.articulation`);
    return {
      destinationId,
      channel: item.channel,
      articulation: item.articulation,
    };
  });
}

/** An optional string field whose empty form would read as "not given": absent stays absent, `""` is refused. */
function optionalName(argName: string, value: unknown, hint: string): string | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
  if (value === '') {
    throw refuse(`${argName} must not be empty; omit it ${hint}`);
  }
  return value;
}

/** The fields of a clip take and of a comp segment. */
const TAKE_KEYS = ['id', 'sourceId', 'sourceOffsetPpq', 'name'];
const COMP_SEGMENT_KEYS = ['startPpq', 'endPpq', 'takeId', 'crossfadePpq'];

/** The take list as the native layer reads it, each take checked by name; absent fields are missing keys. */
function takesToNative(takes: unknown): Array<Record<string, unknown>> {
  if (!Array.isArray(takes)) {
    throw refuseType(`takes must be an array`);
  }
  requireListBudget(takes, 'takes');
  return takes.map((entry: unknown, index) => {
    const where = `takes[${index}]`;
    if (!isNonArrayObject(entry)) {
      throw refuseType(`${where} must be an object`);
    }
    assertDeclaredKeys(entry, TAKE_KEYS, where);
    const { id, sourceId, name, ...rest } = entry as Record<string, unknown>;
    const takeId = requireInteger(id, `${where}.id`);
    assertU32(takeId, `${where}.id`);
    if (takeId === 0) {
      throw refuse(`${where}.id must not be 0`);
    }
    const source = optionalId(`${where}.sourceId`, sourceId, "for the clip's source");
    const label = optionalName(`${where}.name`, name, 'for an unnamed take');
    return {
      ...rest,
      id: takeId,
      ...(source === undefined ? {} : { sourceId: source }),
      ...(label === undefined ? {} : { name: label }),
    };
  });
}

/** The comp segments as the native layer reads them; an absent `takeId` is a missing key. */
function compSegmentsToNative(segments: unknown): Array<Record<string, unknown>> {
  if (!Array.isArray(segments)) {
    throw refuseType(`segments must be an array`);
  }
  requireListBudget(segments, 'segments');
  return segments.map((entry: unknown, index) => {
    if (!isNonArrayObject(entry)) {
      throw refuseType(`segments[${index}] must be an object`);
    }
    assertDeclaredKeys(entry, COMP_SEGMENT_KEYS, `segments[${index}]`);
    const { takeId, ...rest } = entry as Record<string, unknown>;
    const take = optionalId(`segments[${index}].takeId`, takeId, 'for the active take');
    return { ...rest, ...(take === undefined ? {} : { takeId: take }) };
  });
}

/** Render settings the native bounce reads. */
function bounceSettings(request: ProjectBounceOptions): Record<string, unknown> {
  const native: Record<string, unknown> = {};
  const totalFrames = optionalPositiveInt(
    'totalFrames',
    request.totalFrames,
    Number.MAX_SAFE_INTEGER,
  );
  const blockSize = optionalPositiveInt('blockSize', request.blockSize, C_INT_MAX);
  const numChannels = optionalPositiveInt('numChannels', request.numChannels, C_INT_MAX);
  if (!isAbsent(request.countInBars)) {
    native.countInBars = request.countInBars;
  }
  if (totalFrames !== undefined) {
    native.totalFrames = totalFrames;
  }
  if (blockSize !== undefined) {
    native.blockSize = blockSize;
  }
  if (numChannels !== undefined) {
    native.numChannels = numChannels;
  }
  if (!isAbsent(request.instrumentLatencySamples)) {
    native.instrumentLatencySamples = request.instrumentLatencySamples;
  }
  const metronome = bounceMetronomeSettings(request.metronome);
  if (metronome !== undefined) {
    native.metronome = metronome;
  }
  const controllerProfiles = bounceControllerProfiles(request.controllerProfiles);
  if (controllerProfiles !== undefined) {
    native.controllerProfiles = controllerProfiles;
  }
  const articulations = bounceArticulations(request.articulations);
  if (articulations !== undefined) {
    native.articulations = articulations;
  }
  return native;
}

/**
 * Resolve one instrument list of a bounce to the list the native
 * layer reads: each pair's `destinationId` is folded into a copy of its instrument.
 * Anything but an array of pairs is a `TypeError`.
 */
function bindInstruments<T extends object>(
  list: string,
  value: readonly ProjectInstrumentBinding<T>[],
  entry: (instrument: T, binding: Record<string, unknown>, label: string) => T,
  withDestination: (instrument: T, destinationId: number) => T,
  keys: readonly string[] = INSTRUMENT_BINDING_KEYS,
): T[] {
  if (!Array.isArray(value)) {
    throw refuseType(`${list} must be an array of { destinationId?, instrument } pairs`);
  }
  requireListBudget(value, list);
  return value.map((pair: unknown, index: number) => {
    const label = `${list}[${index}]`;
    const binding = instrumentBinding(pair, label, keys);
    const instrument = binding.instrument as T;
    const resolved = entry(instrument, binding, label);
    if (isAbsent(binding.destinationId)) {
      return resolved;
    }
    const where = `${label}.destinationId`;
    assertU32(requireInteger(binding.destinationId, where), where);
    return withDestination(resolved, binding.destinationId as number);
  });
}

/** The keys of a `{ destinationId?, instrument }` pair. */
const INSTRUMENT_BINDING_KEYS = ['destinationId', 'instrument'];
const SF2_BINDING_KEYS = [...INSTRUMENT_BINDING_KEYS, 'soundFontUri'];

/**
 * One `{ destinationId?, instrument }` pair, refused by its path: an undeclared key, a
 * missing or non-object instrument, and a `destinationId` given inside the instrument
 * instead of beside it.
 */
function instrumentBinding(
  pair: unknown,
  label: string,
  keys: readonly string[],
): Record<string, unknown> {
  if (!isNonArrayObject(pair)) {
    throw refuseType(`${label} must be a { destinationId?, instrument } pair`);
  }
  const record = pair as Record<string, unknown>;
  assertDeclaredKeys(record, keys, label);
  if (isAbsent(record.instrument)) {
    throw refuseType(`${label}.instrument must be given`);
  }
  const instrument = record.instrument;
  if (typeof instrument !== 'object' || Array.isArray(instrument)) {
    throw refuseType(`${label}.instrument must be an object`);
  }
  if ('destinationId' in instrument) {
    throw refuseUnknownKey(`${label}.instrument.destinationId`, [`${label}.destinationId`]);
  }
  return record;
}

const withDestinationCopy = <T extends object>(instrument: T, destinationId: number): T => ({
  ...instrument,
  destinationId,
});

/** Same as {@link withDestinationCopy} but keeps the instrument's prototype callbacks reachable. */
const withDestinationLayer = <T extends object>(instrument: T, destinationId: number): T =>
  Object.assign(Object.create(instrument) as T, { destinationId });

function soundFontUri(options: ProjectSoundFontOptions | undefined): string | undefined {
  if (isAbsent(options)) {
    return undefined;
  }
  if (!isPlainRecord(options)) {
    throw refuseType('options must be an object');
  }
  assertDeclaredKeys(options, ['uri'], '');
  return options.uri === undefined ? undefined : requireProjectText(options.uri, 'uri');
}

const PROJECT_INSTRUMENT_KINDS = ['synth', 'builtin', 'sf2'] as const;
const MAX_SYNTH_MOD_ROUTINGS = 8;
const NONPERSISTENT_INSTRUMENT_KINDS = ['sample', 'sampler', 'callback', 'external'] as const;
const BUILTIN_INSTRUMENT_KEYS = [
  'waveform',
  'gain',
  'attackMs',
  'decayMs',
  'sustain',
  'releaseMs',
  'polyphony',
];
const SF2_INSTRUMENT_KEYS = [
  'gain',
  'polyphony',
  'preferModelForModeledFamilies',
  'clearBankRig',
  'gsEfxRealization',
];
let synthPatchParamRows: ReturnType<typeof synthPatchParamInfo> | undefined;

function projectSynthPatchRows(): ReturnType<typeof synthPatchParamInfo> {
  synthPatchParamRows ??= synthPatchParamInfo();
  return synthPatchParamRows;
}

function respellDestinationInstrument(error: unknown): unknown {
  const refusal = refusalOf(error);
  if (refusal === undefined) {
    return error;
  }
  const subjects = refusal.subjects;
  const respelled: Record<string, Respelling> = {};
  for (const subject of subjects) {
    const name = subject.replace(/^instrument\.(synth|builtin|sf2)(?=\.|$)/, (_root, kind) =>
      kind === 'synth' ? 'instrument.patch' : 'instrument.config',
    );
    if (name !== subject) {
      respelled[subject] = { name, scale: 1 };
    }
  }
  return respellRefusal(error, respelled);
}

/**
 * Re-renders a bounce refusal the C entry names under its own binding member (`patch` /
 * `config`) at the `instrument` the caller wrote in that `{ destinationId?, instrument }` pair.
 */
function respellBounceInstrument(error: unknown): unknown {
  const refusal = refusalOf(error);
  if (refusal === undefined) {
    return error;
  }
  const respelled: Record<string, Respelling> = {};
  for (const subject of refusal.subjects) {
    const name = subject.replace(
      /^((?:builtin|synth|sf2)Instruments\[\d+\])\.(?:patch|config)(?=\.|$)/,
      '$1.instrument',
    );
    if (name !== subject) {
      respelled[subject] = { name, scale: 1 };
    }
  }
  return respellRefusal(error, respelled);
}

/** The keys a {@link SynthPatch} declares, in the order the patch catalog lists them. */
function synthPatchKeys(): string[] {
  return [
    'preset',
    'engineMode',
    'engineParams',
    'modRoutings',
    'useGmPrograms',
    'sampleBank',
    ...projectSynthPatchRows().map((r) => r.name),
  ];
}

/**
 * Refuses a synth patch the caller wrote at @p at by its path: an undeclared key, a field of the
 * wrong type, a token outside its set, an empty preset name and a malformed mod routing.
 */
function checkSynthPatch(patch: Record<string, unknown>, at: string): void {
  const rows = projectSynthPatchRows();
  assertDeclaredKeys(patch, synthPatchKeys(), at);
  refuseSynthPatchOrdinals(patch, at);
  if (!isAbsent(patch.preset)) {
    if (typeof patch.preset !== 'string') {
      throw refuseType(`${at}.preset must be a string`);
    }
    if (patch.preset.replace(/^va:/, '').length === 0) {
      throw refuse(`${at}.preset must not be empty; omit it for the init patch`);
    }
  }
  if (!isAbsent(patch.engineMode)) {
    assertClosedToken(patch.engineMode, SYNTH_ENGINE_MODES, `${at}.engineMode`);
  }
  for (const row of rows) {
    const value = patch[row.name];
    if (isAbsent(value)) {
      continue;
    }
    if (row.type === 'enum') {
      assertClosedToken(value, row.values, `${at}.${row.name}`);
    } else if (row.type === 'boolean' && typeof value !== 'boolean') {
      throw refuseType(`${at}.${row.name} must be a boolean`);
    } else if (row.type === 'number' && typeof value !== 'number') {
      throw refuseType(`${at}.${row.name} must be a number`);
    }
  }
  if (!isAbsent(patch.engineParams) && !isNonArrayObject(patch.engineParams)) {
    throw refuseType(`${at}.engineParams must be an object`);
  }
  if (!isAbsent(patch.modRoutings)) {
    if (!Array.isArray(patch.modRoutings)) {
      throw refuseType(`${at}.modRoutings must be an array`);
    }
    if (patch.modRoutings.length > MAX_SYNTH_MOD_ROUTINGS) {
      throw refuse(`${at}.modRoutings must contain at most ${MAX_SYNTH_MOD_ROUTINGS} entries`);
    }
    patch.modRoutings.forEach((routing, index) => {
      const path = `${at}.modRoutings[${index}]`;
      if (!isNonArrayObject(routing)) {
        throw refuseType(`${path} must be an object`);
      }
      assertDeclaredKeys(routing, ['source', 'destination', 'depth'], path);
      assertClosedToken(routing.source, SYNTH_MOD_SOURCES, `${path}.source`);
      assertClosedToken(routing.destination, SYNTH_MOD_DESTINATIONS, `${path}.destination`);
      for (const end of ['source', 'destination'] as const) {
        if (routing[end] === 'none') {
          throw refuse(`${path}.${end} must not be none`);
        }
      }
    });
  }
}

/** Refuses a builtin synth configuration the caller wrote at @p at by its path. */
function checkBuiltinConfig(config: Record<string, unknown>, at: string): void {
  assertDeclaredKeys(config, BUILTIN_INSTRUMENT_KEYS, at);
  refuseBuiltinSynthOrdinals(config);
  if (!isAbsent(config.waveform)) {
    assertClosedToken(config.waveform, BUILTIN_SYNTH_WAVEFORMS, `${at}.waveform`);
  }
}

/** Refuses a SoundFont instrument configuration the caller wrote at @p at by its path. */
function checkSf2Config(config: Record<string, unknown>, at: string): void {
  assertDeclaredKeys(config, SF2_INSTRUMENT_KEYS, at);
  if (!isAbsent(config.gsEfxRealization)) {
    assertClosedToken(config.gsEfxRealization, ['modern', 'classic'], `${at}.gsEfxRealization`);
  }
}

function normalizeDestinationInstrument(
  value: unknown,
  options: unknown,
): {
  instrument: Record<string, unknown> | null;
  options?: Record<string, unknown>;
} {
  let nativeOptions: Record<string, unknown> | undefined;
  if (!isAbsent(options)) {
    if (!isNonArrayObject(options)) {
      throw refuseType('options must be an object');
    }
    const settings = options as Record<string, unknown>;
    assertDeclaredKeys(settings, ['bank', 'program'], 'options');
    nativeOptions = {};
    if (!isAbsent(settings.bank)) {
      if (!isNonArrayObject(settings.bank)) {
        throw refuseType('options.bank must be an object');
      }
      const bank = settings.bank as Record<string, unknown>;
      assertDeclaredKeys(bank, ['msb', 'lsb'], 'options.bank');
      nativeOptions.bank = {
        msb: assertU7(requireInteger(bank.msb, 'options.bank.msb'), 'options.bank.msb'),
        lsb: assertU7(requireInteger(bank.lsb, 'options.bank.lsb'), 'options.bank.lsb'),
      };
    }
    if (!isAbsent(settings.program)) {
      nativeOptions.program = assertU7(
        requireInteger(settings.program, 'options.program'),
        'options.program',
      );
    }
  }

  if (isAbsent(value)) {
    if (nativeOptions !== undefined && Object.keys(nativeOptions).length > 0) {
      throw refuse('bank and program require an instrument');
    }
    return { instrument: null, options: nativeOptions };
  }
  if (!isNonArrayObject(value)) {
    throw refuseType('instrument must be a ProjectInstrument object');
  }
  const instrument = value as Record<string, unknown>;
  if (
    typeof instrument.kind === 'string' &&
    (NONPERSISTENT_INSTRUMENT_KINDS as readonly string[]).includes(instrument.kind)
  ) {
    throw codedError(ErrorCode.NotSupported, 'this instrument kind cannot be persisted', {
      reason: 'notPersistable',
    });
  }
  if (
    instrument.kind === undefined &&
    ['prepare', 'onEvent', 'render'].some((key) => typeof instrument[key] === 'function')
  ) {
    throw codedError(ErrorCode.NotSupported, 'callback instruments cannot be persisted', {
      reason: 'notPersistable',
    });
  }
  assertClosedToken(instrument.kind, PROJECT_INSTRUMENT_KINDS, 'instrument.kind');
  const normalized: Record<string, unknown> = { kind: instrument.kind };

  if (instrument.kind === 'synth') {
    assertDeclaredKeys(instrument, ['kind', 'patch', 'useGmPrograms'], 'instrument');
    if (!isNonArrayObject(instrument.patch)) {
      throw refuseType('instrument.patch must be an object');
    }
    const patch = instrument.patch as Record<string, unknown>;
    if (!isAbsent(patch.sampleBank)) {
      throw codedError(ErrorCode.NotSupported, 'sample banks cannot be persisted', {
        reason: 'notPersistable',
      });
    }
    checkSynthPatch(patch, 'instrument.patch');
    const nestedUseGmPrograms = isAbsent(patch.useGmPrograms)
      ? undefined
      : requireBoolean(patch.useGmPrograms, 'instrument.patch.useGmPrograms');
    const outerUseGmPrograms = isAbsent(instrument.useGmPrograms)
      ? undefined
      : requireBoolean(instrument.useGmPrograms, 'instrument.useGmPrograms');
    if (
      nestedUseGmPrograms !== undefined &&
      outerUseGmPrograms !== undefined &&
      nestedUseGmPrograms !== outerUseGmPrograms
    ) {
      throw refuse('instrument.useGmPrograms conflicts with instrument.patch.useGmPrograms');
    }
    const useGmPrograms = outerUseGmPrograms ?? nestedUseGmPrograms;
    if (useGmPrograms !== undefined) {
      normalized.useGmPrograms = useGmPrograms;
    }
    const patchFields = { ...patch };
    delete patchFields.useGmPrograms;
    delete patchFields.sampleBank;
    normalized.patch = patchFields;
  } else if (instrument.kind === 'builtin') {
    assertDeclaredKeys(instrument, ['kind', 'config'], 'instrument');
    if (!isNonArrayObject(instrument.config)) {
      throw refuseType('instrument.config must be an object');
    }
    const config = instrument.config as Record<string, unknown>;
    checkBuiltinConfig(config, 'instrument.config');
    normalized.config = config;
  } else {
    assertDeclaredKeys(instrument, ['kind', 'config', 'soundFontUri'], 'instrument');
    if (!isNonArrayObject(instrument.config)) {
      throw refuseType('instrument.config must be an object');
    }
    const config = instrument.config as Record<string, unknown>;
    checkSf2Config(config, 'instrument.config');
    normalized.config = config;
    if (!isAbsent(instrument.soundFontUri)) {
      const uri = requireProjectText(instrument.soundFontUri, 'instrument.soundFontUri');
      if (uri.length === 0) {
        throw refuse('instrument.soundFontUri must not be empty');
      }
      normalized.soundFontUri = uri;
    }
  }

  return { instrument: normalized, options: nativeOptions };
}

/** A native bounce: interleaved samples and the width the render was mixed at. */
interface NativeBounce {
  interleaved: Float32Array;
  numChannels: number;
}

function planarBounce(raw: NativeBounce, sampleRate: number): ProjectBounceResult {
  const { interleaved, numChannels } = raw;
  return { channels: deinterleave(interleaved, numChannels), sampleRate };
}

function diagnosticsFromNative(raw: unknown): ProjectDiagnostic[] {
  const list = raw as Array<{
    code: ProjectDiagnostic['code'];
    severity: ProjectDiagnostic['severity'];
    targetId: number;
    message: string;
  }>;
  return list.map((d) => ({
    code: d.code,
    severity: d.severity,
    ...(d.targetId === 0 ? {} : { targetId: d.targetId }),
    message: d.message,
  }));
}

/** Copy of an addon record under the key set the public type declares. */
function trackFromNative(raw: Record<string, unknown>): ProjectTrack {
  return { ...raw, kind: tokenName(TRACK_KIND, raw.kind as number, 'track kind') } as ProjectTrack;
}

/**
 * A clip loop length as the native layer reads it: an omitted length is its whole-clip `0`, and an
 * explicit `0` is refused so that omission is the one spelling of a whole-clip loop.
 */
function wholeClipLoopLength(value: unknown): number {
  if (isAbsent(value)) {
    return 0;
  }
  const length = requireNumber(value, 'loopLengthPpq');
  if (length === 0) {
    throw refuse('loopLengthPpq must not be 0; omit it to loop the whole clip');
  }
  return length;
}

/** A clip record; a whole-clip loop, or no loop, reads back without `loopLengthPpq`. */
function clipFromNative(raw: Record<string, unknown>): ProjectClip {
  const { loopLengthPpq, ...rest } = raw;
  return {
    ...rest,
    ...(loopLengthPpq === 0 ? {} : { loopLengthPpq }),
    sourceKind: tokenName(SOURCE_KIND, raw.sourceKind as number, 'source kind'),
    loopMode: tokenName(LOOP_MODE, raw.loopMode as number, 'loop mode'),
  } as ProjectClip;
}

/** An unset metadata string reads back as an absent key, so a source record never carries `""`. */
function sourceFromNative(raw: Record<string, unknown>): ProjectSource {
  const { contentHash, externalStemRole, ...rest } = raw;
  return {
    ...rest,
    ...(contentHash === '' ? {} : { contentHash }),
    ...(externalStemRole === '' ? {} : { externalStemRole }),
    kind: tokenName(SOURCE_KIND, raw.kind as number, 'source kind'),
  } as ProjectSource;
}

function markerFromNative(raw: Record<string, unknown>): ProjectMarker {
  return {
    id: raw.id,
    ppq: raw.ppq,
    ...(raw.name === '' ? {} : { name: raw.name }),
    kind: tokenName(MARKER_KIND, raw.kind as number, 'marker kind'),
    keyFifths: raw.keyFifths,
    keyMinor: raw.keyMinor,
  } as ProjectMarker;
}

function tempoOptionsToNative(request: ProjectTempoOptions): ProjectTempoOptions | undefined {
  const native: Record<string, unknown> = {};
  for (const key of [
    'adaptiveTempo',
    'tempoUpdateIntervalBeats',
    'rampThreshold',
    'includeOctaveCandidates',
  ] as const) {
    if (!isAbsent(request[key])) {
      native[key] = request[key];
    }
  }
  return Object.keys(native).length === 0 ? undefined : (native as ProjectTempoOptions);
}

function tempoCandidateFromNative(raw: Record<string, unknown>): ProjectTempoCandidate {
  const { timeSignature, ...rest } = raw;
  return { ...rest, firstTimeSignature: timeSignature } as unknown as ProjectTempoCandidate;
}

function respellMidiSysexBudget(error: unknown): unknown {
  const refusal = refusalOf(error);
  if (refusal === undefined) {
    return error;
  }
  const subjects = refusal.subjects;
  const respelled: Record<string, Respelling> = {};
  for (const subject of subjects) {
    const match = /^events\[(\d+)\]\.sysexSize$/.exec(subject);
    if (match !== null) {
      respelled[subject] = { name: `events[${match[1]}].sysex`, scale: 1 };
    }
  }
  return Object.keys(respelled).length === 0 ? error : respellRefusal(error, respelled);
}

const AUTOMATION_LANE_DESC_KEYS = [
  'targetKind',
  'targetParamId',
  'sendId',
  'insertIndex',
  'processor',
  'paramName',
  'points',
];

function automationLaneToNative(
  desc: ProjectAutomationLaneDesc | ProjectAutomationLaneEditDesc,
  editing: boolean,
): NativeAutomationLaneDesc {
  if (!isNonArrayObject(desc)) {
    throw refuseType('desc must be an object');
  }
  assertDeclaredKeys(desc, AUTOMATION_LANE_DESC_KEYS, '');
  const targetKind = isAbsent(desc.targetKind) ? undefined : desc.targetKind;
  if (targetKind !== undefined) {
    assertClosedToken(targetKind, automationTargetKindNames(), 'targetKind');
  }
  const hasTargetParamId = !isAbsent(desc.targetParamId);
  const identityKeys = ['sendId', 'insertIndex', 'processor', 'paramName'] as const;
  if (targetKind === undefined && editing) {
    if (hasTargetParamId) {
      throw refuse('targetParamId must be omitted when targetKind is omitted');
    }
    for (const key of identityKeys) {
      if (!isAbsent(desc[key])) {
        throw refuse(`${key} must be omitted when targetKind is omitted`);
      }
    }
  }
  const effectiveKind = targetKind ?? (editing ? undefined : 'opaque');
  if (effectiveKind === 'opaque' && !hasTargetParamId) {
    throw refuseType('targetParamId must be given for an opaque target');
  }
  if (effectiveKind !== undefined && effectiveKind !== 'opaque' && hasTargetParamId) {
    throw refuse('targetParamId must be omitted for a typed target');
  }
  const expectedIdentity =
    effectiveKind === 'trackSendDb'
      ? ['sendId']
      : effectiveKind === 'trackInsertParam'
        ? ['insertIndex', 'processor', 'paramName']
        : [];
  for (const key of identityKeys) {
    if (!isAbsent(desc[key]) && !expectedIdentity.includes(key)) {
      throw refuse(`${key} must be omitted for targetKind ${effectiveKind}`);
    }
  }
  if (expectedIdentity.some((key) => isAbsent((desc as Record<string, unknown>)[key]))) {
    throw refuseType(`targetKind ${effectiveKind} requires ${expectedIdentity.join(', ')}`);
  }
  const normalized = {
    ...desc,
    targetKind,
    ...(typeof desc.sendId === 'string'
      ? { sendId: requireProjectText(desc.sendId, 'sendId') }
      : {}),
    ...(typeof desc.processor === 'string'
      ? { processor: requireProjectText(desc.processor, 'processor') }
      : {}),
    ...(typeof desc.paramName === 'string'
      ? { paramName: requireProjectText(desc.paramName, 'paramName') }
      : {}),
  } as ProjectAutomationLaneDesc;
  return projectAutomationLaneValue(normalized);
}

function automationLaneFromNative(
  raw: Record<string, unknown>,
  targetKinds: readonly ProjectAutomationTargetKind[],
): ProjectAutomationLane {
  const points = raw.points as Array<{ ppq: number; value: number; curveToNext: number }>;
  const targetKind = libraryToken(targetKinds, raw.targetKind, 'automation target kind');
  return {
    ...raw,
    targetKind,
    points: points.map(projectAutomationPointFromNative),
  } as unknown as ProjectAutomationLane;
}

/**
 * Folds the request forms of `bakeMidiFx` into the native argument shape,
 * refusing an unknown key by name.
 */
function midiFxBakeRequest(request: ProjectMidiFxBakeRequest): {
  clipId: number;
  configJson: string;
  withSourceIndex: boolean;
} {
  const record = requestObject('bakeMidiFx', request, 'request', true);
  return {
    clipId: record.clipId,
    configJson: midiFxConfigJson(record.config),
    withSourceIndex: record.withSourceIndex ?? false,
  };
}

/** Interleave planar loop-recording audio; an interleaved array passes through. */
function interleaveLoopRecording(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingDesc {
  const { audio } = desc;
  if (!Array.isArray(audio)) {
    return desc;
  }
  if ((desc as { audioChannels?: unknown }).audioChannels !== undefined) {
    throw refuseType(
      'audioChannels must be omitted for planar audio; the channel count is the number of arrays',
    );
  }
  const { data, numChannels } = interleavePlanar(audio, 'audio');
  return { ...desc, audio: data, audioChannels: numChannels };
}

function respellSourceAudioRefusal(error: unknown, numChannels: number): unknown {
  const refusal = refusalOf(error);
  if (refusal === undefined || refusal.cEntry !== 'project_set_source_audio' || numChannels === 0) {
    return error;
  }
  const respelled: Record<string, Respelling> = {};
  for (const subject of refusal.subjects) {
    const match = /^channels\[(\d+)\]$/.exec(subject);
    if (match !== null) {
      const index = Number(match[1]);
      respelled[subject] = {
        name: `channels[${index % numChannels}][${Math.floor(index / numChannels)}]`,
        scale: 1,
      };
    }
  }
  return Object.keys(respelled).length === 0 ? error : respellRefusal(error, respelled);
}

let createTimeline: (native: InstanceType<typeof addon.ProjectTimeline>) => ProjectTimeline;

/**
 * Immutable compiled playback snapshot of a {@link Project}, produced by
 * {@link Project.compile} with `withTimeline: true` and installed with
 * {@link RealtimeEngine.applyProjectTimeline}. The native handle is not
 * garbage-collected, so release it with {@link destroy} (or `using`). It cannot
 * be constructed directly.
 */
@entryClass('ProjectTimeline')
export class ProjectTimeline {
  private disposed = false;

  private constructor(token: symbol, native: InstanceType<typeof addon.ProjectTimeline>) {
    if (token !== TIMELINE_TOKEN) {
      throw refuseType(
        'ProjectTimeline cannot be constructed; use Project.compile({ withTimeline: true })',
      );
    }
    this.nativeTimeline = native;
    setTimelineHandle(this, native);
  }

  private readonly nativeTimeline: InstanceType<typeof addon.ProjectTimeline>;

  /**
   * Release the native timeline. Idempotent. An engine the timeline was
   * applied to keeps its own reference, so this is safe right after
   * {@link RealtimeEngine.applyProjectTimeline}.
   */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.nativeTimeline.destroy();
  }

  /**
   * Releases the native timeline; lets `using` free it automatically (needs TypeScript 5.2+
   * or a runtime with native explicit resource management; Node 22 does not parse `using`).
   */
  [Symbol.dispose](): void {
    this.destroy();
  }

  static {
    createTimeline = (native) => new ProjectTimeline(TIMELINE_TOKEN, native);
  }
}

/**
 * Refuse a bar index past the safe-integer range in the core's own domain terms: no such bar has a
 * representable start, and a negative one is below the domain, so neither states this surface's
 * number width.
 */
function refuseUnsafeBarIndex(barIndex: unknown): void {
  if (
    typeof barIndex === 'number' &&
    !Number.isNaN(barIndex) &&
    (Number.isInteger(barIndex) || !Number.isFinite(barIndex)) &&
    !Number.isSafeInteger(barIndex)
  ) {
    throw refuse(
      barIndex < 0
        ? 'barIndex must be an integer of at least 0'
        : 'barIndex must identify a bar with a representable PPQ start in [0, 1000000000000]',
    );
  }
}

/**
 * Headless arrangement / DAW project (the curated `sonare_project_*` C ABI).
 *
 * Wraps an opaque native project handle over the arrangement control plane
 * (EditHistory + the offline compiler/bounce, serializer, and MIR tempo/grid
 * bridges). Every method is control-thread-only and performs no file or device
 * I/O: project JSON and SMF bytes are exchanged in memory. All mutation routes
 * through the native EditHistory, so {@link undo} / {@link redo} work, and
 * serialization is deterministic (`toJson` is byte-stable for a given project
 * state within one build). Musical positions are PPQ (quarter notes).
 *
 * @example
 * ```ts
 * const project = new Project();
 * const track = project.addTrack({ kind: 'audio', name: 'lead' });
 * const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
 * const json = project.toJson();
 * project.destroy();
 * ```
 */
@entryClass('Project', ['native', 'assertAlive'])
export class Project {
  private readonly handle: NativeProject;

  /** The native project; refused while an instrument bounce is calling back into JavaScript. */
  private get native(): NativeProject {
    if (this.instrumentBounceActive) {
      throw invalidState(
        'Project is busy: it cannot be used from inside a bounce instrument callback',
      );
    }
    return this.handle;
  }

  private instrumentBounceActive = false;

  /**
   * Applies a vocal edit to a clip's audio.
   *
   * @param request - The edit and the original audio it applies to
   * @returns The rendered result and the stored edit state
   */
  applyVocalEdit(request: ProjectVocalEditApplyRequest): ProjectVocalEditApplyResult {
    this.assertAlive();
    return projectApplyVocalEdit(this.native, request);
  }

  /** Vocal-edit dependencies of every clip that carries a stored edit. */
  vocalEditDependencies(): ProjectVocalEditDependency[] {
    this.assertAlive();
    return projectGetVocalEditDependencies(this.native);
  }

  /**
   * Re-renders the stored vocal edits against the original audio.
   *
   * @param originals - The original audio of each edited source
   * @param options - `cancel` is polled between items; return `true` to stop
   * @returns One entry per stored edit
   */
  rehydrateVocalEdits(
    originals: readonly ProjectVocalOriginalSource[],
    options?: ProjectVocalRehydrateOptions,
  ): ProjectVocalRehydrateItem[] {
    this.assertAlive();
    return projectRehydrateVocalEdits(this.native, originals, options);
  }

  private assertAlive(): void {
    assertHandleLive(this.disposed, 'Project');
  }

  private disposed = false;

  /** Create a new empty project (throws on a project ABI mismatch). */
  constructor();
  constructor(adopted?: unknown) {
    if (adopted !== undefined && !(adopted instanceof AdoptedProject)) {
      throw refuseType('Project takes no arguments');
    }
    this.handle = adopted === undefined ? new addon.Project() : adopted.native;
  }

  /**
   * Deserialize project JSON into a new project. Throws cleanly (with the joined
   * native diagnostic messages) on malformed input.
   */
  static fromJson(document: string): Project {
    return adoptProject(addon.Project.fromJson(document));
  }

  /**
   * Deserialize project JSON and return native warning diagnostics emitted on
   * successful loads, such as dangling source references preserved for repair.
   */
  static fromJsonWithDiagnostics(document: string): { project: Project; diagnostics: string } {
    const result = addon.Project.fromJson(document, true);
    return {
      project: adoptProject(result.project),
      diagnostics: result.diagnostics,
    };
  }

  // -- serialization --

  /** Serialize the project to deterministic JSON. */
  toJson(): string {
    return this.native.toJson();
  }

  /**
   * Set the project sample rate in Hz. Must be an integer in `[8000, 384000]`;
   * anything else throws. Applied through the edit history, so
   * it is undoable.
   */
  setSampleRate(sampleRate: number): void {
    this.native.setSampleRate(sampleRate);
  }

  /** The project sample rate in Hz. */
  get sampleRate(): number {
    return this.native.getSampleRate();
  }

  /** Set the project's clip-overlap policy. */
  setOverlapPolicy(policy: ProjectOverlapPolicy): void {
    this.native.setOverlapPolicy(tokenOrdinal(policy, OVERLAP_POLICY, 'policy'));
  }

  /** The project's clip-overlap policy. */
  get overlapPolicy(): ProjectOverlapPolicy {
    return tokenName(OVERLAP_POLICY, this.native.getOverlapPolicy(), 'overlap policy');
  }

  /**
   * Replace the project's mixer scene. Omit the scene to clear it.
   * @throws TypeError for an unknown key or wrong field type.
   * @throws RangeError for a value outside its domain.
   */
  setMixerScene(scene?: MixSceneDocumentInput): void {
    withEntry('Project.setMixerScene', () => {
      const document = scene === undefined ? undefined : sceneArgumentJson(scene);
      try {
        this.native.setMixerSceneJson(document);
      } catch (error) {
        throw sceneArgumentRefusal(error);
      }
    });
  }

  /** Read the project's mixer scene, or `undefined` when no scene is stored. */
  mixerScene(): MixSceneDocument | undefined {
    const document = this.native.mixerSceneJson();
    return document === null || document === undefined
      ? undefined
      : (JSON.parse(document) as MixSceneDocument);
  }

  /**
   * Add or replace a marker. An omitted `id` allocates a new one; an explicit
   * `0` is a `RangeError`.
   *
   * @returns The marker id
   */
  setMarker(input: ProjectMarkerInput): number {
    const fnName = 'setMarker';
    const request = requestObject(fnName, input, 'input', true);
    const marker: Record<string, unknown> = {
      ppq: requireNumber(request.ppq, 'ppq'),
    };
    const id = optionalId('id', request.id);
    if (id !== undefined) {
      marker.id = id;
    }
    if (!isAbsent(request.name)) {
      if (typeof request.name !== 'string') {
        throw refuseType(`name must be a string`);
      }
      if (request.name === '') {
        throw refuse('marker name must not be empty; omit it for an unnamed marker');
      }
      marker.name = request.name;
    }
    if (!isAbsent(request.kind)) {
      marker.kind = tokenOrdinal(request.kind, MARKER_KIND, 'kind');
    }
    if (!isAbsent(request.keyFifths)) {
      marker.keyFifths = request.keyFifths;
    }
    if (!isAbsent(request.keyMinor)) {
      marker.keyMinor = request.keyMinor;
    }
    return this.native.setMarker(marker);
  }

  /** Every marker, in stored order. */
  markers(): ProjectMarker[] {
    const native = this.native;
    return Array.from({ length: native.markerCount() }, (_, i) =>
      markerFromNative(native.markerByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Every track, in stored order. */
  tracks(): ProjectTrack[] {
    const native = this.native;
    return Array.from({ length: native.trackCount() }, (_, i) =>
      trackFromNative(native.trackByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Every clip, in stored order. */
  clips(): ProjectClip[] {
    const native = this.native;
    return Array.from({ length: native.clipCount() }, (_, i) =>
      clipFromNative(native.clipByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Every source, in stored order. */
  sources(): ProjectSource[] {
    const native = this.native;
    return Array.from({ length: native.sourceCount() }, (_, i) =>
      sourceFromNative(native.sourceByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Replace the project's tempo segment list. */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    if (Array.isArray(segments)) {
      requireListBudget(segments, 'segments');
    }
    this.native.setTempoSegments(segments);
  }

  /** Replace the project's time-signature segment list. */
  setTimeSignatures(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    if (Array.isArray(segments)) {
      requireListBudget(segments, 'segments');
    }
    this.native.setTimeSignatures(segments);
  }

  /** Every tempo segment, in stored order, in the shape {@link setTempoSegments} accepts. */
  tempoSegments(): ProjectTempoSegment[] {
    return this.native.tempoSegments();
  }

  /** Every time-signature segment, in stored order, in the shape {@link setTimeSignatures} accepts. */
  timeSignatures(): ProjectTimeSignatureSegment[] {
    return this.native.timeSignatures();
  }

  /** Count-in length at the project's initial tempo and meter. */
  countInFrames(countInBars: number): number {
    return this.native.countInFrames(countInBars);
  }

  /** Convert a project PPQ position to seconds from PPQ zero. */
  ppqToSec(ppq: number): number {
    assertFiniteScalar(ppq, 'ppq');
    return this.native.ppqToSec(ppq);
  }

  /** Convert seconds from PPQ zero to a project PPQ position. */
  secToPpq(sec: number): number {
    assertFiniteScalar(sec, 'sec');
    return this.native.secToPpq(sec);
  }

  /** Read the effective tempo, meter, bar, and beat at a project PPQ position. */
  positionAtPpq(ppq: number): ProjectPosition {
    assertFiniteScalar(ppq, 'ppq');
    return this.native.positionAtPpq(ppq);
  }

  /** Read the PPQ start of a zero-based bar. */
  barStartPpq(barIndex: number): number {
    refuseUnsafeBarIndex(barIndex);
    assertSafeInteger(barIndex, 'barIndex');
    return this.native.barStartPpq(barIndex);
  }

  /**
   * Read the end in PPQ of the last clip with a valid source and range on an audible track.
   * A track is audible when unmuted and, if any track is soloed, soloed itself.
   * Returns zero when no clip qualifies. Render tails are excluded.
   */
  endPpq(): number {
    return this.native.endPpq();
  }

  /** Audio source ids that need decoded PCM after deserialization. */
  unresolvedAudioSourceIds(): number[] {
    return this.native.unresolvedAudioSourceIds();
  }

  /** Unresolved audio source descriptors, in the order of {@link unresolvedAudioSourceIds}; `nameOrUri` is the full URI. */
  unresolvedAudioSources(): ProjectSource[] {
    return (this.native.unresolvedAudioSources() as unknown as Record<string, unknown>[]).map(
      sourceFromNative,
    );
  }

  /** SoundFont URIs used by destination instruments that have no loaded bytes. */
  unresolvedSoundFontUris(): string[] {
    return this.native.unresolvedSoundFontUris();
  }

  /** Stored destination instruments, ordered by destination id. */
  destinationInstruments(): ProjectDestinationInstrumentBinding<ProjectInstrument>[] {
    return this.native.destinationInstruments() as ProjectDestinationInstrumentBinding<ProjectInstrument>[];
  }

  /** Add an unresolved audio source and return its new id. */
  addAudioSource(uri: string): number {
    return this.native.addAudioSource(requireProjectText(uri, 'uri'));
  }

  /** Relink an audio source and return its unchanged id. */
  relinkSource(sourceId: number, uri: string): number {
    const id = requireInteger(sourceId, 'sourceId');
    assertU32(id, 'sourceId');
    this.native.relinkSource(id, requireProjectText(uri, 'uri'));
    return id;
  }

  /** Register decoded PCM for an existing audio source (undoable). */
  setSourceAudio(request: ProjectSourceAudioRequest): void {
    const fnName = 'setSourceAudio';
    const record = requestObject(fnName, request, 'request', true);
    assertSampleRate(record.sampleRate);
    const { data, numChannels } = interleavePlanar(record.channels);
    try {
      this.native.setSourceAudio(
        requireInteger(record.sourceId, 'sourceId'),
        data,
        numChannels,
        record.sampleRate,
      );
    } catch (error) {
      throw respellSourceAudioRefusal(error, numChannels);
    }
  }

  /** Replace an audio source's metadata strings as one undoable edit. */
  setAudioSourceMetadata(request: ProjectAudioSourceMetadataRequest): void {
    const fnName = 'setAudioSourceMetadata';
    const record = requestObject(fnName, request, 'request', true);
    const [contentHash, externalStemRole] = (['contentHash', 'externalStemRole'] as const).map(
      (field) => optionalName(field, record[field], 'to clear it'),
    );
    this.native.setAudioSourceMetadata(
      requireInteger(record.sourceId, 'sourceId'),
      contentHash ?? '',
      externalStemRole ?? '',
    );
  }

  // -- edit --

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    const { name, ...rest } = desc;
    const label = optionalName('name', name, 'for no name');
    return this.native.addTrack({
      ...rest,
      kind: tokenOrdinal(desc.kind ?? 'audio', TRACK_KIND, 'kind'),
      ...(label === undefined ? {} : { name: label }),
    });
  }

  /**
   * Add an audio or MIDI clip and return its allocated clip id. An audio clip
   * with `channels` is bound to a fresh renderable source; without it the source
   * is metadata-only.
   */
  addClip(desc: ProjectClipDesc): number {
    const fnName = 'addClip';
    const { channels, sampleRate, ...rest } = requestObject(fnName, desc, 'desc', true);
    if (requireInteger(rest.trackId, 'trackId') === 0) {
      throw refuse(`trackId must not be 0; a clip needs an existing track`);
    }
    if (channels === undefined) {
      return this.native.addClip(rest);
    }
    assertSampleRate(sampleRate);
    const { data, numChannels } = interleavePlanar(channels);
    return this.native.addClip({
      ...rest,
      audio: data,
      audioChannels: numChannels,
      audioSampleRate: sampleRate,
    });
  }

  /**
   * Import host-separated PCM as ordinary audio tracks and clips. Input PCM is
   * copied; the native layer never resamples, retimes, phase-aligns, or adds
   * hidden gain compensation.
   */
  importExternalStems(
    request: ExternalSeparatedStemImportRequest,
  ): ExternalSeparatedStemImportResult {
    const fnName = 'importExternalStems';
    const record = requestObject(fnName, request, 'request', true);
    if (!Array.isArray(record.stems)) {
      throw refuseType(`stems must be an array`);
    }
    requireListBudget(record.stems, 'stems');
    return this.native.importExternalStems({
      ...record,
      stems: record.stems.map((entry) => {
        const stem = requestObject('ExternalSeparatedStem', entry, 'stem', true);
        const { channels, layout, ...rest } = stem;
        return {
          ...rest,
          layout: tokenOrdinal(layout, STEM_LAYOUT, 'layout'),
          planarSamples: channels,
        };
      }),
    });
  }

  /**
   * Split captured loop-recording audio into takes and add one clip. `audio` may
   * be interleaved or planar (`Float32Array[]`). The last take is made active
   * unless it is partial and an earlier complete take exists; `partialTail:
   * 'drop'` does not create the partial take. A partial take is marked
   * `"partial": true` in its `takes[]` entry of `toJson()`.
   */
  addLoopRecordingTakes(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingResult {
    const framesName = Array.isArray(desc.audio)
      ? 'audio[0].length'
      : 'audio.length / audioChannels';
    return withRespelled(
      {
        audioFrames: { name: framesName, scale: 1 },
        flags: { name: 'partialTail', scale: 1 },
      },
      () => this.native.addLoopRecordingTakes(interleaveLoopRecording(desc)),
    );
  }

  /**
   * Create a MIDI track + clip; returns `{ trackId, clipId }`.
   *
   * @remarks
   * The clip window is half-open, `[0, lengthPpq)`: an event at exactly
   * `lengthPpq` is outside it. Export and validation close a note still
   * sounding at the clip end with a note-off there, so a note running the full
   * length of the clip is a matched pair.
   *
   * @param startPpq Clip start in quarter notes, in [0, 1e12].
   * @param lengthPpq Clip length in quarter notes, in (0, 1e12].
   */
  addMidiClip(startPpq: number, lengthPpq: number): ProjectMidiClipResult {
    return this.native.addMidiClip(startPpq, lengthPpq);
  }

  /** Split a clip at `splitPpq` (absolute PPQ, in [0, 1e12]); returns the new (right-hand) clip id. */
  splitClip(clipId: number, splitPpq: number): number {
    return this.native.splitClip(clipId, splitPpq);
  }

  /**
   * Trim a clip's start / length (PPQ).
   *
   * @param newStartPpq New clip start in quarter notes, in [0, 1e12].
   * @param newLengthPpq New clip length in quarter notes, in (0, 1e12].
   */
  trimClip(clipId: number, newStartPpq: number, newLengthPpq: number): void {
    this.native.trimClip(clipId, newStartPpq, newLengthPpq);
  }

  /** Move a clip to `newStartPpq` (in [0, 1e12]); with `options.trackId` it also moves to that track. */
  moveClip(clipId: number, newStartPpq: number, options?: ProjectClipMoveOptions): void {
    const fnName = 'moveClip';
    const record = options === undefined ? {} : requestObject(fnName, options, 'options', true);
    const trackId = optionalId('trackId', record.trackId, 'to keep the track');
    this.native.moveClip(clipId, newStartPpq, trackId ?? 0);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackKind): void {
    this.native.setTrackKind(trackId, tokenOrdinal(kind, TRACK_KIND, 'kind'));
  }

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Set a clip's warp playback mode. */
  setClipWarpMode(clipId: number, mode: ProjectWarpMode): void {
    this.native.setClipWarpMode(clipId, tokenOrdinal(mode, WARP_MODE, 'mode'));
  }

  /** Add or replace a first-class warp map referenced by clip warp ids. */
  setWarpMap(map: import('./types.js').ProjectWarpMapDesc): void {
    if (!isNonArrayObject(map)) {
      throw refuseType('map must be an object');
    }
    if (Array.isArray(map.anchors)) {
      requireListBudget(map.anchors, 'anchors');
    }
    const { name, ...rest } = map;
    const label = optionalName('name', name, 'for no name');
    this.native.setWarpMap(label === undefined ? rest : { ...rest, name: label });
  }

  /** Remove a first-class warp map by id. */
  removeWarpMap(warpRefId: number): void {
    this.native.removeWarpMap(warpRefId);
  }

  /**
   * Route a track's MIDI clips to a host/instrument destination id. Destination
   * `0`, which every track starts on, is a real id like any other.
   *
   * Builtin, NativeSynth, and SF2 instruments retain source-track provenance
   * inside a shared destination voice pool. With only zero-latency instruments
   * bound, live lanes and channel-strip bounces remain aligned. Configure a
   * live lane per source track that needs strip processing. A shared opaque or
   * latency-bearing instrument cannot be separated back into strip inputs; its
   * channel-strip bounce throws `NOT_SUPPORTED`.
   */
  setTrackMidiDestination(trackId: number, destinationId?: number | null): void {
    this.native.setTrackMidiDestination(trackId, destinationId ?? 0);
  }

  /** Store or clear a destination instrument. Omit `instrument` to remove the binding. */
  setDestinationInstrument(
    destinationId: number,
    instrument?: ProjectInstrument | null,
    options?: ProjectDestinationInstrumentOptions,
  ): void {
    const id = requireInteger(destinationId, 'destinationId');
    assertU32(id, 'destinationId');
    const normalized = normalizeDestinationInstrument(instrument, options);
    try {
      this.native.setDestinationInstrument(id, normalized.instrument, normalized.options);
    } catch (error) {
      throw respellDestinationInstrument(error);
    }
  }

  /**
   * Set a destination's part rig through an undoable edit. `part` is 0-15 or
   * {@link PART_RIG_ALL_PARTS} for the destination default. `mode` is `'bank'`
   * (the instrument's own rig), `'none'`, or `'chain'` with 1-8 `inserts`.
   * Entries are stored whether or not a track routes to the destination and take
   * effect when a bounce binds an instrument that carries part rigs.
   */
  setPartRig(request: PartRigRequest): void {
    const fnName = 'setPartRig';
    requestObject(fnName, request, 'request', true);
    const rig = normalizePartRig(request);
    assertClosedToken(rig.mode, PART_RIG_MODES, 'mode');
    this.native.setPartRig(rig.destinationId, rig.part, rig.mode as PartRigMode, rig.insertsJson);
  }

  /** Read one part rig entry, or `null` when none is set. */
  partRig(key: PartRigKey): PartRigEntry | null {
    const record = requestObject('partRig', key, 'key', true);
    const raw = this.native.getPartRig(
      requireInteger(record.destinationId, 'destinationId'),
      requireInteger(record.part, 'part'),
    );
    if (raw === null) {
      return null;
    }
    if (raw.inserts === undefined) {
      return { mode: raw.mode };
    }
    const inserts = JSON.parse(raw.inserts) as NonNullable<PartRigEntry['inserts']>;
    return { mode: raw.mode, inserts };
  }

  /** Remove one part rig entry through an undoable edit. */
  clearPartRig(key: PartRigKey): void {
    const record = requestObject('clearPartRig', key, 'key', true);
    this.native.clearPartRig(
      requireInteger(record.destinationId, 'destinationId'),
      requireInteger(record.part, 'part'),
    );
  }

  /**
   * Set a track's linear playback gain (1.0 = unity; >= 0) via an undoable edit.
   *
   * The value reaches the track's audio and MIDI alike, but the stage it lands
   * on follows the track's channel strip. A strip bound by this track alone
   * (including one synthesized for an unbound track) carries the controls on its
   * own fader and panner. A strip several tracks share processes their sum and
   * carries none of them; each track applies its controls upstream instead — on
   * its own clip schedules for audio, on its track lane for MIDI.
   *
   * A MIDI track's gain/pan on a shared strip ride the track lane, which is fed
   * per source track only by an instrument that preserves source-track identity
   * (see {@link setTrackMidiDestination}). An opaque host-callback instrument, or
   * one reporting non-zero latency, renders one buffer per destination and has no
   * per-track stage on a shared strip, so its gain/pan do not reach the bounce
   * there; bind such an instrument to a track with an exclusive strip. Mute and
   * solo are unaffected: a silenced MIDI track schedules no events at all.
   */
  setTrackGain(trackId: number, gain: number): void {
    this.native.setTrackGain(trackId, gain);
  }

  /** Set a track's mute flag via an undoable edit (a muted track is silent). */
  setTrackMute(trackId: number, mute: boolean): void {
    this.native.setTrackMute(trackId, mute);
  }

  /** Set a track's solo flag via an undoable edit (when any track is soloed, only soloed tracks sound). */
  setTrackSolo(trackId: number, solo: boolean): void {
    this.native.setTrackSolo(trackId, solo);
  }

  /**
   * Set a track's stereo balance in [-1, +1] (0 = center) via an undoable edit.
   *
   * See {@link setTrackGain} for which stage a track's controls land on. The pan
   * law that shapes the balance belongs to that stage: the strip's configured law
   * on a channel strip and on the clips of an audio track sharing a strip, and
   * the track lane's law for a MIDI track sharing a strip (the law of whatever
   * strip the host bound to that lane, or a linear balance when none is bound).
   * Every law is normalized so a centered track stays at unity and only the away
   * channel is attenuated, so the difference is a taper, not a level offset.
   *
   * @throws {RangeError} When `pan` is outside `[-1, 1]` or
   *   non-finite; the project and its undo history are left unchanged.
   */
  setTrackPan(trackId: number, pan: number): void {
    this.native.setTrackPan(trackId, pan);
  }

  /** Remove a clip via an undoable edit (undo restores it + its MIDI content). */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /** Set a clip's linear playback gain (>= 0; 0 = muted) via an undoable edit. */
  setClipGain(clipId: number, gain: number): void {
    this.native.setClipGain(clipId, gain);
  }

  /**
   * Set a clip's fade-in / fade-out regions via an undoable edit. An omitted side,
   * or an omitted length, is a zero-length linear fade.
   */
  setClipFade(clipId: number, request: ProjectClipFadeRequest): void {
    const fnName = 'setClipFade';
    const record = requestObject(fnName, request, 'request', true);
    const side = (ppq: unknown, curve: unknown, lengthName: string, curveName: string) => {
      if (isAbsent(ppq) && isAbsent(curve)) {
        return undefined;
      }
      return {
        lengthPpq: isAbsent(ppq) ? 0 : requireNumber(ppq, lengthName),
        curve: isAbsent(curve) ? FADE_CURVE.linear : tokenOrdinal(curve, FADE_CURVE, curveName),
      };
    };
    const fadeIn = side(record.fadeInPpq, record.fadeInCurve, 'fadeInPpq', 'fadeInCurve');
    const fadeOut = side(record.fadeOutPpq, record.fadeOutCurve, 'fadeOutPpq', 'fadeOutCurve');
    withRespelled(
      {
        'fadeIn.lengthPpq': { name: 'fadeInPpq', scale: 1 },
        'fadeOut.lengthPpq': { name: 'fadeOutPpq', scale: 1 },
      },
      () => this.native.setClipFade(clipId, fadeIn, fadeOut),
    );
  }

  /**
   * Replace a clip's take list and active take id via an undoable edit. An
   * omitted `options.activeTakeId` plays the clip's base source; an explicit `0`
   * is a `RangeError`.
   */
  setClipTakes(
    clipId: number,
    takes: ReadonlyArray<ProjectClipTake>,
    options?: ProjectClipTakesOptions,
  ): void {
    const fnName = 'setClipTakes';
    const record = options === undefined ? {} : requestObject(fnName, options, 'options', true);
    const nativeTakes = takesToNative(takes);
    const activeTakeId = optionalId('activeTakeId', record.activeTakeId);
    this.native.setClipTakes(clipId, nativeTakes as never, activeTakeId ?? 0);
  }

  /** Replace a clip's comp segments via an undoable edit. */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    this.native.setClipCompSegments(clipId, compSegmentsToNative(segments) as never);
  }

  /** Set a clip's loop mode and loop length via an undoable edit; see {@link ProjectClipLoopRequest}. */
  setClipLoop(clipId: number, request: ProjectClipLoopRequest): void {
    const fnName = 'setClipLoop';
    const record = requestObject(fnName, request, 'request', true);
    this.native.setClipLoop(
      clipId,
      tokenOrdinal(record.loopMode, LOOP_MODE, 'loopMode'),
      wholeClipLoopLength(record.loopLengthPpq),
      isAbsent(record.loopCrossfadePpq)
        ? 0
        : requireNumber(record.loopCrossfadePpq, 'loopCrossfadePpq'),
    );
  }

  /** Rebind a clip to a different already-registered source via an undoable edit. */
  setClipSource(clipId: number, sourceId: number): void {
    this.native.setClipSource(clipId, requireInteger(sourceId, 'sourceId'));
  }

  /**
   * Duplicate a clip at `newStartPpq` (in [0, 1e12]; same track), copying any MIDI content,
   * via an undoable edit; returns the new clip id.
   */
  duplicateClip(clipId: number, newStartPpq: number): number {
    return this.native.duplicateClip(clipId, newStartPpq);
  }

  /** Remove a track (and its clips) via an undoable edit. */
  removeTrack(trackId: number): void {
    this.native.removeTrack(trackId);
  }

  /** Rename a track via an undoable edit; an empty `name` clears it. */
  renameTrack(trackId: number, name: string): void {
    if (typeof name !== 'string') {
      throw refuseType('name must be a string');
    }
    this.native.renameTrack(trackId, name);
  }

  /**
   * Set a track's mixer-strip binding + output target via an undoable edit.
   * Omit either field to clear it. A supplied route must be nonempty.
   */
  setTrackRoute(trackId: number, channelStripRef?: string, outputTarget?: string): void {
    this.native.setTrackRoute(trackId, channelStripRef ?? null, outputTarget ?? null);
  }

  /**
   * Append an automation lane to a track via an undoable edit; returns the
   * lane's stable id. An omitted `desc.targetKind` adds an opaque lane.
   */
  addAutomationLane(trackId: number, desc: ProjectAutomationLaneDesc): number {
    return this.native.addAutomationLane(trackId, automationLaneToNative(desc, false));
  }

  /**
   * Replace the lane identified by its stable lane id. An omitted `desc.targetKind`
   * keeps the current target; in that form only `points` may be supplied.
   */
  editAutomationLane(trackId: number, laneId: number, desc: ProjectAutomationLaneEditDesc): void {
    this.native.editAutomationLane(trackId, laneId, automationLaneToNative(desc, true));
  }

  /** Every automation lane, flattened with its owning track and stable lane id. */
  automationLanes(): ProjectAutomationLane[] {
    const lanes = this.native.automationLanes() as Record<string, unknown>[];
    const targetKinds = automationTargetKindNames();
    return lanes.map((lane) => automationLaneFromNative(lane, targetKinds));
  }

  /** Remove the lane identified by its stable lane id. */
  removeAutomationLane(trackId: number, laneId: number): void {
    this.native.removeAutomationLane(trackId, laneId);
  }

  /** Undo the most recent edit (throws when the undo stack is empty). */
  undo(): void {
    this.native.undo();
  }

  /** Redo the most recently undone edit (throws when the redo stack is empty). */
  redo(): void {
    this.native.redo();
  }

  /** Clear the undo + redo stacks without changing the current project state. */
  clearHistory(): void {
    this.native.clearHistory();
  }

  /**
   * Cap the undo history to `depth` most-recent edits; older entries beyond the
   * cap are discarded immediately.
   *
   * @throws {RangeError} for a `depth` outside [1, 500000000] or not an integer
   */
  setMaxUndoDepth(depth: number): void {
    this.native.setMaxUndoDepth(depth);
  }

  /**
   * Set the combined retained-byte cap for undo and redo history. Zero is
   * valid and makes subsequent successful edits non-undoable.
   */
  setMaxHistoryBytes(bytes: number): void {
    this.native.setMaxHistoryBytes(bytes);
  }

  // -- MIDI --

  /** Read a MIDI clip's events in stable PPQ order, copying any SysEx payloads. */
  midiEvents(clipId: number): ProjectMidiEvent[] {
    assertU32(clipId, 'clipId');
    return this.native.midiEvents(clipId);
  }

  /**
   * Replace a MIDI clip's entire event list. Each event is
   * `{ ppq, word0, word1?, sysex? }`; pass an empty array to clear. `word0` and
   * `word1` carry the UMP message, and `sysex` carries the bytes of a Data64 event.
   *
   * Only events inside the clip's half-open window `[0, lengthPpq)` (after the
   * source offset) are kept for export; a note still sounding at the clip end,
   * including one whose note-off sits at or past `lengthPpq`, is closed with a
   * note-off at the clip end.
   */
  setMidiEvents(clipId: number, events: ReadonlyArray<ProjectMidiEvent>): void {
    assertProjectMidiEvents(events);
    try {
      this.native.setMidiEvents(clipId, events);
    } catch (error) {
      throw respellMidiSysexBudget(error);
    }
  }

  /**
   * Import an in-memory SMF buffer; returns the first added clip id.
   * Malformed or partially truncated tracks are rejected instead of installing
   * a silently shortened clip.
   */
  importSmf(data: Buffer | Uint8Array): number {
    return this.native.importSmf(data);
  }

  /** Export the project's tempo map + MIDI clips to SMF bytes. */
  exportSmf(): Uint8Array {
    return new Uint8Array(this.native.exportSmf());
  }

  /**
   * Import a MIDI 2.0 Clip File (`SMF2CLIP`); returns the first added clip id.
   * Unlike {@link importSmf}, MIDI 2.0 channel-voice messages (16-bit velocity,
   * 32-bit CC, per-note / registered controllers, bank-valid Program Change)
   * survive without loss.
   */
  importClipFile(data: Buffer | Uint8Array): number {
    return this.native.importClipFile(data);
  }

  /**
   * Export the project's tempo map + MIDI clips to MIDI 2.0 Clip File
   * (`SMF2CLIP`) bytes. MIDI 2.0-only events are written without loss —
   * prefer this over {@link exportSmf} when MIDI 2.0 fidelity matters.
   */
  exportClipFile(): Uint8Array {
    return new Uint8Array(this.native.exportClipFile());
  }

  /**
   * Set a MIDI clip's program at source PPQ 0. `options.bank` emits a Bank Select
   * before the program (omitted, none is emitted); `options.group` and
   * `options.channel` pick the UMP group and MIDI channel and default to 0.
   */
  setProgram(clipId: number, program: number, options?: ProjectProgramOptions): void {
    const fnName = 'setProgram';
    const record = options === undefined ? {} : requestObject(fnName, options, 'options', true);
    let bank = -1;
    if (!isAbsent(record.bank)) {
      const msb = assertU7(requireInteger(record.bank.msb, 'bank.msb'), 'bank.msb');
      const lsb = assertU7(requireInteger(record.bank.lsb, 'bank.lsb'), 'bank.lsb');
      bank = (msb << 7) | lsb;
    }
    this.native.setProgram(
      clipId,
      requireInteger(program, 'program'),
      bank,
      isAbsent(record.group) ? 0 : requireInteger(record.group, 'group'),
      isAbsent(record.channel) ? 0 : requireInteger(record.channel, 'channel'),
    );
  }

  /**
   * Destructively bake a MIDI-FX chain into all stored events. Large clips are
   * drained without truncation; failure leaves the original clip unchanged.
   * Setting `withSourceIndex` also returns per-event provenance, so a selection
   * or an editorial annotation can be carried across the bake.
   */
  bakeMidiFx(request: ProjectMidiFxBakeRequest): ProjectMidiFxBakeResult {
    const { clipId, configJson, withSourceIndex } = midiFxBakeRequest(request);
    if (!withSourceIndex) {
      this.native.bakeMidiFx(clipId, configJson);
      return {};
    }
    return { sourceIndex: this.native.bakeMidiFx(clipId, configJson, true) };
  }

  /**
   * Count the events {@link bakeMidiFx} would produce for this clip and
   * configuration, without mutating the project. The transform is
   * deterministic, so the count matches what the bake goes on to produce.
   */
  previewMidiFxCount(request: ProjectMidiFxPreviewRequest): number {
    const record = requestObject('previewMidiFx', request, 'request', true);
    return this.native.previewMidiFxCount(record.clipId, midiFxConfigJson(record.config));
  }

  /**
   * Validate that every note-on in the clip's exported half-open playback
   * window `[0, lengthPpq)` has a matching note-off. A note still sounding at
   * the clip end (its note-off at or past `lengthPpq`) is closed there by
   * export, so it validates as paired.
   *
   * @param clipId Target MIDI clip id.
   * @returns `ok` is `true` when fully paired; `unmatchedNoteOns` /
   *          `unmatchedNoteOffs` count the dangling events of each kind.
   * @throws If `clipId` is not a MIDI clip.
   */
  validateMidiNotes(clipId: number): ProjectNotePairValidation {
    return this.native.validateMidiNotes(clipId);
  }

  /**
   * Transcribe mono audio straight into a MIDI clip's event list, returning the
   * number of notes written.
   *
   * The PPQ grid is this project's own tempo map, which is why the request
   * carries no tempo: a project whose tempo was installed by
   * {@link autoTempo} transcribes onto that map rather than onto a second,
   * separately detected one. The clip's entire event list is REPLACED, exactly
   * as {@link setMidiEvents} replaces it.
   *
   * @throws If `clipId` is not a MIDI clip, if `samples` is empty, or if
   *         `sampleRate` is out of range.
   */
  transcribeToClip(request: ProjectTranscribeRequest): number {
    assertSamples(request.samples, true);
    assertSampleRate(request.sampleRate);
    return this.native.transcribeToClip(request);
  }

  // -- MIR --

  /**
   * Return ranked tempo-octave and detected-meter candidates without editing the
   * project. Takes `(samples, sampleRate, options?)` or one
   * {@link ProjectAnalyzeTempoRequest}.
   */
  analyzeTempo(request: ProjectAnalyzeTempoRequest): ProjectTempoCandidate[];
  analyzeTempo(
    samples: Float32Array,
    sampleRate: number,
    options?: ProjectTempoOptions,
  ): ProjectTempoCandidate[];
  analyzeTempo(...args: unknown[]): ProjectTempoCandidate[] {
    const fnName = 'analyzeTempo';
    const request = sampleOneShot<ProjectAnalyzeTempoRequest>(fnName, args);
    assertAudioInput(request.samples, request.sampleRate);
    return (
      this.native.analyzeTempo(
        request.samples,
        request.sampleRate,
        tempoOptionsToNative(request),
      ) as unknown as Record<string, unknown>[]
    ).map(tempoCandidateFromNative);
  }

  /**
   * Detect and install a ranked tempo candidate; optionally apply its detected meter.
   * Takes `(samples, sampleRate, options?)` or one {@link ProjectAutoTempoRequest}.
   *
   * @remarks
   * `candidateIndex` indexes the ranking {@link analyzeTempo} produced, so pair
   * the two on the same options. Read the installed map back with
   * {@link tempoSegments}.
   */
  autoTempo(request: ProjectAutoTempoRequest): ProjectAutoTempoResult;
  autoTempo(
    samples: Float32Array,
    sampleRate: number,
    options?: ProjectAutoTempoOptions,
  ): ProjectAutoTempoResult;
  autoTempo(...args: unknown[]): ProjectAutoTempoResult {
    const fnName = 'autoTempo';
    const request = sampleOneShot<ProjectAutoTempoRequest>(fnName, args, (value) => {
      if (!isAbsent(value.candidateIndex) && typeof value.candidateIndex !== 'number') {
        throw refuseType('candidateIndex must be an integer');
      }
      for (const [name, field] of [
        ['tempoUpdateIntervalBeats', value.tempoUpdateIntervalBeats],
        ['rampThreshold', value.rampThreshold],
      ] as const) {
        if (!isAbsent(field) && typeof field !== 'number') {
          throw refuseType(`${name} must be a number`);
        }
      }
      for (const [name, field] of [
        ['applyTimeSignatures', value.applyTimeSignatures],
        ['adaptiveTempo', value.adaptiveTempo],
        ['includeOctaveCandidates', value.includeOctaveCandidates],
      ] as const) {
        if (!isAbsent(field) && typeof field !== 'boolean') {
          throw refuseType(`${name} must be a boolean`);
        }
      }
    });
    assertAudioInput(request.samples, request.sampleRate);
    return this.native.autoTempo(
      request.samples,
      request.sampleRate,
      request.candidateIndex ?? 0,
      request.applyTimeSignatures ?? false,
      tempoOptionsToNative(request),
    );
  }

  /**
   * Snap a PPQ coordinate, in [0, 1e12], to the project grid. `division` is `0` for bars,
   * `1` for beats, or the number of subdivisions per beat. `strength` is in
   * `[0, 1]` (0 = no snap, 1 = exact grid line).
   */
  snapToGrid(ppq: number, strength = 1.0, division = 1): number {
    return this.native.snapToGrid(ppq, strength, division);
  }

  /**
   * Replace the project's key annotation stream via an undoable edit (existing
   * chord / section / onset annotations are preserved).
   */
  annotateKeys(keys: ProjectKeySegment[]): void {
    if (Array.isArray(keys)) {
      requireListBudget(keys, 'keys');
    }
    this.native.annotateKeys(
      keys.map((key) => ({
        ...key,
        ...(key.mode === undefined ? {} : { mode: tokenOrdinal(key.mode, KEY_MODE, 'mode') }),
      })),
    );
  }

  /** Read key annotations in their stored order. */
  keyAnnotations(): ProjectKeySegment[] {
    const keys = this.native.keyAnnotations() as unknown as Record<string, unknown>[];
    return keys.map((key) => {
      const tonicPc = key.tonicPc as number;
      if (tonicPc !== 255 && (!Number.isInteger(tonicPc) || tonicPc < 0 || tonicPc > 11)) {
        throw unknownOrdinal('pitch class', tonicPc);
      }
      return {
        startPpq: key.startPpq as number,
        endPpq: key.endPpq as number,
        ...(tonicPc === 255 ? {} : { tonicPc }),
        mode: tokenName(KEY_MODE, key.mode as number, 'key mode'),
      };
    });
  }

  /** Replace the project's chord-symbol annotation stream via an undoable edit. */
  annotateChords(chords: ReadonlyArray<ProjectChordSymbol>): void {
    if (!Array.isArray(chords)) {
      throw refuseType('chords must be an array');
    }
    requireListBudget(chords, 'chords');
    this.native.annotateChords(
      chords.map((chord: unknown, index) => {
        if (!isNonArrayObject(chord)) {
          throw refuseType(`chords[${index}] must be an object`);
        }
        return {
          ...chord,
          ...(chord.quality === undefined
            ? {}
            : { quality: tokenOrdinal(chord.quality, CHORD_QUALITY, 'quality') }),
        };
      }),
    );
  }

  /** Read chord-symbol annotations in their stored order. */
  chordAnnotations(): ProjectChordSymbol[] {
    const chords = this.native.chordAnnotations() as unknown as Record<string, unknown>[];
    return chords.map((chord) => {
      const rootPc = chord.rootPc as number;
      const slashBassPc = chord.slashBassPc as number;
      if (rootPc !== 255 && (!Number.isInteger(rootPc) || rootPc < 0 || rootPc > 11)) {
        throw unknownOrdinal('pitch class', rootPc);
      }
      if (
        slashBassPc !== 255 &&
        (!Number.isInteger(slashBassPc) || slashBassPc < 0 || slashBassPc > 11)
      ) {
        throw unknownOrdinal('pitch class', slashBassPc);
      }
      const romanNumeral = chord.romanNumeral;
      return {
        startPpq: chord.startPpq as number,
        endPpq: chord.endPpq as number,
        ...(rootPc === 255 ? {} : { rootPc }),
        quality: tokenName(CHORD_QUALITY, chord.quality as number, 'chord quality'),
        extensions: chord.extensions as number[],
        ...(slashBassPc === 255 ? {} : { slashBassPc }),
        ...(typeof romanNumeral === 'string' && romanNumeral.length > 0 ? { romanNumeral } : {}),
        modulationBoundary: chord.modulationBoundary as boolean,
      };
    });
  }

  /** Replace all labeled section annotations as one undoable edit. */
  annotateSections(sections: ProjectSection[]): void {
    if (!Array.isArray(sections)) {
      throw refuseType('sections must be an array');
    }
    requireListBudget(sections, 'sections');
    const normalized = sections.map((section: unknown, index) => {
      const at = `sections[${index}]`;
      if (!isNonArrayObject(section)) {
        throw refuseType(`${at} must be an object`);
      }
      assertDeclaredKeys(section, ['startPpq', 'endPpq', 'label'], at);
      const startPpq = section.startPpq;
      const endPpq = section.endPpq;
      assertSectionPpq(startPpq, `${at}.startPpq`);
      assertSectionPpq(endPpq, `${at}.endPpq`);
      if (endPpq <= startPpq) {
        throw refuse(`${at}.endPpq must be greater than ${at}.startPpq`);
      }
      const label = requireProjectText(section.label, `${at}.label`);
      return { startPpq, endPpq, label };
    });
    this.native.annotateSections(normalized);
  }

  /** Read all labeled section annotations in stored order. */
  sectionAnnotations(): ProjectSection[] {
    return this.native.sectionAnnotations();
  }

  // -- assist sidecars --

  /**
   * Add or update an opaque assist sidecar (keyed by module id + target scope)
   * via an undoable edit. The payload bytes are copied.
   */
  setAssistSidecar(sidecar: ProjectAssistSidecarInput): void {
    this.native.setAssistSidecar(requestObject('setAssistSidecar', sidecar, 'sidecar', true));
  }

  /** Every stored assist sidecar, in stable project order. */
  assistSidecars(): ProjectAssistSidecar[] {
    return this.native.assistSidecars();
  }

  // -- compile / render --

  /**
   * Compile the project into an RT-readable timeline, surfacing diagnostics.
   *
   * With `withTimeline: true` the result also carries `timeline`, absent when compilation
   * produced none. Install it with {@link RealtimeEngine.applyProjectTimeline}; later project
   * edits do not change an already compiled timeline.
   */
  compile(options?: ProjectCompileOptions): ProjectCompileResult {
    if (options === undefined) {
      return { diagnostics: diagnosticsFromNative(this.native.compile().diagnostics) };
    }
    const request = requestObject('compile', options, 'options', true);
    const withTimeline = isAbsent(request.withTimeline)
      ? false
      : requireBoolean(request.withTimeline, 'withTimeline');
    if (!withTimeline) {
      return { diagnostics: diagnosticsFromNative(this.native.compile().diagnostics) };
    }
    const raw = this.native.compile(true);
    const diagnostics = diagnosticsFromNative(raw.diagnostics);
    return raw.timeline === null
      ? { diagnostics }
      : { diagnostics, timeline: createTimeline(raw.timeline) };
  }

  /**
   * Retrieve the compile result from the most recent successful {@link bounce}.
   *
   * `undefined` until a bounce succeeds. A successful bounce with no diagnostics
   * returns an empty list; a failed bounce reports its own error and leaves this
   * result unchanged.
   */
  lastBounceCompileResult(): ProjectCompileResult | undefined {
    const raw = this.native.lastBounceCompileResult();
    if (!raw.hasTimeline && raw.diagnostics.length === 0) {
      return undefined;
    }
    return { diagnostics: diagnosticsFromNative(raw.diagnostics) };
  }

  /**
   * Compile + render the project offline to planar float audio at the project
   * sample rate. Deterministic: the same project + options yields bit-identical
   * output within one build.
   *
   * Omitting `options.totalFrames` derives the render length from the arrangement.
   * An empty project yields no frames.
   *
   * MIDI tracks routed to a destination no instrument list binds render as
   * silence. Bind instruments with `builtinInstruments` (built-in oscillator
   * synth), `synthInstruments` (patch-driven NativeSynth), `sf2Instruments`
   * (GS-compatible SoundFont players fed by {@link loadSoundFont}) or
   * `instruments` (JavaScript instruments); an empty list
   * renders silence. A destination bound twice, within one list or across
   * lists, is refused.
   *
   * Every `instruments` callback runs synchronously on the calling thread while
   * this method is blocked, so a callback that returns a Promise is refused with
   * a `TypeError`. A callback that throws stops all further callbacks, and the
   * bounce rethrows that first error once it returns (a native failure is
   * reported ahead of it). While the bounce runs, using this project from inside
   * a callback (a mutation, a nested bounce, or `destroy`) throws `InvalidState`.
   * One instrument shared by tracks that feed different channel strips throws a
   * `SonareError` with `NOT_SUPPORTED`; bind one destination per strip instead.
   */
  bounce(options?: ProjectBounceOptions): ProjectBounceResult {
    const request = requestObject('bounce', options ?? {}, 'options', true);
    const builtin =
      request.builtinInstruments === undefined
        ? undefined
        : bindInstruments(
            'builtinInstruments',
            request.builtinInstruments,
            (i, _binding, label) => {
              checkBuiltinConfig(i as Record<string, unknown>, `${label}.instrument`);
              return i;
            },
            withDestinationCopy,
          );
    const synth =
      request.synthInstruments === undefined
        ? undefined
        : bindInstruments(
            'synthInstruments',
            request.synthInstruments,
            (i, _binding, label) => {
              checkSynthPatch(i as Record<string, unknown>, `${label}.instrument`);
              return normalizeSynthInstrument(i) as SynthPatch;
            },
            withDestinationCopy,
          );
    const sf2 =
      request.sf2Instruments === undefined
        ? undefined
        : bindInstruments(
            'sf2Instruments',
            request.sf2Instruments,
            (instrument, binding, label) => {
              if ('soundFontUri' in instrument) {
                throw refuseUnknownKey(`${label}.instrument.soundFontUri`, [
                  `${label}.soundFontUri`,
                ]);
              }
              checkSf2Config(instrument as Record<string, unknown>, `${label}.instrument`);
              if (binding.soundFontUri === undefined) {
                return instrument;
              }
              const subject = `${label}.soundFontUri`;
              const uri = requireProjectText(binding.soundFontUri, subject);
              if (uri.length === 0) {
                throw refuse(`${subject} must not be empty`);
              }
              return { ...instrument, soundFontUri: uri };
            },
            withDestinationCopy,
            SF2_BINDING_KEYS,
          );
    const callbacks =
      request.instruments === undefined
        ? undefined
        : bindInstruments('instruments', request.instruments, (i) => i, withDestinationLayer);
    const settings = bounceSettings(request);
    const native = this.native;
    const run = (): ReturnType<typeof native.bounce> => {
      try {
        return callbacks === undefined
          ? native.bounce(settings, builtin, synth, sf2)
          : native.bounce(settings, builtin, synth, sf2, callbacks);
      } catch (error) {
        throw respellBounceInstrument(error);
      }
    };
    if (callbacks === undefined) {
      return planarBounce(run(), native.getSampleRate());
    }
    this.instrumentBounceActive = true;
    try {
      return planarBounce(run(), native.getSampleRate());
    } finally {
      this.instrumentBounceActive = false;
    }
  }

  /**
   * Load (parse) SoundFont 2 bytes into the project: presets / instruments /
   * sample headers plus the sample PCM decoded to a float pool. Replaces the
   * font at the selected URI. Omit the URI to use the default font.
   */
  loadSoundFont(data: Uint8Array, options?: ProjectSoundFontOptions): void {
    withEntry('Project.loadSoundFont', () => {
      if (data instanceof Uint8Array && data.length === 0) {
        throw refuse('data must not be empty');
      }
      this.native.loadSoundFont(data, soundFontUri(options));
    });
  }

  /** Release the project's loaded SoundFont (no-op when none is loaded). */
  clearSoundFont(options?: ProjectSoundFontOptions): void {
    withEntry('Project.clearSoundFont', () => this.native.clearSoundFont(soundFontUri(options)));
  }

  /** Number of presets in the loaded SoundFont (0 when none is loaded). */
  soundFontPresetCount(options?: ProjectSoundFontOptions): number {
    return withEntry('Project.soundFontPresetCount', () =>
      this.native.soundFontPresetCount(soundFontUri(options)),
    );
  }

  /**
   * Enumerate every (channel, bank, program) combination the arrangement plays
   * a note through, in first-use order, reporting whether each resolves in the
   * loaded SoundFont (`'sf2'`, GS variation/drum fallbacks included) or would
   * fall back to the built-in synth (`'synth'`). Without a loaded SoundFont
   * every entry is a synth fallback.
   */
  soundFontManifest(options?: ProjectSoundFontOptions): Sf2ProgramStatus[] {
    return withEntry('Project.soundFontManifest', () =>
      this.native.soundFontManifest(soundFontUri(options)),
    );
  }

  /** Release the underlying native project. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**
   * Releases the native project; lets `using` free it automatically (needs TypeScript 5.2+
   * or a runtime with native explicit resource management; Node 22 does not parse `using`).
   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

function adoptProject(native: NativeProject): Project {
  return new (Project as unknown as new (adopted: AdoptedProject) => Project)(
    new AdoptedProject(native),
  );
}
