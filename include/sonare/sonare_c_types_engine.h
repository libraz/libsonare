#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors engine::TelemetryErrorCode. These ordinals are part of the
   cross-binding telemetry contract: core owns 0..18 and 20, while ordinal 19
   is reserved by the WASM worklet protocol. SonareEngineTelemetry.error stays
   an int below so this enum does not change the telemetry POD layout. */
typedef enum {
  SONARE_ENGINE_TELEMETRY_ERROR_NONE = 0,
  SONARE_ENGINE_TELEMETRY_ERROR_COMMAND_QUEUE_OVERFLOW = 1,
  SONARE_ENGINE_TELEMETRY_ERROR_PENDING_COMMAND_OVERFLOW = 2,
  SONARE_ENGINE_TELEMETRY_ERROR_BOUNDARY_OVERFLOW = 3,
  SONARE_ENGINE_TELEMETRY_ERROR_TELEMETRY_OVERFLOW = 4,
  SONARE_ENGINE_TELEMETRY_ERROR_CAPTURE_OVERFLOW = 5,
  SONARE_ENGINE_TELEMETRY_ERROR_MAX_BLOCK_EXCEEDED = 6,
  SONARE_ENGINE_TELEMETRY_ERROR_UNKNOWN_TARGET = 7,
  SONARE_ENGINE_TELEMETRY_ERROR_NON_REALTIME_SAFE_PARAMETER = 8,
  SONARE_ENGINE_TELEMETRY_ERROR_NOT_PREPARED = 9,
  SONARE_ENGINE_TELEMETRY_ERROR_NON_QUEUEABLE_COMMAND = 10,
  SONARE_ENGINE_TELEMETRY_ERROR_AUTOMATION_BIND_TARGET_OVERFLOW = 11,
  SONARE_ENGINE_TELEMETRY_ERROR_STALE_AUTOMATION_LANES = 12,
  SONARE_ENGINE_TELEMETRY_ERROR_SMOOTHED_PARAMETER_CAPACITY = 13,
  SONARE_ENGINE_TELEMETRY_ERROR_COMMAND_BACKLOG_DEFERRED = 14,
  SONARE_ENGINE_TELEMETRY_ERROR_CLIP_PAGE_UNDERRUN = 15,
  SONARE_ENGINE_TELEMETRY_ERROR_INSERT_AUTOMATION_OVERFLOW = 16,
  SONARE_ENGINE_TELEMETRY_ERROR_MIDI_CLOCK_OVERFLOW = 17,
  SONARE_ENGINE_TELEMETRY_ERROR_METRONOME_OVERFLOW = 18,
  SONARE_ENGINE_TELEMETRY_ERROR_MAX_CHANNELS_EXCEEDED = 20
} SonareEngineTelemetryError;

/* Naming-compatible alias for code that mirrors the C++ enum's type name. */
typedef SonareEngineTelemetryError SonareEngineTelemetryErrorCode;

typedef struct {
  int type;
  int error;
  int64_t render_frame;
  int64_t timeline_sample;
  int64_t audible_timeline_sample;
  int32_t graph_latency_samples_q8;
  uint32_t value;
} SonareEngineTelemetry;

/* Mirrors engine::MeterTelemetryRecord: a fixed-size meter snapshot published by
   the engine's meter tap. Drained with sonare_engine_drain_meter_telemetry. */
typedef struct {
  uint32_t target_id;
  int64_t render_frame;
  uint64_t seq;
  float peak_db_l;
  float peak_db_r;
  float rms_db_l;
  float rms_db_r;
  float true_peak_db_l;
  float true_peak_db_r;
  float max_true_peak_db;
  float correlation;
  float mono_compat_width;
  float momentary_lufs;
  float short_term_lufs;
  float integrated_lufs;
  float gain_reduction_db;
  uint32_t dropped_records;
} SonareMeterTelemetryRecord;

/* Widest per-plane meter the wide telemetry record carries (7.1). */
#define SONARE_METER_MAX_CHANNELS 8

/* Per-plane meter snapshot for a surround mix target. Mirrors
   engine::MeterTelemetryRecord with the per-channel peak/rms/true_peak arrays
   exposed (planes [0, channel_count)). Drained with
   sonare_engine_drain_meter_telemetry_wide. The legacy stereo
   SonareMeterTelemetryRecord stays the byte-identical fast path for <=2ch
   targets; hosts pick the drain matching their target's bus layout. */
