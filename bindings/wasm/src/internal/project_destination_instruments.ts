import { ErrorCode } from '../errors.js';
import type { BuiltinSynthConfig, Sf2InstrumentConfig, SynthPatch } from '../instrument_types.js';
import {
  BUILTIN_SYNTH_WAVEFORMS,
  SYNTH_ENGINE_MODES,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
} from '../instrument_types.js';
import { synthPatchParamInfo } from '../project_synth.js';
import type { ProjectDestinationInstrumentOptions, ProjectInstrument } from '../project_types.js';
import { assertClosedToken, assertFiniteScalar, assertU7, optionalBoolean } from '../validation.js';
import { assertDeclaredKeys, codedError, refuse, refuseType } from './entry.js';
import { refuseBuiltinSynthOrdinals, refuseSynthPatchOrdinals } from './synth_tokens.js';
import { isAbsent, isNonArrayObject } from './value_guards.js';

const PROJECT_INSTRUMENT_KINDS = ['synth', 'builtin', 'sf2'] as const;
const NONPERSISTENT_INSTRUMENT_KINDS = ['sample', 'sampler', 'callback', 'external'];
const SYNTH_INSTRUMENT_KEYS = ['kind', 'patch', 'useGmPrograms'];
const BUILTIN_INSTRUMENT_KEYS = ['kind', 'config'];
const SF2_INSTRUMENT_KEYS = ['kind', 'config', 'soundFontUri'];
const PROJECT_INSTRUMENT_OPTIONS_KEYS = ['bank', 'program'];
const MIDI_BANK_KEYS = ['msb', 'lsb'];
const SYNTH_PATCH_KEYS = [
  'preset',
  'engineMode',
  'engineParams',
  'modRoutings',
  'useGmPrograms',
  'sampleBank',
];
const SYNTH_MOD_ROUTING_KEYS = ['source', 'destination', 'depth'];
const BUILTIN_CONFIG_KEYS = [
  'waveform',
  'gain',
  'attackMs',
  'decayMs',
  'sustain',
  'releaseMs',
  'polyphony',
];
const SF2_CONFIG_KEYS = [
  'gain',
  'polyphony',
  'preferModelForModeledFamilies',
  'clearBankRig',
  'gsEfxRealization',
];

interface NormalizedProjectDestinationInstrument {
  instrument: ProjectInstrument | undefined;
  options: ProjectDestinationInstrumentOptions | undefined;
}

function normalizeDestinationInstrumentOptions(
  value: unknown,
): ProjectDestinationInstrumentOptions | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (!isNonArrayObject(value)) {
    throw refuseType('options must be an object');
  }
  assertDeclaredKeys(value, PROJECT_INSTRUMENT_OPTIONS_KEYS, 'options');

  const bank = value.bank;
  let normalizedBank: ProjectDestinationInstrumentOptions['bank'];
  if (!isAbsent(bank)) {
    if (!isNonArrayObject(bank)) {
      throw refuseType('options.bank must be an object');
    }
    assertDeclaredKeys(bank, MIDI_BANK_KEYS, 'options.bank');
    if (isAbsent(bank.msb)) {
      throw refuseType('options.bank.msb must be given');
    }
    if (isAbsent(bank.lsb)) {
      throw refuseType('options.bank.lsb must be given');
    }
    normalizedBank = {
      msb: assertU7(bank.msb as number, 'options.bank.msb'),
      lsb: assertU7(bank.lsb as number, 'options.bank.lsb'),
    };
  }
  const program = value.program;
  const normalizedProgram = isAbsent(program)
    ? undefined
    : assertU7(program as number, 'options.program');
  if (normalizedBank === undefined && normalizedProgram === undefined) {
    return undefined;
  }
  return {
    ...(normalizedBank === undefined ? {} : { bank: normalizedBank }),
    ...(normalizedProgram === undefined ? {} : { program: normalizedProgram }),
  };
}

function requireProjectText(value: unknown, name: string): string {
  if (typeof value !== 'string') {
    throw refuseType(`${name} must be a string`);
  }
  if (value.includes('\0')) {
    throw refuse(`${name} must not contain NUL`);
  }
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff) {
      const next = value.charCodeAt(i + 1);
      if (!(next >= 0xdc00 && next <= 0xdfff)) {
        throw refuse(`${name} must contain well-formed Unicode`);
      }
      i++;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      throw refuse(`${name} must contain well-formed Unicode`);
    }
  }
  return value;
}

