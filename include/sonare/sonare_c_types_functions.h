#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

// Audio functions
SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out);
SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out);

#ifndef __EMSCRIPTEN__
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
/// @brief Returns the positive source channel count reported by an audio file.
/// @details @p out_channels is set to 0 immediately when it is non-NULL, before
///          argument validation or file inspection. On @c SONARE_OK it receives
///          a positive channel count and is left at 0 on every failure. Both
///          @p path and @p out_channels must be non-NULL; either NULL argument
///          returns @c SONARE_ERROR_INVALID_PARAMETER. A missing/unreadable file
///          returns @c SONARE_ERROR_FILE_NOT_FOUND. A malformed recognized file
///          or decoder/probe failure returns @c SONARE_ERROR_DECODE_FAILED.
///          An unknown or unsupported format returns @c SONARE_ERROR_INVALID_FORMAT
///          when no FFmpeg decoder is enabled; with FFmpeg enabled, failures
///          while probing that format return @c SONARE_ERROR_DECODE_FAILED.
SonareError sonare_audio_file_channel_count(const char* path, int* out_channels);
#endif

void sonare_audio_free(SonareAudio* audio);
const float* sonare_audio_data(const SonareAudio* audio);
size_t sonare_audio_length(const SonareAudio* audio);
int sonare_audio_sample_rate(const SonareAudio* audio);
float sonare_audio_duration(const SonareAudio* audio);
SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float* out_bpm);
SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key);
SonareError sonare_audio_detect_beats(const SonareAudio* audio, float** out_times,
                                      size_t* out_count);
SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count);
SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count);
// Runs the full quick analysis pipeline and fills the flat C result. Use the
// single-purpose sonare_audio_detect_* helpers for cheaper queries.
SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// Quick detection functions
SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate, float* out_bpm);
SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key);
SonareError sonare_detect_key_with_options(const float* samples, size_t length, int sample_rate,
                                           int n_fft, int hop_length, int use_hpss,
                                           int loudness_weighted, float high_pass_hz,
                                           SonareKey* out_key);
SonareError sonare_detect_key_with_options_and_modes(const float* samples, size_t length,
                                                     int sample_rate, int n_fft, int hop_length,
                                                     int use_hpss, int loudness_weighted,
                                                     float high_pass_hz, const SonareMode* modes,
                                                     size_t mode_count, SonareKey* out_key);
SonareError sonare_detect_key_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKey* out_key);
SonareError sonare_detect_key_candidates(const float* samples, size_t length, int sample_rate,
                                         int n_fft, int hop_length, int use_hpss,
                                         int loudness_weighted, float high_pass_hz,
                                         SonareKeyCandidate** out_candidates, size_t* out_count);
SonareError sonare_detect_key_candidates_with_modes(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyCandidate** out_candidates, size_t* out_count);
SonareError sonare_detect_key_candidates_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKeyCandidate** out_candidates,
    size_t* out_count);
SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate,
                                float** out_times, size_t* out_count);
SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count);
SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count);

/// @brief Controls peak-picking for @ref sonare_detect_onsets_ex.
/// @details Zero-initialize this POD, then set every field explicitly; the
///          legacy @ref sonare_detect_onsets function retains its defaults.
typedef struct {
  int32_t n_fft;
  int32_t hop_length;
  float threshold;
  int32_t pre_max;
  int32_t post_max;
  int32_t pre_avg;
  int32_t post_avg;
  float delta;
  int32_t wait;
  uint8_t backtrack;
  uint8_t reserved[3];
  int32_t backtrack_range;
} SonareOnsetDetectConfig;

#ifdef __cplusplus
static_assert(sizeof(SonareOnsetDetectConfig) == 44u, "SonareOnsetDetectConfig layout drift");
static_assert(offsetof(SonareOnsetDetectConfig, hop_length) == 4u,
              "SonareOnsetDetectConfig hop_length offset");
static_assert(offsetof(SonareOnsetDetectConfig, backtrack_range) == 40u,
              "SonareOnsetDetectConfig backtrack_range offset");
