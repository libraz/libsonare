#pragma once

#include <stddef.h>
#include <stdint.h>

// Pulled in for SonareMasteringParam, which the mixing assistant reuses as its
// flat key/value configuration carrier. The struct is a generic
// {const char* key; double value;} pair with nothing mastering-specific in it,
// and duplicating an identical type would mean a second mirror to keep in step
// across four surfaces for no gain.
#include "sonare_c_mastering.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Mixing
// ============================================================================

/// How a strip's pan position is applied to a stereo pair. Balance keeps the
/// stereo image and is unity at center under every pan law. StereoPan folds the
/// pair to mono and takes the law's center attenuation (-3.01 / -4.52 / -6.02 /
/// 0 dB). DualPan pans the left and right inputs independently (defaults -1 and
/// +1, a pass-through) with the same center attenuation.
typedef enum SONARE_ENUM_BASE {
  SONARE_PAN_MODE_BALANCE = 0,     ///< Balance: keeps the stereo image
  SONARE_PAN_MODE_STEREO_PAN = 1,  ///< StereoPan: folds to mono and pans it
  SONARE_PAN_MODE_DUAL_PAN = 2     ///< DualPan: pans left and right independently
} SonarePanMode;

/// Sentinel for sonare_strip_set_pan's pan_mode argument: keep the strip's
/// current pan mode and only move the pan position (see sonare_strip_set_pan).
#define SONARE_PAN_MODE_KEEP (-1)

/// Far-channel taper of a strip's pan law; see @ref sonare_strip_set_pan_law.
typedef enum SONARE_ENUM_BASE {
  SONARE_PAN_LAW_CONST_3DB = 0,    ///< Constant power, -3 dB at center
  SONARE_PAN_LAW_CONST_4P5DB = 1,  ///< -4.5 dB at center
  SONARE_PAN_LAW_CONST_6DB = 2,    ///< -6 dB at center
  SONARE_PAN_LAW_LINEAR_0DB = 3    ///< Linear, 0 dB at center
} SonarePanLaw;

/// Where a send taps its strip: before or after the fader.
typedef enum SONARE_ENUM_BASE {
  /* Post-fader is value 0 so a zero-initialized SonareEngineTrackSend defaults to
     the historical lane-send behavior (post-fader) instead of silently flipping
     to pre-fader. The integer is an in-memory ABI detail only -- scene/project
     JSON serializes send timing as the strings "pre"/"post", never this enum. */
  SONARE_SEND_TIMING_POST_FADER = 0,  ///< Send taps the signal after the fader
  SONARE_SEND_TIMING_PRE_FADER = 1    ///< Send taps the signal before the fader
} SonareSendTiming;

/// Point in a strip at which a meter snapshot is read.
typedef enum SONARE_ENUM_BASE {
  SONARE_METER_TAP_PRE_FADER = 0,  ///< Before the fader
  SONARE_METER_TAP_POST_FADER = 1  ///< After the fader
} SonareMeterTap;

/// Maximum number of surround planes a meter snapshot can report (7.1).
#define SONARE_MAX_METER_CHANNELS 8

/// One reading of a strip, bus or master meter, describing the most recently metered block. Level
/// fields sit at -120 dB when nothing has been measured or the measurement is switched off.
typedef struct {
  float peak_db_l;    ///< Left sample peak in dBFS
  float peak_db_r;    ///< Right sample peak in dBFS
  float rms_db_l;     ///< Left RMS in dBFS
  float rms_db_r;     ///< Right RMS in dBFS
  float correlation;  ///< Left/right correlation, -1 (anti-phase) to 1 (identical); 0 if silent
  float mono_compat_width;     ///< RMS ratio of side to mid; 0 is mono, about 1 decorrelated
  float mono_compat_peak;      ///< Peak of the mono sum 0.5 * (L + R), linear
  float mono_compat_side_rms;  ///< RMS of the side signal 0.5 * (L - R), linear
  int likely_mono_compatible;  ///< Non-zero when the correlation is not below the meter's threshold
  float momentary_lufs;        ///< Momentary (400 ms) loudness in LUFS
  float short_term_lufs;       ///< Short-term (3 s) loudness in LUFS
  float integrated_lufs;       ///< Gated integrated loudness in LUFS since the meter was reset
  float gain_reduction_db;  ///< Gain reduction in dB the strip's dynamics last reported; 0 if none
  /* Inter-sample (true) peak in dBFS, from the ITU-R BS.1770-4 polyphase
     reconstruction at the strip's configured oversample factor (4 or 8). This is a STREAMING
     measurement that runs one reconstruction-filter group delay (a few samples) behind the block,
     so every value has its whole stencil and the reading does not depend on the block size.
     sonare_metering_true_peak_dbtp measures a whole signal at once. */
  float true_peak_dbtp_l;    ///< Left true peak in dBTP; see sonare_metering_true_peak_dbtp
  float true_peak_dbtp_r;    ///< Right true peak in dBTP
  float max_true_peak_dbtp;  ///< Largest true peak over every plane in dBTP
  uint64_t seq;              ///< Counter of published snapshots, advancing once per metered block
  /* Per-plane surround meters (5.1/7.1). Indices [0, num_channels) carry valid
     values; index 0 and 1 mirror the _l and _r stereo fields above.
     num_channels is 0 for a snapshot taken before the meter has seen audio,
     else 1..8. */
  float peak_db[SONARE_MAX_METER_CHANNELS];         ///< Per-plane sample peak in dBFS
  float rms_db[SONARE_MAX_METER_CHANNELS];          ///< Per-plane RMS in dBFS
  float true_peak_dbtp[SONARE_MAX_METER_CHANNELS];  ///< Per-plane true peak in dBTP
  int num_channels;  ///< Number of planes carrying valid per-plane values, 0..8
} SonareMixMeterSnapshot;