typedef struct {
  uint32_t target_id;
  int64_t render_frame;
  uint64_t seq;
  int32_t channel_count;
  float peak_db[SONARE_METER_MAX_CHANNELS];
  float rms_db[SONARE_METER_MAX_CHANNELS];
  float true_peak_db[SONARE_METER_MAX_CHANNELS];
  float max_true_peak_db;
  float correlation;
  float mono_compat_width;
  float momentary_lufs;
  float short_term_lufs;
  float integrated_lufs;
  float gain_reduction_db;
  uint32_t dropped_records;
} SonareMeterTelemetryRecordWide;

/* One lowered MIDI 1.0 message drained from the engine's external-MIDI output
   queue (sonare_engine_drain_external_midi). A single queued channel-voice event
   may lower to more than one message (e.g. a MIDI 2.0 program change with bank
   select). destination_id == 0xFFFFFFFF tags a transport/clock byte; otherwise
   it is the track's MIDI destination id. byte_count is 1..3 valid bytes.
   render_frame is the monotonic DEVICE render frame for every record (sequenced
   channel-voice events, live input, and clock/transport bytes alike), so drained
   events stay in dispatch order across a loop wrap or seek. */
typedef struct {
  uint32_t destination_id;
  uint32_t byte_count;
  int64_t render_frame;
  uint8_t bytes[3];
  uint8_t reserved[5];
} SonareExternalMidiEvent;

#define SONARE_SCOPE_MAX_BANDS 64
#define SONARE_SCOPE_MAX_POINTS 32

/* Mirrors engine::ScopeTelemetryRecord: a fixed-size spectrum + goniometer
   (vectorscope) snapshot for one mix target, published by the engine's scope tap
   and drained with sonare_engine_drain_scope_telemetry. band_count entries of
   bands[] (FFT magnitude in dBFS, linear-spaced over [0, Nyquist]) and
   point_count interleaved left/right pairs of points[] are valid. */
typedef struct {
  uint32_t target_id;
  int64_t render_frame;
  uint64_t seq;
  uint32_t dropped_records;
  uint32_t band_count;
  float bands[SONARE_SCOPE_MAX_BANDS];
  uint32_t point_count;
  float points[SONARE_SCOPE_MAX_POINTS * 2];
} SonareScopeTelemetryRecord;

/* Read-only snapshot of the engine transport state. */
typedef struct {
  int playing;
  int looping;
  int64_t render_frame;
  int64_t sample_position;
  double ppq_position;
  double bpm;
  double loop_start_ppq;
  double loop_end_ppq;
  double sample_rate;
  /* Musical position derived from the tempo map (computed every block).
     Appended after the original fields to preserve struct layout. */
  double bar_start_ppq;               /* PPQ of the current bar's downbeat. */
  int64_t bar_count;                  /* Zero-based index of the current bar. */
  SonareTimeSignature time_signature; /* Time signature in effect at this PPQ. */
  /* Musical beat within the current bar. `beat` is one-based (bar_count above
     is zero-based); `beat_fraction` is the fractional position within the
     current beat, in [0, 1). Appended to preserve the earlier field offsets. */
  int64_t beat;
  double beat_fraction;
} SonareTransportState;

typedef struct {
  uint32_t id;
  char name[64];
  char unit[16];
  float min_value;
  float max_value;
  float default_value;
  int rt_safe;
  int default_curve;
} SonareParameterInfo;

typedef struct {
  double ppq;
  float value;
  /* PPQ-domain curve to the next breakpoint. Used by
     sonare_engine_set_automation_lane. The canonical ordinal mapping is:
       0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
     This matches the sample-accurate mixer curves accepted by
     sonare_strip_schedule_*_automation in sonare_c_mixing.h and the
     AutomationCurve enums in the Node / Python / WASM bindings. The
     mapping is pinned by static_assert against sonare::AutomationCurve
     in src/util/automation_curve.h. */
  int curve_to_next;
} SonareAutomationPoint;

/* Marker kind ordinals. Mirrors sonare::midi::SmfMarkerKind and the binding
   MarkerKind enums; values are part of the ABI and must not be renumbered. */