#endif

/// @brief Detect onsets with explicit FFT, peak-picking, and backtracking settings.
SonareError sonare_detect_onsets_ex(const float* samples, size_t length, int sample_rate,
                                    const SonareOnsetDetectConfig* config, float** out_times,
                                    size_t* out_count);

// Runs the full quick analysis pipeline and fills the flat C result. Use the
// single-purpose sonare_detect_* helpers for cheaper queries.
SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out);

/* Progress callback for the JSON analysis variants: progress in [0,1] plus a
   stage label. user_data is the opaque pointer passed to the analyze call. */
typedef void (*SonareAnalyzeProgressCallback)(float progress, const char* stage, void* user_data);

/* Cooperative cancellation callback for long-running offline operations.
   Return nonzero to stop after the current progress-report boundary. */
typedef int (*SonareCancelCallback)(void* user_data);

/* Full analysis serialized to a camelCase JSON object. Unlike sonare_analyze
   (which fills only the flat bpm/key/beats struct), this returns the complete
   result: chords, sections, timbre, dynamics, rhythm, melody and form, with
   per-beat strength. *out_json is heap-allocated and MUST be released with
   sonare_free_string. */
SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                char** out_json);
/* Capacity of SonareMusicAnalyzeOptions::meter_candidate_numerators. Part of
   the public contract: a caller cannot request more numerators than this, and
   every binding rejects an over-long list rather than truncating it. */
#define SONARE_MAX_METER_CANDIDATE_NUMERATORS 16

/* Options for sonare_analyze_json_ex. Boolean fields are 0/1 ints. Always
   start from sonare_music_analyze_options_default() rather than zeroing the
   struct: a zeroed meter_candidate_numerator_count is rejected, not treated as
   "use the default set". */
typedef struct {
  int n_fft;
  int hop_length;
  float bpm_min;
  float bpm_max;
  float start_bpm;
  int use_triads_only;
  int use_hpss;
  float chroma_highpass_hz;
  int use_bass_weighted;
  int chroma_hop_multiplier;
  int use_chord_hmm;
  int use_chord_key_context;
  int chord_hmm_beam_width;
  int detect_chord_inversions;
  /* Track a locally updated tempo prior through beat tracking. */
  int adaptive_tempo;
  /* Local tempo context length in beats; used only when adaptive_tempo is 1. */
  int tempo_update_interval_beats;
  /* Meter numerators the estimator scores. Only the first
     meter_candidate_numerator_count entries are read; each must be in [2, 32].
     Widening the set does not force a wider meter — the default {3, 4, 6}
     reproduces the historical result. */
  int meter_candidate_numerators[SONARE_MAX_METER_CANDIDATE_NUMERATORS];
  int meter_candidate_numerator_count;
  /* Beat unit reported for the detected meter; a power of two in [1, 32]. The
     estimator still reports 8 on its own when it resolves a compound meter. */
  int meter_denominator;
} SonareMusicAnalyzeOptions;

SonareMusicAnalyzeOptions sonare_music_analyze_options_default(void);
SonareError sonare_analyze_json_ex(const float* samples, size_t length, int sample_rate,
                                   const SonareMusicAnalyzeOptions* options, char** out_json);

/* Same as sonare_analyze_json but reports per-stage progress. A null callback
   runs silently. The callback fires on the calling thread before return. */
SonareError sonare_analyze_json_with_progress(const float* samples, size_t length, int sample_rate,
                                              SonareAnalyzeProgressCallback callback,
                                              void* user_data, char** out_json);

/* Cancellation-capable equivalent of sonare_analyze_json_with_progress. When
   cancelled, returns SONARE_ERROR_CANCELLED and leaves *out_json NULL. */
SonareError sonare_analyze_json_with_progress_ex(
    const float* samples, size_t length, int sample_rate, SonareAnalyzeProgressCallback callback,
    void* user_data, char** out_json, SonareCancelCallback cancel_cb, void* cancel_user_data);

