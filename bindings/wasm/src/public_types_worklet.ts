/**
 * Option, capability and diagnostics types of the `./worklet` entry: what a host
 * passes to a processor or node factory and what it reads back, and the two
 * listener payloads a node hands to the host. The wire messages these processors
 * exchange stay in `internal/worklet_messages.ts`.
 */

import type { ClipPagePageRequest } from './clip_page_streamer.js';
import type {
  PlaybackDiagnostics,
  PlaybackRendererConfig,
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  StreamConfig,
} from './index.js';
import type { SonareEngineSyncMessage } from './internal/worklet_messages.js';
import type { WorkletMeterSnapshot, WorkletSpectrumSnapshot } from './worklet/meter-rings.js';
import type { SonareEngineCommandRecord } from './worklet/protocol.js';

/** Options of the mixer worklet processor (`SonareWorkletProcessor`). */
export interface SonareWorkletProcessorOptions {
  /**
   * Compiled `sonare.wasm` bytes, for a worklet realm where the module is not
   * initialized yet. Same recipe as {@link SonareRealtimeEngineWorkletProcessorOptions}:
   * `globalThis.SonareEmbindModuleFactory`, set to the default export of
   * `@libraz/libsonare/module`, builds the module from these bytes, and the
   * processor posts `{ type: 'ready' }` once it is up.
   */
  wasmBinary?: ArrayBuffer | Uint8Array;
  /** Mixer scene document as JSON. Required: a missing scene is a TypeError. */
  sceneJson: string;
  /**
   * Defaults to the AudioWorkletGlobalScope `sampleRate`; any other value is
   * refused with a RangeError. Outside a worklet scope it is required.
   */
  sampleRate?: number;
  /** Largest block the mixer is prepared for, in frames. Default 128. */
  maxBlockSize?: number;
  /** Strip count the host expects. Omitted: the scene's own. A different value is a RangeError. */
  stripCount?: number;
  /** Frames between meter publications, an integer of at least 0; 0 switches meters off. Default 2048. */
  meterIntervalFrames?: number;
  /** Shared memory from `createMeterRingBuffer`. Omitted: meters go to the transport instead. */
  meterSharedBuffer?: SharedArrayBuffer;
  /** Slots of `meterSharedBuffer`. Omitted: read from the buffer's header; a differing value is a RangeError. */
  meterRingCapacity?: number;
  /** Frames between spectrum publications, an integer of at least 0; 0 switches the spectrum off. Default 0. */
  spectrumIntervalFrames?: number;
  /** Spectrum bands per snapshot, an integer of at least 1. Omitted: the ring's count, else 16. */
  spectrumBands?: number;
  /** Shared memory from `createSpectrumRingBuffer`. Omitted: spectra go to the transport instead. */
  spectrumSharedBuffer?: SharedArrayBuffer;
  /** Slots of `spectrumSharedBuffer`. Omitted: read from the buffer's header; a differing value is a RangeError. */
  spectrumRingCapacity?: number;
}

/** Options of the `SonareWorkletProcessor` constructor: its `processorOptions` and the host it reports to. */
export interface SonareWorkletProcessorConstructorOptions extends SonareWorkletProcessorOptions {
  /** Where replies, meters and spectra go when the processor runs outside a worklet scope. Omitted: nowhere. */
  transport?: WorkletTransport;
}

