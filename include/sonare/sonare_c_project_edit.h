#pragma once

/// @file sonare_c_project_edit.h
/// @brief Track / clip / warp / take / automation descriptors and the undoable
///        edit-command surface for the headless arrangement C ABI. Included via
///        @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Description for @ref sonare_project_add_track. The track name is an
///        optional NUL-terminated C string (NULL = empty name).
typedef struct {
  int kind; /* SonareProjectTrackKind */
  const char* name;
} SonareProjectTrackDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectTrackDesc, kind) == 0, "TrackDesc.kind offset");
static_assert(offsetof(SonareProjectTrackDesc, name) == sizeof(void*), "TrackDesc.name offset");
static_assert(sizeof(SonareProjectTrackDesc) == 2u * sizeof(void*),
              "SonareProjectTrackDesc layout drift");
#endif

/// @brief Description for @ref sonare_project_add_clip.
///
/// For an AUDIO clip, optionally supply decoded interleaved samples
/// (@p audio_interleaved / @p audio_frames / @p audio_channels /
/// @p audio_sample_rate). When @p audio_interleaved is non-NULL the call
/// registers a fresh audio source, populates the audio content store (so the
/// clip is renderable by @ref sonare_project_bounce), and binds the clip to it.
/// When @p audio_interleaved is NULL the clip is created against a metadata-only
/// audio source (URI from @p source_uri, which may be NULL/empty).
///
/// For a MIDI clip set @p is_midi to non-zero: a MIDI source is registered and
/// the clip is bound to it. MIDI events are supplied separately via
/// @ref sonare_project_set_midi_events.
///
/// All musical positions are PPQ (quarter notes). @p length_ppq must be > 0.
typedef struct {
  uint32_t track_id;
  int is_midi; /* 0 = audio clip, non-zero = MIDI clip */

  double start_ppq;
  double length_ppq;
  double source_offset_ppq;
  float gain; /* linear playback gain; must be finite and >= 0. 0 = silent clip
                 (passed through verbatim — no coercion to unity). */

  /* Audio source content (audio clips only; NULL => metadata-only source). */
  const float* audio_interleaved;
  int64_t audio_frames;
  int audio_channels;
  int audio_sample_rate;

  /* Optional host-local source reference for metadata-only audio sources. */
  const char* source_uri;
} SonareProjectClipDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipDesc, track_id) == 0, "ClipDesc.track_id offset");
static_assert(offsetof(SonareProjectClipDesc, is_midi) == 4, "ClipDesc.is_midi offset");
static_assert(offsetof(SonareProjectClipDesc, start_ppq) == 8, "ClipDesc.start_ppq offset");
static_assert(offsetof(SonareProjectClipDesc, length_ppq) == 16, "ClipDesc.length_ppq offset");
static_assert(offsetof(SonareProjectClipDesc, source_offset_ppq) == 24,
              "ClipDesc.source_offset_ppq offset");
static_assert(offsetof(SonareProjectClipDesc, gain) == 32, "ClipDesc.gain offset");
// The tail (pointer + the int64/int run + trailing pointer) is pointer-size
// dependent, so express offsets relative to sizeof(void*) instead of hardcoding
// 64-bit values. This holds on both LP64/LLP64 native (8-byte pointers ->
// 40/48/56/60/64, size 72) and wasm32 (4-byte pointers -> 36/40/48/52/56,
// size 64). Native flat-POD memcpy FFI consumers are 64-bit; wasm marshals via
// embind, but the layout stays self-consistent on every target.
static_assert(offsetof(SonareProjectClipDesc, audio_interleaved) == 32u + sizeof(void*),
              "ClipDesc.audio_interleaved offset");
static_assert(offsetof(SonareProjectClipDesc, audio_frames) == 32u + 2u * sizeof(void*),
              "ClipDesc.audio_frames offset");
static_assert(offsetof(SonareProjectClipDesc, audio_channels) == 40u + 2u * sizeof(void*),
              "ClipDesc.audio_channels offset");