typedef enum {
  SONARE_MARKER_KIND_MARKER = 0,
  SONARE_MARKER_KIND_TEXT = 1,
  SONARE_MARKER_KIND_LYRIC = 2,
  SONARE_MARKER_KIND_CUE_POINT = 3,
  SONARE_MARKER_KIND_KEY_SIGNATURE = 4
} SonareMarkerKind;

/* The kind / key fields occupy the 4-byte padding hole after `id`, so the
   layout (and ppq / name offsets) is unchanged from when this struct carried
   only id / ppq / name. */
typedef struct {
  uint32_t id;
  uint8_t kind;      /* SonareMarkerKind */
  int8_t key_fifths; /* key signature only: -7..7 (sharps positive) */
  uint8_t key_minor; /* key signature only: 0 major / 1 minor */
  double ppq;
  char name[64];
} SonareEngineMarker;

#ifdef __cplusplus
static_assert(sizeof(SonareEngineMarker) == 80u, "SonareEngineMarker layout drift");
static_assert(offsetof(SonareEngineMarker, ppq) == 8u, "SonareEngineMarker ppq offset");
static_assert(offsetof(SonareEngineMarker, name) == 16u, "SonareEngineMarker name offset");
#endif

typedef struct {
  int enabled;
  float beat_gain;
  float accent_gain;
  /* Explicit click length in samples. 0 means "use the sample-rate-derived
     default" (the engine derives the length from click_seconds and the prepared
     sample rate). Valid range: 0..384000. */
  int click_samples;
  /* Click duration in seconds, used when click_samples is 0 to derive the click
     length from the prepared sample rate. Valid range: 0..1; defaults to 0.002
     (2 ms). */
  double click_seconds;
} SonareEngineMetronomeConfig;

typedef enum {
  SONARE_ENGINE_WARP_MODE_OFF = 0,
  SONARE_ENGINE_WARP_MODE_REPITCH = 1,
  SONARE_ENGINE_WARP_MODE_TEMPO_SYNC = 2,
} SonareEngineWarpMode;

typedef struct {
  double warp_sample;
  double source_sample;
} SonareEngineWarpAnchor;

typedef struct {
  uint32_t clip_id;
  uint32_t channel;
  int64_t sample;
} SonareClipPageRequest;

typedef struct {
  uint32_t id;
  uint32_t track_id;
  const float* const* channels;
  int num_channels;
  int64_t num_samples;
  double start_ppq;
  int64_t clip_offset_samples;
  int64_t length_samples;
  int loop;
  float gain;
  int64_t fade_in_samples;
  int64_t fade_out_samples;
  int warp_mode;
  const SonareEngineWarpAnchor* warp_anchors;
  size_t warp_anchor_count;
  SonareClipPageProvider* page_provider;
} SonareEngineClip;

typedef struct {
  uint32_t bus_id;
  float level_db;
  int enabled;
  /* Pre/post-fader tap point (SonareSendTiming: 0 = post, 1 = pre). Post-fader is
     value 0 so a zero-initialized struct defaults to the historical lane-send
     post-fader behavior; set SONARE_SEND_TIMING_PRE_FADER explicitly for a
     pre-fader send. */
  int send_timing;
} SonareEngineTrackSend;

/* Speaker bed layout for a bus or source. Values match sonare::ChannelLayout
   and are part of the ABI / JSON wire format — never renumber.
   Plane order is WAVE_FORMAT_EXTENSIBLE (also ITU-R BS.2051 / SMPTE):
     5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Ls Rs Lss Rss. */
typedef enum {
  SONARE_CHANNEL_LAYOUT_MONO = 0,
  SONARE_CHANNEL_LAYOUT_STEREO = 1,
  SONARE_CHANNEL_LAYOUT_5_1 = 2,
  SONARE_CHANNEL_LAYOUT_7_1 = 3,
} SonareChannelLayout;

typedef struct {
  uint32_t track_id;
  const SonareEngineTrackSend* sends;
  size_t send_count;
  /* Bus the lane's post-fader output sums into instead of the master mix
     (group/folder routing); 0 keeps the lane on the master mix. */
  uint32_t output_bus_id;
  /* Input channel layout of the source feeding this lane (SonareChannelLayout).
     0 (mono) / 1 (stereo) keep existing behavior; surround upmix is applied by
     the panner once the surround DSP path lands. */
  uint8_t source_channel_layout;
} SonareEngineTrackLane;

/* Per-lane cue/monitor tap. The mode is queued against a lane index and takes
   effect at the requested render frame; the lane's track-id mapping keeps the
   state attached to a track when lane order is republished. */
