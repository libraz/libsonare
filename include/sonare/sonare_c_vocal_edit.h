#pragma once

/// @file sonare_c_vocal_edit.h
/// @brief Offline, non-destructive editing of monophonic vocal notes.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief ABI version of the vocal-edit structs and functions in this header; raised when a
///        layout or signature changes, and reported by sonare_vocal_edit_abi_version.
#define SONARE_VOCAL_EDIT_ABI_VERSION 1u

/// @brief Opaque editing session over one mono source; destroy with sonare_vocal_session_destroy.
typedef struct SonareVocalSession SonareVocalSession;
/// @brief Opaque uncommitted edit draft of a session; destroy with sonare_vocal_draft_destroy.
typedef struct SonareVocalDraft SonareVocalDraft;
/// @brief Opaque immutable render snapshot; destroy with sonare_vocal_snapshot_destroy.
typedef struct SonareVocalSnapshot SonareVocalSnapshot;
/// @brief Opaque incremental render job; destroy with sonare_vocal_render_job_destroy.
typedef struct SonareVocalRenderJob SonareVocalRenderJob;

/// @brief How a note's target pitch is defined.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_TARGET_NONE = 0,    ///< No target; pitch is only transposed or scaled.
  SONARE_VOCAL_TARGET_CENTER = 1,  ///< Pull the note toward the constant @c target_midi.
  SONARE_VOCAL_TARGET_CURVE = 2    ///< Pull the note toward the @c target_points curve.
} SonareVocalTargetMode;

/// @brief How a note's formants follow its pitch edit.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_FORMANT_PRESERVE = 0,  ///< Keep the original formants; the shift must be 0.
  SONARE_VOCAL_FORMANT_SHIFT = 1      ///< Shift formants by @c formant_shift_semitones.
} SonareVocalFormantMode;

/// @brief Kind of a @ref SonareVocalOperation; selects which of its fields are read.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_SET_EDIT = 0,         ///< Replace a note's edit; reads note_id and edit.
  SONARE_VOCAL_SET_SOURCE_SPAN = 1,  ///< Move a note's source span and its destination span.
  SONARE_VOCAL_SPLIT = 2,            ///< Split a note at source_sample; reads note_id.
  SONARE_VOCAL_MERGE = 3,            ///< Merge note_ids into one note; reads policy.
  SONARE_VOCAL_SET_TRANSITION = 4,   ///< Add or replace a pitch transition; reads transition.
  SONARE_VOCAL_REMOVE_TRANSITION =
      5,                  ///< Remove the transition between the two transition note ids.
  SONARE_VOCAL_RESET = 6  ///< Restore note_ids to their unedited state.
} SonareVocalOperationKind;

/// @brief What a merge does with the merged notes' edits.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_MERGE_PRESERVE = 0,  ///< Keep the notes' edits; they must be compatible.
  SONARE_VOCAL_MERGE_RESET = 1      ///< Discard the edits of the merged notes.
} SonareVocalMergePolicy;

/// @brief Machine-readable cause of the last failed vocal-edit call.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_REASON_NONE = 0,               ///< The last call succeeded.
  SONARE_VOCAL_REASON_INVALID_INPUT = 1,      ///< An argument or struct field was refused.
  SONARE_VOCAL_REASON_REVISION_CONFLICT = 2,  ///< The expected revision or generation is stale.
  SONARE_VOCAL_REASON_SOURCE_MISMATCH = 3,    ///< The source audio does not match the state.
  SONARE_VOCAL_REASON_UNSUPPORTED = 4,        ///< The request or the feature is not supported.
  SONARE_VOCAL_REASON_CANCELLED = 5,          ///< A cancel callback or abort stopped the call.
  SONARE_VOCAL_REASON_COUNTER_EXHAUSTED = 6,  ///< An internal id or revision counter ran out.
  SONARE_VOCAL_REASON_INVALID_STATE = 7       ///< The object is in a state that refuses the call.
} SonareVocalReason;

/// @brief One point of a target pitch curve.
typedef struct {
  double source_sample;  ///< Position in source samples.
  double midi;           ///< Target pitch as a MIDI note number (fractional allowed).
} SonareVocalPitchPoint;

/// @brief Half-open sample interval [start_sample, end_sample).
typedef struct {
  int64_t start_sample;  ///< First sample of the interval.
  int64_t end_sample;    ///< One past the last sample of the interval.
} SonareVocalRange;