/// One goniometer (vectorscope) sample of a strip.
typedef struct {
  float left;   ///< Left sample value, linear
  float right;  ///< Right sample value, linear
} SonareMixGoniometerPoint;

/// Surround pan position for a strip feeding a >2-channel bus: azimuth (-180..180 deg,
/// 0 = front-center, + = right), divergence (0 = point source, 1 = spread across the
/// front) and lfe (0..1 scalar send into the LFE plane). Applied when
/// the engine's track mixer feeds the strip's lane into a destination with more
/// than two channels; ignored by sonare_mixer_process_stereo and
/// sonare_mixer_drain_tail_stereo, which are stereo-only.
typedef struct {
  float azimuth;     ///< Angle in degrees, -180..180; 0 is front-center, positive is right
  float divergence;  ///< 0 is a point source, 1 spreads across the front; range 0..1
  float lfe;         ///< Scalar send into the LFE plane, 0..1
} SonareSurroundPan;

/// Creates a mixer. sample_rate must be in [8000, 384000] and max_block_size in
/// [1, 1048576], or 0 for the library default (512 frames); a negative max_block_size is refused.
/// @param out Receives the handle on success, NULL on failure; destroy with
///        sonare_mixer_destroy, which must be called exactly once.
/// @return SONARE_OK, SONARE_ERROR_INVALID_PARAMETER on an invalid argument (or NULL @p out),
///         or another error code on an allocation failure.
SonareError sonare_mixer_create(int sample_rate, int max_block_size, SonareMixer** out);
/// @brief Options for @ref sonare_mixer_add_strip. Start from
///        @ref sonare_mixer_add_strip_options_init.
/// @details A strip's meters size their buffers when the strip is built, so this is the only
///   place their configuration can be chosen. A full meter costs about 646 KB at 48 kHz and a
///   strip carries two of them; measure_lufs = 0 takes one meter to about 83 KB, and
///   enable_metering = 0 drops both (about 1.4 MB to 145 KB for the strip) for strips whose
///   snapshots are never read. The same configuration travels in scene JSON as the strip's
///   optional "metering" object.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_mixer_add_strip_options_init.
  uint32_t struct_size;
  int32_t enable_metering;       ///< Meter the strip pre and post; default 1, non-zero is on
  int32_t measure_lufs;          ///< Measure loudness; default 1, non-zero is on
  int32_t measure_true_peak;     ///< Measure true peak; default 1, non-zero is on
  int32_t true_peak_oversample;  ///< True-peak oversample factor, 4 (default) or 8
} SonareMixerAddStripOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_mixer_add_strip_options_init(SonareMixerAddStripOptions* options);

/// Adds a strip whose pre/post meters are configured up front.
///
/// @p options NULL is the full meter configuration (LUFS + true peak at 4x). Returns NULL with
/// sonare_last_error_message() set for a NULL handle or id, a duplicate id, a wrong
/// struct_size, or an oversample factor other than 4 or 8.
///
/// A non-NULL strip is owned by the mixer, NOT the caller: it stays valid until
/// sonare_mixer_destroy and must never be freed separately. Adding further strips
/// does not invalidate it.
SonareStrip* sonare_mixer_add_strip(SonareMixer* mixer, const char* id,
                                    const SonareMixerAddStripOptions* options);