static_assert(offsetof(SonareProjectClipDesc, audio_sample_rate) == 44u + 2u * sizeof(void*),
              "ClipDesc.audio_sample_rate offset");
static_assert(offsetof(SonareProjectClipDesc, source_uri) == 48u + 2u * sizeof(void*),
              "ClipDesc.source_uri offset");
static_assert(sizeof(SonareProjectClipDesc) ==
                  ((48u + 3u * sizeof(void*) + 7u) & ~static_cast<size_t>(7u)),
              "SonareProjectClipDesc layout drift");
#endif

/// @brief Description for @ref sonare_project_add_loop_recording_takes.
///
/// The input is a captured interleaved audio buffer. The project tempo map is
/// used to convert @p loop_length_ppq at @p start_ppq into an audio-frame loop
/// span; each loop span becomes a separate audio source/take, and one clip of
/// length @p loop_length_ppq is added to @p track_id. The newest take is made
/// active. A final partial loop is kept as the last take.
typedef struct {
  uint32_t track_id;
  uint32_t reserved;
  double start_ppq;
  double loop_length_ppq;
  const float* audio_interleaved;
  int64_t audio_frames;
  int audio_channels;
  int audio_sample_rate;
} SonareProjectLoopRecordingDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectLoopRecordingDesc, track_id) == 0,
              "LoopRecordingDesc.track_id offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, reserved) == sizeof(uint32_t),
              "LoopRecordingDesc.reserved offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, start_ppq) == sizeof(double),
              "LoopRecordingDesc.start_ppq offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, loop_length_ppq) == 2u * sizeof(double),
              "LoopRecordingDesc.loop_length_ppq offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, audio_interleaved) == 3u * sizeof(double),
              "LoopRecordingDesc.audio_interleaved offset");
static_assert(offsetof(SonareProjectLoopRecordingDesc, audio_frames) ==
                  ((3u * sizeof(double) + sizeof(void*) + 7u) & ~static_cast<size_t>(7u)),
              "LoopRecordingDesc.audio_frames offset");
static_assert(sizeof(SonareProjectLoopRecordingDesc) == 6u * sizeof(double),
              "SonareProjectLoopRecordingDesc layout drift");
#endif

/// @brief One warp-map anchor for @ref sonare_project_set_warp_map.
typedef struct {
  double warp_sample;
  double source_sample;
} SonareProjectWarpAnchor;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectWarpAnchor, warp_sample) == 0, "WarpAnchor.warp_sample offset");
static_assert(offsetof(SonareProjectWarpAnchor, source_sample) == sizeof(double),
              "WarpAnchor.source_sample offset");
static_assert(sizeof(SonareProjectWarpAnchor) == 2u * sizeof(double),
              "SonareProjectWarpAnchor layout drift");
#endif

typedef enum {
  SONARE_PROJECT_WARP_MODE_OFF = 0,
  SONARE_PROJECT_WARP_MODE_REPITCH = 1,
  SONARE_PROJECT_WARP_MODE_TEMPO_SYNC = 2,
} SonareProjectWarpMode;

/// @brief First-class project warp-map descriptor.
typedef struct {
  uint32_t id;
  const char* name;
  const SonareProjectWarpAnchor* anchors;
  size_t anchor_count;
} SonareProjectWarpMapDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectWarpMapDesc, id) == 0, "WarpMapDesc.id offset");
static_assert(offsetof(SonareProjectWarpMapDesc, name) == sizeof(void*), "WarpMapDesc.name offset");
static_assert(offsetof(SonareProjectWarpMapDesc, anchors) == 2u * sizeof(void*),
              "WarpMapDesc.anchors offset");
static_assert(offsetof(SonareProjectWarpMapDesc, anchor_count) == 3u * sizeof(void*),
              "WarpMapDesc.anchor_count offset");
static_assert(sizeof(SonareProjectWarpMapDesc) == 3u * sizeof(void*) + sizeof(size_t),
              "SonareProjectWarpMapDesc layout drift");
#endif

