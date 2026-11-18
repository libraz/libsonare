// Type-only: erased before runtime, so this does not create a module cycle with
// playback_renderer.ts, which imports the config types declared below.
import type { HrtfSet } from './playback_renderer.js';

/**
 * The playback renderer configuration document, in the schema
 * `schemas/playback-renderer-config.schema.json` (also shipped as the
 * `./schemas/playback-renderer-config.schema.json` package export). Field
 * spelling is the schema's own (snake_case, dotted nesting) rather than this
 * binding's usual camelCase: the document is parsed by the shared C++ core, so
 * every surface has to agree on one literal spelling.
 *
 * Every key is either a "prepare" key (fixed at {@link PlaybackRenderer}
 * construction; changing one through `setConfig` throws) or a "realtime" key
 * (adopted at the next processed block). Omitted keys take the schema defaults
 * documented on each field below.
 */
export interface PlaybackRendererConfig {
  /** Input layout and channel mapping. Omitted: layout `"auto"`, canonical channel order. */
  input?: PlaybackInputConfig;
  /** Output target. Omitted: headphones with the default room preset. */
  target?: PlaybackTargetConfig;
  /** LFE level folded into L/R when the output has no LFE plane or no subwoofer. Realtime, default 0. */
  lfe_mix_db?: number;
  /** Stereo-to-surround upmix settings. Omitted: schema defaults. */
  upmix?: PlaybackUpmixConfig;
  /** Static gain on the discrete centre of 5.1 / 7.1 input. Realtime, [-12, 12], default 0. */
  dialogue_level_db?: number;
  /** Program loudness alignment. Omitted: no alignment gain. */
  loudness?: PlaybackLoudnessConfig;
  /** Night-mode dynamic range control. Omitted: disabled (amount 0). */
  night_mode?: PlaybackNightModeConfig;
  /** Room simulation for a headphones target. Omitted: schema defaults (`livingRoom`, -6 dB). */
  room?: PlaybackRoomConfig;
  /** Head-tracking settings. Omitted: enabled. */
  head_tracking?: PlaybackHeadTrackingConfig;
  /** Final output limiter. Omitted: enabled with a -1 dB ceiling. */
  output_limiter?: PlaybackOutputLimiterConfig;
}

/** A speaker role name (`schemas/playback-renderer-config.schema.json` `$defs.role`). */
export type PlaybackChannelRole = 'L' | 'R' | 'C' | 'LFE' | 'Ls' | 'Rs' | 'Lss' | 'Rss';

/** Input layout settings of {@link PlaybackRendererConfig}; all keys are optional. */
export interface PlaybackInputConfig {
  /** Prepare. `"auto"` follows the channel count of each processed block (1, 2, 6 or 8). Default `"auto"`. */
  layout?: 'auto' | 'mono' | 'stereo' | '5.1' | '7.1';
  /**
   * Prepare. Role of each input channel, covering the fixed layout's roles
   * exactly once; requires a fixed `layout`. `null` means canonical order, and
   * `config()` returns the key as `null` when no map is set.
   */
  channel_map?: PlaybackChannelRole[] | null;
}

/** Calibration of one speaker of the output layout; all keys are optional. */
export interface PlaybackSpeakerConfig {
  /** Prepare. Listener distance in metres, [0.1, 30]; `null` disables distance compensation. Default `null`; `config()` returns the key for each speaker of the layout, `null` when unset. */
  distance_m?: number | null;
  /** Realtime. Level trim in dB, [-20, 20]. Default 0. */
  trim_db?: number;
  /** Prepare. A `"small"` speaker is high-passed at the crossover when bass management is enabled. Default `"large"`. */
  size?: 'large' | 'small';
}