/// @brief Sets the strip's input trim in dB, applied before the strip's processing. Refused with
///        SONARE_ERROR_INVALID_PARAMETER for a NULL strip or a non-finite value.
SonareError sonare_strip_set_input_trim_db(SonareStrip* strip, float db);
/// @brief Sets the strip's fader gain in dB. Refused with SONARE_ERROR_INVALID_PARAMETER for a
///        NULL strip or a non-finite value.
SonareError sonare_strip_set_fader_db(SonareStrip* strip, float db);
/// Sets the strip's pan position. pan_mode < 0 (SONARE_PAN_MODE_KEEP) keeps the
/// strip's current pan mode (only the pan position changes); otherwise it selects
/// the mode: 0 = Balance, 1 = StereoPan, 2 = DualPan. A pan outside [-1, 1] is refused
/// with SONARE_ERROR_INVALID_PARAMETER and leaves the strip unchanged.
SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode);
/// Both pans must lie in [-1, 1]; otherwise SONARE_ERROR_INVALID_PARAMETER, strip unchanged.
SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan);
/// Sets the strip's surround pan position (used when the strip feeds a >2-channel
/// bus). Returns SONARE_ERROR_INVALID_PARAMETER if strip or pan is NULL, an azimuth is outside
/// -180..180, or divergence or lfe is outside 0..1. The
/// values are stored on the strip/scene and applied when the engine's track
/// mixer renders this strip's lane into a destination with more than two
/// channels; the stereo-only mixer block entry points ignore them.
SonareError sonare_strip_set_surround_pan(SonareStrip* strip, const SonareSurroundPan* pan);
/// A width outside [0, 2] is refused with SONARE_ERROR_INVALID_PARAMETER, strip unchanged.
SonareError sonare_strip_set_width(SonareStrip* strip, float width);
/// @brief Mutes (non-zero) or unmutes (0) the strip.
SonareError sonare_strip_set_muted(SonareStrip* strip, int muted);
/// Sets the strip's solo state. Solo changes take effect on the next process
/// without a graph recompile (implied mutes are recomputed across the mixer).
SonareError sonare_strip_set_soloed(SonareStrip* strip, int soloed);
/// Marks a strip as solo-safe so it is never implied-muted by another strip's
/// solo. Takes effect on the next process without a graph recompile.
SonareError sonare_strip_set_solo_safe(SonareStrip* strip, int solo_safe);
/// Inverts the polarity of the left and/or right channel. Does not change
/// latency or topology.
SonareError sonare_strip_set_polarity_invert(SonareStrip* strip, int invert_left, int invert_right);
/// Sets the far-channel taper used by the strip's pan law. @c pan_law:
/// 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB, 3 = linear (0 dB). In Balance mode the
/// center remains unity and only the far channel is tapered. StereoPan folds the
/// pair to mono (0.5 * (L + R)) and applies the law's gains, so center takes
/// -3.01 / -4.52 / -6.02 / 0 dB for the four laws; DualPan pans each input with
/// the law's gains and takes the same center attenuation. A mono source is unity
/// at center under every law through this API: the mixer always feeds two
/// planes, and the law changes a mono strip's center gain only for a C++
/// ChannelStrip built without mono_pan_centre_unity. Returns
/// @c SONARE_ERROR_INVALID_PARAMETER if strip is NULL or pan_law is unknown.
SonareError sonare_strip_set_pan_law(SonareStrip* strip, int pan_law);
/// Sets a per-strip channel delay in samples, moving the strip later relative to the
/// others. It is not latency: latency compensation does not undo it and the mixer's
/// reported latency is unchanged, while the mixer tail grows by the delay.
SonareError sonare_strip_set_channel_delay_samples(SonareStrip* strip, int delay_samples);
/// Sets the strip's direct VCA trim in dB. It is persisted as the strip's
/// vcaOffsetDb scene field, separately from the offsets its VCA groups add.
SonareError sonare_strip_set_vca_offset_db(SonareStrip* strip, float offset_db);
/// Snaps the strip's input-trim, fader, pan and width smoothers to the values
/// already set on it, so the next processed block opens at those values instead
/// of gliding to them over the smoothing window (~5 ms by default).
///
/// Call it after configuring a strip and before rendering a finite buffer: a
/// strip is smoothed for a live fader, and an offline render that does not
/// settle carries that glide as a level and image sweep across the head of its
/// output. Unlike a reset it clears nothing -- automation, meters and insert
/// state are untouched.
///
/// Returns @c SONARE_ERROR_INVALID_PARAMETER if strip is NULL.
SonareError sonare_strip_settle(SonareStrip* strip);
/// @brief Adds a send from the strip to a destination bus.
/// @param id Optional send identifier; NULL is stored as the empty string.
/// @param destination_bus_id Id of the bus the send feeds; must not be NULL.
/// @param send_db Send level in dB; must be finite.
/// @param timing A @ref SonareSendTiming; any other value is refused.
/// @param index_out Receives the send's index in add order; may be NULL. Written 0 on failure.
SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out);
/// @brief Sets the level in dB of the send at @p index (in add order).
SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db);
/// Removes the send at @c index (in add order) from the strip. Higher sends shift
/// down by one index. The send is dropped from both the live strip and the scene
/// mirror, and the routing graph is marked dirty (recompile before processing).
/// Returns @c SONARE_ERROR_INVALID_PARAMETER if strip is NULL or @c index is out
/// of range.
SonareError sonare_strip_remove_send(SonareStrip* strip, unsigned int index);
/// Reads a meter snapshot at the given tap point. @c tap: 0 = pre-fader,
/// 1 = post-fader (see SonareMeterTap). Returns @c SONARE_ERROR_INVALID_PARAMETER
/// if strip or out is NULL, or tap is unknown.
SonareError sonare_strip_meter(const SonareStrip* strip, int tap, SonareMixMeterSnapshot* out);
/// @brief Reads the post-insert meter for a compiled bus (including master).
SonareError sonare_mixer_bus_meter(SonareMixer* mixer, const char* bus_id,
                                   SonareMixMeterSnapshot* out);
