import {
  automationCurveCode,
  meterTapCode,
  panLawCode,
  panModeCode,
  sendTimingCode,
} from './codes';
import { getSonareModule } from './module_state';
import type {
  AutomationCurve,
  GoniometerPoint,
  MeterTap,
  MixMeterSnapshot,
  MixerProcessResult,
  PanLaw,
  PanMode,
  SendTiming,
} from './public_types';

export interface MixerRealtimeBuffer {
  leftInputs: Float32Array[];
  rightInputs: Float32Array[];
  outLeft: Float32Array;
  outRight: Float32Array;
  process: (numSamples?: number) => void;
}

// ============================================================================
// Mixer Class (scene-based persistent mixer)
// ============================================================================

/**
 * Persistent, scene-based stereo mixer.
 *
 * Build one from a scene JSON string (e.g. {@link mixingScenePresetJson} or a
 * hand-authored scene), then feed per-strip stereo blocks through
 * {@link processStereo} to get the routed stereo master. Strips, sends, buses,
 * and inserts are described entirely by the scene; the routing graph is
 * compiled lazily on the first {@link processStereo} call (or eagerly via
 * {@link compile}).
 *
 * Call {@link delete} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const mixer = Mixer.fromSceneJson(mixingScenePresetJson('basicStereo'), 48000, 512);
 * try {
 *   const out = mixer.processStereo([stripL], [stripR]);
 * } finally {
 *   mixer.delete();
 * }
 * ```
 */
export class Mixer {
  private mixer: import('./sonare.js').WasmMixer;

  private constructor(mixer: import('./sonare.js').WasmMixer) {
    this.mixer = mixer;
  }

  /**
   * Build a mixer from a scene JSON string.
   *
   * @param json - Scene JSON (strips, buses, sends, connections, inserts)
   * @param sampleRate - Sample rate in Hz (default: 48000)
   * @param blockSize - Maximum block size per {@link processStereo} call (default: 512)
   */
  static fromSceneJson(json: string, sampleRate = 48000, blockSize = 512): Mixer {
    const module = getSonareModule();
    return new Mixer(module.createMixerFromSceneJson(json, sampleRate, blockSize));
  }

  /** Rebuild and compile the routing graph from the current scene topology. */
  compile(): void {
    this.mixer.compile();
  }

  /**
   * Mix one block of per-strip stereo audio into the stereo master.
   *
   * @param leftChannels - `leftChannels[i]` is the left channel of strip `i`
   * @param rightChannels - `rightChannels[i]` is the right channel of strip `i`
   * @returns Mixed stereo master (`left`, `right`, `sampleRate`)
   */
  processStereo(leftChannels: Float32Array[], rightChannels: Float32Array[]): MixerProcessResult {
    if (leftChannels.length !== rightChannels.length) {
      throw new Error('leftChannels and rightChannels must have the same length.');
    }
    return this.mixer.processStereo(leftChannels, rightChannels);
  }

  /**
   * Mix one block into caller-owned output arrays.
   *
   * This avoids allocating the result object and result `Float32Array`s. It is
   * intended for realtime bridges such as AudioWorklet; the input channel count
   * must match the scene strip count and all arrays must have the same length.
   */
  processStereoInto(
    leftChannels: Float32Array[],
    rightChannels: Float32Array[],
    outLeft: Float32Array,
    outRight: Float32Array,
  ): void {
    if (leftChannels.length !== rightChannels.length) {
      throw new Error('leftChannels and rightChannels must have the same length.');
    }
    if (outLeft.length !== outRight.length) {
      throw new Error('outLeft and outRight must have the same length.');
    }
    this.mixer.processStereoInto(leftChannels, rightChannels, outLeft, outRight);
  }

