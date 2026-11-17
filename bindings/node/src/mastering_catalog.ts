import { type MasteringInsertParams, resolveInsertParams } from './internal/processor_params.js';
import { addon } from './native.js';
import type {
  MasteringInsertParamInfo,
  MasteringInsertTiming,
  MasteringPreset,
  MasteringProcessorCatalogEntry,
  PairAnalysis,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
} from './types.js';

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

/**
 * Returns every key an insert / FX processor's construction reads, plus every
 * realtime automation target. Entries come in two runs: first the processor's
 * realtime automation targets in id order (`id` the integer used by
 * {@link RealtimeEngine.setTrackStripInsertParamByName}, `rtSafe` whether it
 * can be changed live); then, sorted by name, the keys construction reads
 * that are not automation targets (`id` null, `rtSafe` false — they take
 * effect only when the insert is built). The names are the same set
 * {@link masteringInsertParamNames} returns, plus any automation target
 * construction does not read. Any id of the processor catalog is served, an
 * offline repair stage (`repair.declick`, `repair.declip`, `repair.trimSilence`)
 * included, with the rows its catalog entry's `params` carries; those rows have
 * a null `id`. Returns an empty array for an unknown name.
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 * @param sampleRate - Optional host rate in Hz. A key whose `maxRelativeTo` is
 *   `"nyquist"` then reports, as `max` and `maxExclusive`, the bound accepted
 *   when the insert is built and prepared at that rate (an EQ band frequency
 *   reaches 48000 for a 96000 Hz host). Omitted, the rate-less answer is
 *   returned, with the 24000 cap of the 48 kHz probe. Throws for a rate outside
 *   the supported range.
 */
export function masteringInsertParamInfo(
  name: string,
  sampleRate?: number,
): MasteringInsertParamInfo[] {
  const json = addon.masteringInsertParamInfo(name, sampleRate);
  return JSON.parse(json) as MasteringInsertParamInfo[];
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
 *   {@link masteringInsertParamInfo}. Each value must match the key's declared
 *   `type`: a finite number or boolean, the `choices` name of an `enum` key, a
 *   string for a `string` key, or a list of finite numbers for an `array` key.
 *   The same document a scene or strip insert is built from.
 * @param sampleRate - Rate the insert is prepared at.
 */
export function masteringInsertTiming(
  name: string,
  params: MasteringInsertParams,
  sampleRate: number,
): MasteringInsertTiming {
  const resolved = resolveInsertParams('masteringInsertTiming', name, params);
  return addon.masteringInsertTiming(
    name,
    JSON.stringify(resolved),
    sampleRate,
  ) as MasteringInsertTiming;
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
