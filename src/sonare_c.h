#pragma once

/// @file sonare_c.h
/// @brief C API for libsonare.
/// @details Provides a C-compatible interface for use from C code and WASM.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef enum {
  SONARE_OK = 0,
  SONARE_ERROR_FILE_NOT_FOUND = 1,
  SONARE_ERROR_INVALID_FORMAT = 2,
  SONARE_ERROR_DECODE_FAILED = 3,
  SONARE_ERROR_INVALID_PARAMETER = 4,
  SONARE_ERROR_OUT_OF_MEMORY = 5,
  SONARE_ERROR_UNKNOWN = 99
} SonareError;

// Pitch class enum
typedef enum {
  SONARE_PITCH_C = 0,
  SONARE_PITCH_CS = 1,
  SONARE_PITCH_D = 2,
  SONARE_PITCH_DS = 3,
  SONARE_PITCH_E = 4,
  SONARE_PITCH_F = 5,
  SONARE_PITCH_FS = 6,
  SONARE_PITCH_G = 7,
  SONARE_PITCH_GS = 8,
  SONARE_PITCH_A = 9,
  SONARE_PITCH_AS = 10,
  SONARE_PITCH_B = 11
} SonarePitchClass;

// Mode enum
typedef enum {
  SONARE_MODE_MAJOR = 0,
  SONARE_MODE_MINOR = 1,
  SONARE_MODE_DORIAN = 2,
  SONARE_MODE_PHRYGIAN = 3,
  SONARE_MODE_LYDIAN = 4,
  SONARE_MODE_MIXOLYDIAN = 5,
  SONARE_MODE_LOCRIAN = 6
} SonareMode;

typedef enum {
  SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER = 0,
  SONARE_KEY_PROFILE_TEMPERLEY = 1,
  SONARE_KEY_PROFILE_SHAATH = 2,
  SONARE_KEY_PROFILE_FARALDO_EDMT = 3,
  SONARE_KEY_PROFILE_FARALDO_EDMA = 4,
  SONARE_KEY_PROFILE_FARALDO_EDMM = 5,
  SONARE_KEY_PROFILE_BELLMAN_BUDGE = 6
} SonareKeyProfileType;

// Opaque types
typedef struct SonareAudio SonareAudio;
typedef struct SonareAnalyzer SonareAnalyzer;
typedef struct SonareMixer SonareMixer;
typedef struct SonareStrip SonareStrip;
typedef struct SonareEq SonareEq;

#define SONARE_EQ_MAX_BANDS 24
#define SONARE_EQ_SPECTRUM_STREAM_CAPACITY 256
#define SONARE_EQ_SPECTRUM_PROFILE_BANDS 16

// Values match the offline `phaseMode` param and eq::PhaseMode ordinals;
// 0 (Inherit) is invalid for a global phase mode.
typedef enum {
  SONARE_EQ_PHASE_ZERO_LATENCY = 1,
  SONARE_EQ_PHASE_NATURAL = 2,
  SONARE_EQ_PHASE_LINEAR = 3
} SonareEqPhaseMode;

// Key structure
typedef struct {
  SonarePitchClass root;
  SonareMode mode;
  float confidence;
} SonareKey;

typedef struct {
  SonareKey key;
  float correlation;
} SonareKeyCandidate;

// Time signature structure
typedef struct {
  int numerator;
  int denominator;
  float confidence;
} SonareTimeSignature;

// Analysis result structure
typedef struct {
  float bpm;
  float bpm_confidence;
  SonareKey key;
  SonareTimeSignature time_signature;
  float* beat_times;
  size_t beat_count;
} SonareAnalysisResult;

typedef enum {
  SONARE_GROOVE_STRAIGHT = 0,
  SONARE_GROOVE_SHUFFLE = 1,
  SONARE_GROOVE_SWING = 2
} SonareGrooveType;

