/**
 * Type definitions for the standalone realtime WASM module (`sonare-rt`).
 *
 * This is a DELIBERATELY REDUCED surface optimized for the AudioWorklet hot
 * path: a thin C ABI over {@link RealtimeEngine} that avoids embind, JS `val`
 * marshalling, and string handling so the audio render quantum stays
 * allocation- and GC-free. Use the full {@link RealtimeEngine} class from the
 * main `sonare` bundle when you need the complete feature set.
 *
 * Omitted here (available only on the full embind `RealtimeEngine`):
 * - Parameter registry & automation: `addParameter`, `parameterInfo`,
 *   `setAutomationLane`, `automationLaneCount`, `setParameter`,
 *   `setParameterSmoothed`.
 * - Transport read-back: `getTransportState` (transport is driven from the
 *   worklet; state is mirrored on the main thread there).
 * - Markers beyond seek: `setMarkers`, `markerCount`, `marker`,
 *   `setLoopFromMarkers`, `setTimeSignature`, `countInEndSample`.
 * - Graph topology: `setGraph`, `graphNodeCount`, `graphConnectionCount`.
 * - Clip scheduling: `setClips`, `clipCount`, `freezeOffline`.
 * - Capture read-back & offline rendering: `setCaptureBuffer`, `captureStatus`,
 *   `capturedAudio`, `resetCapture`, `renderOffline`, `bounceOffline`.
 * - Meter telemetry: `drainMeterTelemetry`.
 *
 * The exposed surface intentionally covers only transport, tempo/loop, marker
 * seek, metronome, capture arming/punch, block processing, and basic telemetry
 * drain.
 */
export interface SonareRtModuleOptions {
  locateFile?: (path: string, scriptDirectory: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
  wasmMemory?: WebAssembly.Memory;
  print?: (...args: unknown[]) => void;
  printErr?: (...args: unknown[]) => void;
}

export interface SonareRtModule {
  _malloc(size: number): number;
  _free(ptr: number): void;
  _sonare_rt_engine_abi_version(): number;
  _sonare_rt_engine_create(): number;
  _sonare_rt_engine_destroy(engine: number): void;
  _sonare_rt_engine_prepare(
    engine: number,
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity: number,
    telemetryCapacity: number,
  ): number;
  _sonare_rt_engine_play(engine: number, renderFrame: bigint): number;
  _sonare_rt_engine_stop(engine: number, renderFrame: bigint): number;
  _sonare_rt_engine_seek_sample(
    engine: number,
    timelineSample: bigint,
    renderFrame: bigint,
  ): number;
  _sonare_rt_engine_seek_ppq(engine: number, ppq: number, renderFrame: bigint): number;
  _sonare_rt_engine_set_tempo(engine: number, bpm: number): number;
  _sonare_rt_engine_set_loop(
    engine: number,
    startPpq: number,
    endPpq: number,
    enabled: number,
  ): number;
  _sonare_rt_engine_seek_marker(engine: number, markerId: number, renderFrame: bigint): number;
  _sonare_rt_engine_set_metronome_enabled(
    engine: number,
    enabled: number,
    beatGain: number,
    accentGain: number,
    clickSamples: number,
  ): number;
  _sonare_rt_engine_set_capture_armed(engine: number, armed: number): number;
  _sonare_rt_engine_set_capture_punch(
    engine: number,
    startSample: bigint,
    endSample: bigint,
    enabled: number,
  ): number;
  _sonare_rt_engine_process(
    engine: number,
    channelsPtr: number,
    numChannels: number,
    numFrames: number,
  ): void;
  _sonare_rt_engine_drain_telemetry(
    engine: number,
    typesErrorsValuesPtr: number,
    frameValuesPtr: number,
    maxRecords: number,
  ): number;
}

export default function SonareRt(options?: SonareRtModuleOptions): Promise<SonareRtModule>;