typedef enum {
  SONARE_ENGINE_TRACK_MONITOR_MODE_OFF = 0,
  SONARE_ENGINE_TRACK_MONITOR_MODE_PFL = 1,
  SONARE_ENGINE_TRACK_MONITOR_MODE_AFL = 2,
  /* Short aliases retained for callers that use the semantic names directly. */
  SONARE_ENGINE_TRACK_MONITOR_OFF = SONARE_ENGINE_TRACK_MONITOR_MODE_OFF,
  SONARE_ENGINE_TRACK_MONITOR_PFL = SONARE_ENGINE_TRACK_MONITOR_MODE_PFL,
  SONARE_ENGINE_TRACK_MONITOR_AFL = SONARE_ENGINE_TRACK_MONITOR_MODE_AFL,
} SonareEngineTrackMonitorMode;

typedef struct {
  uint32_t bus_id;
  float gain_db;
  /* Channel layout of this bus (SonareChannelLayout). The master bus carries the
     project output layout. Defaults to stereo. A 5.1/7.1 bus enables the
     realtime lane mixer's surround scatter and per-plane metering. */
  uint8_t channel_layout;
} SonareEngineBus;

typedef struct {
  float* const* channels;
  int num_channels;
  int64_t capacity_frames;
} SonareEngineCaptureBuffer;

typedef enum {
  SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT = 0,
  SONARE_ENGINE_CAPTURE_SOURCE_INPUT = 1,
} SonareEngineCaptureSource;

typedef struct {
  int64_t captured_frames;
  uint32_t overflow_count;
  int armed;
  int punch_enabled;
  int source;
  int64_t record_offset_samples;
} SonareEngineCaptureStatus;

/* Canonical fallback target loudness used by sonare_engine_bounce_offline when
   normalize_lufs is enabled but target_lufs is left at 0.0f (its zero-init
   sentinel). Matches streaming-platform reference loudness (Spotify/YouTube).
   Bindings (WASM, Node, Python) MUST surface this same default; do not
   hardcode a different value. */
#define SONARE_DEFAULT_BOUNCE_TARGET_LUFS (-14.0f)

typedef struct {
  int64_t total_frames;
  int block_size;
  int num_channels;
  int target_sample_rate;
  int source_sample_rate;
  int normalize_lufs;
  /* Target integrated loudness in LUFS when normalize_lufs != 0. The value
     0.0f is treated as a "use default" sentinel and is normalized to
     SONARE_DEFAULT_BOUNCE_TARGET_LUFS (-14.0 LUFS). Pass a non-zero value
     to override. Call sonare_engine_bounce_options_default() to obtain a
     fully-initialized options struct with documented defaults. */
  float target_lufs;
  int dither; /* 0 = none, 1 = RPDF, 2 = TPDF, 3 = noise-shaped */
  int dither_bits;
  uint32_t dither_seed;
} SonareEngineBounceOptions;

typedef struct {
  float* interleaved; /* heap-allocated; free with sonare_free_bounce_result */
  size_t sample_count;
  int64_t frames;
  int num_channels;
  int sample_rate;
  float integrated_lufs;
} SonareEngineBounceResult;

typedef struct {
  int64_t total_frames;
  int block_size;
  int num_channels;
  uint32_t clip_id;
  double start_ppq;
  float gain;
} SonareEngineFreezeOptions;

typedef struct {
  uint32_t clip_id;
  int64_t frames;
  int num_channels;
} SonareEngineFreezeResult;

typedef struct {
  char id[64];
  int type; /* 0 = pass-through, 1 = gain */
  float gain_db;
  int num_ports;
} SonareEngineGraphNode;

typedef struct {
  char source_node[64];
  int source_port;
  char dest_node[64];
  int dest_port;
  /* Mixing intent (0 = replace, 1 = add). NOTE: not currently honored — the
     compiled graph always sums edges into a shared destination port in an
     order-independent way (the first edge into a port overwrites, every later
     edge adds), regardless of this value. Retained for API compatibility and to
     express intent; multiple edges into one port are always summed. */
  int mix;
} SonareEngineGraphConnection;

typedef struct {
  uint32_t param_id;
  char node_id[64];
} SonareEngineGraphParameterBinding;

