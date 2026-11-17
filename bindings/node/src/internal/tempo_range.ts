import type { TempoRangeOptions } from '../analysis.js';
import { assertFrequencyBounds } from '../validation.js';

const DEFAULT_BPM_MIN = 60;
const DEFAULT_BPM_MAX = 200;

/** The search range the addon requires, defaults filled in and checked as one pair. */
export function tempoRange(fnName: string, options: TempoRangeOptions): [number, number] {
  const bpmMin = options.bpmMin ?? DEFAULT_BPM_MIN;
  const bpmMax = options.bpmMax ?? DEFAULT_BPM_MAX;
  assertFrequencyBounds(fnName, bpmMin, bpmMax, 'bpmMin', 'bpmMax');
  return [bpmMin, bpmMax];
}