/// @brief Caller-supplied pitch analysis passed to sonare_vocal_session_create.
///   Pointer members are borrowed from the caller and copied during the call; the library
///   does not retain them, so this struct does not transfer ownership.
typedef struct {
  uint32_t struct_size;           ///< sizeof of this struct; set by the matching init function.
  double frame_origin_sample;     ///< Source sample at which frame 0 is positioned.
  double samples_per_frame;       ///< Hop between frames, in source samples (default 512).
  uint32_t frame_length_samples;  ///< Analysis window length in samples (default 2048).
  const float* f0_hz;             ///< frame_count fundamentals in Hz.
  const uint8_t* voiced;          ///< frame_count flags; nonzero marks a voiced frame.
  size_t frame_count;             ///< Number of entries in f0_hz and voiced.
  const char*
      algorithm_id;  ///< NUL-terminated analyzer name, fewer than 256 bytes (default "host").
  uint32_t algorithm_version;  ///< Version of the analyzer named by algorithm_id.
  double fmin_hz;              ///< Lowest fundamental searched, in Hz.
  double fmax_hz;              ///< Highest fundamental searched, in Hz.
  double yin_threshold;        ///< YIN absolute threshold used by the analyzer.
  double voiced_threshold;  ///< Voicing probability threshold used by the analyzer (default 0.5).
  uint32_t centered;        ///< Nonzero if frames are centered on their positions; only 0 or 1.
  double segmentation_threshold_cents;  ///< Pitch deviation in cents that starts a new note
                                        ///< (default 50).
  double min_note_ms;                   ///< Shortest note, in milliseconds (default 30).
  double reference_hz;                  ///< Frequency of MIDI note 69 in Hz (default 440).
} SonareVocalAnalysis;

/// @brief Session creation options; @c analysis is optional and may be NULL.
///   Pointer members are borrowed from the caller and copied during the call; the library
///   does not retain them, so this struct does not transfer ownership.
typedef struct {
  uint32_t struct_size;           ///< sizeof of this struct; set by the matching init function.
  int64_t output_length_samples;  ///< Output length in samples; 0 selects the source length.
  double edge_fade_ms;            ///< Fade applied at note edges, in milliseconds (default 5).
  double vibrato_cutoff_hz;       ///< Cutoff separating vibrato from drift, in Hz (default 3).
  double segmentation_threshold_cents;  ///< Pitch deviation in cents that starts a new note
                                        ///< (default 50).
  double min_note_ms;                   ///< Shortest note, in milliseconds (default 30).
  uint32_t frame_length_samples;        ///< Pitch analysis window length in samples (default 2048).
  uint32_t hop_length_samples;          ///< Pitch analysis hop in samples (default 512).
  double fmin_hz;                       ///< Lowest fundamental searched, in Hz.
  double fmax_hz;                       ///< Highest fundamental searched, in Hz.
  uint64_t max_history_bytes;  ///< Memory budget of the undo history in bytes (default 64 MiB).
  uint64_t max_cache_bytes;    ///< Memory budget of the render cache in bytes (default 128 MiB).
  uint32_t max_undo_depth;     ///< Maximum number of undo steps kept (default 128).
  uint32_t max_render_jobs;    ///< Maximum concurrent render jobs (default 4).
  const SonareVocalAnalysis* analysis;  ///< Host-supplied analysis; NULL runs pYIN once.
  double yin_threshold;                 ///< YIN absolute threshold of the built-in analysis.
  double voiced_threshold;              ///< Voicing probability threshold (default 0.5).
  uint32_t centered;                    ///< Nonzero if analysis frames are centered; only 0 or 1.
  double reference_hz;                  ///< Frequency of MIDI note 69 in Hz (default 440).
} SonareVocalCreateOptions;

/// @brief Runtime limits for a restored session; the authored state carries everything else.
typedef struct {
  uint32_t struct_size;        ///< sizeof of this struct; set by the matching init function.
  uint64_t max_history_bytes;  ///< Memory budget of the undo history in bytes (default 64 MiB).
  uint64_t max_cache_bytes;    ///< Memory budget of the render cache in bytes (default 128 MiB).
  uint32_t max_undo_depth;     ///< Maximum number of undo steps kept (default 128).
  uint32_t max_render_jobs;    ///< Maximum concurrent render jobs (default 4).
} SonareVocalRestoreOptions;