  /**
   * Create reusable WASM-heap input/output views for realtime-style processing.
   *
   * Fill `leftInputs[i]` / `rightInputs[i]`, call `process()`, then read
   * `outLeft` / `outRight`. The views are owned by this mixer and become invalid
   * after {@link delete}.
   */
  createRealtimeBuffer(): MixerRealtimeBuffer {
    const stripCount = this.stripCount();
    let leftInputs: Float32Array[] = [];
    let rightInputs: Float32Array[] = [];
    let outLeft = this.mixer.outputLeftView();
    let outRight = this.mixer.outputRightView();
    const acquire = (): void => {
      leftInputs = [];
      rightInputs = [];
      for (let index = 0; index < stripCount; index++) {
        leftInputs.push(this.mixer.inputLeftView(index));
        rightInputs.push(this.mixer.inputRightView(index));
      }
      outLeft = this.mixer.outputLeftView();
      outRight = this.mixer.outputRightView();
    };
    acquire();
    // The cached heap views can detach if WASM linear memory grows (the embind
    // module is built ALLOW_MEMORY_GROWTH). Re-acquire them if detached
    // (byteLength === 0) before use, mirroring the worklet RT path.
    const reacquireIfDetached = (): void => {
      if (outLeft.byteLength === 0 || (leftInputs[0]?.byteLength ?? 1) === 0) {
        acquire();
      }
    };
    return {
      get leftInputs(): Float32Array[] {
        reacquireIfDetached();
        return leftInputs;
      },
      get rightInputs(): Float32Array[] {
        reacquireIfDetached();
        return rightInputs;
      },
      get outLeft(): Float32Array {
        reacquireIfDetached();
        return outLeft;
      },
      get outRight(): Float32Array {
        reacquireIfDetached();
        return outRight;
      },
      process: (numSamples = outLeft.length) => {
        reacquireIfDetached();
        this.mixer.processPreparedStereo(numSamples);
      },
    };
  }

  /** Number of strips in the mixer (e.g. strips loaded from the scene). */
  stripCount(): number {
    return this.mixer.stripCount();
  }

  /**
   * Schedule sample-accurate insert-parameter automation on a strip's insert.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param insertIndex - Index into the strip's combined insert sequence
   *   (`[pre-inserts... post-inserts...]`)
   * @param paramId - Processor-specific parameter id
   * @param samplePos - Absolute samples from the start of processing (the mixer
   *   advances an internal position from 0 on the first {@link processStereo}
   *   call; recompiling resets it to 0)
   * @param value - Target parameter value
   * @param curve - Interpolation curve (default: `'linear'`)
   * @throws If the strip index is out of range or the schedule call fails
   *   (unknown curve, out-of-range insert index, or full event lane)
   */
  scheduleInsertAutomation(
    stripIndex: number,
    insertIndex: number,
    paramId: number,
    samplePos: number,
    value: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleInsertAutomation(
      stripIndex,
      insertIndex,
      paramId,
      samplePos,
      value,
      automationCurveCode(curve),
    );
  }

  /**
   * Resolve a strip's index in `[0, stripCount())` from its scene id, or `null`
   * when no strip with that id exists (matches the Node binding's `number | null`).
   */
  stripById(id: string): number | null {
    const index = this.mixer.stripById(id);
    return index < 0 ? null : index;
  }

  /**
   * Add a bus to the mixer topology. `role` is one of `'master'`, `'aux'`, or
   * `'submix'` (defaults to `'aux'`). Marks the routing graph dirty; call
   * {@link compile} (or {@link processStereo}) to rebuild.
   */
  addBus(id: string, role = 'aux'): void {
    this.mixer.addBus(id, role);
  }

  /** Remove a bus by id. Marks the routing graph dirty. */
  removeBus(id: string): void {
    this.mixer.removeBus(id);
  }

  /** Number of buses in the mixer topology. */
  busCount(): number {
    return this.mixer.busCount();
  }

  /**
   * Add a VCA group with the given gain offset (dB). `members` is a list of
   * strip ids governed by the group (may be empty).
   */
  addVcaGroup(id: string, gainDb = 0.0, members: string[] = []): void {
    this.mixer.addVcaGroup(id, gainDb, members);
  }

