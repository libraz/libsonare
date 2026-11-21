import {
  entryClass,
  nameNativeError,
  refuse,
  refuseType,
  refuseUnknownKey,
  withEntry,
} from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { deinterleavePlanes, interleavePlanes, nativeVoiceConfig } from './internal/pod_config.js';
import {
  checkedVoicePresetId,
  VOICE_PRESET_IDS,
  voicePresetOrdinal,
} from './internal/voice_preset_ids.js';
import { getSonareModule } from './module_state.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  VoicePresetId,
} from './public_types.js';
import { assertBoundedInteger, assertSampleRate, assertString } from './validation.js';

/** Construction options for {@link RealtimeVoiceChanger}. */
export interface RealtimeVoiceChangerOptions {
  /** Sample rate in Hz, an integer in [8000, 384000]. Required. */
  sampleRate: number;
  /** Largest block the process calls accept, a positive integer. Omit it for the library default; an explicit 0 is refused. */
  maxBlockSize?: number;
  /** Channel count of the stream, 1 or 2. Required. */
  numChannels: 1 | 2;
  /** Preset id, preset document or flat config. Default `'neutralMonitor'`. */
  config?: RealtimeVoiceChangerConfigInput;
}

/** Result of {@link realtimeVoiceChangerValidatePresetJson}. */
export interface RealtimeVoiceChangerValidatePresetJsonResult {
  /** Whether the document was accepted. */
  ok: boolean;
  /** The document with defaults filled in; present when `ok`. */
  normalizedJson?: string;
  /** Why the document was refused; present when not `ok`. */
  error?: string;
}

/**
 * Zero-copy realtime buffer set for {@link RealtimeVoiceChanger} mono
 * processing. `input`, `output` and `carrier` are typed-memory views onto the
 * WASM heap: write samples into `input` (and `carrier` for an external vocoder
 * carrier), call `process()` or `processWithCarrier()`, then read `output`.
 * The views are owned by the {@link RealtimeVoiceChanger} and remain valid
 * until `destroy()` is called on it.
 */
export interface RealtimeVoiceChangerMonoBuffer {
  /** Heap view receiving the mono input block. */
  input: Float32Array;
  /** Heap view holding the processed mono block after `process()`. */
  output: Float32Array;
  /** Mono external vocoder carrier, one sample per frame. */
  carrier: Float32Array;
  /** Processes `input` into `output` without an external carrier. */
  process: () => void;
  /** Like `process()`, reading the external carrier from `carrier`. */
  processWithCarrier: () => void;
}

/**
 * Zero-copy realtime buffer set for {@link RealtimeVoiceChanger} interleaved
 * multi-channel processing. Layout is L0,R0,L1,R1,... for stereo. The views
 * are owned by the {@link RealtimeVoiceChanger}.
 */
export interface RealtimeVoiceChangerInterleavedBuffer {
  /** Heap view receiving the interleaved input block (L0,R0,L1,R1,...). */
  input: Float32Array;
  /** Heap view holding the interleaved processed block after `process()`. */
  output: Float32Array;
  /** Mono external vocoder carrier, one sample per frame, shared by every channel. */
  carrier: Float32Array;
  /** Channel count of the interleaved layout, 1 or 2. */
  readonly numChannels: 1 | 2;
  /** Processes `input` into `output` without an external carrier. */
  process: () => void;
  /** Like `process()`, reading the external carrier from `carrier`. */
  processWithCarrier: () => void;
}

/**
 * Zero-copy realtime buffer for {@link RealtimeVoiceChanger} planar
 * processing. Each entry in `channels` is a heap-backed `Float32Array` for one
 * channel (matching AudioWorklet's native layout). Process happens in place:
 * write samples into each channel view, call `process()`, then read back from
 * the same views.
 */
export interface RealtimeVoiceChangerPlanarBuffer {
  /** One heap view per channel, processed in place. */
  channels: Float32Array[];
  /** Mono external vocoder carrier, one sample per frame, shared by every channel. */
  carrier: Float32Array;
  /** Processes `channels` in place without an external carrier. */
  process: () => void;
  /** Like `process()`, reading the external carrier from `carrier`. */
  processWithCarrier: () => void;
}

function assertChannelCount(numChannels: unknown): asserts numChannels is 1 | 2 {
  if (typeof numChannels !== 'number') {
    throw refuseType(`numChannels must be 1 or 2`);
  }
  if (numChannels !== 1 && numChannels !== 2) {
    throw refuse(`numChannels must be 1 or 2`);
  }
}

