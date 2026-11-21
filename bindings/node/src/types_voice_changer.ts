/**
 * Realtime voice-changer preset catalogue and configuration.
 */
import type { FormantMode } from './voice_changer.js';

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

/** Identifying metadata shared by every preset document. */
export interface RealtimeVoiceChangerPresetMetadata {
  /** Preset document schema version; always `4`. */
  schemaVersion: 4;
  /** Preset identifier. */
  id: string;
  /** Display name of the preset. */
  name: string;
  /** Free-form description of the preset. Optional. */
  description?: string;
  /** Catalogue category the preset belongs to. */
  category: VoicePresetCategory;
}

/** A preset document: metadata plus either a nested `dsp` section or a `macros` map, never both. */
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

/** Anything a voice changer accepts as its configuration: a preset id, a preset document or a flat config. */
export type RealtimeVoiceChangerConfigInput =
  | VoicePresetId
  | RealtimeVoiceChangerPreset
  | RealtimeVoiceChangerConfig;

/** Construction options for `RealtimeVoiceChanger`. */
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

/** Pitch engine: `'psola'` tracks f0; `'grain'` is the f0-less SOLA retune. Fixed when the changer is prepared. */
export type VoiceEngine = 'grain' | 'psola';

/** Pitch correction: off, snap to the key's scale (`'auto'`), or snap to one pitch class (`'robot'`). */
export type PitchCorrectionMode = 'off' | 'auto' | 'robot';

/** Scale a pitch correction snaps to. `'chromatic'` ignores the key root. */
export type ScaleKind = 'major' | 'naturalMinor' | 'chromatic';

/** Where the vocoder takes its carrier from; `'external'` reads the `carrier` argument of the process calls. */
export type VocoderCarrierSource = 'internal' | 'external';

/** Waveform of the vocoder's internal carrier. */
export type VocoderCarrierWave = 'saw' | 'pulse' | 'ensemble';

/** Character stage type. */
export type CharacterType = 'none' | 'megaphone' | 'radio' | 'bbdChorus' | 'strobo';

/** Space stage variation; every variation shares `reverbMix`. */
export type ReverbVariation = 'reverb' | 'echo' | 'tempoDelay' | 'dubEcho';