/// @brief Clip fade-curve ordinals; mirror sonare::arrangement::FadeCurve.
///        Pinned by a static_assert in the .cpp so reordering is caught.
typedef enum {
  SONARE_FADE_CURVE_LINEAR = 0,
  SONARE_FADE_CURVE_EQUAL_POWER = 1,
  SONARE_FADE_CURVE_EXPONENTIAL = 2,
  SONARE_FADE_CURVE_LOGARITHMIC = 3,
} SonareProjectFadeCurve;

/// @brief Clip loop-mode ordinals; mirror sonare::arrangement::LoopMode.
///        Pinned by a static_assert in the .cpp.
typedef enum {
  SONARE_LOOP_MODE_OFF = 0,
  SONARE_LOOP_MODE_LOOP = 1,
} SonareProjectLoopMode;

/// @brief Automation breakpoint interpolation ordinals; mirror
///        sonare::AutomationCurve. Pinned by a static_assert in the .cpp.
typedef enum {
  SONARE_CURVE_LINEAR = 0,
  SONARE_CURVE_EXPONENTIAL = 1,
  SONARE_CURVE_HOLD = 2,
  SONARE_CURVE_SCURVE = 3,
} SonareProjectAutomationCurve;

/// @brief One clip fade region for @ref sonare_project_set_clip_fade.
///        @p length_ppq must be finite and >= 0 (0 = no fade). @p curve is a
///        @ref SonareProjectFadeCurve ordinal.
typedef struct {
  double length_ppq;
  uint32_t curve; /* SonareProjectFadeCurve */
} SonareProjectClipFade;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipFade, length_ppq) == 0, "ClipFade.length_ppq offset");
static_assert(offsetof(SonareProjectClipFade, curve) == sizeof(double), "ClipFade.curve offset");
static_assert(sizeof(SonareProjectClipFade) == sizeof(double) + 2u * sizeof(uint32_t),
              "SonareProjectClipFade layout drift");
#endif

/// @brief One recorded take/generation attached to a clip.
///
/// @p id is clip-local and must be non-zero. @p source_id may be 0 to use the
/// clip's current source, or an existing source id compatible with the clip's
/// track kind. @p source_offset_ppq must be finite and >= 0. @p name is optional.
typedef struct {
  uint32_t id;
  uint32_t source_id;
  double source_offset_ppq;
  const char* name;
} SonareProjectClipTake;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipTake, id) == 0, "ClipTake.id offset");
static_assert(offsetof(SonareProjectClipTake, source_id) == sizeof(uint32_t),
              "ClipTake.source_id offset");
static_assert(offsetof(SonareProjectClipTake, source_offset_ppq) == sizeof(double),
              "ClipTake.source_offset_ppq offset");
static_assert(offsetof(SonareProjectClipTake, name) == 2u * sizeof(double), "ClipTake.name offset");
static_assert(sizeof(SonareProjectClipTake) ==
                  ((2u * sizeof(double) + sizeof(void*) + 7u) & ~static_cast<size_t>(7u)),
              "SonareProjectClipTake layout drift");
#endif

/// @brief One clip-local comp segment selecting a take over [start_ppq,end_ppq).
typedef struct {
  double start_ppq;
  double end_ppq;
  uint32_t take_id;
} SonareProjectClipCompSegment;

#ifdef __cplusplus
static_assert(offsetof(SonareProjectClipCompSegment, start_ppq) == 0,
              "ClipCompSegment.start_ppq offset");
static_assert(offsetof(SonareProjectClipCompSegment, end_ppq) == sizeof(double),
              "ClipCompSegment.end_ppq offset");
static_assert(offsetof(SonareProjectClipCompSegment, take_id) == 2u * sizeof(double),
              "ClipCompSegment.take_id offset");
static_assert(sizeof(SonareProjectClipCompSegment) == 3u * sizeof(double),
              "SonareProjectClipCompSegment layout drift");
#endif

