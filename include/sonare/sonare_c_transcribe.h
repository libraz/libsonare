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
/// The tuning reference is the caller's by default: pass @c reference_hz, or
/// convert a measured tuning with @ref sonare_tuning_to_reference_hz. A take
/// recorded away from A440 can instead set @c reference_auto, which measures it
/// from the audio (@ref sonare_estimate_tuning at 12 bins per octave) before
/// any pitch is tracked. @ref SonareTranscribeResult reports the tuning that
/// was used either way, so a wrong transcription can be traced to it.

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
///          @ref sonare_transcribe_config_default initializes the configuration
///          and keeps @c fmin, @c fmax, @c min_note_ms and the version-3 fields
///          at 0, so each resolves from the selected source.
///
///          **A struct field has no way to spell "absent", which is why 0 means
///          "default" here and does NOT mean that on the bindings.** Python,
///          Node and WASM each have a spelling for absence -- an omitted keyword
///          or an omitted request field -- so a 0 written there carries no
///          meaning except a value the field's domain excludes, and all three
///          refuse it by name for @c reference_hz, @c fmin, @c fmax,
///          @c min_note_ms, @c min_note_division,
///          @c segmentation_threshold_cents, @c velocity_floor_db and
///          @c fixed_velocity. @c group and
///          @c channel keep accepting 0 everywhere, because there 0 is a value
///          a caller can mean. The divergence is deliberate and is the reason
///          the bindings do not simply forward this struct.
typedef struct {
  /// 1, 2 or 3; any other value is rejected. Version 2 adds @c reference_auto
  /// and version 3 the four polyphonic limits and @c min_note_division; each
  /// is read only from a struct of its version or later.
  int32_t struct_version;
  /// Non-zero reads the multi-F0 chain, which finds overlapping notes at the
  /// cost of a full STFT and a mask per tracked ridge. 0 reads pYIN cut into
  /// notes, which follows one line at a time.
  int32_t polyphonic;
  /// Tuning reference the MIDI note numbers are measured against; 0 => 440.
  float reference_hz;
  /// F0 tracker range in Hz. A zero endpoint uses the selected source's
  /// default: 65..2093 Hz for the monophonic path, or 55..1760 Hz for the
  /// polyphonic path. Both use the resolved bounds. The polyphonic path applies
  /// them to its salience estimator's F0 axis; its cent-spectrum bounds remain
  /// independent.
  float fmin;
  float fmax;
  /// Shortest span kept as a note, in milliseconds; 0 => @c min_note_division
  /// if set, else the source's default: 30 for the monophonic path, and for the
  /// polyphonic path, where it is the shortest ridge the tracker keeps, a
  /// thirty-second note at the transcription tempo held within [30, 60].
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
  /* --- struct_version 2 --- */
  /// Non-zero measures the tuning reference from the audio and ignores
  /// @c reference_hz. 0 keeps @c reference_hz.
  int32_t reference_auto;
  /* --- struct_version 3 --- */
  /// The four fields below are polyphonic only: with @c polyphonic at 0, any
  /// of them non-zero, negative included, is refused as INVALID_PARAMETER
  /// naming the field. Each 0 selects the transcription default, which is not
  /// the editing chain's (@ref SonarePolyphonicConfig).
  ///
  /// Voices one frame may hold, at most 64; 0 => the transcription default (10).
  int32_t max_polyphony;
  /// Stops a frame's search below this share of its first peak, at most 1;
  /// 0 => 0.20, negative => 0.
  float min_frame_peak_ratio;
  /// Ends a ridge below this share of its own running peak, at most 1;
  /// 0 => 0.10, negative => 0.
  float min_ridge_peak_ratio;
  /// Splits a ridge where its salience climbs past this multiple of the level
  /// just before, the same pitch struck again while it sounds. 0 => the
  /// transcription default (2.0); negative => no split; otherwise finite and
  /// above 1.
  float reattack_ratio;
  /// Shortest span kept as a note, as a note value on either path: n is a 1/n
  /// note at the transcription tempo (32 => a thirty-second note), 1..128.
  /// 0 => unset. Refused together with a non-zero @c min_note_ms. The tempo
  /// is the one the events are placed on: the given or detected one for
  /// @ref sonare_transcribe, the project's at its start for a clip.
  int32_t min_note_division;
} SonareTranscribeConfig;

/// @brief Heap-owned transcription output. Release with
///        @ref sonare_free_transcribe_result.
typedef struct {
  /// 2; written by the function that fills the result.
  int32_t struct_version;
  /// Tuning the MIDI note numbers were measured against, in fractions of a
  /// semitone from concert A440: the measured value under @c reference_auto,
  /// otherwise the one @c reference_hz amounts to
  /// (@ref sonare_reference_hz_to_tuning).
  float tuning;
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