/// @brief Per-note edit parameters.
///   Pointer members are borrowed from the caller and copied during the call; the library
///   does not retain them, so this struct does not transfer ownership.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by the matching init function.
  uint32_t target_mode;  ///< A @ref SonareVocalTargetMode choosing how the target is given.
  double target_midi;    ///< Target pitch as a MIDI note number for CENTER mode.
  const SonareVocalPitchPoint* target_points;  ///< target_point_count curve points for CURVE mode.
  size_t target_point_count;                   ///< Number of entries in target_points.
  double amount;                       ///< Correction strength toward the target, in [0, 1].
  double speed_ms;                     ///< Correction smoothing time constant in ms; 0 is instant.
  double max_correction_semitones;     ///< Largest correction applied, in semitones; at least 0.
  double transpose_semitones;          ///< Pitch shift added to the note, in semitones.
  double drift_scale;                  ///< Scale of the slow pitch drift (1 keeps it).
  double vibrato_scale;                ///< Scale of the vibrato depth (1 keeps it).
  int64_t destination_start_sample;    ///< Start of the note in the output, in samples.
  int64_t destination_length_samples;  ///< Length of the note in the output, in samples.
  double gain_db;                      ///< Gain applied to the note, in dB.
  uint32_t muted;                      ///< Nonzero silences the note; only 0 or 1.
  uint32_t formant_mode;               ///< A @ref SonareVocalFormantMode choosing formant handling.
  double formant_shift_semitones;      ///< Formant shift in semitones; limits in capabilities.
  const float* amplitude_envelope;     ///< amplitude_envelope_count non-negative gain factors.
  size_t amplitude_envelope_count;     ///< Number of entries in amplitude_envelope; 0 means none.
} SonareVocalNoteEdit;

/// @brief A detected note with its current edit.
///   @c amplitude and the edit's arrays are library-allocated; release them through
///   sonare_free_vocal_notes_result on the owning @ref SonareVocalNotesResult.
typedef struct {
  uint32_t struct_size;           ///< sizeof of this struct; set by the matching init function.
  uint32_t id;                    ///< Note id; never 0.
  int64_t source_start_sample;    ///< First source sample of the note.
  int64_t source_end_sample;      ///< One past the last source sample of the note.
  uint64_t analysis_frame_start;  ///< First analysis frame of the note.
  uint64_t analysis_frame_end;    ///< One past the last analysis frame of the note.
  uint32_t has_pitch;        ///< Nonzero if the note is pitched; otherwise the pitch fields are 0.
  double median_hz;          ///< Median fundamental in Hz.
  double center_midi;        ///< Center pitch as a MIDI note number.
  double f0_stability;       ///< Pitch steadiness in [0, 1]; 1 is steadiest.
  const float* amplitude;    ///< amplitude_count per-frame amplitudes of the note.
  size_t amplitude_count;    ///< Number of entries in amplitude.
  SonareVocalNoteEdit edit;  ///< Current edit; its arrays are library-allocated.
} SonareVocalNote;

/// @brief Pitch blend across the boundary between two adjacent notes.
typedef struct {
  uint32_t struct_size;          ///< sizeof of this struct; set by the matching init function.
  uint32_t left_note_id;         ///< Id of the earlier note.
  uint32_t right_note_id;        ///< Id of the later note.
  int64_t left_window_samples;   ///< Blend length inside the left note, in samples; positive.
  int64_t right_window_samples;  ///< Blend length inside the right note, in samples; positive.
  double strength;               ///< Blend strength in [0, 1].
} SonareVocalTransition;

