import { refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';

/** SynthPatch fields that hold a closed-set name. */
const PATCH_TOKEN_FIELDS = [
  'engineMode',
  'waveform',
  'filterModel',
  'filterOutput',
  'lfoShape',
  'body',
  'retrigger',
  'sampleLoop',
  'sampleKeyTrack',
] as const;

function requireName(path: string, value: unknown): void {
  if (value !== undefined && value !== null && typeof value !== 'string') {
    throw refuseType(`${path} must be a name`);
  }
}

/**
 * Refuse a closed-set field of a synth patch given as anything but a name, so an
 * ordinal never reaches the native resolver, which still reads one. An unknown
 * name is left to that resolver, which refuses it as a RangeError.
 */
export function refuseSynthPatchOrdinals(patch: unknown): void {
  if (typeof patch !== 'object' || patch === null) {
    return;
  }
  const record = patch as Record<string, unknown>;
  for (const field of PATCH_TOKEN_FIELDS) {
    requireName(field, record[field]);
  }
  const routings = record.modRoutings;
  if (!Array.isArray(routings)) {
    return;
  }
  requireListBudget(routings, 'modRoutings');
  routings.forEach((routing, index) => {
    if (typeof routing === 'object' && routing !== null) {
      const entry = routing as Record<string, unknown>;
      requireName(`modRoutings[${index}].source`, entry.source);
      requireName(`modRoutings[${index}].destination`, entry.destination);
    }
  });
}

/** Refuse a sample descriptor's loop mode given as anything but a name. */
export function refuseSampleDescOrdinals(desc: unknown): void {
  if (typeof desc === 'object' && desc !== null) {
    requireName('loopMode', (desc as Record<string, unknown>).loopMode);
  }
}

/** Refuse a built-in synth config's waveform given as anything but a name. */
export function refuseBuiltinSynthOrdinals(config: unknown): void {
  if (typeof config === 'object' && config !== null) {
    requireName('waveform', (config as Record<string, unknown>).waveform);
  }
}
