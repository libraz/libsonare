#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/// @brief Returns the channel-strip insert / FX processor names that
///        sonare_mixing scene inserts can build, separated by '\n'. Includes the
///        creative effects.* reverbs / modulation / delay when the build has FX
///        support. Use these to discover valid insert names instead of
///        hardcoding magic strings.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it (mirrors
///          @ref sonare_mastering_processor_names).
const char* sonare_mastering_insert_names(void);
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
  const char* name;
  float target_lufs;
  float ceiling_db;
} SonareStreamingPlatform;

/// @brief Preview platform normalization gain and ceiling risk as JSON.
/// @details Pass NULL/0 for @p platforms to use the built-in platform list.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_streaming_preview(const float* samples, size_t length, int sample_rate,
                                               const SonareStreamingPlatform* platforms,
                                               size_t platform_count, char** json_out);

/// @brief Analyze audio and suggest a mastering chain as JSON.
/// @details @p params accepts targetLufs, ceilingDb, enableRepair,
/// and preferStreamingSafe. The returned string must be released with
/// sonare_free_string().
SonareError sonare_mastering_assistant_suggest(const float* samples, size_t length, int sample_rate,
                                               const SonareMasteringParam* params,
                                               size_t param_count, char** json_out);

/// @brief Analyze audio and return mastering assistant profile JSON.
/// @details @p params accepts nFft, hopLength, and truePeakOversample.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_audio_profile(const float* samples, size_t length, int sample_rate,
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

// ----------------------------------------------------------------------------
// Mastering: offline repair processors (declick, denoise_classical)
// ----------------------------------------------------------------------------

// Algorithm modes for sonare_mastering_repair_denoise_classical.
#define SONARE_DENOISE_MODE_LOG_MMSE 0              // Ephraim-Malah LSA (1985)
#define SONARE_DENOISE_MODE_MMSE_STSA 1             // Ephraim-Malah STSA (1984)
#define SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION 2  // Berouti spectral subtraction (1979)

// Noise PSD estimators for sonare_mastering_repair_denoise_classical.
#define SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE 0  // Quietest-frames quantile
#define SONARE_DENOISE_NOISE_ESTIMATOR_MCRA 1      // Minimum-controlled recursive averaging
#define SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA 2     // Improved MCRA with SPP gating

/// @brief Flat POD mirror of @c mastering::repair::DeclickConfig.
/// @details Pass NULL to @ref sonare_mastering_repair_declick to use library defaults.
typedef struct {
  float threshold;           // amplitude threshold vs LPC prediction (default 0.8)
  float neighbor_ratio;      // ratio vs neighbour amplitude (default 4.0)
  size_t max_click_samples;  // max click run length in samples (default 8)
  int lpc_order;             // LPC order used for prediction (default 20)
  float residual_ratio;      // residual / signal threshold (default 8.0)
} SonareDeclickConfig;

/// @brief Flat POD mirror of @c mastering::repair::DenoiseClassicalConfig.
/// @details Pass NULL to @ref sonare_mastering_repair_denoise_classical to use library
///          defaults. The library validates @c n_fft (must be a power of two) and
///          @c hop_length (> 0); other fields are clamped by the underlying processor.
typedef struct {
  int mode;                         // SONARE_DENOISE_MODE_*
  int noise_estimator;              // SONARE_DENOISE_NOISE_ESTIMATOR_*
  int n_fft;                        // STFT size (default 1024, power of two)
  int hop_length;                   // hop in samples (default 256)
  float dd_alpha;                   // decision-directed SNR smoothing (default 0.98)
  float gain_floor;                 // minimum bin gain, linear (default 0.05)
  float over_subtraction;           // Berouti alpha (SpectralSubtraction only)
  float spectral_floor;             // Berouti beta (SpectralSubtraction only)
  float noise_estimation_quantile;  // noise-only frame fraction (default 0.1)
  int speech_presence_gain;         // 0/1 (default 1)
  int gain_smoothing;               // 0/1 (default 1)
} SonareDenoiseClassicalConfig;

/// @brief Offline LPC-based declicker.
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_declick(const float* samples, size_t length, int sample_rate,
                                            const SonareDeclickConfig* config, float** out,
                                            size_t* out_length);