/// @brief Number of blocks in which a bus discarded recursive state because a
///        non-finite value had reached it. Same contract as
///        @ref sonare_strip_non_finite_discard_count, for a bus.
/// @details Covers every insert the bus owns and its meter; neither is
///   separately addressable, so a discard inside one is observable only here.
///   The unit is one processed block, never a channel and never an insert.
///   The count lives with the bus rather than with the compiled node, so it is
///   cumulative across graph recompiles and an unrelated edit elsewhere in the
///   mixer does not reset it. A bus's DSP record is created by the first
///   compile, so a bus that has been declared but never compiled is reported as
///   unknown rather than as zero.
///   Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer, bus_id or out_count is
///   NULL, or if no compiled bus has that id. Realtime-safe.
SonareError sonare_mixer_bus_non_finite_discard_count(const SonareMixer* mixer, const char* bus_id,
                                                      size_t* out_count);
/// @brief Copies the most recent goniometer points of the strip into @p out.
/// @param out Buffer of @p max_points points.
/// @param max_points Capacity of @p out.
/// @return Number of points written; 0 if strip or out is NULL or @p max_points is 0.
///         Realtime-safe, does not allocate.
size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points);
/// @brief Number of blocks in which the strip discarded recursive state because
///        a non-finite value had reached it.
/// @details Advisory telemetry, and the only thing that separates a degraded
///   strip from a clean one. A discard returns the affected state to its
///   post-reset value, so the strip recovers in silence and the output stays
///   finite and in range while carrying samples unrelated to the input; nothing
///   else reports that this happened.
///   The count covers the strip's own state, its EQ, every insert it owns and
///   both of its meters. None of those is separately addressable here, so a
///   discard inside one is observable only through this number -- and a meter
///   that loses its loudness window then reports the floor, which is what a
///   genuinely silent strip reports, so nothing else distinguishes the two.
///   The unit is one processed block, never a channel and never an insert, so a
///   stereo block that discards on both channels adds one and the number does
///   not depend on a dimension the caller did not choose. Cumulative since the
///   strip was created and never cleared, so two readings bracket a span of
///   audio. Returns @c SONARE_ERROR_INVALID_PARAMETER if strip or out_count is
///   NULL. Realtime-safe.
SonareError sonare_strip_non_finite_discard_count(const SonareStrip* strip, size_t* out_count);

