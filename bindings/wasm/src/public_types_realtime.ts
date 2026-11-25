import type { FormantMode } from './effects_voice_change.js';

/** One audio device returned by {@link LiveSession.audioDevices}. */
export interface LiveAudioDevice {
  /** Stable device UID used to select this device. */
  readonly uid: string;
  /** Display name. */
  readonly name: string;
  /** Available input channels. */
  readonly inputChannels: number;
  /** Available output channels. */
  readonly outputChannels: number;
  /** Whether this is the current default input device. */
  readonly isDefaultInput: boolean;
  /** Whether this is the current default output device. */
  readonly isDefaultOutput: boolean;
  /** Nominal device sample rate in Hz. */
  readonly nominalSampleRate: number;
}

/** One MIDI source returned by {@link LiveSession.midiSources}. */
export interface LiveMidiDevice {
  /** Stable device UID used to select this source. */
  readonly uid: string;
  /** Display name. */
  readonly name: string;
}

/** Settings for constructing a {@link LiveSession}. */
export interface LiveSessionConfig {
  /** Output device: `'default'`, a stable UID selector, or omitted to leave it disconnected. */
  readonly audioOutput?:
    | 'default'
    | {
        /** Stable device UID from enumeration. */
        readonly uid: string;
      };
  /** Input device: `'default'`, a stable UID selector, or omitted to leave it disconnected. */
  readonly audioInput?:
    | 'default'
    | {
        /** Stable device UID from enumeration. */
        readonly uid: string;
      };
  /** MIDI source selected by stable UID, or omitted to leave MIDI disconnected. */
  readonly midiInput?: {
    /** Stable device UID from enumeration. */
    readonly uid: string;
  };
  /** Requested device sample rate in Hz. Omitted to use the device's nominal rate. */
  readonly sampleRate?: number;
  /** Requested audio block size in frames. Omitted to use the library default. */
  readonly blockSize?: number;
  /** Engine destination for received MIDI. Omitted to use destination `0`. */
  readonly destinationId?: number;
}

/** Current state and counters of a {@link LiveSession}. */
export interface LiveSessionStatus {
  /** Whether a device is currently driving the engine. */
  readonly running: boolean;
  /** Whether a selected audio device disappeared. */
  readonly deviceLost: boolean;
  /** Whether a device's nominal sample rate changed after opening. */
  readonly rateChanged: boolean;
  /** Active engine sample rate in Hz. */
  readonly sampleRate: number;
  /** Active output block size in frames. */
  readonly outputBlockSize: number;
  /** Active input block size in frames. */
  readonly inputBlockSize: number;
  /** Output latency in milliseconds. */
  readonly outputLatencyMs: number;
  /** Input latency in milliseconds. */
  readonly inputLatencyMs: number;
  /** Number of audio device dropouts. */
  readonly xrunCount: number;
  /** Dropped input frames after priming, including high-water discards. */
  readonly inputOverrunFrames: number;
  /** Input frames repeated after priming because the ring ran low. */
  readonly inputUnderrunFrames: number;
  /** Captured MIDI events dropped because the capture list was full. */
  readonly droppedMidiEvents: number;
}

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
  /** Pre-EQ left waveform scope samples. */
  preLeft: Float32Array;
  /** Pre-EQ right waveform scope samples. */
  preRight: Float32Array;
  /** Post-EQ left waveform scope samples. */
  postLeft: Float32Array;
  /** Post-EQ right waveform scope samples. */
  postRight: Float32Array;
  /** Applied gain per band in dB (24 entries). */
  bandGainDb: Float32Array;
  /** Post-EQ level in dB across 16 bands from 20 Hz to 20 kHz. `-Infinity` for a band of zero power, NaN for a band no bin reaches and before the first transform. */
  profileDb: Float32Array;
  /** Latest auto-gain compensation in dB. */
  lastAutoGainDb: number;
  /** Sequence counter; increments each time a new snapshot is published. */
  seq: number;
}

/**
 * Equalizer band type (string union mirroring `sonare::mastering::eq::EqBandType`).
 */
export type EqBandType =
  | 'peak'
  | 'lowShelf'
  | 'highShelf'
  | 'lowPass'
  | 'highPass'
  | 'bandPass'
  | 'notch'
  | 'tiltShelf'
  | 'flatTilt'
  | 'allPass';

/** Biquad coefficient design mode. */
export type EqCoeffMode = 'rbj' | 'vicanek';

/** Stereo placement for an EQ band. */
export type EqStereoPlacement = 'stereo' | 'left' | 'right' | 'mid' | 'side';

/** Per-band phase behaviour. */
export type EqBandPhase = 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';

