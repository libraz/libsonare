import { closeSync, openSync, readSync } from 'node:fs';
import {
  analyzeBpm as analyzeBpmFn,
  analyzeDynamics as analyzeDynamicsFn,
  analyzeRhythm as analyzeRhythmFn,
  analyzeTimbre as analyzeTimbreFn,
  chordFunctionalAnalysis as chordFunctionalAnalysisFn,
  detectAcoustic as detectAcousticFn,
  detectChords as detectChordsFn,
} from './analysis.js';
import type { VoiceChangeOptions } from './effects_mastering.js';
import {
  masterAudio as masterAudioFn,
  masteringChain as masteringChainFn,
  mastering as masteringFn,
  noteStretch as noteStretchFn,
  voiceChange as voiceChangeFn,
} from './effects_mastering.js';
import { addon } from './native.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeTimbreOptions,
  AutomationCurve,
  BpmAnalysisResult,
  BuiltinInstrumentConfig,
  BuiltinSynthConfig,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  ClipPageRequest,
  DynamicsResult,
  EngineAutomationPoint,
  EngineAutomationPointCurve,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTrackLane,
  EngineTransportState,
  EqBandInput,
  FileClipPageProviderOptions,
  GoniometerPoint,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringOptions,
  MasteringPreset,
  MasteringResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MidiCcBindOptions,
  MidiCcLearnOptions,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  NoteStretchOptions,
  PanLaw,
  PanMode,
  PitchResult,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectBounceOptions,
  ProjectChordSymbol,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFade,
  ProjectClipTake,
  ProjectCompileResult,
  ProjectFadeCurve,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMidiCcBinding,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrackDesc,
  RhythmResult,
  SendTiming,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SoloProcessor,
  StftDbResult,
  StftResult,
  StripRef,
  SynthEnumTables,
  SynthPatch,
  SynthWaveform,
  TimbreResult,
  WarpMode,
} from './types.js';
// Runtime value re-exported from types (the rest of `./types.js` is type-only).
import { EXPECTED_PROJECT_ABI_VERSION } from './types.js';
import type { ValidateOptions } from './validation.js';
import {
  assertFiniteScalar,
  assertProjectMidiEvents,
  assertSamples,
  midi1Event,
} from './validation.js';

export * from './analysis.js';
export * from './effects_mastering.js';
export * from './errors.js';
export * from './features.js';
export * from './metering.js';
export {
  EXPECTED_PROJECT_ABI_VERSION,
  MarkerKind,
  SYNTH_BODY_TYPES,
  SYNTH_ENGINE_MODES,
  SYNTH_FILTER_MODELS,
  SYNTH_FILTER_OUTPUTS,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
  SYNTH_OSC_WAVEFORMS,
} from './types.js';
export type { ValidateOptions } from './validation.js';

export class Audio {
  private native: InstanceType<typeof addon.Audio>;
  private disposed = false;

  private constructor(native: InstanceType<typeof addon.Audio>) {
    this.native = native;
  }

  static fromFile(path: string): Audio {
    return new Audio(addon.Audio.fromFile(path));
  }

  /**
   * Wrap raw mono float samples as an {@link Audio}. `sampleRate` defaults to
   * `48000` (the project default) when omitted.
   */
  static fromBuffer(samples: Float32Array, sampleRate = 48000): Audio {
    return new Audio(addon.Audio.fromBuffer(samples, sampleRate));
  }

  static fromMemory(data: Buffer | Uint8Array): Audio {
    return new Audio(addon.Audio.fromMemory(data));
  }

  getData(): Float32Array {
    return this.native.getData();
  }

  getLength(): number {
    return this.native.getLength();
  }

  getSampleRate(): number {
    return this.native.getSampleRate();
  }

