import { addon } from './native.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  VoicePresetId,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

/**
 * True when `config` is a flat POD (the shape `realtimeVoiceChangerPresetConfig`
 * returns) rather than a preset name or a nested preset. `retuneSemitones` only
 * exists on the flat POD; the nested form spells it `retune.semitones`.
 */
function isFlatVoiceChangerPod(config: unknown): config is RealtimeVoiceChangerConfig {
  return typeof config === 'object' && config !== null && 'retuneSemitones' in config;
}

/**
 * Rewrite the flat 36-field POD into the nested shape the native config parser
 * reads. Passing the flat POD straight to `setConfig` would leave every nested
 * field at its default (only the three root fields are read flat), silently
 * discarding retune/EQ/reverb/etc.
 */
function flatVoiceChangerPodToNested(pod: RealtimeVoiceChangerConfig): Record<string, unknown> {
  return {
    inputGainDb: pod.inputGainDb,
    outputGainDb: pod.outputGainDb,
    wetMix: pod.wetMix,
    retune: { semitones: pod.retuneSemitones, mix: pod.retuneMix, grainSize: pod.retuneGrainSize },
    formant: {
      factor: pod.formantFactor,
      amount: pod.formantAmount,
      body: pod.formantBody,
      brightness: pod.formantBrightness,
      nasal: pod.formantNasal,
    },
    eq: {
      highpassHz: pod.eqHighpassHz,
      bodyDb: pod.eqBodyDb,
      presenceDb: pod.eqPresenceDb,
      airDb: pod.eqAirDb,
    },
    gate: {
      thresholdDb: pod.gateThresholdDb,
      attackMs: pod.gateAttackMs,
      releaseMs: pod.gateReleaseMs,
      rangeDb: pod.gateRangeDb,
    },
    compressor: {
      thresholdDb: pod.compressorThresholdDb,
      ratio: pod.compressorRatio,
      attackMs: pod.compressorAttackMs,
      releaseMs: pod.compressorReleaseMs,
      makeupGainDb: pod.compressorMakeupGainDb,
    },
    deesser: {
      frequencyHz: pod.deesserFrequencyHz,
      thresholdDb: pod.deesserThresholdDb,
      ratio: pod.deesserRatio,
      rangeDb: pod.deesserRangeDb,
    },
    reverb: {
      mix: pod.reverbMix,
      timeMs: pod.reverbTimeMs,
      damping: pod.reverbDamping,
      seed: pod.reverbSeed,
    },
    limiter: {
      ceilingDb: pod.limiterCeilingDb,
      releaseMs: pod.limiterReleaseMs,
      enableIspLimiter: pod.limiterEnableIspLimiter,
      ispCeilingDbtp: pod.limiterIspCeilingDbtp,
    },
  };
}

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

  setConfig(config: RealtimeVoiceChangerConfigInput | RealtimeVoiceChangerConfig): void {
    // A flat POD (from realtimeVoiceChangerPresetConfig) is rewritten to the
    // nested shape the native parser reads; a preset name or nested preset is
    // passed through unchanged.
    this.native.setConfig(
      isFlatVoiceChangerPod(config) ? flatVoiceChangerPodToNested(config) : config,
    );
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

/** Inputs for the one-shot {@link voiceChange} facade. */
export interface VoiceChangeRequest extends VoiceChangeOptions {
  samples: Float32Array;
  sampleRate?: number;
}

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
  return addon.voiceChange(
    request.samples,
    request.sampleRate ?? 22050,
    request.pitchSemitones ?? 0.0,
    request.formantFactor ?? 1.0,
  );
}

export interface VoiceChangeRealtimeOptions extends ValidateOptions {
  /** Channel count: 1 = mono, 2 = interleaved stereo (L0,R0,L1,R1,...). */
  channels?: 1 | 2;
}

/** Inputs for the one-shot {@link voiceChangeRealtime} facade. */
export interface VoiceChangeRealtimeRequest extends VoiceChangeRealtimeOptions {
  samples: Float32Array;
  sampleRate?: number;
  preset?: RealtimeVoiceChangerConfigInput;
}

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
  const validate = request.validate !== false;
  assertSamples('voiceChangeRealtime', request.samples, validate);
  const channels = request.channels ?? 1;
  if (channels !== 1 && channels !== 2) {
    throw new Error('voiceChangeRealtime: channels must be 1 or 2.');
  }
  if (channels === 2 && request.samples.length % 2 !== 0) {
    throw new Error('voiceChangeRealtime: stereo input length must be a multiple of 2.');
  }
  const presetConfig = request.preset ?? 'neutral-monitor';
  return addon.voiceChangeRealtime(
    request.samples,
    request.sampleRate ?? 48000,
    typeof presetConfig === 'string' ? presetConfig : JSON.stringify(presetConfig),
    channels,
  );
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
