import { getSonareModule } from './module_state';
import type { RealtimeVoiceChangerConfigInput } from './public_types';
import { RealtimeVoiceChanger } from './streaming_mixing';
import type { ValidateOptions } from './validation';
import { assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

/** Options for {@link voiceChange}. All fields are optional. */
export interface VoiceChangeOptions extends ValidateOptions {
  /** Pitch shift in semitones (negative = down). Default 0. */
  pitchSemitones?: number;
  /** Formant scale factor (>1 brightens, <1 darkens). Default 1. */
  formantFactor?: number;
}

/** Canonical request form for one-shot voice changing. */
export interface VoiceChangeRequest extends VoiceChangeOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/**
 * Apply a voice change by shifting pitch and formants independently.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - Pitch/formant settings ({@link VoiceChangeOptions})
 * @returns Voice-changed audio
 */
export function voiceChange(request: VoiceChangeRequest): Float32Array;
export function voiceChange(
  samples: Float32Array,
  sampleRate?: number,
  options?: VoiceChangeOptions,
): Float32Array;
export function voiceChange(
  samples: Float32Array | VoiceChangeRequest,
  sampleRate = 22050,
  options: VoiceChangeOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('voiceChange', request.samples, request.validate !== false);
  return requireModule().voiceChange(
    request.samples,
    request.sampleRate ?? 22050,
    request.pitchSemitones ?? 0.0,
    request.formantFactor ?? 1.0,
  );
}

/** Options for the offline {@link voiceChangeRealtime} convenience wrapper. */
export interface VoiceChangeRealtimeOptions extends ValidateOptions {
  /** Channel count (1 = mono, 2 = interleaved stereo). */
  channels?: 1 | 2;
  /** Block size for the internal render loop (default 512). */
  blockSize?: number;
}

/** Canonical request form for offline realtime voice changing. */
export interface VoiceChangeRealtimeRequest extends VoiceChangeRealtimeOptions {
  samples: Float32Array;
  sampleRate?: number;
  preset?: RealtimeVoiceChangerConfigInput;
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
    for (let offset = 0; offset < total; offset += blockFrames) {
      const block = input.subarray(offset, Math.min(offset + blockFrames, total));
      processed.set(changer.processMono(block), offset);
    }
    return processed.slice(latencyFrames, latencyFrames + samples.length);
  }

  const frames = samples.length / 2;
  const totalFrames = frames + latencyFrames;
  const input = new Float32Array(totalFrames * 2);
  input.set(samples);
  const processed = new Float32Array(totalFrames * 2);
  const frameStride = blockFrames * 2;
  for (let offset = 0; offset < input.length; offset += frameStride) {
    const block = input.subarray(offset, Math.min(offset + frameStride, input.length));
    processed.set(changer.processInterleaved(block, 2), offset);
  }
  const start = latencyFrames * 2;
  return processed.slice(start, start + samples.length);
}

/**
 * Applies the realtime voice-changer chain to a whole buffer in one call.
 *
 * Constructs and prepares a {@link RealtimeVoiceChanger}, runs the block loop
 * for the caller, then disposes it — matching the Python `voice_change_realtime`
 * and Node `voiceChangeRealtime` convenience wrappers. For mono, `samples` is a
 * plain mono buffer; for stereo, `samples` is interleaved (L0,R0,L1,R1,...).
 *
 * @param samples - Audio samples (mono, or interleaved stereo when channels=2)
 * @param sampleRate - Sample rate in Hz (default 48000, matching Python/Node)
 * @param preset - Voice-changer preset id or full config object
 * @param options - Channel count and block size ({@link VoiceChangeRealtimeOptions})
 * @returns The processed buffer (same layout/length as the input).
 */
export function voiceChangeRealtime(request: VoiceChangeRealtimeRequest): Float32Array;
export function voiceChangeRealtime(
  samples: Float32Array,
  sampleRate?: number,
  preset?: RealtimeVoiceChangerConfigInput,
  options?: VoiceChangeRealtimeOptions,
): Float32Array;
export function voiceChangeRealtime(
  samples: Float32Array | VoiceChangeRealtimeRequest,
  sampleRate = 48000,
  preset: RealtimeVoiceChangerConfigInput = 'neutral-monitor',
  options: VoiceChangeRealtimeOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, preset, ...options } : samples;
  assertSamples('voiceChangeRealtime', request.samples, request.validate !== false);
  const channels = request.channels ?? 1;
  if (channels !== 1 && channels !== 2) {
    throw new Error('voiceChangeRealtime: channels must be 1 or 2.');
  }
  if (channels === 2 && request.samples.length % 2 !== 0) {
    throw new Error('voiceChangeRealtime: stereo input length must be a multiple of 2.');
  }
  const blockSize = Math.max(1, Math.floor(request.blockSize ?? 512));
  const changer = new RealtimeVoiceChanger(request.preset ?? 'neutral-monitor');
  try {
    changer.prepare(request.sampleRate ?? 48000, blockSize, channels);
    return latencyCompensatedVoiceChange(changer, request.samples, channels, blockSize);
  } finally {
    changer.delete();
  }
}
