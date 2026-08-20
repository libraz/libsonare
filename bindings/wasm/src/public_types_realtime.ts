/**
 * Realtime equalizer snapshot.
 *
 * Mirrors the C++ `EqualizerSpectrumSnapshot`: `preLeft`/`preRight` and
 * `postLeft`/`postRight` are the pre- and post-EQ waveform streams (uniformly
 * decimated time-domain samples, trimmed to their valid count), so they are a
 * scope feed rather than a spectral estimate. `bandGainDb` holds per-band
 * applied gain (24 entries). `profileDb` is the frequency-domain view: the
 * post-EQ signal is Hann-windowed, transformed and its bin powers summed into
 * 16 geometrically spaced bands covering 20 Hz to 20 kHz, in amplitude decibels
 * relative to full scale (a full-scale sine reads about 0 dB in its own band),
 * rising immediately and falling smoothly. `lastAutoGainDb` is the latest
 * auto-gain compensation, and `seq` increments each time a new snapshot is
 * published.
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
  /**
   * Resonance/slope. Ignored for a LowShelf/HighShelf band once `phase:
   * 'NaturalPhase'` forces Vicanek coefficients (the Vicanek matched-Z shelf
   * design has no Q/S parameter) -- the value is still stored and read back
   * verbatim, so a reflected `q` on such a band does not describe the applied
   * response. Set `coeffMode: 'Rbj'` for a Q-controllable shelf.
   */
  q?: number;
  enabled?: boolean;
  coeffMode?: EqCoeffMode;
  slopeDbOct?: number;
  placement?: EqStereoPlacement;
  /**
   * `'NaturalPhase'` forces `coeffMode: 'Vicanek'` for this band, which
   * ignores `q` for LowShelf/HighShelf (see `q`'s doc).
   */
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
  /**
   * Delays the detector's view of the signal by this many ms; a larger value
   * makes the band react LATER, not earlier -- this is a detector delay, not
   * true look-ahead, and adds no latency to the audio path.
   */
  detectorDelayMs?: number;
  /** Former (misleading) spelling of `detectorDelayMs`, still accepted; `detectorDelayMs` wins if both are set. */
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

export type VoicePresetCategory =
  | 'monitor'
  | 'bright'
  | 'soft'
  | 'deep'
  | 'robot'
  | 'dark'
  | 'custom';

export interface RealtimeVoiceChangerPresetMetadata {
  schemaVersion: 1;
  id: string;
  name: string;
  description?: string;
  category: VoicePresetCategory;
}

export type RealtimeVoiceChangerPreset =
  | (RealtimeVoiceChangerPresetMetadata & {
      dsp: Record<string, unknown>;
      macros?: never;
    })
  | (RealtimeVoiceChangerPresetMetadata & {
      macros: Record<string, number>;
      dsp?: never;
    });

export type RealtimeVoiceChangerConfigInput = VoicePresetId | RealtimeVoiceChangerPreset;

/**
 * Flat (POD) realtime voice-changer configuration. Keys are camelCase to match
 * the Node addon getter, so a preset config reads identically across both JS
 * surfaces (the underlying C ABI / Python POD uses the snake_case equivalents).
 */
export interface RealtimeVoiceChangerPodConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retuneSemitones: number;
  retuneMix: number;
  retuneGrainSize: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  eqHighpassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  gateAttackMs: number;
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  compressorAttackMs: number;
  compressorReleaseMs: number;
  compressorMakeupGainDb: number;
  deesserFrequencyHz: number;
  deesserThresholdDb: number;
  deesserRatio: number;
  deesserRangeDb: number;
  reverbMix: number;
  reverbTimeMs: number;
  reverbDamping: number;
  reverbSeed: number;
  limiterCeilingDb: number;
  limiterReleaseMs: number;
  /** Non-zero enables the 4x-oversampled inter-sample-peak limiter (default enabled). */
  limiterEnableIspLimiter: boolean;
  /** True-peak ceiling in dBTP applied by the ISP limiter (default -1.0). */
  limiterIspCeilingDbtp: number;
}