/**
 * Equalizer band configuration accepted by {@link StreamingEqualizer.setBand}.
 *
 * All fields are optional; omitted values fall back to the C++ band defaults
 * (`'peak'`, 1000 Hz, 0 dB gain, Butterworth Q, disabled).
 */
export interface EqBandInput {
  /** Band shape. Default `'peak'`. */
  type?: EqBandType;
  /** Centre or corner frequency in Hz. Default `1000`. */
  frequencyHz?: number;
  /** Band gain in dB. Default `0`. */
  gainDb?: number;
  /**
   * Resonance/slope. Ignored for a `'lowShelf'`/`'highShelf'` band once `phase:
   * 'naturalPhase'` forces Vicanek coefficients (the Vicanek matched-Z shelf
   * design has no Q/S parameter) -- the value is still stored and read back
   * verbatim, so a reflected `q` on such a band does not describe the applied
   * response. Set `coeffMode: 'rbj'` for a Q-controllable shelf.
   */
  q?: number;
  /** Whether the band processes audio. Default `false`. */
  enabled?: boolean;
  /** Biquad coefficient design. Default `'rbj'`. */
  coeffMode?: EqCoeffMode;
  /**
   * Slope in dB/octave. 6 selects a first-order section on a pass or shelf
   * band; a first-order shelf's frequency is its half-gain point and its `q`
   * is unused.
   */
  slopeDbOct?: number;
  /** Signal path the band sits on. Default `'stereo'`. */
  placement?: EqStereoPlacement;
  /**
   * `'naturalPhase'` forces `coeffMode: 'vicanek'` for this band, which
   * ignores `q` for `'lowShelf'`/`'highShelf'` (see `q`'s doc).
   */
  phase?: EqBandPhase;
  /** Listen to this band alone. Default `false`. */
  soloed?: boolean;
  /** Skip the band without removing it. Default `false`. */
  bypassed?: boolean;
  /** Narrow the Q as the gain magnitude grows. Default `false`. */
  proportionalQ?: boolean;
  /** How strongly the gain magnitude narrows Q when `proportionalQ` is on. Default `0.03`. */
  proportionalQStrength?: number;
  /** Make the band gain follow the detector level. Default `false`. */
  dynamic?: boolean;
  /** Detector threshold in dB above which a dynamic band acts. Default `-24`. */
  thresholdDb?: number;
  /** Derive the dynamic threshold from the signal. Default `false`. */
  autoThreshold?: boolean;
  /** Dynamic compression ratio. Default `2`. */
  ratio?: number;
  /** Maximum dynamic gain change in dB; negative compresses. Default `-6`. */
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
  /** Drive the detector from the external sidechain input. Default `false`. */
  externalSidechain?: boolean;
  /** Detector centre frequency in Hz; exactly `-1` follows the band frequency. Default `-1`. */
  sidechainFreqHz?: number;
  /** Detector band-pass Q. Default `1`. */
  sidechainQ?: number;
}

/** Construction options for {@link StreamingEqualizer}. */
export interface StreamingEqualizerConfig {
  /** Sample rate in Hz (integer). Required; `TypeError` when absent. */
  sampleRate: number;
  /** Largest block passed to a process call, in samples. Omit it for the library default; an explicit 0 is refused. */
  maxBlockSize?: number;
}

/** Configuration for {@link StreamingRetune}. */
export interface StreamingRetuneConfig {
  /** Pitch shift in semitones, finite and in [-24, 24]; outside it is refused. Default 0. */
  pitchSemitones?: number;
  /** Dry/wet blend in [0, 1]; outside it is refused. Default 1 (fully wet). */
  mix?: number;
  /**
   * Grain size in samples, a multiple of 4 in [4, 8192]. Omitted: the automatic grain, derived
   * from the sample rate at each `prepare` and readable from `StreamingRetune.grainSizeSamples`
   * after it; 0 is refused. Takes effect at the next `prepare`.
   */
  grainSizeSamples?: number;
}

/** The controls {@link StreamingRetune.config} reports; `grainSizeSamples` is absent when automatic. */
export type StreamingRetuneAppliedConfig = Required<
  Omit<StreamingRetuneConfig, 'grainSizeSamples'>
> &
  Pick<StreamingRetuneConfig, 'grainSizeSamples'>;