// Memory management
void sonare_free_floats(float* ptr);
void sonare_free_ints(int* ptr);
void sonare_free_string(char* ptr);
void sonare_free_key_candidates(SonareKeyCandidate* ptr);
void sonare_free_result(SonareAnalysisResult* result);

// Error handling
const char* sonare_error_message(SonareError error);

/// @brief Returns the detailed message for the most recent error on the calling thread.
/// @details The returned string is owned by libsonare and valid until the next API call on
///   the same thread. The pointer is never NULL; returns an empty string ("") when no
///   detailed message is currently recorded.
///
///   CONTRACT (read carefully):
///   - Every public C-ABI call that returns SonareError CLEARS the thread-local
///     message on entry, so a detailed message can never leak into a later
///     error-code result. Diagnostic accessors and void cleanup helpers do not
///     clear it, allowing callers to release partial outputs before inspection.
///   - A message is recorded ONLY on the caught-C++-exception return path (the
///     library mapped a thrown sonare::SonareException / std::exception to a
///     SonareError). For those, this returns the exception's what() text.
///   - Validation early-returns that produce an error code WITHOUT throwing
///     (e.g. a NULL out-pointer, an out-of-range sample rate, a non-finite input
///     sample) record NO message: this returns "" even though the call failed.
///     Use sonare_error_message(SonareError) for a human-readable string for ANY
///     error code; use this only for the extra detail of exception-path errors.
///   - RIR synthesis is a deliberate SUCCESS-return diagnostic path: when
///     @ref sonare_synthesize_rir sets SonareRirSynthResult::has_error, this
///     returns its first Error as ``acoustic.code: explanation``.
/// @return Pointer to a NUL-terminated thread-local message string.
const char* sonare_last_error_message(void);

/// @brief Returns the most recent non-fatal warning recorded on the calling
///        thread, or "" when none.
/// @details A SEPARATE channel from sonare_last_error_message so a warning on a
///   SUCCESS return never has to share storage with (or be mistaken for) an
///   error. It is recorded by sonare_mixer_from_scene_json when a scene loads
///   successfully but a channel-strip insert was handed param keys it does not
///   read, and by @ref sonare_synthesize_rir for its first recoverable acoustic
///   diagnostic (such as a clamped RIR tail). Use
///   sonare_mastering_insert_param_names() to discover the keys a given insert
///   accepts.
///   - The pointer is owned by libsonare, never NULL, and valid until the next
///     API call that records or clears a warning on the same thread.
///   - Cleared at the entry of sonare_mixer_from_scene_json, so a stale warning
///     from an earlier load never leaks into a later, clean one.
/// @return Pointer to a NUL-terminated thread-local message string.
const char* sonare_last_warning_message(void);

// Version
const char* sonare_version(void);
uint32_t sonare_engine_abi_version(void);

/// @brief Returns a JSON document describing the capabilities of this build, or
///        NULL when the document could not be assembled.
/// @details The document contains the library version, project and engine ABI
///   versions, platform, enabled feature families, available decode backends,
///   SIMD mode, and reported hardware concurrency. The pointer is owned by
///   libsonare and must not be freed. Copy the JSON before making another C API
///   call on the same thread.
///   - The document is rebuilt into thread-local storage on every call, so an
///     allocation failure returns NULL rather than a partial or stale document.
///     Check the pointer before dereferencing it;
///     sonare_last_error_message() then carries the reason.
/// @return Pointer to a NUL-terminated JSON document, or NULL on failure.
const char* sonare_capabilities_json(void);

/// @brief Returns 1 if libsonare was compiled with FFmpeg-backed decoding for
///        M4A/AAC/FLAC/OGG, 0 otherwise.
/// @details This reflects the value of the @c SONARE_WITH_FFMPEG CMake option at
///   build time. Language bindings expose this so test suites can conditionally
///   exercise the FFmpeg decode path without false failures.
int sonare_has_ffmpeg_support(void);

#ifdef __cplusplus
}
#endif
