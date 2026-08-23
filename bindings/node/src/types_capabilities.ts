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

/**
 * One parameter descriptor in the cross-surface capability catalog.
 *
 * `default` is the value the processor uses when the key is absent, read from
 * the config struct's own field initializer; it is null only for a param id
 * with no construction key.
 *
 * `min` and `max` are the range construction ACCEPTS, measured by handing
 * candidate values to the same code path a caller would use. They are a hard
 * constraint, not a recommended UI range — a value outside them is an error,
 * while an unvalidated control (most gains) reports null on both, meaning
 * "this catalog states no limit" rather than "unknown". Three properties to
 * plan for: a bound is measured with every other parameter at its default, so
 * two parameters that constrain each other each report the other's default; a
 * sample-rate-derived bound reflects the un-prepared processor and rises once
 * the insert is prepared at a higher rate; and an exclusive bound is reported
 * as its limit value, so a control requiring `> 0` reports `min` 0 and still
 * rejects 0.
 */
export interface CapabilityCatalogParameter {
  name: string;
  id: number;
  rtSafe: boolean;
  type: 'boolean' | 'number';
  min: number | null;
  max: number | null;
  default: boolean | number | null;
  unit: string | null;
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
  abi: Capabilities['abi'];
  processors: CapabilityCatalogProcessor[];
  presets: CapabilityCatalogPresets;
}
