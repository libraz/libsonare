import {
  automationCurveCode,
  meterTapCode,
  panLawCode,
  panModeCode,
  sendTimingCode,
} from './codes.js';
import { entryClass, nameNativeError, refuse, refuseType } from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { stringifyWithinBudget } from './internal/list_budget.js';
import { measured } from './internal/metering_native.js';
import { assertStripMetering } from './internal/mix_scene.js';
import { isAbsent, isPlainRecord } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type {
  AutomationCurve,
  GoniometerPoint,
  MeterTap,
  MixerProcessResult,
  MixMeterSnapshot,
  MixSceneDocumentInput,
  PanLaw,
  PanMode,
  SendTiming,
  SurroundPan,
} from './public_types.js';
import type { StripMeteringOptions } from './public_types_mixing.js';
import {
  assertFloatValue,
  assertIntegerValue,
  assertSampleRate,
  assertString,
  assertSurroundPan,
  requestObject,
  requireBoolean,
  requireToken,
} from './validation.js';

export type { StripMeteringOptions } from './public_types_mixing.js';

/** A strip addressed by 0-based index or by its scene id (a digit-only id is still an id). */
export type StripRef = number | string;

/** Refuses input lists that are not arrays, by name, before comparing their lengths. */
function assertInputLists(leftInputs: unknown, rightInputs: unknown): void {
  if (!Array.isArray(leftInputs)) {
    throw refuseType('leftInputs must be an array of Float32Array');
  }
  if (!Array.isArray(rightInputs)) {
    throw refuseType('rightInputs must be an array of Float32Array');
  }
  if (leftInputs.length !== rightInputs.length) {
    throw refuse('leftInputs and rightInputs must have the same length.');
  }
}

/** Role of a bus added with {@link Mixer.addBus}. */
export type MixBusRole = 'master' | 'aux' | 'submix';

const MIX_BUS_ROLES: readonly MixBusRole[] = ['master', 'aux', 'submix'];

/**
 * Field of the meter snapshot latched by {@link Mixer.latchMeterSnapshot}, as
 * read by {@link Mixer.meterScratchValue}.
 */
export const MeterScratchField = {
  peakDbL: 0,
  peakDbR: 1,
  rmsDbL: 2,
  rmsDbR: 3,
  correlation: 4,
  truePeakDbtpL: 5,
  truePeakDbtpR: 6,
} as const;

/** A value of {@link MeterScratchField}. */
export type MeterScratchField = (typeof MeterScratchField)[keyof typeof MeterScratchField];

const METER_SCRATCH_FIELD_COUNT = Object.keys(MeterScratchField).length;

/** Construction options of a {@link Mixer}. */
export interface MixerConfig {
  /** Scene to build the mixer from; omitted gives an empty mixer. */
  scene?: MixSceneDocumentInput;
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

/** A required finite number field of an object argument. */
function requiredNumber(value: unknown, name: string): number {
  assertFloatValue(value, name);
  return value as number;
}

/** An optional finite number field; absent takes `fallback`. */
function optionalNumber(value: unknown, name: string, fallback: number): number {
  return isAbsent(value) ? fallback : requiredNumber(value, name);
}

/** A closed token: absent takes `fallback` when one is given, and the resolver refuses anything else by listing the set. */
function tokenArg(value: unknown, resolve: (token: string) => number, fallback?: string): number {
  return resolve((isAbsent(value) && fallback !== undefined ? fallback : value) as string);
}

const curveToken = (token: string): number =>
  automationCurveCode(token as AutomationCurve, 'curve');

function maxBlockSizeOf(value: unknown): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  assertIntegerValue(value, 'maxBlockSize');
  return value;
}

/** Scene JSON handed from {@link Mixer.fromJson} to the constructor; consumed there. */
let adoptedSceneJson: string | undefined;

/** Reusable WASM-heap input and output views for realtime-style mixing; see {@link Mixer.createRealtimeBuffer}. */
export interface MixerRealtimeBuffer {
  /** Per-strip left input views; fill before calling `process`. */
  leftInputs: Float32Array[];
  /** Per-strip right input views; fill before calling `process`. */
  rightInputs: Float32Array[];
  /** Left output view; read after `process`. */
  outLeft: Float32Array;
  /** Right output view; read after `process`. */
  outRight: Float32Array;
  /** Mixes the filled inputs into the output views. `numSamples` is the block length in samples. */
  process: (numSamples?: number) => void;
}

