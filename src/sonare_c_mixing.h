#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Mixing
// ============================================================================

typedef enum {
  SONARE_PAN_MODE_BALANCE = 0,
  SONARE_PAN_MODE_STEREO_PAN = 1,
  SONARE_PAN_MODE_DUAL_PAN = 2
} SonarePanMode;

// Sentinel for sonare_strip_set_pan's pan_mode argument: keep the strip's
// current pan mode and only move the pan position (see sonare_strip_set_pan).
#define SONARE_PAN_MODE_KEEP (-1)

typedef enum {
  SONARE_PAN_LAW_CONST_3DB = 0,
  SONARE_PAN_LAW_CONST_4P5DB = 1,
  SONARE_PAN_LAW_CONST_6DB = 2,
  SONARE_PAN_LAW_LINEAR_0DB = 3
} SonarePanLaw;

typedef enum {
  SONARE_SEND_TIMING_PRE_FADER = 0,
  SONARE_SEND_TIMING_POST_FADER = 1
} SonareSendTiming;

typedef enum { SONARE_METER_TAP_PRE_FADER = 0, SONARE_METER_TAP_POST_FADER = 1 } SonareMeterTap;

typedef struct {
  float peak_db_l;
  float peak_db_r;
  float rms_db_l;
  float rms_db_r;
  float correlation;
  float mono_compat_width;
  float mono_compat_peak;
  float mono_compat_side_rms;
  int likely_mono_compatible;
  float momentary_lufs;
  float short_term_lufs;
  float integrated_lufs;
  float gain_reduction_db;
  float true_peak_db_l;
  float true_peak_db_r;
  float max_true_peak_db;
  uint64_t seq;
} SonareMixMeterSnapshot;

typedef struct {
  float left;
  float right;
} SonareMixGoniometerPoint;

SonareMixer* sonare_mixer_create(int sample_rate, int max_block_size);
SonareStrip* sonare_mixer_add_strip(SonareMixer* mixer, const char* id);
SonareError sonare_strip_set_input_trim_db(SonareStrip* strip, float db);
SonareError sonare_strip_set_fader_db(SonareStrip* strip, float db);
// Sets the strip's pan position. pan_mode < 0 (SONARE_PAN_MODE_KEEP) keeps the
// strip's current pan mode (only the pan position changes); otherwise it selects
// the mode: 0 = Balance, 1 = StereoPan, 2 = DualPan.
SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode);
SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan);
SonareError sonare_strip_set_width(SonareStrip* strip, float width);
SonareError sonare_strip_set_muted(SonareStrip* strip, int muted);
// Sets the strip's solo state. Solo changes take effect on the next process
// without a graph recompile (implied mutes are recomputed across the mixer).
SonareError sonare_strip_set_soloed(SonareStrip* strip, int soloed);
// Marks a strip as solo-safe so it is never implied-muted by another strip's
// solo. Takes effect on the next process without a graph recompile.
SonareError sonare_strip_set_solo_safe(SonareStrip* strip, int solo_safe);
// Inverts the polarity of the left and/or right channel. Does not change
// latency or topology.
SonareError sonare_strip_set_polarity_invert(SonareStrip* strip, int invert_left, int invert_right);
// Sets the strip's pan law. @c pan_law: 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB,
// 3 = linear (0 dB). Returns @c SONARE_ERROR_INVALID_PARAMETER if strip is NULL
// or pan_law is unknown.
SonareError sonare_strip_set_pan_law(SonareStrip* strip, int pan_law);
// Sets a per-strip channel delay in samples. This changes the strip's reported
// latency; the routing graph re-runs latency compensation at the next compile.
SonareError sonare_strip_set_channel_delay_samples(SonareStrip* strip, int delay_samples);
// Sets the strip's live VCA gain offset in dB. VCA is a group concept with no
// per-strip scene field, so this is not persisted to the scene JSON.
SonareError sonare_strip_set_vca_offset_db(SonareStrip* strip, float offset_db);
SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out);
SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db);
// Removes the send at @c index (in add order) from the strip. Higher sends shift
// down by one index. The send is dropped from both the live strip and the scene
// mirror, and the routing graph is marked dirty (recompile before processing).
// Returns @c SONARE_ERROR_INVALID_PARAMETER if strip is NULL or @c index is out
// of range.
SonareError sonare_strip_remove_send(SonareStrip* strip, unsigned int index);
SonareError sonare_strip_meter(const SonareStrip* strip, SonareMixMeterSnapshot* out);
// Reads a meter snapshot at the given tap point. @c tap: 0 = pre-fader,
// 1 = post-fader (see SonareMeterTap). Returns @c SONARE_ERROR_INVALID_PARAMETER
// if strip or out is NULL, or tap is unknown.
SonareError sonare_strip_meter_tap(const SonareStrip* strip, int tap, SonareMixMeterSnapshot* out);
size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points);

