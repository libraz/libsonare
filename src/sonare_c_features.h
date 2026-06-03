#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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
/// @brief Functional (Roman-numeral) harmony analysis of detected chords.
/// @details Detects chords with @p options (same algorithm as
///   sonare_detect_chords_ex), then labels each detected chord with a Roman
///   numeral relative to the supplied key (e.g. "I", "IV", "V", "vi"). The
///   returned array has one string per detected chord, in chord order.
/// @param key_root Tonic pitch class of the analysis key.
/// @param key_mode Mode of the analysis key (major/minor/...).
/// @param out Receives a heap-owned string array; free with
///   sonare_free_string_array. Empty audio with no chords yields an empty array.
SonareError sonare_chord_functional_analysis(const float* samples, size_t length, int sample_rate,
                                             const SonareChordDetectionOptions* options,
                                             SonarePitchClass key_root, SonareMode key_mode,
                                             SonareStringArray* out);
/// @brief Detects song-structure sections (intro/verse/chorus/...).
SonareError sonare_analyze_sections(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float min_section_sec,
                                    SonareSectionResult* out);
/// @brief Extracts the melody contour from monophonic audio via YIN.
SonareError sonare_analyze_melody(const float* samples, size_t length, int sample_rate, float fmin,
                                  float fmax, int frame_length, int hop_length, float threshold,
                                  SonareMelodyResult* out);
void sonare_free_bpm_analysis_result(SonareBpmAnalysisResult* result);
void sonare_free_acoustic_result(SonareAcousticResult* result);
void sonare_free_rhythm_result(SonareRhythmResult* result);
void sonare_free_dynamics_result(SonareDynamicsResult* result);
void sonare_free_timbre_result(SonareTimbreResult* result);
void sonare_free_chord_analysis_result(SonareChordAnalysisResult* result);
void sonare_free_string_array(SonareStringArray* result);
void sonare_free_section_result(SonareSectionResult* result);
void sonare_free_melody_result(SonareMelodyResult* result);

// ============================================================================
// Features - Constant-Q / Variable-Q transforms
// ============================================================================

/* Forward CQT/VQT magnitude result. @c magnitude is [n_bins x n_frames]
   row-major and @c frequencies has @c n_bins center frequencies (Hz). Free both
   arrays with sonare_free_cqt_result. */
typedef struct {
  int n_bins;
  int n_frames;
  int hop_length;
  int sample_rate;
  float* magnitude;   /* n_bins * n_frames */
  float* frequencies; /* n_bins */
} SonareCqtResult;

/// @brief Computes the Constant-Q Transform magnitude.
SonareError sonare_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out);
/// @brief Computes the Variable-Q Transform magnitude (gamma controls Q).
SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, float gamma,
                       SonareCqtResult* out);
void sonare_free_cqt_result(SonareCqtResult* result);

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

/// @brief Mel spectrogram with an explicit Mel range, so a forward transform can
///        round-trip with the inverse API (sonare_mel_to_stft / _to_audio).
/// @param fmin Minimum Mel frequency in Hz (0.0 keeps the librosa default).
/// @param fmax Maximum Mel frequency in Hz (0.0 = sr/2).
/// @param htk Non-zero to use the HTK Mel formula instead of Slaney.
SonareError sonare_mel_spectrogram_ex(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, int n_mels, float fmin, float fmax,
                                      int htk, SonareMelResult* out);
/// @brief MFCC with an explicit Mel range (see sonare_mel_spectrogram_ex).
SonareError sonare_mfcc_ex(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int n_mels, int n_mfcc, float fmin, float fmax, int htk,
                           SonareMfccResult* out);

// ============================================================================
// Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
// ============================================================================

/* Result of an inverse spectrogram reconstruction. @c data is a row-major
   [rows x n_frames] matrix (rows = n_bins for mel_to_stft, n_mels for
   mfcc_to_mel). Free @c data with sonare_free_inverse_result. */