// ============================================================================
// Mixer Class (scene-based persistent mixer)
// ============================================================================

/**
 * Persistent, scene-based stereo mixer.
 *
 * Build one with `new Mixer({ scene, sampleRate })` (e.g. a scene from
 * {@link mixingScenePreset}, or none for an empty mixer) or with
 * {@link Mixer.fromJson}, then feed per-strip stereo blocks through
 * {@link processStereo} to get the routed stereo master. Strips are addressed
 * by 0-based index or by scene id ({@link StripRef}). The routing graph is
 * compiled lazily on the first {@link processStereo} call (or eagerly via
 * {@link compile}).
 *
 * Call {@link destroy} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```ts
 * const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
 *   sampleRate: 48000,
 * });
 * try {
 *   const out = mixer.processStereo([left], [right]);
 * } finally {
 *   mixer.destroy();
 * }
 * ```
 */
@entryClass('Mixer', ['processStereo', 'processStereoInto', 'mixer', 'stripIndex'])
export class Mixer {
  private mixerHandle: import('./sonare.js').WasmMixer;
  private released = false;

  private get mixer(): import('./sonare.js').WasmMixer {
    assertHandleLive(this.released, 'Mixer');
    return this.mixerHandle;
  }

  /**
   * Build a mixer from a scene document.
   *
   * A strip's meters are sized when the strip is built, so this is where their
   * configuration is chosen: an optional `metering` object on the strip
   * (`enabled` / `lufs` / `truePeak` / `truePeakOversample`) selects it, and
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
    assertSampleRate(bag.sampleRate);
    const maxBlockSize = maxBlockSizeOf(bag.maxBlockSize);
    let sceneJson = json;
    if (sceneJson === undefined) {
      if (!isAbsent(bag.scene) && !isPlainRecord(bag.scene)) {
        throw refuseType('scene must be an object');
      }
      sceneJson = stringifyWithinBudget(bag.scene ?? { strips: [], buses: [] }, 'scene');
    }
    const module = getSonareModule();
    this.mixerHandle = module.createMixerFromSceneJson(
      sceneJson,
      bag.sampleRate as number,
      maxBlockSize,
    );
  }

  /**
   * Build a mixer from a scene JSON string.
   *
   * @param document - Scene JSON (strips, buses, sends, connections, inserts)
   * @param options - Sample rate (required) and block size, see {@link MixerJsonOptions}
   * @throws TypeError for a non-string `document`, a wrong-typed field or an unknown key;
   *   RangeError for a sample rate outside `[8000, 384000]` or a `maxBlockSize` of `0` or below.
   */
  static fromJson(document: string, options: MixerJsonOptions): Mixer {
    assertString(document, 'document');
    const bag = requestObject('Mixer.fromJson', options, 'options', true);
    adoptedSceneJson = document;
    try {
      return new Mixer({
        sampleRate: bag.sampleRate,
        maxBlockSize: bag.maxBlockSize,
      } as MixerConfig);
    } finally {
      adoptedSceneJson = undefined;
    }
  }

  /** Resolve a strip reference to its index; an unknown id is a `RangeError`. */
  private stripIndex(strip: StripRef): number {
    if (typeof strip === 'string') {
      const index = this.mixer.stripById(strip);
      if (index < 0) {
        throw refuse(`no strip with id '${strip}'`);
      }
      return index;
    }
    if (typeof strip !== 'number') {
      throw refuseType('strip reference must be a number (index) or string (id)');
    }
    return strip;
  }

  /**
   * Rebuild and compile the routing graph without resetting its absolute
   * automation sample position or queued strip automation.
   */
  compile(): void {
    this.mixer.compile();
  }

