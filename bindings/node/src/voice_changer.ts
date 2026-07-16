import { addon } from './native.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  VoicePresetId,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

export class RealtimeVoiceChanger {
  private native: InstanceType<typeof addon.RealtimeVoiceChanger>;
  private disposed = false;

  constructor(options: RealtimeVoiceChangerOptions) {
    this.native = new addon.RealtimeVoiceChanger(options.preset ?? 'neutral-monitor');
    this.native.prepare(options.sampleRate, options.maxBlockSize ?? 128, options.channels ?? 1);
  }

  reset(): void {
    this.native.reset();
  }

  setConfig(config: RealtimeVoiceChangerConfigInput): void {
    this.native.setConfig(config);
  }

  configJson(): string {
    return this.native.configJson();
  }

  latencySamples(): number {
    return this.native.latencySamples();
  }

  processMono(input: Float32Array): Float32Array {
    return this.native.processMono(input);
  }

  processMonoInto(input: Float32Array, output: Float32Array): void {
    this.native.processMonoInto(input, output);
  }

  processInterleaved(input: Float32Array, channels: 1 | 2): Float32Array {
    return this.native.processInterleaved(input, channels);
  }

  processInterleavedInto(input: Float32Array, channels: 1 | 2, output: Float32Array): void {
    this.native.processInterleavedInto(input, channels, output);
  }

  /**
   * Process a block of planar (non-interleaved) stereo audio in place. The
   * `left` and `right` buffers must have equal length and are mutated with the
   * processed output. Requires the changer to have been prepared with at least
   * 2 channels.
   */
  processPlanarStereo(left: Float32Array, right: Float32Array): void {
    this.native.processPlanarStereo(left, right);
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /** Releases native resources; lets `using` (Node 22+) free them automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** Options for {@link voiceChange}. All fields are optional. */
export interface VoiceChangeOptions extends ValidateOptions {
  /** Pitch shift in semitones (negative = down). Default 0. */
  pitchSemitones?: number;
  /** Formant scale factor (>1 brightens, <1 darkens). Default 1. */
  formantFactor?: number;
}

export function voiceChange(
  samples: Float32Array,
  sampleRate = 22050,
  options: VoiceChangeOptions = {},
): Float32Array {
  assertSamples('voiceChange', samples, options.validate !== false);
  return addon.voiceChange(
    samples,
    sampleRate,
    options.pitchSemitones ?? 0.0,
    options.formantFactor ?? 1.0,
  );
}

export interface VoiceChangeRealtimeOptions extends ValidateOptions {
  /** Channel count: 1 = mono, 2 = interleaved stereo (L0,R0,L1,R1,...). */
  channels?: 1 | 2;
}

function latencyCompensatedVoiceChange(
  changer: RealtimeVoiceChanger,
  samples: Float32Array,
  channels: 1 | 2,
  blockFrames: number,
): Float32Array {
  const latencyFrames = Math.max(0, changer.latencySamples());
  if (channels === 1) {
    const total = samples.length + latencyFrames;
    const input = new Float32Array(total);
    input.set(samples);
    const processed = new Float32Array(total);
    for (let pos = 0; pos < total; pos += blockFrames) {
      const inputBlock = input.subarray(pos, Math.min(total, pos + blockFrames));
      const outputBlock = processed.subarray(pos, pos + inputBlock.length);
      changer.processMonoInto(inputBlock, outputBlock);
    }
    return processed.slice(latencyFrames, latencyFrames + samples.length);
  }

  const frames = samples.length / 2;
  const totalFrames = frames + latencyFrames;
  const input = new Float32Array(totalFrames * 2);
  input.set(samples);
  const processed = new Float32Array(totalFrames * 2);
  const frameStride = blockFrames * 2;
  for (let pos = 0; pos < input.length; pos += frameStride) {
    const inputBlock = input.subarray(pos, Math.min(input.length, pos + frameStride));
    const outputBlock = processed.subarray(pos, pos + inputBlock.length);
    changer.processInterleavedInto(inputBlock, 2, outputBlock);
  }
  const offset = latencyFrames * 2;
  return processed.slice(offset, offset + samples.length);
}

export function voiceChangeRealtime(
  samples: Float32Array,
  sampleRate = 48000,
  preset: RealtimeVoiceChangerConfigInput = 'neutral-monitor',
  options: VoiceChangeRealtimeOptions = {},
): Float32Array {
  const validate = options.validate !== false;
  assertSamples('voiceChangeRealtime', samples, validate);
  const channels = options.channels ?? 1;
  if (channels !== 1 && channels !== 2) {
    throw new Error('voiceChangeRealtime: channels must be 1 or 2.');
  }
  if (channels === 2 && samples.length % 2 !== 0) {
    throw new Error('voiceChangeRealtime: stereo input length must be a multiple of 2.');
  }
  const block = 512;
  const changer = new RealtimeVoiceChanger({
    sampleRate,
    maxBlockSize: block,
    channels,
    preset,
  });
  try {
    return latencyCompensatedVoiceChange(changer, samples, channels, block);
  } finally {
    changer.destroy();
  }
}

export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  return addon.realtimeVoiceChangerPresetNames() as VoicePresetId[];
}

export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return addon.realtimeVoiceChangerPresetJson(name);
}

export function validateRealtimeVoiceChangerPresetJson(json: string): {
  ok: boolean;
  normalizedJson?: string;
  error?: string;
} {
  return addon.validateRealtimeVoiceChangerPresetJson(json);
}

// Ordinals mirror the SonareVoiceCharacterPreset enum (sonare_c_types.h).
const VOICE_PRESET_ORDINALS: Record<VoicePresetId, number> = {
  'neutral-monitor': 0,
  'bright-idol': 1,
  'soft-whisper': 2,
  'deep-narrator': 3,
  'robot-mascot': 4,
  'dark-villain': 5,
};

function resolveVoicePresetOrdinal(preset: VoicePresetId | number): number {
  if (typeof preset === 'number') {
    return preset;
  }
  const ordinal = VOICE_PRESET_ORDINALS[preset];
  if (ordinal === undefined) {
    // Mirror the WASM/Python bindings: an unknown preset name is an error, not
    // a silent `undefined` ordinal that would corrupt the native call.
    throw new Error(`Unknown voice character preset: ${preset}`);
  }
  return ordinal;
}

/**
 * Returns the canonical preset id for a voice-character preset ordinal (or id),
 * or `null` when the ordinal is out of range.
 */
export function voiceCharacterPresetId(preset: VoicePresetId | number): VoicePresetId | null {
  return addon.voiceCharacterPresetId(resolveVoicePresetOrdinal(preset)) as VoicePresetId | null;
}

/**
 * Returns the flat (normalized) realtime-voice-changer config for a built-in
 * preset, skipping the JSON round-trip.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId | number,
): RealtimeVoiceChangerConfig {
  return addon.realtimeVoiceChangerPresetConfig(
    resolveVoicePresetOrdinal(preset),
  ) as RealtimeVoiceChangerConfig;
}