typedef struct {
  int rows;     /* Number of rows (frequency/Mel bins) */
  int n_frames; /* Number of time frames */
  float* data;  /* rows * n_frames, row-major */
} SonareInverseResult;

/* UNCHECKED-LENGTH CONTRACT (the four inverse functions below):
   each reads exactly (rows * n_frames) floats from its input matrix, where rows
   is n_mels for the mel_* functions and n_mfcc for the mfcc_* functions. The
   functions CANNOT verify that the caller's buffer is actually that long, so a
   wrong n_frames / n_mels / n_mfcc relative to the real allocation is a heap
   over-read (undefined behaviour). Prefer the *_checked variants further below,
   which take an explicit input_length and return SONARE_ERROR_INVALID_PARAMETER
   when it does not equal rows * n_frames. The unchecked forms are retained for
   ABI back-compat; each *_checked variant delegates to its unchecked form after
   validating the length. */

/// @brief Approximate inverse of a Mel filterbank (Mel power -> STFT power).
/// @param mel Mel power spectrogram [n_mels x n_frames] row-major.
/// @param n_mels Number of Mel bands.
/// @param n_frames Number of time frames.
/// @param sample_rate Sample rate of the audio that produced @p mel (Hz).
/// @param n_fft FFT size of the source STFT (sets output bins = n_fft/2 + 1).
/// @param fmin Minimum Mel frequency in Hz (0.0 for librosa default).
/// @param fmax Maximum Mel frequency in Hz (0.0 = sr/2).
/// @param out Receives an [(n_fft/2 + 1) x n_frames] STFT power matrix.
/// @warning Reads n_mels * n_frames floats from @p mel without verifying the
///          buffer length; see the UNCHECKED-LENGTH CONTRACT above. Use
///          sonare_mel_to_stft_checked to have the length validated.
SonareError sonare_mel_to_stft(const float* mel, int n_mels, int n_frames, int sample_rate,
                               int n_fft, float fmin, float fmax, SonareInverseResult* out);

/// @brief Reconstructs audio from a Mel spectrogram via Griffin-Lim.
/// @param mel Mel power spectrogram [n_mels x n_frames] row-major.
/// @param n_mels Number of Mel bands.
/// @param n_frames Number of time frames.
/// @param sample_rate Sample rate of the original audio (Hz).
/// @param n_fft FFT size used for reconstruction.
/// @param hop_length Hop length used for reconstruction.
/// @param fmin Minimum Mel frequency in Hz (0.0 for librosa default).
/// @param fmax Maximum Mel frequency in Hz (0.0 = sr/2).
/// @param n_iter Griffin-Lim iterations (e.g. 32).
/// @param out Receives the reconstructed audio samples (caller frees with
///        sonare_free_floats).
/// @param out_length Receives the number of reconstructed samples.
SonareError sonare_mel_to_audio(const float* mel, int n_mels, int n_frames, int sample_rate,
                                int n_fft, int hop_length, float fmin, float fmax, int n_iter,
                                float** out, size_t* out_length);

/// @brief Inverts MFCC coefficients back to a Mel spectrogram (dB scale).
/// @param mfcc MFCC matrix [n_mfcc x n_frames] row-major.
/// @param n_mfcc Number of MFCCs.
/// @param n_frames Number of time frames.
/// @param n_mels Number of Mel bins to reconstruct.
/// @param out Receives an [n_mels x n_frames] Mel power matrix.
SonareError sonare_mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                               SonareInverseResult* out);

/// @brief Reconstructs audio directly from MFCC via Mel inversion + Griffin-Lim.
/// @param mfcc MFCC matrix [n_mfcc x n_frames] row-major.
/// @param n_mfcc Number of MFCCs.
/// @param n_frames Number of time frames.
/// @param n_mels Number of Mel bins (must match the MFCC source config).
/// @param sample_rate Sample rate of the original audio (Hz).
/// @param n_fft FFT size used for reconstruction.
/// @param hop_length Hop length used for reconstruction.
/// @param fmin Minimum Mel frequency in Hz (0.0 for librosa default).
/// @param fmax Maximum Mel frequency in Hz (0.0 = sr/2).
/// @param n_iter Griffin-Lim iterations (e.g. 32).
/// @param out Receives the reconstructed audio samples (caller frees with
///        sonare_free_floats).
/// @param out_length Receives the number of reconstructed samples.
SonareError sonare_mfcc_to_audio(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                 int sample_rate, int n_fft, int hop_length, float fmin, float fmax,
                                 int n_iter, float** out, size_t* out_length);