/** Checks a planar channel list (1 or 2 equal-length planes) and the optional carrier. */
function assertPlanes(channels: unknown, carrier?: unknown): Float32Array[] {
  if (!Array.isArray(channels)) {
    throw refuseType(`channels must be an array of Float32Array`);
  }
  assertChannelCount(channels.length);
  channels.forEach((plane, i) => {
    if (!(plane instanceof Float32Array)) {
      throw refuseType(`channels[${i}] must be a Float32Array`);
    }
    if (plane.length !== channels[0].length) {
      throw refuse(`channels must have equal lengths`);
    }
  });
  if (carrier !== undefined && !(carrier instanceof Float32Array)) {
    throw refuseType(`carrier must be a Float32Array`);
  }
  return channels as Float32Array[];
}

// ============================================================================
// RealtimeVoiceChanger Class
// ============================================================================

/** Realtime voice-changer chain: pitch, formant, EQ, dynamics, harmony, vocoder, character and space stages over mono or stereo blocks. Call {@link RealtimeVoiceChanger.destroy} when done. */
@entryClass('RealtimeVoiceChanger', [
  'processInterleavedInto',
  'processMono',
  'processMonoInto',
  'changer',
])
export class RealtimeVoiceChanger {
  private changerHandle: import('./sonare.js').WasmRealtimeVoiceChanger;
  private released = false;

  private get changer(): import('./sonare.js').WasmRealtimeVoiceChanger {
    assertHandleLive(this.released, 'RealtimeVoiceChanger');
    return this.changerHandle;
  }

  /**
   * Creates a voice changer prepared for `options.sampleRate`.
   *
   * @throws TypeError for a wrong-typed or unknown option, or a missing `sampleRate`;
   *   RangeError for an out-of-domain `sampleRate`, `maxBlockSize` (explicit 0 included),
   *   `numChannels` or an unknown preset id or token.
   */
  constructor(options: RealtimeVoiceChangerOptions) {
    if (typeof options !== 'object' || options === null || Array.isArray(options)) {
      throw refuseType(`options must be an object with a sampleRate`);
    }
    for (const key of Object.keys(options)) {
      if (!CONSTRUCTOR_KEYS.includes(key)) {
        throw refuseUnknownKey(key, CONSTRUCTOR_KEYS);
      }
    }
    assertSampleRate(options.sampleRate);
    const maxBlockSize = options.maxBlockSize ?? undefined;
    const numChannels = options.numChannels;
    const nativeConfig = nativeVoiceConfig(options.config ?? 'neutralMonitor');
    this.changerHandle = getSonareModule().createRealtimeVoiceChanger(nativeConfig);
    // The handle is already allocated; a throwing constructor leaves `this` unreachable and
    // embind has no GC finalizer, so release it here.
    let prepared = false;
    try {
      this.changerHandle.prepare(options.sampleRate, maxBlockSize, numChannels);
      prepared = true;
    } finally {
      if (!prepared) {
        this.changerHandle.delete();
      }
    }
  }

  /** Clears the streaming DSP state (filters, envelopes, reverb, limiter) and keeps the configuration. Not for use while a block is being processed. */
  reset(): void {
    this.changer.reset();
  }

  /**
   * Replaces the configuration.
   *
   * @throws TypeError for a wrong-typed config; RangeError for an unknown preset id or token or
   *   a field outside its range (nothing is clamped); the previous configuration stays in effect.
   */
  setConfig(config: RealtimeVoiceChangerConfigInput): void {
    this.changer.setConfig(nativeVoiceConfig(config));
  }

  /**
   * The applied configuration as a flat record, in the form {@link setConfig} and the
   * constructor accept, so `other.setConfig(changer.config())` reproduces it. An automatic
   * retune grain reads back as an absent `retuneGrainSizeSamples`.
   */
  config(): RealtimeVoiceChangerConfig {
    return this.changer.config();
  }

  /** Current configuration as a JSON preset document. */
  configJson(): string {
    return this.changer.configJson();
  }

  /** Latency of the chain in samples. */
  get latencySamples(): number {
    return this.changer.latencySamples();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.changer.maxBlockSize();
  }

  /**
   * Blocks in which the chain discarded its own state because a
   * non-finite value had reached it.
   *
   * Advisory telemetry, and the only thing that separates a degraded stream
   * from a clean one. Every stage of this chain leaves an in-domain finite
   * value where a non-finite one was, so the output stays finite, in range
   * and free of any error while carrying samples unrelated to the input. A
   * non-zero count is what says the samples in between were not computed from
   * what you supplied.
   *
   * Monotonic for the lifetime of the instance. The unit is one processed
   * block, never a channel.
   *
   * @example
   * ```ts
   * changer.processPlanar([left, right]);
   * if (changer.nonFiniteDiscardCount > 0) {
   *   // the audio just produced is not a function of the input
   * }
   * ```
   */
  get nonFiniteDiscardCount(): number {
    return this.changer.nonFiniteDiscardCount();
  }