typedef enum {
  SONARE_CHORD_MAJOR = 0,
  SONARE_CHORD_MINOR = 1,
  SONARE_CHORD_DIMINISHED = 2,
  SONARE_CHORD_AUGMENTED = 3,
  SONARE_CHORD_DOMINANT7 = 4,
  SONARE_CHORD_MAJOR7 = 5,
  SONARE_CHORD_MINOR7 = 6,
  SONARE_CHORD_SUS2 = 7,
  SONARE_CHORD_SUS4 = 8,
  SONARE_CHORD_UNKNOWN = 9,
  SONARE_CHORD_ADD9 = 10,
  SONARE_CHORD_MINOR_ADD9 = 11,
  SONARE_CHORD_DIM7 = 12,
  SONARE_CHORD_HALF_DIM7 = 13,
  SONARE_CHORD_MAJOR9 = 14,
  SONARE_CHORD_DOMINANT9 = 15,
  SONARE_CHORD_SUS2_ADD4 = 16
} SonareChordQuality;

// Audio functions
SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out);
SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out);

#ifndef __EMSCRIPTEN__
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
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

// Full analysis
SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out);

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
///   the same thread that records or clears an error. Returns an empty string ("") when no
///   detailed message has been recorded. The pointer is never NULL.
/// @return Pointer to a NUL-terminated thread-local message string.
const char* sonare_last_error_message(void);

// Version
const char* sonare_version(void);

/// @brief Returns 1 if libsonare was compiled with FFmpeg-backed decoding for
///        M4A/AAC/FLAC/OGG, 0 otherwise.
/// @details This reflects the value of the @c SONARE_WITH_FFMPEG CMake option at
///   build time. Language bindings expose this so test suites can conditionally
///   exercise the FFmpeg decode path without false failures.
int sonare_has_ffmpeg_support(void);

// ============================================================================
// Result structures for feature/effect functions
// ============================================================================

// STFT result
typedef struct {
  int n_bins;
  int n_frames;
  int n_fft;
  int hop_length;
  int sample_rate;
  float* magnitude;  // n_bins * n_frames, caller frees with sonare_free_floats
  float* power;      // n_bins * n_frames, caller frees with sonare_free_floats
} SonareStftResult;

// Mel spectrogram result
typedef struct {
  int n_mels;
  int n_frames;
  int sample_rate;
  int hop_length;
  float* power;  // n_mels * n_frames
  float* db;     // n_mels * n_frames
} SonareMelResult;

// MFCC result
typedef struct {
  int n_mfcc;
  int n_frames;
  float* coefficients;  // n_mfcc * n_frames
} SonareMfccResult;

// Chroma result
typedef struct {
  int n_chroma;
  int n_frames;
  int sample_rate;
  int hop_length;
  float* features;     // n_chroma * n_frames
  float* mean_energy;  // n_chroma
} SonareChromaResult;

// Pitch result
typedef struct {
  int n_frames;
  float* f0;           // n_frames
  float* voiced_prob;  // n_frames
  int* voiced_flag;    // n_frames (0 or 1)
  float median_f0;
  float mean_f0;
} SonarePitchResult;

// HPSS result
typedef struct {
  float* harmonic;    // length samples
  float* percussive;  // length samples
  size_t length;
  int sample_rate;
} SonareHpssResult;

typedef struct {
  float bpm;
  float confidence;
} SonareBpmCandidate;

typedef struct {
  float rt60;
  float edt;
  float c50;
  float c80;
  float d50;
  float* rt60_bands;
  float* edt_bands;
  float* c50_bands;
  float* c80_bands;
  size_t band_count;
  float confidence;
  int is_blind;
} SonareAcousticResult;

// LUFS loudness result (no heap pointers; no free function required).
typedef struct {
  float integrated_lufs;
  float momentary_lufs;
  float short_term_lufs;
  float loudness_range;
} SonareLufsResult;

typedef struct {
  float bpm;
  float confidence;
  SonareBpmCandidate* candidates;
  size_t candidate_count;
  float* autocorrelation;
  size_t autocorrelation_count;
  float* tempogram;
  size_t tempogram_count;
} SonareBpmAnalysisResult;

