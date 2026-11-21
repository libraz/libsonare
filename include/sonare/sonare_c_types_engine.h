#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Reason code carried by an error telemetry record. Mirrors engine::TelemetryErrorCode. The
/// ordinals are part of the cross-binding telemetry contract: core owns 0..18 and 20..21, while
/// ordinal 19 is reserved by the WASM worklet protocol. SonareEngineTelemetry.error stays an int
/// so this enum does not change the telemetry POD layout.
typedef enum SONARE_ENUM_BASE {
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
  SONARE_ENGINE_TELEMETRY_ERROR_MAX_CHANNELS_EXCEEDED = 20,
  SONARE_ENGINE_TELEMETRY_ERROR_PARAMETER_BASE_OVERFLOW = 21
} SonareEngineTelemetryError;

/// Alias of @ref SonareEngineTelemetryError under the C++ enum's type name.
typedef SonareEngineTelemetryError SonareEngineTelemetryErrorCode;

/// One realtime engine telemetry event, drained with sonare_engine_drain_telemetry.
typedef struct {
  int type;                         ///< Event kind: 0 = processed block, 1 = error.
  int error;                        ///< A @ref SonareEngineTelemetryError; 0 unless type is 1.
  int64_t render_frame;             ///< Monotonic device render frame the event was raised at.
  int64_t timeline_sample;          ///< Engine timeline position in samples at the event.
  int64_t audible_timeline_sample;  ///< Timeline position in samples being heard at the event.
  int32_t
      graph_latency_samples_q8;  ///< Graph latency in samples, fixed point with 8 fraction bits.
  uint32_t value;                ///< Event-specific payload (for example an overflow count).
} SonareEngineTelemetry;

/// Telemetry tap target id, shared by the meter, wide-meter and scope records.
/// The full value range is:
///   0        master mix
///   1..32    track lane, encoded as lane_index + 1
///   33..40   bus, encoded as 33 + bus_index
///   0xFFFF   input-monitor capture tap
/// A host mapping records back to its own lane array must handle 0xFFFF: a naive
/// target_id - 1 index lookup runs past the end of a 32-entry lane array.

/// target_id of the master mix tap.
#define SONARE_TELEMETRY_TARGET_MASTER 0u
/// target_id of the input-monitor capture tap; not a lane or bus index.
#define SONARE_TELEMETRY_TARGET_INPUT_MONITOR 0xFFFFu

/// Mirrors engine::MeterTelemetryRecord: a fixed-size stereo meter snapshot published by the
/// engine's meter tap. Drained with sonare_engine_drain_meter_telemetry. See the target-id range
/// above for how target_id encodes the mix target.
typedef struct {
  uint32_t target_id;    ///< Mix target the snapshot was taken at (see the target-id range).
  int64_t render_frame;  ///< Device render frame the snapshot was published at.
  uint64_t seq;          ///< Publication sequence number.
  float peak_db_l;       ///< Left sample peak of the block in dBFS.
  float peak_db_r;       ///< Right sample peak of the block in dBFS.
  float rms_db_l;        ///< Left block RMS in dBFS.
  float rms_db_r;        ///< Right block RMS in dBFS.
  /// Left inter-sample (true) peak in dBFS, from the ITU-R BS.1770-4 polyphase
  /// reconstruction at 4x. This is a STREAMING measurement that runs one
  /// reconstruction-filter group delay (a few samples) behind the block, so every
  /// value has its whole stencil and the reading does not depend on the block
  /// size. sonare_metering_true_peak_dbtp measures a whole signal at once.
  float true_peak_dbtp_l;
  float true_peak_dbtp_r;    ///< Right inter-sample peak in dBTP; same measurement as the left.
  float max_true_peak_dbtp;  ///< Highest true peak reached so far, in dBTP.
  float correlation;         ///< Left/right phase correlation in [-1, 1].
  float mono_compat_width;   ///< Stereo width relevant to mono compatibility.
  float momentary_lufs;      ///< Momentary (400 ms) loudness in LUFS.
  float short_term_lufs;     ///< Short-term (3 s) loudness in LUFS.
  float integrated_lufs;     ///< Integrated loudness in LUFS since the meter started.
  float gain_reduction_db;   ///< Gain reduction applied by the target's dynamics, in dB.
  uint32_t dropped_records;  ///< Records dropped because the telemetry queue was full.
} SonareMeterTelemetryRecord;

/// Widest per-plane meter the wide telemetry record carries (7.1).
#define SONARE_METER_MAX_CHANNELS 8

