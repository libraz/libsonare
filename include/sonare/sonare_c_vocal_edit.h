#pragma once

/// @file sonare_c_vocal_edit.h
/// @brief Offline, non-destructive editing of monophonic vocal notes.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SONARE_VOCAL_EDIT_API_VERSION 1u

typedef struct SonareVocalEditSession SonareVocalEditSession;
typedef struct SonareVocalEditDraft SonareVocalEditDraft;
typedef struct SonareVocalRenderSnapshot SonareVocalRenderSnapshot;
typedef struct SonareVocalRenderJob SonareVocalRenderJob;

typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_TARGET_NONE = 0,
  SONARE_VOCAL_TARGET_CENTER = 1,
  SONARE_VOCAL_TARGET_CURVE = 2
} SonareVocalTargetMode;

typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_FORMANT_PRESERVE = 0,
  SONARE_VOCAL_FORMANT_SHIFT = 1
} SonareVocalFormantMode;

typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_SET_EDIT = 0,
  SONARE_VOCAL_SET_SOURCE_SPAN = 1,
  SONARE_VOCAL_SPLIT = 2,
  SONARE_VOCAL_MERGE = 3,
  SONARE_VOCAL_SET_TRANSITION = 4,
  SONARE_VOCAL_REMOVE_TRANSITION = 5,
  SONARE_VOCAL_RESET = 6
} SonareVocalOperationKind;

typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_MERGE_PRESERVE = 0,
  SONARE_VOCAL_MERGE_RESET = 1
} SonareVocalMergePolicy;

typedef enum SONARE_ENUM_BASE {
  SONARE_VOCAL_REASON_NONE = 0,
  SONARE_VOCAL_REASON_INVALID_INPUT = 1,
  SONARE_VOCAL_REASON_REVISION_CONFLICT = 2,
  SONARE_VOCAL_REASON_SOURCE_MISMATCH = 3,
  SONARE_VOCAL_REASON_UNSUPPORTED = 4,
  SONARE_VOCAL_REASON_CANCELLED = 5,
  SONARE_VOCAL_REASON_COUNTER_EXHAUSTED = 6,
  SONARE_VOCAL_REASON_INVALID_STATE = 7
} SonareVocalReason;

typedef struct {
  double source_sample;
  double midi;
} SonareVocalPitchPoint;

typedef struct {
  int64_t start_sample;
  int64_t end_sample;
} SonareVocalRange;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  double frame_origin_sample;
  double samples_per_frame;
  uint32_t frame_length_samples;
  const float* f0_hz;
  const uint8_t* voiced;
  uint64_t frame_count;
  const char* algorithm_id;
  uint32_t algorithm_version;
  double fmin_hz;
  double fmax_hz;
  double yin_threshold;
  double voiced_threshold;
  uint32_t centered;
  double segmentation_threshold_cents;
  double min_note_ms;
  double reference_hz;
} SonareVocalAnalysis;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  int64_t output_length_samples; /* 0 selects the source length. */
  double edge_fade_ms;
  double vibrato_cutoff_hz;
  double segmentation_threshold_cents;
  double min_note_ms;
  uint32_t frame_length_samples;
  uint32_t hop_length_samples;
  double fmin_hz;
  double fmax_hz;
  uint64_t max_history_bytes;
  uint64_t max_cache_bytes;
  uint32_t max_undo_depth;
  uint32_t max_render_jobs;
  const SonareVocalAnalysis* analysis; /* NULL runs pYIN once. */
  double yin_threshold;
  double voiced_threshold;
  uint32_t centered;
  double reference_hz;
} SonareVocalCreateOptions;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t target_mode;
  double target_midi;
  const SonareVocalPitchPoint* target_points;
  uint64_t target_point_count;
  double amount;
  double speed_ms;
  double max_correction_semitones;
  double transpose_semitones;
  double drift_scale;
  double vibrato_scale;
  int64_t destination_start_sample;
  int64_t destination_length_samples;
  double gain_db;
  uint32_t muted;
  uint32_t formant_mode;
  double formant_shift_semitones;
  const float* amplitude_envelope;
  uint64_t amplitude_envelope_count;
} SonareVocalNoteEdit;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t id;
  int64_t source_start_sample;
  int64_t source_end_sample;
  uint64_t analysis_frame_start;
  uint64_t analysis_frame_end;
  uint32_t has_pitch;
  double median_hz;
  double center_midi;
  double f0_stability;
  const float* amplitude;
  uint64_t amplitude_count;
  SonareVocalNoteEdit edit;
} SonareVocalNote;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t left_note_id;
  uint32_t right_note_id;
  int64_t left_window_samples;
  int64_t right_window_samples;
  double strength;
} SonareVocalTransition;