/// @brief Description for @ref sonare_project_add_automation_lane and
///        @ref sonare_project_edit_automation_lane.
///
/// @p target_param_id identifies the parameter the lane drives (host-defined,
/// e.g. a mixer-strip volume parameter id). @p points is an array of
/// @p point_count breakpoints (the shared @ref SonareAutomationPoint POD whose
/// @c curve_to_next uses the @ref SonareProjectAutomationCurve ordinals). It may
/// be NULL only when @p point_count is 0. The breakpoints are copied; the core
/// does not require them pre-sorted but stores them verbatim.
typedef struct {
  uint32_t target_param_id;
  const SonareAutomationPoint* points;
  size_t point_count;
} SonareAutomationLaneDesc;

#ifdef __cplusplus
static_assert(offsetof(SonareAutomationLaneDesc, target_param_id) == 0,
              "AutomationLaneDesc.target_param_id offset");
static_assert(offsetof(SonareAutomationLaneDesc, points) == sizeof(void*),
              "AutomationLaneDesc.points offset");
static_assert(offsetof(SonareAutomationLaneDesc, point_count) == 2u * sizeof(void*),
              "AutomationLaneDesc.point_count offset");
static_assert(sizeof(SonareAutomationLaneDesc) == 2u * sizeof(void*) + sizeof(size_t),
              "SonareAutomationLaneDesc layout drift");
#endif

// ============================================================================
// Edit (all mutation routes through EditHistory commands)
// ============================================================================

/// @brief Adds a track; returns the allocated stable id via @p out_track_id.
SonareError sonare_project_add_track(SonareProject* project, const SonareProjectTrackDesc* desc,
                                     uint32_t* out_track_id);

/// @brief Adds a clip (audio or MIDI per @p desc); returns the allocated clip id.
SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id);

/// @brief Splits captured loop-recording audio into takes and adds one clip.
///        Returns the allocated clip id and optional take count.
SonareError sonare_project_add_loop_recording_takes(SonareProject* project,
                                                    const SonareProjectLoopRecordingDesc* desc,
                                                    uint32_t* out_clip_id, size_t* out_take_count);

/// @brief Convenience wrapper that creates a MIDI track + a MIDI clip on it.
///        Returns the allocated track and clip ids. @p length_ppq must be > 0.
SonareError sonare_project_add_midi_clip(SonareProject* project, double start_ppq,
                                         double length_ppq, uint32_t* out_track_id,
                                         uint32_t* out_clip_id);

/// @brief Splits a clip at @p split_ppq (absolute PPQ). @p out_new_clip_id
///        receives the right-hand clip id (may be NULL).
SonareError sonare_project_split_clip(SonareProject* project, uint32_t clip_id, double split_ppq,
                                      uint32_t* out_new_clip_id);

/// @brief Trims a clip's start / length (PPQ). See arrangement::TrimClip.
SonareError sonare_project_trim_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     double new_length_ppq);

/// @brief Moves a clip to @p new_start_ppq, optionally to @p new_track_id
///        (0 = keep current track).
SonareError sonare_project_move_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     uint32_t new_track_id);

/// @brief Changes a track kind via an undoable edit command.
SonareError sonare_project_set_track_kind(SonareProject* project, uint32_t track_id, uint32_t kind);

/// @brief Sets a clip's warp reference id via an undoable edit command.
///
/// Use @p warp_ref_id = 0 to clear the reference. Non-zero ids are intended to
/// reference a map registered with @ref sonare_project_set_warp_map.
SonareError sonare_project_set_clip_warp_ref(SonareProject* project, uint32_t clip_id,
                                             uint32_t warp_ref_id);

/// @brief Sets a clip's warp playback mode via an undoable edit command.
SonareError sonare_project_set_clip_warp_mode(SonareProject* project, uint32_t clip_id,
                                              SonareProjectWarpMode mode);