/**
 * Refuses a synth patch the caller wrote at @p at by its path: an undeclared key, a field of the
 * wrong type, a token outside its set, an empty preset name and a malformed mod routing.
 */
export function checkSynthPatch(patch: Record<string, unknown>, at: string): void {
  const patchInfo = synthPatchParamInfo();
  assertDeclaredKeys(patch, [...SYNTH_PATCH_KEYS, ...patchInfo.map((field) => field.name)], at);
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
  for (const field of patchInfo) {
    const value = patch[field.name];
    if (isAbsent(value)) {
      continue;
    }
    if (field.type === 'enum') {
      assertClosedToken(value, field.values, `${at}.${field.name}`);
    } else if (field.type === 'boolean' && typeof value !== 'boolean') {
      throw refuseType(`${at}.${field.name} must be a boolean`);
    } else if (field.type === 'number' && typeof value !== 'number') {
      throw refuseType(`${at}.${field.name} must be a number`);
    }
  }
  if (!isAbsent(patch.engineParams) && !isNonArrayObject(patch.engineParams)) {
    throw refuseType(`${at}.engineParams must be an object`);
  }
  if (!isAbsent(patch.modRoutings)) {
    if (!Array.isArray(patch.modRoutings)) {
      throw refuseType(`${at}.modRoutings must be an array`);
    }
    if (patch.modRoutings.length > 8) {
      throw refuse(`${at}.modRoutings must contain at most 8 entries`);
    }
    for (const [index, routing] of patch.modRoutings.entries()) {
      const path = `${at}.modRoutings[${index}]`;
      if (!isNonArrayObject(routing)) {
        throw refuseType(`${path} must be an object`);
      }
      assertDeclaredKeys(routing, SYNTH_MOD_ROUTING_KEYS, path);
      if (isAbsent(routing.source)) {
        throw refuseType(`${path}.source must be given`);
      }
      if (isAbsent(routing.destination)) {
        throw refuseType(`${path}.destination must be given`);
      }
      assertClosedToken(routing.source, SYNTH_MOD_SOURCES, `${path}.source`);
      assertClosedToken(routing.destination, SYNTH_MOD_DESTINATIONS, `${path}.destination`);
      for (const end of ['source', 'destination'] as const) {
        if (routing[end] === 'none') {
          throw refuse(`${path}.${end} must not be none`);
        }
      }
      assertFiniteScalar(routing.depth as number, `${path}.depth`);
    }
  }
}

/** Refuses a builtin synth configuration the caller wrote at @p at by its path. */
export function checkBuiltinConfig(config: Record<string, unknown>, at: string): void {
  assertDeclaredKeys(config, BUILTIN_CONFIG_KEYS, at);
  refuseBuiltinSynthOrdinals(config);
  if (!isAbsent(config.waveform)) {
    assertClosedToken(config.waveform, BUILTIN_SYNTH_WAVEFORMS, `${at}.waveform`);
  }
}

/** Refuses a SoundFont instrument configuration the caller wrote at @p at by its path. */
export function checkSf2Config(config: Record<string, unknown>, at: string): void {
  assertDeclaredKeys(config, SF2_CONFIG_KEYS, at);
  if (!isAbsent(config.gsEfxRealization)) {
    assertClosedToken(config.gsEfxRealization, ['modern', 'classic'], `${at}.gsEfxRealization`);
  }
  if (!isAbsent(config.preferModelForModeledFamilies)) {
    optionalBoolean(
      config.preferModelForModeledFamilies,
      `${at}.preferModelForModeledFamilies`,
      false,
    );
  }
  if (!isAbsent(config.clearBankRig)) {
    optionalBoolean(config.clearBankRig, `${at}.clearBankRig`, false);
  }
}

