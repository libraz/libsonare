import { closeSync, openSync, readSync } from 'node:fs';
import { addon } from './native.js';
import type {
  BuiltinSynthConfig,
  ClipPageRequest,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineExternalMidiEvent,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMeterTelemetryWide,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineScopeTelemetry,
  EngineTelemetry,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTransportState,
  EqBandInput,
  FileClipPageProviderOptions,
  MidiCcBindOptions,
  PanLawInput,
  PanMode,
  ProjectMidiCcBinding,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  Sf2InstrumentConfig,
  SynthPatch,
} from './types.js';
import {
  engineAutomationPointValue,
  panLawValue,
  panModeValue,
  sendTimingValue,
  trackMonitorModeValue,
} from './value_coercion.js';

export class RealtimeEngine {
  private native: InstanceType<typeof addon.RealtimeEngine>;
  private disposed = false;

  constructor(
    sampleRate = 48000,
    maxBlockSize = 128,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
    maxChannels = 64,
  ) {
    this.native = new addon.RealtimeEngine(
      sampleRate,
      maxBlockSize,
      commandCapacity,
      telemetryCapacity,
      maxChannels,
    );
  }

  prepare(
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
    maxChannels = 64,
  ): void {
    this.native.prepare(sampleRate, maxBlockSize, commandCapacity, telemetryCapacity, maxChannels);
  }

  play(renderFrame = -1): void {
    this.native.play(renderFrame);
  }

  stop(renderFrame = -1): void {
    this.native.stop(renderFrame);
  }

  seekSample(timelineSample: number, renderFrame = -1): void {
    this.native.seekSample(timelineSample, renderFrame);
  }

  /**
   * Snaps every in-flight parameter ramp (engine-level smoothed params, mixer
   * lane fader/pan/gate, bus gains) to its target value. Offline renders call
   * this after a priming process() block so the first audible block renders at
   * settled values instead of ramping in from defaults.
   */
  settleParameters(): void {
    this.native.settleParameters();
  }

  /**
   * Applies commands queued on an offline/control-only engine immediately.
   * A host that never calls {@link process} still queues onto a bounded
   * realtime command ring, so those commands have to be drained explicitly
   * rather than by the next process() call. Not safe to call concurrently with
   * a running process().
   */
  flushControlCommands(): void {
    this.native.flushControlCommands();
  }

  seekPpq(ppq: number, renderFrame = -1): void {
    this.native.seekPpq(ppq, renderFrame);
  }