/// @brief Replaces a clip's take list and active take via an undoable edit.
///
/// @p takes may be NULL only when @p take_count is 0. @p active_take_id may be
/// 0 to use the clip's base source, otherwise it must reference one of @p takes.
/// Existing comp segments must still reference valid take ids after replacement.
SonareError sonare_project_set_clip_takes(SonareProject* project, uint32_t clip_id,
                                          const SonareProjectClipTake* takes, size_t take_count,
                                          uint32_t active_take_id);

/// @brief Replaces a clip's comp lane via an undoable edit.
///
/// Segments are clip-local PPQ ranges and must be finite, positive length,
/// sorted, non-overlapping, inside the clip, and reference existing take ids
/// (or 0 for the base/active fallback).
SonareError sonare_project_set_clip_comp_segments(SonareProject* project, uint32_t clip_id,
                                                  const SonareProjectClipCompSegment* segments,
                                                  size_t segment_count);

/// @brief Adds or replaces a first-class project warp map via an undoable edit.
///        @p desc->id must be non-zero and @p anchors must contain at least two
///        finite, strictly increasing anchor pairs.
SonareError sonare_project_set_warp_map(SonareProject* project,
                                        const SonareProjectWarpMapDesc* desc);

/// @brief Removes a first-class project warp map via an undoable edit command.
SonareError sonare_project_remove_warp_map(SonareProject* project, uint32_t warp_ref_id);

/// @brief Routes a track's MIDI to host-instrument destination @p destination_id
///        (0 = the default destination). The arrangement compiler stamps every
///        MIDI clip on the track with this id so the engine dispatches its events
///        to the instrument registered for that destination. Routes through an
///        undoable edit command. @p track_id must reference an existing track.
SonareError sonare_project_set_track_midi_destination(SonareProject* project, uint32_t track_id,
                                                      uint32_t destination_id);

/// @brief Sets a track's linear playback gain (1.0 = unity) via an undoable edit
///        command. The arrangement compiler folds the track's gain/mute/solo/pan
///        into the track's channel strip (synthesizing one when the track is not
///        bound to a strip), so the value applies uniformly to the track's audio
///        and MIDI. @p gain is clamped to >= 0; @p track_id must reference an
///        existing track.
SonareError sonare_project_set_track_gain(SonareProject* project, uint32_t track_id, float gain);

/// @brief Sets a track's mute flag via an undoable edit command. A muted track is
///        silent. @p track_id must reference an existing track. See
///        @ref sonare_project_set_track_gain for how track controls are applied.
SonareError sonare_project_set_track_mute(SonareProject* project, uint32_t track_id, int mute);

/// @brief Sets a track's solo flag via an undoable edit command. When any track is
///        soloed, only soloed tracks sound. @p track_id must reference an existing
///        track. See @ref sonare_project_set_track_gain for how controls apply.
SonareError sonare_project_set_track_solo(SonareProject* project, uint32_t track_id, int solo);

/// @brief Sets a track's stereo balance in [-1, +1] (0 = center) via an undoable
///        edit command. @p pan is clamped to the valid range. @p track_id must
///        reference an existing track. See @ref sonare_project_set_track_gain for
///        how track controls are applied.
SonareError sonare_project_set_track_pan(SonareProject* project, uint32_t track_id, float pan);

/// @brief Removes a clip via an undoable edit command. @p clip_id must
///        reference an existing clip. Undo restores the clip (and its MIDI
///        content) at its original position.
SonareError sonare_project_remove_clip(SonareProject* project, uint32_t clip_id);

/// @brief Sets a clip's linear playback gain via an undoable edit command.
///        @p gain must be finite and >= 0 (0 = muted). Like the @p gain field of
///        @ref sonare_project_add_clip, the value is applied verbatim (no
///        coercion of 0 to unity).
SonareError sonare_project_set_clip_gain(SonareProject* project, uint32_t clip_id, float gain);

/// @brief Sets a clip's fade-in and fade-out regions via an undoable edit
///        command. Each fade length (PPQ) must be finite and >= 0 (0 = no fade);
///        each curve is a @ref SonareProjectFadeCurve ordinal.
SonareError sonare_project_set_clip_fade(SonareProject* project, uint32_t clip_id,
                                         const SonareProjectClipFade* fade_in,
                                         const SonareProjectClipFade* fade_out);