/** Validate one destination instrument and its bank/program overrides. */
export function normalizeProjectDestinationInstrument(
  instrument: unknown,
  options: unknown,
): NormalizedProjectDestinationInstrument {
  const normalizedOptions = normalizeDestinationInstrumentOptions(options);
  if (isAbsent(instrument)) {
    if (normalizedOptions !== undefined) {
      throw refuse('bank and program require an instrument');
    }
    return { instrument: undefined, options: undefined };
  }
  if (
    typeof instrument === 'function' ||
    (isNonArrayObject(instrument) &&
      isAbsent(instrument.kind) &&
      typeof instrument.render === 'function')
  ) {
    throw codedError(ErrorCode.NotSupported, 'callback instruments cannot be persisted', {
      reason: 'notPersistable',
    });
  }
  if (!isNonArrayObject(instrument)) {
    throw refuseType('instrument must be an object');
  }
  const kind = instrument.kind;
  if (typeof kind !== 'string') {
    throw refuseType('instrument.kind must be a string token');
  }
  if ((NONPERSISTENT_INSTRUMENT_KINDS as readonly string[]).includes(kind)) {
    throw codedError(ErrorCode.NotSupported, 'this instrument kind cannot be persisted', {
      reason: 'notPersistable',
    });
  }
  assertClosedToken(kind, PROJECT_INSTRUMENT_KINDS, 'instrument.kind');

  let normalized: ProjectInstrument;
  if (kind === 'synth') {
    assertDeclaredKeys(instrument, SYNTH_INSTRUMENT_KEYS, 'instrument');
    if (!isNonArrayObject(instrument.patch)) {
      throw refuseType('instrument.patch must be an object');
    }
    const patch = instrument.patch;
    if (!isAbsent(patch.sampleBank)) {
      throw codedError(
        ErrorCode.NotSupported,
        'sample-bank patches cannot be stored as destination instruments',
        { reason: 'notPersistable' },
      );
    }
    checkSynthPatch(patch, 'instrument.patch');
    const rawUseGmPrograms = isAbsent(instrument.useGmPrograms)
      ? patch.useGmPrograms
      : instrument.useGmPrograms;
    if (!isAbsent(instrument.useGmPrograms) && !isAbsent(patch.useGmPrograms)) {
      const outer = optionalBoolean(instrument.useGmPrograms, 'instrument.useGmPrograms', false);
      const inner = optionalBoolean(patch.useGmPrograms, 'instrument.patch.useGmPrograms', false);
      if (outer !== inner) {
        throw refuse('instrument.useGmPrograms conflicts with instrument.patch.useGmPrograms');
      }
    }
    const useGmPrograms = isAbsent(rawUseGmPrograms)
      ? undefined
      : optionalBoolean(rawUseGmPrograms, 'instrument.useGmPrograms', false);
    const {
      sampleBank: _sampleBank,
      useGmPrograms: _patchUseGmPrograms,
      ...patchFields
    } = patch as SynthPatch;
    normalized = {
      kind,
      patch: patchFields,
      ...(useGmPrograms === undefined ? {} : { useGmPrograms }),
    };
  } else if (kind === 'builtin') {
    assertDeclaredKeys(instrument, BUILTIN_INSTRUMENT_KEYS, 'instrument');
    if (!isNonArrayObject(instrument.config)) {
      throw refuseType('instrument.config must be an object');
    }
    checkBuiltinConfig(instrument.config, 'instrument.config');
    normalized = { kind, config: instrument.config as unknown as BuiltinSynthConfig };
  } else if (kind === 'sf2') {
    assertDeclaredKeys(instrument, SF2_INSTRUMENT_KEYS, 'instrument');
    if (!isNonArrayObject(instrument.config)) {
      throw refuseType('instrument.config must be an object');
    }
    checkSf2Config(instrument.config, 'instrument.config');
    const soundFontUri = isAbsent(instrument.soundFontUri)
      ? undefined
      : requireProjectText(instrument.soundFontUri, 'instrument.soundFontUri');
    if (soundFontUri === '') {
      throw refuse('instrument.soundFontUri must not be empty');
    }
    normalized = {
      kind,
      config: instrument.config as unknown as Sf2InstrumentConfig,
      ...(soundFontUri === undefined ? {} : { soundFontUri }),
    };
  } else {
    throw refuse('instrument.kind must be one of synth, builtin, sf2');
  }

  return { instrument: normalized, options: normalizedOptions };
}
