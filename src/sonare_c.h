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
typedef enum { SONARE_MODE_MAJOR = 0, SONARE_MODE_MINOR = 1 } SonareMode;

// Opaque types
typedef struct SonareAudio SonareAudio;
typedef struct SonareAnalyzer SonareAnalyzer;

// Key structure
typedef struct {
  SonarePitchClass root;
  SonareMode mode;
  float confidence;
} SonareKey;

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
  SONARE_CHORD_UNKNOWN = 9
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
SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count);
SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// Quick detection functions
SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate, float* out_bpm);
SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key);
SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate,
                                float** out_times, size_t* out_count);
SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count);

// Full analysis
SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out);

// Memory management
void sonare_free_floats(float* ptr);
void sonare_free_ints(int* ptr);
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

// TTS quality result
typedef struct {
  float duration_sec;
  float peak_db;
  float rms_db;
  float silence_ratio;
  float clipping_ratio;
  float leading_silence_sec;
  float trailing_silence_sec;
} SonareTtsQualityResult;

typedef struct {
  float bpm;
  float confidence;
} SonareBpmCandidate;

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
} SonareChord;

typedef struct {
  SonareChord* chords;
  size_t chord_count;
} SonareChordAnalysisResult;

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
SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length);
SonareError sonare_analyze_tts_quality(const float* samples, size_t length, int sample_rate,
                                       float silence_threshold_db, SonareTtsQualityResult* out);
SonareError sonare_prepare_tts(const float* samples, size_t length, int sample_rate,
                               float target_rms_db, float silence_threshold_db, float peak_limit_db,
                               float fade_sec, float** out, size_t* out_length);
SonareError sonare_compress_pauses(const float* samples, size_t length, int sample_rate,
                                   float max_pause_sec, float silence_threshold_db, float** out,
                                   size_t* out_length);

// ============================================================================
// Detailed analysis primitives
// ============================================================================

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, int hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out);
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
void sonare_free_bpm_analysis_result(SonareBpmAnalysisResult* result);
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