  /** Remove a VCA group by id. */
  removeVcaGroup(id: string): void {
    this.mixer.removeVcaGroup(id);
  }

  /** Number of VCA groups in the mixer topology. */
  vcaGroupCount(): number {
    return this.mixer.vcaGroupCount();
  }

  /** Set the strip's input trim in dB. */
  setInputTrimDb(stripIndex: number, db: number): void {
    this.mixer.setInputTrimDb(stripIndex, db);
  }

  /** Set the strip's fader level in dB. */
  setFaderDb(stripIndex: number, db: number): void {
    this.mixer.setFaderDb(stripIndex, db);
  }

  /**
   * Set the strip's pan position.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param pan - Pan position in `[-1, 1]`
   * @param panMode - Optional pan mode. When omitted the strip's current pan
   *   mode is kept (passes `SONARE_PAN_MODE_KEEP`), so a plain pan nudge does
   *   not reset a scene-defined `'stereoPan'` / `'dualPan'` mode back to
   *   balance. Pass `'balance'` (or `0`) explicitly to force balance mode.
   */
  setPan(stripIndex: number, pan: number, panMode?: PanMode | number): void {
    // SONARE_PAN_MODE_KEEP (-1) = keep the strip's current pan mode.
    const mode = panMode === undefined ? -1 : panModeCode(panMode);
    this.mixer.setPan(stripIndex, pan, mode);
  }

  /** Set the strip's stereo width. */
  setWidth(stripIndex: number, width: number): void {
    this.mixer.setWidth(stripIndex, width);
  }

  /** Set the strip's mute state. */
  setMuted(stripIndex: number, muted: boolean): void {
    this.mixer.setMuted(stripIndex, muted);
  }

  /**
   * Set a strip's solo state. Takes effect on the next process without a
   * graph recompile.
   */
  setSoloed(stripIndex: number, soloed: boolean): void {
    this.mixer.setSoloed(stripIndex, soloed);
  }

  /**
   * Mark a strip solo-safe so it is never implied-muted by another strip's
   * solo. Takes effect on the next process without a graph recompile.
   */
  setSoloSafe(stripIndex: number, soloSafe: boolean): void {
    this.mixer.setSoloSafe(stripIndex, soloSafe);
  }

  /** Invert the polarity of the left and/or right channel of a strip. */
  setPolarityInvert(stripIndex: number, invertLeft: boolean, invertRight: boolean): void {
    this.mixer.setPolarityInvert(stripIndex, invertLeft, invertRight);
  }

  /** Set the strip's pan law. */
  setPanLaw(stripIndex: number, panLaw: PanLaw | number): void {
    this.mixer.setPanLaw(stripIndex, panLawCode(panLaw));
  }

  /**
   * Set a per-strip channel delay in samples. This changes the strip's reported
   * latency; recompile to re-run latency compensation.
   */
  setChannelDelaySamples(stripIndex: number, delaySamples: number): void {
    this.mixer.setChannelDelaySamples(stripIndex, delaySamples);
  }

  /** Set the strip's live VCA gain offset in dB (not persisted to the scene). */
  setVcaOffsetDb(stripIndex: number, offsetDb: number): void {
    this.mixer.setVcaOffsetDb(stripIndex, offsetDb);
  }

  /** Set independent left/right pan positions (dual-pan mode). */
  setDualPan(stripIndex: number, leftPan: number, rightPan: number): void {
    this.mixer.setDualPan(stripIndex, leftPan, rightPan);
  }

  /**
   * Add a send to a strip after construction.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param id - Send id
   * @param destinationBusId - Destination bus id
   * @param sendDb - Initial send level in dB
   * @param timing - `'preFader'` or `'postFader'` (default: `'postFader'`)
   * @returns The new send's index
   */
  addSend(
    stripIndex: number,
    id: string,
    destinationBusId: string,
    sendDb: number,
    timing: SendTiming | number = 'postFader',
  ): number {
    return this.mixer.addSend(stripIndex, id, destinationBusId, sendDb, sendTimingCode(timing));
  }