  getDuration(): number {
    return this.native.getDuration();
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

  // -- Analysis --

  detectBpm(): number {
    return this.native.detectBpm();
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    // Native instance method reads the handle's buffer directly (same options
    // and result shape as the standalone addon.detectKey); routing through
    // getData() would copy the whole buffer out of native memory first.
    return this.native.detectKey(options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return this.native.detectKeyCandidates(options);
  }

  detectBeats(): Float32Array {
    return this.native.detectBeats();
  }

  detectDownbeats(): Float32Array {
    return this.native.detectDownbeats();
  }

  detectOnsets(): Float32Array {
    return this.native.detectOnsets();
  }

  analyze(): AnalysisResult {
    return this.native.analyze();
  }

  analyzeBpm(options: AnalyzeBpmOptions = {}): BpmAnalysisResult {
    return analyzeBpmFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeImpulseResponse(nOctaveBands = 6): AcousticResult {
    return addon.analyzeImpulseResponse(this.getData(), this.getSampleRate(), nOctaveBands);
  }

  detectAcoustic(options: AcousticOptions = {}): AcousticResult {
    return detectAcousticFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeRhythm(options: AnalyzeRhythmOptions = {}): RhythmResult {
    return analyzeRhythmFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeDynamics(options: AnalyzeDynamicsOptions = {}): DynamicsResult {
    return analyzeDynamicsFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeTimbre(options: AnalyzeTimbreOptions = {}): TimbreResult {
    return analyzeTimbreFn(this.getData(), this.getSampleRate(), options);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChordsFn(this.getData(), this.getSampleRate(), options);
  }

  chordFunctionalAnalysis(
    keyRoot: number,
    keyMode = 0,
    options: ChordDetectionOptions = {},
  ): string[] {
    return chordFunctionalAnalysisFn(
      this.getData(),
      keyRoot,
      keyMode,
      this.getSampleRate(),
      options,
    );
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return addon.hpss(this.getData(), this.getSampleRate(), kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return addon.harmonic(this.getData(), this.getSampleRate());
  }

  percussive(): Float32Array {
    return addon.percussive(this.getData(), this.getSampleRate());
  }

  timeStretch(rate: number): Float32Array {
    return addon.timeStretch(this.getData(), this.getSampleRate(), rate);
  }

  pitchShift(semitones: number): Float32Array {
    return addon.pitchShift(this.getData(), this.getSampleRate(), semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return addon.pitchCorrectToMidi(this.getData(), this.getSampleRate(), currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretchFn(this.getData(), this.getSampleRate(), options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChangeFn(this.getData(), this.getSampleRate(), options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return addon.normalize(this.getData(), this.getSampleRate(), targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return masteringFn(this.getData(), this.getSampleRate(), options);
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return addon.masteringProcess(processorName, this.getData(), this.getSampleRate(), params);
  }

  masteringChain(
    config: MasteringChainConfig = {},
    onProgress?: (progress: number, stage: string) => void,
  ): MasteringChainResult {
    return masteringChainFn(this.getData(), this.getSampleRate(), config, onProgress);
  }

  masterAudio(
    preset: MasteringPreset = 'pop',
    overrides: MasteringChainConfig = {},
    onProgress?: (progress: number, stage: string) => void,
  ): MasteringChainResult {
    return masterAudioFn(this.getData(), this.getSampleRate(), preset, overrides, onProgress);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return addon.trim(this.getData(), this.getSampleRate(), thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return addon.stft(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): StftDbResult {
    return addon.stftDb(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  melSpectrogram(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MelSpectrogramResult {
    return addon.melSpectrogram(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      nMels,
      fmin,
      fmax,
      htk,
    );
  }

  mfcc(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    nMfcc = 20,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MfccResult {
    return addon.mfcc(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      nMels,
      nMfcc,
      fmin,
      fmax,
      htk,
    );
  }

  chroma(nFft = 2048, hopLength = 512): ChromaResult {
    return addon.chroma(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralCentroid(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralBandwidth(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return addon.spectralRolloff(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      rollPercent,
    );
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralFlatness(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return addon.zeroCrossingRate(this.getData(), this.getSampleRate(), frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return addon.rmsEnergy(this.getData(), this.getSampleRate(), frameLength, hopLength);
  }

  pitchYin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
    fillNa = false,
  ): PitchResult {
    return addon.pitchYin(
      this.getData(),
      this.getSampleRate(),
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  pitchPyin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
    fillNa = false,
  ): PitchResult {
    return addon.pitchPyin(
      this.getData(),
      this.getSampleRate(),
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  resample(targetSr: number): Float32Array {
    return addon.resample(this.getData(), this.getSampleRate(), targetSr);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return addon.onsetEnvelope(this.getData(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  nnlsChroma(): { nChroma: number; nFrames: number; data: Float32Array } {
    return addon.nnlsChroma(this.getData(), this.getSampleRate());
  }

  lufs(options: ValidateOptions = {}): LufsResult {
    const data = this.getData();
    assertSamples('lufs', data, options.validate !== false);
    return addon.lufs(data, this.getSampleRate());
  }

  momentaryLufs(options: ValidateOptions = {}): Float32Array {
    const data = this.getData();
    assertSamples('momentaryLufs', data, options.validate !== false);
    return addon.momentaryLufs(data, this.getSampleRate());
  }

  shortTermLufs(options: ValidateOptions = {}): Float32Array {
    const data = this.getData();
    assertSamples('shortTermLufs', data, options.validate !== false);
    return addon.shortTermLufs(data, this.getSampleRate());
  }
}

export class RealtimeEngine {
  private native: InstanceType<typeof addon.RealtimeEngine>;
  private disposed = false;

  constructor(
    sampleRate = 48000,
    maxBlockSize = 128,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ) {
    this.native = new addon.RealtimeEngine(
      sampleRate,
      maxBlockSize,
      commandCapacity,
      telemetryCapacity,
    );
  }

  prepare(
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ): void {
    this.native.prepare(sampleRate, maxBlockSize, commandCapacity, telemetryCapacity);
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

  seekPpq(ppq: number, renderFrame = -1): void {
    this.native.seekPpq(ppq, renderFrame);
  }

  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
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
      lanes.map((lane) => (typeof lane === 'number' ? { trackId: lane } : lane)),
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

  setCaptureBuffer(channels: Float32Array[]): void {
    this.native.setCaptureBuffer(channels);
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
   * Read the recorded samples out of the capture buffer.
   *
   * Returns one `Float32Array` per capture channel, each sliced to the number
   * of frames recorded so far (see {@link captureStatus}). Call after capture
   * to retrieve the audio written into the buffers passed to
   * {@link setCaptureBuffer}.
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

  setSoloMute(laneIndex: number, solo: boolean, mute: boolean, renderFrame = -1): void {
    this.native.setSoloMute(laneIndex, solo, mute, renderFrame);
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

/**
 * Returns the runtime project ABI version of the loaded native binding.
 *
 * Equals {@link EXPECTED_PROJECT_ABI_VERSION} when the arrangement subsystem is
 * compiled in, `0` when the native library was built without it.
 */
export function projectAbiVersion(): number {
  return addon.projectAbiVersion();
}

/**
 * NativeSynth preset catalog names (`'sine'`, `'saw-lead'`, `'e-piano'`,
 * `'drum-kit'`, ...). Use these to discover valid {@link SynthPatch} preset
 * names instead of hardcoding magic strings.
 */
export function synthPresetNames(): string[] {
  return addon.synthPresetNames();
}

/**
 * Fetch a named catalog preset as a {@link SynthPatch} (the preset name plus
 * the wrapper-section values), so hosts can inspect a preset and tweak fields
 * before binding it. A `"va:"` routing prefix is accepted; unknown names
 * throw.
 */
export function synthPresetPatch(name: string): SynthPatch {
  return addon.synthPresetPatch(name);
}

/** Return the canonical NativeSynth enum tables from the native C oracle. */
export function synthEnumTables(): SynthEnumTables {
  return addon._synthEnumTables();
}

/**
 * Headless arrangement / DAW project (the curated `sonare_project_*` C ABI).
 *
 * Wraps an opaque native project handle over the arrangement control plane
 * (EditHistory + the offline compiler/bounce, serializer, and MIR tempo/grid
 * bridges). Every method is control-thread-only and performs no file or device
 * I/O: project JSON and SMF bytes are exchanged in memory. All mutation routes
 * through the native EditHistory, so {@link undo} / {@link redo} work, and
 * serialization is deterministic (`toJson` is byte-stable for a given project
 * state within one build). Musical positions are PPQ (quarter notes).
 *
 * @example
 * ```typescript
 * const project = new Project();
 * const track = project.addTrack({ kind: 'audio', name: 'lead' });
 * const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
 * const json = project.toJson();
 * project.destroy();
 * ```
 */
export class Project {
  private native: InstanceType<typeof addon.Project>;
  private disposed = false;

  private constructor(native: InstanceType<typeof addon.Project>) {
    this.native = native;
  }

  /** Create a new empty project (throws on a project ABI mismatch). */
  static create(): Project {
    return new Project(new addon.Project());
  }

  /** Pack a MIDI 1.0 note-on event accepted by {@link setMidiEvents}. */
  static midiNoteOn(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiNoteOn', ppq, group, 0x9, channel, note, velocity);
  }

  /** Pack a MIDI 1.0 note-off event accepted by {@link setMidiEvents}. */
  static midiNoteOff(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiNoteOff', ppq, group, 0x8, channel, note, velocity);
  }

  /** Pack a MIDI 1.0 control-change event. */
  static midiCc(
    ppq: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiCc', ppq, group, 0xb, channel, controller, value);
  }

  /** Pack a MIDI 1.0 poly-pressure event. */
  static midiPolyPressure(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiPolyPressure', ppq, group, 0xa, channel, note, pressure);
  }

  /** Pack a MIDI 1.0 program-change event. */
  static midiProgram(
    ppq: number,
    group: number,
    channel: number,
    program: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiProgram', ppq, group, 0xc, channel, program, 0);
  }

  /** Return the General MIDI instrument name for `program`, or `null` when out of range. */
  static gmInstrumentName(program: number): string | null {
    return addon.midiGmInstrumentName(program);
  }

  /** Return the General MIDI program number for a canonical instrument name, or `-1`. */
  static gmProgramForName(name: string): number {
    return addon.midiGmProgramForName(name);
  }

  /** Return the General MIDI family name for `family`, or `null` when out of range. */
  static gmFamilyName(family: number): string | null {
    return addon.midiGmFamilyName(family);
  }

  /** Return the first General MIDI program number in `family`, or `-1`. */
  static gmFamilyFirstProgram(family: number): number {
    return addon.midiGmFamilyFirstProgram(family);
  }

  /** Return the GM2 bank/program instrument variation name, or `null` when unavailable. */
  static gm2InstrumentName(bankLsb: number, program: number): string | null {
    return addon.midiGm2InstrumentName(bankLsb, program);
  }

  /** Return the General MIDI drum name for `note`, or `null` when out of range. */
  static gmDrumName(note: number): string | null {
    return addon.midiGmDrumName(note);
  }

  /** Return the General MIDI drum note for a canonical drum name, or `-1`. */
  static gmDrumNoteForName(name: string): number {
    return addon.midiGmDrumNoteForName(name);
  }

  /** Return the GM2 drum-set name for `bankLsb`, or `null` when unavailable. */
  static gm2DrumSetName(bankLsb: number): string | null {
    return addon.midiGm2DrumSetName(bankLsb);
  }

  /** Return the GM2 drum name for `bankLsb`/`note`, or `null` when unavailable. */
  static gm2DrumName(bankLsb: number, note: number): string | null {
    return addon.midiGm2DrumName(bankLsb, note);
  }

  /** Return the MIDI CC name for `controller`, or `null` when out of range. */
  static midiCcName(controller: number): string | null {
    return addon.midiCcName(controller);
  }

  /** Return the MIDI CC number for a canonical controller name, or `-1`. */
  static midiCcIndexForName(name: string): number {
    return addon.midiCcIndexForName(name);
  }

  /** Return the MIDI 2.0 per-note controller name for `index`, or `null`. */
  static perNoteControllerName(index: number): string | null {
    return addon.midiPerNoteControllerName(index);
  }

  /** Expand bank-select + program-change into MIDI events accepted by {@link setMidiEvents}. */
  static midiBankProgram(
    ppq: number,
    group: number,
    channel: number,
    bankMsb: number,
    bankLsb: number,
    program: number,
  ): ProjectMidiEvent[] {
    return addon.midiBankProgram(ppq, group, channel, bankMsb, bankLsb, program);
  }

  /** Route MIDI events through the native MidiRouter filter/remap/thru logic. */
  static midiRouteEvents(
    events: ReadonlyArray<ProjectMidiEvent>,
    config: ProjectMidiRouteConfig = {},
  ): ProjectMidiRouteResult {
    return addon.midiRouteEvents(events, config);
  }

  /** Run native MIDI learn over an event stream; returns `null` when nothing is learned. */
  static midiCcLearn(
    events: ReadonlyArray<ProjectMidiEvent>,
    paramId: number,
    options: MidiCcLearnOptions = {},
  ): ProjectMidiCcBinding | null {
    return addon.midiCcLearn(
      events,
      paramId,
      options.minValue ?? 0,
      options.maxValue ?? 1,
      options.minMovement ?? 0,
    );
  }

  /** Convert one CC event to an automation breakpoint using native CcMap. */
  static midiCcToBreakpoint(
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    event: ProjectMidiEvent,
  ): ProjectAutomationPoint | null {
    return addon.midiCcToBreakpoint(bindings, event);
  }

  /** Convert one automation value back to a CC UMP event using native CcMap. */
  static midiParamToCc(
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    paramId: number,
    unitValue: number,
    group: number,
    ppq = 0,
  ): ProjectMidiEvent | null {
    return addon.midiParamToCc(bindings, paramId, unitValue, group, ppq);
  }

  /** Pack a MIDI 1.0 channel-pressure event. */
  static midiChannelPressure(
    ppq: number,
    group: number,
    channel: number,
    pressure: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiChannelPressure', ppq, group, 0xd, channel, pressure, 0);
  }

  /** Pack a MIDI 1.0 pitch-bend event (`bend` is unsigned 14-bit, center = 8192). */
  static midiPitchBend(
    ppq: number,
    group: number,
    channel: number,
    bend: number,
  ): ProjectMidiEvent {
    if (!Number.isInteger(bend) || bend < 0 || bend > 0x3fff) {
      throw new RangeError('Project.midiPitchBend: bend must be an integer in [0, 16383]');
    }
    return midi1Event('Project.midiPitchBend', ppq, group, 0xe, channel, bend & 0x7f, bend >> 7);
  }

  /**
   * Deserialize project JSON into a new project. Throws cleanly (with the joined
   * native diagnostic messages) on malformed input.
   */
  static fromJson(json: string): Project {
    return new Project(addon.Project.fromJson(json));
  }

  /**
   * Deserialize project JSON and return native warning diagnostics emitted on
   * successful loads, such as dangling source references preserved for repair.
   */
  static fromJsonWithDiagnostics(json: string): { project: Project; diagnostics: string } {
    const result = addon.Project.fromJsonWithDiagnostics(json);
    return {
      project: new Project(result.project),
      diagnostics: result.diagnostics,
    };
  }

  // -- serialization --

  /** Serialize the project to deterministic JSON. */
  toJson(): string {
    return this.native.toJson();
  }

  /** Set the project sample rate in Hz (must be > 0). */
  setSampleRate(sampleRate: number): void {
    this.native.setSampleRate(sampleRate);
  }

  /** Read the project sample rate in Hz. */
  getSampleRate(): number {
    return this.native.getSampleRate();
  }

  /** Set the project's clip-overlap policy (ordinal). */
  setOverlapPolicy(policy: number): void {
    this.native.setOverlapPolicy(policy);
  }

  /** Read the project's clip-overlap policy (ordinal). */
  getOverlapPolicy(): number {
    return this.native.getOverlapPolicy();
  }

  /** Replace the project's mixer scene from scene JSON (see {@link Mixer.fromSceneJson}). */
  setMixerSceneJson(sceneJson: string): void {
    this.native.setMixerSceneJson(sceneJson);
  }

  /** Add or replace a marker; `markerId` 0 allocates a new id. Returns the marker id. */
  setMarker(markerId: number, ppq: number, name: string): number {
    return this.native.setMarker(markerId, ppq, name);
  }

  /**
   * Add or replace a marker from a full descriptor, including its kind and key
   * signature. `marker.id` 0 (or omitted) allocates a new id. Returns the id.
   */
  setMarkerEx(marker: {
    id?: number;
    ppq: number;
    name?: string;
    kind?: number;
    keyFifths?: number;
    keyMinor?: boolean;
  }): number {
    return this.native.setMarkerEx(marker);
  }

  /** Read a project marker by index (0-based, in stored order). */
  markerByIndex(index: number): ProjectMarker {
    return this.native.markerByIndex(index);
  }

  /** Number of markers in the project value model. */
  markerCount(): number {
    return this.native.markerCount();
  }

  /** Replace the project's tempo segment list. */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    this.native.setTempoSegments(segments);
  }

  /** Replace the project's time-signature segment list. */
  setTimeSignatures(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    this.native.setTimeSignatures(segments);
  }

  /** Number of tracks in the project value model. */
  trackCount(): number {
    return this.native.trackCount();
  }

  /** Number of sources in the project value model. */
  sourceCount(): number {
    return this.native.sourceCount();
  }

  /** Number of tempo segments in the project value model. */
  tempoSegmentCount(): number {
    return this.native.tempoSegmentCount();
  }

  /** Number of time-signature segments in the project value model. */
  timeSignatureCount(): number {
    return this.native.timeSignatureCount();
  }

  // -- edit --

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    return this.native.addTrack({ ...desc, kind: trackKindValue(desc.kind) });
  }

  /** Add an audio or MIDI clip and return its allocated clip id. */
  addClip(desc: ProjectClipDesc): number {
    return this.native.addClip(desc);
  }

  /** Split captured loop-recording audio into takes and add one clip. */
  addLoopRecordingTakes(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingResult {
    return this.native.addLoopRecordingTakes(desc);
  }

  /** Create a MIDI track + clip; returns `{ trackId, clipId }`. */
  addMidiClip(startPpq: number, lengthPpq: number): ProjectMidiClipResult {
    return this.native.addMidiClip(startPpq, lengthPpq);
  }

  /** Split a clip at `splitPpq` (absolute PPQ); returns the new (right-hand) clip id. */
  splitClip(clipId: number, splitPpq: number): number {
    return this.native.splitClip(clipId, splitPpq);
  }

  /** Trim a clip's start / length (PPQ). */
  trimClip(clipId: number, newStartPpq: number, newLengthPpq: number): void {
    this.native.trimClip(clipId, newStartPpq, newLengthPpq);
  }

  /** Move a clip to `newStartPpq` (and optionally `newTrackId`; 0 = keep track). */
  moveClip(clipId: number, newStartPpq: number, newTrackId = 0): void {
    this.native.moveClip(clipId, newStartPpq, newTrackId);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackDesc['kind']): void {
    this.native.setTrackKind(trackId, trackKindValue(kind));
  }

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Set a clip's warp playback mode. */
  setClipWarpMode(clipId: number, mode: WarpMode | number): void {
    this.native.setClipWarpMode(clipId, warpModeValue(mode));
  }

  /** Add or replace a first-class warp map referenced by clip warp ids. */
  setWarpMap(map: import('./types.js').ProjectWarpMapDesc): void {
    this.native.setWarpMap(map);
  }

  /** Remove a first-class warp map by id. */
  removeWarpMap(warpRefId: number): void {
    this.native.removeWarpMap(warpRefId);
  }

  /** Route a track's MIDI clips to a host/instrument destination id. */
  setTrackMidiDestination(trackId: number, destinationId: number): void {
    this.native.setTrackMidiDestination(trackId, destinationId);
  }

  /** Remove a clip via an undoable edit (undo restores it + its MIDI content). */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /** Set a clip's linear playback gain (>= 0; 0 = muted) via an undoable edit. */
  setClipGain(clipId: number, gain: number): void {
    this.native.setClipGain(clipId, gain);
  }

  /**
   * Set a clip's fade-in / fade-out regions via an undoable edit. Each fade is
   * an optional `{ lengthPpq, curve? }` ({@link ProjectClipFade}); omitted
   * fields and omitted sides become a zero-length linear fade.
   */
  setClipFade(clipId: number, fadeIn?: ProjectClipFade, fadeOut?: ProjectClipFade): void {
    this.native.setClipFade(clipId, projectClipFadeValue(fadeIn), projectClipFadeValue(fadeOut));
  }

  /** Replace a clip's take list and active take id via an undoable edit. */
  setClipTakes(clipId: number, takes: ReadonlyArray<ProjectClipTake>, activeTakeId = 0): void {
    this.native.setClipTakes(clipId, takes, activeTakeId);
  }

  /** Replace a clip's comp segments via an undoable edit. */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    this.native.setClipCompSegments(clipId, segments);
  }

  /**
   * Set a clip's loop mode + loop length (PPQ) via an undoable edit.
   * `loopMode` is a {@link ProjectLoopMode} ordinal/name (0/off, 1/loop). When
   * looping, `loopLengthPpq` must be finite and > 0.
   */
  setClipLoop(clipId: number, loopMode: ProjectLoopMode, loopLengthPpq = 0): void {
    this.native.setClipLoop(clipId, projectLoopModeValue(loopMode), loopLengthPpq);
  }

  /** Rebind a clip to a different already-registered source via an undoable edit. */
  setClipSource(clipId: number, sourceId: number): void {
    this.native.setClipSource(clipId, sourceId);
  }

  /**
   * Duplicate a clip at `newStartPpq` (same track), copying any MIDI content,
   * via an undoable edit; returns the new clip id.
   */
  duplicateClip(clipId: number, newStartPpq: number): number {
    return this.native.duplicateClip(clipId, newStartPpq);
  }

  /** Remove a track (and its clips) via an undoable edit. */
  removeTrack(trackId: number): void {
    this.native.removeTrack(trackId);
  }

  /** Rename a track via an undoable edit (omit / null `name` = empty). */
  renameTrack(trackId: number, name?: string): void {
    this.native.renameTrack(trackId, name ?? null);
  }

  /**
   * Set a track's mixer-strip binding + output target via an undoable edit.
   * Pass `undefined`/empty to clear the respective field.
   */
  setTrackRoute(trackId: number, channelStripRef?: string, outputTarget?: string): void {
    this.native.setTrackRoute(trackId, channelStripRef ?? null, outputTarget ?? null);
  }

  /**
   * Append an automation lane to a track via an undoable edit; returns the
   * appended lane's index within the track.
   */
  addAutomationLane(trackId: number, desc: ProjectAutomationLaneDesc): number {
    return this.native.addAutomationLane(trackId, projectAutomationLaneValue(desc));
  }

  /** Replace an existing automation lane in place via an undoable edit. */
  editAutomationLane(trackId: number, laneIndex: number, desc: ProjectAutomationLaneDesc): void {
    this.native.editAutomationLane(trackId, laneIndex, projectAutomationLaneValue(desc));
  }

  /** Remove an automation lane from a track via an undoable edit. */
  removeAutomationLane(trackId: number, laneIndex: number): void {
    this.native.removeAutomationLane(trackId, laneIndex);
  }

  /** Undo the most recent edit (throws when the undo stack is empty). */
  undo(): void {
    this.native.undo();
  }

  /** Redo the most recently undone edit (throws when the redo stack is empty). */
  redo(): void {
    this.native.redo();
  }

  // -- MIDI --

  /**
   * Replace a MIDI clip's entire event list. Each event is
   * `{ ppq, data0, data1? }` (or a `[ppq, data0, data1]` tuple); pass an empty
   * array to clear. `data0`/`data1` are the first two UMP-1.0 words of a
   * channel-voice message (stored opaquely).
   */
  setMidiEvents(
    clipId: number,
    events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
  ): void {
    assertProjectMidiEvents('Project.setMidiEvents', events);
    this.native.setMidiEvents(clipId, events);
  }

  /** Import an in-memory SMF buffer; returns the first added clip id. */
  importSmf(data: Buffer | Uint8Array): number {
    return this.native.importSmf(data);
  }

  /** Export the project's tempo map + MIDI clips to an SMF byte buffer. */
  exportSmf(): Buffer {
    return this.native.exportSmf();
  }

  /**
   * Import a MIDI 2.0 Clip File (`SMF2CLIP`); returns the first added clip id.
   * Unlike {@link importSmf}, MIDI 2.0 channel-voice messages (16-bit velocity,
   * 32-bit CC, per-note / registered controllers, bank-valid Program Change)
   * survive without loss.
   */
  importClipFile(data: Buffer | Uint8Array): number {
    return this.native.importClipFile(data);
  }

  /**
   * Export the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
   * (`SMF2CLIP`) byte buffer. MIDI 2.0-only events are written without loss —
   * prefer this over {@link exportSmf} when MIDI 2.0 fidelity matters.
   */
  exportClipFile(): Buffer {
    return this.native.exportClipFile();
  }

  /**
   * Set a MIDI clip's channel-0 program / bank at source PPQ 0. `bank` defaults
   * to `-1` (no Bank Select emitted), matching `setProgramOnChannel` and the
   * Python/WASM surfaces; pass `>= 0` to emit a Bank Select.
   */
  setProgram(clipId: number, program: number, bank = -1): void {
    this.native.setProgram(clipId, program, bank);
  }

  /** Set a MIDI clip's program / bank for one UMP group and channel. */
  setProgramOnChannel(
    clipId: number,
    group: number,
    channel: number,
    program: number,
    bank = -1,
  ): void {
    this.native.setProgramOnChannel(clipId, group, channel, program, bank);
  }

  /** Destructively bake a MIDI-FX chain into a clip's stored MIDI events. */
  bakeMidiFx(clipId: number, configJson: string): void {
    this.native.bakeMidiFx(clipId, configJson);
  }

  /** Backward alias for {@link bakeMidiFx}. */
  setMidiFx(clipId: number, configJson: string): void {
    this.bakeMidiFx(clipId, configJson);
  }

  /**
   * Validate that every note-on in a MIDI clip has a matching note-off.
   *
   * @param clipId Target MIDI clip id.
   * @returns `ok` is `true` when fully paired; `unmatchedNoteOns` /
   *          `unmatchedNoteOffs` count the dangling events of each kind.
   * @throws If `clipId` is not a MIDI clip.
   */
  validateMidiNotes(clipId: number): {
    ok: boolean;
    unmatchedNoteOns: number;
    unmatchedNoteOffs: number;
  } {
    return this.native.validateMidiNotes(clipId);
  }

  // -- MIR --

  /** Detect tempo from a mono buffer and install it (undoable); returns the primary BPM. */
  autoTempo(audio: Float32Array, sampleRate: number): number {
    return this.native.autoTempo(audio, sampleRate);
  }

  /**
   * Snap a PPQ coordinate to the nearest beat of the project grid. `strength`
   * in `[0, 1]` (0 = no snap, 1 = exact grid line).
   */
  snapToGrid(ppq: number, strength = 1.0): number {
    return this.native.snapToGrid(ppq, strength);
  }

  /**
   * Replace the project's key annotation stream via an undoable edit (existing
   * chord / section / onset annotations are preserved).
   */
  annotateKeys(keys: ProjectKeySegment[]): void {
    this.native.annotateKeys(keys);
  }

  /** Replace the project's chord-symbol annotation stream via an undoable edit. */
  annotateChords(chords: ProjectChordSymbol[]): void {
    this.native.annotateChords(chords);
  }

  // -- assist sidecars --

  /**
   * Add or update an opaque assist sidecar (keyed by module id + target scope)
   * via an undoable edit. The payload bytes are copied.
   */
  setAssistSidecar(sidecar: ProjectAssistSidecarInput): void {
    this.native.setAssistSidecar(sidecar);
  }

  /** Number of assist sidecars currently stored on the project. */
  assistSidecarCount(): number {
    return this.native.assistSidecarCount();
  }

  /** Read one assist sidecar by stable project order. */
  getAssistSidecar(index: number): ProjectAssistSidecar {
    return this.native.getAssistSidecar(index);
  }

  /** Read every stored assist sidecar as an array (stable project order). */
  assistSidecars(): ProjectAssistSidecar[] {
    return this.native.assistSidecars();
  }

  // -- compile / render --

  /** Compile the project into an RT-readable timeline, surfacing diagnostics. */
  compile(): ProjectCompileResult {
    return this.native.compile();
  }

  /** Retrieve the compile result captured by the most recent {@link bounce}. */
  lastBounceCompileResult(): ProjectCompileResult {
    return this.native.lastBounceCompileResult();
  }

  /**
   * Compile + render the project offline to an interleaved float buffer
   * (`totalFrames * channels` samples). Deterministic: the same project +
   * options yields a bit-identical array within one build.
   *
   * Omitting `options.totalFrames` (or passing `<= 0`) auto-derives the render
   * length from the arrangement rather than producing an empty render.
   *
   * MIDI tracks routed to a destination render as silence here, because no
   * instrument is bound. To audition MIDI through the built-in synth, use
   * {@link bounceWithBuiltinInstrument} / {@link bounceWithBuiltinInstruments}.
   */
  bounce(options: ProjectBounceOptions = {}): Float32Array {
    return this.native.bounce(options);
  }

  /**
   * Like {@link bounce}, but renders MIDI tracks routed to a destination
   * through the built-in oscillator synth so a MIDI-only arrangement bounces
   * to audible audio. Each entry of `instruments` binds a
   * {@link BuiltinInstrumentConfig} patch to a `destinationId` (default `0`).
   * An empty array renders silence, identical to {@link bounce}.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithBuiltinInstruments(
    instruments: BuiltinInstrumentConfig[] = [],
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithBuiltinInstruments(instruments, options);
  }

  /**
   * Convenience wrapper over {@link bounceWithBuiltinInstruments} for the
   * common single-instrument case. Pass a {@link BuiltinInstrumentConfig}
   * (e.g. `{ waveform: 'saw', destinationId: 0 }`) or a bare
   * {@link SynthWaveform} name to bind one built-in synth patch. The
   * `destinationId` field is a JS binding convenience, not part of the
   * oscillator patch itself.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithBuiltinInstrument(
    instrument: BuiltinInstrumentConfig | SynthWaveform = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    const config: BuiltinInstrumentConfig =
      typeof instrument === 'string' ? { waveform: instrument } : instrument;
    return this.native.bounceWithBuiltinInstruments([config], options);
  }

  /**
   * Like {@link bounce}, but renders MIDI tracks routed to a destination
   * through the patch-driven NativeSynth — the full synthesizer (subtractive /
   * FM / Karplus-Strong / modal / additive / percussion /
   * extended-waveguide-piano engines plus the realism layer). Each entry of
   * `instruments` binds a {@link SynthPatch} (or a preset-name string such as
   * `'saw-lead'` / `'va:saw-lead'`; see {@link synthPresetNames}) to a
   * `destinationId` (default `0`). `destinationId` is a JS binding convenience,
   * not part of the NativeSynth patch itself. An empty array renders silence.
   * Unknown preset names throw. Deterministic for a fixed project + options +
   * patch.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithSynthInstruments(
    instruments: (SynthPatch | string)[] = [],
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSynthInstruments(instruments, options);
  }

  /**
   * Convenience wrapper over {@link bounceWithSynthInstruments} for the common
   * single-instrument case. Pass a {@link SynthPatch} or a bare preset name
   * (`'saw-lead'` / `'va:saw-lead'`).
   */
  bounceWithSynthInstrument(
    instrument: SynthPatch | string = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSynthInstruments([instrument], options);
  }

  /**
   * Load (parse) SoundFont 2 bytes into the project: presets / instruments /
   * sample headers plus the sample PCM decoded to a float pool. Replaces any
   * previously loaded SoundFont; the input buffer is not referenced after the
   * call. Throws on malformed input (the previous SoundFont is kept).
   */
  loadSoundFont(data: Uint8Array): void {
    this.native.loadSoundFont(data);
  }

  /** Release the project's loaded SoundFont (no-op when none is loaded). */
  clearSoundFont(): void {
    this.native.clearSoundFont();
  }

  /** Number of presets in the loaded SoundFont (0 when none is loaded). */
  soundFontPresetCount(): number {
    return this.native.soundFontPresetCount();
  }

  /**
   * Enumerate every (channel, bank, program) combination the arrangement plays
   * a note through, in first-use order, reporting whether each resolves in the
   * loaded SoundFont (`'sf2'`, GS variation/drum fallbacks included) or would
   * fall back to the built-in synth (`'synth'`). Without a loaded SoundFont
   * every entry is a synth fallback.
   */
  soundFontManifest(): Sf2ProgramStatus[] {
    return this.native.soundFontManifest();
  }

  /**
   * Like {@link bounceWithBuiltinInstruments}, but each bound destination
   * renders through a GS-compatible SoundFont player fed by the project's
   * loaded SoundFont ({@link loadSoundFont}): 16 MIDI channels per player,
   * channel 10 drums via bank 128, GS NRPN part edits and GS/GM SysEx resets
   * honored. Programs the SoundFont does not cover — including bouncing with
   * no SoundFont loaded at all — play through the built-in synthesizer GM
   * fallback bank (the data-free floor; see {@link soundFontManifest} for the
   * per-program backend). An empty array renders silence.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithSf2Instruments(
    instruments: Sf2InstrumentConfig[] = [],
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSf2Instruments(instruments, options);
  }

  /**
   * Convenience wrapper over {@link bounceWithSf2Instruments} for the common
   * single-instrument case.
   */
  bounceWithSf2Instrument(
    instrument: Sf2InstrumentConfig = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSf2Instruments([instrument], options);
  }

  /** Release the underlying native project. Idempotent. */
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

  /** Releases the native project; lets `using` (Node 22+) free it automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

function trackKindValue(kind: ProjectTrackDesc['kind']): number {
  if (kind === undefined || kind === 'audio') {
    return 0;
  }
  if (kind === 'midi') {
    return 1;
  }
  if (kind === 'aux') {
    return 2;
  }
  if (typeof kind === 'number') {
    return kind;
  }
  throw new Error(`Invalid track kind: ${kind}`);
}

function warpModeValue(mode: WarpMode | number | undefined): number {
  if (mode === undefined || mode === 'off') {
    return 0;
  }
  if (mode === 'repitch') {
    return 1;
  }
  if (mode === 'tempo-sync') {
    return 2;
  }
  if (typeof mode === 'number') {
    return mode;
  }
  throw new Error(`Invalid warp mode: ${mode}`);
}

function engineAutomationCurveValue(curve: EngineAutomationPointCurve | undefined): number {
  if (curve === undefined) {
    return 0;
  }
  if (typeof curve === 'number') {
    return curve;
  }
  return automationCurveValue(curve);
}

function engineAutomationPointValue(point: EngineAutomationPoint): EngineAutomationPoint {
  return {
    ...point,
    curveToNext: engineAutomationCurveValue(point.curveToNext) as EngineAutomationPointCurve,
  };
}

function projectFadeCurveValue(curve: ProjectFadeCurve | undefined | null): number | undefined {
  if (curve === undefined || curve === null) {
    return undefined;
  }
  if (typeof curve === 'number') {
    return curve;
  }
  if (curve === 'linear') {
    return 0;
  }
  if (curve === 'equalPower' || curve === 'equal-power' || curve === 'equal_power') {
    return 1;
  }
  if (curve === 'exponential') {
    return 2;
  }
  if (curve === 'logarithmic') {
    return 3;
  }
  throw new Error(`Invalid project fade curve: ${curve}`);
}

function projectClipFadeValue(fade: ProjectClipFade | undefined): ProjectClipFade | undefined {
  if (fade === undefined) {
    return undefined;
  }
  const curve = projectFadeCurveValue(fade.curve);
  return curve === undefined ? { ...fade } : { ...fade, curve: curve as ProjectFadeCurve };
}

function projectLoopModeValue(mode: ProjectLoopMode): number {
  if (typeof mode === 'number') {
    return mode;
  }
  if (mode === 'off') {
    return 0;
  }
  if (mode === 'loop') {
    return 1;
  }
  throw new Error(`Invalid project loop mode: ${mode}`);
}

function projectAutomationPointValue(point: ProjectAutomationPoint): ProjectAutomationPoint {
  const curve = engineAutomationCurveValue(point.curve ?? point.curveToNext);
  return {
    ...point,
    curve: curve as EngineAutomationPointCurve,
    curveToNext: curve as EngineAutomationPointCurve,
  };
}

function projectAutomationLaneValue(desc: ProjectAutomationLaneDesc): ProjectAutomationLaneDesc {
  return { ...desc, points: desc.points.map(projectAutomationPointValue) };
}

// ============================================================================
// Standalone functions
// ============================================================================

// -- Analysis --

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

export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  return addon.resample(samples, srcSr, targetSr);
}

export function mixingScenePresetNames(): string[] {
  return addon.mixingScenePresetNames();
}

export function mixingScenePresetJson(presetName: string): string {
  return addon.mixingScenePresetJson(presetName);
}

const PAN_LAW_VALUES: Record<PanLaw, number> = {
  const3dB: 0,
  'const4.5dB': 1,
  const6dB: 2,
  linear0dB: 3,
};

const METER_TAP_VALUES: Record<MeterTap, number> = {
  preFader: 0,
  postFader: 1,
};

const SEND_TIMING_VALUES: Record<SendTiming, number> = {
  preFader: 0,
  postFader: 1,
};

function automationCurveValue(curve: AutomationCurve): number {
  if (curve === 'linear') {
    return 0;
  }
  if (curve === 'exponential') {
    return 1;
  }
  if (curve === 'hold') {
    return 2;
  }
  if (curve === 's-curve') {
    return 3;
  }
  throw new Error(`Invalid automation curve: ${curve}`);
}

function panLawValue(panLaw: PanLaw | number): number {
  if (typeof panLaw === 'number') {
    return panLaw;
  }
  const value = PAN_LAW_VALUES[panLaw];
  if (value === undefined) {
    throw new Error(`Invalid pan law: ${panLaw}`);
  }
  return value;
}

function panModeValue(panMode: PanMode): number {
  if (typeof panMode === 'number') {
    return panMode;
  }
  const mode = panMode.replace(/_/g, '-').toLowerCase();
  if (mode === 'stereo-pan' || mode === 'stereopan' || mode === 'pan') {
    return 1; // SONARE_PAN_MODE_STEREO_PAN
  }
  if (mode === 'dual-pan' || mode === 'dualpan') {
    return 2; // SONARE_PAN_MODE_DUAL_PAN
  }
  return 0; // SONARE_PAN_MODE_BALANCE
}

function meterTapValue(tap: MeterTap | number): number {
  if (typeof tap === 'number') {
    return tap;
  }
  const value = METER_TAP_VALUES[tap];
  if (value === undefined) {
    throw new Error(`Invalid meter tap: ${tap}`);
  }
  return value;
}

function sendTimingValue(timing: SendTiming | number): number {
  if (typeof timing === 'number') {
    return timing;
  }
  const value = SEND_TIMING_VALUES[timing];
  if (value === undefined) {
    throw new Error(`Invalid send timing: ${timing}`);
  }
  return value;
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

  /** Rebuild and compile the routing graph from the current scene topology. */
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

  /** Maximum processor tail length (samples) currently in the compiled graph. */
  tailSamples(): number {
    return this.native.tailSamples();
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
export function mixStereo(
  leftChannels: Float32Array[],
  rightChannels: Float32Array[],
  sampleRate = 48000,
  options: MixOptions = {},
): MixResult {
  return addon.mixStereo(leftChannels, rightChannels, sampleRate, options);
}

export type {
  AcousticOptions,
  AcousticResult,
  AnalysisBeat,
  AnalysisChord,
  AnalysisDynamics,
  AnalysisMelody,
  AnalysisPitchPoint,
  AnalysisProgressCallback,
  AnalysisResult,
  AnalysisRhythm,
  AnalysisSection,
  AnalysisTimbre,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  AutomationCurve,
  BpmAnalysisResult,
  BpmCandidate,
  BuiltinInstrumentConfig,
  BuiltinSynthConfig,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordDetectionOptions,
  ChromaResult,
  ClipPageRequest,
  CqtResult,
  DynamicsResult,
  EngineAutomationPoint,
  EngineAutomationPointCurve,
  EngineBus,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineGraphConnection,
  EngineGraphMix,
  EngineGraphNode,
  EngineGraphNodeType,
  EngineGraphParameterBinding,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMetronomeConfig,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTelemetryError,
  EngineTelemetryType,
  EngineTrackLane,
  EngineTrackSend,
  EngineTransportState,
  EqBandInput,
  EqSpectrumSnapshot,
  GoniometerPoint,
  HpssResult,
  InverseMelResult,
  InverseStftResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  KeyMode,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainSection,
  MasteringChainStereoResult,
  MasteringOptions,
  MasteringResult,
  MasteringStereoResult,
  MelodyOptions,
  MelodyPoint,
  MelodyResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MidiCcBindOptions,
  MidiCcLearnOptions,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  NoteStretchOptions,
  PanLaw,
  PanMode,
  PitchResult,
  ProjectAutomationPoint,
  ProjectBounceOptions,
  ProjectClipDesc,
  ProjectCompileResult,
  ProjectDiagnostic,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectWarpAnchor,
  ProjectWarpMapDesc,
  RhythmResult,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometryOptions,
  RoomMorphOptions,
  Section,
  SectionTypeOrdinal,
  SendTiming,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SourceBackend,
  StftDbResult,
  StftResult,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamingPlatform,
  StreamQuantizeConfig,
  StripRef,
  SynthBodyType,
  SynthEngineMode,
  SynthEnumTables,
  SynthFilterModel,
  SynthFilterOutput,
  SynthModDestination,
  SynthModRouting,
  SynthModSource,
  SynthOscWaveform,
  SynthPatch,
  SynthWaveform,
  TimbreFrame,
  TimbreResult,
  TimeSignature,
} from './types.js';
