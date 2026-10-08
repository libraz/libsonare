import { addon } from './native.js';
import type {
  MasteringInsertParamChoice,
  MasteringInsertParamDependency,
  MasteringInsertParamScale,
  MasteringInsertParamUnit,
  MasteringInsertSlot,
  MasteringPreset,
  PairAnalysis,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
} from './types.js';
import { assertFiniteScalar } from './validation.js';

export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
}

/**
 * A built-in preset's chain configuration as a flat `{ "module.param": value }`
 * map -- the same key space {@link masteringAssistantSuggestChain} returns, and
 * a preset applies unchanged when passed straight back as {@link masterAudio}'s
 * `overrides`.
 *
 * @throws Error for a name not in {@link masteringPresetNames}.
 */
export function masteringPresetParams(preset: MasteringPreset): Record<string, number | boolean> {
  return addon.masteringPresetParams(preset);
}

/**
 * Delivery targets the mastering assistant accepts as `targetPlatform`.
 *
 * Read from the library rather than from a list kept here, so a target added in
 * the core is discoverable without a binding change.
 */
export function masteringPlatformNames(): string[] {
  return addon.masteringPlatformNames();
}

export function masteringProcessorNames(): SoloProcessor[] {
  return addon.masteringProcessorNames();
}

export function masteringPairProcessorNames(): PairProcessor[] {
  return addon.masteringPairProcessorNames();
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  return addon.masteringPairAnalysisNames();
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return addon.masteringStereoAnalysisNames();
}

/**
 * Returns the channel-strip insert / FX processor names that mixing scene
 * inserts can build (includes the creative effects.* reverbs / modulation /
 * delay when FX support is compiled in). Use these to discover valid insert
 * names instead of hardcoding magic strings.
 */
export function masteringInsertNames(): string[] {
  return addon.masteringInsertNames();
}

/**
 * Returns the camelCase parameter names a given insert / FX processor reads, for
 * tooling/validation. Any key NOT in this list is silently ignored by the
 * processor (and would be reported via {@link Mixer.sceneWarnings} when a scene
 * carrying it is loaded). Band/sub-band processors enumerate their indexed
 * `band{i}.<field>` keys. Returns an empty array for an unknown name (or one
 * whose insert needs an unavailable build feature, e.g. FX).
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 */
export function masteringInsertParamNames(name: string): string[] {
  return addon.masteringInsertParamNames(name);
}

/** Every key an insert processor's construction reads, plus every realtime automation target. */
export interface MasteringInsertParamInfo {
  /** JSON-key parameter name, as used in scene insert params. */
  name: string;
  /**
   * Integer param id for realtime automation lanes / MIDI-CC binding, or null
   * for a construction-only key (one that is not a realtime automation
   * target).
   */
  id: number | null;
  /** Whether the param can be changed live from the audio thread. Always false when `id` is null. */
  rtSafe: boolean;
  /**
   * The C++ type the processor's config builder reads the key as. An
   * `"enum"` value is sent as the number in its {@link choices} entry. A
   * `"string"` or `"array"` key (an embedded impulse response, a per-band
   * list) is construction-only and reports null for `default`, `min`, `max`
   * and `choices`.
   */
  type: 'boolean' | 'number' | 'enum' | 'string' | 'array';
  /**
   * Smallest value construction accepts, or null when the catalog states no
   * limit or when {@link choices} is non-null. Measured, so it is a hard
   * constraint rather than a UI range; see {@link CapabilityCatalogParameter}
   * for what a measured bound does and does not promise.
   */
  min: number | null;
  /** Largest value construction accepts, or null when the catalog states no limit or when {@link choices} is non-null. */
  max: number | null;
  /** Whether {@link min} itself is rejected (a `> min` constraint); false when `min` is null. */
  minExclusive: boolean;
  /** Whether {@link max} itself is rejected (a `< max` constraint); false when `max` is null. */
  maxExclusive: boolean;
  /**
   * `"nyquist"` when the ceiling follows the processing rate: the effective
   * ceiling is then the lower of `max` (measured at the catalog's probe rate)
   * and the host's Nyquist frequency, both exclusive. Null otherwise.
   */
  maxRelativeTo: 'nyquist' | null;
  /**
   * Value the processor uses when the key is absent — the config struct's own
   * field initializer, an enum as its number. Null for an automation target
   * with no construction key, and for a key construction reads with no
   * fallback.
   */
  default: boolean | number | null;
  /** Declared unit of a number; null for any other type. */
  unit: MasteringInsertParamUnit | null;
  /** Lowest value a control should draw, inside `min` / `max`; null when the accepted range is also the display range. */
  uiMin: number | null;
  /** Highest value a control should draw, inside `min` / `max`; null when the accepted range is also the display range. */
  uiMax: number | null;
  /** Axis a control draws the value on. */
  scale: MasteringInsertParamScale;
  /**
   * The accepted values, in value order, when they form a closed set: every
   * declared value of an enum that construction accepts, or the accepted
   * integers of a whole-number parameter whose accepted set has holes. Null
   * otherwise. Non-null means it is the only accepted set — `min` and `max`
   * are then both null.
   */
  choices: MasteringInsertParamChoice[] | null;
  /**
   * The {@link MasteringInsertSlot} this key belongs to, or null for a key that
   * always exists.
   */
  slot: string | null;
  /**
   * Siblings whose live value bounds this key, each read as `this <relation> sibling`;
   * empty for an independent key. `min` and `max` are measured with every sibling at its default.
   */
  dependsOn: MasteringInsertParamDependency[];
}

