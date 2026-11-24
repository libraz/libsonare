import { chainConfigFromJson } from './internal/chain_config.js';
import { withEntry } from './internal/entry.js';
import { stringifyWithinBudget } from './internal/list_budget.js';
import { resolveInsertParams } from './internal/processor_params.js';
import { parseCatalogJson } from './internal/record_json.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import type {
  MasteringChainConfig,
  MasteringInsertName,
  MasteringInsertParamInfo,
  MasteringInsertTiming,
  MasteringPlatform,
  MasteringPreset,
  MasteringProcessorCatalogEntry,
  MasteringProcessorParams,
  PairAnalysis,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
} from './types.js';
import { assertClosedToken, assertSampleRate, requestOnly } from './validation.js';

/** Names of the built-in mastering presets. */
export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
}

/**
 * The nested {@link MasteringChainConfig} of preset `preset`'s built-in chain.
 * Passing it as `overrides` to {@link masterAudio} reproduces the preset.
 *
 * @throws {TypeError} when `preset` is not a string
 * @throws {RangeError} for an unknown `preset`
 */
export function masteringPresetParams(preset: MasteringPreset): MasteringChainConfig {
  return chainConfigFromJson(addon.masteringPresetParams(preset));
}

/**
 * Read a serialized chain configuration document into the nested
 * {@link MasteringChainConfig}, which {@link masterAudio} takes as `overrides`.
 *
 * The library's own parser validates the document, so a key or token outside
 * the current spelling is refused rather than read. A retired key is refused
 * like any other unknown key, with the sorted list of accepted keys. This is the one entry that
 * takes a chain configuration as JSON; every other call takes the object.
 *
 * @param document - Chain configuration document as JSON:
 *   `{ "version": 2, "params": { "<stage>.<param>": value } }`, the form the
 *   native CLI's `--chain-config` reads. `version` may be left out; dotted keys
 *   and nested objects name the same path, and giving one path twice is refused.
 * @returns The validated configuration in its canonical nested form.
 * @throws {TypeError} when `document` is not a string
 * @throws {SonareError} with code `InvalidFormat` when the document is not
 *   valid JSON, carries another version, or names a key or value the chain
 *   does not accept
 *
 * @example
 * ```ts
 * import { masteringChainConfigFromJson, masterAudio } from '@libraz/libsonare-native';
 *
 * declare const saved: string; // e.g. read from a project file
 * // The document is the whole chain, so it runs without a preset.
 * const overrides = masteringChainConfigFromJson(saved);
 * const mastered = masterAudio({ samples, sampleRate, overrides });
 * ```
 */
export function masteringChainConfigFromJson(document: string): MasteringChainConfig {
  return chainConfigFromJson(addon.masteringChainConfigParams(document));
}

/**
 * Delivery targets the mastering assistant accepts as `targetPlatform`.
 *
 * Read from the library rather than from a list kept here, so a target added in
 * the core is discoverable without a binding change.
 */
export function masteringPlatformNames(): MasteringPlatform[] {
  return addon.masteringPlatformNames();
}

/** Names of the processors `masteringApplyProcessor` accepts. */
export function masteringProcessorNames(): SoloProcessor[] {
  return addon.masteringProcessorNames();
}

/** Names of the pair processors `masteringApplyPairProcessor` accepts. */
export function masteringPairProcessorNames(): PairProcessor[] {
  return addon.masteringPairProcessorNames();
}

/** Names of the pair analyses `masteringAnalyzePair` accepts. */
export function masteringPairAnalysisNames(): PairAnalysis[] {
  return addon.masteringPairAnalysisNames();
}

/** Names of the stereo analyses `masteringAnalyzeStereo` accepts. */
export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return addon.masteringStereoAnalysisNames();
}

/**
 * Returns the insert / FX processor names the realtime insert factory can
 * build (the creative effects.* reverbs / modulation / delay included when FX
 * support is compiled in). Use these to discover valid insert names instead of
 * hardcoding magic strings.
 */
export function masteringInsertNames(): MasteringInsertName[] {
  return addon.masteringInsertNames();
}

/**
 * Returns the camelCase parameter names an insert or FX processor reads.
 * The library refuses an unknown parameter key. Band and sub-band processors
 * enumerate indexed `band{i}.<field>` keys.
 *
 * @param insert - Insert processor name (see {@link masteringInsertNames}).
 * @throws {TypeError} when `insert` is not a string
 * @throws {RangeError} for an unknown `insert`
 */