// Number of strips in the mixer (e.g. strips loaded from a scene). Returns 0 if
// mixer is NULL.
size_t sonare_mixer_strip_count(const SonareMixer* mixer);
// Number of strips, distinguishing a NULL mixer from an empty one. Returns
// @c SONARE_ERROR_INVALID_PARAMETER if mixer or out_count is NULL, otherwise
// @c SONARE_OK with the count written to @p out_count.
SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count);
// Adds a bus to the mixer topology. @c role is one of "master", "aux", "submix"
// (NULL defaults to "aux"). Mark the routing graph dirty; call
// sonare_mixer_compile (or process) to rebuild. Returns
// @c SONARE_ERROR_INVALID_PARAMETER if mixer or id is NULL, or a bus with the
// same id already exists.
SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role);
// Removes a bus by id. Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer or id
// is NULL or no bus with that id exists.
SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id);
// Number of buses in the mixer topology. Returns
// @c SONARE_ERROR_INVALID_PARAMETER if mixer or out_count is NULL.
SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count);
// Adds a VCA group with the given id and gain offset. @c members is an array of
// @c member_count strip-id C strings (may be NULL when member_count is 0).
// Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer or id is NULL, members is
// NULL while member_count > 0, or a group with the same id already exists.
SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count);
// Removes a VCA group by id. Returns @c SONARE_ERROR_INVALID_PARAMETER if mixer
// or id is NULL or no group with that id exists.
SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id);
// Number of VCA groups in the mixer topology. Returns
// @c SONARE_ERROR_INVALID_PARAMETER if mixer or out_count is NULL.
SonareError sonare_mixer_vca_group_count(const SonareMixer* mixer, size_t* out_count);
// Borrowed strip handle by index in [0, count). Returns NULL if out of range or
// mixer is NULL. The handle is owned by the mixer; do not free it.
SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index);
// Borrowed strip handle by strip id. Returns NULL if not found or mixer/id NULL.
// The handle is owned by the mixer; do not free it.
SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id);
// Schedules sample-accurate insert-parameter automation on a strip's insert.
// @c insert_index addresses the combined insert sequence
// [pre-inserts... post-inserts...]. @c param_id is processor-specific (see each
// processor's set_parameter doc). @c sample_pos is in absolute samples from the
// start of processing: the mixer advances an internal sample position from 0 on
// the first sonare_mixer_process_stereo call (reset to 0 on recompile).
//
// AUTOMATION CURVE ORDINALS (canonical, shared across paths):
//   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
//
// This is the SAME canonical ordinal scheme used by the PPQ-domain
// SonareAutomationPoint.curve_to_next (sonare_engine_set_automation_lane). Both
// paths resolve to the single sonare::AutomationCurve enum (mixing::
// AutomationCurveType and automation::CurveType are aliases of it), so no
// per-path "translation" of curve ordinals is needed when moving between the
// engine-automation and mixer-automation APIs. Each call site pins the ordinals
// with static_assert.
//
// Returns @c SONARE_OK on success, or @c SONARE_ERROR_INVALID_PARAMETER if
// strip is NULL, curve is unknown, or insert_index is out of range.
SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve);
// Schedules sample-accurate fader/pan/width automation on a strip. @c sample_pos
// uses the same absolute-sample timeline as sonare_strip_schedule_insert_automation.
//
// @c curve uses the canonical automation-curve ordinal scheme:
//   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
// (This is the same scheme used by the PPQ-domain
// SonareAutomationPoint.curve_to_next; see the ordinal note above
// sonare_strip_schedule_insert_automation.)
//
// Returns @c SONARE_OK on success, or @c SONARE_ERROR_INVALID_PARAMETER if
// strip is NULL, curve is unknown, or the event lane is full.
SonareError sonare_strip_schedule_fader_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float fader_db, int curve);
SonareError sonare_strip_schedule_pan_automation(SonareStrip* strip, int64_t sample_pos, float pan,
                                                 int curve);