/// @brief Offline STFT-domain classical denoiser
///        (LogMMSE / MMSE-STSA / SpectralSubtraction).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_denoise_classical(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareDenoiseClassicalConfig* config,
                                                      float** out, size_t* out_length);

/// @brief Flat POD mirror of @c mastering::repair::DeclipConfig.
typedef struct {
  float clip_threshold;  // amplitude above which a sample is considered clipped (default 0.98)
  int lpc_order;         // LPC order used for prediction (default 36)
  int iterations;        // LPC reconstruction iterations (default 2)
  float lpc_blend;       // LPC vs interpolation blend (default 0.65)
} SonareDeclipConfig;

/// @brief Offline LPC-based declipper.
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_declip(const float* samples, size_t length, int sample_rate,
                                           const SonareDeclipConfig* config, float** out,
                                           size_t* out_length);

// Algorithm modes for sonare_mastering_repair_decrackle.
#define SONARE_DECRACKLE_MODE_MEDIAN 0
#define SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE 1

/// @brief Flat POD mirror of @c mastering::repair::DecrackleConfig.
typedef struct {
  float threshold;  // crackle detection threshold (default 0.4)
  int mode;         // SONARE_DECRACKLE_MODE_*
  int levels;       // wavelet decomposition levels (default 4)
} SonareDecrackleConfig;

/// @brief Offline crackle suppressor (median or wavelet-shrinkage).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_decrackle(const float* samples, size_t length, int sample_rate,
                                              const SonareDecrackleConfig* config, float** out,
                                              size_t* out_length);

/// @brief Flat POD mirror of @c mastering::repair::DehumConfig.
typedef struct {
  float fundamental_hz;   // mains-hum fundamental (default 50 Hz)
  int harmonics;          // notch count including fundamental (default 4)
  float q;                // notch Q (default 20)
  int adaptive;           // 0/1; enable adaptive tracking (default 0)
  float search_range_hz;  // tracking search range in Hz (default 2)
  float adaptation;       // tracking step size (default 0.25)
  int frame_size;         // analysis frame size (default 2048, must be >= 16)
  float pll_bandwidth;    // PLL bandwidth (default 0.01)
} SonareDehumConfig;

/// @brief Offline mains-hum remover (cascaded notch filters with optional PLL tracking).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_dehum(const float* samples, size_t length, int sample_rate,
                                          const SonareDehumConfig* config, float** out,
                                          size_t* out_length);

/// @brief Flat POD mirror of @c mastering::repair::DereverbClassicalConfig.
typedef struct {
  float threshold;         // late-reverb detection threshold (default 0.05)
  float attenuation;       // suppression amount, linear (default 0.5)
  int n_fft;               // STFT size (default 1024, power of two)
  int hop_length;          // hop in samples (default 256)
  float t60_sec;           // estimated T60 (default 0.4)
  float late_delay_ms;     // late-reverb onset relative to direct (default 50)
  float over_subtraction;  // Berouti alpha (default 1.0)
  float spectral_floor;    // Berouti beta (default 0.08)
  int wpe_enabled;         // 0/1; enable WPE pre-stage (default 0)
  int wpe_iterations;      // WPE EM iterations (default 2)
  int wpe_taps;            // WPE filter taps (default 3)
  float wpe_strength;      // WPE blend weight (default 0.7)
} SonareDereverbClassicalConfig;

/// @brief Offline classical dereverberator (spectral subtraction + optional WPE pre-stage).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_dereverb_classical(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDereverbClassicalConfig* config,
                                                       float** out, size_t* out_length);

// Trimming modes for sonare_mastering_repair_trim_silence.
#define SONARE_TRIM_SILENCE_MODE_PEAK 0
#define SONARE_TRIM_SILENCE_MODE_LUFS_GATED 1

/// @brief Flat POD mirror of @c mastering::repair::TrimSilenceConfig.
typedef struct {
  float threshold;         // peak threshold (default 0.001 for Peak mode)
  size_t padding_samples;  // leading/trailing samples to retain (default 0)
  int mode;                // SONARE_TRIM_SILENCE_MODE_*
  float gate_lufs;         // LUFS gate threshold (default -60 for LufsGated mode)
  float window_ms;         // analysis window in milliseconds (default 400)
} SonareTrimSilenceConfig;

