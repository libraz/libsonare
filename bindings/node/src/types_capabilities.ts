/** Host CPU, memory and parallelism facts, from {@link systemInfo}; an unreported fact is absent. */
export interface SystemInfo {
  readonly cpu: {
    /** Logical cores. */
    readonly logicalCores?: number;
    /** Physical cores. */
    readonly physicalCores?: number;
  };
  readonly memory: {
    /** Total memory in GiB. */
    readonly totalGb?: number;
    /** Memory available to the process in GiB. */
    readonly availableGb?: number;
  };
  readonly parallel: {
    /** Whether the library runs work in parallel. */
    readonly enabled: boolean;
    /** Worker count, at least 1. */
    readonly workers: number;
    /** Parallelism strategy name. */
    readonly strategy: string;
  };
}

/** Build and runtime capabilities reported by the loaded library, from {@link capabilities}. */
export interface Capabilities {
  /** libsonare semantic version. */
  readonly version: string;
  /** ABI versions used by project and realtime-engine data structures. */
  readonly abi: {
    /** Version of the project document's flat record layout. */
    readonly project: number;
    /** Version of the realtime engine's command-queue record layout. */
    readonly engine: number;
  };
  /** Native target identifier, such as `darwin-arm64` or `wasm32`. */
  readonly platform: string;
  /**
   * Optional feature families compiled into this library. A false flag means the family was
   * never built, which a caller cannot otherwise tell from a capability that is missing.
   */
  readonly features: {
    /** True when the mastering chain, its processors and presets are compiled in. */
    readonly mastering: boolean;
    /** True when the mixing library is compiled in. */
    readonly mixing: boolean;
    /**
     * True when the offline mixing assistant is compiled in. Separate from
     * `mixing` because it can be dropped on its own, and its entry points stay
     * exported either way — they report unsupported rather than disappearing,
     * so probing for the function tells a host nothing.
     */
    readonly mixingAssistant: boolean;
    /** True when the routing graph library is compiled in. */
    readonly graph: boolean;
    /** True when the creative realtime FX processors are compiled in. */
    readonly fx: boolean;
    /** True when the decoder reads the formats FFmpeg handles (M4A, AAC, FLAC, OGG, Opus); false means WAV and MP3 only. */
    readonly ffmpeg: boolean;
    /**
     * True when hosted instruments expose continuously automatable
     * parameters (`RealtimeEngine.resolveInstrumentAutomationId`).
     */
    readonly instrumentParamAutomation: boolean;
    /** True when the headless arrangement subsystem is compiled in. */
    readonly arrangement: boolean;
    /** True when `Project` documents can be built, edited and rendered in this build. */
    readonly project: boolean;
    /** True when the geometric room-acoustics reverb engine is compiled in. */
    readonly acousticSim: boolean;
    /** True when the playback renderer (upmix, binaural, speaker output) is compiled in. */
    readonly playback: boolean;
    /** True when the monophonic pitch editor is compiled in. */
    readonly pitchEditor: boolean;
    /** True when vocal editing sessions can be opened in this build. */
    readonly vocalEdit: boolean;
    /** True when the realtime voice changer is compiled in. */
    readonly voiceChanger: boolean;
    /** True when the composition-assist injection seam (control and offline only) is compiled in. */
    readonly assist: boolean;
  };
  /** Audio decoding available without, and through, FFmpeg. */
  readonly decode: {
    /** Formats the built-in decoder reads: `'wav'` and `'mp3'`. */
    readonly builtin: readonly string[];
    /** Formats read through FFmpeg; empty when `features.ffmpeg` is false. */
    readonly ffmpeg: readonly string[];
  };
  /** SIMD implementation selected for the target. */
  readonly simd: string;
  /** Hardware thread count reported by the runtime. */
  readonly hardwareConcurrency: number;
}

/** Closed vocabulary of units a numeric insert parameter declares; `none` is a declaration (a fraction, a selector index, a seed). */
export type MasteringInsertParamUnit =
  | 'dB'
  | 'dBFS'
  | 'dBTP'
  | 'LUFS'
  | 'Hz'
  | 'ms'
  | 's'
  | 'samples'
  | 'm'
  | 'cm'
  | 'mm'
  | 'deg'
  | 'percent'
  | 'degC'
  | 'V'
  | 'inPerSec'
  | 'dBPerOct'
  | 'semitones'
  | 'cents'
  | 'ratio'
  | 'bits'
  | 'count'
  | 'N'
  | 'midiNote'
  | 'bpm'
  | 'none';