typedef struct {
  float bpm;
  SonareTimeSignature time_signature;
  SonareGrooveType groove_type;
  float syncopation;
  float pattern_regularity;
  float tempo_stability;
  float* beat_intervals;
  size_t beat_interval_count;
} SonareRhythmResult;

typedef struct {
  float dynamic_range_db;
  float peak_db;
  float rms_db;
  float crest_factor;
  float loudness_range_db;
  int is_compressed;
  float* loudness_times;
  float* loudness_rms_db;
  size_t loudness_count;
} SonareDynamicsResult;

typedef struct {
  float brightness;
  float warmth;
  float density;
  float roughness;
  float complexity;
  float* spectral_centroid;
  size_t spectral_centroid_count;
  float* spectral_flatness;
  size_t spectral_flatness_count;
  float* spectral_rolloff;
  size_t spectral_rolloff_count;
} SonareTimbreResult;

typedef struct {
  SonarePitchClass root;
  SonareChordQuality quality;
  float start;
  float end;
  float confidence;
  SonarePitchClass bass;
} SonareChord;

typedef struct {
  SonareChord* chords;
  size_t chord_count;
} SonareChordAnalysisResult;

typedef struct {
  float min_duration;
  float smoothing_window;
  float threshold;
  int use_triads_only;
  int n_fft;
  int hop_length;
  int use_beat_sync;
  int use_hmm;
  int hmm_beam_width;
  int use_key_context;
  SonarePitchClass key_root;
  SonareMode key_mode;
  int detect_inversions;
  int chroma_method;  // 0 = STFT, 1 = NNLS
} SonareChordDetectionOptions;

// ============================================================================
// Mastering
// ============================================================================

typedef struct {
  float target_lufs;
  float ceiling_db;
  int true_peak_oversample;
} SonareMasteringConfig;

typedef struct {
  float* samples;
  size_t length;
  int sample_rate;
  float input_lufs;
  float output_lufs;
  float applied_gain_db;
  int latency_samples;
} SonareMasteringResult;

typedef struct {
  const char* key;
  double value;
} SonareMasteringParam;

typedef struct {
  float* left;
  float* right;
  size_t length;
  int sample_rate;
  float input_lufs;
  float output_lufs;
  float applied_gain_db;
  int latency_samples;
} SonareMasteringStereoResult;

/// @brief Progress callback type. Called per chain stage completion.
/// @param progress Progress value (0.0 to 1.0).
/// @param stage    Stage identifier C string (e.g. "dynamics.compressor"). Valid
///                 only during the callback invocation; copy if needed later.
/// @param user_data Opaque pointer passed at registration.
typedef void (*SonareMasteringProgressCallback)(float progress, const char* stage, void* user_data);

// Result of running the MasteringChain on a mono buffer.
// Memory for @c samples and @c stages (and each char* entry inside it) is
// allocated by libsonare with @c new[]; free with
// @c sonare_free_mastering_chain_result.
typedef struct {
  float* samples;
  size_t length;
  int sample_rate;
  float input_lufs;
  float output_lufs;
  float applied_gain_db;
  char** stages;  // newline-free stage identifiers, e.g. "dynamics.compressor"
  size_t stages_count;
} SonareMasteringChainResult;

// Result of running the MasteringChain on stereo buffers. Same ownership
// rules as @c SonareMasteringChainResult; free with
// @c sonare_free_mastering_chain_stereo_result.
typedef struct {
  float* left;
  float* right;
  size_t length;
  int sample_rate;
  float input_lufs;
  float output_lufs;
  float applied_gain_db;
  char** stages;
  size_t stages_count;
} SonareMasteringChainStereoResult;

SonareError sonare_mastering_process(const float* samples, size_t length, int sample_rate,
                                     const SonareMasteringConfig* config,
                                     SonareMasteringResult* out);