/// Number of strips in the mixer (e.g. strips loaded from a scene). Returns 0 if
/// mixer is NULL.
size_t sonare_mixer_strip_count(const SonareMixer* mixer);
/// Number of strips, distinguishing a NULL mixer from an empty one. Returns
/// @c SONARE_ERROR_INVALID_PARAMETER if mixer or out_count is NULL, otherwise
/// @c SONARE_OK with the count written to @p out_count.
SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count);
/// Adds a bus to the mixer topology. @c role is one of "master", "aux", "submix"
/// (NULL defaults to "aux"). Mark the routing graph dirty; call
/// sonare_mixer_compile (or process) to rebuild. Returns
/// @c SONARE_ERROR_INVALID_PARAMETER if mixer or id is NULL, role is any other
/// string, or a bus with the same id already exists.
SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role);
/// Removes a bus by id. Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer or id
/// is NULL or no bus with that id exists.
SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id);
/// @brief Routes a strip or bus into a destination bus.
/// @details Removes every connection whose source is @p source_id -- including a
///   strip-to-strip edge authored in scene JSON -- and appends
///   {@p source_id, @p bus_id}; the routing graph is marked dirty and no compile
///   runs in the call. An explicit connection suppresses the default route to
///   the master, so a strip sent to a submix is silent on the master until the
///   submix is routed on. To feed a strip into a submix that goes to the master:
///   @code
///   sonare_mixer_set_output_bus(mixer, "a", "sub");
///   sonare_mixer_set_output_bus(mixer, "sub", "master");
///   @endcode
///   @p source_id is a strip or a non-master bus; @p bus_id is a declared bus or
///   the master (the id resolved the way the compile resolves it). Everything is
///   validated before anything changes, so a refused call leaves the topology
///   untouched. Returns @c SONARE_ERROR_INVALID_PARAMETER, naming the refused
///   argument in @ref sonare_last_error_message, if an argument is NULL,
///   the source or destination is unknown, the source is the master, the
///   destination is not a bus, the route is a self-route, or the edit would
///   close a cycle through connections or sends.
SonareError sonare_mixer_set_output_bus(SonareMixer* mixer, const char* source_id,
                                        const char* bus_id);
/// Number of buses in the mixer topology. Returns
/// @c SONARE_ERROR_INVALID_PARAMETER if mixer or out_count is NULL.
SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count);
/// Adds a VCA group with the given id and gain offset. @c members is an array of
/// @c member_count strip-id C strings (may be NULL when member_count is 0).
/// Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer or id is NULL, members is
/// NULL while member_count > 0, or a group with the same id already exists.
SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count);
/// Sets an existing VCA group's gain in dB. The live member strips receive only
/// the delta from the previous group gain, preserving direct strip VCA trims and
/// other VCA-group contributions. The stored scene gain is updated so
/// sonare_mixer_to_scene_json round-trips the new value.
SonareError sonare_mixer_set_vca_group_gain_db(SonareMixer* mixer, const char* id, float gain_db);
/// Replaces an existing VCA group's strip membership. Removed members lose this
/// group's current gain contribution; added members receive it immediately.
SonareError sonare_mixer_set_vca_group_members(SonareMixer* mixer, const char* id,
                                               const char* const* members, size_t member_count);
/// Removes a VCA group by id. Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer
/// or id is NULL or no group with that id exists.
SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id);
/// Number of VCA groups in the mixer topology. Returns
/// @c SONARE_ERROR_INVALID_PARAMETER if mixer or out_count is NULL.
SonareError sonare_mixer_vca_group_count(const SonareMixer* mixer, size_t* out_count);
/// Borrowed strip handle by index in [0, count). Returns NULL if out of range or
/// mixer is NULL. The handle is owned by the mixer; do not free it. It stays
/// valid until sonare_mixer_destroy — adding strips does not invalidate it, and
/// there is no entry point that removes one.
SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index);
/// Borrowed strip handle by strip id. Returns NULL if not found or mixer/id NULL.
/// The handle is owned by the mixer; do not free it. Same validity as
/// sonare_mixer_strip_at: it remains usable until sonare_mixer_destroy.
SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id);
/// Schedules sample-accurate insert-parameter automation on a strip's insert.
/// @c insert_index addresses the combined insert sequence
/// [pre-inserts... post-inserts...]. @c param_id is processor-specific (see each
/// processor's set_parameter doc). @c sample_pos is in absolute samples from the
/// start of processing: the mixer advances an internal sample position from 0 on
/// the first sonare_mixer_process_stereo call. Explicit and lazy graph
/// recompilation preserve this absolute timeline position; they do not restart
/// automation at sample 0.
///
/// AUTOMATION CURVE ORDINALS (canonical, shared across paths):
///   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
///
/// This is the SAME canonical ordinal scheme used by the PPQ-domain
/// SonareAutomationPoint.curve_to_next (sonare_engine_set_automation_lane). Both
/// paths resolve to the single sonare::AutomationCurve enum (mixing::
/// AutomationCurveType and automation::CurveType are aliases of it), so no
/// per-path "translation" of curve ordinals is needed when moving between the
/// engine-automation and mixer-automation APIs. Each call site pins the ordinals
/// with static_assert.
///
/// Returns @c SONARE_OK on success; @c SONARE_ERROR_INVALID_PARAMETER for a NULL
/// strip, unknown curve/param_id, or out-of-range insert; @c
/// SONARE_ERROR_NOT_SUPPORTED when the parameter exists but is not realtime-safe;
/// or @c SONARE_ERROR_OUT_OF_MEMORY when the fixed automation-lane/event capacity
/// is exhausted.
SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve);
/// Schedules sample-accurate fader/pan/width automation on a strip. @c sample_pos
/// uses the same absolute-sample timeline as sonare_strip_schedule_insert_automation.
///
/// @c curve uses the canonical automation-curve ordinal scheme:
///   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
/// (This is the same scheme used by the PPQ-domain
/// SonareAutomationPoint.curve_to_next; see the ordinal note above
/// sonare_strip_schedule_insert_automation.)
///
/// Returns @c SONARE_OK on success, @c SONARE_ERROR_OUT_OF_MEMORY if the event
/// lane is full, or @c SONARE_ERROR_INVALID_PARAMETER for a NULL strip, an
/// unknown curve, a non-finite value, or a non-monotonic @c sample_pos.
SonareError sonare_strip_schedule_fader_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float fader_db, int curve);
/// @brief Schedules sample-accurate pan automation on a strip; @p pan is the pan position and
///        @p curve the automation-curve ordinal described at
///        @ref sonare_strip_schedule_fader_automation.
SonareError sonare_strip_schedule_pan_automation(SonareStrip* strip, int64_t sample_pos, float pan,
                                                 int curve);