  /**
   * Non-fatal warnings captured when this mixer was built from scene JSON: one
   * entry per channel-strip insert that was handed param keys it does not read
   * (a likely typo, or a key meant for a different processor). The scene still
   * loaded; these keys simply took no effect. Empty when every key was consumed.
   * Use {@link masteringInsertParamNames} to discover the keys an insert accepts.
   */
  sceneWarnings(): string[] {
    return this.mixer.sceneWarnings();
  }

  /**
   * Mix one block of per-strip stereo audio into the stereo master.
   *
   * @param leftInputs - `leftInputs[i]` is the left channel of strip `i`
   * @param rightInputs - `rightInputs[i]` is the right channel of strip `i`.
   *   Fewer inputs than strips leave the remaining strips silent.
   * @throws RangeError when more inputs are given than the mixer has strips
   * @returns Mixed stereo master (`left`, `right`, `sampleRate`)
   */
  processStereo(leftInputs: Float32Array[], rightInputs: Float32Array[]): MixerProcessResult {
    try {
      assertInputLists(leftInputs, rightInputs);
    } catch (error) {
      throw nameNativeError(error, 'Mixer.processStereo');
    }
    return this.mixer.processStereo(leftInputs, rightInputs);
  }

  /**
   * Mix one block into caller-owned output arrays.
   *
   * This avoids allocating the result object and result `Float32Array`s. It is
   * intended for realtime bridges such as AudioWorklet; all arrays must have the
   * same length. Like {@link processStereo}, fewer inputs than strips leave the
   * remaining strips silent and more than the strip count throw a `RangeError`.
   */
  processStereoInto(
    leftInputs: Float32Array[],
    rightInputs: Float32Array[],
    outLeft: Float32Array,
    outRight: Float32Array,
  ): void {
    try {
      assertInputLists(leftInputs, rightInputs);
      if (outLeft.length !== outRight.length) {
        throw refuse('outLeft and outRight must have the same length.');
      }
    } catch (error) {
      throw nameNativeError(error, 'Mixer.processStereoInto');
    }
    this.mixer.processStereoInto(leftInputs, rightInputs, outLeft, outRight);
  }

  /**
   * Create reusable WASM-heap input/output views for realtime-style processing.
   *
   * Fill `leftInputs[i]` / `rightInputs[i]`, call `process()`, then read
   * `outLeft` / `outRight`. The views are owned by this mixer and become invalid
   * after {@link destroy}.
   */
  createRealtimeBuffer(): MixerRealtimeBuffer {
    const leftInputs: Float32Array[] = [];
    const rightInputs: Float32Array[] = [];
    let outLeft = this.mixer.outputLeftView();
    let outRight = this.mixer.outputRightView();
    let acquiredStripCount = -1;

    // Every view shares one heap buffer, so a growth detaches the output views too.
    const viewsDetached = (): boolean => outLeft.byteLength === 0 || outRight.byteLength === 0;

    const acquire = (): void => {
      const stripCount = this.stripCount;
      const detached = viewsDetached();
      if (detached) {
        // A heap growth detached every view: reacquire all planes.
        leftInputs.length = 0;
        rightInputs.length = 0;
      } else {
        // Topology growth keeps existing planes in place: append views for new strips only.
        leftInputs.length = Math.min(leftInputs.length, stripCount);
        rightInputs.length = Math.min(rightInputs.length, stripCount);
      }
      for (let index = leftInputs.length; index < stripCount; index++) {
        leftInputs.push(this.mixer.inputLeftView(index));
      }
      for (let index = rightInputs.length; index < stripCount; index++) {
        rightInputs.push(this.mixer.inputRightView(index));
      }
      if (detached) {
        outLeft = this.mixer.outputLeftView();
        outRight = this.mixer.outputRightView();
      }
      acquiredStripCount = stripCount;
    };
    acquire();

    // The cached heap views can detach if WASM linear memory grows (the embind
    // module is built ALLOW_MEMORY_GROWTH). Also refresh the view list when a
    // caller adds a strip and recompiles the graph after this buffer was made.
    const acquireIfNeeded = (): void => {
      if (acquiredStripCount !== this.stripCount || viewsDetached()) {
        acquire();
      }
    };
    return {
      get leftInputs(): Float32Array[] {
        acquireIfNeeded();
        return leftInputs;
      },
      get rightInputs(): Float32Array[] {
        acquireIfNeeded();
        return rightInputs;
      },
      get outLeft(): Float32Array {
        acquireIfNeeded();
        return outLeft;
      },
      get outRight(): Float32Array {
        acquireIfNeeded();
        return outRight;
      },
      process: (numSamples?: number) => {
        acquireIfNeeded();
        // Resolve the default only after reacquiring: a detached view reports length 0.
        this.mixer.processPreparedStereo(numSamples ?? outLeft.length);
      },
    };
  }

