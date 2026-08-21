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
/// @brief Analyzes an impulse response with an explicit decay-fit range.
/// @details @p min_decay_db must be finite and positive.  The legacy
///          sonare_analyze_impulse_response entry point delegates here with
///          the library default of 30 dB.
SonareError sonare_analyze_impulse_response_ex(const float* samples, size_t length, int sample_rate,
                                               int n_octave_bands, float min_decay_db,
                                               SonareAcousticResult* out);
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
/// @brief Detects a continuous chord/N.C. timeline.
/// @details Final correlations below @p threshold (which must be in [0, 1])
///   produce @c SONARE_CHORD_UNKNOWN segments instead of a guessed chord.
SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 int use_triads_only, int n_fft, int hop_length, int use_beat_sync,
                                 SonareChordAnalysisResult* out);
/// @brief Extended chord detection with the same UNKNOWN/N.C. threshold contract.
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
/// @brief Extracts the melody contour from monophonic audio via plain YIN
///   (left-aligned, no Viterbi smoothing). Shorthand for sonare_analyze_melody_ex
///   with use_pyin=0.
SonareError sonare_analyze_melody(const float* samples, size_t length, int sample_rate, float fmin,
                                  float fmax, int frame_length, int hop_length, float threshold,
                                  SonareMelodyResult* out);
/// @brief Extracts the melody contour with selectable tracker.
/// @param use_pyin Non-zero selects the pYIN tracker (Viterbi-smoothed, less
///   octave-jumpy) instead of plain per-frame YIN.
/// @param center When use_pyin is set, non-zero zero-pads by frame_length/2
///   so frame i is centered at i*hop_length (matches librosa.pyin(center=True));
///   zero left-aligns. Ignored when use_pyin is 0 (plain YIN is always
///   left-aligned).
SonareError sonare_analyze_melody_ex(const float* samples, size_t length, int sample_rate,
                                     float fmin, float fmax, int frame_length, int hop_length,
                                     float threshold, int use_pyin, int center,
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
// Core - Synthetic audio generation
// ============================================================================

/// @brief Generates a sine tone. Free @p out with sonare_free_floats.
SonareError sonare_tone(float frequency, int sample_rate, float duration, float phase,
                        float amplitude, float** out, size_t* out_length);
/// @brief Generates a linear or exponential chirp. Free @p out with sonare_free_floats.
SonareError sonare_chirp(float fmin, float fmax, int sample_rate, float duration, int linear,
                         float** out, size_t* out_length);
/// @brief Generates decaying sine clicks at times in seconds. Free @p out with sonare_free_floats.
SonareError sonare_clicks(const float* times, size_t time_count, int sample_rate, int length,
                          float frequency, float click_duration, float** out, size_t* out_length);

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
/// @brief Computes a faster pseudo-CQT magnitude approximation.
SonareError sonare_pseudo_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out);
/// @brief Computes a hybrid CQT magnitude.
SonareError sonare_hybrid_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out);
/// @brief Computes the Variable-Q Transform magnitude (gamma controls Q).
/// @param gamma Bandwidth offset in Hz. Negative or NaN selects the
/// librosa-compatible ERB-derived automatic value; zero selects standard CQT.
SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, float gamma,
                       SonareCqtResult* out);
void sonare_free_cqt_result(SonareCqtResult* result);

/// @brief Reconstructs mono audio from row-major CQT magnitude with Griffin-Lim.
/// @details The matrix must contain @p n_bins * @p n_frames elements. C callers
///   that cannot prove the allocation size should use @ref sonare_cqt_to_audio_checked.
///   Iterations must be in [1, 256]. The output is owned by the caller and
///   released with @ref sonare_free_floats.
SonareError sonare_cqt_to_audio(const float* magnitude, int n_bins, int n_frames, int sample_rate,
                                int hop_length, float fmin, int bins_per_octave, int n_iter,
                                float** out, size_t* out_length);

/// @brief Length-checked sonare_cqt_to_audio.
/// @details @p input_length must equal @p n_bins * @p n_frames.
SonareError sonare_cqt_to_audio_checked(const float* magnitude, size_t input_length, int n_bins,
                                        int n_frames, int sample_rate, int hop_length, float fmin,
                                        int bins_per_octave, int n_iter, float** out,
                                        size_t* out_length);

