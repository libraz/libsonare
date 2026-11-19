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

function requireName(fnName: string, path: string, value: unknown): void {
  if (value !== undefined && value !== null && typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${path} must be a name`);
  }
}

/**
 * Refuse a closed-set field of a synth patch given as anything but a name, so an
 * ordinal never reaches the native resolver, which still reads one. An unknown
 * name is left to that resolver, which refuses it as a RangeError.
 */
export function refuseSynthPatchOrdinals(fnName: string, patch: unknown): void {
  if (typeof patch !== 'object' || patch === null) {
    return;
  }
  const record = patch as Record<string, unknown>;
  for (const field of PATCH_TOKEN_FIELDS) {
    requireName(fnName, field, record[field]);
  }
  const routings = record.modRoutings;
  if (!Array.isArray(routings)) {
    return;
  }
  routings.forEach((routing, index) => {
    if (typeof routing === 'object' && routing !== null) {
      const entry = routing as Record<string, unknown>;
      requireName(fnName, `modRoutings[${index}].source`, entry.source);
      requireName(fnName, `modRoutings[${index}].destination`, entry.destination);
    }
  });
}

/** Refuse a sample descriptor's loop mode given as anything but a name. */
export function refuseSampleDescOrdinals(fnName: string, desc: unknown): void {
  if (typeof desc === 'object' && desc !== null) {
    requireName(fnName, 'loopMode', (desc as Record<string, unknown>).loopMode);
  }
}

/** Refuse a built-in synth config's waveform given as anything but a name. */
export function refuseBuiltinSynthOrdinals(fnName: string, config: unknown): void {
  if (typeof config === 'object' && config !== null) {
    requireName(fnName, 'waveform', (config as Record<string, unknown>).waveform);
  }
}

/** Refuse an explicit `polyphony: 0` on a built-in synth config: the default voice count is spelt by leaving it out. */
export function refuseBuiltinPolyphonyZero(fnName: string, config: unknown): void {
  if (typeof config === 'object' && config !== null) {
    if ((config as Record<string, unknown>).polyphony === 0) {
      throw new RangeError(`${fnName}: polyphony must not be 0 (omit it for the default)`);
    }
  }
}