/** Options of the realtime-engine worklet processor. Ring capacities apply together with their shared buffer. */
export interface SonareRealtimeEngineWorkletProcessorOptions {
  /**
   * Compiled `sonare.wasm` bytes, for a worklet realm where the module is not initialized yet; the
   * processor posts `{ type: 'ready' }` once it is up. Omitted: the module must already be
   * initialized in the realm, or built from the host's `SonareEmbindModuleFactory`.
   *
   * A bundled app imports the factory and the bytes through package specifiers. The processor
   * module, the one passed to `audioWorklet.addModule()`:
   *
   * @example
   * ```ts
   * import moduleFactory from '@libraz/libsonare/module';
   * import { type ModuleFactory, registerSonareRealtimeEngineWorkletProcessor } from '@libraz/libsonare/worklet';
   * (globalThis as { SonareEmbindModuleFactory?: ModuleFactory }).SonareEmbindModuleFactory =
   *   moduleFactory;
   * registerSonareRealtimeEngineWorkletProcessor();
   * ```
   *
   * The main thread, with the URLs the bundler gives the processor module and
   * `@libraz/libsonare/wasm`:
   *
   * @example
   * ```ts
   * import { SonareRealtimeEngineNode } from '@libraz/libsonare/worklet';
   * declare const context: AudioContext;
   * declare const processorUrl: string;
   * declare const wasmUrl: string;
   * const wasmBinary = await (await fetch(wasmUrl)).arrayBuffer();
   * const engine = await SonareRealtimeEngineNode.create(context, {
   *   moduleUrl: processorUrl,
   *   wasmBinary,
   *   numChannels: 2,
   * });
   * ```
   */
  wasmBinary?: ArrayBuffer | Uint8Array;
  /** Control-plane syncs applied in order once the module is up, before any queued port message. */
  initialSyncMessages?: SonareEngineSyncMessage[];
  /** Commands applied in order after `initialSyncMessages`, before any queued port message. */
  initialCommands?: SonareEngineCommandRecord[];
  /**
   * Engine rate. Defaults to the AudioWorkletGlobalScope `sampleRate` and any
   * other value is refused with a RangeError, since nothing resamples the
   * scope's quanta. Outside a worklet scope it is required. Node creation
   * defaults it to the AudioContext's.
   */
  sampleRate?: number;
  /** Largest render quantum the processor renders per call, in frames. Default 128. */
  maxBlockSize?: number;
  /**
   * Output plane count, required: the engine does not pick one, and in particular
   * does not follow `audioContext.destination.channelCount`. Planes follow the canonical order
   * L R C LFE Ls Rs [Lss Rss] for 6 (5.1) and 8 (7.1) planes.
   *
   * The Web Audio default destination is 2-channel `'speakers'`, so a wider
   * output is mixed down to stereo unless the app raises
   * `audioContext.destination.channelCount` (up to its `maxChannelCount`) to
   * match. Interpretation matters only while the counts differ: `'speakers'`
   * applies the spec's down-mix for mono, stereo, quad and 5.1 sources only,
   * and `'discrete'` copies the first planes and drops the rest. With equal
   * counts the planes pass through unchanged in either mode.
   */
  numChannels: number;
  /** Frames between meter publications, an integer of at least 0; 0 switches meters off. Default 2048. */
  meterIntervalFrames?: number;
  /**
   * Shared memory of the command ring (`createSonareEngineCommandRingBuffer`). Omitted: commands
   * arrive as port messages.
   */
  commandSharedBuffer?: SharedArrayBuffer;
  /**
   * Slots of the command ring, an integer in [1, 65536]. Omitted: read from the buffer's header; a
   * differing value is a RangeError. Node creation: 128.
   */
  commandRingCapacity?: number;
  /**
   * Shared memory of the telemetry ring (`createEngineTelemetryRingBuffer`). Omitted:
   * telemetry is posted as port messages.
   */
  telemetrySharedBuffer?: SharedArrayBuffer;
  /**
   * Slots of the telemetry ring, an integer of at least 1. Omitted: read from the buffer's header;
   * a differing value is a RangeError. Node creation: 128.
   */
  telemetryRingCapacity?: number;
  /** Shared memory from `createMeterRingBuffer`. Omitted: meters are posted as port messages. */
  meterSharedBuffer?: SharedArrayBuffer;
  /**
   * Slots of the meter ring, an integer of at least 1. Omitted: read from the buffer's header; a
   * differing value is a RangeError. Node creation: 128.
   */
  meterRingCapacity?: number;
  // Scope telemetry (FFT spectrum + goniometer): opt-in. The ring is created
  // only when scopeIntervalFrames > 0, since the per-block FFT is heavier than
  // the meter path. scopeBands selects the linear band resolution.
  /**
   * Frames between scope publications, an integer of at least 0; 0 switches scope telemetry off.
   * Omitted: off, or one record per block when a `scopeSharedBuffer` is given.
   */
  scopeIntervalFrames?: number;
  /** Spectrum bands per scope record, an integer in [1, 64]. Omitted: 48. */
  scopeBands?: number;
  /** Shared memory from `createScopeRingBuffer`. Omitted: scope records are posted as port messages. */
  scopeSharedBuffer?: SharedArrayBuffer;
  /**
   * Slots of the scope ring, an integer of at least 1. Omitted: read from the buffer's header; a
   * differing value is a RangeError. Node creation: 64.
   */
  scopeRingCapacity?: number;
  /**
   * Lock-free SPSC queue for clip-page cache misses. Supplying this is required
   * for realtime-safe OPFS streaming: it avoids both embind object creation and
   * postMessage structured cloning from AudioWorklet process().
   */
  clipPageRequestSharedBuffer?: SharedArrayBuffer;
  /**
   * Slots of the clip-page request ring, an integer of at least 1. Omitted: read from the buffer's
   * header; a differing value is a RangeError. Node creation: 128.
   */
  clipPageRequestRingCapacity?: number;
  /** Lock-free worklet-to-main-thread MIDI-1 output ring. */
  externalMidiSharedBuffer?: SharedArrayBuffer;
  /**
   * Slots of the external-MIDI ring, an integer of at least 1. Omitted: read from the buffer's
   * header; a differing value is a RangeError. Node creation: 256.
   */
  externalMidiRingCapacity?: number;
  /**
   * Route the PFL/AFL cue bus to the processor's second output instead of
   * folding it into the program output. Default `false`, so a single-output
   * host keeps its mix sample for sample. The node must be constructed with
   * two outputs for the cue to be audible.
   */
  cueOutput?: boolean;
}

