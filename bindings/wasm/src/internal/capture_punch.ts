import { resolveSampleBound } from '../validation.js';
import { type Respelled, refuse } from './entry.js';
import { requireOneOf } from './one_of.js';

/**
 * Resolves one punch bound, given as exactly one of quarter notes, samples or seconds, into
 * timeline samples with the respelling its refusal needs; `sampleAtPpq` maps a quarter-note bound through the tempo map.
 */
export function capturePunchBound(
  ppq: number | undefined,
  sample: number | undefined,
  sec: number | undefined,
  names: readonly [string, string, string],
  sampleRate: number,
  sampleAtPpq: (ppq: number) => number,
): { value: number; respelled: Respelled } {
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
      'non-negative',
      sampleName,
    ) as { value: number; respelled: Respelled };
  }
  if (typeof ppq !== 'number' || !Number.isFinite(ppq) || ppq < 0) {
    throw refuse(`punch.${ppqName} must be finite and at least 0`);
  }
  return { value: sampleAtPpq(ppq), respelled: {} };
}