/// Per-plane meter snapshot for a surround mix target. Mirrors engine::MeterTelemetryRecord with
/// the per-channel peak/rms/true_peak arrays exposed (planes [0, num_channels)). Drained with
/// sonare_engine_drain_meter_telemetry_wide. The legacy stereo SonareMeterTelemetryRecord stays
/// byte-identical for compatibility; the one drain to use is
/// sonare_engine_drain_meter_telemetry_wide_v2, where a stereo target's L/R are planes 0 and 1.
/// See the target-id range above for how target_id encodes the mix target.
typedef struct {
  uint32_t target_id;    ///< Mix target the snapshot was taken at (see the target-id range).
  int64_t render_frame;  ///< Device render frame the snapshot was published at.
  uint64_t seq;          ///< Publication sequence number.
  int32_t num_channels;  ///< Valid planes in the per-plane arrays, 1..SONARE_METER_MAX_CHANNELS.
  float peak_db[SONARE_METER_MAX_CHANNELS];  ///< Per-plane sample peak of the block in dBFS.
  float rms_db[SONARE_METER_MAX_CHANNELS];   ///< Per-plane block RMS in dBFS.
  /// Per-plane inter-sample (true) peak in dBFS, from the ITU-R BS.1770-4 polyphase
  /// reconstruction at 4x. This is a STREAMING measurement that runs one
  /// reconstruction-filter group delay (a few samples) behind the block, so every
  /// value has its whole stencil and the reading does not depend on the block
  /// size. sonare_metering_true_peak_dbtp measures a whole signal at once.
  float true_peak_dbtp[SONARE_METER_MAX_CHANNELS];
  float max_true_peak_dbtp;  ///< Highest true peak reached so far, in dBTP.
  float correlation;         ///< Phase correlation in [-1, 1].
  float mono_compat_width;   ///< Stereo width relevant to mono compatibility.
  float momentary_lufs;      ///< Momentary (400 ms) loudness in LUFS.
  float short_term_lufs;     ///< Short-term (3 s) loudness in LUFS.
  float integrated_lufs;     ///< Integrated loudness in LUFS since the meter started.
  float gain_reduction_db;   ///< Gain reduction applied by the target's dynamics, in dB.
  uint32_t dropped_records;  ///< Records dropped because the telemetry queue was full.
} SonareMeterTelemetryRecordWide;

/// Stereo meter record with the pre-trim input peaks appended; the legacy 80-byte and 160-byte
/// records above are unchanged. Versioned records are drained from the same queue as the legacy
/// ones; each drain consumes records for all targets. A consumer uses
/// sonare_engine_drain_meter_telemetry_wide_v2 alone and cannot retrieve consumed records again.
typedef struct {
  uint32_t target_id;        ///< Mix target the snapshot was taken at (see the target-id range).
  int64_t render_frame;      ///< Device render frame the snapshot was published at.
  uint64_t seq;              ///< Publication sequence number.
  float peak_db_l;           ///< Left sample peak of the block in dBFS.
  float peak_db_r;           ///< Right sample peak of the block in dBFS.
  float rms_db_l;            ///< Left block RMS in dBFS.
  float rms_db_r;            ///< Right block RMS in dBFS.
  float true_peak_dbtp_l;    ///< Left streaming inter-sample peak in dBTP (see the legacy record).
  float true_peak_dbtp_r;    ///< Right streaming inter-sample peak in dBTP.
  float max_true_peak_dbtp;  ///< Highest true peak reached so far, in dBTP.
  float correlation;         ///< Left/right phase correlation in [-1, 1].
  float mono_compat_width;   ///< Stereo width relevant to mono compatibility.
  float momentary_lufs;      ///< Momentary (400 ms) loudness in LUFS.
  float short_term_lufs;     ///< Short-term (3 s) loudness in LUFS.
  float integrated_lufs;     ///< Integrated loudness in LUFS since the meter started.
  float gain_reduction_db;   ///< Gain reduction applied by the target's dynamics, in dB.
  uint32_t dropped_records;  ///< Records dropped because the telemetry queue was full.
  /// Maximum pre-trim input peak for the left plane, in dBFS.
  float input_peak_db_l;
  /// Maximum pre-trim input peak for the right plane, in dBFS.
  float input_peak_db_r;
} SonareMeterTelemetryRecordV2;