/* ----------------------------------------------------------------------------
   Length-checked inverse variants (recommended)

   Each variant takes an explicit @p input_length (number of floats actually
   present in the input matrix) and returns SONARE_ERROR_INVALID_PARAMETER when
   it does not exactly match the size implied by the declared dimensions
   (n_mels * n_frames for the mel_* forms, n_mfcc * n_frames for the mfcc_*
   forms). On a valid length they behave identically to the unchecked forms (and
   delegate to them). This turns a wrong-dimension call from a heap over-read
   into a clean error. The size product is also bounded so a 32-bit (WASM)
   size_t cannot overflow before the comparison. NaN/Inf in the matrix is NOT
   inspected (inverse transforms are tolerant of non-finite cells); only the
   buffer length is validated.
   ---------------------------------------------------------------------------- */

/// @brief Length-checked sonare_mel_to_stft. @p input_length must equal
///        n_mels * n_frames.
SonareError sonare_mel_to_stft_checked(const float* mel, size_t input_length, int n_mels,
                                       int n_frames, int sample_rate, int n_fft, float fmin,
                                       float fmax, SonareInverseResult* out);

/// @brief Length-checked sonare_mel_to_audio. @p input_length must equal
///        n_mels * n_frames.
SonareError sonare_mel_to_audio_checked(const float* mel, size_t input_length, int n_mels,
                                        int n_frames, int sample_rate, int n_fft, int hop_length,
                                        float fmin, float fmax, int n_iter, float** out,
                                        size_t* out_length);

/// @brief Length-checked sonare_mfcc_to_mel. @p input_length must equal
///        n_mfcc * n_frames.
SonareError sonare_mfcc_to_mel_checked(const float* mfcc, size_t input_length, int n_mfcc,
                                       int n_frames, int n_mels, SonareInverseResult* out);

/// @brief Length-checked sonare_mfcc_to_audio. @p input_length must equal
///        n_mfcc * n_frames.
SonareError sonare_mfcc_to_audio_checked(const float* mfcc, size_t input_length, int n_mfcc,
                                         int n_frames, int n_mels, int sample_rate, int n_fft,
                                         int hop_length, float fmin, float fmax, int n_iter,
                                         float** out, size_t* out_length);

/// @brief Frees the matrix held by a SonareInverseResult.
void sonare_free_inverse_result(SonareInverseResult* result);

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

/// @brief Spectral contrast: peak-to-valley energy difference per band per frame.
/// @details Mirrors librosa.feature.spectral_contrast. Output is a row-major
///   matrix [(@p n_bands + 1) x n_frames]; the extra row is the residual band.
///   Free @p out with sonare_free_floats.
/// @param out Receives the freshly allocated [(n_bands + 1) * n_frames] matrix.
/// @param out_rows Receives the number of rows (n_bands + 1).
/// @param out_cols Receives the number of columns (n_frames).
SonareError sonare_spectral_contrast(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, int n_bands, float fmin,
                                     float quantile, float** out, int* out_rows, int* out_cols);

/// @brief Polynomial coefficients fit to each frame's spectrum.
/// @details Mirrors librosa.feature.poly_features. Output is a row-major matrix
///   [(@p order + 1) x n_frames] with coefficients ordered high-to-low. Free
///   @p out with sonare_free_floats.
/// @param out Receives the freshly allocated [(order + 1) * n_frames] matrix.
/// @param out_rows Receives the number of rows (order + 1).
/// @param out_cols Receives the number of columns (n_frames).
SonareError sonare_poly_features(const float* samples, size_t length, int sample_rate, int n_fft,
                                 int hop_length, int order, float** out, int* out_rows,
                                 int* out_cols);

