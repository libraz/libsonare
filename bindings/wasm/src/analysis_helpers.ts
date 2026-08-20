import { resolveEnumOrdinal } from './codes';
import type {
  AnalysisResult,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChordQuality,
  KeyCandidate,
  KeyDetectionOptions,
  KeyProfileName,
  PitchClass,
  SectionType,
} from './public_types';
import { KeyProfile as KeyProfileValues, Mode } from './public_types';
import type {
  WasmAnalysisResult,
  WasmChordAnalysisResult,
  WasmKeyCandidateResult,
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

export function convertKeyCandidate(wasm: WasmKeyCandidateResult): KeyCandidate {
  return {
    key: {
      root: wasm.key.root as PitchClass,
      mode: wasm.key.mode as Mode,
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    correlation: wasm.correlation,
  };
}

// Consolidated spelling -> ordinal tables, resolved through the same
// resolveEnumOrdinal() primitive as panModeCode/panLawCode/etc. in ./codes.
// An unmapped string or an out-of-range numeric ordinal throws RangeError
// instead of silently resolving to `undefined` (which used to reach the WASM
// layer as a mistyped argument) or passing an unvalidated raw number through.
const KEY_MODE_VALUES: Readonly<Record<string, number>> = {
  major: Mode.Major,
  minor: Mode.Minor,
  dorian: Mode.Dorian,
  phrygian: Mode.Phrygian,
  lydian: Mode.Lydian,
  mixolydian: Mode.Mixolydian,
  locrian: Mode.Locrian,
};

const KEY_PROFILE_VALUES: Readonly<Record<KeyProfileName, number>> = {
  ks: KeyProfileValues.KrumhanslSchmuckler,
  krumhansl: KeyProfileValues.KrumhanslSchmuckler,
  temperley: KeyProfileValues.Temperley,
  shaath: KeyProfileValues.Shaath,
  keyfinder: KeyProfileValues.Shaath,
  'faraldo-edmt': KeyProfileValues.FaraldoEDMT,
  edmt: KeyProfileValues.FaraldoEDMT,
  'faraldo-edma': KeyProfileValues.FaraldoEDMA,
  edma: KeyProfileValues.FaraldoEDMA,
  'faraldo-edmm': KeyProfileValues.FaraldoEDMM,
  edmm: KeyProfileValues.FaraldoEDMM,
  'bellman-budge': KeyProfileValues.BellmanBudge,
  bellman: KeyProfileValues.BellmanBudge,
};

export function keyModeValues(modes: KeyDetectionOptions['modes'] | undefined): number[] {
  if (!modes) {
    return [];
  }
  if (modes === 'major-minor') {
    return [Mode.Major, Mode.Minor];
  }
  if (modes === 'all' || modes === 'modal') {
    return [
      Mode.Major,
      Mode.Minor,
      Mode.Dorian,
      Mode.Phrygian,
      Mode.Lydian,
      Mode.Mixolydian,
      Mode.Locrian,
    ];
  }
  return modes.map((mode) => resolveEnumOrdinal(mode, KEY_MODE_VALUES, 'key mode'));
}

export function keyProfileValue(profile: KeyDetectionOptions['profile'] | undefined): number {
  if (profile === undefined) {
    return -1;
  }
  return resolveEnumOrdinal(profile, KEY_PROFILE_VALUES, 'key profile');
}

export function convertChordAnalysisResult(wasm: WasmChordAnalysisResult): ChordAnalysisResult {
  return {
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      bass: c.bass as PitchClass,
      rootName: pitchClassName(c.root),
      bassName: pitchClassName(c.bass),
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
      duration: c.end - c.start,
      confidence: c.confidence,
      name: c.name,
    })),
  };
}

export function chordChromaMethodValue(method: ChordDetectionOptions['chromaMethod']): number {
  if (method === 'stft') {
    return 0;
  }
  if (method === 'nnls') {
    return 1;
  }
  throw new Error(`Invalid chord chroma method: ${method}`);
}

export function convertAnalysisResult(wasm: WasmAnalysisResult): AnalysisResult {
  const beatTimes = new Float32Array(wasm.beats.length);
  for (let i = 0; i < wasm.beats.length; i++) {
    beatTimes[i] = wasm.beats[i].time;
  }
  return {
    bpm: wasm.bpm,
    bpmConfidence: wasm.bpmConfidence,
    bpmCandidates: wasm.bpmCandidates.map((candidate) => ({
      value: candidate.value,
      confidence: candidate.confidence,
      relation: candidate.relation,
    })),
    key: {
      root: wasm.key.root as PitchClass,
      mode: wasm.key.mode as Mode,
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    timeSignature: wasm.timeSignature,
    timeSignatureCandidates: wasm.timeSignatureCandidates,
    beatTimes,
    beats: wasm.beats,
    downbeatIndices: wasm.downbeatIndices,
    downbeatPhase: wasm.downbeatPhase,
    beatObservations: wasm.beatObservations,
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      bass: c.bass as PitchClass,
      rootName: pitchClassName(c.root),
      bassName: pitchClassName(c.bass),
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
      duration: c.end - c.start,
      confidence: c.confidence,
      name: c.name,
    })),
    sections: wasm.sections.map((s) => ({
      type: s.type as SectionType,
      start: s.start,
      end: s.end,
      energyLevel: s.energyLevel,
      confidence: s.confidence,
      name: s.name,
    })),
    timbre: wasm.timbre,
    dynamics: wasm.dynamics,
    rhythm: wasm.rhythm,
    melody: wasm.melody,
    form: wasm.form,
  };
}
