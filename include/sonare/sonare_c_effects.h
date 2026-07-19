#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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
/// Applies a single CONSTANT transposition: the whole buffer is treated as one
/// note at @p current_midi and shifted by (target_midi - current_midi). This is
/// not pitch tracking — it does not follow a time-varying melody. For
/// contour-following correction use @ref sonare_pitch_correct_to_midi_timevarying
/// with a caller-supplied per-frame F0 track.
SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length);
/// @brief Per-frame ("time-varying") correction toward a fixed MIDI target.
/// @details Unlike @ref sonare_pitch_correct_to_midi (one constant transpose),
///          this follows a caller-supplied F0 contour: each of the @p n_frames
///          frames carries an @p f0_hz value (the measured pitch at that frame)
///          and the corrector retunes every voiced frame toward @p target_midi,
///          so vibrato/drift in the source is tracked rather than flattened.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
/// @param voiced_prob Per-frame voicing probability [0,1] (@p n_frames entries),
///                    or NULL to derive it from @p voiced (1.0 / 0.0).
/// @param voiced      Per-frame voiced flags (non-zero = voiced; @p n_frames
///                    entries), or NULL to treat every frame as voiced.
/// @param hop_length  F0 hop in samples (> 0; frame i covers sample i*hop_length).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_to_midi_timevarying(const float* samples, size_t length,
                                                     int sample_rate, const float* f0_hz,
                                                     const float* voiced_prob,
                                                     const int32_t* voiced, size_t n_frames,
                                                     int hop_length, float target_midi, float** out,
                                                     size_t* out_length);

/// @brief Target selector for @ref sonare_pitch_correct_timevarying.
typedef enum {
  SONARE_PITCH_TARGET_FIXED_MIDI = 0,  ///< Retune every voiced frame toward @c target_midi.
  SONARE_PITCH_TARGET_SCALE = 1,       ///< Snap each voiced frame to the nearest scale degree.
} SonarePitchTargetMode;

/// @brief Tunable configuration for @ref sonare_pitch_correct_timevarying.
/// @details Zero-initialising this struct is NOT a valid default — populate it
///          via @ref sonare_pitch_correction_config_default first, then override
///          the fields you care about.
typedef struct {
  int32_t target_mode;         ///< @ref SonarePitchTargetMode.
  float target_midi;           ///< Target note when @c target_mode is FIXED_MIDI ([0,127]).
  int32_t scale_root;          ///< Scale root pitch class (0=C .. 11=B) when target_mode is SCALE.
  uint32_t scale_mode_mask;    ///< 12-bit degree mask, bit i = semitone i above the root enabled.
  float scale_reference_midi;  ///< Reference MIDI anchoring the scale grid (default 69 = A4).
  float retune_amount;         ///< Correction strength [0,1]; 1 = full snap, 0 = bypass.
  float max_correction_semitones;  ///< Hard clamp on per-frame correction magnitude.
  float retune_speed_ms;           ///< Retune IIR time constant (ms); larger = slower glide.
  float vibrato_threshold_cents;   ///< Corrections below this are bypassed to preserve vibrato.
} SonarePitchCorrectionConfig;

/// @brief Fills @p config with the library defaults (major scale, full retune).
/// @details Mirrors the core PitchCorrectionConfig defaults: FIXED_MIDI target,
///          C-major mask, reference A4, retune 1.0, 12-semitone clamp, 50 ms
///          glide, 20-cent vibrato threshold.
SonareError sonare_pitch_correction_config_default(SonarePitchCorrectionConfig* config);

/// @brief Per-frame pitch correction toward a fixed MIDI note OR a musical scale.
/// @details Generalises @ref sonare_pitch_correct_to_midi_timevarying: the same
///          caller-supplied F0 contour drives correction, but @p config selects
///          between a fixed-MIDI target and scale quantisation and exposes the
///          retune-strength / vibrato-preservation knobs. Pass NULL for @p config
///          to use the library defaults.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
/// @param voiced_prob Per-frame voicing probability [0,1], or NULL.
/// @param voiced      Per-frame voiced flags (non-zero = voiced), or NULL.
/// @param hop_length  F0 hop in samples (> 0).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_timevarying(const float* samples, size_t length, int sample_rate,
                                             const float* f0_hz, const float* voiced_prob,
                                             const int32_t* voiced, size_t n_frames, int hop_length,
                                             const SonarePitchCorrectionConfig* config, float** out,
                                             size_t* out_length);
SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length);
/// Move a note region to a new onset sample while preserving its duration.
SonareError sonare_note_move(const float* samples, size_t length, int sample_rate, int onset_sample,
                             int offset_sample, int target_onset_sample, float** out,
                             size_t* out_length);
SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                float pitch_semitones, float formant_factor, float** out,
                                size_t* out_length);
/// @brief Convenience offline wrapper around the realtime voice changer chain.
/// @details Creates a temporary @ref SonareRealtimeVoiceChanger from @p preset,
///          processes the whole mono or interleaved stereo buffer in realtime-
///          sized blocks, and destroys the handle before returning. @p preset
///          may be NULL/empty (neutral-monitor), a preset id, or a full JSON
///          config document accepted by @ref sonare_realtime_voice_changer_create_json.
///          @p channels must be 1 (mono) or 2 (interleaved stereo).
/// @details The chain's processing latency (retune grain + ISP-limiter lookahead)
///          is compensated: an equal-length silent tail is flushed and the
///          leading @c latency_samples of pre-roll are dropped, so the returned
///          buffer is the same length as @p samples AND time-aligned with the
///          input (no head silence / tail truncation).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_voice_change_realtime(const float* samples, size_t length, int sample_rate,
                                         const char* preset, int channels, float** out,
                                         size_t* out_length);

/// @brief Flat POD mirror of @c editing::voice_changer::RealtimeVoiceChangerConfig
///        for C callers that want to avoid the JSON round-trip.
/// @details Field ordering follows the nested C++ struct (top-level →
///          retune → formant → eq → gate → compressor → deesser → reverb →
///          limiter). Values pass through @c normalize_realtime_voice_changer_config
///          before being applied, so out-of-range entries are clamped rather than
///          rejected (matching the JSON entry point).
typedef struct {
  float input_gain_db;
  float output_gain_db;
  float wet_mix;

  float retune_semitones;
  float retune_mix;
  int retune_grain_size;

  float formant_factor;
  float formant_amount;
  float formant_body;
  float formant_brightness;
  float formant_nasal;

  float eq_highpass_hz;
  float eq_body_db;
  float eq_presence_db;
  float eq_air_db;

  float gate_threshold_db;
  float gate_attack_ms;
  float gate_release_ms;
  float gate_range_db;

  float compressor_threshold_db;
  float compressor_ratio;
  float compressor_attack_ms;
  float compressor_release_ms;
  float compressor_makeup_gain_db;

  float deesser_frequency_hz;
  float deesser_threshold_db;
  float deesser_ratio;
  float deesser_range_db;

  float reverb_mix;
  float reverb_time_ms;
  float reverb_damping;
  int reverb_seed;

  float limiter_ceiling_db;
  float limiter_release_ms;

  /// @brief Enables the optional 4x-oversampled inter-sample peak (true-peak)
  ///        limiter as the final output stage (non-zero = enabled). Mirrors
  ///        editing::voice_changer::LimiterConfig::enable_isp_limiter. Defaults
  ///        to 1 (enabled).
  int limiter_enable_isp_limiter;
  /// @brief True-peak ceiling in dBTP applied by the ISP limiter when
  ///        @ref limiter_enable_isp_limiter is non-zero. Mirrors
  ///        editing::voice_changer::LimiterConfig::isp_ceiling_dbtp. Defaults to
  ///        -1.0 dBTP.
  float limiter_isp_ceiling_dbtp;
} SonareRealtimeVoiceChangerConfig;

// Verify the POD struct layout is stable. The ABI version constant
// SONARE_VOICE_CHANGER_ABI_VERSION below MUST be bumped whenever this size
// or any field offset changes. Bindings that rely on POD memcpy across the
// FFI boundary (Rust FFI, raw C ABI consumers) read this size at compile
// time and detect ABI drift before a single byte is exchanged.
//
// Layout: 33 float fields + 3 int fields, every member is 4 bytes and
// 4-byte aligned -> no struct padding on any target we ship. Exact equality
// (not >=) so silent padding insertion fails the check too.
#ifdef __cplusplus
static_assert(sizeof(SonareRealtimeVoiceChangerConfig) == 36u * sizeof(float),
              "SonareRealtimeVoiceChangerConfig unexpected size");
#endif

#include "sonare_c_voice_changer.h"

/// @brief Compile-time mirror of the runtime ABI version returned by
///        @ref sonare_voice_changer_abi_version. Bindings can `static_assert` /
///        `assertEqual` the runtime value against this at attach time.
#define SONARE_VOICE_CHANGER_ABI_VERSION 2u