/// @brief Raw zero-crossing indices of a signal (librosa.zero_crossings).
/// @details Unlike sonare_zero_crossing_rate (a per-frame rate), this returns the
///   sample indices @c i where the sign of @c y[i] differs from @c y[i-1]. Free
///   @p out with sonare_free_ints.
/// @param threshold Magnitudes <= threshold are treated as zero.
/// @param ref_magnitude Non-zero scales @p threshold by max(|y|).
/// @param pad Non-zero always reports index 0 as a zero-crossing.
/// @param zero_pos Non-zero treats the sign of zero as positive.
/// @param out Receives the freshly allocated array of zero-crossing indices.
/// @param out_count Receives the number of indices.
SonareError sonare_zero_crossings(const float* samples, size_t length, float threshold,
                                  int ref_magnitude, int pad, int zero_pos, int** out,
                                  size_t* out_count);

/// @brief Per-octave tuning offset estimated from a list of detected pitches.
/// @details Mirrors librosa.pitch_tuning. Non-positive frequencies are ignored.
/// @param frequencies Detected pitch frequencies in Hz.
/// @param length Number of frequencies.
/// @param resolution Tuning resolution in fractions of a bin (e.g. 0.01 = 1 cent).
/// @param bins_per_octave Number of pitch bins per octave (e.g. 12).
/// @param out_tuning Receives the tuning offset in fractions of a bin (-0.5, 0.5].
SonareError sonare_pitch_tuning(const float* frequencies, size_t length, float resolution,
                                int bins_per_octave, float* out_tuning);

/// @brief Global tuning offset of an audio signal (librosa.estimate_tuning).
/// @details Uses piptrack to find spectral peaks, then aggregates via pitch_tuning.
/// @param resolution Tuning resolution in fractions of a bin (e.g. 0.01 = 1 cent).
/// @param bins_per_octave Number of pitch bins per octave (e.g. 12).
/// @param out_tuning Receives the tuning offset in fractions of a bin (-0.5, 0.5].
SonareError sonare_estimate_tuning(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, float resolution, int bins_per_octave,
                                   float* out_tuning);

// ============================================================================
// Features - Pitch
// ============================================================================

/// @param fill_na If non-zero, return 0 for unvoiced f0 frames; otherwise keep NaN.
SonareError sonare_pitch_yin(const float* samples, size_t length, int sample_rate, int frame_length,
                             int hop_length, float fmin, float fmax, float threshold, int fill_na,
                             SonarePitchResult* out);
SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float fmin, float fmax,
                              float threshold, int fill_na, SonarePitchResult* out);

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

/* INPUT-BUFFER POLICY for the compat / transform functions below
   (power_to_db, amplitude_to_db, db_to_power, db_to_amplitude, preemphasis,
   deemphasis, trim_silence, split_silence, frame_signal, pad_center, fix_length,
   peak_pick, vector_normalize, pcen, tonnetz, and the tempogram/plp family):

   - EMPTY input (length == 0) is ALLOWED and yields an empty result
     (*out == NULL, *out_length == 0). This differs from the offline
     audio-analysis entry points (validate_audio_params), where empty audio is
     rejected, because these are pure array transforms with a well-defined empty
     result.
   - A NULL buffer with length > 0 is rejected with SONARE_ERROR_INVALID_PARAMETER.
   - NON-FINITE samples (NaN / Inf) are rejected with
     SONARE_ERROR_INVALID_PARAMETER, matching validate_audio_params, so a NaN can
     never silently propagate through these transforms. */

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
SonareError sonare_tempogram_with_mode(const float* onset_envelope, size_t length, int sample_rate,
                                       int hop_length, int win_length, int center, int norm,
                                       int mode, float** out, size_t* out_length,
                                       int* out_n_frames);
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

#ifdef __cplusplus
}
#endif
