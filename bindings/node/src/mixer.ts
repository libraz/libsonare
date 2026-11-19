import { entryClass } from './internal/entry.js';
import { assertStripMetering, mixSceneJsonToNative } from './internal/mix_scene.js';
import { addon } from './native.js';
import type {
  AutomationCurve,
  GoniometerPoint,
  MeterTap,
  MixerProcessResult,
  MixMeterSnapshot,
  MixSceneDocument,
  PanLaw,
  PanMode,
  SendTiming,
  StripMeteringOptions,
  StripRef,
  SurroundPan,
} from './types.js';
import {
  assertClosedToken,
  assertFloatValue,
  assertIntegerValue,
  assertSampleRate,
  requestObject,
} from './validation.js';
import {
  automationCurveValue,
  meterTapValue,
  panLawValue,
  panModeValue,
  sendTimingValue,
} from './value_coercion.js';

/** Role of a bus added with {@link Mixer.addBus}. */
export type MixBusRole = 'master' | 'aux' | 'submix';

const MIX_BUS_ROLES: readonly MixBusRole[] = ['master', 'aux', 'submix'];

/** Construction options of a {@link Mixer}. */
export interface MixerConfig {
  /** Scene to build the mixer from; omitted gives an empty mixer. */
  scene?: MixSceneDocument;
  /** Sample rate in Hz, integer in `[8000, 384000]`. Required; there is no default rate. */
  sampleRate: number;
  /** Maximum block size per {@link Mixer.processStereo} call, in samples. Default `512`; `0` is refused. */
  maxBlockSize?: number;
}

/** Options of {@link Mixer.fromJson}; the same rate and block size as {@link MixerConfig}. */
export interface MixerJsonOptions {
  /** Sample rate in Hz, integer in `[8000, 384000]`. Required. */
  sampleRate: number;
  /** Maximum block size per {@link Mixer.processStereo} call, in samples. Default `512`; `0` is refused. */
  maxBlockSize?: number;
}

/** Master-meter options of {@link Mixer.configureMeter}. */
export interface MixerMeterOptions {
  /** Whether to meter the master output. Required. */
  enabled: boolean;
  /** True-peak oversampling factor, `4` or `8`. Omitted takes the library default (`4`); any other value is a `RangeError`. */
  truePeakOversample?: number;
  /** Whether to measure momentary, short-term and integrated loudness. Omitted takes the library default (on). */
  measureLufs?: boolean;
  /** Whether to measure the inter-sample (true) peak. Omitted takes the library default (on). */
  measureTruePeak?: boolean;
}

/** Polarity of a strip's channels for {@link Mixer.setPolarityInvert}. */
export interface PolarityInvert {
  /** Invert the left channel. */
  left: boolean;
  /** Invert the right channel. */
  right: boolean;
}

/** A send added with {@link Mixer.addSend}; mirrors the scene document's send. */
export interface MixerSend {
  /** Send id. */
  id: string;
  /** Destination bus id. */
  destinationBusId: string;
  /** Initial send level in dB. Default `0`. */
  sendDb?: number;
  /** Tap point, a {@link SendTiming} token. Default `'postFader'`; a number is refused. */
  timing?: SendTiming;
}

/** One automation event; `curve` is an {@link AutomationCurve} token, default `'linear'`. */
export interface MixerAutomationPointBase {
  /** Absolute sample position from the start of processing. */
  samplePos: number;
  /** Interpolation curve toward the value. A number is refused. */
  curve?: AutomationCurve;
}

/** Fader automation event for {@link Mixer.scheduleFaderAutomation}. */
export interface FaderAutomationPoint extends MixerAutomationPointBase {
  /** Target fader level in dB. */
  faderDb: number;
}

/** Pan automation event for {@link Mixer.schedulePanAutomation}. */
export interface PanAutomationPoint extends MixerAutomationPointBase {
  /** Target pan position in `[-1, 1]`. */
  pan: number;
}