/// Per-plane meter record with the pre-trim input peaks appended; the one record the
/// sonare_engine_drain_meter_telemetry_wide_v2 drain returns. A stereo target's L/R are planes 0
/// and 1.
typedef struct {
  uint32_t target_id;    ///< Mix target the snapshot was taken at (see the target-id range).
  int64_t render_frame;  ///< Device render frame the snapshot was published at.
  uint64_t seq;          ///< Publication sequence number.
  int32_t num_channels;  ///< Valid planes in the per-plane arrays, 1..SONARE_METER_MAX_CHANNELS.
  float peak_db[SONARE_METER_MAX_CHANNELS];  ///< Per-plane sample peak of the block in dBFS.
  float rms_db[SONARE_METER_MAX_CHANNELS];   ///< Per-plane block RMS in dBFS.
  /// Per-plane streaming inter-sample (true) peak in dBTP (see the legacy record).
  float true_peak_dbtp[SONARE_METER_MAX_CHANNELS];
  float max_true_peak_dbtp;  ///< Highest true peak reached so far, in dBTP.
  float correlation;         ///< Phase correlation in [-1, 1].
  float mono_compat_width;   ///< Stereo width relevant to mono compatibility.
  float momentary_lufs;      ///< Momentary (400 ms) loudness in LUFS.
  float short_term_lufs;     ///< Short-term (3 s) loudness in LUFS.
  float integrated_lufs;     ///< Integrated loudness in LUFS since the meter started.
  float gain_reduction_db;   ///< Gain reduction applied by the target's dynamics, in dB.
  uint32_t dropped_records;  ///< Records dropped because the telemetry queue was full.
  /// Maximum pre-trim input peak for each valid plane, in dBFS.
  float input_peak_db[SONARE_METER_MAX_CHANNELS];
} SonareMeterTelemetryRecordWideV2;

/// One lowered MIDI 1.0 message drained from the engine's external-MIDI output queue
/// (sonare_engine_drain_external_midi). A single queued channel-voice event may lower to more than
/// one message (e.g. a MIDI 2.0 program change with bank select, or a registered controller as its
/// four RPN messages).
typedef struct {
  /// Track MIDI destination id, or 0xFFFFFFFF for a transport/clock byte.
  uint32_t destination_id;
  uint32_t byte_count;  ///< Number of valid entries in bytes, 1..3.
  /// Monotonic DEVICE render frame for every record (sequenced channel-voice events, live input,
  /// and clock/transport bytes alike), so drained events stay in dispatch order across a loop
  /// wrap or seek.
  int64_t render_frame;
  uint8_t bytes[3];     ///< MIDI 1.0 message bytes (status first); only byte_count are valid.
  uint8_t reserved[5];  ///< Padding; always zero.
} SonareExternalMidiEvent;

/// Capacity of SonareScopeTelemetryRecord.bands.
#define SONARE_SCOPE_MAX_BANDS 64
/// Capacity of SonareScopeTelemetryRecord.points in left/right pairs.
#define SONARE_SCOPE_MAX_POINTS 32

/// Mirrors engine::ScopeTelemetryRecord: a fixed-size spectrum + goniometer (vectorscope)
/// snapshot for one mix target, published by the engine's scope tap and drained with
/// sonare_engine_drain_scope_telemetry. See the target-id range above for how target_id encodes
/// the mix target.
typedef struct {
  uint32_t target_id;        ///< Mix target the snapshot was taken at (see the target-id range).
  int64_t render_frame;      ///< Device render frame the snapshot was published at.
  uint64_t seq;              ///< Publication sequence number.
  uint32_t dropped_records;  ///< Records dropped because the telemetry queue was full.
  uint32_t band_count;       ///< Valid entries in bands, at most SONARE_SCOPE_MAX_BANDS.
  /// FFT magnitude in dBFS, linear-spaced over [0, Nyquist]; band_count entries are valid.
  float bands[SONARE_SCOPE_MAX_BANDS];
  uint32_t point_count;  ///< Valid left/right pairs in points, at most SONARE_SCOPE_MAX_POINTS.
  /// Interleaved left/right goniometer samples; point_count pairs are valid.
  float points[SONARE_SCOPE_MAX_POINTS * 2];
} SonareScopeTelemetryRecord;

/// Read-only snapshot of the engine transport state.
typedef struct {
  int playing;              ///< Non-zero while the transport is playing.
  int looping;              ///< Non-zero while loop playback is enabled.
  int64_t render_frame;     ///< Monotonic device render frame of the snapshot.
  int64_t sample_position;  ///< Timeline position in samples.
  double ppq_position;      ///< Timeline position in quarter notes (PPQ).
  double bpm;               ///< Tempo in beats per minute at the current position.
  double loop_start_ppq;    ///< Loop region start in PPQ.
  double loop_end_ppq;      ///< Loop region end in PPQ.
  double sample_rate;       ///< Prepared engine sample rate in Hz.
  /// PPQ of the current bar's downbeat. Musical position fields from here on are derived from the
  /// tempo map every block.
  double bar_start_ppq;
  int64_t bar_count;                   ///< Zero-based index of the current bar.
  SonareTimeSignature time_signature;  ///< Time signature in effect at this PPQ.
  /// Beat within the current bar, one-based (bar_count is zero-based).
  int64_t beat;
  double beat_fraction;  ///< Fractional position within the current beat, in [0, 1).
} SonareTransportState;