/// @brief Schedules sample-accurate stereo-width automation on a strip; @p curve is the
///        automation-curve ordinal described at @ref sonare_strip_schedule_fader_automation.
SonareError sonare_strip_schedule_width_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float width, int curve);
/// Schedules sample-accurate send-level automation on a strip's send. @c send_index
/// addresses the strip's sends in add order. See the schedulers above for timeline
/// and @c curve semantics (same sample-accurate mixing ordinal scheme:
/// 0 = Linear, 1 = Exponential, 2 = Hold, 3 = SCurve).
///
/// Returns @c SONARE_OK on success, @c SONARE_ERROR_INVALID_PARAMETER if strip is
/// NULL, curve is unknown, or @c send_index is out of range, or
/// @c SONARE_ERROR_OUT_OF_MEMORY if the send's event lane is full (matching the
/// fader/pan/width schedulers, so callers can distinguish a bad argument from a
/// capacity condition).
SonareError sonare_strip_schedule_send_automation(SonareStrip* strip, size_t send_index,
                                                  int64_t sample_pos, float db, int curve);
/// @p document is a mixer scene as sonare_mixer_to_scene_json writes it
/// (schemas/mixer-scene.schema.json).
/// Returns NULL when the scene JSON is malformed or a strip cannot be built; the
/// parse-failure message is available from sonare_last_error_message. A non-NULL
/// handle is owned by the caller and stays valid until sonare_mixer_destroy.
/// max_block_size follows sonare_mixer_create: 0 selects the library default (512 frames).
/// A key the scene reader does not consume (other than `$`- and `x-`-prefixed ones) fails the
/// load with SONARE_ERROR_INVALID_FORMAT naming it, e.g. `unknown key strips[2].faderDB`.
/// Insert params no processor reads do not fail the load; each is one Warning entry of
/// sonare_last_diagnostic (codes in the SonareDiagnostic table).
SonareMixer* sonare_mixer_from_scene_json(const char* document, int sample_rate,
                                          int max_block_size);