/** Options of the `SonareRealtimeEngineWorkletProcessor` constructor: its `processorOptions` and the host it reports to. */
export interface SonareRealtimeEngineWorkletProcessorConstructorOptions
  extends SonareRealtimeEngineWorkletProcessorOptions {
  /** Where replies and meters go when the processor runs outside a worklet scope. Omitted: nowhere. */
  transport?: WorkletTransport;
}

/**
 * The processor reads an external vocoder carrier from the first channel of its second input
 * (`inputs[1]`); a missing second input is a silent carrier. The carrier is ignored unless the
 * configuration selects `vocoderCarrierSource: 'external'`.
 */
export interface SonareRealtimeVoiceChangerWorkletProcessorOptions {
  /** Preset id, preset document or flat config. Default `'neutralMonitor'`. */
  config?: RealtimeVoiceChangerConfigInput;
  /**
   * Defaults to the AudioWorkletGlobalScope `sampleRate`; any other value is
   * refused with a RangeError. Outside a worklet scope it is required.
   */
  sampleRate?: number;
  /** Largest render quantum the processor accepts, a positive integer. Default 128. */
  maxBlockSize?: number;
  /** Channel count of the stream, 1 or 2. Required. */
  numChannels: 1 | 2;
  /** Compiled module bytes, used when the worklet realm has not initialized the module. */
  wasmBinary?: ArrayBuffer | Uint8Array;
}