/// @brief Versioned tagged operation; only the selected kind's fields are read.
typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t kind;
  uint32_t note_id;
  SonareVocalNoteEdit edit;
  int64_t source_start_sample;
  int64_t source_end_sample;
  int64_t destination_start_sample;
  int64_t destination_length_samples;
  int64_t cut_source_sample;
  const uint32_t* note_ids;
  uint64_t note_id_count;
  uint32_t merge_policy;
  SonareVocalTransition transition;
} SonareVocalOperation;

typedef struct {
  uint64_t session_epoch;
  uint64_t revision;
  uint64_t draft_id;
  uint64_t generation;
  uint64_t request_id;
  uint32_t profile_id;
} SonareVocalStateToken;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  SonareVocalNote* notes;
  uint64_t note_count;
  SonareVocalTransition* transitions;
  uint64_t transition_count;
} SonareVocalNotesResult;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  SonareVocalAnalysis analysis;
  char source_sha256[65];
  char analysis_sha256[65];
  int64_t source_length_samples;
  uint32_t sample_rate;
} SonareVocalAnalysisResult;

typedef struct {
  uint32_t operation_index;
  uint32_t retired_id;
  uint32_t first_new_id;
  uint32_t second_new_id;
} SonareVocalIdChange;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  SonareVocalStateToken token;
  SonareVocalRange* dirty_ranges;
  uint64_t dirty_range_count;
  SonareVocalIdChange* id_changes;
  uint64_t id_change_count;
} SonareVocalEditResult;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  double* source_samples;
  double* measured_midi;
  double* target_midi;
  double* effective_midi;
  uint8_t* voiced;
  uint8_t* has_target;
  uint64_t frame_count;
} SonareVocalPitchResult;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  float* samples;
  int64_t sample_count;
  int64_t start_sample;
  SonareVocalStateToken token;
  SonareVocalRange* processed_ranges;
  uint64_t processed_range_count;
  uint64_t cache_hit_units;
  uint64_t dry_passed_frames;
  uint64_t limited_correction_frames;
} SonareVocalRenderResult;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint8_t* data;
  uint64_t size;
} SonareVocalStateBytes;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t api_version;
  uint32_t profile_id;
  uint32_t monophonic_only;
  uint32_t analysis_cancellable;
  double minimum_formant_shift_semitones;
  double maximum_formant_shift_semitones;
} SonareVocalCapabilities;

typedef struct {
  uint32_t struct_size;
  uint32_t schema_version;
  uint32_t reason;
  char field[96];
  uint64_t expected;
  uint64_t actual;
  char expected_text[96];
  char actual_text[96];
} SonareVocalErrorDetail;

/// @brief Nonzero requests cancellation; user_data is used only during the call.
typedef int (*SonareVocalCancelCallback)(void* user_data);

uint32_t sonare_vocal_edit_api_version(void);
void sonare_vocal_analysis_init(SonareVocalAnalysis* analysis);
void sonare_vocal_create_options_init(SonareVocalCreateOptions* options);
void sonare_vocal_note_edit_init(SonareVocalNoteEdit* edit);
void sonare_vocal_operation_init(SonareVocalOperation* operation);
/* Initialize outputs before use. Free owned data before reusing an output. */
void sonare_vocal_notes_result_init(SonareVocalNotesResult* result);
void sonare_vocal_analysis_result_init(SonareVocalAnalysisResult* result);
void sonare_vocal_edit_result_init(SonareVocalEditResult* result);
void sonare_vocal_pitch_result_init(SonareVocalPitchResult* result);
void sonare_vocal_render_result_init(SonareVocalRenderResult* result);
void sonare_vocal_state_bytes_init(SonareVocalStateBytes* result);
void sonare_vocal_capabilities_init(SonareVocalCapabilities* result);
void sonare_vocal_error_detail_init(SonareVocalErrorDetail* result);
int sonare_vocal_available(void);
void sonare_vocal_last_error_detail(SonareVocalErrorDetail* detail);

SonareError sonare_vocal_session_create(const float* samples, int64_t frames, int channels,
                                        int sample_rate, const SonareVocalCreateOptions* options,
                                        SonareVocalEditSession** out_session);
void sonare_vocal_session_destroy(SonareVocalEditSession* session);
SonareError sonare_vocal_session_notes(const SonareVocalEditSession* session,
                                       SonareVocalNotesResult* result);
SonareError sonare_vocal_draft_notes(const SonareVocalEditDraft* draft,
                                     SonareVocalNotesResult* result);
SonareError sonare_vocal_session_token(const SonareVocalEditSession* session,
                                       SonareVocalStateToken* token);
SonareError sonare_vocal_draft_token(const SonareVocalEditDraft* draft,
                                     SonareVocalStateToken* token);
SonareError sonare_vocal_session_analysis(const SonareVocalEditSession* session,
                                          SonareVocalAnalysisResult* result);
SonareError sonare_vocal_session_capabilities(const SonareVocalEditSession* session,
                                              SonareVocalCapabilities* result);