typedef struct {
  const SonareEngineGraphNode* nodes;
  size_t node_count;
  const SonareEngineGraphConnection* connections;
  size_t connection_count;
  const SonareEngineGraphParameterBinding* parameter_bindings;
  size_t parameter_binding_count;
  char input_node[64];
  char output_node[64];
  int num_channels;
} SonareEngineGraphSpec;

#ifdef __cplusplus
// Engine POD layout guards. These structs are mirrored by ctypes and are used
// across the C ABI boundary, so layout drift must be caught at compile time.
static_assert(sizeof(SonareEngineTelemetry) == 40u, "SonareEngineTelemetry layout changed");
static_assert(offsetof(SonareEngineTelemetry, error) == 4u,
              "SonareEngineTelemetry error offset changed");
static_assert(sizeof(SonareMeterTelemetryRecord) == 80u,
              "SonareMeterTelemetryRecord layout changed");
static_assert(offsetof(SonareMeterTelemetryRecord, render_frame) == 8u,
              "SonareMeterTelemetryRecord render_frame offset changed");
static_assert(offsetof(SonareMeterTelemetryRecord, peak_db_l) == 24u,
              "SonareMeterTelemetryRecord meter prefix offset changed");
static_assert(offsetof(SonareMeterTelemetryRecord, dropped_records) == 76u,
              "SonareMeterTelemetryRecord dropped_records offset changed");
static_assert(sizeof(SonareMeterTelemetryRecordWide) == 160u,
              "SonareMeterTelemetryRecordWide layout changed");
static_assert(offsetof(SonareMeterTelemetryRecordWide, render_frame) == 8u,
              "SonareMeterTelemetryRecordWide render_frame offset changed");
static_assert(offsetof(SonareMeterTelemetryRecordWide, channel_count) == 24u,
              "SonareMeterTelemetryRecordWide channel_count offset changed");
static_assert(offsetof(SonareMeterTelemetryRecordWide, peak_db) == 28u,
              "SonareMeterTelemetryRecordWide meter prefix offset changed");
static_assert(offsetof(SonareMeterTelemetryRecordWide, dropped_records) == 152u,
              "SonareMeterTelemetryRecordWide dropped_records offset changed");

static_assert(sizeof(SonareTransportState) == 112u, "SonareTransportState layout changed");
static_assert(offsetof(SonareTransportState, render_frame) == 8u,
              "SonareTransportState render_frame offset changed");
static_assert(offsetof(SonareTransportState, ppq_position) == 24u,
              "SonareTransportState ppq_position offset changed");
static_assert(offsetof(SonareTransportState, bar_start_ppq) == 64u,
              "SonareTransportState bar_start_ppq offset changed");
static_assert(offsetof(SonareTransportState, time_signature) == 80u,
              "SonareTransportState time_signature offset changed");
static_assert(offsetof(SonareTransportState, beat) == 96u,
              "SonareTransportState beat offset changed");
static_assert(offsetof(SonareTransportState, beat_fraction) == 104u,
              "SonareTransportState beat_fraction offset changed");

static_assert(sizeof(SonareEngineBounceResult) ==
                  ((offsetof(SonareEngineBounceResult, integrated_lufs) + sizeof(float) +
                    alignof(SonareEngineBounceResult) - 1u) /
                   alignof(SonareEngineBounceResult)) *
                      alignof(SonareEngineBounceResult),
              "SonareEngineBounceResult layout changed");
static_assert(offsetof(SonareEngineBounceResult, sample_count) == sizeof(float*),
              "SonareEngineBounceResult sample_count offset changed");
static_assert(offsetof(SonareEngineBounceResult, frames) == sizeof(float*) + sizeof(size_t),
              "SonareEngineBounceResult frames offset changed");
static_assert(offsetof(SonareEngineBounceResult, integrated_lufs) ==
                  offsetof(SonareEngineBounceResult, sample_rate) + sizeof(int),
              "SonareEngineBounceResult integrated_lufs offset changed");

static_assert(sizeof(SonareEngineFreezeResult) == 24u, "SonareEngineFreezeResult layout changed");
static_assert(offsetof(SonareEngineFreezeResult, frames) == 8u,
              "SonareEngineFreezeResult frames offset changed");
static_assert(offsetof(SonareEngineFreezeResult, num_channels) == 16u,
              "SonareEngineFreezeResult num_channels offset changed");
#endif

#ifdef __cplusplus
}
#endif