/** Note length of the tempo delay. */
export type NoteDivision = 'quarter' | 'eighth' | 'dottedEighth' | 'sixteenth';

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
  /** Input gain in dB, [-24, 24]. */
  inputGainDb: number;
  /** Output gain in dB, [-36, 12]. */
  outputGainDb: number;
  /** Dry/wet mix in [0, 1]; 1 is fully processed. */
  wetMix: number;
  /** Retune pitch shift in semitones, [-24, 24]. */
  retunePitchSemitones: number;
  /** Retune wet amount in [0, 1]. */
  retuneMix: number;
  /** Retune grain length in samples, a multiple of 4 in [4, 8192]; omitted for the automatic grain. */
  retuneGrainSizeSamples?: number;
  /** Formant scale factor, [0.55, 1.65]; 1 leaves the formants alone. See `formantMode`. */
  formantFactor: number;
  /** Strength of the formant displacement in [0, 1]. Ignored when `formantMode` is `'absolute'`. */
  formantAmount: number;
  /** Formant body amount in [-1, 1]. */
  formantBody: number;
  /** Formant brightness amount in [-1, 1]. */
  formantBrightness: number;
  /** Formant nasal amount in [-1, 1]. */
  formantNasal: number;
  /**
   * How `formantFactor` acts. `'relative'` leaves it to the colour stage
   * after the retune. `'absolute'` warps the formants to `formantFactor` times the input's ahead
   * of the retune, adds one analysis frame of latency and ignores `formantAmount`; it needs
   * `formantFactor / 2^(retunePitchSemitones / 12)` within [0.55, 1.65], else the call throws a
   * `RangeError` naming the reachable `formantFactor` range. Fixed when the changer is prepared:
   * a config carrying a different mode is refused. Required, like every POD field.
   */
  formantMode: FormantMode;
  /** High-pass corner in Hz, [20, 300]. */
  eqHighPassHz: number;
  /** Body band gain in dB, [-12, 12]. */
  eqBodyDb: number;
  /** Presence band gain in dB, [-12, 12]. */
  eqPresenceDb: number;
  /** Air band gain in dB, [-12, 12]. */
  eqAirDb: number;
  /** Gate threshold in dB, [-90, -12]. */
  gateThresholdDb: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  gateAttackMs: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  gateReleaseMs: number;
  /** Gate attenuation depth in dB, [0, 80]. */
  gateRangeDb: number;
  /** Compressor threshold in dB, [-60, 0]. */
  compressorThresholdDb: number;
  /** Compression ratio, [1, 20]. */
  compressorRatio: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  compressorAttackMs: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  compressorReleaseMs: number;
  /** Compressor makeup gain in dB, [-12, 12]. */
  compressorMakeupGainDb: number;
  /** De-esser centre frequency in Hz, [3000, 12000]. */
  deesserFrequencyHz: number;
  /** De-esser threshold in dB, [-60, -6]. */
  deesserThresholdDb: number;
  /** De-esser ratio, [1, 20]. */
  deesserRatio: number;
  /** Maximum de-esser attenuation in dB, [0, 24]. */
  deesserRangeDb: number;
  /** Space-stage wet level in [0, 0.45]; shared by every `reverbVariation`. */
  reverbMix: number;
  /** Reverb decay time in ms, [40, 1800]. */
  reverbTimeMs: number;
  /** Reverb high-frequency damping in [0, 1]. */
  reverbDamping: number;
  /** Integer seed of the reverb's random generator (32-bit signed). */
  reverbSeed: number;
  /** Limiter ceiling in dB, [-12, -1]. */
  limiterCeilingDb: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  limiterReleaseMs: number;
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
  /** Fixed when the changer is prepared. `'grain'` refuses pitch correction, harmony and the vocoder. */
  voiceEngine: VoiceEngine;
  /** Lowest tracked f0 in Hz, [50, 200]; sets the psola latency. Fixed when the changer is prepared. */
  voiceEngineF0MinHz: number;
  /** Pitch correction mode: off, scale snap or single-pitch-class snap. */
  pitchCorrectionMode: PitchCorrectionMode;
  /** Correction strength in [0, 1]. */
  pitchCorrectionAmount: number;
  /** Correction speed in ms, [0, 400]. */
  pitchCorrectionSpeedMs: number;
  /** Scale root, 0 = C .. 11 = B; validated but unused for a chromatic scale. */
  pitchCorrectionKeyRoot: number;
  /** Scale the correction snaps to. */
  pitchCorrectionScale: ScaleKind;
  /** Octave offset of the robot mode, [-1, 1]. */
  pitchCorrectionRobotOctave: number;
  /** Harmony voices beside the lead, [0, 3]. */
  harmonyVoices: number;
  /** Scale degrees of harmony voices 1 to 3 from the lead, each in [-7, 7] (0 = unison, 2 = a third up). */
  harmonySteps: readonly [number, number, number];
  /** Level of the harmony voices in [0, 1]. */
  harmonyLevel: number;
  /** Level of the lead voice in [0, 1]. */
  harmonyLeadLevel: number;
  /** Detune of the harmony voices in cents, [0, 30]. */
  harmonyDetuneCents: number;
  /** Harmony delay in ms, [0, 40]. */
  harmonyDelayMs: number;
  /** Enables the channel vocoder. */
  vocoderEnabled: boolean;
  /** Band count, [8, 32]. Fixed when the changer is prepared. */
  vocoderBands: number;
  /** Envelope attack in ms, [0.5, 50]. */
  vocoderAttackMs: number;
  /** Envelope release in ms, [2, 300]. */
  vocoderReleaseMs: number;
  /** Vocoder sibilance amount in [0, 1]. */
  vocoderSibilance: number;
  /** Where the vocoder takes its carrier from. */
  vocoderCarrierSource: VocoderCarrierSource;
  /** Waveform of the internal carrier. */
  vocoderCarrierWave: VocoderCarrierWave;
  /** Detune of the ensemble carrier in cents, [0, 30]. */
  vocoderEnsembleDetuneCents: number;
  /** Vocoder output level in [0, 1]. */
  vocoderLevel: number;
  /** Character stage type. */
  characterType: CharacterType;
  /** Character stage amount in [0, 1]. */
  characterAmount: number;
  /** Tone in [-1, 1]. */
  characterTone: number;
  /** Space stage variation. */
  reverbVariation: ReverbVariation;
  /** Echo delay in ms, [1, 3000]. */
  reverbVariationDelayMs: number;
  /** Echo feedback in [0, 0.95]. */
  reverbVariationFeedback: number;
  /** Tempo of the tempo delay in BPM, [30, 300]. */
  reverbVariationTempoBpm: number;
  /** Note length of the tempo delay. */
  reverbVariationDivision: NoteDivision;
  /** Note-release mode: `'hold'` keeps the last held note once every note is released, `'input'` returns to following the input. */
  midiRelease: MidiRelease;
}
