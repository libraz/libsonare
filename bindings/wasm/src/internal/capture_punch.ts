import { resolveSampleBound } from '../validation.js';
import { type Respelled, refuse } from './entry.js';
import { requireOneOf } from './one_of.js';
import { isAbsent } from './value_guards.js';

/**
 * Resolves one punch bound, given as exactly one of quarter notes, samples or seconds, into
 * timeline samples with the respelling its refusal needs; `sampleAtPpq` maps a quarter-note bound through the tempo map.
 * The respelling is keyed on the name `setCaptureTiming` refuses the bound under (`punchStartSample`).
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
    [ppq, sample, sec].map((value) => !isAbsent(value)),
  );
  if (given !== 0) {
    return resolveSampleBound(
      sample,
      sec,
      sampleRate,
      `punch.${sampleName}`,
      `punch.${secName}`,
      'non-negative',
      `punch${sampleName.charAt(0).toUpperCase()}${sampleName.slice(1)}`,
    ) as { value: number; respelled: Respelled };
  }
  if (typeof ppq !== 'number' || !Number.isFinite(ppq) || ppq < 0) {
    throw refuse(`punch.${ppqName} must be finite and at least 0`);
  }
  const subject = `punch${sampleName.charAt(0).toUpperCase()}${sampleName.slice(1)}`;
  return {
    value: sampleAtPpq(ppq),
    respelled: { [subject]: { name: `punch.${ppqName}`, scale: 1 } },
  };
}