/// Description of one automatable engine parameter.
typedef struct {
  uint32_t id;          ///< Parameter id used to address it in engine commands.
  char name[64];        ///< NUL-terminated display name; truncated to fit.
  char unit[16];        ///< NUL-terminated unit label (for example "dB"); may be empty.
  float min_value;      ///< Smallest accepted value, in the parameter's unit.
  float max_value;      ///< Largest accepted value, in the parameter's unit.
  float default_value;  ///< Value the parameter holds before any change.
  int rt_safe;          ///< Non-zero when it can be changed from the audio thread.
  int default_curve;    ///< Interpolation ordinal as in SonareAutomationPoint.curve_to_next.
} SonareParameterInfo;

/// One breakpoint of an automation lane (sonare_engine_set_automation_lane).
typedef struct {
  double ppq;   ///< Breakpoint position in quarter notes (PPQ).
  float value;  ///< Parameter value at the breakpoint, in the parameter's unit.
  /// PPQ-domain curve to the next breakpoint. Used by
  /// sonare_engine_set_automation_lane. The canonical ordinal mapping is:
  ///   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
  /// This matches the sample-accurate mixer curves accepted by
  /// sonare_strip_schedule_*_automation in sonare_c_mixing.h and the
  /// AutomationCurve enums in the Node / Python / WASM bindings. The
  /// mapping is pinned by static_assert against sonare::AutomationCurve
  /// in src/util/automation_curve.h.
  int curve_to_next;
} SonareAutomationPoint;

/// Marker kind ordinals. Mirrors sonare::midi::SmfMarkerKind and the binding MarkerKind enums;
/// values are part of the ABI and must not be renumbered.
typedef enum SONARE_ENUM_BASE {
  SONARE_MARKER_KIND_MARKER = 0,        ///< Generic marker.
  SONARE_MARKER_KIND_TEXT = 1,          ///< Free text event.
  SONARE_MARKER_KIND_LYRIC = 2,         ///< Lyric syllable.
  SONARE_MARKER_KIND_CUE_POINT = 3,     ///< Cue point.
  SONARE_MARKER_KIND_KEY_SIGNATURE = 4  ///< Key signature; uses key_fifths and key_minor.
} SonareMarkerKind;

/// One timeline marker. The kind / key fields occupy the 4-byte padding hole after `id`, so the
/// layout (and ppq / name offsets) is unchanged from when this struct carried only id / ppq / name.
typedef struct {
  uint32_t id;        ///< Marker id.
  uint8_t kind;       ///< A @ref SonareMarkerKind selecting the marker's meaning.
  int8_t key_fifths;  ///< Key signature only: -7..7 (sharps positive).
  uint8_t key_minor;  ///< Key signature only: 0 major, 1 minor.
  double ppq;         ///< Marker position in quarter notes (PPQ).
  char name[64];      ///< NUL-terminated marker text; truncated to fit.
} SonareEngineMarker;

#ifdef __cplusplus
static_assert(sizeof(SonareEngineMarker) == 80u, "SonareEngineMarker layout drift");
static_assert(offsetof(SonareEngineMarker, ppq) == 8u, "SonareEngineMarker ppq offset");
static_assert(offsetof(SonareEngineMarker, name) == 16u, "SonareEngineMarker name offset");
#endif

/// The click length taken from @c click_sec and the prepared sample rate. Outside the click
/// length's real domain (a positive sample count), so it never reads as a length.
#define SONARE_ENGINE_METRONOME_CLICK_FROM_SEC (-1)

/// Metronome click settings. Start from @ref sonare_engine_metronome_config_init; every field is
/// then read literally and checked against the range given beside it.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareEngineMetronomeConfig); set by the init function.
  int enabled;           ///< Non-zero turns the click on. Default 0.
  float beat_gain;       ///< Linear gain of an ordinary beat click; finite, >= 0. Default 0.35.
  float accent_gain;     ///< Linear gain of the downbeat click; finite, >= 0. Default 0.7.
  /// Explicit click length in samples, in [1, 384000]. Default
  /// @ref SONARE_ENGINE_METRONOME_CLICK_FROM_SEC, which derives it from @c click_sec.
  int click_samples;
  /// Click duration in seconds, in (0, 1], used when @c click_samples is
  /// @ref SONARE_ENGINE_METRONOME_CLICK_FROM_SEC. Default 0.002 (2 ms).
  double click_sec;
} SonareEngineMetronomeConfig;

/// How a clip's source is mapped onto the timeline.
typedef enum SONARE_ENUM_BASE {
  SONARE_ENGINE_WARP_MODE_OFF = 0,  ///< No warp; the source plays at its own rate.
  SONARE_ENGINE_WARP_MODE_REPITCH =
      1,  ///< Anchor-mapped playback by resampling; pitch follows rate.
  SONARE_ENGINE_WARP_MODE_TEMPO_SYNC =
      2,  ///< Source baked to the tempo map; no paged source, no loop.
  /// Realtime pitch-preserving warp. Follows the same anchor map as REPITCH but
  /// synthesizes the output with an overlap-add stretcher, so a rate change
  /// moves the timing without moving the pitch. No control-thread bake, so a new
  /// anchor set takes effect from the next block.
  SONARE_ENGINE_WARP_MODE_TIME_STRETCH = 3,
} SonareEngineWarpMode;

