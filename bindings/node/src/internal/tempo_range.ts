import type { TempoRangeOptions } from '../analysis.js';
import { assertFrequencyBounds } from '../validation.js';

/** The search range as given, checked as one pair; an omitted bound is left for the addon to fill with the library default. */
export function tempoRange(
  fnName: string,
  options: TempoRangeOptions,
): [number | undefined, number | undefined] {
  assertFrequencyBounds(fnName, options.bpmMin, options.bpmMax, 'bpmMin', 'bpmMax');
  return [options.bpmMin, options.bpmMax];
}
