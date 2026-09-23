export * from './public_types_acoustic';
export * from './public_types_mastering';
export * from './public_types_mixing';
export * from './public_types_music';
export * from './public_types_realtime';
export * from './public_types_repair';
export * from './public_types_spectral';

/** Runtime capabilities reported by the loaded libsonare build. */
export interface SonareCapabilities {
  version: string;
  abi: {
    project: number;
    engine: number;
  };
  platform: string;
  features: {
    mastering: boolean;
    mixing: boolean;
    /**
     * True when the offline mixing assistant is compiled in. Separate from
     * `mixing` because it can be dropped on its own — the analysis-only bundle
     * has it off — and its entry points stay registered either way, throwing
     * rather than disappearing, so probing for the function tells a host
     * nothing.
     */
    mixingAssistant: boolean;
    fx: boolean;
    ffmpeg: boolean;
    /**
     * True when hosted instruments expose continuously automatable
     * parameters (`RealtimeEngine.resolveInstrumentAutomationId`).
     */
    instrumentParamAutomation: boolean;
    /**
     * The four below name the remaining build options that change which
     * commands and entry points a binary answers. Without them a caller can
     * observe that a capability is missing but not that it was never built.
     */
    arrangement: boolean;
    acousticSim: boolean;
    pitchEditor: boolean;
    voiceChanger: boolean;
  };
  decode: {
    builtin: string[];
    ffmpeg: string[];
  };
  simd: string;
  hardwareConcurrency: number;
}

/** One named value an enum- or closed-integer-typed insert param accepts. */
export interface MasteringInsertParamChoice {
  /** Display/identification name (lowerCamelCase); never accepted as input. */
  name: string;
  /** The numeric wire value construction reads for this choice. */
  value: number;
}

/**
 * One parameter descriptor in the cross-surface capability catalog, covering
 * every key a processor's construction reads. Entries come in two runs: first
 * the processor's realtime automation targets in id order (`id` non-null),
 * then, sorted by name, every other construction key with `id` null and
 * `rtSafe` false.
 *
 * `type` is `"enum"` when `choices` names every declared enumerator
 * construction accepts (sent as the number in its `choices` entry), or
 * `"string"` / `"array"` for a construction-only key (an embedded impulse
 * response, a per-band list) that reports null for `min`, `max`, `default`
 * and `choices`.
 *
 * `default` is the value the processor uses when the key is absent, read from
 * the config struct's own field initializer, an enum as its number; it is
 * null for a param id with no construction key, a `"string"` / `"array"` key,
 * or a construction key with no fallback.
 *
 * `min` and `max` are the range construction ACCEPTS, measured by handing
 * candidate values to the same code path a caller would use. They are a hard
 * constraint, not a recommended UI range — a value outside them is an error,
 * while an unvalidated control (most gains) reports null on both, meaning
 * "this catalog states no limit" rather than "unknown". Non-null only when
 * `choices` is null: `choices` non-null names the closed accepted set instead
 * (every declared enumerator, or a `"number"` key whose accepted integers
 * have holes), and then both bounds are null. Three properties to plan for: a
 * bound is measured with every other parameter at its default, so two
 * parameters that constrain each other each report the other's default; a
 * sample-rate-derived bound reflects the un-prepared processor and rises once
 * the insert is prepared at a higher rate; and an exclusive bound is reported
 * as its limit value, so a control requiring `> 0` reports `min` 0 and still
 * rejects 0.
 */
export interface CapabilityCatalogParameter {
  name: string;
  id: number | null;
  rtSafe: boolean;
  type: 'boolean' | 'number' | 'enum' | 'string' | 'array';
  min: number | null;
  max: number | null;
  default: boolean | number | null;
  unit: string | null;
  choices: MasteringInsertParamChoice[] | null;
}

/** One named mastering processor and its host-facing capabilities. */
export interface CapabilityCatalogProcessor {
  id: string;
  kind: 'realtime' | 'offline' | 'pair';
  realtimeInsertable: boolean;
  stereoOnly: boolean;
  latencySamples: number;
  tailSamples: number;
  /** Coarse realtime work estimate, or null when the processor is not an insert. */
  realtimeCost: 'low' | 'moderate' | 'high' | null;
  channelPolicy: 'multichannel' | 'stereoPairOnly' | 'perChannel' | 'passthrough';
  category: string;
  params: CapabilityCatalogParameter[];
}

/** Built-in preset names grouped by feature family. */
export interface CapabilityCatalogPresets {
  mastering: string[];
  synth: string[];
  mixingScene: string[];
  voiceChanger: string[];
}

/** Complete runtime catalog exposed by {@link capabilityCatalog}. */
export interface CapabilityCatalog {
  version: string;
  abi: SonareCapabilities['abi'];
  processors: CapabilityCatalogProcessor[];
  presets: CapabilityCatalogPresets;
}

/** Synchronous progress callback for offline operations. Its return value is ignored. */
export type ProgressCallback = (progress: number, stage: string) => void;
