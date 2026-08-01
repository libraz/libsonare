import { addon } from './native.js';
import type {
  AutomationCurve,
  Capabilities,
  CapabilityCatalog,
  GoniometerPoint,
  MeterTap,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  PanLaw,
  PanMode,
  SendTiming,
  StripRef,
  SurroundPan,
} from './types.js';
import { assertFiniteScalar } from './validation.js';
import {
  automationCurveValue,
  meterTapValue,
  panLawValue,
  panModeValue,
  sendTimingValue,
} from './value_coercion.js';

export function version(): string {
  return addon.version();
}

/**
 * Aggregate native ABI version: the per-subsystem ABI macros folded into one
 * 32-bit value. It bumps whenever any flat C POD layout changes, so callers can
 * detect an incompatible prebuilt native binary.
 */
export function abiVersion(): number {
  return addon.abiVersion();
}

/**
 * Returns the build and runtime capabilities of the loaded native library.
 *
 * The native binding parses the canonical C ABI JSON before returning this
 * object, so callers receive a synchronous typed value rather than JSON text.
 */
export function capabilities(): Capabilities {
  return addon.capabilities() as Capabilities;
}

/**
 * Return the loaded native library's processors, parameter descriptors, and
 * built-in presets as one machine-readable catalog.
 */
export function capabilityCatalog(): CapabilityCatalog {
  return JSON.parse(addon.capabilityCatalog()) as CapabilityCatalog;
}

/**
 * Returns whether the loaded native binding was compiled with FFmpeg support.
 *
 * When `true`, `Audio.fromFile` / `Audio.fromMemory` can decode M4A, AAC,
 * FLAC, OGG, Opus, etc. (anything libavformat handles). When `false`, only
 * WAV and MP3 are supported and other formats throw an actionable error.
 */
export function hasFfmpegSupport(): boolean {
  return addon.hasFfmpegSupport();
}

export function scaleQuantizeMidi(
  root: number,
  modeMask: number,
  midi: number,
  referenceMidi = 0,
): number {
  assertFiniteScalar('scaleQuantizeMidi', midi, 'midi');
  assertFiniteScalar('scaleQuantizeMidi', referenceMidi, 'referenceMidi');
  return addon.scaleQuantizeMidi(root, modeMask, midi, referenceMidi);
}

export function scaleCorrectionSemitones(
  root: number,
  modeMask: number,
  midi: number,
  referenceMidi = 0,
): number {
  assertFiniteScalar('scaleCorrectionSemitones', midi, 'midi');
  assertFiniteScalar('scaleCorrectionSemitones', referenceMidi, 'referenceMidi');
  return addon.scaleCorrectionSemitones(root, modeMask, midi, referenceMidi);
}

export function scalePitchClassEnabled(
  root: number,
  modeMask: number,
  pitchClass: number,
): boolean {
  return addon.scalePitchClassEnabled(root, modeMask, pitchClass);
}

/** Inputs for the one-shot {@link resample} facade. */
export interface ResampleRequest {
  samples: Float32Array;
  srcSr: number;
  targetSr: number;
}

export function resample(request: ResampleRequest): Float32Array;
export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array;
export function resample(
  samples: Float32Array | ResampleRequest,
  srcSr?: number,
  targetSr?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, srcSr, targetSr } : samples;
  return addon.resample(request.samples, request.srcSr, request.targetSr);
}

export function mixingScenePresetNames(): string[] {
  return addon.mixingScenePresetNames();
}

export function mixingScenePresetJson(presetName: string): string {
  return addon.mixingScenePresetJson(presetName);
}

/**
 * Scene-based persistent stereo mixer. Built from a scene JSON string, it routes
 * per-strip stereo blocks through a compiled routing graph (sends, buses,
 * inserts) into a stereo master. Strips are addressed by 0-based index or by
 * their string id; the underlying strip handles are never exposed.
 */
export class Mixer {
  private native: InstanceType<typeof addon.Mixer>;
  private disposed = false;

  private constructor(native: InstanceType<typeof addon.Mixer>) {
    this.native = native;
  }

  /** Build a mixer from a scene JSON string (see {@link mixingScenePresetJson}). */
  static fromSceneJson(json: string, sampleRate = 48000, blockSize = 512): Mixer {
    return new Mixer(new addon.Mixer(json, sampleRate, blockSize));
  }

  /**
   * Rebuild and compile the routing graph without resetting its absolute
   * automation sample position or queued strip automation.
   */
  compile(): void {
    this.native.compile();
  }

