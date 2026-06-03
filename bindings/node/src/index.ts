import type {
  AcousticResult,
  AnalysisProgressCallback,
  AnalysisResult,
  AutomationCurve,
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChromaResult,
  CqtResult,
  DynamicsResult,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineCaptureStatus,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMetronomeConfig,
  EngineParameterInfo,
  EngineTelemetry,
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
  LufsResult,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
  MasteringResult,
  MasteringStereoResult,
  Matrix2D,
  MelodyResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  PairAnalysis,
  PairProcessor,
  PanLaw,
  PanMode,
  PitchResult,
  ProjectBounceOptions,
  ProjectClipDesc,
  ProjectCompileResult,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectTrackDesc,
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  RhythmResult,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomMorphOptions,
  Section,
  SendTiming,
  SoloProcessor,
  StereoAnalysis,
  StftDbResult,
  StftResult,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamingPlatform,
  StripRef,
  TempogramMode,
  TimbreResult,
  VoicePresetId,
} from './types.js';

// Runtime value re-exported from types (the rest of `./types.js` is type-only).
import { EXPECTED_PROJECT_ABI_VERSION } from './types.js';
import { addon } from './native.js';
import type { ValidateOptions } from './validation.js';
import {
  assertFiniteScalar,
  assertProjectMidiEvents,
  assertSamples,
  midi1Event,
} from './validation.js';

export { EXPECTED_PROJECT_ABI_VERSION };
export type { ValidateOptions } from './validation.js';
export * from './features.js';
export * from './analysis.js';
export * from './metering.js';
export * from './effects_mastering.js';

function chordChromaMethodValue(method: ChordChromaMethod): number {
  switch (method) {
    case 'stft':
      return 0;
    case 'nnls':
      return 1;
  }
  throw new Error(`Invalid chord chroma method: ${method}`);
}

export class Audio {
  private native: InstanceType<typeof addon.Audio>;

  private constructor(native: InstanceType<typeof addon.Audio>) {
    this.native = native;
  }

  static fromFile(path: string): Audio {
    return new Audio(addon.Audio.fromFile(path));
  }

  static fromBuffer(samples: Float32Array, sampleRate = 22050): Audio {
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
    this.native.destroy();
  }

  // -- Analysis --