/// @brief Reconstructs mono audio from row-major VQT magnitude with Griffin-Lim.
/// @details Shape, ownership, and iteration rules match @ref sonare_cqt_to_audio.
SonareError sonare_vqt_to_audio(const float* magnitude, int n_bins, int n_frames, int sample_rate,
                                int hop_length, float fmin, int bins_per_octave, float gamma,
                                int n_iter, float** out, size_t* out_length);

/// @brief Length-checked sonare_vqt_to_audio.
/// @details @p input_length must equal @p n_bins * @p n_frames.
SonareError sonare_vqt_to_audio_checked(const float* magnitude, size_t input_length, int n_bins,
                                        int n_frames, int sample_rate, int hop_length, float fmin,
                                        int bins_per_octave, float gamma, int n_iter, float** out,
                                        size_t* out_length);

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
/// @param lifter Cepstral liftering coefficient (0.0 = no liftering, the librosa
///   default). NOTE: the inverse entry points (sonare_mfcc_to_mel /
///   sonare_mfcc_to_audio) do not undo liftering, so inverse reconstruction of a
///   liftered MFCC is only exact for lifter == 0.
SonareError sonare_mfcc_ex(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int n_mels, int n_mfcc, float fmin, float fmax, int htk,
                           float lifter, SonareMfccResult* out);

/// @brief Computes first-order delta features from a row-major feature matrix.
/// @details Mirrors @c MelSpectrogram::delta and librosa.feature.delta with a
///          first-order regression. @p features is [n_features x n_frames].
///          @p width must be odd and at least 3. The returned matrix has the
///          same shape and is released with @ref sonare_free_floats.
SonareError sonare_mel_delta(const float* features, int n_features, int n_frames, int width,
                             float** out);

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
/// @brief HTK-aware sonare_mel_to_stft variant.
/// @param htk Non-zero to rebuild the inverse Mel filterbank with HTK Mel
///        spacing. Zero preserves the Slaney-compatible legacy contract.
SonareError sonare_mel_to_stft_ex(const float* mel, int n_mels, int n_frames, int sample_rate,
                                  int n_fft, float fmin, float fmax, int htk,
                                  SonareInverseResult* out);

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
/// @brief Reconstruct mono audio from an STFT magnitude matrix with Griffin-Lim.
/// @details @p magnitude is row-major [n_bins x n_frames]; @p input_length must
///   equal n_bins * n_frames, n_bins must equal n_fft / 2 + 1, and momentum is in [0, 1).
SonareError sonare_griffin_lim(const float* magnitude, size_t input_length, int n_bins,
                               int n_frames, int n_fft, int hop_length, int sample_rate, int n_iter,
                               float momentum, float** out, size_t* out_length);
/// @brief HTK-aware sonare_mel_to_audio variant.
/// @param htk Non-zero to rebuild the inverse Mel filterbank with HTK Mel
///        spacing. Zero preserves the Slaney-compatible legacy contract.
SonareError sonare_mel_to_audio_ex(const float* mel, int n_mels, int n_frames, int sample_rate,
                                   int n_fft, int hop_length, float fmin, float fmax, int htk,
                                   int n_iter, float** out, size_t* out_length);

/// @brief Inverts MFCC coefficients back to a Mel power spectrogram.
/// @param mfcc MFCC matrix [n_mfcc x n_frames] row-major.
/// @param n_mfcc Number of MFCCs.
/// @param n_frames Number of time frames.
/// @param n_mels Number of Mel bins to reconstruct.
/// @param out Receives an [n_mels x n_frames] Mel power matrix.
SonareError sonare_mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                               SonareInverseResult* out);
/// @brief Lifter-aware sonare_mfcc_to_mel variant.
/// @param lifter Lifter used by the forward MFCC transform. Zero means no
///        liftering; positive values undo the matching sinusoidal lift.
SonareError sonare_mfcc_to_mel_ex(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                  float lifter, SonareInverseResult* out);

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
/// @brief HTK-aware sonare_mfcc_to_audio variant.
/// @param htk Non-zero to rebuild the intermediate inverse Mel filterbank with
///        HTK Mel spacing. Zero preserves the Slaney-compatible legacy contract.
SonareError sonare_mfcc_to_audio_ex(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                    int sample_rate, int n_fft, int hop_length, float fmin,
                                    float fmax, int htk, int n_iter, float** out,
                                    size_t* out_length);