/// @brief Offline silence trimmer (peak threshold or LUFS-gated).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_trim_silence(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareTrimSilenceConfig* config, float** out,
                                                 size_t* out_length);

// ============================================================================
// Mastering: offline dynamics processors (compressor, gate, transient_shaper)
// ----------------------------------------------------------------------------
// These are dedicated offline entry points for the most-used dynamics modules.
// They run the streaming processor (prepare + single-block process) over the
// full input buffer and return a heap-allocated mono output buffer. For
// other dynamics flavours (deesser, expander, parallel_comp, etc.) use
// sonare_mastering_process / apply_named_processor with the canonical name.
// ============================================================================

// Detector modes for sonare_mastering_dynamics_compressor.
#define SONARE_COMPRESSOR_DETECTOR_PEAK 0
#define SONARE_COMPRESSOR_DETECTOR_RMS 1
#define SONARE_COMPRESSOR_DETECTOR_LOG_RMS 2

/// @brief Flat POD mirror of @c mastering::dynamics::CompressorConfig.
typedef struct {
  float threshold_db;         // default -18 dB
  float ratio;                // default 2.0 (clamped to >= 1)
  float attack_ms;            // default 10
  float release_ms;           // default 100
  float knee_db;              // default 0
  float makeup_gain_db;       // default 0
  int auto_makeup;            // bool: 0 = off (default), nonzero = on
  int detector;               // SONARE_COMPRESSOR_DETECTOR_*; default RMS
  int sidechain_hpf_enabled;  // bool: 0 = off (default), nonzero = on
  float sidechain_hpf_hz;     // default 100
  float pdr_time_ms;          // program-dependent release ms; default 0
  float pdr_release_scale;    // PDR release multiplier; default 1.0
} SonareCompressorConfig;

/// @brief Offline feed-forward compressor. Processes the buffer in place
///        (after copying) and returns a new heap-allocated output. Release
///        with @ref sonare_free_floats. @p out_latency_samples may be NULL.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_dynamics_compressor(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareCompressorConfig* config, float** out,
                                                 size_t* out_length, int* out_latency_samples);

/// @brief Flat POD mirror of @c mastering::dynamics::GateConfig.
typedef struct {
  float threshold_db;        // open above this level; default -50 dB
  float attack_ms;           // default 2 ms
  float release_ms;          // default 80 ms
  float range_db;            // closed-state attenuation; default -80 dB
  float hold_ms;             // minimum open time; default 0
  float close_threshold_db;  // hysteresis (clamped <= threshold_db); default -50
  float key_hpf_hz;          // sidechain HPF; default 0 (disabled)
} SonareGateConfig;

/// @brief Offline noise gate. Output buffer is heap-allocated; release with
///        @ref sonare_free_floats. @p out_latency_samples may be NULL.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_dynamics_gate(const float* samples, size_t length, int sample_rate,
                                           const SonareGateConfig* config, float** out,
                                           size_t* out_length, int* out_latency_samples);

/// @brief Flat POD mirror of @c mastering::dynamics::TransientShaperConfig.
typedef struct {
  float attack_gain_db;     // default +3 dB
  float sustain_gain_db;    // default 0
  float fast_attack_ms;     // default 0
  float fast_release_ms;    // default 20
  float slow_attack_ms;     // default 15
  float slow_release_ms;    // default 200
  float sensitivity;        // default 1.0 (clamped >= 0)
  float max_gain_db;        // safety clamp; default 12 dB
  float gain_smoothing_ms;  // default 0 (disabled)
  float lookahead_ms;       // default 0 (disabled)
} SonareTransientShaperConfig;

/// @brief Offline transient shaper (envelope-difference based). Output buffer
///        is heap-allocated; release with @ref sonare_free_floats.
///        @p out_latency_samples may be NULL.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_dynamics_transient_shaper(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareTransientShaperConfig* config,
                                                       float** out, size_t* out_length,
                                                       int* out_latency_samples);

#ifdef __cplusplus
}
#endif
