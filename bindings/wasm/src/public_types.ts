export * from './public_types_acoustic.js';
export type { SidechainCheck, SidechainRefusal, UmpWords } from './public_types_engine.js';
export * from './public_types_mastering.js';
export * from './public_types_mixing.js';
export * from './public_types_music.js';
export * from './public_types_playback.js';
export * from './public_types_realtime.js';
export * from './public_types_repair.js';
export * from './public_types_spectral.js';

/** Build and runtime capabilities reported by the loaded library, from {@link capabilities}. */
export interface Capabilities {
  /** libsonare semantic version. */
  readonly version: string;
  /** ABI versions used by project and realtime-engine data structures. */
  readonly abi: {
    readonly project: number;
    readonly engine: number;
  };
  /** Native target identifier, such as `darwin-arm64` or `wasm32`. */
  readonly platform: string;
  /** Optional feature families compiled into this library. */
  readonly features: {
    readonly mastering: boolean;
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
    readonly fx: boolean;
    /** True when the decoder reads the formats FFmpeg handles (M4A, AAC, FLAC, OGG, Opus); false means WAV and MP3 only. */
    readonly ffmpeg: boolean;
    /**
     * True when hosted instruments expose continuously automatable
     * parameters (`RealtimeEngine.resolveInstrumentAutomationId`).
     */
    readonly instrumentParamAutomation: boolean;
    /**
     * The flags below name the remaining build options that change which
     * commands and entry points a binary answers. Without them a caller can
     * observe that a capability is missing but not that it was never built.
     */
    readonly arrangement: boolean;
    /** True when `Project` documents can be built, edited and rendered in this build. */
    readonly project: boolean;
    readonly acousticSim: boolean;
    /** True when the playback renderer (upmix, binaural, speaker output) is compiled in. */
    readonly playback: boolean;
    readonly pitchEditor: boolean;
    /** True when vocal editing sessions can be opened in this build. */
    readonly vocalEdit: boolean;
    readonly voiceChanger: boolean;
    /** True when the composition-assist injection seam (control and offline only) is compiled in. */
    readonly assist: boolean;
  };
  /** Audio decoding available without, and through, FFmpeg. */
  readonly decode: {
    readonly builtin: readonly string[];
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
  | 'none';

/** Axis a control draws an insert parameter on. */
export type MasteringInsertParamScale = 'linear' | 'log';

/** How a parameter's value is ordered against a sibling's, read as `this <relation> factor * sibling`. */
export type MasteringInsertParamRelation = 'lt' | 'le' | 'gt' | 'ge';

/** A sibling whose live value bounds an insert parameter. */
export interface MasteringInsertParamDependency {
  readonly key: string;
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
   * Enclosing slot, which has to exist for this one to; null for a top-level
   * slot. A parent is always listed before the slots naming it.
   */
  readonly parent: string | null;
  /**
   * `anyKey`: the slot exists once any one of its keys is supplied, so an EQ
   * band is built as soon as a host sends any of its keys. `always`: it exists
   * without any of its keys, subject to `minCrossoverCutoffs`.
   */
  readonly activation: 'anyKey' | 'always';
  /**
   * Crossover cutoffs (`cutoff<i>Hz` keys) the insert needs in effect for the
   * slot to exist; 0 when it needs none. With no cutoff key supplied, the
   * default split applies: the cutoffs that publish a non-null default.
   */
  readonly minCrossoverCutoffs: number;
}

/**
 * One key a processor's construction reads, or one of its realtime automation
 * targets, as both {@link masteringInsertParamInfo} and
 * {@link capabilityCatalog} report it. Entries come in two runs: first the
 * processor's realtime automation targets in id order (`id` non-null), then,
 * sorted by name, every other construction key with `id` null and `rtSafe`
 * false.
 *
 * `min` and `max` are the range construction ACCEPTS, measured by handing
 * candidate values to the same code path a caller would use: a hard
 * constraint, not a UI range, and null on both for an unvalidated control.
 * A bound is measured with every other parameter at its default; a
 * sample-rate-derived bound reflects the un-prepared processor; an exclusive
 * bound is reported as its limit value and flagged by `minExclusive` /
 * `maxExclusive`.
 */
export interface MasteringInsertParamInfo {
  /** JSON-key parameter name, as used in scene insert params. */
  readonly name: string;
  /**
   * Integer param id for realtime automation lanes / MIDI-CC binding, or null
   * for a construction-only key with no automation target.
   */
  readonly id: number | null;
  /** Whether the param can be changed live from the audio thread; false when `id` is null. */
  readonly rtSafe: boolean;
  /**
   * The C++ type the processor's config builder reads the key as. `"enum"` is
   * sent as the number in its `choices` entry; `"string"` / `"array"` (an
   * embedded impulse response, a per-band list) is construction-only and
   * reports null for `min`, `max`, `default` and `choices`.
   */
  readonly type: 'boolean' | 'number' | 'enum' | 'string' | 'array';
  /** Smallest value construction accepts, or null when the catalog states no limit or `choices` is non-null. */
  readonly min: number | null;
  /** Largest value construction accepts, or null when the catalog states no limit or `choices` is non-null. */
  readonly max: number | null;
  /** Whether `min` itself is rejected (a `> min` constraint); false when `min` is null. */
  readonly minExclusive: boolean;
  /** Whether `max` itself is rejected (a `< max` constraint); false when `max` is null. */
  readonly maxExclusive: boolean;
  /**
   * `"nyquist"` when the ceiling follows the processing rate: the effective
   * ceiling is then the lower of `max` (measured at the catalog's probe rate)
   * and the host's Nyquist frequency, both exclusive. Null otherwise.
   */
  readonly maxRelativeTo: 'nyquist' | null;
  /**
   * Value the processor uses when the key is absent — the config struct's own
   * field initializer, an enum as its number. Null for a param id with no
   * construction key, a `"string"` / `"array"` key, or a construction key with
   * no fallback.
   */
  readonly default: boolean | number | null;
  /** Declared unit of a number; null for any other type. */
  readonly unit: MasteringInsertParamUnit | null;
  /** Lowest value a control should draw, inside `min` / `max`; null when the accepted range is also the display range. */
  readonly uiMin: number | null;
  /** Highest value a control should draw, inside `min` / `max`; null when the accepted range is also the display range. */
  readonly uiMax: number | null;
  /** Axis a control draws the value on. */
  readonly scale: MasteringInsertParamScale;
  /**
   * The closed set of values construction accepts, in value order, or null
   * when the accepted values are not a closed set. Non-null only for `"enum"`
   * (every declared enumerator construction accepts) or a `"number"` key
   * whose accepted integers have holes; `min` / `max` are then both null.
   */
  readonly choices: readonly MasteringInsertParamChoice[] | null;
  /** The {@link MasteringInsertSlot} this key belongs to, or null for a key that always exists. */
  readonly slot: string | null;
  /**
   * Siblings whose live value bounds this key, each read as `this <relation> factor * sibling`;
   * empty for an independent key. A bound that only restates this dependency at the sibling's default is left null, so `min` and `max` are limits of the key's own.
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
  /** Coarse realtime work estimate, or null when the processor is not an insert. */
  readonly realtimeCost: MasteringRealtimeCost | null;
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
 * kind's three loudness values are null: its repair stages leave level alone,
 * so it carries no loudness target, ceiling or limiter allowance.
 */
export interface MasteringPresetCatalogEntry {
  readonly name: string;
  readonly kind: 'mastering' | 'restoration';
  readonly targetLufs: number | null;
  readonly truePeakCeilingDb: number | null;
  readonly maxLimiterGainReductionDb: number | null;
}

/** Built-in preset names grouped by feature family. */
export interface CapabilityCatalogPresets {
  readonly mastering: readonly string[];
  readonly synth: readonly string[];
  readonly mixingScene: readonly string[];
  readonly voiceChanger: readonly string[];
  readonly playbackRoom: readonly string[];
}

/** One flat mastering chain stage: `<prefix>.<leaf>` keys are `processor`'s parameters. */
export interface CapabilityCatalogChainStage {
  readonly prefix: string;
  /** Catalog processor id, or null when every key of the stage is chain-only. */
  readonly processor: string | null;
  readonly enabledKey: string;
}

/**
 * An indexed chain list. `<key>.<i>` is the chain parameter `key` when
 * `processor` is null; otherwise `<key>.<i>.<field>` is that processor's `field`.
 */
export interface CapabilityCatalogChainList {
  readonly key: string;
  readonly processor: string | null;
  readonly minItems: number;
  readonly maxItems: number;
}

/** The other spellings of a canonical chain key; `<i>` stands for a list index. */
export interface CapabilityCatalogChainAlias {
  readonly key: string;
  readonly aliases: readonly string[];
}

/** How each flat mastering chain key is described. */
export interface CapabilityCatalogChain {
  readonly stages: readonly CapabilityCatalogChainStage[];
  readonly lists: readonly CapabilityCatalogChainList[];
  /** Keys no processor describes, with bounds measured through the chain's own validator. */
  readonly params: readonly MasteringInsertParamInfo[];
  readonly aliases: readonly CapabilityCatalogChainAlias[];
}

/** Complete runtime catalog exposed by {@link capabilityCatalog}. */
export interface CapabilityCatalog {
  readonly version: string;
  readonly abi: Capabilities['abi'];
  readonly processors: readonly MasteringProcessorCatalogEntry[];
  readonly chain: CapabilityCatalogChain;
  readonly presets: CapabilityCatalogPresets;
  readonly masteringPresets: readonly MasteringPresetCatalogEntry[];
}

/**
 * Row-major 2-D matrix: `data[r * cols + c]` holds row `r`, column `c`. Each
 * function returning one states what its rows and columns are.
 */
export interface Matrix2D {
  /** Row-major `[rows x cols]` data buffer. */
  data: Float32Array;
  /** Number of rows. */
  rows: number;
  /** Number of columns. */
  cols: number;
}

declare const sonareModuleHandleBrand: unique symbol;

/**
 * Opaque handle to a loaded WASM module, as a custom `moduleFactory` passed to
 * `init()` resolves it. Its members are not part of the public interface.
 */
export type SonareModuleHandle = { readonly [sonareModuleHandleBrand]: 'SonareModuleHandle' };

/** Synchronous progress callback for offline operations. Its return value is ignored. */
export type ProgressCallback = (progress: number, stage: string) => void;