/// Free *json_out with sonare_free_string.
SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out);
/// Rebuilds and compiles the internal routing graph from the current topology
/// (strips, sends, buses, connections). Call after manual topology changes
/// (e.g. sonare_mixer_add_strip / sonare_strip_add_send) before processing.
/// sonare_mixer_process_stereo also compiles lazily as a fallback when the
/// topology is dirty. Recompilation preserves the mixer's absolute automation
/// sample position and the queued strip automation lanes.
SonareError sonare_mixer_compile(SonareMixer* mixer);
/// Reports the compiled mixer's latency at the master output. Lazily compiles if
/// the topology is dirty. Returns INVALID_PARAMETER for NULL arguments.
SonareError sonare_mixer_latency_samples(SonareMixer* mixer, int* out_latency_samples);
/// Reports the largest block sonare_mixer_process_stereo and sonare_mixer_drain_tail_stereo
/// accept: the max_block_size given at creation, or the library default when 0 was given.
/// The out-parameter is written 0 first.
SonareError sonare_mixer_max_block_size(const SonareMixer* mixer, int* out_max_block_size);
/// Reports the longest audible processor-tail path to the compiled master:
/// serial processor/node tails add, while parallel main/send branches take the
/// maximum. Lazily compiles if the topology is dirty.
SonareError sonare_mixer_tail_samples(SonareMixer* mixer, int* out_tail_samples);
/// Processes one stereo block through the compiled mixer graph.
///
/// Buffers:
/// - @c input_left and @c input_right are arrays of @c input_count planar channel
///   pointers, one L/R buffer pair per strip in strip order.
/// - @c output_left and @c output_right receive the master bus.
/// - @c num_samples must not exceed the @c max_block_size passed to
///   sonare_mixer_create.
///
/// REAL-TIME SAFETY: NOT guaranteed real-time safe. This is a block-convenience
/// entry point. If the topology is dirty (strips/sends/buses changed since the
/// last compile), it lazily rebuilds and compiles the routing graph, which can
/// allocate. To keep the audio thread allocation-free, call sonare_mixer_compile
/// once after each topology change and before the first process call. The
/// steady-state path with an already-compiled graph performs no heap allocation.
///
/// Returns @c SONARE_OK on success, or @c SONARE_ERROR_INVALID_PARAMETER if any
/// required pointer is NULL, @c input_count exceeds the strip count, a per-strip
/// channel pointer is NULL, or @c num_samples exceeds @c max_block_size.
SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples);
/// Processes a zero-input block through the mixer to drain delayed/tail audio
/// after the host has stopped feeding strip inputs. Equivalent to process_stereo
/// with input_count=0 and NULL input arrays, but explicit for offline renderers.
SonareError sonare_mixer_drain_tail_stereo(SonareMixer* mixer, float* output_left,
                                           float* output_right, size_t num_samples);
/// @brief Configuration of the mixer's master-output meter.
///
/// Prepare one with sonare_mixer_master_meter_config_init, which sets struct_size and every
/// field's library default; every field is then read literally. The measurement switches and
/// the oversample factor mean what they mean on SonareMixerAddStripOptions.
typedef struct {
  /// sizeof(SonareMixerMasterMeterConfig), set by sonare_mixer_master_meter_config_init. A
  /// smaller value is refused with SONARE_ERROR_INVALID_PARAMETER.
  uint32_t struct_size;
  int32_t enabled;               ///< Meter the master output; default 1, non-zero is on
  int32_t measure_lufs;          ///< Measure loudness; default 1, non-zero is on
  int32_t measure_true_peak;     ///< Measure true peak; default 1, non-zero is on
  int32_t true_peak_oversample;  ///< True-peak oversample factor, 4 (default) or 8
} SonareMixerMasterMeterConfig;

/// @brief Sets struct_size and every SonareMixerMasterMeterConfig default. NULL is a no-op.
void sonare_mixer_master_meter_config_init(SonareMixerMasterMeterConfig* config);

/// @brief Turns the master-output meter on or off and sets what it measures.
/// @details While enabled, every sonare_mixer_process_stereo / sonare_mixer_drain_tail_stereo
///   call meters the master output it writes, so every block reaches the meter unbroken. Enabling
///   resets the meter, so a reading never mixes audio from before a period with it off; enabling
///   again with the same measurement settings reuses the meter without allocating. Disabling keeps
///   the last reading readable. NULL @p config selects the defaults (enabled). Not to be called
///   concurrently with a process call or a read.
/// @return SONARE_ERROR_INVALID_PARAMETER for a NULL mixer, a struct_size below sizeof, or
///   true_peak_oversample outside {4, 8} while enabled; the meter is then unchanged.
SonareError sonare_mixer_configure_master_meter(SonareMixer* mixer,
                                                const SonareMixerMasterMeterConfig* config);

/// @brief Reads the master-output meter, describing the most recently metered block.
/// @details Same snapshot as sonare_mixer_bus_meter; a measurement switched off stays at the dB
///   floor. Realtime-safe: a lock-free read that records no error message.
/// @return SONARE_ERROR_INVALID_PARAMETER for a NULL argument, SONARE_ERROR_INVALID_STATE when
///   the master meter has never been enabled.
SonareError sonare_mixer_master_meter(SonareMixer* mixer, SonareMixMeterSnapshot* out);