/**
 * Returns every key an insert / FX processor's construction reads, plus every
 * realtime automation target. Entries come in two runs: first the processor's
 * realtime automation targets in id order (`id` the integer used by
 * {@link RealtimeEngine.setTrackStripInsertParamByName}, `rtSafe` whether it
 * can be changed live); then, sorted by name, the keys construction reads
 * that are not automation targets (`id` null, `rtSafe` false — they take
 * effect only when the insert is built). The names are the same set
 * {@link masteringInsertParamNames} returns, plus any automation target
 * construction does not read. Returns an empty array for an unknown name.
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 * @param sampleRate - Optional host rate in Hz. A key whose `maxRelativeTo` is
 *   `"nyquist"` then reports, as `max` and `maxExclusive`, the bound accepted
 *   when the insert is built and prepared at that rate, which includes any cap
 *   fixed at build time (an EQ band frequency stays at 24000 for a 96000 Hz
 *   host). Omitted, the rate-less answer is returned. Throws for a rate outside
 *   the supported range.
 */
export function masteringInsertParamInfo(
  name: string,
  sampleRate?: number,
): MasteringInsertParamInfo[] {
  const json = addon.masteringInsertParamInfo(name, sampleRate);
  return JSON.parse(json) as MasteringInsertParamInfo[];
}

/** Latency and tail of one insert built from given params and prepared at a given sample rate. */
export interface MasteringInsertTiming {
  /** Latency in samples at the queried sample rate. */
  latencySamples: number;
  /** Tail in samples at the queried sample rate. */
  tailSamples: number;
}

/**
 * Returns the latency and tail of one insert built from `params` and prepared
 * at `sampleRate` — what a host needs for delay compensation of a slot whose
 * configuration changes the processor's delay (an oversampled saturation
 * path, a linear-phase crossover, a lookahead). The insert is built exactly
 * as a scene or strip would build it and asked after `prepare`, so the answer
 * is the one that instance will report. {@link capabilityCatalog}'s
 * `latencySamples` and `tailSamples` are this query at default parameters and
 * 48 kHz.
 *
 * A key the insert does not read is refused rather than ignored, because an
 * ignored key would answer for a configuration the caller did not ask for.
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 * @param params - Flat parameter values, keyed as in
 *   {@link masteringInsertParamInfo}. Each value must be a finite number or a
 *   boolean.
 * @param sampleRate - Rate the insert is prepared at.
 */
export function masteringInsertTiming(
  name: string,
  params: Record<string, number | boolean>,
  sampleRate: number,
): MasteringInsertTiming {
  for (const [key, value] of Object.entries(params)) {
    if (typeof value === 'boolean') {
      continue;
    }
    assertFiniteScalar('masteringInsertTiming', value, key);
  }
  return addon.masteringInsertTiming(
    name,
    JSON.stringify(params),
    sampleRate,
  ) as MasteringInsertTiming;
}

