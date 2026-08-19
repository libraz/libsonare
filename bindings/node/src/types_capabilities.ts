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

/** One parameter descriptor in the cross-surface capability catalog. */
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
