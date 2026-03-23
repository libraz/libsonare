export interface Key {
  root: string;
  mode: string;
  confidence: number;
}

export interface TimeSignature {
  numerator: number;
  denominator: number;
  confidence: number;
}

export interface AnalysisResult {
  bpm: number;
  bpmConfidence: number;
  key: Key;
  timeSignature: TimeSignature;
  beatTimes: Float32Array;
}