/// One warp anchor mapping a position on the clip's warped timeline to a source position.
typedef struct {
  double warp_sample;    ///< Position on the warped clip timeline, in samples (finite, >= 0).
  double source_sample;  ///< Matching position in the source, in samples (finite, >= 0).
} SonareEngineWarpAnchor;

/// A paged-clip page that the audio thread needed and did not have, reported through the clip
/// page miss queue.
typedef struct {
  uint32_t clip_id;  ///< Id of the clip whose page is missing.
  uint32_t channel;  ///< Source channel index of the page.
  int64_t sample;    ///< Source sample position the missing page covers.
} SonareClipPageRequest;

/// The clip plays to the end of its source: num_samples - clip_offset_samples. Outside the
/// length's real domain (a positive frame count), so it never reads as a length.
#define SONARE_ENGINE_CLIP_LENGTH_TO_END (-1)

/// One clip placed on a track's timeline. Start each element from @ref sonare_engine_clip_init.
/// Unless page_provider is set, the engine copies the source audio during the call, so the
/// buffers need not outlive it.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareEngineClip); set by the init function.
  uint32_t id;           ///< Clip id, unique among the published clips.
  uint32_t track_id;     ///< Id of the track lane the clip plays on.
  /// num_channels pointers to num_samples floats each; ignored when page_provider is set.
  const float* const* channels;
  int num_channels;             ///< Source channel count; ignored when page_provider is set.
  int64_t num_samples;          ///< Source length in frames; ignored when page_provider is set.
  double start_ppq;             ///< Timeline position of the clip start in quarter notes (PPQ).
  int64_t clip_offset_samples;  ///< Start offset into the source in frames, 0 <= offset < length.
  /// Playback length in frames; positive. Default @ref SONARE_ENGINE_CLIP_LENGTH_TO_END.
  int64_t length_samples;
  int loop;                  ///< Non-zero repeats the clip over its length.
  float gain;                ///< Linear clip gain, finite and >= 0. Default 1.
  int64_t fade_in_samples;   ///< Fade-in length in frames; 0 for none.
  int64_t fade_out_samples;  ///< Fade-out length in frames; 0 for none.
  int warp_mode;             ///< A @ref SonareEngineWarpMode selecting the warp.
  /// Anchors in strictly increasing warp_sample and source_sample order; may be NULL when
  /// warp_anchor_count is 0.
  const SonareEngineWarpAnchor* warp_anchors;
  size_t warp_anchor_count;  ///< Number of entries in warp_anchors.
  /// Page-fed source created with sonare_clip_page_provider_create, which then supplies the
  /// channels and length; NULL for an in-memory clip.
  SonareClipPageProvider* page_provider;
} SonareEngineClip;

/// One send from a track lane or bus to another bus.
typedef struct {
  uint32_t bus_id;  ///< Id of the destination bus.
  float level_db;   ///< Send level in dB.
  int enabled;      ///< Non-zero when the send is active.
  /// A @ref SonareSendTiming selecting the tap point: 0 = post-fader, 1 = pre-fader. Post-fader
  /// is value 0 so a zero-initialized struct defaults to the historical lane-send post-fader
  /// behavior; set SONARE_SEND_TIMING_PRE_FADER explicitly for a pre-fader send.
  int send_timing;
} SonareEngineTrackSend;

/// Speaker bed layout for a bus or source. Values match sonare::ChannelLayout and are part of the
/// ABI / JSON wire format - never renumber. Plane order is WAVE_FORMAT_EXTENSIBLE (also ITU-R
/// BS.2051 / SMPTE): 5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Ls Rs Lss Rss.
typedef enum SONARE_ENUM_BASE {
  SONARE_CHANNEL_LAYOUT_MONO = 0,    ///< One channel.
  SONARE_CHANNEL_LAYOUT_STEREO = 1,  ///< Two channels, L R.
  SONARE_CHANNEL_LAYOUT_5_1 = 2,     ///< Six channels.
  SONARE_CHANNEL_LAYOUT_7_1 = 3,     ///< Eight channels.
} SonareChannelLayout;

