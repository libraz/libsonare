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
  RealtimeVoiceChangerConfigInput,
  StreamConfig,
} from './index.js';
import type { SonareEngineSyncMessage } from './internal/worklet_messages.js';
import type {
  SonareWorkletMeterSnapshot,
  SonareWorkletSpectrumSnapshot,
} from './worklet/meter-rings.js';
import type { SonareEngineCommandRecord } from './worklet/protocol.js';

export interface SonareWorkletProcessorOptions {
  /**
   * Compiled `sonare.wasm` bytes, for a worklet realm where the module is not
   * initialized yet. Same recipe as {@link SonareRealtimeEngineWorkletProcessorOptions}:
   * `globalThis.SonareEmbindModuleFactory` builds the module from these bytes,
   * and the processor posts `{ type: 'ready' }` once it is up.
   */
  wasmBinary?: ArrayBuffer | Uint8Array;
  sceneJson: string;
  /**
   * Defaults to the AudioWorkletGlobalScope `sampleRate`; any other value is
   * refused with a RangeError. Outside a worklet scope it is required.
   */
  sampleRate?: number;
  /** Largest block the mixer is prepared for, in frames. */
  maxBlockSize?: number;
  stripCount?: number;
  meterIntervalFrames?: number;
  meterSharedBuffer?: SharedArrayBuffer;
  meterRingCapacity?: number;
  spectrumIntervalFrames?: number;
  spectrumBands?: number;
  spectrumSharedBuffer?: SharedArrayBuffer;
  spectrumRingCapacity?: number;
}

export interface SonareRealtimeEngineWorkletProcessorOptions {
  wasmBinary?: ArrayBuffer | Uint8Array;
  initialSyncMessages?: SonareEngineSyncMessage[];
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
   * Output plane count (default 2). Planes follow the canonical order
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
  numChannels?: number;
  meterIntervalFrames?: number;
  commandSharedBuffer?: SharedArrayBuffer;
  commandRingCapacity?: number;
  telemetrySharedBuffer?: SharedArrayBuffer;
  telemetryRingCapacity?: number;
  meterSharedBuffer?: SharedArrayBuffer;
  meterRingCapacity?: number;
  // Scope telemetry (FFT spectrum + goniometer): opt-in. The ring is created
  // only when scopeIntervalFrames > 0, since the per-block FFT is heavier than
  // the meter path. scopeBands selects the linear band resolution.
  scopeIntervalFrames?: number;
  scopeBands?: number;
  scopeSharedBuffer?: SharedArrayBuffer;
  scopeRingCapacity?: number;
  /**
   * Lock-free SPSC queue for clip-page cache misses. Supplying this is required
   * for realtime-safe OPFS streaming: it avoids both embind object creation and
   * postMessage structured cloning from AudioWorklet process().
   */
  clipPageRequestSharedBuffer?: SharedArrayBuffer;
  clipPageRequestRingCapacity?: number;
  /** Lock-free worklet-to-main-thread MIDI-1 output ring. */
  externalMidiSharedBuffer?: SharedArrayBuffer;
  externalMidiRingCapacity?: number;
  /**
   * Route the PFL/AFL cue bus to the processor's SECOND output instead of
   * folding it into the program output. Off by default, so an existing
   * single-output host keeps its current mix sample for sample. The node must
   * be constructed with two outputs for the cue to be audible.
   */
  cueOutput?: boolean;
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
  /** Channel count, 1 or 2. Default 1. */
  numChannels?: 1 | 2;
  /** Compiled module bytes, used when the worklet realm has not initialized the module. */
  wasmBinary?: ArrayBuffer | Uint8Array;
}

/** Options handed to the stream-analyzer worklet processor. */
export interface SonareStreamAnalyzerWorkletProcessorOptions {
  /** Mono samples per posted chunk. Integer of at least 128. Default 4096. */
  chunkFrames?: number;
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

export interface SonarePlaybackWorkletProcessorOptions {
  /** Renderer configuration; parsed once, in the processor constructor. Default `{}`. */
  config?: PlaybackRendererConfig | string;
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
   * `createSonarePlaybackNode` defaults it to the AudioContext's.
   */
  sampleRate?: number;
  /**
   * Compiled `sonare.wasm` bytes, for a worklet realm where the module is not
   * initialized yet. Used with `globalThis.SonareEmbindModuleFactory` (and
   * `SonareEmbindInitPromise` when the host starts the init itself), exactly as
   * for the realtime engine processor; the processor posts `{ type: 'ready' }`
   * once the module is up and renders silence until then.
   */
  wasmBinary?: ArrayBuffer | Uint8Array;
}

export interface SonarePlaybackNodeOptions extends SonarePlaybackWorkletProcessorOptions {
  processorName?: string;
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

/** The renderer's diagnostics plus the worklet's own block counter. */
export interface SonarePlaybackWorkletDiagnostics extends PlaybackDiagnostics {
  /**
   * Render quanta whose input channel count the renderer does not accept
   * (not 1, 2, 6 or 8 under `"auto"`; not the fixed layout's count otherwise).
   * Each was rendered as silence on the active input layout, so the timeline
   * still advanced.
   */
  unsupported_input_blocks: number;
}

export interface SonareRealtimeEngineNodeCapabilities {
  mode: 'sab' | 'postMessage';
  runtimeTarget: 'embind';
  sharedArrayBuffer: boolean;
  atomics: boolean;
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
  engineAbiVersion?: number;
  expectedEngineAbiVersion?: number;
  abiCompatible?: boolean;
  degradedReason?: string;
  readyMessage?: boolean;
}

export interface SonareRealtimeEngineNodeOptions
  extends SonareRealtimeEngineWorkletProcessorOptions {
  processorName?: string;
  moduleUrl?: string | URL;
  mode?: 'auto' | 'sab' | 'postMessage';
  engineAbiVersion?: number;
  expectedEngineAbiVersion?: number;
  requireAbiCompatible?: boolean;
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

/** A control-plane sync rejection as handed to an `onSyncError` listener. */
export interface SonareEngineSyncErrorMessage {
  /** Always `'syncError'`. */
  type: 'syncError';
  /** The refusal as raised on the audio thread, rebuilt with its class and any SonareError code. */
  failure?: Error;
  /**
   * Discriminant of the rejected sync. Usually a known member, but a
   * `sync`-prefixed type this build does not recognize is reported here too
   * rather than dropped, so the set is open. `Record<never, never>` keeps the
   * known members as editor completions instead of collapsing to `string`.
   */
  syncType: SonareEngineSyncMessage['type'] | (string & Record<never, never>);
  /** The refusal's message, as raised on the audio thread. */
  message: string;
}

/**
 * A bounded batch of page misses emitted by the AudioWorklet. The main thread
 * resolves these from OPFS and responds with `syncClipPage`; a missing page is
 * silent until that response arrives.
 */
export interface SonareEngineClipPageRequestMessage {
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
  onMeter?: (meter: SonareWorkletMeterSnapshot) => void;
  /** Receives each spectrum snapshot the mixer processor publishes. */
  onSpectrum?: (spectrum: SonareWorkletSpectrumSnapshot) => void;
}
