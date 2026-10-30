/**
 * Realtime voice-changer preset catalogue and configuration.
 */

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

export type RealtimeVoiceChangerConfigInput =
  | VoicePresetId
  | RealtimeVoiceChangerPreset
  | RealtimeVoiceChangerConfig;

export interface RealtimeVoiceChangerOptions {
  sampleRate: number;
  maxBlockSize?: number;
  channels?: 1 | 2;
  preset?: RealtimeVoiceChangerConfigInput;
}

/** Pitch engine: `'psola'` tracks f0; `'grain'` is the f0-less SOLA retune. Fixed when the changer is prepared. */
export type VoiceEngine = 'grain' | 'psola';

/** Pitch correction: off, snap to the key's scale (`'auto'`), or snap to one pitch class (`'robot'`). */
export type PitchCorrectionMode = 'off' | 'auto' | 'robot';

/** Scale a pitch correction snaps to. `'chromatic'` ignores the key root. */
export type ScaleKind = 'major' | 'natural_minor' | 'chromatic';

/** Where the vocoder takes its carrier from; `'external'` reads the `*WithCarrier` process input. */
export type VocoderCarrierSource = 'internal' | 'external';

/** Waveform of the vocoder's internal carrier. */
export type VocoderCarrierWave = 'saw' | 'pulse' | 'ensemble';

/** Character stage type. */
export type CharacterType = 'none' | 'megaphone' | 'radio' | 'bbd_chorus' | 'strobo';

/** Space stage variation; every variation shares `reverbMix`. */
export type ReverbVariation = 'reverb' | 'echo' | 'tempo_delay' | 'dub_echo';

/** Note length of the tempo delay. */
export type NoteDivision = 'quarter' | 'eighth' | 'dotted_eighth' | 'sixteenth';

/** What the voices do once every held note is released: keep the last one or follow the input. */
export type MidiRelease = 'hold' | 'input';

/**
 * Flat (normalized) realtime-voice-changer configuration, mirroring the
 * `SonareRealtimeVoiceChangerConfig` POD returned by
 * `realtimeVoiceChangerPresetConfig`. A field outside its documented range makes
 * `setConfig` and the constructor throw a `RangeError` naming the field and its
 * range; nothing is clamped.
 */
export interface RealtimeVoiceChangerConfig {
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
   * `formantFactor / 2^(retuneSemitones / 12)` within [0.55, 1.65], else the call throws a
   * `RangeError` naming the reachable `formantFactor` range. Fixed when the changer is prepared:
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
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
  /** Fixed when the changer is prepared. `'grain'` refuses pitch correction, harmony and the vocoder. */
  voiceEngineEngine: VoiceEngine;
  /** Lowest tracked f0 in Hz, [50, 200]; sets the psola latency. Fixed when the changer is prepared. */
  voiceEngineF0MinHz: number;
  pitchCorrectionMode: PitchCorrectionMode;
  /** Correction strength in [0, 1]. */
  pitchCorrectionAmount: number;
  /** Correction speed in ms, [0, 400]. */
  pitchCorrectionSpeedMs: number;
  /** Scale root, 0 = C .. 11 = B; validated but unused for a chromatic scale. */
  pitchCorrectionKeyRoot: number;
  pitchCorrectionScale: ScaleKind;
  /** Octave offset of the robot mode, [-1, 1]. */
  pitchCorrectionRobotOctave: number;
  /** Harmony voices beside the lead, [0, 3]. */
  harmonyVoices: number;
  /** Scale degrees of harmony voice 1 from the lead (0 = unison, 2 = a third up), [-7, 7]. */
  harmonyStep1: number;
  harmonyStep2: number;
  harmonyStep3: number;
  harmonyLevel: number;
  harmonyLeadLevel: number;
  /** Detune of the harmony voices in cents, [0, 30]. */
  harmonyDetuneCents: number;
  /** Harmony delay in ms, [0, 40]. */
  harmonyDelayMs: number;
  vocoderEnabled: boolean;
  /** Band count, [8, 32]. Fixed when the changer is prepared. */
  vocoderBands: number;
  /** Envelope attack in ms, [0.5, 50]. */
  vocoderAttackMs: number;
  /** Envelope release in ms, [2, 300]. */
  vocoderReleaseMs: number;
  vocoderSibilance: number;
  vocoderCarrierSource: VocoderCarrierSource;
  vocoderCarrierWave: VocoderCarrierWave;
  /** Detune of the ensemble carrier in cents, [0, 30]. */
  vocoderEnsembleDetuneCents: number;
  vocoderLevel: number;
  characterType: CharacterType;
  characterAmount: number;
  /** Tone in [-1, 1]. */
  characterTone: number;
  reverbVariationVariation: ReverbVariation;
  /** Echo delay in ms, [1, 3000]. */
  reverbVariationDelayMs: number;
  /** Echo feedback in [0, 0.95]. */
  reverbVariationFeedback: number;
  /** Tempo of the tempo delay in BPM, [30, 300]. */
  reverbVariationTempoBpm: number;
  reverbVariationDivision: NoteDivision;
  midiRelease: MidiRelease;
}
