// Type-only: erased before runtime, so this does not create an actual module
// cycle with playback.ts, which imports the config types declared below.
import type { HrtfSet } from './playback.js';

/**
 * The playback renderer configuration document, in the schema
 * `schemas/playback-renderer-config.schema.json` and `sonare_playback_renderer_create_json`
 * read. The document passes to the shared C++ core as written, and the core
 * refuses an earlier snake_case key or abbreviated role token by name.
 *
 * Every key is either a "prepare" key (fixed at {@link PlaybackRenderer}
 * construction; changing one through {@link PlaybackRenderer.setConfig}
 * throws) or a "realtime" key (adopted at the next processed block). Omitted
 * keys take the schema defaults documented on each field below.
 */
export interface PlaybackRendererConfig {
  /** Document version. Omitted: the current version; any value but 2 is refused. `config()` writes it first. */
  version?: 2;
  /** Input layout and channel mapping. Omitted: layout `"auto"`, canonical channel order. */
  input?: PlaybackInputConfig;
  /** Output target. Omitted: headphones with the default room preset. */
  target?: PlaybackTargetConfig;
  /** LFE level folded into left/right when the output has no LFE plane or no subwoofer. Realtime, default 0. */
  lfeMixDb?: number;
  /** Stereo-to-surround upmix settings. Omitted: schema defaults. */
  upmix?: PlaybackUpmixConfig;
  /** Static gain on the discrete centre of 5.1 / 7.1 input. Realtime, [-12, 12], default 0. */
  dialogueLevelDb?: number;
  /** Program loudness alignment. Omitted: no alignment gain. */
  loudness?: PlaybackLoudnessConfig;
  /** Night-mode dynamic range control. Omitted: disabled (amount 0). */
  nightMode?: PlaybackNightModeConfig;
  /** Room simulation for a headphones target. Omitted: schema defaults (`livingRoom`, -6 dB). */
  room?: PlaybackRoomConfig;
  /** Head-tracking settings. Omitted: enabled. */
  headTracking?: PlaybackHeadTrackingConfig;
  /** Final output limiter. Omitted: enabled with a -1 dB ceiling. */
  outputLimiter?: PlaybackOutputLimiterConfig;
}

/** A `SpeakerRole` name (`schemas/playback-renderer-config.schema.json` `$defs.role`). */
export type PlaybackChannelRole =
  | 'left'
  | 'right'
  | 'center'
  | 'lfe'
  | 'leftSurround'
  | 'rightSurround'
  | 'leftSideSurround'
  | 'rightSideSurround';

/** Input layout settings of {@link PlaybackRendererConfig}; all keys are optional. */
export interface PlaybackInputConfig {
  /** Prepare. `"auto"` follows the channel count of each processed block (1, 2, 6 or 8). Default `"auto"`. */
  layout?: 'auto' | 'mono' | 'stereo' | '5.1' | '7.1';
  /**
   * Prepare. Role of each input channel, covering the fixed layout's roles
   * exactly once; requires a fixed `layout`. Absent means canonical order, and
   * `config()` omits the key while no map is set.
   */
  channelMap?: PlaybackChannelRole[];
}

/** Calibration of one speaker of the output layout; all keys are optional. */
export interface PlaybackSpeakerConfig {
  /** Prepare. Listener distance in metres, [0.1, 30]; absent disables distance compensation, and `config()` omits the key while it is unset. */
  distanceM?: number;
  /** Realtime. Level trim in dB, [-20, 20]. Default 0. */
  trimDb?: number;
  /** Prepare. A `"small"` speaker is high-passed at the crossover when bass management is enabled. Default `"large"`. */
  size?: 'large' | 'small';
}

/** Bass management of a speakers target; all keys are optional. */
export interface PlaybackBassManagementConfig {
  /** Prepare. Default false. */
  enabled?: boolean;
  /** Prepare. LR4 crossover frequency, [40, 200]. Default 80. */
  crossoverHz?: number;
  /** Prepare. false folds the low band and LFE into the large left/right pair; left and right must then be large. Default true. */
  subwoofer?: boolean;
  /** Realtime. LFE gain when feeding the subwoofer, [-10, 15]. Default 10. */
  lfeGainDb?: number;
}

/** Output target of {@link PlaybackRendererConfig}; all keys are optional. */
export interface PlaybackTargetConfig {
  /** Prepare. Default `"headphones"`. */
  kind?: 'headphones' | 'speakers';
  /** Prepare. Required for `kind: "speakers"`, forbidden for `"headphones"`. */
  layout?: 'stereo' | '5.1' | '7.1';
  /** Per-speaker calibration keyed by role; only the non-LFE roles of the output layout. */
  speakers?: Partial<Record<Exclude<PlaybackChannelRole, 'lfe'>, PlaybackSpeakerConfig>>;
  /** Bass management. Omitted: disabled (`enabled` false) with the defaults of the other keys. */
  bassManagement?: PlaybackBassManagementConfig;
}