  /** Set a finite tempo in the range (0, 100000] BPM. */
  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
  }

  /**
   * Installs a tempo map from ramp segments. Each segment needs a finite
   * non-negative `startPpq` and a `bpm` in (0, 100000]; a non-zero `endBpm`
   * uses the same range. An empty array clears the map to the single tempo.
   */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    this.native.setTempoSegments(segments);
  }

  /**
   * Installs a time-signature map. Each segment needs a finite non-negative
   * `startPpq` and a positive `numerator` / `denominator`.
   */
  setTimeSignatureSegments(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    this.native.setTimeSignatureSegments(segments);
  }

  sampleAtPpq(ppq: number): number {
    return this.native.sampleAtPpq(ppq);
  }

  setLoop(startPpq: number, endPpq: number, enabled = true): void {
    this.native.setLoop(startPpq, endPpq, enabled);
  }

  addParameter(info: EngineParameterInfo): void {
    this.native.addParameter(info);
  }

  parameterCount(): number {
    return this.native.parameterCount();
  }

  parameterInfoByIndex(index: number): EngineParameterInfo {
    return this.native.parameterInfoByIndex(index);
  }

  parameterInfo(id: number): EngineParameterInfo {
    return this.native.parameterInfo(id);
  }

  setAutomationLane(paramId: number, points: EngineAutomationPoint[]): void {
    this.native.setAutomationLane(paramId, points.map(engineAutomationPointValue));
  }

  automationLaneCount(): number {
    return this.native.automationLaneCount();
  }

  setMarkers(markers: EngineMarker[]): void {
    this.native.setMarkers(markers);
  }

  markerCount(): number {
    return this.native.markerCount();
  }

  markerByIndex(index: number): EngineMarker {
    return this.native.markerByIndex(index);
  }

  marker(id: number): EngineMarker {
    return this.native.marker(id);
  }

  seekMarker(markerId: number, renderFrame = -1): void {
    this.native.seekMarker(markerId, renderFrame);
  }

  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    this.native.setLoopFromMarkers(startMarkerId, endMarkerId);
  }

  /** Set a metronome config; click lengths are limited to one second. */
  setMetronome(config: EngineMetronomeConfig): void {
    this.native.setMetronome(config);
  }

  metronome(): Required<EngineMetronomeConfig> {
    return this.native.metronome();
  }

  countInEndSample(startSample: number, bars: number): number {
    return this.native.countInEndSample(startSample, bars);
  }

  createClipPageProvider(
    numChannels: number,
    numSamples: number,
    pageFrames: number,
  ): ClipPageProvider {
    const id = this.native.createClipPageProvider(numChannels, numSamples, pageFrames);
    return new ClipPageProvider(this, id);
  }

  createFileClipPageProvider(
    path: string,
    options: FileClipPageProviderOptions,
  ): FileClipPageProvider {
    const id = this.native.createClipPageProvider(
      options.numChannels,
      options.numSamples,
      options.pageFrames,
    );
    try {
      return new FileClipPageProvider(this, id, path, options);
    } catch (err) {
      this.native.destroyClipPageProvider(id);
      throw err;
    }
  }

  setClips(clips: EngineClip[]): void {
    this.native.setClips(
      clips.map((clip) => ({
        ...clip,
        pageProvider:
          typeof clip.pageProvider === 'object' && clip.pageProvider !== null
            ? clip.pageProvider.id
            : clip.pageProvider,
      })),
    );
  }

  clipCount(): number {
    return this.native.clipCount();
  }

  setTrackLanes(lanes: Array<number | EngineTrackLane>): void {
    this.native.setTrackLanes(
      lanes.map((lane) => {
        if (typeof lane === 'number') {
          return { trackId: lane };
        }
        if (lane.sends === undefined) {
          return lane;
        }
        return {
          ...lane,
          sends: lane.sends.map((send) =>
            send.sendTiming === undefined
              ? send
              : { ...send, sendTiming: sendTimingValue(send.sendTiming) },
          ),
        };
      }),
    );
  }

  setTrackBuses(buses: EngineBus[]): void {
    this.native.setTrackBuses(buses);
  }

  /**
   * Keys one insert of a lane strip from another lane's post-strip audio
   * (ducking/sidechainRouter inserts). sourceTrackId 0 removes the binding.
   */
  setLaneSidechain(trackId: number, insertIndex: number, sourceTrackId: number): void {
    this.native.setLaneSidechain(trackId, insertIndex, sourceTrackId);
  }

  setBusStripJson(busId: number, sceneJson: string): void {
    this.native.setBusStripJson(busId, sceneJson);
  }

  setTrackStripJson(trackId: number, sceneJson: string): void {
    this.native.setTrackStripJson(trackId, sceneJson);
  }

  setTrackStripEqBand(trackId: number, bandIndex: number, band: EqBandInput | string): void {
    this.native.setTrackStripEqBandJson(
      trackId,
      bandIndex,
      typeof band === 'string' ? band : JSON.stringify(band),
    );
  }

  setTrackStripEqBandJson(trackId: number, bandIndex: number, bandJson: string): void {
    this.native.setTrackStripEqBandJson(trackId, bandIndex, bandJson);
  }

  setTrackStripInsertBypassed(
    trackId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.native.setTrackStripInsertBypassed(trackId, insertIndex, bypassed, resetOnBypass);
  }

  setMasterStripJson(sceneJson: string): void {
    this.native.setMasterStripJson(sceneJson);
  }

  setMasterStripEqBand(bandIndex: number, band: EqBandInput | string): void {
    this.native.setMasterStripEqBandJson(
      bandIndex,
      typeof band === 'string' ? band : JSON.stringify(band),
    );
  }

  setMasterStripEqBandJson(bandIndex: number, bandJson: string): void {
    this.native.setMasterStripEqBandJson(bandIndex, bandJson);
  }

  setMasterStripInsertBypassed(
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.native.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
  }

  /**
   * Changes one track-strip insert parameter in realtime, addressed by the
   * processor's JSON-key parameter name (see {@link masteringInsertParamInfo}).
   * Applied at the next block head via the engine command queue; safe during
   * playback. Throws if the track, insert, or name is unknown, the param is not
   * realtime-safe, or the command queue is full.
   */
  setTrackStripInsertParamByName(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.setTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
  }

  /** Master-strip counterpart of {@link setTrackStripInsertParamByName}. */
  setMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    this.native.setMasterStripInsertParamByName(insertIndex, paramName, value);
  }

  /**
   * Bus-strip counterpart of {@link setTrackStripInsertParamByName}. The bus
   * must already exist via {@link setTrackBuses} and carry a strip configured
   * with {@link setBusStripJson}.
   */
  setBusStripInsertParamByName(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.setBusStripInsertParamByName(busId, insertIndex, paramName, value);
  }

  /**
   * Bus-strip counterpart of {@link setTrackStripInsertBypassed}. The bus must
   * already exist via {@link setTrackBuses} and carry a strip configured with
   * {@link setBusStripJson}.
   */
  setBusStripInsertBypassed(
    busId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.native.setBusStripInsertBypassed(busId, insertIndex, bypassed, resetOnBypass);
  }

  /**
   * Resolves a track-lane insert parameter (by its JSON-key name) to the
   * reserved automation id that can then be driven with
   * {@link setAutomationLane}, {@link setParameter}, or
   * {@link setParameterSmoothed}, exactly like a fader/pan id. Returns `-1`
   * when the track, insert, or name is unknown. (The Python binding raises a
   * `SonareError` for an unknown id where Node/WASM return the `-1` sentinel.)
   *
   * This trio is how a mastering processor gets time-varying automation: the
   * `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`,
   * `maximizer.*` and `multiband.*` processors are all available as strip
   * inserts, so placing one on a strip and resolving its parameter here drives
   * it at audio-block precision, live and offline alike. The whole-signal
   * stages of the offline mastering chain (`repair.*`, `loudness`, and the
   * match stages) have no insert form and no automation id: they buffer the
   * entire signal by construction and do not run on the realtime path.
   */
  resolveTrackInsertAutomationId(trackId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveTrackInsertAutomationId(trackId, insertIndex, paramName);
  }

  /** Master-strip counterpart of {@link resolveTrackInsertAutomationId}. */
  resolveMasterInsertAutomationId(insertIndex: number, paramName: string): number {
    return this.native.resolveMasterInsertAutomationId(insertIndex, paramName);
  }

  /** Bus-strip counterpart of {@link resolveTrackInsertAutomationId}. */
  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveBusInsertAutomationId(busId, insertIndex, paramName);
  }

  /**
   * Resolves a hosted instrument's continuous parameter (by its JSON-key name)
   * to the reserved automation id, so an instrument parameter is driven at
   * audio-block precision exactly like a strip insert. Returns `-1` when the
   * destination has no bound instrument, the instrument exposes no automatable
   * parameters, or the name is unknown.
   *
   * For the NativeSynth ({@link setSynthInstrument}) the names are the
   * continuous `SynthPatch` fields: `gain`, `busDrive`, `cutoffHz`,
   * `resonanceQ`, `drive`, `keyTrack`, `envToCutoffCents`, `velToCutoffCents`,
   * `ampAttackMs`, `ampDecayMs`, `ampSustain`, `ampReleaseMs`,
   * `filterAttackMs`, `filterDecayMs`, `filterSustain`, `filterReleaseMs`,
   * `lfoRateHz`, `lfoToPitchCents`, `lfo2RateHz`, `glideMs`, `bodyMix`,
   * `stereoSpread`, `detuneCents`, `driftCents`, `pitchOffsetCents`.
   *
   * Structural fields (`preset`, `engineMode`, `waveform`, `filterModel`,
   * `unison`, `polyphony`, `body`, `modRoutings`) are not automatable and
   * return `-1`; rebind the instrument with a new patch instead.
   *
   * `gain`, `busDrive`, `cutoffHz`, `resonanceQ`, `envToCutoffCents`,
   * `lfoToPitchCents` and `pitchOffsetCents` reach already-sounding voices from
   * the next block; the rest are cached at note-on and take effect from the
   * next note.
   */
  resolveInstrumentAutomationId(destinationId: number, paramName: string): number {
    return this.native.resolveInstrumentAutomationId(destinationId, paramName);
  }

  /**
   * Sets a track lane strip's pan position (-1..1) in realtime. Applied at the
   * next block head via the engine command queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param pan Pan position from -1 (hard left) to 1 (hard right).
   */
  setTrackStripPan(trackId: number, pan: number): void {
    this.native.setTrackStripPan(trackId, pan);
  }

  /**
   * Sets a track lane strip's pan law in realtime. Applied at the next block
   * head via the engine command queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param panLaw Pan law as an enum name, the enum, or the raw int.
   */
  setTrackStripPanLaw(trackId: number, panLaw: PanLawInput): void {
    this.native.setTrackStripPanLaw(trackId, panLawValue(panLaw));
  }

  /**
   * Sets a track lane strip's pan mode in realtime. Applied at the next block
   * head via the engine command queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param panMode Pan mode as an enum name, the enum, or the raw int.
   */
  setTrackStripPanMode(trackId: number, panMode: PanMode): void {
    this.native.setTrackStripPanMode(trackId, panModeValue(panMode));
  }

  /**
   * Sets a track lane strip's independent left/right pan positions (dual-pan
   * mode) in realtime. Applied at the next block head via the engine command
   * queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param leftPan Left-channel pan position from -1 to 1.
   * @param rightPan Right-channel pan position from -1 to 1.
   */
  setTrackStripDualPan(trackId: number, leftPan: number, rightPan: number): void {
    this.native.setTrackStripDualPan(trackId, leftPan, rightPan);
  }

  /**
   * Sets a track lane strip's inter-channel alignment delay in samples.
   * `delaySamples` is a non-negative whole-sample delay. This changes strip
   * latency, so PDC and the reported graph latency are rebuilt — treat it as a
   * structural change: do NOT call it concurrently with {@link process}. Stop
   * playback (or otherwise quiesce the audio callback) before calling.
   *
   * @param trackId Lane the strip belongs to.
   * @param delaySamples Non-negative channel delay in samples.
   */
  setTrackStripChannelDelaySamples(trackId: number, delaySamples: number): void {
    this.native.setTrackStripChannelDelaySamples(trackId, delaySamples);
  }

  supplyClipPage(providerId: number, pageIndex: number, channels: Float32Array[]): void {
    this.native.supplyClipPage(providerId, pageIndex, channels);
  }

  clearClipPage(providerId: number, pageIndex: number): void {
    this.native.clearClipPage(providerId, pageIndex);
  }

  destroyClipPageProvider(providerId: number): void {
    this.native.destroyClipPageProvider(providerId);
  }

  popClipPageRequest(): ClipPageRequest | null {
    return this.native.popClipPageRequest();
  }

  /**
   * Sets the clip-page look-ahead window in timeline frames.
   *
   * The player reports the pages it is *about to* read that are not resident
   * yet, so a streaming host can service them before the audio thread reaches
   * them. Without look-ahead a page miss is only reported after the read
   * already produced silence, which costs one block of silence at every page
   * boundary the host has not primed.
   *
   * Look-ahead requests drain through the same {@link popClipPageRequest}
   * queue and are queued *after* the block's genuine misses, so a host that
   * keeps only the newest request per clip tracks the look-ahead frontier.
   *
   * `prepare` defaults this to half a second at the engine's sample rate; `0`
   * disables it. A clip whose pages are all resident produces no requests
   * either way. Safe to call during playback.
   */
  setClipPagePrefetchFrames(frames: number): void {
    this.native.setClipPagePrefetchFrames(frames);
  }

  /** Current clip-page look-ahead window in timeline frames. */
  clipPagePrefetchFrames(): number {
    return this.native.clipPagePrefetchFrames();
  }

  /**
   * Allocate an addon-owned capture buffer with the requested channel count
   * and capacity in frames. This is the canonical cross-binding form.
   */
  setCaptureBuffer(numChannels: number, capacityFrames: number): void;
  /**
   * Install caller-owned capture planes. The addon copies them immediately, so
   * their ArrayBuffers may be transferred after this call.
   *
   * @deprecated Prefer `(numChannels, capacityFrames)` for cross-binding parity.
   */
  setCaptureBuffer(channels: Float32Array[]): void;
  setCaptureBuffer(numChannelsOrChannels: number | Float32Array[], capacityFrames?: number): void {
    if (Array.isArray(numChannelsOrChannels)) {
      this.native.setCaptureBuffer(numChannelsOrChannels);
      return;
    }
    if (
      !Number.isSafeInteger(numChannelsOrChannels) ||
      numChannelsOrChannels <= 0 ||
      capacityFrames === undefined ||
      !Number.isSafeInteger(capacityFrames) ||
      capacityFrames <= 0
    ) {
      throw new RangeError('capture channel count and capacity must be positive safe integers');
    }
    // The runtime validation above proves this optional overload argument is a
    // positive safe integer; retain that fact for TypeScript's type system.
    const validatedCapacityFrames = capacityFrames as number;
    this.native.setCaptureBuffer(
      Array.from(
        { length: numChannelsOrChannels },
        () => new Float32Array(validatedCapacityFrames),
      ),
    );
  }

  armCapture(armed = true): void {
    this.native.armCapture(armed);
  }

  setCapturePunch(startSample: number, endSample: number, enabled = true): void {
    this.native.setCapturePunch(startSample, endSample, enabled);
  }

  setCaptureSource(source: EngineCaptureSource): void {
    this.native.setCaptureSource(source);
  }

  /** Positive values delay capture relative to the punch window. */
  setRecordOffsetSamples(offsetSamples: number): void {
    this.native.setRecordOffsetSamples(offsetSamples);
  }

  setInputMonitor(enabled: boolean, gain = 1): void {
    this.native.setInputMonitor(enabled, gain);
  }

  resetCapture(): void {
    this.native.resetCapture();
  }

  captureStatus(): EngineCaptureStatus {
    return this.native.captureStatus();
  }

  /**
   * Read the recorded samples from the addon-owned capture buffer.
   *
   * Returns one `Float32Array` per capture channel, each sliced to the number
   * of frames recorded so far (see {@link captureStatus}). Call after capture
   * to retrieve the audio written after {@link setCaptureBuffer}. The addon
   * copies that method's inputs, so transferring or detaching the original
   * ArrayBuffers cannot invalidate an active capture.
   */
  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  setGraph(spec: EngineGraphSpec): void {
    this.native.setGraph(spec);
  }

  graphNodeCount(): number {
    return this.native.graphNodeCount();
  }

  graphConnectionCount(): number {
    return this.native.graphConnectionCount();
  }

  /**
   * Renders one block and returns processed channel copies. Input channel
   * buffers are never mutated: the addon copies each plane before adding engine
   * output. Pass zero-filled planes when the engine is the only audio source.
   */
  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  processWithMonitor(channels: Float32Array[]): {
    output: Float32Array[];
    monitor: Float32Array[];
  } {
    return this.native.processWithMonitor(channels);
  }

  renderOffline(channels: Float32Array[], blockSize = 128): Float32Array[] {
    return this.native.renderOffline(channels, blockSize);
  }

  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    return this.native.bounceOffline(options);
  }

  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    return this.native.freezeOffline(options);
  }

  drainTelemetry(maxRecords = 1024): EngineTelemetry[] {
    return this.native.drainTelemetry(maxRecords);
  }

  /** Drain pending meter telemetry records published by the engine's meter tap. */
  drainMeterTelemetry(maxRecords = 1024): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords);
  }

  /**
   * Drain pending meter telemetry as per-plane (wide) records for a surround
   * target. Use this for a surround mix target; {@link drainMeterTelemetry}
   * stays the stereo fast path. The two share one queue, so call only one per
   * target.
   */
  drainMeterTelemetryWide(maxRecords = 1024): EngineMeterTelemetryWide[] {
    return this.native.drainMeterTelemetryWide(maxRecords);
  }

  /**
   * Enable or configure per-target spectrum + vectorscope telemetry.
   *
   * @param intervalFrames - Minimum render-frame gap between published snapshots; `0` disables capture
   * @param bandCount - Requested FFT band resolution (1..64); changing it re-prepares the tap
   * @returns The band count actually applied
   */
  configureScopeTelemetry(intervalFrames: number, bandCount: number): number {
    return this.native.configureScopeTelemetry(intervalFrames, bandCount);
  }

  /** Drain pending spectrum + vectorscope telemetry records published by the engine's scope tap. */
  drainScopeTelemetry(maxRecords = 1024): EngineScopeTelemetry[] {
    return this.native.drainScopeTelemetry(maxRecords);
  }

  /**
   * Push a live parameter value to the engine (immediate jump).
   *
   * @param paramId - Target parameter id
   * @param value - New value
   * @param renderFrame - Render-frame time to apply, or `-1` for immediate
   */
  setParameter(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameter(paramId, value, renderFrame);
  }

  /** Push a live parameter value to the engine using a smoothed ramp. */
  setParameterSmoothed(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameterSmoothed(paramId, value, renderFrame);
  }

  /**
   * Set the default ramp time (ms) for engine-level smoothed parameters —
   * fader/pan glides, insert-parameter automation, and MIDI-CC mappings. The
   * default is 20 ms; pass `0` for instant (un-ramped) changes.
   */
  setParamSmoothingMs(smoothingMs: number): void {
    this.native.setParamSmoothingMs(smoothingMs);
  }

  setSoloMute(laneIndex: number, solo: boolean, mute: boolean, renderFrame = -1): void {
    this.native.setSoloMute(laneIndex, solo, mute, renderFrame);
  }

  /** Schedule a per-track PFL/AFL monitor tap at a render-frame boundary. */
  setTrackMonitorMode(laneIndex: number, mode: EngineTrackMonitorMode, renderFrame = -1): void {
    this.native.setTrackMonitorMode(laneIndex, trackMonitorModeValue(mode), renderFrame);
  }

  /**
   * Remove all registered parameters and release their backing strings. Use
   * before re-registering a parameter id (add() rejects duplicate ids). Not
   * realtime-safe.
   */
  clearParameters(): void {
    this.native.clearParameters();
  }

  /**
   * Replace the realtime MIDI clip snapshot. Events are absolute render-frame
   * UMP events compiled for the engine timeline.
   */
  setMidiClips(clips: ReadonlyArray<EngineMidiClipSchedule>): void {
    this.native.setMidiClips(clips);
  }

  /**
   * Bind a built-in synth to a realtime MIDI destination. Live note/CC commands
   * and scheduled MIDI clips routed to that destination render through it.
   */
  setBuiltinInstrument(
    config: BuiltinSynthConfig = {},
    destinationId = config.destinationId ?? 0,
  ): void {
    this.native.setBuiltinInstrument(destinationId, config);
  }

  /**
   * Bind the patch-driven NativeSynth to a realtime MIDI destination. `patch`
   * is a {@link SynthPatch} or a preset-name string (`'saw-lead'` /
   * `'va:saw-lead'`; see {@link synthPresetNames}), resolving exactly like
   * {@link Project.bounceWithSynthInstrument}. Live note/CC commands and
   * scheduled MIDI clips routed to that destination render through the synth.
   * Unknown preset names throw.
   */
  setSynthInstrument(
    patch: SynthPatch | string = {},
    destinationId = (typeof patch === 'object' ? patch.destinationId : undefined) ?? 0,
  ): void {
    this.native.setSynthInstrument(destinationId, patch);
  }

  /**
   * Load (parse) SoundFont 2 bytes into the engine so SF2 instruments can be
   * bound with {@link setSf2Instrument}. Replaces any previously loaded
   * SoundFont (already-bound SF2 instruments keep the SoundFont they were
   * created with); the input buffer is not referenced after the call.
   */
  loadSoundFont(data: Uint8Array): void {
    this.native.loadSoundFont(data);
  }

  /**
   * Bind a GS-compatible SoundFont player to a realtime MIDI destination, fed
   * by the engine's loaded SoundFont ({@link loadSoundFont}). Live note/CC
   * commands and scheduled MIDI clips routed to that destination render
   * through the player (16 MIDI channels, channel 10 drums, GS NRPN part
   * edits, GS/GM SysEx resets). Without a loaded SoundFont — or for programs
   * the SoundFont does not cover — notes play through the built-in
   * synthesizer GM fallback bank (the data-free floor).
   */
  setSf2Instrument(
    config: Sf2InstrumentConfig = {},
    destinationId = config.destinationId ?? 0,
  ): void {
    this.native.setSf2Instrument(destinationId, config);
  }

  clearMidiInstrument(destinationId = 0): void {
    this.native.clearMidiInstrument(destinationId);
  }

  midiInstrumentCount(): number {
    return this.native.midiInstrumentCount();
  }

  /**
   * Bind a live MIDI CC to an engine automation parameter. The MIDI event still
   * reaches the destination instrument; when bound, its 7-bit value is also
   * mapped into [minValue, maxValue] for `paramId`.
   */
  bindMidiCc(
    channel: number,
    controller: number,
    paramId: number,
    options: MidiCcBindOptions = {},
  ): void {
    this.native.bindMidiCc(
      channel,
      controller,
      paramId,
      options.minValue ?? 0,
      options.maxValue ?? 1,
    );
  }

  /** Bind a 7/14-bit CC, RPN, or NRPN descriptor to a live parameter. */
  bindMidiCcBinding(binding: ProjectMidiCcBinding): void {
    this.native.bindMidiCcBinding(binding);
  }

  clearMidiCcBindings(): void {
    this.native.clearMidiCcBindings();
  }

  midiCcBindingCount(): number {
    return this.native.midiCcBindingCount();
  }

  /** Install/replace a live non-destructive MIDI-FX insert for one destination. */
  setMidiFx(destinationId: number, configJson: string): void {
    this.native.setMidiFx(destinationId, configJson);
  }

  clearMidiFx(destinationId = 0): void {
    this.native.clearMidiFx(destinationId);
  }

  /** Enable the engine-owned live MIDI input source for a destination. */
  setMidiInputSource(destinationId = 0): void {
    this.native.setMidiInputSource(destinationId);
  }

  clearMidiInputSource(): void {
    this.native.clearMidiInputSource();
  }

  midiInputPendingCount(): number {
    return this.native.midiInputPendingCount();
  }

  pushMidiInputNoteOn(
    group: number,
    channel: number,
    note: number,
    velocity: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputNoteOn(group, channel, note, velocity, portTimeSamples);
  }

  pushMidiInputNoteOff(
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputNoteOff(group, channel, note, velocity, portTimeSamples);
  }

  pushMidiInputCc(
    group: number,
    channel: number,
    controller: number,
    value: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputCc(group, channel, controller, value, portTimeSamples);
  }

  pushMidiNoteOn(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiNoteOn(destinationId, group, channel, note, velocity, renderFrame);
  }

  pushMidiNoteOff(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame = -1,
  ): void {
    this.native.pushMidiNoteOff(destinationId, group, channel, note, velocity, renderFrame);
  }

  /**
   * Queue an immediate (live) MIDI control change to a MIDI destination. Values
   * are 7-bit; channel 0..15, group 0..15. `renderFrame` is the render-frame
   * time to apply, or -1 for immediate.
   */
  pushMidiCc(
    destinationId: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiCc(destinationId, group, channel, controller, value, renderFrame);
  }

  /**
   * Queue a MIDI panic (all-notes-off) releasing every sounding note.
   * `renderFrame` is the render-frame time to apply, or -1 for immediate.
   */
  pushMidiPanic(renderFrame = -1): void {
    this.native.pushMidiPanic(renderFrame);
  }

  /**
   * Queue an immediate (live) MIDI SysEx message to a MIDI destination. `data`
   * is the full SysEx frame including the leading 0xF0 and trailing 0xF7, and
   * must be 1..512 bytes. `renderFrame` is the render-frame time to apply, or
   * -1 for immediate.
   */
  pushMidiSysex(destinationId: number, data: Uint8Array, renderFrame = -1): void {
    this.native.pushMidiSysex(destinationId, data, renderFrame);
  }

  /**
   * Routes a MIDI destination (a track lane) to the external-MIDI output queue
   * instead of the internal instrument rack, so the track drives an external
   * device. Its sequenced events are buffered for {@link drainExternalMidi}.
   * Clearing it restores internal-synth playback. Control-thread only.
   */
  setMidiDestinationExternal(destinationId: number, external: boolean): void {
    this.native.setMidiDestinationExternal(destinationId, external);
  }

  /**
   * Enables forwarding MIDI clock (0xF8) and transport (start/continue/stop)
   * bytes to the external output queue, tagged with destination `0xFFFFFFFF`,
   * so external gear stays tempo-synced. Off by default; control-thread only.
   */
  setExternalMidiClockEnabled(enabled: boolean): void {
    this.native.setExternalMidiClockEnabled(enabled);
  }

  /** Number of external-MIDI events dropped because the output queue was full. */
  externalMidiDroppedCount(): number {
    return this.native.externalMidiDroppedCount();
  }

  /**
   * Drains queued external-MIDI events, already lowered to MIDI 1.0 byte
   * messages so the host can write them straight to an output port. Returns one
   * entry per lowered message; transport/clock bytes carry
   * `destinationId === 0xFFFFFFFF`. `maxRecords` caps the number of output
   * events returned — the shared unit across every surface. Events past the cap
   * stay queued for the next call (lossless); call again to drain the rest.
   */
  drainExternalMidi(maxRecords = 1024): EngineExternalMidiEvent[] {
    return this.native.drainExternalMidi(maxRecords);
  }

  /** Read the current engine transport state (playing/position/ppq/tempo). */
  getTransportState(): EngineTransportState {
    return this.native.getTransportState();
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /** Releases the native handle; lets `using` (Node 22+) free it automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

export class ClipPageProvider {
  private disposed = false;

  constructor(
    private readonly engine: RealtimeEngine,
    readonly id: number,
  ) {}

  supply(pageIndex: number, channels: Float32Array[]): void {
    if (this.disposed) {
      throw new Error('ClipPageProvider is destroyed');
    }
    this.engine.supplyClipPage(this.id, pageIndex, channels);
  }

  clear(pageIndex: number): void {
    if (this.disposed) {
      return;
    }
    this.engine.clearClipPage(this.id, pageIndex);
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.engine.destroyClipPageProvider(this.id);
  }

  [Symbol.dispose](): void {
    this.destroy();
  }
}

export class FileClipPageProvider extends ClipPageProvider {
  private fd: number | null;
  private readonly numChannels: number;
  private readonly numSamples: number;
  private readonly pageFrames: number;
  private readonly dataOffsetBytes: number;

  constructor(
    engine: RealtimeEngine,
    id: number,
    path: string,
    options: FileClipPageProviderOptions,
  ) {
    super(engine, id);
    if (options.numChannels <= 0 || options.numSamples <= 0 || options.pageFrames <= 0) {
      throw new Error('numChannels, numSamples, and pageFrames must be positive');
    }
    this.fd = openSync(path, 'r');
    this.numChannels = options.numChannels;
    this.numSamples = options.numSamples;
    this.pageFrames = options.pageFrames;
    this.dataOffsetBytes = options.dataOffsetBytes ?? 0;
  }

  supplyPage(pageIndex: number): boolean {
    if (this.fd === null) {
      throw new Error('FileClipPageProvider is destroyed');
    }
    if (pageIndex < 0) {
      return false;
    }
    const startFrame = pageIndex * this.pageFrames;
    if (startFrame >= this.numSamples) {
      return false;
    }
    const frames = Math.min(this.pageFrames, this.numSamples - startFrame);
    const frameBytes = this.numChannels * Float32Array.BYTES_PER_ELEMENT;
    const buffer = Buffer.allocUnsafe(frames * frameBytes);
    const bytesRead = readSync(
      this.fd,
      buffer,
      0,
      buffer.byteLength,
      this.dataOffsetBytes + startFrame * frameBytes,
    );
    const framesRead = Math.floor(bytesRead / frameBytes);
    if (framesRead <= 0) {
      return false;
    }
    const channels = Array.from({ length: this.numChannels }, () => new Float32Array(framesRead));
    for (let frame = 0; frame < framesRead; ++frame) {
      for (let ch = 0; ch < this.numChannels; ++ch) {
        channels[ch][frame] = buffer.readFloatLE((frame * this.numChannels + ch) * 4);
      }
    }
    this.supply(pageIndex, channels);
    return true;
  }

  supplyRequest(request: ClipPageRequest): boolean {
    return this.supplyPage(Math.floor(request.sample / this.pageFrames));
  }

  destroy(): void {
    if (this.fd !== null) {
      closeSync(this.fd);
      this.fd = null;
    }
    super.destroy();
  }
}

export function engineAbiVersion(): number {
  return addon.engineAbiVersion();
}

export function voiceChangerAbiVersion(): number {
  return addon.voiceChangerAbiVersion();
}