/** Options for {@link StreamingEqualizer.setAutoGain}. */
export interface AutoGainOptions {
  /** `true` enables output auto-gain compensation, `false` disables it. */
  enabled: boolean;
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

/** Name of a built-in realtime voice-changer preset. */
export type VoicePresetId =
  | 'neutralMonitor'
  | 'brightIdol'
  | 'softWhisper'
  | 'deepNarrator'
  | 'robotMascot'
  | 'darkVillain'
  | 'hardTune'
  | 'robotMonotone'
  | 'choirHarmony'
  | 'vocoderStrings'
  | 'megaphone'
  | 'radioVoice';

/** Catalogue category of a voice-changer preset. */
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

/** Metadata shared by every preset document. */
export interface RealtimeVoiceChangerPresetMetadata {
  /** Preset document schema version; always `4`. */
  schemaVersion: 4;
  /** Preset id. */
  id: string;
  /** Display name. */
  name: string;
  /** Optional human-readable description. */
  description?: string;
  /** Category the preset is listed under. */
  category: VoicePresetCategory;
}

/** A preset document: shared metadata plus either raw `dsp` parameters or `macros`. */
export type RealtimeVoiceChangerPreset =
  | (RealtimeVoiceChangerPresetMetadata & {
      /**
       * Every processing stage's settings by section (`retune`, `formant`, `eq`, `gate`, ...);
       * every section is required.
       */
      dsp: Record<string, unknown>;
      macros?: never;
    })
  | (RealtimeVoiceChangerPresetMetadata & {
      /**
       * Macro controls: `pitch` in semitones, `formant` as a frequency factor, and `brightness`,
       * `space`, `intensity`, `noiseControl`, `sibilance` in `[0, 1]`.
       */
      macros: Record<string, number>;
      dsp?: never;
    });

/** A built-in preset id, a preset document, or the flat {@link RealtimeVoiceChangerConfig}. */
export type RealtimeVoiceChangerConfigInput =
  | VoicePresetId
  | RealtimeVoiceChangerPreset
  | RealtimeVoiceChangerConfig;

/** Pitch engine of the voice stage. Fixed when the changer is created. */
export type VoiceEngine = 'grain' | 'psola';

/** How the target pitch is chosen. */
export type PitchCorrectionMode = 'off' | 'auto' | 'robot';

/** Scale used by auto pitch and harmony. */
export type ScaleKind = 'major' | 'naturalMinor' | 'chromatic';

/** Where the vocoder carrier comes from. */
export type VocoderCarrierSource = 'internal' | 'external';

/** Waveform of the internal vocoder carrier. */
export type VocoderCarrierWave = 'saw' | 'pulse' | 'ensemble';

/** Colouration applied after the voice stage. */
export type CharacterType = 'none' | 'megaphone' | 'radio' | 'bbdChorus' | 'strobo';

/** Space effect selected by the reverb stage. */
export type ReverbVariation = 'reverb' | 'echo' | 'tempoDelay' | 'dubEcho';

/** Note value of the tempo-synced delay. */
export type NoteDivision = 'quarter' | 'eighth' | 'dottedEighth' | 'sixteenth';

/** What the voices do once every held MIDI note has been released. */
export type MidiRelease = 'hold' | 'input';

/**
 * Flat realtime voice-changer configuration. Keys are camelCase to match
 * the Node addon getter, so a preset config reads identically across both JS
 * surfaces (the underlying C ABI / Python POD uses the snake_case equivalents).
 */
export interface RealtimeVoiceChangerConfig {
  /** Input gain in dB. */
  inputGainDb: number;
  /** Output gain in dB. */
  outputGainDb: number;
  /** Overall wet/dry mix. */
  wetMix: number;
  /**
   * Retune pitch shift in semitones, [-24, 24]. The changer always applies its input high-pass (20 Hz at the lowest `eqHighPassHz`); for a pitch shift with no other processing use `StreamingRetune`.
   */
  retunePitchSemitones: number;
  /** Retune wet amount in [0, 1]. */
  retuneMix: number;
  /** Retune grain length in samples, a multiple of 4 in [4, 8192]; omitted for the automatic grain. */
  retuneGrainSizeSamples?: number;
  /** Formant shift factor; 1 leaves the formants unchanged. */
  formantFactor: number;
  /** Strength of the formant displacement in [0, 1]. Ignored when `formantMode` is `'absolute'`. */
  formantAmount: number;
  /** Body colour of the formant stage. */
  formantBody: number;
  /** Brightness colour of the formant stage. */
  formantBrightness: number;
  /** Nasal colour of the formant stage. */
  formantNasal: number;
  /**
   * How `formantFactor` acts. `'relative'` leaves it to the colour stage
   * after the retune. `'absolute'` warps the formants to `formantFactor` times the input's ahead
   * of the retune, adds one analysis frame of latency and ignores `formantAmount`; it needs
   * `formantFactor` and the warp `formantFactor / 2^(retunePitchSemitones * retuneMix / 12)` both
   * within [0.55, 1.65], else the call throws a `RangeError` naming the reachable `formantFactor`
   * range, so a dry retune (`retuneMix` 0) refuses no factor in range. Fixed when the changer is prepared:
   * a config carrying a different mode is refused. Required, like every POD field.
   */
  formantMode: FormantMode;
  /** High-pass corner frequency in Hz. */
  eqHighPassHz: number;
  /** Body-band EQ gain in dB. */
  eqBodyDb: number;
  /** Presence-band EQ gain in dB. */
  eqPresenceDb: number;
  /** Air-band EQ gain in dB. */
  eqAirDb: number;
  /** Gate threshold in dB. */
  gateThresholdDb: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  gateAttackMs: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  gateReleaseMs: number;
  /** Gate attenuation range in dB. */
  gateRangeDb: number;
  /** Compressor threshold in dB. */
  compressorThresholdDb: number;
  /** Compressor ratio. */
  compressorRatio: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  compressorAttackMs: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  compressorReleaseMs: number;
  /** Compressor make-up gain in dB. */
  compressorMakeupGainDb: number;
  /** De-esser centre frequency in Hz. */
  deesserFrequencyHz: number;
  /** De-esser threshold in dB. */
  deesserThresholdDb: number;
  /** De-esser ratio. */
  deesserRatio: number;
  /** Maximum de-esser gain reduction in dB. */
  deesserRangeDb: number;
  /** Reverb wet/dry mix. */
  reverbMix: number;
  /** Reverb time in ms. */
  reverbTimeMs: number;
  /** Reverb high-frequency damping. */
  reverbDamping: number;
  /** Seed of the reverb's random generator. */
  reverbSeed: number;
  /** Limiter ceiling in dB. */
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
   * `'neutralMonitor'` uses `'psola'`; `'grain'` is the engine `StreamingRetune` uses, so the two match only with `voiceEngine: 'grain'`.
   */
  voiceEngine: VoiceEngine;
  /** Lowest tracked f0 in Hz, [50, 200]; sets the psola latency. Fixed at creation. */
  voiceEngineF0MinHz: number;
  /** How the target pitch is chosen. */
  pitchCorrectionMode: PitchCorrectionMode;
  /** Correction strength in [0, 1]. */
  pitchCorrectionAmount: number;
  /** Glide time to the target in ms, [0, 400]. */
  pitchCorrectionSpeedMs: number;
  /** Scale root, 0 = C .. 11 = B; validated but unused for a chromatic scale. */
  pitchCorrectionKeyRoot: number;
  /** Scale used by auto pitch correction. */
  pitchCorrectionScale: ScaleKind;
  /** Octave shift of the robot target, [-1, 1]. */
  pitchCorrectionRobotOctave: number;
  /** Harmony voices beside the lead, [0, 3]. */
  harmonyVoices: number;
  /** Scale degrees of harmony voices 1 to 3 from the lead, each in [-7, 7] (0 = unison, +2 = a third up). */
  harmonySteps: readonly [number, number, number];
  /** Level of the harmony voices. */
  harmonyLevel: number;
  /** Level of the lead voice in the harmony mix. */
  harmonyLeadLevel: number;
  /** Detune of the harmony voices in cents, [0, 30]. */
  harmonyDetuneCents: number;
  /** Delay of the harmony voices in ms, [0, 40]. */
  harmonyDelayMs: number;
  /** True when the vocoder is on. */
  vocoderEnabled: boolean;
  /** Band count, [8, 32]. Fixed at creation. */
  vocoderBands: number;
  /** Vocoder envelope attack in ms. */
  vocoderAttackMs: number;
  /** Vocoder envelope release in ms. */
  vocoderReleaseMs: number;
  /** Amount of sibilance passed through the vocoder. */
  vocoderSibilance: number;
  /**
   * `'external'` reads the `carrier` passed to the process methods (the worklet's
   * second input); a carrier-less process call is then refused with an `InvalidState` error.
   */
  vocoderCarrierSource: VocoderCarrierSource;
  /** Waveform of the internal vocoder carrier. */
  vocoderCarrierWave: VocoderCarrierWave;
  /** Detune of the ensemble carrier in cents. */
  vocoderEnsembleDetuneCents: number;
  /** Vocoder output level. */
  vocoderLevel: number;
  /** Colouration applied after the voice stage. */
  characterType: CharacterType;
  /** Amount of the character colouration. */
  characterAmount: number;
  /** Tone tilt in [-1, 1]. */
  characterTone: number;
  /** Space effect selected by the reverb stage. */
  reverbVariation: ReverbVariation;
  /** Echo delay in ms, [1, 3000]. */
  reverbVariationDelayMs: number;
  /** Echo feedback in [0, 0.95]. */
  reverbVariationFeedback: number;
  /** Tempo of the tempo-synced delay, [30, 300]. */
  reverbVariationTempoBpm: number;
  /** Note value of the tempo-synced delay. */
  reverbVariationDivision: NoteDivision;
  /** What the voices do once every held MIDI note has been released. */
  midiRelease: MidiRelease;
}