/** Bass management of a speakers target; all keys are optional. */
export interface PlaybackBassManagementConfig {
  /** Prepare. Default false. */
  enabled?: boolean;
  /** Prepare. LR4 crossover frequency, [40, 200]. Default 80. */
  crossover_hz?: number;
  /** Prepare. false folds the low band and LFE into the large L/R pair; L and R must then be large. Default true. */
  subwoofer?: boolean;
  /** Realtime. LFE gain when feeding the subwoofer, [-10, 15]. Default 10. */
  lfe_gain_db?: number;
}

/** Output target of {@link PlaybackRendererConfig}; all keys are optional. */
export interface PlaybackTargetConfig {
  /** Prepare. Default `"headphones"`. */
  kind?: 'headphones' | 'speakers';
  /** Prepare. Required for `kind: "speakers"`, forbidden for `"headphones"`. */
  layout?: 'stereo' | '5.1' | '7.1';
  /** Per-speaker calibration keyed by role; only the non-LFE roles of the output layout. */
  speakers?: Partial<Record<Exclude<PlaybackChannelRole, 'LFE'>, PlaybackSpeakerConfig>>;
  /** Bass management. Omitted: disabled (`enabled` false) with the defaults of the other keys. */
  bass_management?: PlaybackBassManagementConfig;
}

/** Stereo upmix settings; all keys are optional and realtime. */
export interface PlaybackUpmixConfig {
  /** Realtime. Stereo input only; the latency does not change. Default true. */
  enabled?: boolean;
  /** Realtime. Width of the centre window on the panning index, [0.05, 1]. Default 0.2. */
  center_width?: number;
  /** Realtime. Power share of the ambience kept in the front pair, [0, 1]. Default 0.5. */
  front_ambience?: number;
  /** Realtime. Derive LFE from the low-passed L/R sum. Default false. */
  lfe_from_upmix?: boolean;
}

/** Loudness alignment settings; all keys are optional and realtime. */
export interface PlaybackLoudnessConfig {
  /** Realtime. Measured program loudness, [-70, 0]; `null` means no alignment gain. Default `null`; `config()` always returns the key, `null` when unset. */
  program_lufs?: number | null;
  /** Realtime. [-40, -5]. Default -24. */
  target_lufs?: number;
}

/** Night-mode dynamic range control settings; the key is optional and realtime. */
export interface PlaybackNightModeConfig {
  /** Realtime. 0 disables the dynamic range control. [0, 1]. Default 0. */
  amount?: number;
}

/** Room simulation settings for a headphones target; all keys are optional. */
export interface PlaybackRoomConfig {
  /** Prepare. Headphones only. Default `"livingRoom"`. */
  preset?: 'none' | 'livingRoom' | 'homeTheater' | 'screeningRoom';
  /** Realtime. Level of the early reflections and the late reverberation, [-30, 6]. Default -6. */
  mix_db?: number;
  /** Realtime. Default true. */
  enabled?: boolean;
}

/** Head-tracking settings; the key is optional and realtime. */
export interface PlaybackHeadTrackingConfig {
  /** Realtime. false treats the head pose as zero. Default true. */
  enabled?: boolean;
}

/** Output limiter settings; all keys are optional and realtime. */
export interface PlaybackOutputLimiterConfig {
  /** Realtime. The latency does not change. Default true. */
  enabled?: boolean;
  /** Realtime. [-12, 0]. Default -1. */
  ceiling_db?: number;
}

/** A stage name used in {@link PlaybackDiagnostics.inactive_stages} and `.latency.stages`. */
export type PlaybackStageName =
  | 'reorder'
  | 'dialogueLevel'
  | 'upmix'
  | 'loudness'
  | 'nightMode'
  | 'layoutConvert'
  | 'speakerCalibration'
  | 'bassManagement'
  | 'binaural'
  | 'roomEarly'
  | 'roomLate'
  | 'outputLimiter';

/** Per-stage reported latency, in samples and in Q8 fixed-point. */
export interface PlaybackStageLatency {
  /** Stage latency in samples, rounded from `q8`. */
  samples: number;
  /** Stage latency in samples as 8-bit fixed point (`samples * 256`). */
  q8: number;
}

