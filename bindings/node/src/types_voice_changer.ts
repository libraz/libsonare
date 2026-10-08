/**
 * Realtime voice-changer preset catalogue and configuration.
 */

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

/**
 * Flat (normalized) realtime-voice-changer configuration, mirroring the
 * `SonareRealtimeVoiceChangerConfig` POD returned by
 * `realtimeVoiceChangerPresetConfig`.
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
   * How `formantFactor` acts. `'relative'` (default when omitted) leaves it to the colour stage
   * after the retune. `'absolute'` warps the formants to `formantFactor` times the input's ahead
   * of the retune, adds one analysis frame of latency and ignores `formantAmount`; it needs
   * `formantFactor / 2^(retuneSemitones / 12)` within [0.55, 1.65], else the call throws a
   * `RangeError` naming the reachable `formantFactor` range. Fixed when the changer is prepared:
   * a config carrying a different mode is refused. Always present on a returned config.
   */
  formantMode?: 'relative' | 'absolute';
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
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
}