/// @brief Returns the runtime ABI version of the
///        @ref SonareRealtimeVoiceChangerConfig POD layout.
/// @details Bindings that pass the POD struct across the C ABI (Rust, raw C
///          consumers) call this at attach time and compare against their
///          compile-time expectation; a mismatch means the host libsonare was
///          built against a different struct layout and the POD path would
///          corrupt memory. JSON-based bindings (Node/Python via
///          @ref sonare_realtime_voice_changer_create_json) are tolerant of
///          layout drift and do not need to gate on this.
///
///          Distinct from @ref sonare_engine_abi_version (which tracks the
///          realtime command queue layout) so that voice-changer-only
///          consumers can pin a narrower compatibility envelope.
///
///          Always equals @ref SONARE_VOICE_CHANGER_ABI_VERSION at the time
///          libsonare was built.
uint32_t sonare_voice_changer_abi_version(void);

#include "sonare_c_engine.h"

SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length);

/// @brief Non-negative matrix factorisation of a non-negative spectrogram
///        (mirror of @c sonare::decompose / librosa.decompose.decompose).
/// @details Both output matrices are heap-allocated row-major and MUST be
///          released with @ref sonare_free_floats. @p out_w is the component
///          matrix [n_features x n_components] (length n_features*n_components)
///          and @p out_h is the activation matrix [n_components x n_frames]
///          (length n_components*n_frames). Multi-matrix shape: returned as two
///          flat buffers because sonare_c_types.h has no decompose result struct.
/// @param s Input spectrogram [n_features x n_frames] row-major (non-negative).
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param n_components Target number of components (k). Must be > 0.
/// @param n_iter Number of multiplicative-update iterations. Must be > 0
///        (n_iter==0 would return the raw init matrices and is rejected).
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
/// @note Uses deterministic random initialisation. For the SVD-based NNDSVD
///       warm-start (faster convergence), use @ref sonare_decompose_with_init.
SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, float** out_w, size_t* out_w_length,
                             float** out_h, size_t* out_h_length);

/// @brief Non-negative matrix factorisation with a selectable initialiser
///        (mirror of @c sonare::decompose with the @c init argument).
/// @details Identical to @ref sonare_decompose but exposes the initialisation
///          strategy so callers can opt into the NNDSVD warm-start, which tends
///          to converge in fewer iterations. Both output matrices are
///          heap-allocated row-major and MUST be released with
///          @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major (non-negative).
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param n_components Target number of components (k). Must be > 0.
/// @param n_iter Number of multiplicative-update iterations. Must be > 0.
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param init Initialiser: "random" (default if NULL) or "nndsvd".
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
SonareError sonare_decompose_with_init(const float* s, int n_features, int n_frames,
                                       int n_components, int n_iter, float beta, const char* init,
                                       float** out_w, size_t* out_w_length, float** out_h,
                                       size_t* out_h_length);

/// @brief Nearest-neighbour filter for spectrogram denoising
///        (mirror of @c sonare::nn_filter / librosa.decompose.nn_filter).
/// @details Output is the smoothed spectrogram [n_features x n_frames] row-major
///          (length n_features*n_frames); release with @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major.
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param aggregate Aggregator: "mean", "median", "min" or "max". NULL = "mean".
/// @param k Number of nearest neighbours.
/// @param width Time exclusion half-width. Must be >= 0 (negative widths are
///        rejected, mirroring librosa, instead of silently disabling exclusion).
/// @param out Receives the smoothed spectrogram buffer.
/// @param out_length Receives n_features * n_frames.
SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length);

/// @brief Reorders / concatenates a signal by interval slices
///        (mirror of @c sonare::remix / librosa.effects.remix).
/// @details Each pair (intervals[2*i], intervals[2*i+1]) selects samples
///          [start, end). The output is the concatenation of all slices.
///          Output is heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input signal.
/// @param length Number of samples.
/// @param sample_rate Sample rate (validated, carried for API symmetry).
/// @param intervals Flat array of @p interval_count (start, end) pairs.
/// @param interval_count Number of (start, end) pairs.
/// @param align_zeros Snap boundaries to zero-crossings (non-zero = true).
/// @param out Receives the remixed signal buffer.
/// @param out_length Receives the remixed signal length.
SonareError sonare_remix(const float* samples, size_t length, int sample_rate, const int* intervals,
                         size_t interval_count, int align_zeros, float** out, size_t* out_length);