SonareError sonare_strip_schedule_width_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float width, int curve);
// Schedules sample-accurate send-level automation on a strip's send. @c send_index
// addresses the strip's sends in add order. See the schedulers above for timeline
// and @c curve semantics (same sample-accurate mixing ordinal scheme:
// 0 = Linear, 1 = Exponential, 2 = Hold, 3 = SCurve).
//
// Returns @c SONARE_OK on success, @c SONARE_ERROR_INVALID_PARAMETER if strip is
// NULL, curve is unknown, or @c send_index is out of range, or
// @c SONARE_ERROR_OUT_OF_MEMORY if the send's event lane is full (matching the
// fader/pan/width schedulers, so callers can distinguish a bad argument from a
// capacity condition).
SonareError sonare_strip_schedule_send_automation(SonareStrip* strip, size_t send_index,
                                                  int64_t sample_pos, float db, int curve);
SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size);
SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out);
// Rebuilds and compiles the internal routing graph from the current topology
// (strips, sends, buses, connections). Call after manual topology changes
// (e.g. sonare_mixer_add_strip / sonare_strip_add_send) before processing.
// sonare_mixer_process_stereo also compiles lazily as a fallback when the
// topology is dirty.
SonareError sonare_mixer_compile(SonareMixer* mixer);
// Reports the compiled mixer's latency at the master output. Lazily compiles if
// the topology is dirty. Returns INVALID_PARAMETER for NULL arguments.
SonareError sonare_mixer_latency_samples(SonareMixer* mixer, int* out_latency_samples);
// Reports the maximum processor tail length currently present in the compiled
// mixer graph. Lazily compiles if the topology is dirty.
SonareError sonare_mixer_tail_samples(SonareMixer* mixer, int* out_tail_samples);
// Processes one stereo block through the compiled mixer graph.
//
// Buffers:
// - @c input_left and @c input_right are arrays of @c input_count planar channel
//   pointers, one L/R buffer pair per strip in strip order.
// - @c output_left and @c output_right receive the master bus.
// - @c num_samples must not exceed the @c max_block_size passed to
//   sonare_mixer_create.
//
// REAL-TIME SAFETY: NOT guaranteed real-time safe. This is a block-convenience
// entry point. If the topology is dirty (strips/sends/buses changed since the
// last compile), it lazily rebuilds and compiles the routing graph, which can
// allocate. To keep the audio thread allocation-free, call sonare_mixer_compile
// once after each topology change and before the first process call. The
// steady-state path with an already-compiled graph performs no heap allocation.
//
// Returns @c SONARE_OK on success, or @c SONARE_ERROR_INVALID_PARAMETER if any
// required pointer is NULL, @c input_count exceeds the strip count, a per-strip
// channel pointer is NULL, or @c num_samples exceeds @c max_block_size.
SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples);
// Processes a zero-input block through the mixer to drain delayed/tail audio
// after the host has stopped feeding strip inputs. Equivalent to process_stereo
// with input_count=0 and NULL input arrays, but explicit for offline renderers.
SonareError sonare_mixer_drain_tail_stereo(SonareMixer* mixer, float* output_left,
                                           float* output_right, size_t num_samples);
const char* sonare_mixing_scene_preset_names(void);
SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out);
void sonare_mixer_destroy(SonareMixer* mixer);

#ifdef __cplusplus
}
#endif