/// Marks the end of the signal for every strip, bus and master-output meter: the
/// true-peak reading, which runs one reconstruction-filter group delay behind the
/// block, reads that pending group delay against silence and folds it into the
/// published snapshot. An offline or one-shot caller calls it after its last
/// block so true_peak_dbtp covers the whole signal. A later process call starts a
/// new signal. Returns INVALID_PARAMETER for a NULL mixer.
SonareError sonare_mixer_flush_meters(SonareMixer* mixer);
/// Returns the built-in mixing-scene preset names, separated by '\n'. Same
/// storage contract as sonare_mixing_assistant_source_class_names: thread-local,
/// rebuilt on every call, so the pointer is valid only until the next call to
/// this function on the same thread. Copy it to hold it; never free it. Never
/// NULL: an empty set comes back as the empty string.
const char* sonare_mixing_scene_preset_names(void);
/// Free *json_out with sonare_free_string.
SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out);
/// @brief Destroys a mixer and every strip it owns. NULL is a no-op. Strip handles obtained from
///        the mixer are invalid afterwards.
void sonare_mixer_destroy(SonareMixer* mixer);

// ============================================================================
// Mixing assistant
// ============================================================================
//
// Analyses a set of tracks and returns a suggested mixer scene. The assistant
// only ever returns parameters: it does not process audio and it does not apply
// anything. Feeding the suggestion to the mixer is the caller's separate,
// explicit step through sonare_mixer_from_scene_json, and there is deliberately
// no entry point that performs both halves at once.
//
// Everything here is offline / control-thread only. It allocates, runs an STFT
// per track and evaluates every track pair.
//
// Multi-track input follows the same planar convention as
// sonare_mixer_process_stereo: parallel arrays of per-track channel pointers,
// with @c input_right entries NULL for a mono track. Unlike the block-processing
// entry point, tracks may differ in length, so lengths arrive as a per-track
// array rather than one shared count; truncating every track to the shortest
// would delete a part that only enters late in the song.

/// Analyses @c input_count tracks and writes a suggested scene, the measurements
/// behind it and a human-readable explanation as a JSON object.
///
/// @param input_left   Per-track left/mono channel pointers; @c input_count entries.
/// @param input_right  Per-track right channel pointers, or NULL for an all-mono
///                     set. Individual entries may be NULL for a mono track.
/// @param track_ids    Per-track strip identifiers; @c input_count entries, each
///                     non-NULL, nonempty and unique. A NULL, empty or repeated
///                     id returns @c SONARE_ERROR_INVALID_PARAMETER.
/// @param track_names  Optional per-track display names used only as a
///                     classification hint, or NULL. Individual entries may be NULL.
/// @param track_lengths Per-track frame counts; @c input_count entries.
/// @param track_sample_rates Per-track sample rates, or NULL when every track is
///                     at @c sample_rate. A track at another rate is resampled
///                     onto @c sample_rate before analysis; an entry <= 0 returns
///                     @c SONARE_ERROR_INVALID_PARAMETER.
/// @param input_count  Number of tracks. Zero yields an empty suggestion.
/// @param sample_rate  Analysis rate every track is brought to; must be > 0.
/// @param params       Flat configuration; accepts targetTrackLufs,
///                     suggestionStrength, eqMaxCutDb, mixBusHeadroomDbtp,
///                     tempoBpm, enableStructure,
///                     enableGain, enableBalance, enableEq, enableDynamics,
///                     enableImage, enableHighPass, nFft and hopLength.
///                     tempoBpm is the tempo the suggested delay times are voiced
///                     against; 0 (the default) selects the transport's own
///                     fallback tempo, and a positive value outside 20..400 BPM
///                     is refused rather than clamped.
/// @param param_count  Number of entries in @c params.
/// @param json_out     Receives the result JSON. Release with sonare_free_string().
///
/// A track carrying a NaN or an infinity is not an error: it comes back in the
/// result document as unusable, with "track has non-finite samples" as its
/// exclusionReason, so the buffer is named rather than diagnosed as silence.
///
/// Returns @c SONARE_ERROR_NOT_SUPPORTED when the library was built without the
/// mixing assistant.
SonareError sonare_mixing_assistant_suggest(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, const int* track_sample_rates,
    size_t input_count, int sample_rate, const SonareMasteringParam* params, size_t param_count,
    char** json_out);

/// Returns the source-class identifiers the assistant can report, separated by
/// '\n'. Backed by thread-local storage REBUILT ON EVERY CALL, so the pointer is
/// valid only until the next call to this function on the same thread — unlike
/// the mastering *_names getters, whose storage is built once and stays valid
/// for the thread's lifetime. Copy the string if you need to hold it across
/// another call. Do NOT cache it across threads or free it. Never NULL: a build
/// without the assistant returns the empty string.
const char* sonare_mixing_assistant_source_class_names(void);

/// Resolves a source-class identifier to its enum value, or -1 when unknown.
int sonare_mixing_assistant_source_class_from_name(const char* name);

#ifdef __cplusplus
}
#endif