/** Axis a control draws an insert parameter on. */
export type MasteringInsertParamScale = 'linear' | 'log';

/** How a parameter's value is ordered against a sibling's, read as `this <relation> factor * sibling`. */
export type MasteringInsertParamRelation = 'lt' | 'le' | 'gt' | 'ge';

/** A sibling whose live value bounds an insert parameter. */
export interface MasteringInsertParamDependency {
  /** Key of the sibling parameter that bounds this one. */
  readonly key: string;
  /** How this value is ordered against `factor * sibling`. */
  readonly relation: MasteringInsertParamRelation;
  /** Positive multiplier on the sibling's value; 1 for a plain ordering. */
  readonly factor: number;
}

/** One named value an enum- or closed-integer-typed insert param accepts. */
export interface MasteringInsertParamChoice {
  /** Display/identification name (lowerCamelCase); never accepted as input. */
  readonly name: string;
  /** The numeric wire value construction reads for this choice. */
  readonly value: number;
}

/**
 * A group of an insert's keys that exists only under a condition: an EQ band,
 * a multiband crossover band, or a dynamic sub-band inside one. A parameter
 * belongs to the slot its `slot` field names.
 */
export interface MasteringInsertSlot {
  /** Key prefix without the trailing dot, e.g. `midBand3` or `band1.dyn2`. */
  readonly name: string;
  /**
   * Enclosing slot, which has to exist for this one to; absent for a top-level
   * slot. A parent is always listed before the slots naming it.
   */
  readonly parent?: string;
  /**
   * `anyKey`: the slot exists once any one of its keys is supplied, so an EQ
   * band is built as soon as a host sends any of its keys. `always`: it exists
   * without any of its keys, subject to `minCrossoverCutoffs`.
   */
  readonly activation: 'anyKey' | 'always';
  /**
   * Crossover cutoffs (`cutoff<i>Hz` keys) the insert needs in effect for the
   * slot to exist; 0 when it needs none. With no cutoff key supplied, the
   * default split applies: the cutoffs that publish a default.
   */
  readonly minCrossoverCutoffs: number;
}

/**
 * One key a processor's construction reads, or one of its realtime automation
 * targets, as both {@link masteringInsertParamInfo} and
 * {@link capabilityCatalog} report it. Entries come in two runs: first the
 * processor's realtime automation targets in id order (`id` present), then,
 * sorted by name, every other construction key with no `id` and `rtSafe`
 * false.
 *
 * `min` and `max` are the range construction ACCEPTS, measured by handing
 * candidate values to the same code path a caller would use: a hard
 * constraint, not a UI range, and both absent for an unvalidated control.
 * A bound is measured with every other parameter at its default; a
 * sample-rate-derived bound reflects the un-prepared processor; an exclusive
 * bound is reported as its limit value and flagged by `minExclusive` /
 * `maxExclusive`.
 */
