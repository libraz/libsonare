import type {
  AnalysisResult,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordQuality,
  GenreHint,
  KeyCandidate,
  KeyDetectionOptions,
  KeyMode,
  PitchClass,
  Section,
} from './public_types.js';
import { CHORD_QUALITIES, SECTION_TYPES } from './public_types.js';
import type {
  WasmAnalysisResult,
  WasmChordAnalysisResult,
  WasmChordResult,
  WasmKeyCandidateResult,
  WasmSectionResult,
} from './sonare.js';

const PITCH_CLASS_NAMES = [
  'C',
  'C#',
  'D',
  'D#',
  'E',
  'F',
  'F#',
  'G',
  'G#',
  'A',
  'A#',
  'B',
] as const;

function pitchClassName(value: number): string {
  return PITCH_CLASS_NAMES[value] ?? 'C';
}

const KEY_MODE_NAMES = [
  'major',
  'minor',
  'dorian',
  'phrygian',
  'lydian',
  'mixolydian',
  'locrian',
] as const satisfies readonly KeyMode[];

/** Name of a key-mode ordinal as the module reports it. */
export function keyModeName(ordinal: number): KeyMode {
  const name = KEY_MODE_NAMES[ordinal];
  if (name === undefined) {
    throw new RangeError(`Invalid key mode: ${String(ordinal)}`);
  }
  return name;
}

export function convertKeyCandidate(wasm: WasmKeyCandidateResult): KeyCandidate {
  return {
    key: {
      root: wasm.key.root as PitchClass,
      mode: keyModeName(wasm.key.mode),
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    correlation: wasm.correlation,
  };
}

function closedSetOrdinal(value: unknown, tokens: readonly string[], field: string): number {
  if (typeof value !== 'string') {
    throw new TypeError(`${field} must be one of ${tokens.map((t) => `'${t}'`).join(', ')}`);
  }
  const ordinal = tokens.indexOf(value);
  if (ordinal < 0) {
    throw new RangeError(
      `${field} must be one of ${tokens.map((t) => `'${t}'`).join(', ')}, got '${value}'`,
    );
  }
  return ordinal;
}

/** Ordinal of a key mode given by name; numbers and synonyms are refused. */
export function keyModeOrdinal(mode: unknown, field = 'key mode'): number {
  return closedSetOrdinal(mode, KEY_MODE_NAMES, field);
}

/** Ordinal of a pitch class 0-11; names and non-integers are refused. */
export function pitchClassOrdinal(fnName: string, root: unknown, field: string): number {
  if (typeof root !== 'number') {
    throw new TypeError(`${fnName}: ${field} must be a pitch class number 0-11`);
  }
  if (!Number.isInteger(root) || root < 0 || root > 11) {
    throw new RangeError(`${fnName}: ${field} must be an integer in [0, 11]`);
  }
  return root;
}

/** Native ordinal of a chord-quality token; numbers are refused. */
export function chordQualityOrdinal(quality: unknown, field = 'chord quality'): number {
  return closedSetOrdinal(quality, CHORD_QUALITIES, field);
}

/** Token of a native chord-quality ordinal; an ordinal outside the table is refused. */
export function chordQualityToken(ordinal: number): ChordQuality {
  const token = CHORD_QUALITIES[ordinal];
  if (token === undefined) {
    throw new RangeError(`Invalid chord quality ordinal: ${String(ordinal)}`);
  }
  return token;
}

function sectionTypeToken(ordinal: number): Section['type'] {
  const token = SECTION_TYPES[ordinal];
  if (token === undefined) {
    throw new RangeError(`Invalid section type ordinal: ${String(ordinal)}`);
  }
  return token;
}

const ALL_MODE_ORDINALS = [0, 1, 2, 3, 4, 5, 6];

export function keyModeValues(modes: KeyDetectionOptions['modes'] | undefined): number[] {
  if (modes === undefined || modes === null) {
    return [];
  }
  if (typeof modes === 'string') {
    if (modes === 'majorMinor') {
      return [0, 1];
    }
    if (modes === 'all' || modes === 'modal') {
      return ALL_MODE_ORDINALS;
    }
    throw new RangeError(
      `modes must be an array of mode names, 'majorMinor', 'all' or 'modal', got '${modes}'`,
    );
  }
  if (!Array.isArray(modes)) {
    throw new TypeError('modes must be an array or mode-set string');
  }
  return modes.map((mode) => keyModeOrdinal(mode, 'key mode'));
}

const KEY_PROFILE_TOKENS = [
  'krumhanslSchmuckler',
  'temperley',
  'shaath',
  'faraldoEdmt',
  'faraldoEdma',
  'faraldoEdmm',
  'bellmanBudge',
] as const;

export function keyProfileValue(profile: KeyDetectionOptions['profile'] | undefined): number {
  if (profile === undefined || profile === null) {
    return -1;
  }
  return closedSetOrdinal(profile, KEY_PROFILE_TOKENS, 'key profile');
}

const GENRE_HINTS = ['auto', 'edm', 'electronic', 'dance', 'pop', 'classical', 'jazz'] as const;

/** The genre hint the module reads (`''` when none is given); an unknown token is refused here. */
export function genreHintValue(hint: GenreHint | undefined | null): string {
  if (hint === undefined || hint === null) {
    return '';
  }
  closedSetOrdinal(hint, GENRE_HINTS, 'genreHint');
  return hint;
}

export function chordChromaMethodValue(method: ChordChromaMethod | undefined): number {
  return closedSetOrdinal(method, ['stft', 'nnls', 'cqt'], 'chromaMethod');
}

export function convertSection(s: WasmSectionResult): Section {
  return { ...s, type: sectionTypeToken(s.type) };
}

function convertChord(c: WasmChordResult) {
  return {
    ...c,
    root: c.root as PitchClass,
    bass: c.bass as PitchClass,
    rootName: pitchClassName(c.root),
    bassName: pitchClassName(c.bass),
    quality: chordQualityToken(c.quality),
  };
}

export function convertChordAnalysisResult(wasm: WasmChordAnalysisResult): ChordAnalysisResult {
  return {
    chords: wasm.chords.map(convertChord),
    tuningCents: wasm.tuningCents,
  };
}

// The analysis object already carries the public key names; only the coded
// enums (pitch class, key mode, chord quality, section type) are decoded here.
export function convertAnalysisResult(wasm: WasmAnalysisResult): AnalysisResult {
  return {
    ...wasm,
    key: {
      root: wasm.key.root as PitchClass,
      mode: keyModeName(wasm.key.mode),
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    chords: wasm.chords.map((c) => ({ ...convertChord(c), romanNumeral: c.romanNumeral })),
    sections: wasm.sections.map(convertSection),
    rhythm: {
      ...wasm.rhythm,
      grooveType: wasm.rhythm.grooveType as AnalysisResult['rhythm']['grooveType'],
    },
  };
}