  /**
   * Processes a mono block and returns a new array of the same length. `carrier` is an external
   * vocoder carrier of the same length, read only while the vocoder is enabled with
   * `vocoderCarrierSource: 'external'`; otherwise it is ignored.
   *
   * @throws RangeError when `carrier.length` differs from `samples.length`.
   */
  processMono(samples: Float32Array, carrier?: Float32Array): Float32Array {
    return this.changer.processMono(samples, carrier);
  }

  /** Realtime hot path: {@link RealtimeVoiceChanger.processMono} writing into `output`, which has the length of `samples`. */
  processMonoInto(samples: Float32Array, output: Float32Array, carrier?: Float32Array): void {
    this.changer.processMonoInto(samples, carrier, output);
  }

  /**
   * Processes planar blocks (one `Float32Array` per channel, 1 or 2, equal lengths) and returns
   * the processed planes without touching the inputs. `carrier` is one mono external vocoder
   * carrier of one sample per frame, shared by every channel.
   *
   * @throws TypeError for a wrong-typed argument; RangeError for another channel count or
   *   unequal lengths; SonareError `InvalidState` when more channels are given than the changer
   *   was prepared for.
   */
  processPlanar(channels: Float32Array[], carrier?: Float32Array): Float32Array[] {
    const planes = assertPlanes(channels, carrier);
    if (planes.length === 1) {
      return [this.changer.processMono(planes[0] as Float32Array, carrier)];
    }
    return deinterleavePlanes(
      this.changer.processInterleaved(interleavePlanes(planes), carrier, 2),
      2,
    );
  }

  /**
   * Realtime hot path: processes an interleaved block (L0,R0,L1,R1,...) into `output`, which has
   * the length of `input`. `carrier` is a mono carrier of one sample per frame shared by every channel.
   *
   * @throws RangeError unless `numChannels` is 1 or 2.
   */
  processInterleavedInto(
    input: Float32Array,
    numChannels: 1 | 2,
    output: Float32Array,
    carrier?: Float32Array,
  ): void {
    try {
      assertChannelCount(numChannels);
    } catch (error) {
      throw nameNativeError(error, 'RealtimeVoiceChanger.processInterleavedInto');
    }
    this.changer.processInterleavedInto(input, carrier, numChannels, output);
  }

  /**
   * Holds MIDI note `note` from the start of the next processed block. Held notes become the
   * voices' pitch targets, the newest leading, up to `1 + harmonyVoices` of them.
   *
   * @param note MIDI note number, integer in [0, 127].
   * @param velocity In (0, 1]; default 1. Validated only: it does not change the sound.
   * @throws SonareError with code `InvalidState` on a grain-engine changer or a full event queue.
   */
  noteOn(note: number, velocity = 1.0): void {
    this.changer.noteOn(note, velocity);
  }

  /** Releases `note`; releasing a note that is not held does nothing. */
  noteOff(note: number): void {
    this.changer.noteOff(note);
  }

  /** Releases every held note, including one kept by `midiRelease: 'hold'`. */
  allNotesOff(): void {
    this.changer.allNotesOff();
  }

  /**
   * Zero-copy mono path: returns heap views for input, output and the external carrier plus
   * `process()` / `processWithCarrier()` thunks wired to `numSamples`. The views are reused
   * across calls and become invalid after {@link destroy}.
   */
  createRealtimeMonoBuffer(numSamples: number): RealtimeVoiceChangerMonoBuffer {
    let input: Float32Array;
    let output: Float32Array;
    let carrier: Float32Array;
    let generation = -1;
    // The cached views detach if WASM memory grows or prepare replaces the scratch buffers.
    const acquire = (): void => {
      const changer = this.changer;
      input = changer.getMonoInputBuffer(numSamples);
      output = changer.getMonoOutputBuffer(numSamples);
      carrier = changer.getCarrierBuffer(numSamples);
      generation = changer.bufferGeneration();
    };
    const fresh = (): void => {
      if (
        generation !== this.changer.bufferGeneration() ||
        input.byteLength === 0 ||
        output.byteLength === 0 ||
        carrier.byteLength === 0
      ) {
        acquire();
      }
    };
    acquire();
    return {
      get input(): Float32Array {
        fresh();
        return input;
      },
      get output(): Float32Array {
        fresh();
        return output;
      },
      get carrier(): Float32Array {
        fresh();
        return carrier;
      },
      process: () => {
        fresh();
        this.changer.processPreparedMono(numSamples, undefined);
      },
      processWithCarrier: () => {
        fresh();
        this.changer.processPreparedMono(numSamples, true);
      },
    };
  }