export interface MasteringInsertParamInfo {
  /** JSON-key parameter name, as used in scene insert params. */
  readonly name: string;
  /**
   * Integer param id for realtime automation lanes / MIDI-CC binding, absent
   * for a construction-only key with no automation target.
   */
  readonly id?: number;
  /** Whether the param can be changed live from the audio thread; false when `id` is absent. */
  readonly rtSafe: boolean;
  /**
   * The C++ type the processor's config builder reads the key as. `"enum"` is
   * sent as the number in its `choices` entry; `"string"` / `"array"` (an
   * embedded impulse response, a per-band list) is construction-only and
   * reports no `min`, `max`, `default` or `choices`.
   */
  readonly type: 'boolean' | 'number' | 'enum' | 'string' | 'array';
  /** Smallest value construction accepts; absent when the catalog states no limit or `choices` is present. */
  readonly min?: number;
  /** Largest value construction accepts; absent when the catalog states no limit or `choices` is present. */
  readonly max?: number;
  /** Whether `min` itself is rejected (a `> min` constraint); false when `min` is absent. */
  readonly minExclusive: boolean;
  /** Whether `max` itself is rejected (a `< max` constraint); false when `max` is absent. */
  readonly maxExclusive: boolean;
  /**
   * `"nyquist"` when the ceiling follows the processing rate: the effective
   * ceiling is then the lower of `max` (measured at the catalog's probe rate)
   * and the host's Nyquist frequency, both exclusive. Absent otherwise.
   */
  readonly maxRelativeTo?: 'nyquist';
  /**
   * Value the processor uses when the key is absent — the config struct's own
   * field initializer, an enum as its number. Absent for a param id with no
   * construction key, a `"string"` / `"array"` key, or a construction key with
   * no fallback.
   */
  readonly default?: boolean | number;
  /** Declared unit of a number; absent for any other type. */
  readonly unit?: MasteringInsertParamUnit;
  /** Lowest value a control should draw, inside `min` / `max`; absent when the accepted range is also the display range. */
  readonly uiMin?: number;
  /** Highest value a control should draw, inside `min` / `max`; absent when the accepted range is also the display range. */
  readonly uiMax?: number;
  /** Axis a control draws the value on. */
  readonly scale: MasteringInsertParamScale;
  /**
   * The closed set of values construction accepts, in value order; absent
   * when the accepted values are not a closed set. Present only for `"enum"`
   * (every declared enumerator construction accepts) or a `"number"` key
   * whose accepted integers have holes; `min` / `max` are then both absent.
   */
  readonly choices?: readonly MasteringInsertParamChoice[];
  /** The {@link MasteringInsertSlot} this key belongs to; absent for a key that always exists. */
  readonly slot?: string;
  /**
   * Siblings whose live value bounds this key, each read as `this <relation> factor * sibling`;
   * empty for an independent key. A bound that only restates this dependency at the sibling's default is left absent, so `min` and `max` are limits of the key's own.
   */
  readonly dependsOn: readonly MasteringInsertParamDependency[];
  /**
   * Siblings, other than `dependsOn` entries, whose value moves this key's accepted range in a way
   * `dependsOn` cannot state (a receiver spacing against the room geometry); `min` / `max` are measured with them at their defaults. Empty for an independent key.
   */
  readonly coupledWith: readonly string[];
}

/** Latency and tail of one insert built from given params and prepared at a given sample rate. */
export interface MasteringInsertTiming {
  /** Latency in samples at the queried sample rate. */
  readonly latencySamples: number;
  /** Audible decay tail in samples at the queried sample rate. */
  readonly tailSamples: number;
}

/**
 * How a processor handles a buffer with more than two channels (a surround
 * bed). `multichannel` processes every plane in one call; `stereoPairOnly`
 * operates on the front L/R pair and passes any surround planes through dry.
 * `perChannel` / `passthrough` are reserved and unused by the current catalog.
 */
export type MasteringChannelPolicy =
  | 'multichannel'
  | 'stereoPairOnly'
  | 'perChannel'
  | 'passthrough';

/** Coarse algorithmic work estimate for a realtime insert; not a benchmark. */
export type MasteringRealtimeCost = 'low' | 'moderate' | 'high';

/**
 * Catalog grouping for a processor picker, derived from the id's prefix
 * (`eq.*` -> `eq`, `match.*` -> `reference`); anything unprefixed is `other`.
 */
export type MasteringProcessorCategory =
  | 'dynamics'
  | 'effects'
  | 'eq'
  | 'final'
  | 'maximizer'
  | 'multiband'
  | 'other'
  | 'reference'
  | 'repair'
  | 'saturation'
  | 'spectral'
  | 'stereo'
  | 'utility'
  | 'voice';

/**
 * One processor's role in the catalog, as both
 * {@link masteringProcessorCatalog} and {@link capabilityCatalog} report it.
 */
export interface MasteringProcessorCatalogEntry {
  /** Processor id (the name used for scene inserts / named processors). */
  readonly id: string;
  /**
   * Primary classification, by precedence pair > realtime > offline: `pair` for
   * two-input `match.*` processors, `realtime` for ids that build as a realtime
   * scene insert, `offline` for whole-file-only processors.
   */
  readonly kind: 'realtime' | 'offline' | 'pair';
  /** True exactly for ids that always succeed as a realtime scene insert. */
  readonly realtimeInsertable: boolean;
  /** True for processors with no mono implementation (stereo-only). */
  readonly stereoOnly: boolean;
  /**
   * Reported latency for the default 48 kHz / 512-sample probe configuration.
   * Zero for offline processors; configuration-dependent values are estimates.
   */
  readonly latencySamples: number;
  /**
   * Audible decay length for the same default prepared probe. Zero for
   * offline, dry-only, and no-tail processors.
   */
  readonly tailSamples: number;
  /** Coarse realtime work estimate; absent when the processor is not an insert. */
  readonly realtimeCost?: MasteringRealtimeCost;
  /** How the mixer wraps the processor on a >2-channel (surround) bus insert. */
  readonly channelPolicy: MasteringChannelPolicy;
  /** Grouping for a processor picker. */
  readonly category: MasteringProcessorCategory;
  /**
   * Whether the processor has a configuration whose output at a sample depends only on the
   * input up to it, plus its reported latency. False for `repair.declick`, `repair.declip` and
   * `repair.trimSilence`; true for the other repair stages and every insert.
   */
  readonly causal: boolean;
  /**
   * The processor's construction parameters: for an insert the list
   * {@link masteringInsertParamInfo} returns, for a `repair.*` stage the bounds its own
   * configuration validation enforces (no `id`, never `rtSafe`). Empty for any other offline entry.
   */
  readonly params: readonly MasteringInsertParamInfo[];
  /**
   * The insert's conditional key groups in declaration order, named by each
   * parameter's `slot`. Empty for entries that are not realtime-insertable.
   */
  readonly slots: readonly MasteringInsertSlot[];
}