SonareError sonare_mastering_apply_processor(const char* processor_name, const float* samples,
                                             size_t length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             SonareMasteringResult* out);
SonareError sonare_mastering_apply_processor_stereo(const char* processor_name, const float* left,
                                                    const float* right, size_t length,
                                                    int sample_rate,
                                                    const SonareMasteringParam* params,
                                                    size_t param_count,
                                                    SonareMasteringStereoResult* out);

/// @brief Run the full mastering chain on a mono buffer.
/// @details @p params is a flat (key/value) view of the
///   @c MasteringChainConfig hierarchy using dot-notation keys (see
///   @c parse_chain_config_params). Unknown keys cause
///   @c SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_mastering_chain(const float* samples, size_t length, int sample_rate,
                                   const SonareMasteringParam* params, size_t param_count,
                                   SonareMasteringChainResult* out);

/// @brief Run the full mastering chain on stereo buffers.
SonareError sonare_mastering_chain_stereo(const float* left, const float* right, size_t length,
                                          int sample_rate, const SonareMasteringParam* params,
                                          size_t param_count,
                                          SonareMasteringChainStereoResult* out);

/// @brief Same as sonare_mastering_chain but reports per-stage progress.
SonareError sonare_mastering_chain_with_progress(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareMasteringParam* params,
                                                 size_t param_count,
                                                 SonareMasteringProgressCallback callback,
                                                 void* user_data, SonareMasteringChainResult* out);

/// @brief Same as sonare_mastering_chain_stereo but reports per-stage progress.
SonareError sonare_mastering_chain_stereo_with_progress(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareMasteringParam* params,
                                                        size_t param_count,
                                                        SonareMasteringProgressCallback callback,
                                                        void* user_data,
                                                        SonareMasteringChainStereoResult* out);

/// @brief Returns built-in preset identifiers, separated by '\n'.
/// @details Pointer is owned by libsonare and remains valid for the program lifetime.
const char* sonare_mastering_preset_names(void);

/// @brief Apply a preset chain to mono audio.
/// @param preset_name e.g. "pop", "aiMusic". See @c sonare_mastering_preset_names().
/// @param overrides   Optional Param overrides (same flat dot-notation as
///                    @c sonare_mastering_chain). Pass NULL/0 for preset defaults.
SonareError sonare_master_audio(const char* preset_name, const float* samples, size_t length,
                                int sample_rate, const SonareMasteringParam* overrides,
                                size_t override_count, SonareMasteringChainResult* out);

/// @brief Stereo equivalent of @c sonare_master_audio.
SonareError sonare_master_audio_stereo(const char* preset_name, const float* left,
                                       const float* right, size_t length, int sample_rate,
                                       const SonareMasteringParam* overrides, size_t override_count,
                                       SonareMasteringChainStereoResult* out);

/// @brief Same as sonare_master_audio but reports per-stage progress.
SonareError sonare_master_audio_with_progress(const char* preset_name, const float* samples,
                                              size_t length, int sample_rate,
                                              const SonareMasteringParam* overrides,
                                              size_t override_count,
                                              SonareMasteringProgressCallback callback,
                                              void* user_data, SonareMasteringChainResult* out);

/// @brief Same as sonare_master_audio_stereo but reports per-stage progress.
SonareError sonare_master_audio_stereo_with_progress(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out);

const char* sonare_mastering_processor_names(void);
const char* sonare_mastering_pair_processor_names(void);
const char* sonare_mastering_pair_analysis_names(void);
const char* sonare_mastering_stereo_analysis_names(void);
SonareError sonare_mastering_apply_pair_processor(const char* processor_name, const float* source,
                                                  const float* reference, size_t length,
                                                  int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, SonareMasteringResult* out);
SonareError sonare_mastering_analyze_pair(const char* analysis_name, const float* source,
                                          const float* reference, size_t length, int sample_rate,
                                          const SonareMasteringParam* params, size_t param_count,
                                          char** json_out);
