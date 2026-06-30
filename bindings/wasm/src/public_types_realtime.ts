/**
 * Realtime equalizer spectrum snapshot.
 *
 * Mirrors the C++ `EqualizerSpectrumSnapshot`: `preLeft`/`preRight` and
 * `postLeft`/`postRight` are the pre- and post-EQ spectrum streams (trimmed to
 * their valid count). `bandGainDb` holds per-band applied gain (24 entries),
 * `profileDb` the smoothed magnitude profile (16 entries), `lastAutoGainDb`
 * the latest auto-gain compensation, and `seq` increments each time a new
 * snapshot is published.
 */
export interface EqSpectrumSnapshot {
  preLeft: Float32Array;
  preRight: Float32Array;
  postLeft: Float32Array;
  postRight: Float32Array;
  bandGainDb: Float32Array;
  profileDb: Float32Array;
  lastAutoGainDb: number;
  seq: number;
}

/**
 * Equalizer band type (string union mirroring `sonare::mastering::eq::EqBandType`).
 */
export type EqBandType =
  | 'Peak'
  | 'LowShelf'
  | 'HighShelf'
  | 'LowPass'
  | 'HighPass'
  | 'BandPass'
  | 'Notch'
  | 'TiltShelf'
  | 'FlatTilt';

/** Biquad coefficient design mode. */
export type EqCoeffMode = 'Rbj' | 'Vicanek';

/** Stereo placement for an EQ band. */
export type EqStereoPlacement = 'Stereo' | 'Left' | 'Right' | 'Mid' | 'Side';

/** Per-band phase behaviour. */
export type EqBandPhase = 'Inherit' | 'ZeroLatency' | 'NaturalPhase' | 'LinearPhase';

/**
 * Equalizer band configuration accepted by {@link StreamingEqualizer.setBand}.
 *
 * All fields are optional; omitted values fall back to the C++ band defaults
 * (Peak, 1000 Hz, 0 dB gain, Butterworth Q, disabled).
 */
export interface EqBand {
  type?: EqBandType;
  frequencyHz?: number;
  gainDb?: number;
  q?: number;
  enabled?: boolean;
  coeffMode?: EqCoeffMode;
  slopeDbOct?: number;
  placement?: EqStereoPlacement;
  phase?: EqBandPhase;
  soloed?: boolean;
  bypassed?: boolean;
  proportionalQ?: boolean;
  proportionalQStrength?: number;
  dynamic?: boolean;
  thresholdDb?: number;
  autoThreshold?: boolean;
  ratio?: number;
  rangeDb?: number;
  attackMs?: number;
  releaseMs?: number;
  lookaheadMs?: number;
  externalSidechain?: boolean;
  sidechainFreqHz?: number;
  sidechainQ?: number;
}

/** Construction options for {@link StreamingEqualizer}. */
export interface StreamingEqualizerConfig {
  sampleRate?: number;
  maxBlockSize?: number;
}

/** Configuration for {@link StreamingRetune}. */
export interface StreamingRetuneConfig {
  /** Pitch shift in semitones, clamped by the native processor to +/-24. */
  semitones?: number;
  /** Wet/dry mix, clamped by the native processor to 0..1. */
  mix?: number;
  /** Grain size in samples. Use 0/omit to derive it from the sample rate. */
  grainSize?: number;
}

/** Options for {@link StreamingEqualizer.match}. */
export interface EqMatchOptions {
  sampleRate?: number;
  maxBands?: number;
}

export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain';

export interface RealtimeVoiceChangerPreset {
  schemaVersion: 1;
  id?: string;
  name?: string;
  description?: string;
  macros?: Record<string, number>;
  dsp?: Record<string, unknown>;
}

export type RealtimeVoiceChangerConfigInput = VoicePresetId | RealtimeVoiceChangerPreset;

/**
 * Flat (POD) realtime voice-changer configuration. Field names mirror the
 * C ABI `SonareRealtimeVoiceChangerConfig` / Python POD exactly (snake_case),
 * so a config can be round-tripped across bindings without renaming.
 */
export interface RealtimeVoiceChangerPodConfig {
  input_gain_db: number;
  output_gain_db: number;
  wet_mix: number;
  retune_semitones: number;
  retune_mix: number;
  retune_grain_size: number;
  formant_factor: number;
  formant_amount: number;
  formant_body: number;
  formant_brightness: number;
  formant_nasal: number;
  eq_highpass_hz: number;
  eq_body_db: number;
  eq_presence_db: number;
  eq_air_db: number;
  gate_threshold_db: number;
  gate_attack_ms: number;
  gate_release_ms: number;
  gate_range_db: number;
  compressor_threshold_db: number;
  compressor_ratio: number;
  compressor_attack_ms: number;
  compressor_release_ms: number;
  compressor_makeup_gain_db: number;
  deesser_frequency_hz: number;
  deesser_threshold_db: number;
  deesser_ratio: number;
  deesser_range_db: number;
  reverb_mix: number;
  reverb_time_ms: number;
  reverb_damping: number;
  reverb_seed: number;
  limiter_ceiling_db: number;
  limiter_release_ms: number;
  /** Non-zero enables the 4x-oversampled inter-sample-peak limiter (default enabled). */
  limiter_enable_isp_limiter: boolean;
  /** True-peak ceiling in dBTP applied by the ISP limiter (default -1.0). */
  limiter_isp_ceiling_dbtp: number;
}