/// @brief HTK- and lifter-aware sonare_mfcc_to_audio variant.
SonareError sonare_mfcc_to_audio_ex2(const float* mfcc, int n_mfcc, int n_frames, int n_mels,
                                     int sample_rate, int n_fft, int hop_length, float fmin,
                                     float fmax, int htk, float lifter, int n_iter, float** out,
                                     size_t* out_length);

/* ----------------------------------------------------------------------------
   Length-checked inverse variants (recommended)

   Each variant takes an explicit @p input_length (number of floats actually
   present in the input matrix) and returns SONARE_ERROR_INVALID_PARAMETER when
   it does not exactly match the size implied by the declared dimensions
   (n_mels * n_frames for the mel_* forms, n_mfcc * n_frames for the mfcc_*
   forms). On a valid length they behave identically to the unchecked forms (and
   delegate to them). This turns a wrong-dimension call from a heap over-read
   into a clean error. The size product is also bounded so a 32-bit (WASM)
   size_t cannot overflow before the comparison. NaN/Inf cells are rejected
   uniformly before reconstruction so they cannot poison the inverse pipeline.
   ---------------------------------------------------------------------------- */

/// @brief Length-checked sonare_mel_to_stft. @p input_length must equal
///        n_mels * n_frames.
SonareError sonare_mel_to_stft_checked(const float* mel, size_t input_length, int n_mels,
                                       int n_frames, int sample_rate, int n_fft, float fmin,
                                       float fmax, SonareInverseResult* out);
SonareError sonare_mel_to_stft_checked_ex(const float* mel, size_t input_length, int n_mels,
                                          int n_frames, int sample_rate, int n_fft, float fmin,
                                          float fmax, int htk, SonareInverseResult* out);

/// @brief Length-checked sonare_mel_to_audio. @p input_length must equal
///        n_mels * n_frames.
SonareError sonare_mel_to_audio_checked(const float* mel, size_t input_length, int n_mels,
                                        int n_frames, int sample_rate, int n_fft, int hop_length,
                                        float fmin, float fmax, int n_iter, float** out,
                                        size_t* out_length);
SonareError sonare_mel_to_audio_checked_ex(const float* mel, size_t input_length, int n_mels,
                                           int n_frames, int sample_rate, int n_fft, int hop_length,
                                           float fmin, float fmax, int htk, int n_iter, float** out,
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
SonareError sonare_mfcc_to_audio_checked_ex(const float* mfcc, size_t input_length, int n_mfcc,
                                            int n_frames, int n_mels, int sample_rate, int n_fft,
                                            int hop_length, float fmin, float fmax, int htk,
                                            int n_iter, float** out, size_t* out_length);

/// @brief Frees the matrix held by a SonareInverseResult.
void sonare_free_inverse_result(SonareInverseResult* result);

// ============================================================================
// Features - Chroma
// ============================================================================

/// @brief STFT chromagram (librosa.feature.chroma_stft).
/// @details The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
///   librosa.feature.chroma_stft, which estimates tuning from the signal when
///   none is supplied, this entry point does NOT auto-estimate and exposes no
///   tuning argument; sharp/flat (non-A440) recordings smear across pitch classes
///   accordingly. Estimate tuning separately via @ref sonare_estimate_tuning if a
///   non-A440 reference matters for downstream key/chord detection.
SonareError sonare_chroma(const float* samples, size_t length, int sample_rate, int n_fft,
                          int hop_length, SonareChromaResult* out);
SonareError sonare_chroma_cens(const float* samples, size_t length, int sample_rate, int hop_length,
                               int n_chroma, SonareChromaResult* out);
/// @brief Extended CENS chromagram with configurable CQT resolution.
SonareError sonare_chroma_cens_ex(const float* samples, size_t length, int sample_rate,
                                  int hop_length, int n_chroma, int bins_per_octave,
                                  SonareChromaResult* out);
/// @brief Constant-Q chromagram (librosa.feature.chroma_cqt).
/// @details Fixed tuning of 0 (concert A440); no auto-tuning estimation, matching
///   the other chroma entry points. Use @ref sonare_estimate_tuning separately if
///   a non-A440 reference matters.
SonareError sonare_chroma_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              int n_chroma, SonareChromaResult* out);
/// @brief Extended Constant-Q chromagram with configurable bins per octave.
/// @details @p bins_per_octave must be a positive multiple of @p n_chroma.
SonareError sonare_chroma_cqt_ex(const float* samples, size_t length, int sample_rate,
                                 int hop_length, int n_chroma, int bins_per_octave,
                                 SonareChromaResult* out);