SonareError sonare_mastering_analyze_stereo(const char* analysis_name, const float* left,
                                            const float* right, size_t length, int sample_rate,
                                            const SonareMasteringParam* params, size_t param_count,
                                            char** json_out);

typedef struct {
  float pre_left[SONARE_EQ_SPECTRUM_STREAM_CAPACITY];
  float pre_right[SONARE_EQ_SPECTRUM_STREAM_CAPACITY];
  float post_left[SONARE_EQ_SPECTRUM_STREAM_CAPACITY];
  float post_right[SONARE_EQ_SPECTRUM_STREAM_CAPACITY];
  size_t pre_count;
  size_t post_count;
  float band_gain_db[SONARE_EQ_MAX_BANDS];
  float profile_db[SONARE_EQ_SPECTRUM_PROFILE_BANDS];
  float last_auto_gain_db;
  uint64_t seq;
} SonareEqSnapshot;

SonareEq* sonare_eq_create(double sample_rate, int max_block_size);
void sonare_eq_destroy(SonareEq* eq);
SonareError sonare_eq_set_band(SonareEq* eq, int index, const char* band_json);
void sonare_eq_clear(SonareEq* eq);
SonareError sonare_eq_set_phase_mode(SonareEq* eq, int mode);
SonareError sonare_eq_match(SonareEq* eq, const float* source, const float* reference,
                            size_t length, int sample_rate, int max_bands);
void sonare_eq_set_auto_gain(SonareEq* eq, int enabled);
float sonare_eq_last_auto_gain_db(const SonareEq* eq);
SonareError sonare_eq_set_gain_scale(SonareEq* eq, float scale);
SonareError sonare_eq_set_output_gain_db(SonareEq* eq, float gain_db);
SonareError sonare_eq_set_output_pan(SonareEq* eq, float pan);
int sonare_eq_latency_samples(const SonareEq* eq);
SonareError sonare_eq_set_sidechain(SonareEq* eq, const float* const* channels, int num_channels,
                                    int num_samples);
void sonare_eq_clear_sidechain(SonareEq* eq);
SonareError sonare_eq_process(SonareEq* eq, float* const* channels, int num_channels,
                              int num_samples);
SonareError sonare_eq_spectrum(const SonareEq* eq, SonareEqSnapshot* out);

void sonare_free_mastering_result(SonareMasteringResult* result);
void sonare_free_mastering_stereo_result(SonareMasteringStereoResult* result);
void sonare_free_mastering_chain_result(SonareMasteringChainResult* result);
void sonare_free_mastering_chain_stereo_result(SonareMasteringChainStereoResult* result);

// ----------------------------------------------------------------------------
// Streaming mastering chain
// ----------------------------------------------------------------------------

/// @brief Opaque streaming mastering chain handle.
typedef struct SonareStreamingMasteringChain SonareStreamingMasteringChain;

/// @brief Create a streaming chain from flat params. Returns NULL on error
/// (e.g. unknown key, non-streaming stage enabled).
SonareStreamingMasteringChain* sonare_streaming_mastering_chain_create(
    const SonareMasteringParam* params, size_t param_count);

/// @brief Prepare with sample rate, max block size, and channel count (1 or 2).
SonareError sonare_streaming_mastering_chain_prepare(SonareStreamingMasteringChain* handle,
                                                     int sample_rate, int max_block_size,
                                                     int num_channels);

/// @brief Process one mono block in place. @p samples length must be <= max_block_size.
SonareError sonare_streaming_mastering_chain_process_mono(SonareStreamingMasteringChain* handle,
                                                          float* samples, size_t num_samples);

/// @brief Process one stereo block in place. @p left, @p right same length.
SonareError sonare_streaming_mastering_chain_process_stereo(SonareStreamingMasteringChain* handle,
                                                            float* left, float* right,
                                                            size_t num_samples);

/// @brief Reset processor state without rebuilding.
SonareError sonare_streaming_mastering_chain_reset(SonareStreamingMasteringChain* handle);

