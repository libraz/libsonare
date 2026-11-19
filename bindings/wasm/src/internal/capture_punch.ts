import { resolveSampleBound } from '../validation.js';
import { refuse } from './entry.js';
import { requireOneOf } from './one_of.js';

/**
 * Resolves one punch bound, given as exactly one of quarter notes, samples or seconds, into
 * timeline samples; `sampleAtPpq` maps a quarter-note bound through the tempo map.
 */
export function capturePunchBound(
  ppq: number | undefined,
  sample: number | undefined,
  sec: number | undefined,
  names: readonly [string, string, string],
  sampleRate: number,
  sampleAtPpq: (ppq: number) => number,
): number {
  const [ppqName, sampleName, secName] = names;
  const given = requireOneOf(
    names.map((name) => `punch.${name}`),
    [ppq, sample, sec].map((value) => value !== undefined && value !== null),
  );
  if (given !== 0) {
    return resolveSampleBound(
      sample,
      sec,
      sampleRate,
      `punch.${sampleName}`,
      `punch.${secName}`,
    ) as number;
  }
  if (typeof ppq !== 'number' || !Number.isFinite(ppq) || ppq < 0) {
    throw refuse(`punch.${ppqName} must be a non-negative number`);
  }
  return sampleAtPpq(ppq);
}