  /**
   * Turn the master-output meter on or off.
   *
   * While on, every process and drain call meters the stereo master it just
   * produced, so a caller reads {@link meterSnapshot} instead of copying the
   * output and measuring it again. `truePeakDbtp*` is an inter-sample
   * peak taken after oversampling (ITU-R BS.1770-4 Annex 2 requires at least
   * 4x), which is a different and higher quantity than the sample peak.
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
    this.mixer.configureMeter(
      requireBoolean(bag.enabled, 'enabled'),
      bag.truePeakOversample as number | undefined,
      bag.measureLufs as boolean | undefined,
      bag.measureTruePeak as boolean | undefined,
    );
  }

  /**
   * Latest master-output meter reading, describing the most recently metered
   * block. A level with no energy is `-Infinity`; a field that was not measured is absent.
   *
   * @throws When the meter has never been enabled.
   */
  meterSnapshot(): MixMeterSnapshot {
    return this.mixer.meterSnapshot();
  }

  /**
   * Latch the latest meter reading into the mixer's internal scratch so
   * {@link meterScratchValue} can read it back one number at a time.
   *
   * This is the allocation-free form of {@link meterSnapshot}, for an audio
   * render callback that must not create a JS object per interval. It returns
   * `false` instead of throwing when the meter has never been enabled.
   *
   * @returns Whether a reading was latched.
   */
  latchMeterSnapshot(): boolean {
    return this.mixer.latchMeterSnapshot();
  }

  /**
   * Read one field of the snapshot latched by {@link latchMeterSnapshot}.
   *
   * @param field - A {@link MeterScratchField} value (`0` to `6`).
   * @returns The value; `-Infinity` for no energy, `undefined` when not measured.
   * @throws TypeError for a non-number; RangeError for a value outside `[0, 6]`.
   */
  meterScratchValue(field: MeterScratchField): number | undefined {
    assertIntegerValue(field, 'field');
    if (field < 0 || field >= METER_SCRATCH_FIELD_COUNT) {
      throw refuse(`field must be in [0, ${METER_SCRATCH_FIELD_COUNT - 1}]`);
    }
    return measured(this.mixer.meterScratchValue(field));
  }

  /** Number of strips in the mixer (e.g. strips loaded from the scene). */
  get stripCount(): number {
    return this.mixer.stripCount();
  }

  /**
   * Schedule sample-accurate insert-parameter automation on a strip's insert.
   *
   * @param strip - Strip index or id
   * @param insertIndex - Index into the strip's combined insert sequence
   *   (`[pre-inserts... post-inserts...]`)
   * @param point - `paramId` (processor-specific parameter id), `samplePos`
   *   (absolute samples from the start of processing; the mixer advances an
   *   internal position from 0 on the first {@link processStereo} call and
   *   recompiling resets it to 0), `value` (target) and `curve` (default `'linear'`)
   * @throws TypeError for a wrong-typed field or an unknown key; RangeError for
   *   an unknown strip or curve token, an out-of-range insert index, or a full
   *   event lane
   */
  scheduleInsertAutomation(
    strip: StripRef,
    insertIndex: number,
    point: InsertAutomationPoint,
  ): void {
    const fn = 'scheduleInsertAutomation';
    const bag = requestObject(fn, point, 'point', true);
    this.mixer.scheduleInsertAutomation(
      this.stripIndex(strip),
      insertIndex,
      requiredNumber(bag.paramId, 'paramId'),
      requiredNumber(bag.samplePos, 'samplePos'),
      requiredNumber(bag.value, 'value'),
      tokenArg(bag.curve, curveToken, 'linear'),
    );
  }