/**
 * How a processor handles a buffer with more than two channels (a surround
 * bed). `multichannel` processes every plane in one call; `stereoPairOnly`
 * operates on the front L/R pair and passes any surround planes through dry.
 * `perChannel`/`passthrough` are reserved and unused by the current catalog.
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

/** One mastering processor's role in the catalog. */
export interface MasteringProcessorCatalogEntry {
  /** Stable processor id (e.g. `dynamics.compressor`, `match.abCrossfade`). */
  id: string;
  /**
   * Coarse role: `pair` (two-input source/reference), `realtime` (usable as a
   * live insert), or `offline` (whole-buffer only). Precedence is
   * `pair > realtime > offline`: a processor that fits more than one role is
   * reported under the highest one.
   */
  kind: 'realtime' | 'offline' | 'pair';
  /**
   * Whether the processor belongs to the realtime-insert set (the processors
   * accepted as live track-strip inserts). Mirrors {@link masteringInsertNames}.
   */
  realtimeInsertable: boolean;
  /** Whether the processor requires a stereo signal (e.g. mid/side EQ). */
  stereoOnly: boolean;
  /**
   * Reported latency for the default 48 kHz / 512-sample probe configuration.
   * Zero for offline processors; configuration-dependent values are estimates.
   */
  latencySamples: number;
  /**
   * Audible decay length for the same default prepared probe. Zero for
   * offline, dry-only, and no-tail processors.
   */
  tailSamples: number;
  /** Coarse realtime work estimate, or null when the processor is not an insert. */
  realtimeCost: MasteringRealtimeCost | null;
  /**
   * How the mixer wraps the processor on a >2-channel (surround) bus insert:
   * `multichannel` (one full-buffer call) or `stereoPairOnly` (front L/R pair,
   * surround planes passed through dry).
   */
  channelPolicy: MasteringChannelPolicy;
  /** Grouping for a processor picker; see {@link MasteringProcessorCategory}. */
  category: MasteringProcessorCategory;
  /**
   * Whether the processor has a configuration whose output at a sample depends only on the
   * input up to it, plus its reported latency. False for `repair.declick`, `repair.declip` and
   * `repair.trimSilence`; true for the other repair stages and every insert.
   */
  causal: boolean;
  /**
   * The processor's parameter descriptors: for an insert the list
   * {@link masteringInsertParamInfo} returns, for a `repair.*` stage the bounds its own
   * configuration validation enforces (no `id`, never `rtSafe`). Empty for any other offline entry.
   */
  params: MasteringInsertParamInfo[];
  /**
   * The insert's conditional key groups in declaration order, named by each
   * parameter's `slot`. Empty for entries that are not realtime-insertable.
   */
  slots: MasteringInsertSlot[];
}

/**
 * Returns the full mastering processor catalog: every processor id paired with
 * its coarse role and capability flags. `kind` follows the precedence
 * `pair > realtime > offline`, so a processor usable in more than one role is
 * reported under the highest. `realtimeInsertable` matches the realtime-insert
 * set ({@link masteringInsertNames}). Hosts use it to filter a processor picker
 * — e.g. to show only realtime-insertable entries for a live track strip, or to
 * gate stereo-only entries on mono material.
 */
export function masteringProcessorCatalog(): MasteringProcessorCatalogEntry[] {
  const json = addon.masteringProcessorCatalog();
  return JSON.parse(json) as MasteringProcessorCatalogEntry[];
}

/** One built-in amp-sim rig with its resolved starting configuration. */
export interface MasteringAmpPresetCatalogEntry {
  /** Stable index used by the amp-sim `presetIndex` parameter. */
  index: number;
  /** Canonical preset identifier accepted by the amp-sim insert. */
  name: string;
  /** Effective values from the core preset, before sparse user overrides. */
  params: Record<string, number | boolean>;
}

/**
 * Returns the built-in amp-sim rigs and their resolved control values.
 *
 * The catalog is read-only metadata for hosts such as Studio. Persist only the
 * preset index and explicit overrides in a project so future core updates can
 * continue to define the canonical DSP configuration.
 */
export function masteringAmpPresetCatalog(): MasteringAmpPresetCatalogEntry[] {
  const json = addon.masteringAmpPresetCatalog();
  return JSON.parse(json) as MasteringAmpPresetCatalogEntry[];
}