/// @brief Versioned tagged operation; only the selected kind's fields are read.
///   Pointer members are borrowed from the caller and copied during the call; the library
///   does not retain them, so this struct does not transfer ownership.
typedef struct {
  uint32_t struct_size;              ///< sizeof of this struct; set by the matching init function.
  uint32_t kind;                     ///< A @ref SonareVocalOperationKind selecting the operation.
  uint32_t note_id;                  ///< Target note of SET_EDIT, SET_SOURCE_SPAN and SPLIT.
  SonareVocalNoteEdit edit;          ///< New edit for SET_EDIT.
  int64_t source_start_sample;       ///< New source span start for SET_SOURCE_SPAN.
  int64_t source_end_sample;         ///< New source span end for SET_SOURCE_SPAN.
  int64_t destination_start_sample;  ///< New destination start for SET_SOURCE_SPAN.
  int64_t destination_length_samples;  ///< New destination length for SET_SOURCE_SPAN.
  int64_t source_sample;               ///< Source sample at which SPLIT cuts the note.
  const uint32_t* note_ids;            ///< note_id_count ids for MERGE and RESET.
  size_t note_id_count;                ///< Number of entries in note_ids.
  uint32_t policy;                     ///< A @ref SonareVocalMergePolicy for MERGE.
  SonareVocalTransition
      transition;  ///< Transition for SET_TRANSITION; REMOVE_TRANSITION reads its note ids.
} SonareVocalOperation;

/// @brief Identifies the exact session state a result was produced from.
typedef struct {
  uint64_t session_epoch;  ///< Identifies the session instance.
  uint64_t revision;       ///< Committed revision of the session.
  uint64_t draft_id;       ///< Draft the state belongs to; 0 outside a draft.
  uint64_t generation;     ///< Draft generation, advanced by each apply; 0 outside a draft.
  uint64_t request_id;     ///< Caller request id of a render; 0 for non-render results.
  uint32_t profile_id;     ///< Render profile id; see @ref SonareVocalCapabilities.
} SonareVocalStateToken;

/// @brief Notes and transitions of a session or draft.
///   Library-allocated arrays; release them with sonare_free_vocal_notes_result, which also resets
///   the struct.
typedef struct {
  uint32_t struct_size;    ///< sizeof of this struct; set by the matching init function.
  SonareVocalNote* notes;  ///< note_count notes.
  size_t note_count;       ///< Number of entries in notes.
  SonareVocalTransition* transitions;  ///< transition_count transitions.
  size_t transition_count;             ///< Number of entries in transitions.
} SonareVocalNotesResult;

/// @brief The analysis a session holds, with digests of its source and of the analysis.
///   Library-allocated arrays; release them with sonare_free_vocal_analysis_result, which also
///   resets the struct.
typedef struct {
  uint32_t struct_size;           ///< sizeof of this struct; set by the matching init function.
  SonareVocalAnalysis analysis;   ///< Analysis grid, F0 track and settings; library-owned arrays.
  char source_sha256[65];         ///< Lowercase hex SHA-256 of the source, NUL-terminated.
  char analysis_sha256[65];       ///< Lowercase hex SHA-256 of the analysis, NUL-terminated.
  int64_t source_length_samples;  ///< Source length in samples.
  int sample_rate;                ///< Source sample rate in Hz.
} SonareVocalAnalysisResult;

/// @brief Note ids replaced by one applied operation.
typedef struct {
  uint32_t operation_index;  ///< Index of the operation in the applied array.
  uint32_t retired_id;       ///< Id no longer in use.
  uint32_t first_new_id;     ///< First id that replaced it; 0 if none.
  uint32_t second_new_id;    ///< Second id that replaced it; 0 if none.
} SonareVocalIdChange;

/// @brief Outcome of an edit, undo, redo or commit.
///   Library-allocated arrays; release them with sonare_free_vocal_edit_result, which also resets
///   the struct.
typedef struct {
  uint32_t struct_size;             ///< sizeof of this struct; set by the matching init function.
  SonareVocalStateToken token;      ///< State after the call.
  SonareVocalRange* dirty_ranges;   ///< Ranges whose rendered audio changed.
  size_t dirty_range_count;         ///< Number of entries in dirty_ranges.
  SonareVocalIdChange* id_changes;  ///< Note ids retired and created; only set by apply.
  size_t id_change_count;           ///< Number of entries in id_changes.
} SonareVocalEditResult;

/// @brief Per-frame pitch evaluation of one note.
///   Library-allocated arrays; release them with sonare_free_vocal_pitch_result, which also resets
///   the struct.
typedef struct {
  uint32_t struct_size;    ///< sizeof of this struct; set by the matching init function.
  double* source_samples;  ///< frame_count frame positions, in source samples.
  double* measured_midi;   ///< frame_count measured pitches as MIDI note numbers.
  double* target_midi;     ///< frame_count target pitches as MIDI note numbers.
  double* effective_midi;  ///< frame_count pitches after the edit as MIDI note numbers.
  uint8_t* voiced;         ///< frame_count flags; nonzero marks a voiced frame.
  uint8_t* has_target;     ///< frame_count flags; nonzero where target_midi is defined.
  size_t frame_count;      ///< Number of entries in each array.
} SonareVocalPitchResult;