/// @brief Returns total latency in samples (0 if not prepared).
int sonare_streaming_mastering_chain_latency_samples(const SonareStreamingMasteringChain* handle);

/// @brief Destroy and free the handle.
void sonare_streaming_mastering_chain_destroy(SonareStreamingMasteringChain* handle);

// ============================================================================
// Mixing
// ============================================================================

typedef enum {
  SONARE_PAN_MODE_BALANCE = 0,
  SONARE_PAN_MODE_STEREO_PAN = 1,
  SONARE_PAN_MODE_DUAL_PAN = 2
} SonarePanMode;

typedef enum {
  SONARE_SEND_TIMING_PRE_FADER = 0,
  SONARE_SEND_TIMING_POST_FADER = 1
} SonareSendTiming;

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
SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode);
SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan);
SonareError sonare_strip_set_width(SonareStrip* strip, float width);
SonareError sonare_strip_set_muted(SonareStrip* strip, int muted);
SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out);
SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db);
SonareError sonare_strip_meter(const SonareStrip* strip, SonareMixMeterSnapshot* out);
size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points);

// Number of strips in the mixer (e.g. strips loaded from a scene). Returns 0 if
// mixer is NULL.
size_t sonare_mixer_strip_count(const SonareMixer* mixer);
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
// @c curve: 0 = Linear, 1 = Exponential. Returns @c SONARE_OK on success, or
// @c SONARE_ERROR_INVALID_PARAMETER if strip is NULL, curve is unknown, or
// insert_index is out of range.
SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve);
SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size);
SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out);
// Rebuilds and compiles the internal routing graph from the current topology
// (strips, sends, buses, connections). Call after manual topology changes
// (e.g. sonare_mixer_add_strip / sonare_strip_add_send) before processing.
// sonare_mixer_process_stereo also compiles lazily as a fallback when the
// topology is dirty.
SonareError sonare_mixer_compile(SonareMixer* mixer);
SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples);
const char* sonare_mixing_scene_preset_names(void);
SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out);
void sonare_mixer_destroy(SonareMixer* mixer);

// ============================================================================
// Effects
// ============================================================================

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out);
SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length);
SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length);
SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate, float rate,
                                float** out, size_t* out_length);
SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float semitones, float** out, size_t* out_length);
SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length);
SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length);
SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                float pitch_semitones, float formant_factor, float** out,
                                size_t* out_length);
SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length);

// ============================================================================
// Detailed analysis primitives
// ============================================================================

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, int hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out);
SonareError sonare_analyze_impulse_response(const float* samples, size_t length, int sample_rate,
                                            int n_octave_bands, SonareAcousticResult* out);
SonareError sonare_detect_acoustic(const float* samples, size_t length, int sample_rate,
                                   int n_octave_bands, int n_third_octave_subbands,
                                   float min_decay_db, float noise_floor_margin_db,
                                   SonareAcousticResult* out);
SonareError sonare_analyze_rhythm(const float* samples, size_t length, int sample_rate,
                                  float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  int hop_length, SonareRhythmResult* out);
SonareError sonare_analyze_dynamics(const float* samples, size_t length, int sample_rate,
                                    float window_sec, int hop_length, float compression_threshold,
                                    SonareDynamicsResult* out);
SonareError sonare_analyze_timbre(const float* samples, size_t length, int sample_rate, int n_fft,
                                  int hop_length, int n_mels, int n_mfcc, float window_sec,
                                  SonareTimbreResult* out);
SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 int use_triads_only, int n_fft, int hop_length, int use_beat_sync,
                                 SonareChordAnalysisResult* out);
SonareError sonare_detect_chords_ex(const float* samples, size_t length, int sample_rate,
                                    const SonareChordDetectionOptions* options,
                                    SonareChordAnalysisResult* out);