SonareError sonare_bass_chroma(const float* samples, size_t length, int sample_rate, int hop_length,
                               int n_chroma, SonareChromaResult* out);

// ============================================================================
// Features - Spectral (each returns a float array of per-frame values)
// ============================================================================

SonareError sonare_spectral_centroid(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count);
SonareError sonare_spectral_bandwidth(const float* samples, size_t length, int sample_rate,
                                      int n_fft, int hop_length, float** out, size_t* out_count);
/// @brief Spectral bandwidth with a configurable Minkowski exponent.
/// @details @p p must be finite and positive; 2.0 reproduces the legacy API.
SonareError sonare_spectral_bandwidth_ex(const float* samples, size_t length, int sample_rate,
                                         int n_fft, int hop_length, float p, float** out,
                                         size_t* out_count);
SonareError sonare_spectral_rolloff(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float roll_percent, float** out,
                                    size_t* out_count);
SonareError sonare_spectral_flatness(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int hop_length, float** out, size_t* out_count);
/// @brief Unsigned L1 spectral flux envelope with a positive frame lag.
SonareError sonare_spectral_flux(const float* samples, size_t length, int sample_rate, int n_fft,
                                 int hop_length, int lag, float** out, size_t* out_count);
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

/// @brief Backtrack onset event indices to local minima in an energy curve.
/// @details Mirrors librosa.onset.onset_backtrack. Free @p out with sonare_free_ints.
SonareError sonare_onset_backtrack(const int* events, size_t event_count, const float* energy,
                                   size_t energy_count, int** out, size_t* out_count);

/// @brief Per-octave tuning offset estimated from a list of detected pitches.
/// @details Mirrors librosa.pitch_tuning. Non-positive frequencies are ignored.
/// @param frequencies Detected pitch frequencies in Hz.
/// @param length Number of frequencies.
/// @param resolution Tuning resolution in fractions of a bin (e.g. 0.01 = 1 cent).
/// @param bins_per_octave Number of pitch bins per octave (e.g. 12).
/// @param out_tuning Receives the tuning offset in fractions of a bin
///        [-0.5, 0.5). -0.5 is attainable (a pitch half a bin flat); +0.5 is
///        not, because a residual of +0.5 wraps to -0.5.
SonareError sonare_pitch_tuning(const float* frequencies, size_t length, float resolution,
                                int bins_per_octave, float* out_tuning);

/// @brief Global tuning offset of an audio signal (librosa.estimate_tuning).
/// @details Uses piptrack to find spectral peaks, then aggregates via pitch_tuning.
/// @param resolution Tuning resolution in fractions of a bin (e.g. 0.01 = 1 cent).
/// @param bins_per_octave Number of pitch bins per octave (e.g. 12).
/// @param out_tuning Receives the tuning offset in fractions of a bin
///        [-0.5, 0.5). -0.5 is attainable (a pitch half a bin flat); +0.5 is
///        not, because a residual of +0.5 wraps to -0.5.
SonareError sonare_estimate_tuning(const float* samples, size_t length, int sample_rate, int n_fft,
                                   int hop_length, float resolution, int bins_per_octave,
                                   float* out_tuning);

/// @brief Detects per-frame spectral pitch peaks (librosa.piptrack).
/// @details @p pitches and @p magnitudes are row-major [n_bins x n_frames]
///          matrices and must each be released with @ref sonare_free_floats.
SonareError sonare_piptrack(const float* samples, size_t length, int sample_rate, int n_fft,
                            int hop_length, float fmin, float fmax, float threshold,
                            int* out_n_bins, int* out_n_frames, float** out_pitches,
                            float** out_magnitudes);

/// @brief Owned result of @ref sonare_reassigned_spectrogram.
/// @details All three arrays are row-major [n_bins x n_frames] and are released
///          together with @ref sonare_free_reassigned_spectrogram_result.
typedef struct SonareReassignedSpectrogramResult {
  int n_bins;
  int n_frames;
  float* magnitude;
  float* times;
  float* frequencies;
} SonareReassignedSpectrogramResult;

