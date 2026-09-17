#pragma once

/// @file sonare_c_transcribe.h
/// @brief Audio-to-MIDI transcription: the conversion from measured notes to
///        MIDI events on a musical grid. Included via @ref sonare_c.h.
///
/// This surface joins parts that already exist rather than adding a detector.
/// The note spans and their measured pitch and level come from the monophonic
/// and polyphonic chains; the tempo comes either from the caller or from a
/// project's own tempo map; and the output is @ref SonareMidiEventPod, which is
/// exactly what @ref sonare_project_set_midi_events takes, so nothing is left
/// for the caller to convert.
///
/// Three things are deliberately NOT done here, because the library already
/// does each of them somewhere else and a second implementation would drift
/// from the first:
///   - Quantizing to a grid -- @ref sonare_project_bake_midi_fx's
///     @c quantize_ppq / @c quantize_strength.
///   - Detecting and installing a tempo map -- @ref sonare_project_auto_tempo
///     and its options-bearing siblings.
///   - Annotating key and chords -- @ref sonare_project_annotate_keys and
///     @ref sonare_project_annotate_chords.
///
/// The tuning reference is likewise not measured here. A take recorded away
/// from A440 should have its reference measured first -- run
/// @ref sonare_pitch_pyin and feed its F0 array to @ref sonare_pitch_tuning --
/// and the result passed in as @c reference_hz. Measuring it internally would
/// mean tracking the pitch twice, and it would hide which of the two answers a
/// wrong transcription came from.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_midi.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Versioned transcription configuration.
/// @details Every numeric field takes its documented default at 0, so a
///          zero-filled struct with @c struct_version set is the defaults.
///          Prefer @ref sonare_transcribe_config_default, which fills them in
///          explicitly.
///
///          **A struct field has no way to spell "absent", which is why 0 means
///          "default" here and does NOT mean that on the bindings.** Python,
///          Node and WASM each have a spelling for absence -- an omitted keyword
///          or an omitted request field -- so a 0 written there carries no
///          meaning except a value the field's domain excludes, and all three
///          refuse it by name for @c reference_hz, @c fmin, @c fmax,
///          @c min_note_ms, @c segmentation_threshold_cents,
///          @c velocity_floor_db and @c fixed_velocity. @c group and
///          @c channel keep accepting 0 everywhere, because there 0 is a value
///          a caller can mean. The divergence is deliberate and is the reason
///          the bindings do not simply forward this struct.
typedef struct {
  /// Must be 1. Any other value is rejected.
  int32_t struct_version;
  /// Non-zero reads the multi-F0 chain, which finds overlapping notes at the
  /// cost of a full STFT and a mask per tracked ridge. 0 reads pYIN cut into
  /// notes, which follows one line at a time.
  int32_t polyphonic;
  /// Tuning reference the MIDI note numbers are measured against; 0 => 440.
  float reference_hz;
  /// Monophonic tracker range in Hz; 0 => 65 and 2093 respectively. The
  /// polyphonic chain sets its own range and reads neither.
  float fmin;
  float fmax;
  /// Shortest span kept as a note, in milliseconds; 0 => 30.
  float min_note_ms;
  /// Pitch movement, in cents, that ends one note and starts the next;
  /// 0 => 50.
  float segmentation_threshold_cents;
  /// Level mapped to velocity 1. A note's PEAK per-frame RMS is taken in dBFS
  /// and mapped linearly from [velocity_floor_db, 0] onto [1, 127], clamped at
  /// both ends. Must be negative; 0 => -48.
  float velocity_floor_db;
  /// 1..127 gives every note that velocity and skips the level measurement;
  /// 0 measures. Anything else is rejected.
  int32_t fixed_velocity;
  /// UMP group and MIDI channel the events are emitted on; both 0..15.
  int32_t group;
  int32_t channel;
} SonareTranscribeConfig;

/// @brief Heap-owned transcription output. Release with
///        @ref sonare_free_transcribe_result.
typedef struct {
  /// Note-on / note-off pairs in canonical PPQ order, ready to hand to
  /// @ref sonare_project_set_midi_events. NULL when @c count is 0.
  SonareMidiEventPod* events;
  /// Number of events, which is twice @c note_count.
  size_t count;
  /// Number of notes.
  size_t note_count;
  /// The tempo the PPQ coordinates were built on -- the caller's value when one
  /// was given, and the detected one otherwise.
  float tempo_bpm;
} SonareTranscribeResult;

/// @brief Returns the transcription defaults.
SonareTranscribeConfig sonare_transcribe_config_default(void);

/// @brief Transcribes mono audio into MIDI events on a constant-tempo grid.
/// @details Finding no notes is not an error: silence, and material the chain
///          cannot resolve, come back as NULL events with zero counts, and
///          @c tempo_bpm still reports the tempo that was used or detected.
/// @param samples Mono source; @p length must be non-zero.
/// @param tempo_bpm The tempo the PPQ grid is built on. Pass a value <= 0 to
///        have it detected from @p samples, which costs an onset/tempo pass.
/// @param config Optional; NULL selects the defaults.
/// @param out Receives a heap-owned result, cleared before validation.
/// @note SONARE_ERROR_NOT_SUPPORTED when the library was built without the
///       pitch editor.
SonareError sonare_transcribe(const float* samples, size_t length, int sample_rate, float tempo_bpm,
                              const SonareTranscribeConfig* config, SonareTranscribeResult* out);

/// @brief Releases a transcription result. NULL is a no-op.
void sonare_free_transcribe_result(SonareTranscribeResult* result);

/// @brief Transcribes mono audio straight into a MIDI clip's event list.
/// @details The PPQ grid is the PROJECT's tempo map, so a project whose tempo
///          was installed by @ref sonare_project_auto_tempo transcribes onto
///          that map rather than onto a second, separately detected tempo --
///          which is why this entry takes no tempo argument at all. It replaces
///          the clip's entire event list, exactly as
///          @ref sonare_project_set_midi_events does.
/// @param out_note_count Optional; receives the number of notes written.
/// @note SONARE_ERROR_NOT_SUPPORTED when the library was built without the
///       pitch editor or without the arrangement subsystem.
SonareError sonare_project_transcribe_to_clip(SonareProject* project, uint32_t clip_id,
                                              const float* samples, size_t length, int sample_rate,
                                              const SonareTranscribeConfig* config,
                                              size_t* out_note_count);

#ifdef __cplusplus
}  // extern "C"
#endif