/** Width automation event for {@link Mixer.scheduleWidthAutomation}. */
export interface WidthAutomationPoint extends MixerAutomationPointBase {
  /** Target stereo width in `[0, 2]`. */
  width: number;
}

/** Send-level automation event for {@link Mixer.scheduleSendAutomation}. */
export interface SendAutomationPoint extends MixerAutomationPointBase {
  /** Target send level in dB. */
  sendDb: number;
}

/** Insert-parameter automation event for {@link Mixer.scheduleInsertAutomation}. */
export interface InsertAutomationPoint extends MixerAutomationPointBase {
  /** Processor-specific parameter id. */
  paramId: number;
  /** Target parameter value. */
  value: number;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !ArrayBuffer.isView(value)
  );
}

function isAbsent(value: unknown): value is undefined | null {
  return value === undefined || value === null;
}

function requiredNumber(fnName: string, value: unknown, name: string): number {
  assertFloatValue(fnName, value, name);
  return value as number;
}

function optionalNumber(fnName: string, value: unknown, name: string, fallback: number): number {
  return isAbsent(value) ? fallback : requiredNumber(fnName, value, name);
}

function requiredString(fnName: string, value: unknown, name: string): string {
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${name} must be a string`);
  }
  return value;
}

function requiredBoolean(fnName: string, value: unknown, name: string): boolean {
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: ${name} must be a boolean`);
  }
  return value;
}