/// Routing of one track lane, published with sonare_engine_set_track_lanes.
typedef struct {
  uint32_t track_id;                   ///< Id of the track the lane belongs to.
  const SonareEngineTrackSend* sends;  ///< send_count sends to buses; may be NULL when 0.
  size_t send_count;                   ///< Number of entries in sends.
  /// Bus the lane's post-fader output sums into instead of the master mix
  /// (group/folder routing); 0 keeps the lane on the master mix.
  uint32_t output_bus_id;
  /// A @ref SonareChannelLayout giving the input channel layout of the source feeding this lane.
  /// Only SONARE_CHANNEL_LAYOUT_STEREO is accepted; any other value makes
  /// sonare_engine_set_track_lanes fail with SONARE_ERROR_NOT_SUPPORTED.
  uint8_t source_channel_layout;
} SonareEngineTrackLane;

/// Per-lane cue/monitor tap. The mode is queued against a lane index and takes effect at the
/// requested render frame; the lane's track-id mapping keeps the state attached to a track when
/// lane order is republished.
typedef enum SONARE_ENUM_BASE {
  SONARE_ENGINE_TRACK_MONITOR_MODE_OFF = 0,  ///< Lane not routed to the monitor.
  SONARE_ENGINE_TRACK_MONITOR_MODE_PFL = 1,  ///< Pre-fader listen.
  SONARE_ENGINE_TRACK_MONITOR_MODE_AFL = 2,  ///< After-fader listen.
  /// Short aliases retained for callers that use the semantic names directly.
  SONARE_ENGINE_TRACK_MONITOR_OFF = SONARE_ENGINE_TRACK_MONITOR_MODE_OFF,
  SONARE_ENGINE_TRACK_MONITOR_PFL = SONARE_ENGINE_TRACK_MONITOR_MODE_PFL,
  SONARE_ENGINE_TRACK_MONITOR_AFL = SONARE_ENGINE_TRACK_MONITOR_MODE_AFL,
} SonareEngineTrackMonitorMode;

/// Source a bus or master insert's sidechain key is taken from. Values are part of the ABI -
/// never renumber.
typedef enum SONARE_ENUM_BASE {
  /// A track lane's post-strip signal, before its lane fader.
  SONARE_SIDECHAIN_SOURCE_TRACK = 0,
  /// A bus's processed signal, before its gain_db; folded to stereo when wider.
  SONARE_SIDECHAIN_SOURCE_BUS = 1,
} SonareSidechainSourceKind;

/// Why a sidechain binding would be refused, as reported by the sonare_engine_can_set_*_sidechain
/// queries. Values are part of the ABI - never renumber. When several reasons hold, the first
/// check the matching setter runs is reported.
typedef enum SONARE_ENUM_BASE {
  /// The binding would be accepted.
  SONARE_SIDECHAIN_REFUSAL_NONE = 0,
  /// Track id 0, or a bus that is not configured.
  SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET = 1,
  /// The insert index is past the target's insert chain.
  SONARE_SIDECHAIN_REFUSAL_INSERT_OUT_OF_RANGE = 2,
  /// The source track or bus is not declared.
  SONARE_SIDECHAIN_REFUSAL_UNDECLARED_SOURCE = 3,
  /// source_kind is not a SonareSidechainSourceKind.
  SONARE_SIDECHAIN_REFUSAL_INVALID_SOURCE_KIND = 4,
  /// The key source is the target itself.
  SONARE_SIDECHAIN_REFUSAL_SELF_KEY = 5,
  /// The binding would close a routing cycle.
  SONARE_SIDECHAIN_REFUSAL_CYCLE = 6,
  /// The sidechain binding table has no free entry.
  SONARE_SIDECHAIN_REFUSAL_TABLE_FULL = 7,
  /// The latency the key would add to the mix is past the alignment ceiling of 192000 samples;
  /// the setter fails with SONARE_ERROR_INVALID_STATE.
  SONARE_SIDECHAIN_REFUSAL_PLAN_REFUSED = 8,
} SonareSidechainRefusal;

/// Configuration of one mix bus, published with the engine's bus setters.
typedef struct {
  uint32_t bus_id;  ///< Bus id; 0 is not a bus and refers to the master mix.
  /// The bus fader in dB. Pre-fader sends and bus-sourced sidechain keys tap before it.
  float gain_db;
  /// A @ref SonareChannelLayout giving the layout of this bus. Defaults to stereo. A
  /// 5.1/7.1 bus enables the realtime lane mixer's surround scatter and per-plane
  /// metering. A destination narrower than the bus (another bus, or the master at
  /// the rendered channel count) receives an ITU-R BS.775 downmix; a wider one
  /// receives the bus planes on the same plane indices.
  uint8_t channel_layout;
  /// Bus the output sums into instead of the master mix (bus-to-bus routing);
  /// 0 keeps the bus on the master mix.
  uint32_t output_bus_id;
  /// Sends to other buses, in the same shape as a track lane's sends. A cycle
  /// through outputs, sends or bus-sourced sidechain keys is rejected.
  const SonareEngineTrackSend* sends;
  size_t send_count;  ///< Number of entries in sends.
} SonareEngineBus;