/** Stereo upmix settings; all keys are optional and realtime. */
export interface PlaybackUpmixConfig {
  /** Realtime. Stereo input only; the latency does not change. Default true. */
  enabled?: boolean;
  /** Realtime. Width of the centre window on the panning index, [0.05, 1]. Default 0.2. */
  centerWidth?: number;
  /** Realtime. Power share of the ambience kept in the front pair, [0, 1]. Default 0.5. */
  frontAmbience?: number;
  /** Realtime. Derive LFE from the low-passed L/R sum. Default false. */
  lfeFromUpmix?: boolean;
}

/** Loudness alignment settings; all keys are optional and realtime. */
export interface PlaybackLoudnessConfig {
  /** Realtime. Measured program loudness, [-70, 0]; absent means no alignment gain, and `config()` omits the key while it is unset. */
  programLufs?: number;
  /** Realtime. [-40, -5]. Default -24. */
  targetLufs?: number;
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
  mixDb?: number;
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
  /** Realtime. Sample-peak ceiling in dBFS, [-12, 0]. Default -1. */
  ceilingDb?: number;
}

/** A stage name used in {@link PlaybackDiagnostics.inactiveStages} and `.latency.stages`. */
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

/** Per-stage reported latency. */
export interface PlaybackStageLatency {
  /** Stage latency in samples; fractional where a stage's delay is not a whole sample. */
  samples: number;
}

/**
 * {@link PlaybackRenderer.diagnostics}'s result, passed through from the core as
 * written.
 */
export interface PlaybackDiagnostics {
  /** Input layout the renderer is currently processing; follows the channel count under `"auto"`. */
  activeInputLayout: 'mono' | 'stereo' | '5.1' | '7.1';
  /** Number of input layout changes handled since construction. */
  layoutSwitches: number;
  /** Number of layout switches that cut a previous layout's tail short with a fade-out. */
  truncatedDrains: number;
  /** Stages this configuration does not exercise (e.g. `roomEarly` for a speakers target). */
  inactiveStages: PlaybackStageName[];
  /** Reported latency of the active signal path. */
  latency: {
    /** Total latency in samples at the renderer's sample rate. */
    samples: number;
    /** Every stage name reports an entry, whether or not it is currently active. */
    stages: Record<PlaybackStageName, PlaybackStageLatency>;
  };
  /** Alignment gain currently applied by the loudness stage, in dB. */
  loudnessGainDb: number;
  /** True when the loudness alignment gain was limited to its allowed range. */
  loudnessGainClamped: boolean;
  /** True when a headphones-target `hrtf` was ignored (the target is speakers). */
  hrtfIgnored: boolean;
  /** Deepest last-block gain reduction across the main and LFE limiters, in dB (<= 0). */
  limiterGainReductionDb: number;
  /** Count of NaN/Inf output samples replaced by silence since construction. */
  nonFiniteDiscards: number;
}

/** Request shape for {@link PlaybackRenderer}'s constructor. */
export interface PlaybackRendererOptions {
  /** Renderer configuration document; see {@link PlaybackRendererConfig}. */
  config: PlaybackRendererConfig;
  /** Required for a headphones target on WASM; optional (defaults to the built-in set) elsewhere. */
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
  /** Integer sample rate in Hz, [8000, 384000]; out-of-range values throw RangeError. */
  sampleRate: number;
}

/** Request shape for the one-shot {@link renderPlayback} facade. */
export interface RenderPlaybackRequest {
  /** Interleaved input, `numChannels` samples per frame; must be finite. */
  samples: Float32Array;
  /** Interleaved channel count of the input: 1, 2, 6 or 8; the configured input layout must accept it. */
  numChannels: number;
  /** Sample rate of the input, in Hz; integer in `[8000, 384000]`. Required. */
  sampleRate: number;
  /** Renderer configuration document; see {@link PlaybackRendererConfig}. */
  config: PlaybackRendererConfig;
  /** HRTF set for a headphones target. Omitted: the built-in set. Ignored by a speakers target. */
  hrtf?: HrtfSet;
}

/** {@link renderPlayback}'s result: the output has its own channel count, which the caller cannot know in advance. */
export interface RenderPlaybackResult {
  /** Interleaved output, `numChannels` samples per frame, at the input sample rate. */
  samples: Float32Array;
  /** Output channel count: that of the target layout (2 for headphones). */
  numChannels: number;
}
