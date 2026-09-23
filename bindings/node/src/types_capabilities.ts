/** Build and runtime capabilities reported by the loaded native library. */
export interface Capabilities {
  /** libsonare semantic version. */
  version: string;
  /** ABI versions used by project and realtime-engine data structures. */
  abi: {
    project: number;
    engine: number;
  };
  /** Native target identifier, such as `darwin-arm64` or `wasm32`. */
  platform: string;
  /** Optional feature families compiled into this library. */
  features: {
    mastering: boolean;
    mixing: boolean;
    /**
     * True when the offline mixing assistant is compiled in. Separate from
     * `mixing` because it can be dropped on its own, and its entry points stay
     * exported either way — they report unsupported rather than disappearing,
     * so probing for the function tells a host nothing.
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
  /** Audio decoding available without, and through, FFmpeg. */
  decode: {
    builtin: string[];
    ffmpeg: string[];
  };
  /** SIMD implementation selected for the target. */
  simd: string;
  /** Hardware thread count reported by the native runtime. */
  hardwareConcurrency: number;
}

/** One named value of an enum parameter, or of a whole-number parameter whose accepted set has holes. */
export interface MasteringInsertParamChoice {
  name: string;
  value: number;
}

/**
 * One parameter descriptor in the cross-surface capability catalog.
 *
 * Entries come in two runs: automation targets (`id` the integer used by
 * realtime automation, sorted by id), then construction-only keys (`id` null,
 * `rtSafe` false, sorted by name).
 *
 * `type` is the C++ type the processor's config builder reads the key as. An
 * `"enum"` value is sent as the number in its `choices` entry. A `"string"` or
 * `"array"` key (an embedded impulse response, a per-band list) is
 * construction-only and reports null for `default`, `min`, `max` and
 * `choices`.
 *
 * `default` is the value the processor uses when the key is absent, read from
 * the config struct's own field initializer; it is null for an automation
 * target with no construction key and for a key construction reads with no
 * fallback.
 *
 * `choices` is null unless the accepted values are a closed set, listed in
 * value order; when it is non-null, it is the only accepted set and `min` /
 * `max` are both null. Otherwise `min` and `max` are the range construction
 * ACCEPTS, measured by handing candidate values to the same code path a
 * caller would use. They are a hard constraint, not a recommended UI range —
 * a value outside them is an error, while an unvalidated control (most gains)
 * reports null on both, meaning "this catalog states no limit" rather than
 * "unknown". Three properties to plan for: a bound is measured with every
 * other parameter at its default, so two parameters that constrain each other
 * each report the other's default; a sample-rate-derived bound reflects the
 * un-prepared processor and rises once the insert is prepared at a higher
 * rate; and an exclusive bound is reported as its limit value, so a control
 * requiring `> 0` reports `min` 0 and still rejects 0.
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

/**
 * One built-in mastering preset's loudness-stage identity, in `preset_names()`
 * order.
 *
 * `kind: 'restoration'` presets enable repair stages only and leave level
 * alone, so all three numeric fields are null; every other preset carries the
 * loudness target its chain builds toward.
 */
export interface MasteringPresetInfo {
  name: string;
  kind: 'mastering' | 'restoration';
  targetLufs: number | null;
  truePeakCeilingDb: number | null;
  maxLimiterGainReductionDb: number | null;
}

/** Complete runtime catalog exposed by {@link capabilityCatalog}. */
export interface CapabilityCatalog {
  version: string;
  abi: Capabilities['abi'];
  processors: CapabilityCatalogProcessor[];
  presets: CapabilityCatalogPresets;
  masteringPresets: MasteringPresetInfo[];
}