SonareError sonare_vocal_session_revision(const SonareVocalEditSession* session,
                                          uint64_t* revision);
SonareError sonare_vocal_session_output_length(const SonareVocalEditSession* session,
                                               int64_t* samples);
SonareError sonare_vocal_session_history(const SonareVocalEditSession* session, int* can_undo,
                                         int* can_redo);
SonareError sonare_vocal_session_begin_edit(SonareVocalEditSession* session,
                                            uint64_t expected_revision,
                                            SonareVocalEditDraft** out_draft);
SonareError sonare_vocal_draft_apply(SonareVocalEditDraft* draft, uint64_t expected_generation,
                                     const SonareVocalOperation* operations, uint64_t count,
                                     SonareVocalEditResult* result);
SonareError sonare_vocal_draft_commit(SonareVocalEditDraft* draft, uint64_t expected_revision,
                                      SonareVocalEditResult* result);
SonareError sonare_vocal_draft_cancel(SonareVocalEditDraft* draft);
void sonare_vocal_draft_destroy(SonareVocalEditDraft* draft);
SonareError sonare_vocal_session_undo(SonareVocalEditSession* session, uint64_t expected_revision,
                                      SonareVocalEditResult* result);
SonareError sonare_vocal_session_redo(SonareVocalEditSession* session, uint64_t expected_revision,
                                      SonareVocalEditResult* result);
SonareError sonare_vocal_session_evaluate_pitch(const SonareVocalEditSession* session,
                                                uint32_t note_id, SonareVocalPitchResult* result);
SonareError sonare_vocal_draft_evaluate_pitch(const SonareVocalEditDraft* draft, uint32_t note_id,
                                              SonareVocalPitchResult* result);
SonareError sonare_vocal_session_source_to_destination(const SonareVocalEditSession* session,
                                                       uint32_t note_id, double source_sample,
                                                       double* destination_sample);
SonareError sonare_vocal_session_destination_to_source(const SonareVocalEditSession* session,
                                                       uint32_t note_id, double destination_sample,
                                                       double* source_sample);
SonareError sonare_vocal_draft_source_to_destination(const SonareVocalEditDraft* draft,
                                                     uint32_t note_id, double source_sample,
                                                     double* destination_sample);
SonareError sonare_vocal_draft_destination_to_source(const SonareVocalEditDraft* draft,
                                                     uint32_t note_id, double destination_sample,
                                                     double* source_sample);
SonareError sonare_vocal_session_capture_snapshot(const SonareVocalEditSession* session,
                                                  SonareVocalRenderSnapshot** snapshot);
SonareError sonare_vocal_draft_capture_snapshot(const SonareVocalEditDraft* draft,
                                                SonareVocalRenderSnapshot** snapshot);
void sonare_vocal_snapshot_destroy(SonareVocalRenderSnapshot* snapshot);
SonareError sonare_vocal_snapshot_output_length(const SonareVocalRenderSnapshot* snapshot,
                                                int64_t* samples);
SonareError sonare_vocal_snapshot_render(const SonareVocalRenderSnapshot* snapshot,
                                         SonareVocalRange range, uint64_t request_id,
                                         SonareVocalCancelCallback cancel, void* user_data,
                                         SonareVocalRenderResult* result);
SonareError sonare_vocal_render_job_begin(const SonareVocalRenderSnapshot* snapshot,
                                          SonareVocalRange range, uint64_t request_id,
                                          SonareVocalRenderJob** job);
SonareError sonare_vocal_render_job_next(SonareVocalRenderJob* job,
                                         SonareVocalCancelCallback cancel, void* user_data,
                                         int* complete);
SonareError sonare_vocal_render_job_finalize(SonareVocalRenderJob* job,
                                             SonareVocalCancelCallback cancel, void* user_data,
                                             SonareVocalRenderResult* result);
void sonare_vocal_render_job_abort(SonareVocalRenderJob* job);
void sonare_vocal_render_job_destroy(SonareVocalRenderJob* job);
SonareError sonare_vocal_session_export_state(const SonareVocalEditSession* session,
                                              SonareVocalStateBytes* state);
SonareError sonare_vocal_session_restore(const float* samples, int64_t frames, int channels,
                                         int sample_rate, const uint8_t* state, uint64_t size,
                                         SonareVocalEditSession** session);

void sonare_vocal_free_notes(SonareVocalNotesResult* result);
void sonare_vocal_free_analysis(SonareVocalAnalysisResult* result);
void sonare_vocal_free_edit_result(SonareVocalEditResult* result);
void sonare_vocal_free_pitch_result(SonareVocalPitchResult* result);
void sonare_vocal_free_render_result(SonareVocalRenderResult* result);
void sonare_vocal_free_state_bytes(SonareVocalStateBytes* state);

#ifdef __cplusplus
}
#endif