  /**
   * Resolve a strip's index in `[0, stripCount)` from its scene id.
   *
   * @throws RangeError when no strip has that id
   */
  stripById(id: string): number {
    assertString(id, 'id');
    const index = this.mixer.stripById(id);
    if (index < 0) {
      throw refuse(`no strip with id '${id}'`);
    }
    return index;
  }

  /**
   * Add a channel strip to the mixer topology. `metering` configures the strip's
   * pre/post taps; omitting it keeps the full default (LUFS + true peak at 4x,
   * about 1.4 MB per strip at 48 kHz). Marks the routing graph dirty; call
   * {@link compile} (or {@link processStereo}) to rebuild.
   *
   * @returns The new strip's index
   * @throws If the id is already taken; RangeError if `truePeakOversample` is not 4 or 8 (omit for the default)
   */
  addStrip(id: string, metering: StripMeteringOptions = {}): number {
    assertStripMetering(metering);
    this.mixer.addStrip(id, metering);
    return this.mixer.stripById(id);
  }

  /**
   * Add a bus to the mixer topology. `role` is a {@link MixBusRole} (defaults
   * to `'aux'`); an unknown value is a `RangeError`. Marks the routing graph
   * dirty; call {@link compile} (or {@link processStereo}) to rebuild.
   */
  addBus(id: string, role?: MixBusRole): void {
    this.mixer.addBus(id, requireToken(role, 'role', MIX_BUS_ROLES, 'aux'));
  }

  /** Remove a bus by id. Marks the routing graph dirty. */
  removeBus(id: string): void {
    this.mixer.removeBus(id);
  }

  /**
   * Re-route the strip or non-master bus `sourceId` into the bus `busId` (a
   * declared bus id or the master's resolved id; `"master"` is not an alias for
   * a master bus with another id). Every existing connection out of `sourceId`
   * is replaced, including a scene-authored strip-to-strip edge. Takes ids, not
   * the strip index the other strip methods take, because the source may be a
   * bus. Lazy like {@link addBus}: call {@link compile} (or
   * {@link processStereo}) to rebuild. An unknown id, a destination that is not
   * a bus, the master as source, a self-route or an edit that would close a
   * cycle throws a `RangeError` naming the ids and
   * leaves the topology unchanged.
   */
  setOutputBus(sourceId: string, busId: string): void {
    this.mixer.setOutputBus(sourceId, busId);
  }

  /** Number of buses in the mixer topology. */
  get busCount(): number {
    return this.mixer.busCount();
  }

  /**
   * Add a VCA group with the given gain offset (dB). `members` is a list of
   * strip ids governed by the group (may be empty).
   */
  addVcaGroup(id: string, gainDb = 0.0, members: string[] = []): void {
    this.mixer.addVcaGroup(id, gainDb, members);
  }

  /** Set an existing VCA group's gain in dB. */
  setVcaGroupGainDb(id: string, gainDb: number): void {
    this.mixer.setVcaGroupGainDb(id, gainDb);
  }

  /** Replace an existing VCA group's strip membership. */
  setVcaGroupMembers(id: string, members: string[]): void {
    this.mixer.setVcaGroupMembers(id, members);
  }

  /** Remove a VCA group by id. */
  removeVcaGroup(id: string): void {
    this.mixer.removeVcaGroup(id);
  }

  /** Number of VCA groups in the mixer topology. */
  get vcaGroupCount(): number {
    return this.mixer.vcaGroupCount();
  }

  /** Set the strip's input trim in dB. */
  setInputTrimDb(strip: StripRef, db: number): void {
    this.mixer.setInputTrimDb(this.stripIndex(strip), db);
  }

  /** Set the strip's fader level in dB. */
  setFaderDb(strip: StripRef, db: number): void {
    this.mixer.setFaderDb(this.stripIndex(strip), db);
  }