  /** Same as {@link createRealtimeMonoBuffer} but for interleaved I/O. */
  createRealtimeInterleavedBuffer(
    numFrames: number,
    numChannels: 1 | 2,
  ): RealtimeVoiceChangerInterleavedBuffer {
    assertChannelCount(numChannels);
    let input: Float32Array;
    let output: Float32Array;
    let carrier: Float32Array;
    let generation = -1;
    const acquire = (): void => {
      const changer = this.changer;
      input = changer.getInterleavedInputBuffer(numFrames, numChannels);
      output = changer.getInterleavedOutputBuffer(numFrames, numChannels);
      carrier = changer.getCarrierBuffer(numFrames);
      generation = changer.bufferGeneration();
    };
    const fresh = (): void => {
      if (
        generation !== this.changer.bufferGeneration() ||
        input.byteLength === 0 ||
        output.byteLength === 0 ||
        carrier.byteLength === 0
      ) {
        acquire();
      }
    };
    acquire();
    return {
      get input(): Float32Array {
        fresh();
        return input;
      },
      get output(): Float32Array {
        fresh();
        return output;
      },
      get carrier(): Float32Array {
        fresh();
        return carrier;
      },
      numChannels,
      process: () => {
        fresh();
        this.changer.processPreparedInterleaved(numFrames, numChannels, undefined);
      },
      processWithCarrier: () => {
        fresh();
        this.changer.processPreparedInterleaved(numFrames, numChannels, true);
      },
    };
  }

  /**
   * Same as {@link createRealtimeMonoBuffer} for planar I/O: one heap-backed `Float32Array` per
   * channel, processed in place.
   */
  createRealtimePlanarBuffer(
    numFrames: number,
    numChannels: 1 | 2,
  ): RealtimeVoiceChangerPlanarBuffer {
    assertChannelCount(numChannels);
    let channels: Float32Array[] = [];
    let carrier: Float32Array;
    let generation = -1;
    const acquire = (): void => {
      const changer = this.changer;
      channels = [];
      for (let ch = 0; ch < numChannels; ch++) {
        channels.push(changer.getPlanarChannelBuffer(ch, numFrames));
      }
      carrier = changer.getCarrierBuffer(numFrames);
      generation = changer.bufferGeneration();
    };
    const fresh = (): void => {
      if (
        generation !== this.changer.bufferGeneration() ||
        (channels[0]?.byteLength ?? 0) === 0 ||
        carrier.byteLength === 0
      ) {
        acquire();
      }
    };
    acquire();
    return {
      get channels(): Float32Array[] {
        fresh();
        return channels;
      },
      get carrier(): Float32Array {
        fresh();
        return carrier;
      },
      process: () => {
        fresh();
        this.changer.processPreparedPlanar(numFrames, undefined);
      },
      processWithCarrier: () => {
        fresh();
        this.changer.processPreparedPlanar(numFrames, true);
      },
    };
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.changerHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

const CONSTRUCTOR_KEYS: readonly string[] = ['sampleRate', 'maxBlockSize', 'numChannels', 'config'];

/** The built-in preset ids, in preset-ordinal order. */
export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  return withEntry('realtimeVoiceChangerPresetNames', () => {
    // Array.from re-roots embind's vector as a plain, structured-cloneable Array.
    return Array.from(getSonareModule().realtimeVoiceChangerPresetNames() as VoicePresetId[]);
  });
}

/**
 * The preset document of a built-in preset, as JSON.
 *
 * @throws TypeError unless `name` is a string; RangeError for an unknown id.
 */
export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return withEntry('realtimeVoiceChangerPresetJson', () => {
    return getSonareModule().realtimeVoiceChangerPresetJson(checkedVoicePresetId(name, 'name'));
  });
}

/**
 * Checks a preset document and returns it normalised.
 *
 * @throws TypeError unless `json` is a string.
 */
export function realtimeVoiceChangerValidatePresetJson(
  json: string,
): RealtimeVoiceChangerValidatePresetJsonResult {
  return withEntry('realtimeVoiceChangerValidatePresetJson', () => {
    assertString(json, 'json');
    return getSonareModule().validateRealtimeVoiceChangerPresetJson(json);
  });
}

/**
 * The preset id of a voice-character preset ordinal.
 *
 * @throws TypeError unless `ordinal` is a number; RangeError for a non-integer or unknown ordinal.
 */
export function voiceCharacterPresetId(ordinal: number): VoicePresetId {
  return withEntry('voiceCharacterPresetId', () => {
    assertBoundedInteger(ordinal, 'ordinal', 0, VOICE_PRESET_IDS.length - 1);
    return getSonareModule().voiceCharacterPresetId(ordinal) as VoicePresetId;
  });
}

/**
 * The flat config of a built-in preset, skipping the JSON round trip.
 *
 * @throws TypeError unless `preset` is a string (an ordinal is refused); RangeError for an
 *   unknown id.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId,
): RealtimeVoiceChangerConfig {
  return withEntry('realtimeVoiceChangerPresetConfig', () => {
    return getSonareModule().realtimeVoiceChangerPresetConfig(voicePresetOrdinal(preset));
  });
}
