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

// Version
const char* sonare_version(void);

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