  /**
   * Set the strip's pan position.
   *
   * @param strip - Strip index or id
   * @param pan - Pan position in `[-1, 1]`
   * @param panMode - Optional pan mode. When omitted the strip's current pan
   *   mode is kept (passes `SONARE_PAN_MODE_KEEP`), so a plain pan nudge does
   *   not reset a scene-defined `'stereoPan'` / `'dualPan'` mode back to
   *   balance. Pass `'balance'` explicitly to force balance mode.
   * @throws {RangeError} when `pan` is outside `[-1, 1]` or non-finite.
   */
  setPan(strip: StripRef, pan: number, panMode?: PanMode): void {
    // SONARE_PAN_MODE_KEEP (-1) = keep the strip's current pan mode.
    const mode = panMode === undefined ? -1 : panModeCode(panMode, 'panMode');
    this.mixer.setPan(this.stripIndex(strip), pan, mode);
  }

  /**
   * Set the strip's stereo width in `[0, 2]` (0 = mono, 1 = original, 2 = widest).
   *
   * @throws {RangeError} when `width` is outside `[0, 2]` or non-finite.
   */
  setWidth(strip: StripRef, width: number): void {
    this.mixer.setWidth(this.stripIndex(strip), width);
  }

  /**
   * Snap the strip's input-trim, fader, pan and width smoothers to the values
   * already set on it, so the next processed block opens at those values
   * instead of gliding to them over the smoothing window (~5 ms).
   *
   * Call it after configuring a strip and before rendering a finite buffer: a
   * strip is smoothed for a live fader, and an offline render that does not
   * settle carries that glide as a level and image sweep across the head of
   * its output. Unlike a reset it clears nothing — automation, meters and
   * insert state are untouched.
   *
   * @param strip - Strip index or id
   *
   * @example
   * ```ts
   * mixer.setFaderDb(0, -3);
   * mixer.setPan(0, 0.3);
   * mixer.settle(0);
   * const { left, right } = mixer.processStereo([leftSamples], [rightSamples]);
   * ```
   */
  settle(strip: StripRef): void {
    this.mixer.settle(this.stripIndex(strip));
  }

  /** Set the strip's mute state. */
  setMuted(strip: StripRef, muted: boolean): void {
    this.mixer.setMuted(this.stripIndex(strip), requireBoolean(muted, 'muted'));
  }

  /**
   * Set a strip's solo state. Takes effect on the next process without a
   * graph recompile.
   */
  setSoloed(strip: StripRef, soloed: boolean): void {
    this.mixer.setSoloed(this.stripIndex(strip), requireBoolean(soloed, 'soloed'));
  }

  /**
   * Mark a strip solo-safe so it is never implied-muted by another strip's
   * solo. Takes effect on the next process without a graph recompile.
   */
  setSoloSafe(strip: StripRef, soloSafe: boolean): void {
    this.mixer.setSoloSafe(this.stripIndex(strip), requireBoolean(soloSafe, 'soloSafe'));
  }

  /**
   * Invert the polarity of the left and/or right channel of a strip.
   *
   * @throws TypeError when `left` or `right` is not a boolean, or for an unknown key
   */
  setPolarityInvert(strip: StripRef, invert: PolarityInvert): void {
    const bag = requestObject('setPolarityInvert', invert, 'invert', true);
    this.mixer.setPolarityInvert(
      this.stripIndex(strip),
      requireBoolean(bag.left, 'left'),
      requireBoolean(bag.right, 'right'),
    );
  }

  /**
   * Set the strip's pan law.
   *
   * @throws TypeError for a non-string (a number never selects a law); RangeError for an unknown name
   */
  setPanLaw(strip: StripRef, panLaw: PanLaw): void {
    this.mixer.setPanLaw(
      this.stripIndex(strip),
      tokenArg(panLaw, (token) => panLawCode(token as PanLaw, 'panLaw')),
    );
  }

  /**
   * Set a per-strip channel delay in samples, moving the strip later relative to
   * the others. It is not latency: latency compensation does not undo it.
   */
  setChannelDelaySamples(strip: StripRef, delaySamples: number): void {
    this.mixer.setChannelDelaySamples(this.stripIndex(strip), delaySamples);
  }

  /** Set the strip's direct VCA trim in dB (persisted as the scene's `vcaOffsetDb`). */
  setVcaOffsetDb(strip: StripRef, offsetDb: number): void {
    this.mixer.setVcaOffsetDb(this.stripIndex(strip), offsetDb);
  }