/** A closed token: a non-string is a `TypeError` (a number never selects an ordinal), an unknown string a `RangeError`. */
function tokenArg(
  fnName: string,
  value: unknown,
  name: string,
  resolve: (token: string) => number,
  fallback?: string,
): number {
  if (isAbsent(value)) {
    if (fallback === undefined) {
      throw new TypeError(`${fnName}: ${name} must be a string`);
    }
    return resolve(fallback);
  }
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${name} must be a string`);
  }
  return resolve(value);
}

const curveToken = (token: string): number => automationCurveValue(token as AutomationCurve);

/** An omitted block size stays undefined, which the addon passes on as the library default. */
function maxBlockSizeOf(fnName: string, value: unknown): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  assertIntegerValue(fnName, value, 'maxBlockSize');
  if (value === 0) {
    throw new RangeError(`${fnName}: maxBlockSize must not be 0 (omit it for the default)`);
  }
  if (value < 0) {
    throw new RangeError(`${fnName}: maxBlockSize must be a positive integer`);
  }
  return value;
}

/** Scene JSON handed from {@link Mixer.fromJson} to the constructor; consumed there. */
let adoptedSceneJson: string | undefined;

/**
 * Scene-based persistent stereo mixer. Built from a scene document (or empty),
 * it routes per-strip stereo blocks through a compiled routing graph (sends,
 * buses, inserts) into a stereo master. Strips are addressed by 0-based index
 * or by their string id ({@link StripRef}); the underlying strip handles are
 * never exposed.
 */
@entryClass('Mixer', ['processStereo'])
export class Mixer {
  private native: InstanceType<typeof addon.Mixer>;
  private disposed = false;

  /**
   * Build a mixer from a scene document.
   *
   * A strip's meters are sized when the strip is built, so this is where their
   * configuration is chosen: {@link MixSceneStrip.metering} selects it, and
   * leaving it out keeps the full default (LUFS + true peak at 4x, about 1.4 MB
   * per strip at 48 kHz).
   *
   * @param config - See {@link MixerConfig}. An absent `scene` gives an empty mixer.
   * @throws TypeError for a wrong-typed field or an unknown key; RangeError for
   *   a sample rate outside `[8000, 384000]` or a `maxBlockSize` of `0` or below.
   */
  constructor(config: MixerConfig) {
    const json = adoptedSceneJson;
    adoptedSceneJson = undefined;
    const bag = requestObject('Mixer', config, 'config', true);
    assertSampleRate('Mixer', bag.sampleRate);
    const maxBlockSize = maxBlockSizeOf('Mixer', bag.maxBlockSize);
    let sceneJson = json;
    if (sceneJson === undefined) {
      if (!isAbsent(bag.scene) && !isObject(bag.scene)) {
        throw new TypeError('Mixer: scene must be an object');
      }
      sceneJson = JSON.stringify(bag.scene ?? { strips: [], buses: [] });
    }
    sceneJson = mixSceneJsonToNative('Mixer', sceneJson);
    this.native = new addon.Mixer(sceneJson, bag.sampleRate as number, maxBlockSize);
  }

  /**
   * Build a mixer from a scene JSON string (for example a {@link mixingScenePreset} serialized with `JSON.stringify`).
   *
   * @param json - Scene JSON (strips, buses, sends, connections, inserts)
   * @param options - Sample rate (required) and block size, see {@link MixerJsonOptions}
   * @throws TypeError for a non-string `json`, a wrong-typed field or an unknown key;
   *   RangeError for a sample rate outside `[8000, 384000]` or a `maxBlockSize` of `0` or below.
   */
  static fromJson(json: string, options: MixerJsonOptions): Mixer {
    requiredString('Mixer.fromJson', json, 'json');
    const bag = requestObject('Mixer.fromJson', options, 'options', true);
    adoptedSceneJson = json;
    try {
      return new Mixer({
        sampleRate: bag.sampleRate,
        maxBlockSize: bag.maxBlockSize,
      } as MixerConfig);
    } finally {
      adoptedSceneJson = undefined;
    }
  }

  /**
   * Rebuild and compile the routing graph without resetting its absolute
   * automation sample position or queued strip automation.
   */
  compile(): void {
    this.native.compile();
  }

  /** Number of strips in the mixer. */
  get stripCount(): number {
    return this.native.stripCount();
  }

  /**
   * Non-fatal warnings captured when this mixer was built from scene JSON: one
   * entry per channel-strip insert that was handed param keys it does not read
   * (a likely typo, or a key meant for a different processor). The scene still
   * loaded; these keys simply took no effect. Empty when every key was consumed.
   * Use {@link masteringInsertParamNames} to discover the keys an insert accepts.
   */
  sceneWarnings(): string[] {
    return this.native.sceneWarnings();
  }

  /** Longest audible serial processor-tail path to the master, in samples. */
  get tailSamples(): number {
    return this.native.tailSamples();
  }

  /** Reported latency (samples) of the compiled graph, for aligning dry/wet material. */
  get latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.native.maxBlockSize();
  }

  /**
   * Process a zero-input block to drain delayed / tail audio after the host has
   * stopped feeding strip inputs. `numSamples` must not exceed the configured
   * block size.
   */
  drainTailStereo(numSamples: number): MixerProcessResult {
    return this.native.drainTailStereo(numSamples);
  }

  /**
   * Marks the end of the signal for every strip, bus and master-output meter
   * (see {@link configureMeter}). The true-peak
   * reading runs one reconstruction-filter group delay behind the block; this
   * reads that pending stretch against silence and folds it into the
   * snapshots, so `truePeakDbtp` covers the whole signal. Call it after the last
   * block of an offline render; a later process call starts a new signal.
   */
  flushMeters(): void {
    this.native.flushMeters();
  }

  /**
   * Turn the master-output meter on or off.
   *
   * While on, every process and drain call meters the stereo master it just
   * produced, so a caller reads {@link meterSnapshot} instead of copying the
   * output and measuring it again. `truePeakDbtp*` is an inter-sample peak taken
   * after oversampling (ITU-R BS.1770-4 Annex 2 requires at least 4x), which is a
   * different and higher quantity than the sample peak.
   *
   * Enabling resets the meter, so a reading never mixes in audio from a period
   * when metering was off; disabling keeps the last reading readable.
   *
   * @param options - See {@link MixerMeterOptions}.
   * @throws TypeError for a wrong-typed field or an unknown key; RangeError when
   *   `truePeakOversample` is not `4` or `8`.
   */
  configureMeter(options: MixerMeterOptions): void {
    const bag = requestObject('configureMeter', options, 'options', true);
    this.native.configureMeter(
      requiredBoolean('configureMeter', bag.enabled, 'enabled'),
      bag.truePeakOversample as number | undefined,
      bag.measureLufs as boolean | undefined,
      bag.measureTruePeak as boolean | undefined,
    );
  }

  /**
   * Latest master-output meter reading, the same record {@link busMeter} reads
   * for a bus. All dB fields are finite and floored at -120.
   *
   * @throws SonareError with code `InvalidState` when the meter has never been enabled.
   */
  meterSnapshot(): MixMeterSnapshot {
    return this.native.meterSnapshot();
  }

  /**
   * Mix one block of per-strip stereo audio into the stereo master.
   *
   * @param leftInputs - `leftInputs[i]` is the left channel of strip `i`
   * @param rightInputs - `rightInputs[i]` is the right channel of strip `i`.
   *   Fewer inputs than strips leave the remaining strips silent.
   * @throws RangeError when more inputs are given than the mixer has strips
   */
  processStereo(leftInputs: Float32Array[], rightInputs: Float32Array[]): MixerProcessResult {
    if (leftInputs.length !== rightInputs.length) {
      throw new RangeError('leftInputs and rightInputs must have the same length.');
    }
    return this.native.processStereo(leftInputs, rightInputs);
  }

  /**
   * Schedule a sample-accurate insert-parameter automation event.
   *
   * @param strip - Strip index or id
   * @param insertIndex - Index into the strip's combined [pre... post...] inserts
   * @param point - `paramId` (processor-specific parameter id), `samplePos`
   *   (absolute sample position from the start of processing), `value` (target)
   *   and `curve` (default `'linear'`)
   * @throws TypeError for a wrong-typed field, a numeric `curve` or an unknown key;
   *   RangeError for an unknown strip or curve token
   */
  scheduleInsertAutomation(
    strip: StripRef,
    insertIndex: number,
    point: InsertAutomationPoint,
  ): void {
    const fn = 'scheduleInsertAutomation';
    const bag = requestObject(fn, point, 'point', true);
    // The native call addresses the strip by index only.
    const index = typeof strip === 'string' ? this.stripById(strip) : strip;
    this.native.scheduleInsertAutomation(
      index,
      insertIndex,
      requiredNumber(fn, bag.paramId, 'paramId'),
      requiredNumber(fn, bag.samplePos, 'samplePos'),
      requiredNumber(fn, bag.value, 'value'),
      tokenArg(fn, bag.curve, 'curve', curveToken, 'linear'),
    );
  }

  /**
   * Resolve a strip id to its 0-based index.
   *
   * @throws RangeError when no strip has that id
   */
  stripById(id: string): number {
    requiredString('stripById', id, 'id');
    const index = this.native.stripById(id);
    if (index === null || index === undefined) {
      throw new RangeError(`stripById: no strip with id '${id}'`);
    }
    return index;
  }

  /**
   * Add a channel strip to the mixer topology.
   *
   * @param id - Unique strip id
   * @param metering - Meter configuration for the strip's pre/post taps; omitted
   *   keeps the full default (LUFS + true peak at 4x, about 1.4 MB per strip at
   *   48 kHz)
   *
   * Marks the routing graph dirty; call {@link compile} (or process) to rebuild.
   *
   * @returns The new strip's index
   * @throws If the id is already taken; RangeError when `truePeakOversample` is not 4 or 8 (omit it for the default)
   */
  addStrip(id: string, metering?: StripMeteringOptions): number {
    assertStripMetering('addStrip', metering);
    this.native.addStrip(id, metering);
    return this.stripById(id);
  }

  /**
   * Add a bus to the mixer topology.
   *
   * @param id - Unique bus id
   * @param role - A {@link MixBusRole}; defaults to `'aux'`. An unknown value is a `RangeError`.
   *
   * Marks the routing graph dirty; call {@link compile} (or process) to rebuild.
   */
  addBus(id: string, role?: MixBusRole): void {
    if (!isAbsent(role)) {
      assertClosedToken('addBus', role, MIX_BUS_ROLES, 'role');
    }
    this.native.addBus(id, role ?? 'aux');
  }

  /**
   * Route a strip or bus into a submix bus (or the master), replacing where it
   * went before.
   *
   * @param sourceId - A strip id or a non-master bus id
   * @param busId - A declared bus id, or the id the master resolves to; the
   *   literal `"master"` is not an alias when the master bus has another id
   *
   * Every connection leaving `sourceId` is removed first, including a
   * scene-authored strip-to-strip edge, then `sourceId -> busId` is added. Like
   * {@link addBus} it only marks the routing graph dirty: nothing compiles in
   * the call, so a bus's meters read as never compiled until {@link compile}
   * (or process) runs. An explicit route suppresses the default master edge, so
   * a strip sent to a submix is silent on the master until the submix itself is
   * routed on. The recipe for sending a strip into a submix:
   *
   * @example
   * mixer.addStrip('a');
   * mixer.addBus('sub', 'submix');
   * mixer.setOutputBus('a', 'sub');
   * mixer.setOutputBus('sub', 'master');
   * mixer.compile();
   *
   * @throws RangeError naming the ids (and the path, for a
   *   cycle) for an unknown source or destination, a destination that is not a
   *   bus, the master as source, a self-route, or an edit that closes a cycle
   *   through connections or sends; the scene is unchanged after a refusal
   */
  setOutputBus(sourceId: string, busId: string): void {
    this.native.setOutputBus(sourceId, busId);
  }

  /** Remove a bus by id. */
  removeBus(id: string): void {
    this.native.removeBus(id);
  }

  /** Number of buses in the mixer topology. */
  get busCount(): number {
    return this.native.busCount();
  }

  /**
   * Add a VCA group with the given id and gain offset.
   *
   * @param id - Unique group id
   * @param gainDb - Group gain offset in dB
   * @param members - Strip ids that belong to the group
   */
  addVcaGroup(id: string, gainDb = 0.0, members: string[] = []): void {
    this.native.addVcaGroup(id, gainDb, members);
  }

  /** Set an existing VCA group's gain in dB. */
  setVcaGroupGainDb(id: string, gainDb: number): void {
    this.native.setVcaGroupGainDb(id, gainDb);
  }

  /** Replace an existing VCA group's strip membership. */
  setVcaGroupMembers(id: string, members: string[]): void {
    this.native.setVcaGroupMembers(id, members);
  }

  /** Remove a VCA group by id. */
  removeVcaGroup(id: string): void {
    this.native.removeVcaGroup(id);
  }

  /** Number of VCA groups in the mixer topology. */
  get vcaGroupCount(): number {
    return this.native.vcaGroupCount();
  }

  /** Set a strip's input trim in dB (applied before the channel processing). */
  setInputTrimDb(strip: StripRef, db: number): void {
    this.native.setInputTrimDb(strip, db);
  }

  /** Set a strip's fader gain in dB. */
  setFaderDb(strip: StripRef, db: number): void {
    this.native.setFaderDb(strip, db);
  }

  /**
   * Set a strip's pan position (-1..1) with an optional pan mode. Omitting
   * `panMode` keeps the strip's current mode (a plain pan nudge does not reset
   * a scene strip's pan mode).
   *
   * @throws {RangeError} when `pan` is outside `[-1, 1]` or non-finite.
   */
  setPan(strip: StripRef, pan: number, panMode?: PanMode): void {
    if (panMode === undefined) {
      this.native.setPan(strip, pan);
    } else {
      this.native.setPan(strip, pan, panModeValue(panMode));
    }
  }

  /**
   * Set a strip's stereo width in `[0, 2]` (0 = mono, 1 = original, 2 = widest).
   *
   * @throws {RangeError} when `width` is outside `[0, 2]` or non-finite.
   */
  setWidth(strip: StripRef, width: number): void {
    this.native.setWidth(strip, width);
  }

  /**
   * Snap a strip's input-trim, fader, pan and width smoothers to the values
   * already set on it, so the next processed block opens at those values
   * instead of gliding to them over the smoothing window (~5 ms).
   *
   * Call it after configuring a strip and before rendering a finite buffer: a
   * strip is smoothed for a live fader, and an offline render that does not
   * settle carries that glide as a level and image sweep across the head of
   * its output. Unlike a reset it clears nothing — automation, meters and
   * insert state are untouched.
   *
   * @example
   * ```ts
   * mixer.setFaderDb('bass', -3);
   * mixer.setPan('bass', 0.3);
   * mixer.settle('bass');
   * const { left, right } = mixer.processStereo([leftSamples], [rightSamples]);
   * ```
   */
  settle(strip: StripRef): void {
    this.native.settle(strip);
  }

  /** Set a strip's mute state. */
  setMuted(strip: StripRef, muted: boolean): void {
    this.native.setMuted(strip, muted);
  }

  /** Set a strip's solo state. Takes effect on the next process (no recompile). */
  setSoloed(strip: StripRef, soloed: boolean): void {
    this.native.setSoloed(strip, soloed);
  }

  /** Mark a strip solo-safe so it is never implied-muted by another strip's solo. */
  setSoloSafe(strip: StripRef, soloSafe: boolean): void {
    this.native.setSoloSafe(strip, soloSafe);
  }

  /**
   * Invert the polarity of a strip's left and/or right channel.
   *
   * @throws TypeError when `left` or `right` is not a boolean, or for an unknown key
   */
  setPolarityInvert(strip: StripRef, invert: PolarityInvert): void {
    const bag = requestObject('setPolarityInvert', invert, 'invert', true);
    this.native.setPolarityInvert(
      strip,
      requiredBoolean('setPolarityInvert', bag.left, 'left'),
      requiredBoolean('setPolarityInvert', bag.right, 'right'),
    );
  }

  /**
   * Set a strip's pan law.
   *
   * @throws TypeError for a non-string (a number never selects a law); RangeError for an unknown name
   */
  setPanLaw(strip: StripRef, panLaw: PanLaw): void {
    this.native.setPanLaw(
      strip,
      tokenArg('setPanLaw', panLaw, 'panLaw', (token) => panLawValue(token as PanLaw)),
    );
  }

  /** Set a per-strip channel delay in samples, moving the strip later relative to the others. */
  setChannelDelaySamples(strip: StripRef, delaySamples: number): void {
    this.native.setChannelDelaySamples(strip, delaySamples);
  }

  /** Set a strip's direct VCA trim in dB (persisted as the scene's `vcaOffsetDb`). */
  setVcaOffsetDb(strip: StripRef, offsetDb: number): void {
    this.native.setVcaOffsetDb(strip, offsetDb);
  }

  /** Set a strip's independent left/right pan positions (dual-pan mode). */
  setDualPan(strip: StripRef, leftPan: number, rightPan: number): void {
    this.native.setDualPan(strip, leftPan, rightPan);
  }

  /**
   * Set a strip's surround pan position, used when the strip feeds a >2-channel
   * bus.
   *
   * Applied when the engine's track mixer renders this strip's lane into a
   * destination with more than two channels. This stereo-only mixer's own block
   * entry points ignore it.
   *
   * @throws TypeError for a wrong-typed field or an unknown key; RangeError for a
   *   field outside its range
   */
  setSurroundPan(strip: StripRef, pan: SurroundPan): void {
    this.native.setSurroundPan(strip, requestObject('setSurroundPan', pan, 'pan', true));
  }

  /**
   * Add a post-construction send from a strip to a destination bus.
   *
   * @param strip - Strip index or id
   * @param send - See {@link MixerSend}; mirrors the scene document's send
   * @returns The 0-based index of the new send (use with {@link setSendDb} /
   *   {@link scheduleSendAutomation}).
   * @throws TypeError for a wrong-typed field, a numeric `timing` or an unknown key;
   *   RangeError for an unknown timing token
   */
  addSend(strip: StripRef, send: MixerSend): number {
    const fn = 'addSend';
    const bag = requestObject(fn, send, 'send', true);
    return this.native.addSend(
      strip,
      requiredString(fn, bag.id, 'id'),
      requiredString(fn, bag.destinationBusId, 'destinationBusId'),
      optionalNumber(fn, bag.sendDb, 'sendDb', 0),
      tokenArg(
        fn,
        bag.timing,
        'timing',
        (token) => sendTimingValue(token as SendTiming),
        'postFader',
      ),
    );
  }

  /** Set the send level (dB) of a strip's send addressed by add-order index. */
  setSendDb(strip: StripRef, sendIndex: number, sendDb: number): void {
    this.native.setSendDb(strip, sendIndex, sendDb);
  }

  /**
   * Remove a strip's send addressed by add-order index.
   *
   * Sends with a higher index shift down by one after removal, so cached send
   * indices must be re-resolved following this call.
   */
  removeSend(strip: StripRef, sendIndex: number): void {
    this.native.removeSend(strip, sendIndex);
  }

  /**
   * Read a strip's current meter snapshot. With no `tap` this returns the
   * post-fader meter; pass a {@link MeterTap} to read that tap instead.
   *
   * @throws TypeError for a non-string `tap` (a number never selects a tap);
   *   RangeError for an unknown name
   */
  stripMeter(strip: StripRef, tap?: MeterTap): MixMeterSnapshot {
    if (isAbsent(tap)) {
      return this.native.stripMeter(strip);
    }
    return this.native.meterTap(
      strip,
      tokenArg('stripMeter', tap, 'tap', (token) => meterTapValue(token as MeterTap)),
    );
  }

  /** Read the post-insert meter for a compiled bus, including the master bus. */
  busMeter(busId: string): MixMeterSnapshot {
    return this.native.busMeter(busId);
  }

  /**
   * Number of blocks in which a bus discarded recursive state because a
   * non-finite value had reached it. Same contract as
   * {@link stripNonFiniteDiscardCount}, for a bus: covers every insert the
   * bus owns and its meter, neither of which is separately addressable, so a
   * discard inside one is observable only through this number.
   *
   * A bus's DSP record is created by the first {@link compile}, so a bus
   * that has been added with {@link addBus} but never compiled throws rather
   * than reading as zero -- zero would read as clean. An unknown bus id also
   * throws.
   */
  busNonFiniteDiscardCount(busId: string): number {
    return this.native.busNonFiniteDiscardCount(busId);
  }

  /**
   * Read up to `maxPoints` of the latest goniometer samples for a strip.
   *
   * `maxPoints` must be a finite non-negative integer; anything else throws a
   * `TypeError` or `RangeError`. It is a request rather than an allocation
   * size — a value beyond the strip's goniometer ring simply returns every
   * point the ring holds.
   */
  readGoniometerLatest(strip: StripRef, maxPoints: number): GoniometerPoint[] {
    return this.native.readGoniometerLatest(strip, maxPoints);
  }

  /**
   * Number of blocks in which the strip discarded recursive state because a
   * non-finite value had reached it.
   *
   * Advisory telemetry, and the only thing that separates a degraded strip
   * from a clean one. A discard returns the affected state to its
   * post-reset value, so the strip recovers in silence and the output stays
   * finite and in range while carrying samples unrelated to the input;
   * nothing else reports that this happened.
   *
   * The count covers the strip's own state, its EQ, every insert it owns and
   * both of its meters. None of those is separately addressable here, so a
   * discard inside one is observable only through this number -- and a
   * meter that loses its loudness window then reports the floor, which is
   * exactly what a genuinely silent strip reports, so nothing else
   * distinguishes the two.
   *
   * A meter's own discard lags by one block: it checks its loudness state at
   * the top of a block, before consuming that block's samples, so the block
   * that corrupts it is not the block the count moves on -- read this again
   * after one more block has processed. The EQ and inserts have no such lag;
   * they discard at the end of their own process, in the same block that
   * carried the poison.
   *
   * Cumulative since the strip was created and never cleared, so two
   * readings bracket a span of audio. The unit is one processed block, never
   * a channel, so a stereo block that discards on both channels adds one and
   * the number does not depend on a dimension the caller did not choose.
   */
  stripNonFiniteDiscardCount(strip: StripRef): number {
    return this.native.stripNonFiniteDiscardCount(strip);
  }

  /**
   * Schedule sample-accurate fader (dB) automation on a strip.
   *
   * @param strip - Strip index or id
   * @param point - See {@link FaderAutomationPoint}; `curve` defaults to `'linear'`
   * @throws TypeError for a wrong-typed field, a numeric `curve` or an unknown key;
   *   RangeError for an unknown strip or curve token
   */
  scheduleFaderAutomation(strip: StripRef, point: FaderAutomationPoint): void {
    const fn = 'scheduleFaderAutomation';
    const bag = requestObject(fn, point, 'point', true);
    this.native.scheduleFaderAutomation(
      strip,
      requiredNumber(fn, bag.samplePos, 'samplePos'),
      requiredNumber(fn, bag.faderDb, 'faderDb'),
      tokenArg(fn, bag.curve, 'curve', curveToken, 'linear'),
    );
  }

  /**
   * Schedule sample-accurate pan automation on a strip.
   *
   * @param strip - Strip index or id
   * @param point - See {@link PanAutomationPoint}; `curve` defaults to `'linear'`
   * @throws TypeError for a wrong-typed field, a numeric `curve` or an unknown key;
   *   RangeError for an unknown strip or curve token
   */
  schedulePanAutomation(strip: StripRef, point: PanAutomationPoint): void {
    const fn = 'schedulePanAutomation';
    const bag = requestObject(fn, point, 'point', true);
    this.native.schedulePanAutomation(
      strip,
      requiredNumber(fn, bag.samplePos, 'samplePos'),
      requiredNumber(fn, bag.pan, 'pan'),
      tokenArg(fn, bag.curve, 'curve', curveToken, 'linear'),
    );
  }

  /**
   * Schedule sample-accurate width automation on a strip.
   *
   * @param strip - Strip index or id
   * @param point - See {@link WidthAutomationPoint}; `curve` defaults to `'linear'`
   * @throws TypeError for a wrong-typed field, a numeric `curve` or an unknown key;
   *   RangeError for an unknown strip or curve token
   */
  scheduleWidthAutomation(strip: StripRef, point: WidthAutomationPoint): void {
    const fn = 'scheduleWidthAutomation';
    const bag = requestObject(fn, point, 'point', true);
    this.native.scheduleWidthAutomation(
      strip,
      requiredNumber(fn, bag.samplePos, 'samplePos'),
      requiredNumber(fn, bag.width, 'width'),
      tokenArg(fn, bag.curve, 'curve', curveToken, 'linear'),
    );
  }

  /**
   * Schedule sample-accurate send-level (dB) automation on a strip's send.
   *
   * @param strip - Strip index or id
   * @param sendIndex - Send index in the strip's add order
   * @param point - See {@link SendAutomationPoint}; `curve` defaults to `'linear'`
   * @throws TypeError for a wrong-typed field, a numeric `curve` or an unknown key;
   *   RangeError for an unknown strip or curve token
   */
  scheduleSendAutomation(strip: StripRef, sendIndex: number, point: SendAutomationPoint): void {
    const fn = 'scheduleSendAutomation';
    const bag = requestObject(fn, point, 'point', true);
    this.native.scheduleSendAutomation(
      strip,
      sendIndex,
      requiredNumber(fn, bag.samplePos, 'samplePos'),
      requiredNumber(fn, bag.sendDb, 'sendDb'),
      tokenArg(fn, bag.curve, 'curve', curveToken, 'linear'),
    );
  }

  /** Serialize the current scene (strips, buses, sends, connections) to JSON. */
  toJson(): string {
    return this.native.toSceneJson();
  }

  /** Release the underlying native mixer. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases the native mixer; lets `using` free it automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
