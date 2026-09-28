// Type-only: erased before runtime, so this does not create a module cycle with
// playback_renderer.ts, which imports the config types declared below.
import type { HrtfSet } from './playback_renderer';

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
  input?: PlaybackInputConfig;
  target?: PlaybackTargetConfig;
  /** LFE level folded into L/R when the output has no LFE plane or no subwoofer. Realtime, default 0. */
  lfe_mix_db?: number;
  upmix?: PlaybackUpmixConfig;
  /** Static gain on the discrete centre of 5.1 / 7.1 input. Realtime, [-12, 12], default 0. */
  dialogue_level_db?: number;
  loudness?: PlaybackLoudnessConfig;
  night_mode?: PlaybackNightModeConfig;
  room?: PlaybackRoomConfig;
  head_tracking?: PlaybackHeadTrackingConfig;
  output_limiter?: PlaybackOutputLimiterConfig;
}

/** A speaker role name (`schemas/playback-renderer-config.schema.json` `$defs.role`). */
export type PlaybackChannelRole = 'L' | 'R' | 'C' | 'LFE' | 'Ls' | 'Rs' | 'Lss' | 'Rss';

export interface PlaybackInputConfig {
  /** Prepare. `"auto"` follows the channel count of each processed block (1, 2, 6 or 8). Default `"auto"`. */
  layout?: 'auto' | 'mono' | 'stereo' | '5.1' | '7.1';
  /**
   * Prepare. Role of each input channel, covering the fixed layout's roles
   * exactly once; requires a fixed `layout`. `null` means canonical order.
   */
  channel_map?: PlaybackChannelRole[] | null;
}

export interface PlaybackSpeakerConfig {
  /** Prepare. Listener distance in metres, [0.1, 30]; `null` disables distance compensation. Default `null`. */
  distance_m?: number | null;
  /** Realtime. Level trim in dB, [-20, 20]. Default 0. */
  trim_db?: number;
  /** Prepare. A `"small"` speaker is high-passed at the crossover when bass management is enabled. Default `"large"`. */
  size?: 'large' | 'small';
}

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

export interface PlaybackTargetConfig {
  /** Prepare. Default `"headphones"`. */
  kind?: 'headphones' | 'speakers';
  /** Prepare. Required for `kind: "speakers"`, forbidden for `"headphones"`. */
  layout?: 'stereo' | '5.1' | '7.1';
  /** Per-speaker calibration keyed by role; only the non-LFE roles of the output layout. */
  speakers?: Partial<Record<Exclude<PlaybackChannelRole, 'LFE'>, PlaybackSpeakerConfig>>;
  bass_management?: PlaybackBassManagementConfig;
}

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

export interface PlaybackLoudnessConfig {
  /** Realtime. Measured program loudness, [-70, 0]; `null` means no alignment gain. Default `null`. */
  program_lufs?: number | null;
  /** Realtime. [-40, -5]. Default -24. */
  target_lufs?: number;
}

export interface PlaybackNightModeConfig {
  /** Realtime. 0 disables the dynamic range control. [0, 1]. Default 0. */
  amount?: number;
}

export interface PlaybackRoomConfig {
  /** Prepare. Headphones only. Default `"living_room"`. */
  preset?: 'none' | 'living_room' | 'home_theater' | 'screening_room';
  /** Realtime. Level of the early reflections and the late reverberation, [-30, 6]. Default -6. */
  mix_db?: number;
  /** Realtime. Default true. */
  enabled?: boolean;
}

export interface PlaybackHeadTrackingConfig {
  /** Realtime. false treats the head pose as zero. Default true. */
  enabled?: boolean;
}

export interface PlaybackOutputLimiterConfig {
  /** Realtime. The latency does not change. Default true. */
  enabled?: boolean;
  /** Realtime. [-12, 0]. Default -1. */
  ceiling_db?: number;
}

/** A stage name used in {@link PlaybackDiagnostics.inactive_stages} and `.latency.stages`. */
export type PlaybackStageName =
  | 'reorder'
  | 'dialogue_level'
  | 'upmix'
  | 'loudness'
  | 'night_mode'
  | 'layout_convert'
  | 'speaker_calibration'
  | 'bass_management'
  | 'binaural'
  | 'room_early'
  | 'room_late'
  | 'output_limiter';

/** Per-stage reported latency, in samples and in Q8 fixed-point. */
export interface PlaybackStageLatency {
  samples: number;
  q8: number;
}

/**
 * The renderer's diagnostics document, a plain object that can be sent through
 * `postMessage` as is. Field spelling is the core's own (snake_case), the same
 * pass-through convention as {@link PlaybackRendererConfig}.
 */
export interface PlaybackDiagnostics {
  active_input_layout: 'mono' | 'stereo' | '5.1' | '7.1';
  layout_switches: number;
  truncated_drains: number;
  /** Stages this configuration does not exercise (e.g. `room_early` for a speakers target). */
  inactive_stages: PlaybackStageName[];
  latency: {
    samples: number;
    /** Every stage name reports an entry, whether or not it is currently active. */
    stages: Record<PlaybackStageName, PlaybackStageLatency>;
  };
  loudness_gain_db: number;
  loudness_gain_clamped: boolean;
  /** True when an `hrtf` was supplied to a speakers target, which ignores it. */
  hrtf_ignored: boolean;
  limiter_gain_reduction_db: number;
  non_finite_discards: number;
}

/** Request shape for the {@link PlaybackRenderer} constructor. */
export interface PlaybackRendererOptions {
  config: PlaybackRendererConfig | string;
  /**
   * Required for a headphones target: this build embeds no HRTF data. Load the
   * package asset `./hrtf/default.shrf` (or any SHRF v1 file) and pass it
   * through `HrtfSet.fromBytes`. Ignored by a speakers target.
   */
  hrtf?: HrtfSet;
  /** Default 48000. */
  sampleRate?: number;
  /** Upper bound on the frames of every process call. Default 1024. */
  maxBlockSize?: number;
}

/** Request shape for the one-shot `renderPlayback`. */
export interface RenderPlaybackRequest {
  /** Interleaved input, `channels` samples per frame. */
  samples: Float32Array;
  channels: number;
  sampleRate: number;
  config: PlaybackRendererConfig | string;
  /** Required for a headphones target; see {@link PlaybackRendererOptions.hrtf}. */
  hrtf?: HrtfSet;
}

/** `renderPlayback`'s result: interleaved output with the target's own channel count. */
export interface RenderPlaybackResult {
  samples: Float32Array;
  channels: number;
}