void sonare_free_bpm_analysis_result(SonareBpmAnalysisResult* result);
void sonare_free_acoustic_result(SonareAcousticResult* result);
void sonare_free_rhythm_result(SonareRhythmResult* result);
void sonare_free_dynamics_result(SonareDynamicsResult* result);
void sonare_free_timbre_result(SonareTimbreResult* result);
void sonare_free_chord_analysis_result(SonareChordAnalysisResult* result);

// ============================================================================
// Features - Spectrogram
// ============================================================================

SonareError sonare_stft(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, SonareStftResult* out);
SonareError sonare_stft_db(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int* out_n_bins, int* out_n_frames, float** out_db);

// ============================================================================
// Features - Mel
// ============================================================================

SonareError sonare_mel_spectrogram(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, int n_mels, SonareMelResult* out);
SonareError sonare_mfcc(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, int n_mels, int n_mfcc, SonareMfccResult* out);

// ============================================================================
// Features - Chroma
// ============================================================================

SonareError sonare_chroma(const float* samples, size_t length, int sample_rate, int n_fft,
                          int hop_length, SonareChromaResult* out);

// ============================================================================
// Features - Spectral (each returns a float array of per-frame values)
// ============================================================================

SonareError sonare_spectral_centroid(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count);
SonareError sonare_spectral_bandwidth(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, float** out, size_t* out_count);
SonareError sonare_spectral_rolloff(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float roll_percent, float** out,
                                    size_t* out_count);
SonareError sonare_spectral_flatness(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count);
SonareError sonare_zero_crossing_rate(const float* samples, size_t length, int sample_rate,
                                      int frame_length, int hop_length, float** out,
                                      size_t* out_count);
SonareError sonare_rms_energy(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float** out, size_t* out_count);

// ============================================================================
// Features - Pitch
// ============================================================================

SonareError sonare_pitch_yin(const float* samples, size_t length, int sample_rate, int frame_length,
                             int hop_length, float fmin, float fmax, float threshold,
                             SonarePitchResult* out);
SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float fmin, float fmax,
                              float threshold, SonarePitchResult* out);

// ============================================================================
// Core - Conversion
// ============================================================================

float sonare_hz_to_mel(float hz);
float sonare_mel_to_hz(float mel);
float sonare_hz_to_midi(float hz);
float sonare_midi_to_hz(float midi);
const char* sonare_hz_to_note(float hz);
float sonare_note_to_hz(const char* note);
float sonare_frames_to_time(int frames, int sr, int hop_length);
int sonare_time_to_frames(float time, int sr, int hop_length);

int sonare_frames_to_samples(int frames, int hop_length, int n_fft);
int sonare_samples_to_frames(int samples, int hop_length, int n_fft);

SonareError sonare_power_to_db(const float* values, size_t length, float ref, float amin,
                               float top_db, float** out, size_t* out_length);
SonareError sonare_amplitude_to_db(const float* values, size_t length, float ref, float amin,
                                   float top_db, float** out, size_t* out_length);
SonareError sonare_db_to_power(const float* values, size_t length, float ref, float** out,
                               size_t* out_length);
SonareError sonare_db_to_amplitude(const float* values, size_t length, float ref, float** out,
                                   size_t* out_length);

SonareError sonare_preemphasis(const float* samples, size_t length, float coef, float zi,
                               int use_zi, float** out, size_t* out_length);
SonareError sonare_deemphasis(const float* samples, size_t length, float coef, float zi, int use_zi,
                              float** out, size_t* out_length);

SonareError sonare_trim_silence(const float* samples, size_t length, float top_db, int frame_length,
                                int hop_length, float** out, size_t* out_length, int* start_sample,
                                int* end_sample);
SonareError sonare_split_silence(const float* samples, size_t length, float top_db,
                                 int frame_length, int hop_length, int** out_intervals,
                                 size_t* out_interval_count);

SonareError sonare_frame_signal(const float* samples, size_t length, int frame_length,
                                int hop_length, float** out, size_t* out_length, int* out_n_frames);