/// @brief Sets a clip's loop mode + loop length (PPQ) via an undoable edit
///        command. @p loop_mode is a @ref SonareProjectLoopMode ordinal. When
///        @p loop_mode is SONARE_LOOP_MODE_LOOP, @p loop_length_ppq must be
///        finite and > 0; otherwise it must be finite and >= 0.
/// @param loop_crossfade_ppq Optional equal-power crossfade length at the loop
///        seam, in PPQ (must be finite and >= 0; 0 = hard loop). Applied only
///        when looping; the engine clamps it to the clip's available pre-roll
///        (source offset) and to half the loop, and ignores it under warp.
SonareError sonare_project_set_clip_loop(SonareProject* project, uint32_t clip_id, int loop_mode,
                                         double loop_length_ppq, double loop_crossfade_ppq);

/// @brief Rebinds a clip to a different (already-registered) source via an
///        undoable edit command. @p source_id must reference an existing source;
///        @p clip_id an existing clip.
SonareError sonare_project_set_clip_source(SonareProject* project, uint32_t clip_id,
                                           uint32_t source_id);

/// @brief Duplicates a clip at @p new_start_ppq (same track), allocating a fresh
///        id and copying any MIDI content, via an undoable edit command.
///        @p out_new_clip_id receives the new clip id (may be NULL).
SonareError sonare_project_duplicate_clip(SonareProject* project, uint32_t clip_id,
                                          double new_start_ppq, uint32_t* out_new_clip_id);

/// @brief Removes a track via an undoable edit command. @p track_id must
///        reference an existing track. (The command layer removes the track's
///        clips first; undo restores the track and its clips.)
SonareError sonare_project_remove_track(SonareProject* project, uint32_t track_id);

/// @brief Renames a track via an undoable edit command. @p name is an optional
///        NUL-terminated C string (NULL = empty name). @p track_id must
///        reference an existing track.
SonareError sonare_project_rename_track(SonareProject* project, uint32_t track_id,
                                        const char* name);

/// @brief Sets a track's mixer-strip binding + output target via an undoable
///        edit command. @p channel_strip_ref and @p output_target are
///        NUL-terminated C strings; pass NULL or "" to clear the respective
///        field. @p track_id must reference an existing track.
SonareError sonare_project_set_track_route(SonareProject* project, uint32_t track_id,
                                           const char* channel_strip_ref,
                                           const char* output_target);

/// @brief Appends an automation lane to a track via an undoable edit command.
///        @p track_id must reference an existing track; @p desc describes the
///        target parameter id and breakpoints (see @ref SonareAutomationLaneDesc).
///        @p out_lane_index receives the appended lane's index within the track
///        (may be NULL).
SonareError sonare_project_add_automation_lane(SonareProject* project, uint32_t track_id,
                                               const SonareAutomationLaneDesc* desc,
                                               size_t* out_lane_index);

/// @brief Replaces an existing automation lane in place via an undoable edit
///        command. @p track_id must reference an existing track and
///        @p lane_index an existing lane on it.
SonareError sonare_project_edit_automation_lane(SonareProject* project, uint32_t track_id,
                                                size_t lane_index,
                                                const SonareAutomationLaneDesc* desc);

/// @brief Removes an automation lane from a track via an undoable edit command.
///        @p track_id must reference an existing track and @p lane_index an
///        existing lane on it.
SonareError sonare_project_remove_automation_lane(SonareProject* project, uint32_t track_id,
                                                  size_t lane_index);

/// @brief Undoes the most recent edit. Returns SONARE_ERROR_INVALID_STATE when
///        the undo stack is empty.
SonareError sonare_project_undo(SonareProject* project);
/// @brief Redoes the most recently undone edit. Returns
///        SONARE_ERROR_INVALID_STATE when the redo stack is empty.
SonareError sonare_project_redo(SonareProject* project);

#ifdef __cplusplus
}
#endif