/// @brief Rendered PCM and the ranges it covers.
///   Library-allocated arrays; release them with sonare_free_vocal_render_result, which also resets
///   the struct.
typedef struct {
  uint32_t struct_size;         ///< sizeof of this struct; set by the matching init function.
  float* samples;               ///< sample_count mono samples.
  size_t sample_count;          ///< Number of entries in samples.
  int64_t start_sample;         ///< Output sample at which samples begins.
  SonareVocalStateToken token;  ///< State rendered; request_id echoes the caller's.
  SonareVocalRange* processed_ranges;  ///< Ranges of the output covered by this render.
  size_t processed_range_count;        ///< Number of entries in processed_ranges.
  uint64_t cache_hit_units;            ///< Render units served from the cache.
  uint64_t dry_passed_frames;          ///< Voiced frames left unshifted: target pitch out of range.
  uint64_t limited_correction_frames;  ///< Frames whose correction was clamped to the maximum.
} SonareVocalRenderResult;

/// @brief Serialized session state.
///   Library-allocated arrays; release them with sonare_free_vocal_state_bytes, which also resets
///   the struct.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by the matching init function.
  uint8_t* data;         ///< size bytes of state; pass to sonare_vocal_session_restore.
  uint64_t size;         ///< Number of bytes in data.
} SonareVocalStateBytes;

/// @brief What the session's renderer supports.
typedef struct {
  uint32_t struct_size;           ///< sizeof of this struct; set by the matching init function.
  uint32_t abi_version;           ///< Vocal-edit ABI version of the library.
  uint32_t profile_id;            ///< Render profile id; 1 is the PSOLA vocal profile.
  uint32_t monophonic_only;       ///< Nonzero if only monophonic sources are supported.
  uint32_t analysis_cancellable;  ///< Nonzero if analysis can be cancelled.
  double minimum_formant_shift_semitones;  ///< Lowest accepted formant shift, in semitones.
  double maximum_formant_shift_semitones;  ///< Highest accepted formant shift, in semitones.
} SonareVocalCapabilities;

/// @brief Structured context of the last failed vocal-edit call on the calling thread.
typedef struct {
  uint32_t struct_size;    ///< sizeof of this struct; set by the matching init function.
  uint32_t reason;         ///< A @ref SonareVocalReason naming the cause.
  char field[96];          ///< Name of the offending field; empty if none.
  uint64_t expected;       ///< Numeric form of expected_text; 0 if it is not a number.
  uint64_t actual;         ///< Numeric form of actual_text; 0 if it is not a number.
  char expected_text[96];  ///< Expected value as text; empty if none.
  char actual_text[96];    ///< Offending value as text; empty if none.
} SonareVocalErrorDetail;

/// @brief Nonzero requests cancellation; user_data is used only during the call.
typedef int (*SonareVocalCancelCallback)(void* user_data);