  detectBpm(): number {
    return this.native.detectBpm();
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    return addon.detectKey(this.getData(), this.getSampleRate(), options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return addon.detectKeyCandidates(this.getData(), this.getSampleRate(), options);
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

  analyzeBpm(
    bpmMin = 30.0,
    bpmMax = 300.0,
    startBpm = 120.0,
    nFft = 2048,
    hopLength = 512,
    maxCandidates = 5,
  ): BpmAnalysisResult {
    return addon.analyzeBpm(
      this.getData(),
      this.getSampleRate(),
      bpmMin,
      bpmMax,
      startBpm,
      nFft,
      hopLength,
      maxCandidates,
    );
  }

  analyzeImpulseResponse(nOctaveBands = 6): AcousticResult {
    return addon.analyzeImpulseResponse(this.getData(), this.getSampleRate(), nOctaveBands);
  }

  detectAcoustic(
    nOctaveBands = 6,
    nThirdOctaveSubbands = 24,
    minDecayDb = 30.0,
    noiseFloorMarginDb = 10.0,
  ): AcousticResult {
    return addon.detectAcoustic(
      this.getData(),
      this.getSampleRate(),
      nOctaveBands,
      nThirdOctaveSubbands,
      minDecayDb,
      noiseFloorMarginDb,
    );
  }

  analyzeRhythm(
    bpmMin = 60.0,
    bpmMax = 200.0,
    startBpm = 120.0,
    nFft = 2048,
    hopLength = 512,
  ): RhythmResult {
    return addon.analyzeRhythm(
      this.getData(),
      this.getSampleRate(),
      bpmMin,
      bpmMax,
      startBpm,
      nFft,
      hopLength,
    );
  }

  analyzeDynamics(windowSec = 0.4, hopLength = 512, compressionThreshold = 6.0): DynamicsResult {
    return addon.analyzeDynamics(
      this.getData(),
      this.getSampleRate(),
      windowSec,
      hopLength,
      compressionThreshold,
    );
  }

  analyzeTimbre(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    nMfcc = 13,
    windowSec = 0.5,
  ): TimbreResult {
    return addon.analyzeTimbre(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      nMels,
      nMfcc,
      windowSec,
    );
  }

  detectChords(
    minDuration = 0.3,
    smoothingWindow = 2.0,
    threshold = 0.5,
    useTriadsOnly = false,
    nFft = 2048,
    hopLength = 512,
    useBeatSync = true,
    useHmm = false,
    hmmBeamWidth = 24,
    useKeyContext = false,
    keyRoot = 0,
    keyMode = 0,
    detectInversions = false,
    chromaMethod: ChordChromaMethod = 'stft',
  ): ChordAnalysisResult {
    return addon.detectChords(
      this.getData(),
      this.getSampleRate(),
      minDuration,
      smoothingWindow,
      threshold,
      useTriadsOnly,
      nFft,
      hopLength,
      useBeatSync,
      useHmm,
      hmmBeamWidth,
      useKeyContext,
      keyRoot,
      keyMode,
      detectInversions,
      chordChromaMethodValue(chromaMethod),
    );
  }

  chordFunctionalAnalysis(
    keyRoot: number,
    keyMode = 0,
    minDuration = 0.3,
    smoothingWindow = 2.0,
    threshold = 0.5,
    useTriadsOnly = false,
    nFft = 2048,
    hopLength = 512,
    useBeatSync = true,
    useHmm = false,
    hmmBeamWidth = 24,
    useKeyContext = false,
    detectInversions = false,
    chromaMethod: ChordChromaMethod = 'stft',
  ): string[] {
    return addon.chordFunctionalAnalysis(
      this.getData(),
      keyRoot,
      keyMode,
      this.getSampleRate(),
      minDuration,
      smoothingWindow,
      threshold,
      useTriadsOnly,
      nFft,
      hopLength,
      useBeatSync,
      useHmm,
      hmmBeamWidth,
      useKeyContext,
      detectInversions,
      chordChromaMethodValue(chromaMethod),
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

  noteStretch(onsetSample = 0, offsetSample = 0, stretchRatio = 1.0): Float32Array {
    return addon.noteStretch(
      this.getData(),
      this.getSampleRate(),
      onsetSample,
      offsetSample,
      stretchRatio,
    );
  }

  voiceChange(
    pitchSemitones = 0.0,
    formantFactor = 1.0,
    options: ValidateOptions = {},
  ): Float32Array {
    const data = this.getData();
    assertSamples('voiceChange', data, options.validate !== false);
    return addon.voiceChange(data, this.getSampleRate(), pitchSemitones, formantFactor);
  }

  normalize(targetDb = 0.0): Float32Array {
    return addon.normalize(this.getData(), this.getSampleRate(), targetDb);
  }

  mastering(targetLufs = -14.0, ceilingDb = -1.0, truePeakOversample = 4): MasteringResult {
    return addon.mastering(
      this.getData(),
      this.getSampleRate(),
      targetLufs,
      ceilingDb,
      truePeakOversample,
    );
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return addon.masteringProcess(processorName, this.getData(), this.getSampleRate(), params);
  }

  masteringChain(
    config: Record<string, number | boolean> = {},
    onProgress?: (progress: number, stage: string) => void,
  ): MasteringChainResult {
    if (onProgress) {
      return addon.masteringChainWithProgress(
        this.getData(),
        this.getSampleRate(),
        config,
        onProgress,
      );
    }
    return addon.masteringChain(this.getData(), this.getSampleRate(), config);
  }

  masterAudio(
    preset: MasteringPreset = 'pop',
    overrides: Record<string, number | boolean> = {},
    onProgress?: (progress: number, stage: string) => void,
  ): MasteringChainResult {
    if (onProgress) {
      return addon.masterAudioWithProgress(
        preset,
        this.getData(),
        this.getSampleRate(),
        overrides,
        onProgress,
      );
    }
    return addon.masterAudio(preset, this.getData(), this.getSampleRate(), overrides);
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

  melSpectrogram(nFft = 2048, hopLength = 512, nMels = 128): MelSpectrogramResult {
    return addon.melSpectrogram(this.getData(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  mfcc(nFft = 2048, hopLength = 512, nMels = 128, nMfcc = 20): MfccResult {
    return addon.mfcc(this.getData(), this.getSampleRate(), nFft, hopLength, nMels, nMfcc);
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

  seekPpq(ppq: number, renderFrame = -1): void {
    this.native.seekPpq(ppq, renderFrame);
  }

  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
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
    this.native.setAutomationLane(paramId, points);
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

  setClips(clips: EngineClip[]): void {
    this.native.setClips(clips);
  }

  clipCount(): number {
    return this.native.clipCount();
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

  /** Read the current engine transport state (playing/position/ppq/tempo). */
  getTransportState(): EngineTransportState {
    return this.native.getTransportState();
  }

  destroy(): void {
    this.native.destroy();
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

  // -- serialization --

  /** Serialize the project to deterministic JSON. */
  toJson(): string {
    return this.native.toJson();
  }

  /** Set the project sample rate in Hz (must be > 0). */
  setSampleRate(sampleRate: number): void {
    this.native.setSampleRate(sampleRate);
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

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Route a track's MIDI clips to a host/instrument destination id. */
  setTrackMidiDestination(trackId: number, destinationId: number): void {
    this.native.setTrackMidiDestination(trackId, destinationId);
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

  /** Set a MIDI clip's channel-0 program / bank at source PPQ 0. */
  setProgram(clipId: number, program: number, bank = 0): void {
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

  /** Configure and apply a clip's MIDI-FX chain from JSON. */
  setMidiFx(clipId: number, configJson: string): void {
    this.native.setMidiFx(clipId, configJson);
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

  // -- compile / render --

  /** Compile the project into an RT-readable timeline, surfacing diagnostics. */
  compile(): ProjectCompileResult {
    return this.native.compile();
  }

  /**
   * Compile + render the project offline to an interleaved float buffer
   * (`totalFrames * channels` samples). Deterministic: the same project +
   * options yields a bit-identical array within one build.
   */
  bounce(options: ProjectBounceOptions = {}): Float32Array {
    return this.native.bounce(options);
  }

  /** Release the underlying native project. Safe to call only once. */
  destroy(): void {
    this.native.destroy();
  }

  /** Alias for {@link destroy}, provided for cross-binding (WASM) compatibility. */
  delete(): void {
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

// ============================================================================
// Standalone functions
// ============================================================================

// -- Analysis --

export function version(): string {
  return addon.version();
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

  /** Set a strip's pan position (-1..1) with an optional pan mode. */
  setPan(strip: StripRef, pan: number, panMode: PanMode = 0): void {
    this.native.setPan(strip, pan, panModeValue(panMode));
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

  /** Read a strip's current (post-fader) meter snapshot. */
  stripMeter(strip: StripRef): MixMeterSnapshot {
    return this.native.stripMeter(strip);
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

  /** Release the underlying native mixer. Safe to call only once. */
  destroy(): void {
    this.native.destroy();
  }

  /** Alias for {@link destroy}, provided for cross-binding (WASM) compatibility. */
  delete(): void {
    this.destroy();
  }
}

export function mixStereo(
  leftChannels: Float32Array[],
  rightChannels: Float32Array[],
  sampleRate = 48000,
  options: MixOptions = {},
): MixResult {
  return addon.mixStereo(leftChannels, rightChannels, sampleRate, options);
}

export type {
  AcousticResult,
  AnalysisProgressCallback,
  AnalysisResult,
  AutomationCurve,
  BpmAnalysisResult,
  BpmCandidate,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChromaResult,
  CqtResult,
  DynamicsResult,
  EngineAutomationPoint,
  EngineAutomationPointCurve,
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
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringResult,
  MasteringStereoResult,
  MelodyPoint,
  MelodyResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  PanLaw,
  PanMode,
  PitchResult,
  ProjectBounceOptions,
  ProjectClipDesc,
  ProjectCompileResult,
  ProjectDiagnostic,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectTrackDesc,
  ProjectTrackKind,
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
  StftDbResult,
  StftResult,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamingPlatform,
  StripRef,
  TimbreFrame,
  TimbreResult,
  TimeSignature,
} from './types.js';