/** Options for {@link SonareRealtimeVoiceChangerNode.create}. */
export interface SonareRealtimeVoiceChangerNodeOptions
  extends SonareRealtimeVoiceChangerWorkletProcessorOptions {
  /** Registered processor name. Default `'sonare-realtime-voice-changer-processor'`. */
  processorName?: string;
  /** Worklet module loaded with `audioWorklet.addModule` before the node is created. Omitted: no module is added. */
  moduleUrl?: string | URL;
  /** Creates the `AudioWorkletNode`. Omitted: `new AudioWorkletNode(context, name, options)`. */
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

/** What {@link SonareRealtimeVoiceChangerNode.status} reads from the worklet's changer. */
export interface SonareRealtimeVoiceChangerNodeStatus {
  /** Latency of the chain in samples, as `RealtimeVoiceChanger.latencySamples` reports it. */
  latencySamples: number;
  /** Blocks in which the chain discarded its state over a non-finite value; see `RealtimeVoiceChanger.nonFiniteDiscardCount`. */
  nonFiniteDiscardCount: number;
  /** The applied configuration, as `RealtimeVoiceChanger.config()` returns it. */
  config: RealtimeVoiceChangerConfig;
}

/** Options handed to the stream-analyzer worklet processor. */
export interface SonareStreamAnalyzerWorkletProcessorOptions {
  /** Mono samples per posted chunk. Integer of at least 128. Default 4096. */
  chunkFrames?: number;
}

/** Options of the `SonareStreamAnalyzerWorkletProcessor` constructor: its `processorOptions` and its port. */
export interface SonareStreamAnalyzerWorkletProcessorConstructorOptions
  extends SonareStreamAnalyzerWorkletProcessorOptions {
  /** The port chunks are posted on. Omitted: chunks are dropped. */
  port?: WorkletPort;
}

/** Options for creating a stream-analyzer worklet node. */
export interface SonareStreamAnalyzerNodeOptions
  extends SonareStreamAnalyzerWorkletProcessorOptions {
  /**
   * Analyzer configuration, passed to `StreamAnalyzer` as given. `sampleRate`
   * defaults to the context's; any other value throws a RangeError.
   */
  config?: Partial<StreamConfig>;
  /** Registered processor name. Default `'sonare-stream-analyzer-processor'`. */
  processorName?: string;
  /** Worklet module loaded with `audioWorklet.addModule` before the node is created. Omitted: no module is added. */
  moduleUrl?: string | URL;
  /** Creates the `AudioWorkletNode`. Omitted: `new AudioWorkletNode(context, name, options)`. */
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

/** Options of the playback worklet processor. */
export interface SonarePlaybackWorkletProcessorOptions {
  /** Renderer configuration; parsed once, in the processor constructor. Default `{}`. */
  config?: PlaybackRendererConfig;
  /**
   * SHRF v1 bytes, required for a headphones target (e.g. the package asset
   * `./hrtf/default.shrf`). Copied into the renderer at construction.
   */
  hrtf?: ArrayBuffer | Uint8Array;
  /** Largest render quantum the processor accepts. Default 128. */
  maxBlockSize?: number;
  /**
   * Defaults to the AudioWorkletGlobalScope `sampleRate`; any other value is
   * refused with a RangeError, since nothing resamples the scope's quanta.
   * Outside a worklet scope there is no default and the option is required.
   * `SonarePlaybackNode.create` defaults it to the AudioContext's.
   */
  sampleRate?: number;
  /**
   * Compiled `sonare.wasm` bytes, for a worklet realm where the module is not
   * initialized yet. Used with `globalThis.SonareEmbindModuleFactory`, set to the
   * default export of `@libraz/libsonare/module` (and
   * `SonareEmbindInitPromise` when the host starts the init itself), exactly as
   * for the realtime engine processor; the processor posts `{ type: 'ready' }`
   * once the module is up and renders silence until then.
   */
  wasmBinary?: ArrayBuffer | Uint8Array;
}

/** Options of the `SonarePlaybackWorkletProcessor` constructor: its `processorOptions` and its port. */
export interface SonarePlaybackWorkletProcessorConstructorOptions
  extends SonarePlaybackWorkletProcessorOptions {
  /** The port replies and diagnostics are posted on. Omitted: nowhere. */
  port?: WorkletPort;
}

/** Options of `SonarePlaybackNode.create`, on top of the processor options. */
export interface SonarePlaybackNodeOptions extends SonarePlaybackWorkletProcessorOptions {
  /** Registered processor name. Default `'sonare-playback-processor'`. */
  processorName?: string;
  /** Worklet module loaded with `audioWorklet.addModule` before the node is created. Omitted: no module is added. */
  moduleUrl?: string | URL;
  /** Creates the `AudioWorkletNode`. Omitted: `new AudioWorkletNode(context, name, options)`. */
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

/** The renderer's diagnostics plus the worklet's own block counter. */
export interface SonarePlaybackWorkletProcessorDiagnostics extends PlaybackDiagnostics {
  /**
   * Render quanta whose input channel count the renderer does not accept
   * (not 1, 2, 6 or 8 under `"auto"`; not the fixed layout's count otherwise).
   * Each was rendered as silence on the active input layout, so the timeline
   * still advanced.
   */
  unsupported_input_blocks: number;
}

/** What the realtime-engine node resolved at creation: transport, platform support and ABI state. */
export interface SonareRealtimeEngineNodeCapabilities {
  /** Transport in use: `'sab'` for shared-memory rings, `'postMessage'` for port messages. */
  mode: 'sab' | 'postMessage';
  /** Runtime behind the processor; always `'embind'`. */
  runtimeTarget: 'embind';
  /** Whether `SharedArrayBuffer` exists in the creating realm. */
  sharedArrayBuffer: boolean;
  /** Whether `Atomics` exists in the creating realm. */
  atomics: boolean;
  /** Whether `AudioWorkletNode` exists in the creating realm or a `nodeFactory` was supplied. */
  audioWorklet: boolean;
  /** True only when clip-page misses use the bounded SAB ring. */
  clipPageRequestsRealtimeSafe: boolean;
  /** True when external MIDI uses the SAB output ring rather than postMessage. */
  externalMidiRealtimeSafe: boolean;
  /**
   * True when the node carries a second output fed by the PFL/AFL cue bus. When
   * false the cue is folded into the program output, as it always was.
   */
  cueOutput: boolean;
  /** Engine ABI version of the loaded module. Omitted: not detected. */
  engineAbiVersion?: number;
  /** Engine ABI version the host expects. Omitted: not detected. */
  expectedEngineAbiVersion?: number;
  /** Whether the two ABI versions agree. Omitted: not detected. */
  abiCompatible?: boolean;
  /** Why `'postMessage'` was chosen over `'sab'`. Omitted: the transport was not degraded. */
  degradedReason?: string;
  /** Whether the processor reports readiness with a `ready` or `error` message the node awaits. */
  readyMessage?: boolean;
}

/** Options of `SonareRealtimeEngineNode.create`, on top of the processor options. */
export interface SonareRealtimeEngineNodeOptions
  extends SonareRealtimeEngineWorkletProcessorOptions {
  /** Registered processor name. Default `'sonare-realtime-engine-processor'`. */
  processorName?: string;
  /** Worklet module loaded with `audioWorklet.addModule` before the node is created. Omitted: no module is added. */
  moduleUrl?: string | URL;
  /**
   * Transport. Omitted or `'auto'`: shared-memory rings when `SharedArrayBuffer` and `Atomics`
   * exist, else port messages. `'sab'` without that support throws `SonareError` `NotSupported`.
   */
  mode?: 'auto' | 'sab' | 'postMessage';
  /** Engine ABI version of the loaded module. Omitted: read from the module. */
  engineAbiVersion?: number;
  /** Engine ABI version the host expects. Omitted: `engineAbiVersion` when given, else the version this package was built for. */
  expectedEngineAbiVersion?: number;
  /** Omitted or `true`: differing ABI versions throw `SonareError` `AbiMismatch`. `false`: the node is created anyway. */
  requireAbiCompatible?: boolean;
  /** Creates the `AudioWorkletNode`. Omitted: `new AudioWorkletNode(context, name, options)`. */
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

/** A control-plane sync rejection as handed to an `onSyncError` listener. */
export interface EngineSyncErrorMessage {
  /** Always `'syncError'`. */
  type: 'syncError';
  /**
   * The refusal as raised on the audio thread, rebuilt with its class and any SonareError code,
   * and named with the public entry whose call posted the sync (`SonareEngine.setTempo: ...`).
   */
  failure?: Error;
  /**
   * Discriminant of the rejected sync. Usually a known member, but a
   * `sync`-prefixed type this build does not recognize is reported here too
   * rather than dropped, so the set is open. `Record<never, never>` keeps the
   * known members as editor completions instead of collapsing to `string`.
   */
  syncType: SonareEngineSyncMessage['type'] | (string & Record<never, never>);
  /**
   * The refusal's message, prefixed with the public entry whose call posted the sync; a sync
   * posted outside any entry call (a background page pump) carries the bare text.
   */
  message: string;
}

/**
 * A bounded batch of page misses emitted by the AudioWorklet. The main thread
 * resolves these from OPFS and responds with `syncClipPage`; a missing page is
 * silent until that response arrives.
 */
export interface EngineClipPageRequestMessage {
  /** Always `'clipPageRequest'`. */
  type: 'clipPageRequest';
  /** The pages the audio thread missed, oldest first. */
  requests: ClipPagePageRequest[];
  /** Number of misses dropped because a bounded native or SAB request queue was full. */
  dropped?: number;
}

/** The message port a processor listens and replies on; an AudioWorkletProcessor `port` satisfies it. */
export interface WorkletPort {
  /** Sends a reply or notification to the host. */
  postMessage?: (message: unknown, transfer?: Transferable[]) => void;
  /** Receives host messages when the port is driven by assignment. */
  onmessage?: (event: { data: unknown }) => void;
  /** Receives host messages when the port is driven by listeners. */
  addEventListener?: (type: 'message', listener: (event: { data: unknown }) => void) => void;
  /** Starts delivery on a port that queues until started. */
  start?: () => void;
}

/**
 * In-process host for a processor run outside an AudioWorkletGlobalScope: where
 * its replies go and, for the mixer processor, its meter and spectrum snapshots.
 */
export interface WorkletTransport {
  /** Receives the processor's replies. */
  postMessage?: (message: unknown, transfer?: Transferable[]) => void;
  /** Receives each meter snapshot the mixer processor publishes. */
  onMeter?: (meter: WorkletMeterSnapshot) => void;
  /** Receives each spectrum snapshot the mixer processor publishes. */
  onSpectrum?: (spectrum: WorkletSpectrumSnapshot) => void;
}