/// @brief Returns SONARE_VOCAL_EDIT_ABI_VERSION as compiled into the library.
uint32_t sonare_vocal_edit_abi_version(void);
/// @brief Sets @p analysis to its defaults (pitch defaults, algorithm id "host", no frames).
/// @param analysis Struct to initialize; NULL is ignored.
void sonare_vocal_analysis_init(SonareVocalAnalysis* analysis);
/// @brief Sets @p options to the defaults noted on each field; NULL is ignored.
void sonare_vocal_create_options_init(SonareVocalCreateOptions* options);
/// @brief Sets @p options to the default runtime limits; NULL is ignored.
void sonare_vocal_restore_options_init(SonareVocalRestoreOptions* options);
/// @brief Sets @p edit to an identity edit (no target, no gain, 12 semitone correction limit).
void sonare_vocal_note_edit_init(SonareVocalNoteEdit* edit);
/// @brief Sets @p operation to defaults, including an initialized nested edit and transition.
void sonare_vocal_operation_init(SonareVocalOperation* operation);
/// @brief Initializes an output struct before use; any owned data must be freed first.
void sonare_vocal_notes_result_init(SonareVocalNotesResult* result);
/// @brief Initializes an @ref SonareVocalAnalysisResult before use; any owned data must be freed
/// first.
void sonare_vocal_analysis_result_init(SonareVocalAnalysisResult* result);
/// @brief Initializes an @ref SonareVocalEditResult before use; any owned data must be freed first.
void sonare_vocal_edit_result_init(SonareVocalEditResult* result);
/// @brief Initializes an @ref SonareVocalPitchResult before use; any owned data must be freed
/// first.
void sonare_vocal_pitch_result_init(SonareVocalPitchResult* result);
/// @brief Initializes an @ref SonareVocalRenderResult before use; any owned data must be freed
/// first.
void sonare_vocal_render_result_init(SonareVocalRenderResult* result);
/// @brief Initializes an @ref SonareVocalStateBytes before use; any owned data must be freed first.
void sonare_vocal_state_bytes_init(SonareVocalStateBytes* result);
/// @brief Initializes an @ref SonareVocalCapabilities before use; any owned data must be freed
/// first.
void sonare_vocal_capabilities_init(SonareVocalCapabilities* result);
/// @brief Initializes an @ref SonareVocalErrorDetail before use; any owned data must be freed
/// first.
void sonare_vocal_error_detail_init(SonareVocalErrorDetail* result);
/// @brief Copies the detail of the calling thread's last failed vocal-edit call into @p detail.
///   @p detail must be initialized with sonare_vocal_error_detail_init; otherwise it is left
///   unchanged.
void sonare_vocal_last_error_detail(SonareVocalErrorDetail* detail);

/// @brief Creates a session over mono source PCM; @p sample_rate must lie in [8000, 384000] Hz.
/// @param out_session Receives the session, or NULL on failure. Release it with
///   sonare_vocal_session_destroy, which accepts NULL.
SonareError sonare_vocal_session_create(const float* samples, int64_t frames, int channels,
                                        int sample_rate, const SonareVocalCreateOptions* options,
                                        SonareVocalSession** out_session);
/// @brief Destroys a session; NULL is ignored.
void sonare_vocal_session_destroy(SonareVocalSession* session);
/// @brief Reads the session's notes and transitions into @p result; free with
///   sonare_free_vocal_notes_result.
SonareError sonare_vocal_session_notes(const SonareVocalSession* session,
                                       SonareVocalNotesResult* result);
/// @brief Reads the draft's notes and transitions into @p result; free with
///   sonare_free_vocal_notes_result.
SonareError sonare_vocal_draft_notes(const SonareVocalDraft* draft, SonareVocalNotesResult* result);
/// @brief Reads the session's current state token.
SonareError sonare_vocal_session_token(const SonareVocalSession* session,
                                       SonareVocalStateToken* token);
/// @brief Reads the draft's current state token.
SonareError sonare_vocal_draft_token(const SonareVocalDraft* draft, SonareVocalStateToken* token);
/// @brief Reads the session's analysis into @p result; free with sonare_free_vocal_analysis_result.
SonareError sonare_vocal_session_analysis(const SonareVocalSession* session,
                                          SonareVocalAnalysisResult* result);
/// @brief Reads the renderer capabilities into @p result.
SonareError sonare_vocal_session_capabilities(const SonareVocalSession* session,
                                              SonareVocalCapabilities* result);
/// @brief Reads the committed revision number of the session.
SonareError sonare_vocal_session_revision(const SonareVocalSession* session, uint64_t* revision);
/// @brief Reads the session's output length in samples.
/// @param samples Receives the length.
SonareError sonare_vocal_session_output_length(const SonareVocalSession* session, int64_t* samples);
/// @brief Reports whether undo and redo are available.
/// @param can_undo Receives 1 or 0.
/// @param can_redo Receives 1 or 0.
SonareError sonare_vocal_session_history(const SonareVocalSession* session, int* can_undo,
                                         int* can_redo);
/// @brief Opens a draft over the session at @p expected_revision.
/// @param out_draft Receives the draft, or NULL on failure. Release it with
///   sonare_vocal_draft_destroy, which accepts NULL.
SonareError sonare_vocal_session_begin_edit(SonareVocalSession* session, uint64_t expected_revision,
                                            SonareVocalDraft** out_draft);
