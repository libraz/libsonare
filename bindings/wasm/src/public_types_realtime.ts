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
  | 'FlatTilt'
  | 'AllPass';

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
  /**
   * Slope in dB/octave. 6 selects a first-order section on a pass or shelf
   * band; a first-order shelf's frequency is its half-gain point and its `q`
   * is unused.
   */
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
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
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
  /** Rate of `source`, in Hz. Omitted: the equalizer's own rate. */
  sampleRate?: number;
  /**
   * Rate of `reference`, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto the source's before its spectrum is taken.
   */
  referenceSampleRate?: number;
  /** Most bands the match may set; at least 1. Default 8. */
  maxBands?: number;
}

export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain'
  | 'hard-tune'
  | 'robot-monotone'
  | 'choir-harmony'
  | 'vocoder-strings'
  | 'megaphone'
  | 'radio-voice';

export type VoicePresetCategory =
  | 'monitor'
  | 'bright'
  | 'soft'
  | 'deep'
  | 'robot'
  | 'dark'
  | 'tune'
  | 'harmony'
  | 'vocoder'
  | 'character'
  | 'custom';

export interface RealtimeVoiceChangerPresetMetadata {
  schemaVersion: 1 | 2;
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

/** Pitch engine of the voice stage. Fixed when the changer is created. */
export type VoicePitchEngine = 'grain' | 'psola';

/** How the target pitch is chosen. */
export type VoicePitchCorrectionMode = 'off' | 'auto' | 'robot';

/** Scale used by auto pitch and harmony. */
export type VoiceScaleKind = 'major' | 'natural_minor' | 'chromatic';

/** Where the vocoder carrier comes from. */
export type VoiceVocoderCarrierSource = 'internal' | 'external';

/** Waveform of the internal vocoder carrier. */
export type VoiceVocoderCarrierWave = 'saw' | 'pulse' | 'ensemble';

/** Colouration applied after the voice stage. */
export type VoiceCharacterType = 'none' | 'megaphone' | 'radio' | 'bbd_chorus' | 'strobo';

/** Space effect selected by the reverb stage. */
export type VoiceReverbVariation = 'reverb' | 'echo' | 'tempo_delay' | 'dub_echo';

/** Note value of the tempo-synced delay. */
export type VoiceNoteDivision = 'quarter' | 'eighth' | 'dotted_eighth' | 'sixteenth';

/** What the voices do once every held MIDI note has been released. */
export type VoiceMidiRelease = 'hold' | 'input';

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
  /** Strength of the formant displacement in [0, 1]. Ignored when `formantMode` is `'absolute'`. */
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  /**
   * How `formantFactor` acts. `'relative'` leaves it to the colour stage
   * after the retune. `'absolute'` warps the formants to `formantFactor` times the input's ahead
   * of the retune, adds one analysis frame of latency and ignores `formantAmount`; it needs
   * `formantFactor / 2^(retuneSemitones / 12)` within [0.55, 1.65], else the call is refused
   * with an error naming the reachable `formantFactor` range. Fixed when the changer is prepared:
   * a config carrying a different mode is refused. Required, like every POD field.
   */
  formantMode: 'relative' | 'absolute';
  eqHighpassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  gateAttackMs: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  compressorAttackMs: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
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
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  limiterReleaseMs: number;
  /** Non-zero enables the 4x-oversampled inter-sample-peak limiter (default enabled). */
  limiterEnableIspLimiter: boolean;
  /** True-peak ceiling in dBTP applied by the ISP limiter (default -1.0). */
  limiterIspCeilingDbtp: number;
  /**
   * Pitch engine. `'psola'` tracks f0; `'grain'` is the f0-less retune and refuses pitch
   * correction, harmony and the vocoder. Fixed when the changer is created.
   */
  voiceEngineEngine: VoicePitchEngine;
  /** Lowest tracked f0 in Hz, [50, 200]; sets the psola latency. Fixed at creation. */
  voiceEngineF0MinHz: number;
  pitchCorrectionMode: VoicePitchCorrectionMode;
  /** Correction strength in [0, 1]. */
  pitchCorrectionAmount: number;
  /** Glide time to the target in ms, [0, 400]. */
  pitchCorrectionSpeedMs: number;
  /** Scale root, 0 = C .. 11 = B; validated but unused for a chromatic scale. */
  pitchCorrectionKeyRoot: number;
  pitchCorrectionScale: VoiceScaleKind;
  /** Octave shift of the robot target, [-1, 1]. */
  pitchCorrectionRobotOctave: number;
  /** Harmony voices beside the lead, [0, 3]. */
  harmonyVoices: number;
  /** Scale degrees of harmony voice 1 from the lead, [-7, 7] (0 = unison, +2 = a third up). */
  harmonyStep1: number;
  harmonyStep2: number;
  harmonyStep3: number;
  harmonyLevel: number;
  harmonyLeadLevel: number;
  /** Detune of the harmony voices in cents, [0, 30]. */
  harmonyDetuneCents: number;
  /** Delay of the harmony voices in ms, [0, 40]. */
  harmonyDelayMs: number;
  vocoderEnabled: boolean;
  /** Band count, [8, 32]. Fixed at creation. */
  vocoderBands: number;
  vocoderAttackMs: number;
  vocoderReleaseMs: number;
  vocoderSibilance: number;
  /**
   * `'external'` reads the carrier passed to the `*WithCarrier` process methods (the worklet's
   * second input); a carrier-less process call is then refused with an `InvalidState` error.
   */
  vocoderCarrierSource: VoiceVocoderCarrierSource;
  vocoderCarrierWave: VoiceVocoderCarrierWave;
  vocoderEnsembleDetuneCents: number;
  vocoderLevel: number;
  characterType: VoiceCharacterType;
  characterAmount: number;
  /** Tone tilt in [-1, 1]. */
  characterTone: number;
  reverbVariationVariation: VoiceReverbVariation;
  /** Echo delay in ms, [1, 3000]. */
  reverbVariationDelayMs: number;
  /** Echo feedback in [0, 0.95]. */
  reverbVariationFeedback: number;
  /** Tempo of the tempo-synced delay, [30, 300]. */
  reverbVariationTempoBpm: number;
  reverbVariationDivision: VoiceNoteDivision;
  midiRelease: VoiceMidiRelease;
}

/** One raw UMP message: 1 to 4 words, most significant word first. */
export type UmpWords = Uint32Array | readonly number[];

/**
 * Why a sidechain binding would be refused; the camelCase form of the C ABI
 * `SonareSidechainRefusal`. When several reasons hold, the first check the
 * matching setter runs is reported.
 */
export type SidechainRefusal =
  | 'invalidTarget'
  | 'insertOutOfRange'
  | 'undeclaredSource'
  | 'invalidSourceKind'
  | 'selfKey'
  | 'cycle'
  | 'tableFull'
  | 'planRefused';

/** Result of a `canSet*Sidechain` query; `reason` is `null` exactly when `ok`. */
export interface SidechainCheck {
  ok: boolean;
  reason: SidechainRefusal | null;
}