/// @brief Computes an Auger-Flandrin reassigned spectrogram.
/// @param ref_power Bins with lower power use ordinary bin coordinates, or NaN
///        when @p fill_nan is non-zero.
SonareError sonare_reassigned_spectrogram(const float* samples, size_t length, int sample_rate,
                                          int n_fft, int hop_length, float ref_power, int fill_nan,
                                          SonareReassignedSpectrogramResult* out);
/// @brief Releases all arrays in a reassigned-spectrogram result and clears it.
void sonare_free_reassigned_spectrogram_result(SonareReassignedSpectrogramResult* result);

// ============================================================================
// Features - Segmentation
// ============================================================================

/// @brief Heap-owned row-major float matrix returned by segmentation APIs.
typedef struct SonareSegmentMatrix {
  int rows;
  int cols;
  float* values;
} SonareSegmentMatrix;

/// @brief Heap-owned integer vector returned by segmentation APIs.
typedef struct SonareSegmentIndices {
  int* values;
  size_t count;
} SonareSegmentIndices;

/// @brief Cross-similarity of column-feature matrices. `metric` is `cosine` or
/// `euclidean`; `mode` is `connectivity` or `affinity`.
SonareError sonare_segment_cross_similarity(const float* x, int x_rows, int x_cols, const float* y,
                                            int y_rows, int y_cols, int k, const char* metric,
                                            const char* mode, SonareSegmentMatrix* out);
SonareError sonare_segment_recurrence_matrix(const float* data, int rows, int cols, int k,
                                             int width, int sym, const char* metric,
                                             const char* mode, SonareSegmentMatrix* out);
SonareError sonare_segment_recurrence_to_lag(const float* recurrence, int n, int pad,
                                             SonareSegmentMatrix* out);
SonareError sonare_segment_lag_to_recurrence(const float* lag, int n_rows, int n_lags,
                                             SonareSegmentMatrix* out);
SonareError sonare_segment_subsegment(const float* data, int rows, int cols, const int* boundaries,
                                      size_t boundary_count, int n_segments,
                                      SonareSegmentIndices* out);
SonareError sonare_segment_agglomerative(const float* data, int rows, int cols, int k,
                                         const char* linkage, SonareSegmentIndices* out);
SonareError sonare_segment_path_enhance(const float* recurrence, int n, int win, int max_ratio,
                                        int min_ratio, int n_filters, SonareSegmentMatrix* out);
void sonare_free_segment_matrix(SonareSegmentMatrix* result);
void sonare_free_segment_indices(SonareSegmentIndices* result);

// ============================================================================
// Features - Pitch
// ============================================================================

/// @brief Estimate f0 with YIN.
/// @details Like librosa.yin, every complete frame receives a finite frequency estimate;
/// voiced_flag reports whether the threshold was crossed. fill_na is retained for ABI
/// compatibility and has no effect on YIN output.
SonareError sonare_pitch_yin(const float* samples, size_t length, int sample_rate, int frame_length,
                             int hop_length, float fmin, float fmax, float threshold, int fill_na,
                             SonarePitchResult* out);
/// @brief Estimate f0 with probabilistic YIN (librosa.pyin).
/// @param fill_na If non-zero, return 0 for unvoiced pYIN f0 frames; otherwise keep NaN.
/// @note @c out->voiced_flag is the voicing decision — the Viterbi path's
///       voiced/unvoiced state — and is what a consumer should gate on.
///
///       @c out->voiced_prob is NOT a signal-quality confidence. It is pYIN's
///       per-frame voiced OBSERVATION MASS (the same quantity librosa returns):
///       the summed probability of the frame's voiced pitch hypotheses. That
///       mass depends on how many periods of the pitch fit inside
///       @p frame_length, because the CMNDF troughs of a long period computed
///       over a short frame are shallower. For a fixed @p frame_length it
///       therefore rises monotonically with f0 even when the signal quality is
///       identical: a steady three-harmonic tone measured at 2048 samples /
///       48 kHz averages well under 0.1 at C2 and around 0.5 at C5, with every
///       frame flagged voiced throughout.
///
///       Two consequences worth designing around:
///        - a fixed 0.5 threshold on @c voiced_prob (the default in
///          @ref sonare_note_segments) drops entire low registers. Pass
///          @c voiced_flag as 0.0/1.0 there, or set the config's
///          @c voiced_threshold.
///        - @c voiced_prob is not a correction weight.
///          @ref sonare_pitch_correct_timevarying does not scale its correction
///          by it; it uses it only to derive voicing when no explicit @c voiced
///          array is supplied.
SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              int frame_length, int hop_length, float fmin, float fmax,
                              float threshold, int fill_na, SonarePitchResult* out);