/// @brief Applies @p count operations to the draft.
/// @param expected_generation Draft generation the caller last saw; a stale value is refused.
/// @param result Receives the new token, dirty ranges and id changes; free with
///   sonare_free_vocal_edit_result.
SonareError sonare_vocal_draft_apply(SonareVocalDraft* draft, uint64_t expected_generation,
                                     const SonareVocalOperation* operations, uint64_t count,
                                     SonareVocalEditResult* result);
/// @brief Publishes the draft as a new revision. A draft never applied closes as a no-op: the
///   result carries the current revision and no dirty ranges, and redo history is kept.
SonareError sonare_vocal_draft_commit(SonareVocalDraft* draft, uint64_t expected_revision,
                                      SonareVocalEditResult* result);
/// @brief Discards the draft's changes; the draft must still be destroyed.
SonareError sonare_vocal_draft_cancel(SonareVocalDraft* draft);
/// @brief Destroys a draft; NULL is ignored.
void sonare_vocal_draft_destroy(SonareVocalDraft* draft);
/// @brief Undoes the last committed edit.
/// @param expected_revision Revision the caller last saw; a stale value is refused.
/// @param result Receives the new token and dirty ranges; free with
///   sonare_free_vocal_edit_result.
SonareError sonare_vocal_session_undo(SonareVocalSession* session, uint64_t expected_revision,
                                      SonareVocalEditResult* result);
/// @brief Redoes the last undone edit.
/// @param expected_revision Revision the caller last saw; a stale value is refused.
/// @param result Receives the new token and dirty ranges; free with
///   sonare_free_vocal_edit_result.
SonareError sonare_vocal_session_redo(SonareVocalSession* session, uint64_t expected_revision,
                                      SonareVocalEditResult* result);
/// @brief Evaluates the per-frame pitch of note @p note_id in the session.
/// @param result Receives the frames; free with sonare_free_vocal_pitch_result.
SonareError sonare_vocal_session_evaluate_pitch(const SonareVocalSession* session, uint32_t note_id,
                                                SonareVocalPitchResult* result);
/// @brief Evaluates the per-frame pitch of note @p note_id in the draft.
/// @param result Receives the frames; free with sonare_free_vocal_pitch_result.
SonareError sonare_vocal_draft_evaluate_pitch(const SonareVocalDraft* draft, uint32_t note_id,
                                              SonareVocalPitchResult* result);
/// @brief Maps a source sample position within note @p note_id to its output position in the
/// session.
/// @param destination_sample Receives the output position in samples.
SonareError sonare_vocal_session_source_to_destination(const SonareVocalSession* session,
                                                       uint32_t note_id, double source_sample,
                                                       double* destination_sample);
/// @brief Maps an output sample position within note @p note_id to its source position in the
/// session.
/// @param source_sample Receives the source position in samples.
SonareError sonare_vocal_session_destination_to_source(const SonareVocalSession* session,
                                                       uint32_t note_id, double destination_sample,
                                                       double* source_sample);
/// @brief Maps a source sample position within note @p note_id to its output position in the draft.
/// @param destination_sample Receives the output position in samples.
SonareError sonare_vocal_draft_source_to_destination(const SonareVocalDraft* draft,
                                                     uint32_t note_id, double source_sample,
                                                     double* destination_sample);
/// @brief Maps an output sample position within note @p note_id to its source position in the
/// draft.
/// @param source_sample Receives the source position in samples.
SonareError sonare_vocal_draft_destination_to_source(const SonareVocalDraft* draft,
                                                     uint32_t note_id, double destination_sample,
                                                     double* source_sample);
/// @brief Captures an immutable render snapshot of the session.
/// @param snapshot Receives the snapshot, or NULL on failure. Release it with
///   sonare_vocal_snapshot_destroy, which accepts NULL.
SonareError sonare_vocal_session_capture_snapshot(const SonareVocalSession* session,
                                                  SonareVocalSnapshot** snapshot);
/// @brief Captures an immutable render snapshot of the draft.
/// @param snapshot Receives the snapshot, or NULL on failure. Release it with
///   sonare_vocal_snapshot_destroy, which accepts NULL.
SonareError sonare_vocal_draft_capture_snapshot(const SonareVocalDraft* draft,
                                                SonareVocalSnapshot** snapshot);