export function masteringInsertParamNames<N extends MasteringInsertName>(
  insert: N,
): (keyof MasteringProcessorParams<N> & string)[] {
  return withEntry('masteringInsertParamNames', () => {
    assertClosedToken(
      insert,
      withEntry('masteringInsertParamNames', () => masteringInsertNames()),
      'insert',
    );
    return addon.masteringInsertParamNames(insert);
  });
}

/** Request form of {@link masteringInsertParamInfo}. */
export interface MasteringInsertParamInfoRequest {
  /** Insert processor name (see {@link masteringInsertNames}). */
  insert: MasteringInsertName;
  /**
   * Host rate in Hz. Absent: the rate-less catalog view, where a `maxRelativeTo`
   * of `"nyquist"` keeps the 24000 cap of the 48 kHz probe.
   */
  sampleRate?: number;
}

/** Request form of {@link masteringInsertTiming}. */
export interface MasteringInsertTimingRequest<N extends MasteringInsertName = MasteringInsertName> {
  /** Insert processor name (see {@link masteringInsertNames}). */
  insert: N;
  /** Parameters of the insert. */
  params?: MasteringProcessorParams<N>;
  /** Rate the insert is prepared at, in Hz; there is no default. */
  sampleRate: number;
}

/**
 * Returns every key an insert / FX processor's construction reads, plus every
 * realtime automation target. Entries come in two runs: first the processor's
 * realtime automation targets in id order (`id` the integer used by
 * {@link RealtimeEngine.setTrackStripInsertParamByName}, `rtSafe` whether it
 * can be changed live); then, sorted by name, the keys construction reads
 * that are not automation targets (no `id`, `rtSafe` false — they take
 * effect only when the insert is built). The names are the same set
 * {@link masteringInsertParamNames} returns, plus any automation target
 * construction does not read. Returns an empty array for an unknown name.
 *
 * With `sampleRate`, a key whose `maxRelativeTo` is `"nyquist"` reports, as
 * `max` and `maxExclusive`, the bound accepted when the insert is built and
 * prepared at that rate (an EQ band frequency reaches 48000 for a 96000 Hz
 * host). Without it the rate-less answer is returned, with the 24000 cap of the
 * 48 kHz probe.
 *
 * @throws {TypeError} when the request is the wrong type or `insert` is not a string
 * @throws {RangeError} for a `sampleRate` outside the supported range
 */
export function masteringInsertParamInfo(
  request: MasteringInsertParamInfoRequest,
): MasteringInsertParamInfo[] {
  return withEntry('masteringInsertParamInfo', () => {
    const r = requestOnly<MasteringInsertParamInfoRequest>('masteringInsertParamInfo', [request]);
    assertClosedToken(
      r.insert,
      masteringProcessorCatalog().map((entry) => entry.id),
      'insert',
    );
    if (!isAbsent(r.sampleRate)) {
      assertSampleRate(r.sampleRate);
    }
    const json = addon.masteringInsertParamInfo(r.insert, r.sampleRate ?? undefined);
    return parseCatalogJson(json) as MasteringInsertParamInfo[];
  });
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
 * @throws {TypeError} for a missing `sampleRate`, or a value of the wrong type or an unknown key
 * @throws {RangeError} for an unknown insert, a non-finite number or a value the insert refuses
 */
export function masteringInsertTiming<N extends MasteringInsertName>(
  request: MasteringInsertTimingRequest<N>,
): MasteringInsertTiming {
  return withEntry('masteringInsertTiming', () => {
    const r = requestOnly<MasteringInsertTimingRequest<N>>('masteringInsertTiming', [request]);
    assertSampleRate(r.sampleRate);
    const resolved = withEntry('masteringInsertTiming', () =>
      resolveInsertParams(r.insert, r.params ?? {}),
    );
    return addon.masteringInsertTiming(
      r.insert,
      stringifyWithinBudget(resolved, 'params'),
      r.sampleRate,
    ) as MasteringInsertTiming;
  });
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
  return parseCatalogJson(json) as MasteringProcessorCatalogEntry[];
}

/** One built-in amp-sim rig with its resolved starting configuration. */
export interface MasteringAmpPresetCatalogEntry {
  /** Stable index used by the amp-sim `presetIndex` parameter. */
  index: number;
  /** Canonical preset identifier accepted by the amp-sim insert. */
  name: string;
  /** Effective values from the core preset, before sparse user overrides. */
  params: MasteringProcessorParams<'saturation.ampSim'>;
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
  return parseCatalogJson(json) as MasteringAmpPresetCatalogEntry[];
}