  /** Number of strips in the mixer. */
  stripCount(): number {
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
  tailSamples(): number {
    return this.native.tailSamples();
  }

  /** Reported latency (samples) of the compiled graph, for aligning dry/wet material. */
  latencySamples(): number {
    return this.native.latencySamples();
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
   * Mix one block of per-strip stereo audio into the stereo master.
   *
   * @param leftChannels - `leftChannels[i]` is the left channel of strip `i`
   * @param rightChannels - `rightChannels[i]` is the right channel of strip `i`
   */
  processStereo(leftChannels: Float32Array[], rightChannels: Float32Array[]): MixerProcessResult {
    if (leftChannels.length !== rightChannels.length) {
      throw new Error('leftChannels and rightChannels must have the same length.');
    }
    return this.native.processStereo(leftChannels, rightChannels);
  }

  /**
   * Schedule a sample-accurate insert-parameter automation event.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param insertIndex - Index into the strip's combined [pre... post...] inserts
   * @param paramId - Processor-specific parameter id
   * @param samplePos - Absolute sample position from the start of processing
   * @param value - Target parameter value
   * @param curve - Interpolation curve toward the value (default `'linear'`)
   */
  scheduleInsertAutomation(
    stripIndex: number,
    insertIndex: number,
    paramId: number,
    samplePos: number,
    value: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleInsertAutomation(
      stripIndex,
      insertIndex,
      paramId,
      samplePos,
      value,
      automationCurveValue(curve),
    );
  }

  /** Resolve a strip id to its 0-based index, or `null` if not found. */
  stripById(id: string): number | null {
    return this.native.stripById(id);
  }

  /**
   * Add a bus to the mixer topology.
   *
   * @param id - Unique bus id
   * @param role - Bus role (`'master'` | `'aux'` | `'submix'`); defaults to `'aux'`
   *
   * Marks the routing graph dirty; call {@link compile} (or process) to rebuild.
   */
  addBus(id: string, role?: 'master' | 'aux' | 'submix' | string): void {
    this.native.addBus(id, role);
  }

  /** Remove a bus by id. */
  removeBus(id: string): void {
    this.native.removeBus(id);
  }

  /** Number of buses in the mixer topology. */
  busCount(): number {
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
  vcaGroupCount(): number {
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
   */
  setPan(strip: StripRef, pan: number, panMode?: PanMode): void {
    if (panMode === undefined) {
      this.native.setPan(strip, pan);
    } else {
      this.native.setPan(strip, pan, panModeValue(panMode));
    }
  }

  /** Set a strip's stereo width (0 = mono, 1 = original, >1 = widened). */
  setWidth(strip: StripRef, width: number): void {
    this.native.setWidth(strip, width);
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

  /** Invert the polarity of a strip's left and/or right channel. */
  setPolarityInvert(strip: StripRef, invertLeft: boolean, invertRight: boolean): void {
    this.native.setPolarityInvert(strip, invertLeft, invertRight);
  }

  /** Set a strip's pan law (`'const3dB'` | `'const4.5dB'` | `'const6dB'` | `'linear0dB'`). */
  setPanLaw(strip: StripRef, panLaw: PanLaw | number): void {
    this.native.setPanLaw(strip, panLawValue(panLaw));
  }

  /** Set a per-strip channel delay in samples (recompiled at the next {@link compile}). */
  setChannelDelaySamples(strip: StripRef, delaySamples: number): void {
    this.native.setChannelDelaySamples(strip, delaySamples);
  }

  /** Set a strip's live VCA gain offset in dB (not persisted to the scene JSON). */
  setVcaOffsetDb(strip: StripRef, offsetDb: number): void {
    this.native.setVcaOffsetDb(strip, offsetDb);
  }

  /** Set a strip's independent left/right pan positions (dual-pan mode). */
  setDualPan(strip: StripRef, leftPan: number, rightPan: number): void {
    this.native.setDualPan(strip, leftPan, rightPan);
  }

  /**
   * Set a strip's surround pan position, used when the strip feeds a >2-channel
   * bus. Stored on the scene; inert until the surround DSP path applies it.
   */
  setSurroundPan(strip: StripRef, pan: SurroundPan): void {
    this.native.setSurroundPan(strip, pan);
  }

  /**
   * Add a post-construction send from a strip to a destination bus.
   *
   * @returns The 0-based index of the new send (use with {@link setSendDb} /
   *   {@link scheduleSendAutomation}).
   */
  addSend(
    strip: StripRef,
    sendId: string,
    destinationBusId: string,
    sendDb = 0.0,
    timing: SendTiming | number = 'postFader',
  ): number {
    return this.native.addSend(strip, sendId, destinationBusId, sendDb, sendTimingValue(timing));
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
   * Read a strip's current meter snapshot. With no `tap` (or `'postFader'`)
   * this returns the post-fader meter; pass `'preFader'` (or the enum int) to
   * read the pre-fader tap instead.
   */
  stripMeter(strip: StripRef, tap?: MeterTap | number): MixMeterSnapshot {
    if (tap === undefined) {
      return this.native.stripMeter(strip);
    }
    return this.native.meterTap(strip, meterTapValue(tap));
  }

  /** Read the post-insert meter for a compiled bus, including the master bus. */
  busMeter(busId: string): MixMeterSnapshot {
    return this.native.busMeter(busId);
  }

  /** Read a strip's meter snapshot at the given tap point (`'preFader'` | `'postFader'`). */
  meterTap(strip: StripRef, tap: MeterTap | number = 'postFader'): MixMeterSnapshot {
    return this.native.meterTap(strip, meterTapValue(tap));
  }

  /** Read up to `maxPoints` of the latest goniometer samples for a strip. */
  readGoniometerLatest(strip: StripRef, maxPoints: number): GoniometerPoint[] {
    return this.native.readGoniometerLatest(strip, maxPoints);
  }

  /** Schedule sample-accurate fader (dB) automation on a strip. */
  scheduleFaderAutomation(
    strip: StripRef,
    samplePos: number,
    faderDb: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleFaderAutomation(strip, samplePos, faderDb, automationCurveValue(curve));
  }

  /** Schedule sample-accurate pan automation on a strip. */
  schedulePanAutomation(
    strip: StripRef,
    samplePos: number,
    pan: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.schedulePanAutomation(strip, samplePos, pan, automationCurveValue(curve));
  }

  /** Schedule sample-accurate width automation on a strip. */
  scheduleWidthAutomation(
    strip: StripRef,
    samplePos: number,
    width: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleWidthAutomation(strip, samplePos, width, automationCurveValue(curve));
  }

  /** Schedule sample-accurate send-level (dB) automation on a strip's send. */
  scheduleSendAutomation(
    strip: StripRef,
    sendIndex: number,
    samplePos: number,
    db: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleSendAutomation(
      strip,
      sendIndex,
      samplePos,
      db,
      automationCurveValue(curve),
    );
  }

  /** Serialize the current scene (strips, buses, sends, connections) to JSON. */
  toSceneJson(): string {
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

  /** Alias for {@link destroy}, provided for cross-binding (WASM) compatibility. */
  delete(): void {
    this.destroy();
  }

  /** Releases the native mixer; lets `using` (Node 22+) free it automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/**
 * One-shot stereo mix of multiple input strips down to a single stereo bus.
 *
 * The returned `meters` array carries a per-strip {@link MixMeterSnapshot}.
 * Note that the integrating fields (`momentaryLufs`, `shortTermLufs`,
 * `integratedLufs`, `truePeakDbL`/`truePeakDbR`) require sustained streaming to
 * converge; on a short one-shot mix they have not accumulated enough signal and
 * read the -120 dB floor sentinel. Use the streaming {@link Mixer} for
 * meaningful loudness/true-peak readings.
 */
/** Inputs for the one-shot {@link mixStereo} facade. */
export interface MixStereoRequest extends MixOptions {
  leftChannels: Float32Array[];
  rightChannels: Float32Array[];
  sampleRate?: number;
}

export function mixStereo(request: MixStereoRequest): MixResult;
export function mixStereo(
  leftChannels: Float32Array[],
  rightChannels: Float32Array[],
  sampleRate?: number,
  options?: MixOptions,
): MixResult;
export function mixStereo(
  leftChannels: Float32Array[] | MixStereoRequest,
  rightChannels?: Float32Array[],
  sampleRate = 48000,
  options: MixOptions = {},
): MixResult {
  const request = Array.isArray(leftChannels)
    ? { leftChannels, rightChannels: rightChannels as Float32Array[], sampleRate, ...options }
    : leftChannels;
  return addon.mixStereo(
    request.leftChannels,
    request.rightChannels,
    request.sampleRate ?? 48000,
    request,
  );
}