/**
 * One built-in mastering preset's loudness-stage identity. The restoration
 * kind's three loudness values are absent: its repair stages leave level alone,
 * so it carries no loudness target, ceiling or limiter allowance.
 */
export interface MasteringPresetCatalogEntry {
  /** Preset name. */
  readonly name: string;
  /** Preset family: `'mastering'` or `'restoration'`. */
  readonly kind: 'mastering' | 'restoration';
  /** Integrated loudness target in LUFS; absent for a restoration preset. */
  readonly targetLufs?: number;
  /** True-peak ceiling in dBTP; absent for a restoration preset. */
  readonly truePeakCeilingDbtp?: number;
  /** Largest gain reduction the limiter may apply, in dB; absent for a restoration preset. */
  readonly maxLimiterGainReductionDb?: number;
}

/** Built-in preset names grouped by feature family. */
export interface CapabilityCatalogPresets {
  /** Mastering chain presets. */
  readonly mastering: readonly string[];
  /** Synth presets. */
  readonly synth: readonly string[];
  /** Mixing scene presets. */
  readonly mixingScene: readonly string[];
  /** Realtime voice changer presets. */
  readonly voiceChanger: readonly string[];
  /** Playback room presets. */
  readonly playbackRoom: readonly string[];
}

/** One flat mastering chain stage: `<prefix>.<leaf>` keys are `processor`'s parameters. */
export interface CapabilityCatalogChainStage {
  /** Prefix shared by the stage's keys, such as `'repair.declick'`. */
  readonly prefix: string;
  /** Catalog processor id; absent when every key of the stage is chain-only. */
  readonly processor?: string;
  /** Boolean key that switches the stage on, such as `'repair.declick.enabled'`. */
  readonly enabledKey: string;
}

/**
 * An indexed chain list. `<key>.<i>` is the chain parameter `key` when
 * `processor` is absent; otherwise `<key>.<i>.<field>` is that processor's `field`.
 */
export interface CapabilityCatalogChainList {
  /** Key the item index is appended to. */
  readonly key: string;
  /** Catalog processor id whose parameters each item carries; absent for a list of numbers. */
  readonly processor?: string;
  /** Fewest items the chain accepts. */
  readonly minItems: number;
  /** Most items the chain accepts. */
  readonly maxItems: number;
}

/** How each flat mastering chain key is described. */
export interface CapabilityCatalogChain {
  /** One entry per chain stage. */
  readonly stages: readonly CapabilityCatalogChainStage[];
  /** Chain keys that take an item index. */
  readonly lists: readonly CapabilityCatalogChainList[];
  /** Keys no processor describes, with bounds measured through the chain's own validator. */
  readonly params: readonly MasteringInsertParamInfo[];
}

/** Complete runtime catalog exposed by {@link capabilityCatalog}. */
export interface CapabilityCatalog {
  /** libsonare semantic version, as {@link Capabilities} reports it. */
  readonly version: string;
  /** ABI versions, as {@link Capabilities} reports them. */
  readonly abi: Capabilities['abi'];
  /** Every mastering processor in this build, realtime inserts and offline stages alike. */
  readonly processors: readonly MasteringProcessorCatalogEntry[];
  /** How the flat mastering chain keys map onto processors. */
  readonly chain: CapabilityCatalogChain;
  /** Built-in preset names by feature family. */
  readonly presets: CapabilityCatalogPresets;
  /** Loudness identity of each built-in mastering preset. */
  readonly masteringPresets: readonly MasteringPresetCatalogEntry[];
}