/// Caller-owned buffer the engine records captured audio into
/// (sonare_engine_set_capture_buffer).
typedef struct {
  float* const* channels;   ///< num_channels writable planes of capacity_frames floats each.
  int num_channels;         ///< Plane count; must be positive.
  int64_t capacity_frames;  ///< Capacity of each plane in frames; must be positive.
} SonareEngineCaptureBuffer;

/// Signal the capture records.
typedef enum SONARE_ENUM_BASE {
  SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT = 0,  ///< The engine's rendered output.
  SONARE_ENGINE_CAPTURE_SOURCE_INPUT = 1,   ///< The live input signal.
} SonareEngineCaptureSource;

/// Capture state returned by sonare_engine_capture_status.
typedef struct {
  int64_t captured_frames;        ///< Frames recorded into the capture buffer so far.
  uint32_t overflow_count;        ///< Times the capture buffer filled and dropped audio.
  int armed;                      ///< Non-zero while capture is armed.
  int punch_enabled;              ///< Non-zero while the punch-in/out window is enabled.
  int source;                     ///< A @ref SonareEngineCaptureSource naming the recorded signal.
  int64_t record_offset_samples;  ///< Capture window shift on the timeline, in samples.
} SonareEngineCaptureStatus;

/// Target loudness sonare_engine_bounce_offline normalizes to when normalize_lufs is set and
/// target_lufs is NaN (not given). Bindings (WASM, Node, Python) MUST surface this same default.
#define SONARE_DEFAULT_BOUNCE_TARGET_LUFS (-14.0f)

/// The default offline block: 128 frames, capped at the prepared maximum block size. Outside the
/// block size's real domain (a positive frame count), so it never reads as a size.
#define SONARE_ENGINE_BLOCK_SIZE_AUTO (-1)

/// The engine's prepared sample rate. Outside the rate's real domain, so it never reads as a rate.
#define SONARE_ENGINE_SAMPLE_RATE_PREPARED (-1)

/// The dither's built-in noise seed, which @ref sonare_engine_bounce_options_init writes.
#define SONARE_ENGINE_DEFAULT_DITHER_SEED 0x51A7E5u

/// Options for sonare_engine_bounce_offline. Start from @ref sonare_engine_bounce_options_init;
/// every field is then read literally and checked against the range given beside it.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareEngineBounceOptions); set by the init function.
  int64_t total_frames;  ///< Frames to render, counted at source_sample_rate; positive. Default 0.
  /// Offline block in frames, in [1, prepared maximum block size]. Default
  /// @ref SONARE_ENGINE_BLOCK_SIZE_AUTO.
  int block_size;
  /// Required, positive; mono, stereo, 5.1 or 7.1. Left at 0 by the init function, so the caller
  /// must set it.
  int num_channels;
  /// Output sample rate; positive, and anything other than the source rate resamples to it.
  /// Default @ref SONARE_ENGINE_SAMPLE_RATE_PREPARED, which keeps the source rate. The result
  /// reports the rate actually returned.
  int target_sample_rate;
  /// Rate total_frames is counted in. It must equal the prepared rate, because the render always
  /// runs there. Default @ref SONARE_ENGINE_SAMPLE_RATE_PREPARED.
  int source_sample_rate;
  int normalize_lufs;  ///< Non-zero normalizes the result to target_lufs. Default 0.
  /// Target integrated loudness in LUFS when normalize_lufs != 0; finite. Default NaN (not given),
  /// which normalizes to @ref SONARE_DEFAULT_BOUNCE_TARGET_LUFS.
  float target_lufs;
  int dither;  ///< Dither type: 0 = none, 1 = RPDF, 2 = TPDF, 3 = noise-shaped. Default 0.
  /// Word length in bits the dither targets, in [2, 32], read when dither is not 0. Default 16.
  int dither_bits;
  /// Dither noise seed, any value. Default @ref SONARE_ENGINE_DEFAULT_DITHER_SEED.
  uint32_t dither_seed;
} SonareEngineBounceOptions;

/// Result of sonare_engine_bounce_offline. Release with sonare_free_engine_bounce_result.
typedef struct {
  float* interleaved;     ///< Heap-allocated interleaved samples; free with
                          ///< sonare_free_engine_bounce_result.
  size_t sample_count;    ///< Total floats in interleaved (frames * num_channels).
  int64_t frames;         ///< Frames per channel.
  int num_channels;       ///< Interleaved channel count.
  int sample_rate;        ///< Sample rate of the returned audio in Hz.
  float integrated_lufs;  ///< Integrated loudness of the returned audio in LUFS.
} SonareEngineBounceResult;