/// @brief Destroys a snapshot; NULL is ignored.
void sonare_vocal_snapshot_destroy(SonareVocalSnapshot* snapshot);
/// @brief Reads the snapshot's output length in samples.
/// @param samples Receives the length.
SonareError sonare_vocal_snapshot_output_length(const SonareVocalSnapshot* snapshot,
                                                int64_t* samples);
/// @brief Renders @p range in one call.
/// @param request_id Caller-chosen id echoed in the result token.
/// @param result Receives the audio; free with sonare_free_vocal_render_result.
/// @param cancel Optional; @p cancel and @p user_data are used only during the call and are not
///   retained afterwards.
SonareError sonare_vocal_snapshot_render(const SonareVocalSnapshot* snapshot,
                                         SonareVocalRange range, uint64_t request_id,
                                         SonareVocalCancelCallback cancel, void* user_data,
                                         SonareVocalRenderResult* result);
/// @brief Starts an incremental render of @p range.
/// @param request_id Caller-chosen id echoed in the result token.
/// @param job Receives the job, or NULL on failure. Release it with
///   sonare_vocal_render_job_destroy, which accepts NULL.
SonareError sonare_vocal_render_job_begin(const SonareVocalSnapshot* snapshot,
                                          SonareVocalRange range, uint64_t request_id,
                                          SonareVocalRenderJob** job);
/// @brief Advances the job by one step.
/// @param complete Receives 1 once the job has finished all work, otherwise 0.
/// @param cancel Optional; @p cancel and @p user_data are used only during the call and are not
///   retained afterwards.
SonareError sonare_vocal_render_job_next(SonareVocalRenderJob* job,
                                         SonareVocalCancelCallback cancel, void* user_data,
                                         int* complete);
/// @brief Collects the finished job's output.
/// @param result Receives the audio; free with sonare_free_vocal_render_result.
/// @param cancel Optional; @p cancel and @p user_data are used only during the call and are not
///   retained afterwards.
SonareError sonare_vocal_render_job_finalize(SonareVocalRenderJob* job,
                                             SonareVocalCancelCallback cancel, void* user_data,
                                             SonareVocalRenderResult* result);
/// @brief Releases a job's partial artifacts. Not synchronized: call it on the thread that drives
///   next/finalize, never concurrently with them; cancel a running call through its callback.
void sonare_vocal_render_job_abort(SonareVocalRenderJob* job);
/// @brief Destroys a job; NULL is ignored.
void sonare_vocal_render_job_destroy(SonareVocalRenderJob* job);
/// @brief Serializes the session's authored state into @p state; free with
///   sonare_free_vocal_state_bytes.
SonareError sonare_vocal_session_export_state(const SonareVocalSession* session,
                                              SonareVocalStateBytes* state);
/// @brief Restores a session from exported state over the same source audio
///   (@p sample_rate in [8000, 384000] Hz).
/// @param session Receives the session, or NULL on failure. Release it with
///   sonare_vocal_session_destroy, which accepts NULL.
/// @param options Runtime limits, or NULL for the defaults sonare_vocal_restore_options_init
///   writes (the same defaults sonare_vocal_create_options_init writes).
SonareError sonare_vocal_session_restore(const float* samples, int64_t frames, int channels,
                                         int sample_rate, const uint8_t* state, uint64_t size,
                                         const SonareVocalRestoreOptions* options,
                                         SonareVocalSession** session);

/// @brief Frees the arrays owned by @p result and resets it; NULL is ignored.
void sonare_free_vocal_notes_result(SonareVocalNotesResult* result);
/// @brief Frees the arrays owned by @p result and resets it; NULL is ignored.
void sonare_free_vocal_analysis_result(SonareVocalAnalysisResult* result);
/// @brief Frees the arrays owned by @p result and resets it; NULL is ignored.
void sonare_free_vocal_edit_result(SonareVocalEditResult* result);
/// @brief Frees the arrays owned by @p result and resets it; NULL is ignored.
void sonare_free_vocal_pitch_result(SonareVocalPitchResult* result);
/// @brief Frees the arrays owned by @p result and resets it; NULL is ignored.
void sonare_free_vocal_render_result(SonareVocalRenderResult* result);
/// @brief Frees the arrays owned by @p state and resets it; NULL is ignored.
void sonare_free_vocal_state_bytes(SonareVocalStateBytes* state);

#ifdef __cplusplus
}
#endif