  /** Set the send level (in dB) for an existing send by index. */
  setSendDb(stripIndex: number, sendIndex: number, sendDb: number): void {
    this.mixer.setSendDb(stripIndex, sendIndex, sendDb);
  }

  /**
   * Remove an existing send from a strip by index.
   *
   * Sends are addressed in add order. After removal, sends with a higher index
   * than `sendIndex` shift down by one. Recompile (or process) before reading
   * results so the routing graph rebuilds.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param sendIndex - Send index in add order
   */
  removeSend(stripIndex: number, sendIndex: number): void {
    this.mixer.removeSend(stripIndex, sendIndex);
  }

  /**
   * Read a strip's meter snapshot at the given tap point.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param tap - `'preFader'` or `'postFader'` (default: `'postFader'`)
   */
  meterTap(stripIndex: number, tap: MeterTap = 'postFader'): MixMeterSnapshot {
    return this.mixer.meterTap(stripIndex, meterTapCode(tap));
  }

  /**
   * Read a strip's meter snapshot.
   *
   * With no `tap` argument this reads the strip's own (post-fader) meter,
   * matching the Node/Python tap-less `stripMeter` contract. Pass an optional
   * `tap` (`'preFader'` / `'postFader'`) to read the tap-selectable snapshot
   * instead — the same backing call as {@link meterTap}.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param tap - Optional tap point (`'preFader'` / `'postFader'`); when omitted
   *   the tap-less post-fader strip meter is read.
   */
  stripMeter(stripIndex: number, tap?: MeterTap | number): MixMeterSnapshot {
    if (tap === undefined) {
      return this.mixer.stripMeter(stripIndex);
    }
    return this.mixer.meterTap(stripIndex, meterTapCode(tap));
  }

  /**
   * Schedule sample-accurate fader automation on a strip.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param samplePos - Absolute samples from the start of processing
   * @param faderDb - Target fader level in dB
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  scheduleFaderAutomation(
    stripIndex: number,
    samplePos: number,
    faderDb: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleFaderAutomation(stripIndex, samplePos, faderDb, automationCurveCode(curve));
  }

  /**
   * Schedule sample-accurate pan automation on a strip.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param samplePos - Absolute samples from the start of processing
   * @param pan - Target pan position
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  schedulePanAutomation(
    stripIndex: number,
    samplePos: number,
    pan: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.schedulePanAutomation(stripIndex, samplePos, pan, automationCurveCode(curve));
  }

  /**
   * Schedule sample-accurate width automation on a strip.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param samplePos - Absolute samples from the start of processing
   * @param width - Target stereo width
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  scheduleWidthAutomation(
    stripIndex: number,
    samplePos: number,
    width: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleWidthAutomation(stripIndex, samplePos, width, automationCurveCode(curve));
  }

  /**
   * Schedule sample-accurate send-level automation on a strip's send.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param sendIndex - Send index in the strip's add order
   * @param samplePos - Absolute samples from the start of processing
   * @param db - Target send level in dB
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  scheduleSendAutomation(
    stripIndex: number,
    sendIndex: number,
    samplePos: number,
    db: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleSendAutomation(
      stripIndex,
      sendIndex,
      samplePos,
      db,
      automationCurveCode(curve),
    );
  }

  /**
   * Read up to `maxPoints` of a strip's most recent goniometer samples
   * (oldest to newest).
   */
  readGoniometerLatest(stripIndex: number, maxPoints: number): GoniometerPoint[] {
    return this.mixer.readGoniometerLatest(stripIndex, maxPoints);
  }

  /** Serialize the current scene (strips, buses, sends, connections) to JSON. */
  toSceneJson(): string {
    return this.mixer.toSceneJson();
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.mixer.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}