  /** Set independent left/right pan positions (dual-pan mode). */
  setDualPan(strip: StripRef, leftPan: number, rightPan: number): void {
    this.mixer.setDualPan(this.stripIndex(strip), leftPan, rightPan);
  }

  /**
   * Set the strip's surround pan position, used when it feeds a >2-channel bus.
   *
   * Applied when the engine's track mixer renders this strip's lane into a
   * destination with more than two channels. This stereo-only mixer's own
   * block entry points ignore it. `azimuth` outside [-180, 180] or `divergence`/`lfe`
   * outside [0, 1] throws a RangeError; a key outside those three throws a TypeError.
   */
  setSurroundPan(strip: StripRef, pan: SurroundPan): void {
    requestObject('setSurroundPan', pan, 'pan', true);
    assertSurroundPan(pan);
    this.mixer.setSurroundPan(this.stripIndex(strip), pan);
  }

  /**
   * Add a send to a strip after construction.
   *
   * @param strip - Strip index or id
   * @param send - See {@link MixerSend}; mirrors the scene document's send
   * @returns The new send's index
   * @throws TypeError for a wrong-typed field, a numeric `timing` or an unknown key;
   *   RangeError for an unknown strip or timing token
   */
  addSend(strip: StripRef, send: MixerSend): number {
    const fn = 'addSend';
    const bag = requestObject(fn, send, 'send', true);
    assertString(bag.id, 'id');
    assertString(bag.destinationBusId, 'destinationBusId');
    return this.mixer.addSend(
      this.stripIndex(strip),
      bag.id as string,
      bag.destinationBusId as string,
      optionalNumber(bag.sendDb, 'sendDb', 0),
      tokenArg(bag.timing, (token) => sendTimingCode(token as SendTiming, 'timing'), 'postFader'),
    );
  }

  /** Set the send level (in dB) for an existing send by index. */
  setSendDb(strip: StripRef, sendIndex: number, sendDb: number): void {
    this.mixer.setSendDb(this.stripIndex(strip), sendIndex, sendDb);
  }

  /**
   * Remove an existing send from a strip by index.
   *
   * Sends are addressed in add order. After removal, sends with a higher index
   * than `sendIndex` shift down by one. Recompile (or process) before reading
   * results so the routing graph rebuilds.
   *
   * @param strip - Strip index or id
   * @param sendIndex - Send index in add order
   */
  removeSend(strip: StripRef, sendIndex: number): void {
    this.mixer.removeSend(this.stripIndex(strip), sendIndex);
  }

  /**
   * Read a strip's meter snapshot.
   *
   * With no `tap` this reads the strip's post-fader meter. Pass a
   * {@link MeterTap} to read the other tap.
   *
   * @param strip - Strip index or id
   * @param tap - `'preFader'` or `'postFader'`; a number is refused with a `TypeError`
   */
  stripMeter(strip: StripRef, tap?: MeterTap): MixMeterSnapshot {
    const index = this.stripIndex(strip);
    return this.mixer.stripMeter(
      index,
      isAbsent(tap)
        ? meterTapCode('postFader', 'tap')
        : tokenArg(tap, (token) => meterTapCode(token as MeterTap, 'tap')),
    );
  }