/**
 * The renderer's diagnostics document, a plain object that can be sent through
 * `postMessage` as is. Field spelling is the core's own (snake_case), the same
 * pass-through convention as {@link PlaybackRendererConfig}.
 */
export interface PlaybackDiagnostics {
  /** Input layout the renderer is currently processing; follows the channel count under `"auto"`. */
  active_input_layout: 'mono' | 'stereo' | '5.1' | '7.1';
  /** Number of input layout changes handled since construction. */
  layout_switches: number;
  /** Number of layout switches that cut a previous layout's tail short with a fade-out. */
  truncated_drains: number;
  /** Stages this configuration does not exercise (e.g. `roomEarly` for a speakers target). */
  inactive_stages: PlaybackStageName[];
  /** Reported latency of the active signal path. */
  latency: {
    /** Total latency in samples at the renderer's sample rate. */
    samples: number;
    /** Every stage name reports an entry, whether or not it is currently active. */
    stages: Record<PlaybackStageName, PlaybackStageLatency>;
  };
  /** Alignment gain currently applied by the loudness stage, in dB. */
  loudness_gain_db: number;
  /** True when the loudness alignment gain was limited to its allowed range. */
  loudness_gain_clamped: boolean;
  /** True when an `hrtf` was supplied to a speakers target, which ignores it. */
  hrtf_ignored: boolean;
  /** Deepest last-block gain reduction across the main and LFE limiters, in dB (<= 0). */
  limiter_gain_reduction_db: number;
  /** Count of NaN/Inf output samples replaced by silence since construction. */
  non_finite_discards: number;
}

/** Request shape for the {@link PlaybackRenderer} constructor. */
export interface PlaybackRendererOptions {
  /** Renderer configuration document; see {@link PlaybackRendererConfig}. */
  config: PlaybackRendererConfig;
  /**
   * Required for a headphones target: this build embeds no HRTF data. Load the
   * package asset `./hrtf/default.shrf` (or any SHRF v1 file) and pass it
   * through `HrtfSet.fromBytes`. Ignored by a speakers target.
   */
  hrtf?: HrtfSet;
  /** Sample rate in Hz, integer in `[8000, 384000]`. Required. */
  sampleRate: number;
  /** Largest block per call, in frames. Omit it for the renderer's default (1024); `0` is a `RangeError`. */
  maxBlockSize?: number;
}

/** Request shape for the {@link PlaybackLoudnessMeter} constructor. */
export interface PlaybackLoudnessMeterOptions {
  /** Interleaved channel count of the program: 1, 2, 6 or 8. */
  numChannels: number;
  /** Sample rate of the program, in Hz. */
  sampleRate: number;
}

/** Request shape for the one-shot `renderPlayback`. */
export interface RenderPlaybackRequest {
  /** Interleaved input, `numChannels` samples per frame; must be finite unless `validate` is false. */
  samples: Float32Array;
  /** Interleaved channel count of the input: 1, 2, 6 or 8; the configured input layout must accept it. */
  numChannels: number;
  /** Sample rate of the input, in Hz; integer in `[8000, 384000]`. Required. */
  sampleRate: number;
  /** Renderer configuration document; see {@link PlaybackRendererConfig}. */
  config: PlaybackRendererConfig;
  /** Required for a headphones target; see {@link PlaybackRendererOptions.hrtf}. */
  hrtf?: HrtfSet;
  /** Set `false` to skip the NaN/Inf pre-scan of the samples. Default `true`. */
  validate?: boolean;
}

/** `renderPlayback`'s result: interleaved output with the target's own channel count. */
export interface RenderPlaybackResult {
  /** Interleaved output, `numChannels` samples per frame, at the input sample rate. */
  samples: Float32Array;
  /** Output channel count: that of the target layout (2 for headphones). */
  numChannels: number;
}