/// @brief Versioned configuration for @ref sonare_note_segments.
/// @details Zero-initialize for the defaults (50 cents, 30 ms, A4 = 440 Hz,
///          voiced threshold 0.5). @c struct_version 0 and 1 select the
///          version-1 layout; 2 additionally honours @c voiced_threshold.
typedef struct {
  int struct_version;
  float segmentation_threshold_cents;
  float min_note_ms;
  float reference_hz;
  /* --- struct_version 2 --- */
  /* Value of voiced_prob at or above which a frame counts as voiced. 0 keeps
     the default 0.5. Ignored unless struct_version is 2, so a caller passing a
     version-1 struct is unaffected.

     Raise or lower this when the caller's probability track is not a 0/1 flag.
     In particular, pYIN's voiced_prob is a frame's voiced observation mass and
     rises with F0 for a fixed frame_length, so the 0.5 default silently drops
     low-register material: see sonare_pitch_pyin. Passing the pYIN voiced_flag
     as 0.0/1.0 is the recommended form and needs no threshold change. */
  float voiced_threshold;
} SonareNoteSegmenterConfig;

/// One monophonic note region detected from an F0 track. Frame bounds are
/// half-open (@c [frame_start, frame_end)); seconds use the supplied frame rate.
typedef struct {
  int frame_start;
  int frame_end;
  float start_seconds;
  float end_seconds;
  float median_cents;
} SonareNoteSegment;

/// Heap-owned note-segmentation output. Release with
/// @ref sonare_free_note_segments.
typedef struct {
  SonareNoteSegment* segments;
  size_t count;
} SonareNoteSegmentsResult;

/// @brief Segment a monophonic F0 track into stable note regions.
/// @param f0_hz F0 values in Hz. Every value must be finite and non-negative;
///        zero denotes an unvoiced frame.
/// @param f0_count Number of F0 frames; must be non-zero.
/// @param voiced_prob Per-frame voicing values in [0, 1]. A value below the
///        config's @c voiced_threshold (default 0.5) is treated as unvoiced;
///        @p voiced_prob_count must equal @p f0_count.
///        Pass @ref sonare_pitch_pyin's @c voiced_flag as 0.0/1.0 here.
///        Passing its @c voiced_prob instead returns no segments at all for
///        low-register material, because that value is a frame's voiced
///        observation mass and rises with F0 rather than with signal quality.
/// @param frame_rate Number of F0 frames per second; must be finite and > 0.
/// @param config Optional versioned configuration; NULL selects defaults.
/// @param out Receives a heap-owned result, cleared before validation. Empty
///        segmentation is returned as (@c NULL, 0).
SonareError sonare_note_segments(const float* f0_hz, size_t f0_count, const float* voiced_prob,
                                 size_t voiced_prob_count, float frame_rate,
                                 const SonareNoteSegmenterConfig* config,
                                 SonareNoteSegmentsResult* out);
void sonare_free_note_segments(SonareNoteSegmentsResult* result);

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
   deemphasis, frame_signal, pad_center, fix_length, peak_pick,
   vector_normalize, pcen, tonnetz, and the tempogram/plp family):

   - EMPTY input (length == 0) is ALLOWED and yields an empty result
     (*out == NULL, *out_length == 0). This differs from the offline
     audio-analysis entry points (validate_audio_params), where empty audio is
     rejected, because these are pure array transforms with a well-defined empty
     result.
   - A NULL buffer with length > 0 is rejected with SONARE_ERROR_INVALID_PARAMETER.
   - NON-FINITE samples (NaN / Inf) are rejected with
     SONARE_ERROR_INVALID_PARAMETER, matching validate_audio_params, so a NaN can
     never silently propagate through these transforms.

   EMPTY-INPUT EXCEPTIONS: sonare_trim_silence, sonare_split_silence,
   sonare_fix_frames and sonare_tempogram_ratio reject an empty input with
   SONARE_ERROR_INVALID_PARAMETER. For each of them the empty result is already
   the encoding of a real measurement (an all-silent signal, an empty frame list,
   a ratio with no autocorrelation energy), so accepting an empty input would
   make "no data was supplied" indistinguishable from that measurement. Their
   individual contracts are documented at each declaration below. */

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