SonareError sonare_pad_center(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length);
SonareError sonare_fix_length(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length);
SonareError sonare_fix_frames(const int* frames, size_t length, int x_min, int x_max, int pad,
                              int** out, size_t* out_length);
SonareError sonare_peak_pick(const float* values, size_t length, int pre_max, int post_max,
                             int pre_avg, int post_avg, float delta, int wait, int** out,
                             size_t* out_length);
SonareError sonare_vector_normalize(const float* values, size_t length, int norm_type,
                                    float threshold, float** out, size_t* out_length);

SonareError sonare_pcen(const float* values, int n_bins, int n_frames, int sample_rate,
                        int hop_length, float time_constant, float gain, float bias, float power,
                        float eps, float** out, size_t* out_length);
SonareError sonare_tonnetz(const float* chromagram, int n_chroma, int n_frames, float** out,
                           size_t* out_length);
SonareError sonare_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                             int hop_length, int win_length, int center, int norm, float** out,
                             size_t* out_length, int* out_n_frames);
SonareError sonare_cyclic_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                                    int hop_length, int win_length, float bpm_min, int n_bins,
                                    float** out, size_t* out_length, int* out_n_frames);
SonareError sonare_plp(const float* onset_envelope, size_t length, int sample_rate, int hop_length,
                       float tempo_min, float tempo_max, int win_length, float** out,
                       size_t* out_length);

/// @brief Onset strength envelope from audio (librosa.onset.onset_strength).
/// @details Builds a Mel spectrogram from @p samples and returns the half-wave
///   rectified onset strength envelope. Output length is the number of frames.
SonareError sonare_onset_strength(const float* samples, size_t length, int sr, int n_fft,
                                  int hop_length, int n_mels, float** out, size_t* out_length);

/// @brief Fourier (FFT-based) tempogram of an onset envelope.
/// @details Returns a magnitude matrix [n_bins x n_frames] row-major, where
///   n_bins = win_length / 2 + 1 (derivable as out_length / out_n_frames).
SonareError sonare_fourier_tempogram(const float* onset_envelope, size_t length, int sr,
                                     int hop_length, int win_length, int center, int norm,
                                     float** out, size_t* out_length, int* out_n_frames);

/// @brief Aggregated tempogram values at integer tempo ratios of a reference tempo.
/// @details If @p factors is NULL or @p n_factors is 0, the library default
///   factors {0.5, 1, 2, 3, 4} are used. The output contains one value per factor.
SonareError sonare_tempogram_ratio(const float* tempogram_data, size_t length, int win_length,
                                   int sr, int hop_length, const float* factors, size_t n_factors,
                                   float** out, size_t* out_length);

/// @brief NNLS chroma from audio (12 x n_frames row-major).
SonareError sonare_nnls_chroma(const float* samples, size_t length, int sr, float** out,
                               size_t* out_length, int* out_n_frames);

/// @brief Integrated/momentary/short-term LUFS and loudness range (offline meter).
SonareError sonare_lufs(const float* samples, size_t length, int sr, SonareLufsResult* out);

/// @brief Per-block momentary LUFS time series.
SonareError sonare_momentary_lufs(const float* samples, size_t length, int sr, float** out,
                                  size_t* out_length);

/// @brief Per-block short-term LUFS time series.
SonareError sonare_short_term_lufs(const float* samples, size_t length, int sr, float** out,
                                   size_t* out_length);

// ============================================================================
// Core - Resample
// ============================================================================

SonareError sonare_resample(const float* samples, size_t length, int src_sr, int target_sr,
                            float** out, size_t* out_length);

// ============================================================================
// Memory management for result types
// ============================================================================

void sonare_free_stft_result(SonareStftResult* result);
void sonare_free_mel_result(SonareMelResult* result);
void sonare_free_mfcc_result(SonareMfccResult* result);
void sonare_free_chroma_result(SonareChromaResult* result);
void sonare_free_pitch_result(SonarePitchResult* result);
void sonare_free_hpss_result(SonareHpssResult* result);

#ifdef __cplusplus
}
#endif