  /** Read the post-insert meter for a compiled bus, including master. */
  busMeter(busId: string): MixMeterSnapshot {
    return this.mixer.busMeter(busId);
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
   *
   * @param strip - Strip index or id
   */
  stripNonFiniteDiscardCount(strip: StripRef): number {
    return this.mixer.stripNonFiniteDiscardCount(this.stripIndex(strip));
  }

  /**
   * Number of blocks in which a bus discarded recursive state because a
   * non-finite value had reached it. Same contract as
   * {@link stripNonFiniteDiscardCount}, for a bus: covers every insert the
   * bus owns and its meter, neither separately addressable, so a discard
   * inside one is observable only here. Cumulative across graph recompiles
   * -- the count lives with the bus, not the compiled node, so an unrelated
   * edit elsewhere in the mixer does not reset it.
   *
   * A bus's DSP record is created by the first {@link compile}. Throws for a
   * bus that has been declared with {@link addBus} but never compiled --
   * reading zero there would read as clean, and it is not. Also throws for
   * an unknown bus id.
   *
   * @param busId - Bus id, as passed to {@link addBus} or declared in scene JSON
   */
  busNonFiniteDiscardCount(busId: string): number {
    return this.mixer.busNonFiniteDiscardCount(busId);
  }

  /**
   * Schedule sample-accurate fader automation on a strip.
   *
   * @param strip - Strip index or id
   * @param point - See {@link FaderAutomationPoint}; `curve` defaults to `'linear'`
   * @throws TypeError for a wrong-typed field, a numeric `curve` or an unknown key;
   *   RangeError for an unknown strip or curve token
   */
  scheduleFaderAutomation(strip: StripRef, point: FaderAutomationPoint): void {
    const fn = 'scheduleFaderAutomation';
    const bag = requestObject(fn, point, 'point', true);
    this.mixer.scheduleFaderAutomation(
      this.stripIndex(strip),
      requiredNumber(bag.samplePos, 'samplePos'),
      requiredNumber(bag.faderDb, 'faderDb'),
      tokenArg(bag.curve, curveToken, 'linear'),
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
    this.mixer.schedulePanAutomation(
      this.stripIndex(strip),
      requiredNumber(bag.samplePos, 'samplePos'),
      requiredNumber(bag.pan, 'pan'),
      tokenArg(bag.curve, curveToken, 'linear'),
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
    this.mixer.scheduleWidthAutomation(
      this.stripIndex(strip),
      requiredNumber(bag.samplePos, 'samplePos'),
      requiredNumber(bag.width, 'width'),
      tokenArg(bag.curve, curveToken, 'linear'),
    );
  }

  /**
   * Schedule sample-accurate send-level automation on a strip.
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
    this.mixer.scheduleSendAutomation(
      this.stripIndex(strip),
      sendIndex,
      requiredNumber(bag.samplePos, 'samplePos'),
      requiredNumber(bag.sendDb, 'sendDb'),
      tokenArg(bag.curve, curveToken, 'linear'),
    );
  }

  /**
   * Read up to `maxPoints` of a strip's most recent goniometer samples
   * (oldest to newest).
   *
   * `maxPoints` must be a finite non-negative integer; anything else throws a
   * `RangeError`. It is a request rather than an allocation size —
   * a value beyond the strip's goniometer ring simply returns every point the
   * ring holds.
   */
  readGoniometerLatest(strip: StripRef, maxPoints: number): GoniometerPoint[] {
    return this.mixer.readGoniometerLatest(this.stripIndex(strip), maxPoints);
  }

  /** Serialize the current scene (strips, buses, sends, connections) to JSON. */
  toJson(): string {
    return this.mixer.toSceneJson();
  }

  /**
   * Longest audible serial processor-tail path to the master, in samples. Lazily
   * compiles the routing graph if the topology is dirty.
   */
  get tailSamples(): number {
    return this.mixer.tailSamples();
  }

  /**
   * Reported latency (samples) of the compiled mixer graph, for aligning
   * dry/wet material. Lazily compiles the routing graph if the topology is dirty.
   */
  get latencySamples(): number {
    return this.mixer.latencySamples();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.mixer.maxBlockSize();
  }

  /**
   * Drain delayed / tail audio by processing a zero-input block of `numSamples`
   * frames after the host stops feeding strip inputs. Returns the mixed stereo
   * master (`left`, `right`, `sampleRate`).
   */
  drainTailStereo(numSamples: number): MixerProcessResult {
    return this.mixer.drainTailStereo(numSamples);
  }

  /**
   * Marks the end of the signal for every strip, bus and master-output meter
   * (see {@link configureMeter}). The true-peak reading runs one
   * reconstruction-filter group delay behind the block; this reads that pending
   * stretch against silence and folds it into the snapshots, so `truePeakDbtp`
   * covers the whole signal. Call it after the last
   * block of an offline render; a later process call starts a new signal.
   */
  flushMeters(): void {
    this.mixer.flushMeters();
  }

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.mixerHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