/// @brief Trims leading and trailing silence (librosa.effects.trim).
/// @details An entirely silent input succeeds and reports the all-silent result:
///   @c *out == NULL, @c *out_length == 0, @c *start_sample == @c *end_sample == 0.
///   An EMPTY input (@p length == 0) is rejected with
///   SONARE_ERROR_INVALID_PARAMETER so it cannot be confused with that result.
SonareError sonare_trim_silence(const float* samples, size_t length, float top_db, int frame_length,
                                int hop_length, float** out, size_t* out_length, int* start_sample,
                                int* end_sample);
/// @brief Lists non-silent intervals as flattened (start, end) pairs.
/// @details An entirely silent input succeeds with zero intervals
///   (@c *out_intervals == NULL, @c *out_interval_count == 0). An EMPTY input
///   (@p length == 0) is rejected with SONARE_ERROR_INVALID_PARAMETER so it
///   cannot be confused with that result.
SonareError sonare_split_silence(const float* samples, size_t length, float top_db,
                                 int frame_length, int hop_length, int** out_intervals,
                                 size_t* out_interval_count);

SonareError sonare_frame_signal(const float* samples, size_t length, int frame_length,
                                int hop_length, float** out, size_t* out_length, int* out_n_frames);
SonareError sonare_pad_center(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length);
SonareError sonare_fix_length(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length);
/// @brief Clamps, pads and de-duplicates frame indices (librosa.util.fix_frames).
/// @details An EMPTY @p frames (@p length == 0) is rejected with
///   SONARE_ERROR_INVALID_PARAMETER. With @p pad set, the padded result would be
///   just the bounds (@p x_min, and @p x_max when >= 0) — values a caller cannot
///   tell apart from real detected frames, so an empty onset or beat list would
///   silently gain a frame at @p x_min. The result is never empty.
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
/// @brief Multi-band onset strength envelope from audio.
/// @details Output is [n_bands x n_frames] row-major. @p out_n_frames receives
///   the frame count so callers can derive the matrix shape.
SonareError sonare_onset_strength_multi(const float* samples, size_t length, int sr, int n_fft,
                                        int hop_length, int n_mels, int n_bands, float** out,
                                        size_t* out_length, int* out_n_frames);

/// @brief Fourier (FFT-based) tempogram of an onset envelope.
/// @details Returns a magnitude matrix [n_bins x n_frames] row-major, where
///   n_bins = win_length / 2 + 1 (derivable as out_length / out_n_frames).
SonareError sonare_fourier_tempogram(const float* onset_envelope, size_t length, int sr,
                                     int hop_length, int win_length, int center, int norm,
                                     float** out, size_t* out_length, int* out_n_frames);

/// @brief Aggregated tempogram values at integer tempo ratios of a reference tempo.
/// @details If @p factors is NULL or @p n_factors is 0, the library default
///   factors {0.5, 1, 2, 3, 4} are used. The output contains one value per factor.
///
///   Rejected with SONARE_ERROR_INVALID_PARAMETER: an EMPTY @p tempogram_data
///   (@p length == 0), a @p length below @p win_length (fewer than one frame),
///   and any @p factors entry that is not finite and > 0. A zero in the output
///   means "no autocorrelation energy at that lag", so none of these cases may
///   answer with a zero-filled vector. Supplying fewer factors than requested is
///   not possible: the output length always equals the effective factor count.
SonareError sonare_tempogram_ratio(const float* tempogram_data, size_t length, int win_length,
                                   int sr, int hop_length, const float* factors, size_t n_factors,
                                   float** out, size_t* out_length);

/// @brief NNLS chroma from audio (12 x n_frames row-major).
SonareError sonare_nnls_chroma(const float* samples, size_t length, int sr, float** out,
                               size_t* out_length, int* out_n_frames);
SonareError sonare_nnls_chroma_ex(const float* samples, size_t length, int sr,
                                  int enable_stft_blend, float stft_blend_weight,
                                  int stft_blend_n_fft, float** out, size_t* out_length,
                                  int* out_n_frames);
/// @brief NNLS chroma with an explicit CQT hop length.
/// @details The legacy @c sonare_nnls_chroma_ex entry point delegates here
///          with a 512-sample hop.
SonareError sonare_nnls_chroma_ex2(const float* samples, size_t length, int sr,
                                   int enable_stft_blend, float stft_blend_weight,
                                   int stft_blend_n_fft, int hop_length, float** out,
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