/// Options for sonare_engine_freeze_offline, which renders the engine's output into a new clip.
/// Start from @ref sonare_engine_freeze_options_init.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareEngineFreezeOptions); set by the init function.
  int64_t total_frames;  ///< Frames to render; positive. Default 0, so the caller must set it.
  /// Offline block in frames, in [1, prepared maximum block size]. Default
  /// @ref SONARE_ENGINE_BLOCK_SIZE_AUTO.
  int block_size;
  /// Required, positive; at most the channel count the engine was prepared for. Default 0.
  int num_channels;
  uint32_t clip_id;  ///< Id given to the frozen clip. Default 0, so the caller must set it.
  double start_ppq;  ///< Timeline position of the frozen clip in PPQ; finite, >= 0. Default 0.
  float gain;        ///< Linear gain of the frozen clip; finite, >= 0. Default 1.
} SonareEngineFreezeOptions;

/// Result of sonare_engine_freeze_offline.
typedef struct {
  uint32_t clip_id;  ///< Id of the clip that was created.
  int64_t frames;    ///< Frames rendered into the clip.
  int num_channels;  ///< Channel count of the clip.
} SonareEngineFreezeResult;

/// The node has as many ports as the spec has channels. Outside the port count's real domain (a
/// positive count), so it never reads as a count.
#define SONARE_ENGINE_GRAPH_PORTS_FROM_SPEC (-1)

/// One node of an engine processing graph. Start each element from
/// @ref sonare_engine_graph_node_init.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareEngineGraphNode); set by the init function.
  char id[64];           ///< NUL-terminated node name, referenced by connections and bindings.
  int type;              ///< Node kind: 0 = pass-through (default), 1 = gain.
  float gain_db;         ///< Gain in dB applied by a gain node (type 1). Default 0.
  /// Port count for this node; positive. Default @ref SONARE_ENGINE_GRAPH_PORTS_FROM_SPEC.
  int num_ports;
} SonareEngineGraphNode;

/// One edge of an engine processing graph.
typedef struct {
  char source_node[64];  ///< Id of the node the edge starts at.
  int source_port;       ///< Output port index on the source node.
  char dest_node[64];    ///< Id of the node the edge ends at.
  int dest_port;         ///< Input port index on the destination node.
  /// How this edge meets the others into the same destination port, taken in
  /// connection order: 0 = replace (the port takes this edge's signal, dropping
  /// what earlier edges put there), 1 = add (accumulates onto it). A port's first
  /// edge initializes it either way, so the two differ only on a shared port.
  /// A zero-initialized connection is replace, unlike the language bindings,
  /// which default an omitted mix to add: set 1 on every edge that should sum.
  /// Any other value is refused with SONARE_ERROR_INVALID_PARAMETER.
  int mix;
} SonareEngineGraphConnection;

/// Binds an engine parameter id to a graph node so the parameter drives that node.
typedef struct {
  uint32_t param_id;  ///< Engine parameter id being bound.
  char node_id[64];   ///< Id of the graph node the parameter drives.
} SonareEngineGraphParameterBinding;

/// An engine processing graph, published with sonare_engine_set_graph. The arrays are read during
/// the call and need not outlive it.
typedef struct {
  const SonareEngineGraphNode* nodes;  ///< node_count nodes; must be non-NULL and non-empty.
  size_t node_count;                   ///< Number of entries in nodes.
  const SonareEngineGraphConnection* connections;  ///< connection_count edges; NULL when 0.
  size_t connection_count;                         ///< Number of entries in connections.
  const SonareEngineGraphParameterBinding* parameter_bindings;  ///< NULL when the count is 0.
  size_t parameter_binding_count;  ///< Number of entries in parameter_bindings.
  char input_node[64];             ///< Id of the node that receives the engine input.
  char output_node[64];            ///< Id of the node whose output is the graph result.
  int num_channels;                ///< Graph channel count; must be positive.
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
static_assert(offsetof(SonareMeterTelemetryRecordWide, num_channels) == 24u,
              "SonareMeterTelemetryRecordWide num_channels offset changed");
static_assert(offsetof(SonareMeterTelemetryRecordWide, peak_db) == 28u,
              "SonareMeterTelemetryRecordWide meter prefix offset changed");
static_assert(offsetof(SonareMeterTelemetryRecordWide, dropped_records) == 152u,
              "SonareMeterTelemetryRecordWide dropped_records offset changed");
static_assert(sizeof(SonareMeterTelemetryRecordV2) == 88u,
              "SonareMeterTelemetryRecordV2 layout changed");
static_assert(offsetof(SonareMeterTelemetryRecordV2, input_peak_db_l) == 80u,
              "SonareMeterTelemetryRecordV2 input peaks must be appended");
static_assert(sizeof(SonareMeterTelemetryRecordWideV2) == 192u,
              "SonareMeterTelemetryRecordWideV2 layout changed");
static_assert(offsetof(SonareMeterTelemetryRecordWideV2, input_peak_db) == 156u,
              "SonareMeterTelemetryRecordWideV2 input peaks must be appended");

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
