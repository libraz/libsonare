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
  const names = {
    major: Mode.Major,
    minor: Mode.Minor,
    dorian: Mode.Dorian,
    phrygian: Mode.Phrygian,
    lydian: Mode.Lydian,
    mixolydian: Mode.Mixolydian,
    locrian: Mode.Locrian,
  } as const;
  return modes.map((mode) => (typeof mode === 'number' ? mode : names[mode]));
}

export function keyProfileValue(profile: KeyDetectionOptions['profile'] | undefined): number {
  if (profile === undefined) {
    return -1;
  }
  if (typeof profile === 'number') {
    return profile;
  }
  const names: Record<KeyProfileName, number> = {
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
  return names[profile];
}

export function convertChordAnalysisResult(wasm: WasmChordAnalysisResult): ChordAnalysisResult {
  return {
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      bass: c.bass as PitchClass,
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
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
    key: {
      root: wasm.key.root as PitchClass,
      mode: wasm.key.mode as Mode,
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    timeSignature: wasm.timeSignature,
    beatTimes,
    beats: wasm.beats,
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      bass: c.bass as PitchClass,
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
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
    form: wasm.form,
  };
}