/// @brief HPSS with residual: separates audio into harmonic, percussive and
///        residual signals (mirror of @c sonare::hpss_with_residual).
/// @details All three outputs share the same @p out_length and @p out_sample_rate
///          (residual = original - harmonic - percussive). Each buffer is
///          heap-allocated and MUST be released with @ref sonare_free_floats.
///          Three-signal shape: emitted as three flat buffers because
///          sonare_c_types.h has no with-residual HPSS result struct.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param kernel_harmonic Horizontal median filter size (odd, >= 3).
/// @param kernel_percussive Vertical median filter size (odd, >= 3).
/// @param out_harmonic Receives the harmonic signal.
/// @param out_percussive Receives the percussive signal.
/// @param out_residual Receives the residual signal.
/// @param out_length Receives the (shared) signal length.
/// @param out_sample_rate Receives the (shared) sample rate.
SonareError sonare_hpss_with_residual(const float* samples, size_t length, int sample_rate,
                                      int kernel_harmonic, int kernel_percussive,
                                      float** out_harmonic, float** out_percussive,
                                      float** out_residual, size_t* out_length,
                                      int* out_sample_rate);

/// @brief Phase-vocoder time-scale modification of audio
///        (STFT -> @c sonare::phase_vocoder -> iSTFT).
/// @details Faithful audio wrapper: computes the STFT, time-stretches the
///          spectrogram with phase coherence, and reconstructs audio. Output is
///          heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param rate Time stretch rate (< 1.0 = slower, > 1.0 = faster). Must be > 0.
/// @param n_fft FFT size used for analysis/synthesis.
/// @param hop_length Hop length used for analysis/synthesis.
/// @param out Receives the time-stretched audio buffer.
/// @param out_length Receives the output length.
SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate, float rate,
                                 int n_fft, int hop_length, float** out, size_t* out_length);

/// @brief How a spectral region op modifies the masked STFT bins.
typedef enum {
  SONARE_SPECTRAL_EDIT_MODE_GAIN = 0,      /* multiply magnitude by 10^(gain_db/20); phase kept */
  SONARE_SPECTRAL_EDIT_MODE_ATTENUATE = 1, /* gain with a (typically negative) gain_db */
  SONARE_SPECTRAL_EDIT_MODE_MUTE = 2,      /* hard zero the masked bins (gain_db ignored) */
  SONARE_SPECTRAL_EDIT_MODE_HEAL = 3,      /* tonal continuation from neighbouring time frames */
} SonareSpectralEditMode;

/// @brief STFT + heal parameters for @ref sonare_spectral_edit.
/// @details Zero-init friendly: every "0 => default" field below picks the
///          documented default so a memset(0) config is the all-defaults case.
typedef struct {
  int n_fft;              /* 0 => default 2048; must be a power of two (>= 2) */
  int hop_length;         /* 0 => default 512; must satisfy 0 < hop <= n_fft/2 */
  int window;             /* SonareWindowType (sonare_c_streaming.h); 0 = Hann */
  int heal_radius_frames; /* 0 => default 2; neighbour frames each side used by Heal */
} SonareSpectralEditConfig;

/// @brief One time x frequency rectangle edit op (POD; ops apply in array order).
typedef struct {
  int64_t start_sample; /* region time start (input samples); clamped to [0, length] */
  int64_t end_sample;   /* region time end, exclusive; clamped to [0, length] */
  float low_hz;         /* region frequency low edge (Hz); clamped to [0, nyquist] */
  float high_hz;        /* region frequency high edge (Hz); <=0 or >= nyquist => nyquist */
  float gain_db;        /* for GAIN/ATTENUATE; ignored by MUTE/HEAL */
  int mode;             /* SonareSpectralEditMode */
} SonareSpectralRegionOp;

/// @brief Region-based spectral editing: STFT -> per-op bin/frame masking -> iSTFT.
/// @details Stateless mono transform; output has the same length/sample rate as the
///          input. @p config may be NULL (all defaults). @p ops may be NULL iff
///          @p n_ops is 0 (identity transform that returns the input). Each op is a
///          time x frequency rectangle applied in order; see @ref SonareSpectralEditMode.
///          The returned array is heap-allocated and MUST be released with
///          @ref sonare_free_floats.
/// @param samples Input audio (mono).
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param config STFT + heal config, or NULL for all defaults.
/// @param ops Array of @p n_ops region ops, or NULL iff @p n_ops == 0.
/// @param n_ops Number of region ops.
/// @param out Receives the edited audio buffer.
/// @param out_length Receives the output length.
SonareError sonare_spectral_edit(const float* samples, size_t length, int sample_rate,
                                 const SonareSpectralEditConfig* config,
                                 const SonareSpectralRegionOp* ops, size_t n_ops, float** out,
                                 size_t* out_length);

#ifdef __cplusplus
}
#endif
