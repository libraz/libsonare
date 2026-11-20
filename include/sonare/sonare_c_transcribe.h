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

/// Absence of @c min_note_division: no note-value minimum is set. Outside its [1, 128] domain.
#define SONARE_NOTE_DIVISION_UNSET 0
/// Absence of @c fixed_velocity: each note's velocity is mapped from its level. Outside its
/// [1, 127] domain.
#define SONARE_FIXED_VELOCITY_UNSET 0
/// Absence of @c max_polyphony: the transcription default (10) applies on the polyphonic path.
/// Outside its [1, 64] domain.
#define SONARE_MAX_POLYPHONY_UNSET 0

/// @brief Transcription configuration.
/// @details Start from @ref sonare_transcribe_config_init, which writes the defaults. An optional
///          float is "not given" when it holds NaN and resolves from the selected source; every
///          other value, 0 and negatives included, is checked against its domain and refused by
///          name when outside it. The bindings spell absence as an omitted keyword or request
///          field and refuse a 0 for the same fields.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_transcribe_config_init.
  /// Non-zero reads the multi-F0 chain, which finds overlapping notes at the
  /// cost of a full STFT and a mask per tracked ridge. 0 (default) reads pYIN cut into
  /// notes, which follows one line at a time. The polyphonic working memory
  /// is about 0.75 MB per second of input at any rate, plus about 3 MB, plus
  /// about 32 bytes per sample of the longest note while it is measured.
  int32_t polyphonic;
  /// Tuning reference the MIDI note numbers are measured against, in Hz; positive. Default 440.
  float reference_hz;
  /// F0 tracker range in Hz; positive. NaN (the default) takes the selected source's default
  /// lower end: 65 Hz for the monophonic path, 55 Hz for the polyphonic path. Both paths use the
  /// resolved bounds. The polyphonic path applies them to its salience estimator's F0 axis; its
  /// cent-spectrum bounds remain independent.
  float f0_min_hz;
  /// Upper end of the F0 tracker range in Hz, greater than @c f0_min_hz; NaN (the default) takes
  /// the source's default: 2093 Hz monophonic, 1760 Hz polyphonic.
  float f0_max_hz;
  /// Shortest span kept as a note, in milliseconds; positive. NaN (the default) takes
  /// @c min_note_division if given, else the source's default: 30 for the monophonic segments
  /// model, 45 for the monophonic singing model, and for the polyphonic path, where it is the
  /// shortest ridge the tracker keeps, 60 whatever the tempo.
  float min_note_ms;
  /// Monophonic pitch threshold in cents; positive. Default 50. The segments model ends one
  /// note and starts the next where the pitch moves this far; the singing model
  /// reads it as the scale of the movement kept within one note.
  float segmentation_threshold_cents;
  /// Level mapped to velocity 1; negative. Default -48. A note's PEAK per-frame RMS of the input
  /// over its span (other notes sounding then included) is taken in dBFS and mapped
  /// linearly from [velocity_floor_db, 0] onto [1, 127], clamped at both ends.
  /// It does not drop notes: a note more than 36 dB
  /// under the take's loudest notes, or under -80 dBFS, is dropped whatever it is.
  float velocity_floor_db;
  /// 1..127 gives every note that velocity instead of the mapped one;
  /// @ref SONARE_FIXED_VELOCITY_UNSET (the default) maps the level. The level gate still applies.
  /// Anything else is rejected.
  int32_t fixed_velocity;
  /// UMP group the events are emitted on, 0..15. Default 0.
  int32_t group;
  /// MIDI channel of the emitted events, 0..15 (see @c group). Default 0.
  int32_t channel;
  /// Non-zero measures the tuning reference from the audio and ignores
  /// @c reference_hz. 0 (default) keeps @c reference_hz.
  int32_t reference_auto;
  /// The four fields below are polyphonic only: with @c polyphonic at 0, any
  /// of them given is refused as INVALID_PARAMETER naming the field. Each absence
  /// selects the transcription default, which is not the editing chain's
  /// (@ref SonarePolyphonicConfig).
  ///
  /// Voices one frame may hold, 1..64; @ref SONARE_MAX_POLYPHONY_UNSET (the default) takes 10.
  int32_t max_polyphony;
  /// Stops a frame's search below this share of its first peak, in [0, 1]; NaN (the default)
  /// takes 0.20.
  float min_frame_peak_ratio;
  /// Ends a ridge below this share of its own running peak, in [0, 1]; NaN (the default) takes
  /// 0.10.
  float min_ridge_peak_ratio;
  /// Splits a ridge where its salience climbs past this multiple of the level
  /// just before, the same pitch struck again while it sounds; finite and above 1. NaN (the
  /// default) does not split (as @ref SonarePolyphonicConfig).
  float reattack_ratio;
  /// A @ref SonareNoteDivision giving the shortest span kept as a note, on either path: n is a 1/n
  /// note at the transcription tempo (32 => a thirty-second note), 1..128.
  /// @ref SONARE_NOTE_DIVISION_UNSET (the default) sets none. Refused together with a given
  /// @c min_note_ms. The tempo is the one the events are placed on: the given or detected one for
  /// @ref sonare_transcribe, the project's at its start for a clip.
  int32_t min_note_division;
  /// Monophonic segmentation model. 0 (segments, the default) cuts the pitch
  /// track where it moves and splits brief RMS dips, accurate on instrument
  /// lines including short notes. 1 (singing) decodes notes jointly over pitch
  /// bins, holding vibrato and intonation drift within a note; it can merge
  /// very short notes at fast tempos. Any other value is rejected, and with
  /// @c polyphonic non-zero a non-zero value is refused naming the field.
  int32_t monophonic_model;
} SonareTranscribeConfig;

/// @brief Fills @p config with sizeof and the transcription defaults. A NULL @p config is
///        ignored.
void sonare_transcribe_config_init(SonareTranscribeConfig* config);

/// @brief Heap-owned transcription output. Release with
///        @ref sonare_free_transcribe_result.
typedef struct {
  /// Tuning the MIDI note numbers were measured against, in cents from
  /// concert A440: the measured value under @c reference_auto, otherwise the
  /// one @c reference_hz amounts to (@ref sonare_reference_hz_to_tuning).
  float tuning_cents;
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

/// @brief Transcribes mono audio into MIDI events on a constant-tempo grid.
/// @details Finding no notes is not an error: silence, and material the chain
///          cannot resolve, come back as NULL events with zero counts, and
///          @c tempo_bpm still reports the tempo that was used or detected.
/// @param samples Mono source; @p length must be non-zero.
/// @param tempo_bpm The tempo the PPQ grid is built on. Pass a value <= 0 to
///        have it detected from @p samples, which costs an onset/tempo pass.
///        It is also the tempo @c min_note_division is read at, so a grid tempo
///        that is not the music's wants @c min_note_ms instead.
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
