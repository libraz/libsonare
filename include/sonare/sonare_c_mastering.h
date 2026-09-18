#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_acoustic.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Mastering
// ============================================================================

// Config for the simple one-shot sonare_mastering_process / _stereo helpers.
// These run the maximizer in full apply (not detect-only) mode. To select
// detect-only mode or chain other processors, use the named-processor path
// (sonare_mastering_apply_named_processor with "maximizer.loudnessOptimize") or
// the full MasteringChain API instead.
//
// ceiling_db always holds; target_lufs is what one normalization pass aims at.
// Where the input's peak headroom cannot supply the gain the target needs, the
// helper drives its true-peak limiter up to 12 dB to close the distance and
// stops there, so peaky or peak-normalized input can finish below target with
// loudness_target_limited set on the result. The named-processor and
// MasteringChain paths behave identically and expose that depth as
// "maxLimiterGainReductionDb"; this struct always uses the default.
//
// The struct is zero-init friendly: a field left 0 uses the library default
// (target_lufs/ceiling_db are real values, so pass them explicitly). release_ms
// and apply_gain_at_input_rate are appended so older zero-initialized callers
// keep the previous fixed-release / input-rate-off behavior.
typedef struct {
  float target_lufs;
  float ceiling_db;
  int true_peak_oversample;
  // Post true-peak limiter release in ms. 0 => library default (50 ms). The
  // helper drives this limiter whenever peak headroom alone cannot reach
  // target_lufs, so the release shapes the result on low-headroom input; it is
  // inert only when the static gain already lands under the ceiling.
  float release_ms;
  // Apply the static loudness gain at the input (pre-oversample) rate. 0 => off
  // (default); nonzero => on. Matches TruePeakLimiterConfig::apply_gain_at_input_rate.
  int apply_gain_at_input_rate;
} SonareMasteringConfig;

typedef struct {
  float* samples;
  size_t length;
  int sample_rate;
  float input_lufs;
  float output_lufs;
  float applied_gain_db;
  int latency_samples;
  /// Non-zero when the true-peak ceiling prevented reaching target_lufs.
  int loudness_target_limited;
  /// Samples a stage replaced with a finite in-domain one, keeping the output
  /// finite and in range. A non-finite sample supplied by the caller is
  /// rejected before any stage runs, so a replacement is always of a value a
  /// stage itself produced.
  ///
  /// Which stages can contribute depends on the call that filled this result,
  /// so what a zero means is stated on each of them rather than here.
  uint32_t non_finite_substitution_count;
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
  /// Non-zero when the true-peak ceiling prevented reaching target_lufs.
  int loudness_target_limited;
  /// See @c SonareMasteringResult::non_finite_substitution_count. Aggregated
  /// over both channels.
  uint32_t non_finite_substitution_count;
} SonareMasteringStereoResult;

/// @brief Progress callback type. Called per chain stage completion.
/// @param progress Progress value (0.0 to 1.0).
/// @param stage    Stage identifier C string (e.g. "dynamics.compressor"). Valid
///                 only during the callback invocation; copy if needed later.
/// @param user_data Opaque pointer passed at registration.
typedef void (*SonareMasteringProgressCallback)(float progress, const char* stage, void* user_data);

/// Number of compact logarithmic spectral-energy bins in a mastering report.
#define SONARE_MASTERING_REPORT_BAND_COUNT 32

/// Whole-program loudness values used by @ref SonareMasteringReport.
typedef struct {
  float integrated_lufs;
  float max_momentary_lufs;
  float max_short_term_lufs;
  float true_peak_dbtp;
  float loudness_range;
} SonareMasteringLoudnessSummary;

/// Explanation-oriented before/after measurements for an offline mastering run.
/// @c band_energy_delta_db is after-minus-before, from 20 Hz to Nyquist.
typedef struct {
  SonareMasteringLoudnessSummary before;
  SonareMasteringLoudnessSummary after;
  float applied_gain_db;
  float max_gain_reduction_db;
  int loudness_target_limited;
  float band_energy_delta_db[SONARE_MASTERING_REPORT_BAND_COUNT];
} SonareMasteringReport;

// Result of running the MasteringChain on a mono buffer. Offline chain/master_audio
// outputs are latency-compensated, so this frozen result layout intentionally
// reports no separate latency_samples field.
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
  // ITU-R BS.1770-4 true peak of the output (dBTP). Lets callers verify a
  // preset ceiling was met without a second oversampled scan.
  //
  // The oversample factor follows the peak-limiting stage the chain actually
  // applied, so it is not fixed: the loudness stage's true-peak oversample
  // (default 4x) when loudness is enabled, and the maximizer true-peak
  // limiter's own oversample factor when loudness is disabled but that stage
  // ran. The two disagree by roughly 0.02 dB between 4x and 8x, so a caller
  // comparing this against an independently measured peak must use the same
  // factor the chain used.
  float output_true_peak_dbtp;
  // EBU Tech 3342 Loudness Range of the output (LU).
  float output_lra;
  // Non-zero when the requested LUFS gain was reduced to respect the true-peak
  // ceiling. output_lufs is the achieved value, not the requested target.
  int loudness_target_limited;
  // Per-stage gain reductions for the dynamics / maximizer stages that report
  // one (a subset of @c stages). @c stage_gain_reduction_stages holds the stage
  // identifiers and @c stage_gain_reduction_values the matching dB values
  // (<= 0), both of length @c stage_gain_reductions_count. Memory is allocated
  // by libsonare and released by @c sonare_free_mastering_chain_result.
  char** stage_gain_reduction_stages;
  float* stage_gain_reduction_values;
  size_t stage_gain_reductions_count;
  /// Aggregated before/after measurements for UI and artifact reporting.
  SonareMasteringReport report;
  /// Samples a stage replaced with a finite in-domain one, keeping the output
  /// finite and in range. A non-finite sample supplied by the caller is
  /// rejected before any stage runs, so a replacement is always of a value a
  /// stage itself produced.
  ///
  /// Only the true-peak limiters replace anything, so with the maximizer's
  /// limiter and the loudness stage both disabled a zero here means no stage
  /// was able to replace anything rather than that nothing needed replacing.
  uint32_t non_finite_substitution_count;
} SonareMasteringChainResult;

// Result of running the MasteringChain on stereo buffers. Offline chain/master_audio
// outputs are latency-compensated; same ownership rules as
// @c SonareMasteringChainResult; free with
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
  // See @c SonareMasteringChainResult for field semantics. Released by
  // @c sonare_free_mastering_chain_stereo_result.
  float output_true_peak_dbtp;
  float output_lra;
  int loudness_target_limited;
  char** stage_gain_reduction_stages;
  float* stage_gain_reduction_values;
  size_t stage_gain_reductions_count;
  SonareMasteringReport report;
  /// @copydoc SonareMasteringChainResult::non_finite_substitution_count
  uint32_t non_finite_substitution_count;
} SonareMasteringChainStereoResult;

/// @details This call always runs exactly one true-peak limiter, so
///   @ref SonareMasteringResult::non_finite_substitution_count is non-zero
///   only when that limiter had a non-finite sample to replace.
/// @param out Receives heap-owned buffers; free with sonare_free_mastering_result.
SonareError sonare_mastering_process(const float* samples, size_t length, int sample_rate,
                                     const SonareMasteringConfig* config,
                                     SonareMasteringResult* out);
/// @details @ref SonareMasteringResult::non_finite_substitution_count here:
///   whether this can be non-zero depends on which processor was named. One
///   that does not substitute reports zero because it has nothing to replace
///   with, not because nothing needed replacing.
/// @param out Receives heap-owned buffers; free with sonare_free_mastering_result.
SonareError sonare_mastering_apply_processor(const char* processor_name, const float* samples,
                                             size_t length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             SonareMasteringResult* out);
/// @details See @c sonare_mastering_apply_processor for
///   @c non_finite_substitution_count semantics.
/// @param out Receives heap-owned buffers; free with sonare_free_mastering_stereo_result.
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
/// @param user_data Passed back to @p callback unchanged. Neither pointer is retained past this
///   call: the callback runs on the calling thread and nothing is stored once it returns.
SonareError sonare_mastering_chain_with_progress(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareMasteringParam* params,
                                                 size_t param_count,
                                                 SonareMasteringProgressCallback callback,
                                                 void* user_data, SonareMasteringChainResult* out);

/// @brief Cancellation-capable equivalent of @ref sonare_mastering_chain_with_progress.
/// @details When @p cancel_cb returns nonzero after a progress report, returns
///          @c SONARE_ERROR_CANCELLED and leaves @p out without allocated output buffers.
/// @param user_data Passed back to @p callback unchanged, as @p cancel_user_data is to
///   @p cancel_cb. No callback or user pointer is retained past this call: both run on the
///   calling thread and nothing is stored once it returns.
SonareError sonare_mastering_chain_with_progress_ex(
    const float* samples, size_t length, int sample_rate, const SonareMasteringParam* params,
    size_t param_count, SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainResult* out, SonareCancelCallback cancel_cb, void* cancel_user_data);

/// @brief Same as sonare_mastering_chain_stereo but reports per-stage progress.
/// @param user_data Passed back to @p callback unchanged. Neither pointer is retained past this
///   call: the callback runs on the calling thread and nothing is stored once it returns.
SonareError sonare_mastering_chain_stereo_with_progress(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareMasteringParam* params,
                                                        size_t param_count,
                                                        SonareMasteringProgressCallback callback,
                                                        void* user_data,
                                                        SonareMasteringChainStereoResult* out);

/// @brief Cancellation-capable equivalent of
///        @ref sonare_mastering_chain_stereo_with_progress.
/// @details On cancellation, @p out has no allocated output buffers.
/// @param user_data Passed back to @p callback unchanged, as @p cancel_user_data is to
///   @p cancel_cb. No callback or user pointer is retained past this call: both run on the
///   calling thread and nothing is stored once it returns.
SonareError sonare_mastering_chain_stereo_with_progress_ex(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* params, size_t param_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out, SonareCancelCallback cancel_cb, void* cancel_user_data);

/// @brief Returns built-in preset identifiers, separated by '\n'.
/// @details Backed by thread-local storage filled on first use; the pointer is
///   valid for the calling thread's lifetime (and stays valid across later API
///   calls on that thread, unlike @ref sonare_mastering_insert_param_names). Do
///   NOT cache it across threads or use it after the producing thread exits, and
///   do NOT free it. Returns NULL if the name table cannot be built.
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
/// @param user_data Passed back to @p callback unchanged. Neither pointer is retained past this
///   call: the callback runs on the calling thread and nothing is stored once it returns.
SonareError sonare_master_audio_with_progress(const char* preset_name, const float* samples,
                                              size_t length, int sample_rate,
                                              const SonareMasteringParam* overrides,
                                              size_t override_count,
                                              SonareMasteringProgressCallback callback,
                                              void* user_data, SonareMasteringChainResult* out);

/// @brief Cancellation-capable equivalent of @ref sonare_master_audio_with_progress.
/// @details On cancellation, @p out has no allocated output buffers.
/// @param user_data Passed back to @p callback unchanged, as @p cancel_user_data is to
///   @p cancel_cb. No callback or user pointer is retained past this call: both run on the
///   calling thread and nothing is stored once it returns.
SonareError sonare_master_audio_with_progress_ex(
    const char* preset_name, const float* samples, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data, SonareMasteringChainResult* out,
    SonareCancelCallback cancel_cb, void* cancel_user_data);

/// @brief Same as sonare_master_audio_stereo but reports per-stage progress.
/// @param user_data Passed back to @p callback unchanged. Neither pointer is retained past this
///   call: the callback runs on the calling thread and nothing is stored once it returns.
SonareError sonare_master_audio_stereo_with_progress(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out);

/// @brief Cancellation-capable equivalent of
///        @ref sonare_master_audio_stereo_with_progress.
/// @details On cancellation, @p out has no allocated output buffers.
/// @param user_data Passed back to @p callback unchanged, as @p cancel_user_data is to
///   @p cancel_cb. No callback or user pointer is retained past this call: both run on the
///   calling thread and nothing is stored once it returns.
SonareError sonare_master_audio_stereo_with_progress_ex(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out, SonareCancelCallback cancel_cb, void* cancel_user_data);

/// @brief The mono processor ids this build supports, separated by '\n'.
/// @details Backed by thread-local storage built once on first use; the pointer
///          is valid for the calling thread's lifetime and stays valid across
///          later API calls on that thread. Do NOT cache it across threads, use
///          it after the producing thread exits, or free it. Returns NULL if the
///          name table cannot be built. Same contract as
///          @ref sonare_mastering_insert_names. NOTE the mixing header's
///          `*_names` getters look alike but differ: theirs are rebuilt on every
///          call and are valid only until the next call on the same thread.
const char* sonare_mastering_processor_names(void);
/// @brief The pair (L/R) processor ids, separated by '\n'.
/// @details Backed by thread-local storage built once on first use; the pointer is valid for the
///          calling thread's lifetime and stays valid across later API calls on that thread. Do NOT
///          cache it across threads, use it after the producing thread exits, or free it. Returns
///          NULL if the name table cannot be built.
const char* sonare_mastering_pair_processor_names(void);
/// @brief The pair analysis ids, separated by '\n'.
/// @details Backed by thread-local storage built once on first use; the pointer is valid for the
///          calling thread's lifetime and stays valid across later API calls on that thread. Do NOT
///          cache it across threads, use it after the producing thread exits, or free it. Returns
///          NULL if the name table cannot be built.
const char* sonare_mastering_pair_analysis_names(void);
/// @brief The stereo analysis ids, separated by '\n'.
/// @details Backed by thread-local storage built once on first use; the pointer is valid for the
///          calling thread's lifetime and stays valid across later API calls on that thread. Do NOT
///          cache it across threads, use it after the producing thread exits, or free it. Returns
///          NULL if the name table cannot be built.
const char* sonare_mastering_stereo_analysis_names(void);

/// @brief Machine-readable classification catalog for every named processor id.
/// @return A JSON array string whose entries contain `id`, `kind`,
///   `realtimeInsertable`, `stereoOnly`, `latencySamples`, `tailSamples`,
///   `realtimeCost`,
///   `channelPolicy`, `category`, and `params` (UTF-8). `kind` is one of
///   "realtime" / "offline" / "pair"
///   (pair > realtime > offline precedence); `realtimeInsertable` is true exactly
///   for ids usable as live inserts. `realtimeCost` is `"low"`, `"moderate"`,
///   or `"high"` for those ids and JSON null otherwise; it is a coarse
///   algorithmic estimate, not a hardware benchmark.
///   for the ids in @ref sonare_mastering_insert_names. Timing values come from
///   one prepared default 48 kHz / 512-sample probe and are representative for
///   configuration-dependent inserts; offline entries report zero. The id
///   universe is the union of @ref sonare_mastering_processor_names, the insert
///   set, and @ref sonare_mastering_pair_processor_names, so realtime-only and
///   pair-only ids are still reported.
/// @details Backed by thread-local storage filled on first use; the pointer is
///          valid for the calling thread's lifetime and stays valid across later
///          API calls on that thread. Do NOT cache it across threads or use it
///          after the producing thread exits, and do NOT free it (mirrors
///          @ref sonare_mastering_processor_names).
const char* sonare_mastering_processor_catalog(void);

/// @brief Returns the complete host-facing capability catalog as UTF-8 JSON.
/// @details Aggregates the processor catalog (including existing parameter
///   descriptors) with the built-in mastering, synth, mixing-scene, and
///   realtime voice-changer preset name lists. `version` and `abi` use the same
///   values as @ref sonare_capabilities_json. Parameter metadata follows @ref
///   sonare_mastering_insert_param_info. Backed by thread-local storage and
///   valid until the next call on the same thread. Do NOT free the pointer.
///   Returns NULL if the catalog cannot be built.
const char* sonare_capability_catalog_json(void);

/// @brief Returns the channel-strip insert / FX processor names that
///        sonare_mixing scene inserts can build, separated by '\n'. Includes the
///        creative effects.* reverbs / modulation / delay when the build has FX
///        support. Use these to discover valid insert names instead of
///        hardcoding magic strings.
/// @details Backed by thread-local storage filled on first use; the pointer is
///          valid for the calling thread's lifetime and stays valid across later
///          API calls on that thread. Do NOT cache it across threads or use it
///          after the producing thread exits, and do NOT free it (mirrors
///          @ref sonare_mastering_processor_names). Returns NULL if the name
///          table cannot be built.
const char* sonare_mastering_insert_names(void);

/// @brief Returns the camelCase parameter names a given insert / FX processor
///        reads, separated by '\n' (empty string when @p name is unknown or its
///        insert needs an unavailable build feature).
/// @details For tools/UIs that want to validate a scene insert's params before
///   loading it: any supplied key NOT in this list is silently ignored by the
///   processor (and would be reported via @ref sonare_last_warning_message on a
///   scene load). Band/sub-band processors enumerate their indexed
///   `band{i}.<field>` keys. Unlike @ref sonare_mastering_insert_names (which
///   stays valid across later API calls on the calling thread), this pointer is
///   thread-local valid only until the next API call on the same thread; the
///   caller must NOT free it. Returns NULL if the name list cannot be built.
/// @param name Insert processor name (see @ref sonare_mastering_insert_names).
const char* sonare_mastering_insert_param_names(const char* name);

/// @brief Realtime-automatable parameter descriptors for an insert processor.
/// @return A JSON array string
///   `[{"name","id","rtSafe","type","min","max","default","unit"}, ...]`
///   (UTF-8). Each entry maps a processor JSON-key parameter name to the integer
///   `id` used by @ref sonare_engine_set_track_strip_insert_param_by_name (and
///   the master variant), with `rtSafe` reporting whether the param can be
///   changed live from the audio thread. Returns `"[]"` for an unknown @p name
///   or a processor with no automatable parameters. Unlike @ref
///   sonare_mastering_insert_param_names (every construction key), this lists
///   only the realtime-controllable subset. The returned pointer is a
///   thread-local valid only until the next API call on the same thread; the
///   caller must NOT free it.
/// @details `type` is `"number"` or `"boolean"`, taken from the C++ type the
///   processor's config builder reads the key as. `default` is the value the
///   processor uses when the key is absent — the config struct's own field
///   initializer — and is JSON null only for a parameter whose id has no
///   construction key at all.
///
///   `min` and `max` are the range construction ACCEPTS, measured by handing
///   candidate values to the same code path a caller would use. They are a hard
///   constraint, not a recommended UI range: a value outside them is an error,
///   while an unvalidated control (most gains) reports null on both, meaning
///   "this catalog states no limit" rather than "unknown". Three properties a
///   host should plan for:
///     - a bound is measured with every OTHER parameter at its default, so two
///       parameters that constrain each other each report the other's default;
///     - a sample-rate-derived bound reflects the un-prepared processor and
///       rises once the insert is prepared at a higher rate;
///     - an exclusive bound is reported as its limit value, so a control
///       requiring `> 0` reports `min` 0 and still rejects 0.
/// @param name Insert processor name (see @ref sonare_mastering_insert_names).
const char* sonare_mastering_insert_param_info(const char* name);

/// @details @ref SonareMasteringResult::non_finite_substitution_count here:
///   whether this can be non-zero depends on which processor was named. One
///   that does not substitute reports zero because it has nothing to replace
///   with, not because nothing needed replacing.
/// @param out Receives heap-owned buffers; free with sonare_free_mastering_result.
SonareError sonare_mastering_apply_pair_processor(const char* processor_name, const float* source,
                                                  const float* reference, size_t length,
                                                  int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, SonareMasteringResult* out);

/// @brief Apply a two-input "match.*" processor with independent source and
/// reference lengths. Reference masters are commonly a different length than the
/// source; the underlying match primitives consume each buffer at its own
/// length. @ref sonare_mastering_apply_pair_processor delegates here with
/// reference_length == length.
/// @details See @c sonare_mastering_apply_pair_processor for
///   @c non_finite_substitution_count semantics.
/// @param out Receives heap-owned buffers; free with sonare_free_mastering_result.
SonareError sonare_mastering_apply_pair_processor_ex(
    const char* processor_name, const float* source, size_t source_length, const float* reference,
    size_t reference_length, int sample_rate, const SonareMasteringParam* params,
    size_t param_count, SonareMasteringResult* out);

/// @note Free @p json_out with @ref sonare_free_string.
SonareError sonare_mastering_analyze_pair(const char* analysis_name, const float* source,
                                          const float* reference, size_t length, int sample_rate,
                                          const SonareMasteringParam* params, size_t param_count,
                                          char** json_out);

/// @brief Analyze a two-input "match.*" analysis with independent source and
/// reference lengths. See @ref sonare_mastering_apply_pair_processor_ex.
/// @ref sonare_mastering_analyze_pair delegates here with
/// reference_length == length.
/// @note Free @p json_out with @ref sonare_free_string.
SonareError sonare_mastering_analyze_pair_ex(const char* analysis_name, const float* source,
                                             size_t source_length, const float* reference,
                                             size_t reference_length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             char** json_out);
/// @note Free @p json_out with @ref sonare_free_string.
SonareError sonare_mastering_analyze_stereo(const char* analysis_name, const float* left,
                                            const float* right, size_t length, int sample_rate,
                                            const SonareMasteringParam* params, size_t param_count,
                                            char** json_out);

/// @brief What gain-matching one take to another's loudness took, and produced.
typedef struct {
  float reference_lufs;          // the reference take's BS.1770 integrated loudness
  float source_lufs;             // the matched take's, before the gain
  float applied_gain_db;         // reference_lufs - source_lufs
  float matched_true_peak_dbtp;  // the matched take's true peak after the gain
} SonareLoudnessMatch;

/// @brief Gain-matches @p source to @p reference's integrated loudness, so an
///        A/B between them is not decided by level.
/// @details The gain is applied with no upper bound and @c matched_true_peak_dbtp
///          reports where that left the peak, rather than the call capping it:
///          a headroom clamp would return @p source at its own loudness whenever
///          it started near full scale, which is the one thing a match must not
///          do. Both loudness values are non-finite for a silent or below-gate
///          take, and @c applied_gain_db is then 0.
/// @param out Receives @p source at @p reference's loudness. Heap-allocated;
///            release with @ref sonare_free_floats.
/// @param out_match Pass NULL to take only the audio.
SonareError sonare_mastering_ab_match_loudness(const float* source, size_t source_length,
                                               const float* reference, size_t reference_length,
                                               int sample_rate, float** out, size_t* out_length,
                                               SonareLoudnessMatch* out_match);

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

/// @brief Stereo counterpart of @ref sonare_mastering_streaming_preview.
/// @details Measures the integrated loudness with BS.1770 channel summing and
/// reports the larger of the two channel true peaks. Both the normalization
/// gain and the ceiling-risk flag follow from that loudness, so passing a
/// @c 0.5*(L+R) downmix to the mono entry point reads roughly 6 dB low on
/// decorrelated stereo and understates the risk by the same amount.
/// Pass NULL/0 for @p platforms to use the built-in platform list.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_streaming_preview_stereo(const float* left, const float* right,
                                                      size_t length, int sample_rate,
                                                      const SonareStreamingPlatform* platforms,
                                                      size_t platform_count, char** json_out);

/// @brief Returns the delivery-target identifiers the mastering assistant
///        accepts, separated by '\n'.
/// @details These are the values @c targetPlatform selects between. Backed by thread-local storage
///   built once on first use: valid for the calling thread's lifetime and across later API calls on
///   it, not to be cached across threads, used after that thread exits, or freed. Returns NULL if
///   the name table cannot be built.
const char* sonare_mastering_platform_names(void);

/// @brief Converts a delivery-target identifier to the index @c targetPlatform
///        carries.
/// @details Accepts the names returned by @ref sonare_mastering_platform_names.
///   Returns -1 for NULL or an unknown name. The index is resolved by the same
///   library that consumes it, so callers must look it up rather than embed it.
int sonare_mastering_platform_from_name(const char* name);

/// @brief Analyze audio and suggest a mastering chain as JSON.
/// @details @p params accepts targetLufs, ceilingDb, enableRepair,
/// preferStreamingSafe, speechMonoAmount, and targetPlatform. The last carries
/// the index from @ref sonare_mastering_platform_from_name, because a param
/// value is a number; an out-of-range or non-integral value is rejected. The
/// returned string must be released with sonare_free_string().
SonareError sonare_mastering_assistant_suggest(const float* samples, size_t length, int sample_rate,
                                               const SonareMasteringParam* params,
                                               size_t param_count, char** json_out);

/// @brief Stereo counterpart of @ref sonare_mastering_assistant_suggest.
/// @details Profiles the pair through @ref sonare_mastering_audio_profile_stereo,
/// so the loudness stage of the suggested chain is built on the channel-summed
/// program rather than a downmix that reads roughly 6 dB low.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_assistant_suggest_stereo(const float* left, const float* right,
                                                      size_t length, int sample_rate,
                                                      const SonareMasteringParam* params,
                                                      size_t param_count, char** json_out);

/// @brief As @ref sonare_mastering_assistant_suggest, but writes only the chain
///        configuration the mastering chain consumes.
/// @details The fuller document carries an explanation, genre candidates and a
/// profile alongside the configuration, so a caller that wants to apply a
/// suggestion has to dig the configuration out of it. This writes that
/// configuration alone, in the core's own canonical serialization. The document's
/// @c chainConfig block nests the same configuration as a parsed object rather
/// than this text, so the two agree as JSON and not as bytes.
/// @p params accepts the same keys as
/// @ref sonare_mastering_assistant_suggest. One analysis pass, as there.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_assistant_suggest_chain_json(const float* samples, size_t length,
                                                          int sample_rate,
                                                          const SonareMasteringParam* params,
                                                          size_t param_count, char** json_out);

/// @brief Stereo counterpart of @ref sonare_mastering_assistant_suggest_chain_json.
/// @details Profiles the pair the way @ref sonare_mastering_assistant_suggest_stereo
/// does, so the loudness stage of the suggested chain is built on the
/// channel-summed program rather than a downmix that reads roughly 6 dB low.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_assistant_suggest_chain_json_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* params, size_t param_count, char** json_out);

/// @brief Analyze audio and return mastering assistant profile JSON.
/// @details @p params accepts nFft, hopLength, truePeakOversample, and
/// detectDefects. The last runs the six repair detectors and fills the
/// @c defects block; it is off by default because they are six further
/// analysis passes. The block is present either way, and its @c measured
/// field is what says whether anything looked.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_audio_profile(const float* samples, size_t length, int sample_rate,
                                           const SonareMasteringParam* params, size_t param_count,
                                           char** json_out);

/// @brief Stereo counterpart of @ref sonare_mastering_audio_profile.
/// @details Only the @c loudness block is measured from the two channels:
/// integrated LUFS and LRA come from the channel-summed program and the true
/// peak is the larger of the two, so decorrelated stereo is not read roughly
/// 6 dB low the way a @c 0.5*(L+R) downmix reads it. The spectral, dynamics and
/// tempo fields describe shape and timing rather than absolute level and are
/// measured on the downmix, which keeps them comparable with the mono entry
/// point. @p params accepts the same keys as the mono entry point.
/// The returned string must be released with sonare_free_string().
SonareError sonare_mastering_audio_profile_stereo(const float* left, const float* right,
                                                  size_t length, int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, char** json_out);

/// @brief Latest equalizer waveform streams, per-band gains and magnitude profile.
/// @details @c pre_* and @c post_* hold uniformly decimated time-domain samples
/// of the block as it entered and left the equalizer, valid up to @c pre_count
/// and @c post_count entries. They are a scope feed and are not a spectral
/// estimate. @c band_gain_db reports the gain each configured band applied.
/// @c profile_db is the frequency-domain view: the post-equalizer signal is
/// Hann-windowed, transformed, and its bin powers summed into
/// @c SONARE_EQ_SPECTRUM_PROFILE_BANDS geometrically spaced bands covering
/// 20 Hz to 20 kHz. Values are amplitude decibels relative to full scale, so a
/// full-scale sine reads roughly 0 dB in the band containing it; the profile
/// rises immediately and falls smoothly. Bands that no analysis bin reaches
/// (above Nyquist) read the numerical floor.
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

/// @brief Creates a prepared equalizer handle.
/// @param sample_rate Processing sample rate; must be positive.
/// @param max_block_size Maximum number of frames accepted by @ref sonare_eq_process.
/// @return A handle, or NULL when the arguments are invalid or allocation fails. The caller owns
///   it and must release it with @ref sonare_eq_destroy, which is what invalidates it; no other
///   call does.
SonareEq* sonare_eq_create(double sample_rate, int max_block_size);
/// @brief Destroys an equalizer handle. Accepts NULL.
void sonare_eq_destroy(SonareEq* eq);
/// @brief Sets one of the @c SONARE_EQ_MAX_BANDS equalizer bands from JSON.
/// @details @p index is in [0, SONARE_EQ_MAX_BANDS). Accepted fields are
/// @c type (Peak, LowShelf, HighShelf, LowPass, HighPass, BandPass, Notch,
/// TiltShelf, FlatTilt, or AllPass), @c frequencyHz, @c gainDb, @c q, @c enabled,
/// @c coeffMode (Rbj or Vicanek), @c slopeDbOct, @c placement (Stereo, Left,
/// Right, Mid, or Side), @c phase (Inherit, ZeroLatency, NaturalPhase, or
/// LinearPhase), @c soloed, @c bypassed, @c proportionalQ, and
/// @c proportionalQStrength. Dynamic-band fields are @c dynamic / @c dynEnabled,
/// @c thresholdDb, @c autoThreshold, @c ratio, @c rangeDb, @c attackMs,
/// @c releaseMs, @c detectorDelayMs, @c sidechainFreqHz, @c sidechainQ, and
/// @c externalSidechain. The corresponding snake_case spellings are also
/// accepted where applicable. Omitted fields use the default @c EqBand values.
/// @c detectorDelayMs delays the detector's view of the signal, so a larger
/// value makes the band react later; it is not look-ahead and adds no latency
/// to the audio path. @c lookaheadMs is still accepted as its former spelling.
/// @c phase == NaturalPhase forces @c coeffMode = Vicanek for the band. Vicanek
/// has no Q/S parameter, so @c q is ignored on LowShelf and HighShelf, though it
/// is still stored and read back verbatim; Peak, pass and notch bands honour
/// @c q in both coefficient modes.
SonareError sonare_eq_set_band(SonareEq* eq, int index, const char* band_json);
/// @brief Removes all configured bands and restores zero-latency processing.
void sonare_eq_clear(SonareEq* eq);
/// @brief Sets the global phase mode.
/// @param mode One of @c SONARE_EQ_PHASE_ZERO_LATENCY,
/// @c SONARE_EQ_PHASE_NATURAL, or @c SONARE_EQ_PHASE_LINEAR.
SonareError sonare_eq_set_phase_mode(SonareEq* eq, int mode);
/// @brief Configures bands that match @p source to @p reference.
/// @details Both mono buffers contain @p length samples at @p sample_rate;
/// @p max_bands must be in [1, SONARE_EQ_MAX_BANDS].
SonareError sonare_eq_match(SonareEq* eq, const float* source, const float* reference,
                            size_t length, int sample_rate, int max_bands);
/// @brief Enables or disables automatic output-gain compensation. Accepts NULL.
void sonare_eq_set_auto_gain(SonareEq* eq, int enabled);
/// @brief Returns the automatic gain applied by the latest process call, or zero for NULL.
float sonare_eq_last_auto_gain_db(const SonareEq* eq);
/// @brief Sets a non-negative multiplier for all configured band gains.
SonareError sonare_eq_set_gain_scale(SonareEq* eq, float scale);
/// @brief Sets the final output gain in dB.
SonareError sonare_eq_set_output_gain_db(SonareEq* eq, float gain_db);
/// @brief Sets final output pan in [-1, 1].
SonareError sonare_eq_set_output_pan(SonareEq* eq, float pan);
/// @brief Returns processing latency in samples, or zero for NULL.
int sonare_eq_latency_samples(const SonareEq* eq);
/// @brief Number of blocks in which the equalizer discarded recursive state
///        because a non-finite value had reached it.
/// @details Advisory telemetry, and the only thing that separates a degraded
///   EQ from a clean one. A discard returns the affected filter cells to their
///   post-reset value, so the EQ recovers in silence and the output stays
///   finite and in range while carrying samples unrelated to the input;
///   nothing else reports that this happened.
///   The count covers every IIR plane the band layout uses -- stereo, per
///   channel, and mid/side -- together with the automatic output gain and the
///   detector state the dynamic bands drive. Linear-phase bands are not
///   included and have nothing to include: an FIR keeps no recursive state, so
///   a non-finite sample leaves its history on its own.
///   The unit is one processed block, never a channel and never a plane, so a
///   stereo block that discards on both adds one and the number does not depend
///   on a dimension the caller did not choose. Cumulative since the handle was
///   created and never cleared, so two readings bracket a span of audio.
///   Returns @c SONARE_ERROR_INVALID_PARAMETER if eq or out_count is NULL.
///   Realtime-safe.
SonareError sonare_eq_non_finite_discard_count(const SonareEq* eq, uint32_t* out_count);
/// @brief Sets an external detector sidechain for dynamic bands.
/// @details The supplied planar block must have the same @p num_samples as the
/// next @ref sonare_eq_process call. It is used only by bands with
/// @c externalSidechain enabled.
///
/// BORROWED, NOT COPIED. The @p channels pointer array and every buffer it
/// names must outlive the next @ref sonare_eq_process call and must not be
/// freed, reallocated or moved until @ref sonare_eq_clear_sidechain or another
/// @ref sonare_eq_set_sidechain replaces them; the processor dereferences them
/// on the audio thread. This is the same borrow contract
/// @ref sonare_engine_set_capture_buffer states in sonare_c_engine.h.
SonareError sonare_eq_set_sidechain(SonareEq* eq, const float* const* channels, int num_channels,
                                    int num_samples);
/// @brief Clears the external sidechain. Accepts NULL.
void sonare_eq_clear_sidechain(SonareEq* eq);
/// @brief Processes one in-place planar audio block.
/// @details @p num_samples must not exceed the @p max_block_size supplied to
/// @ref sonare_eq_create; all @p channels must contain @p num_samples frames.
SonareError sonare_eq_process(SonareEq* eq, float* const* channels, int num_channels,
                              int num_samples);
/// @brief Copies the latest waveform, per-band gain, magnitude profile, and
/// meter snapshot to @p out.
/// @details Refreshed by every @ref sonare_eq_process call; see
/// @ref SonareEqSnapshot for what each field holds.
SonareError sonare_eq_spectrum(const SonareEq* eq, SonareEqSnapshot* out);
/// @brief Writes the composite magnitude of the equalizer's bands, in dB, at
/// each of @p count frequencies.
/// @details The curve to draw over an analyzer. Built from the same coefficient
/// design, tilt expansion and cut-slope cascade the audio path uses, so it
/// states what the equalizer does rather than what its settings look like, and
/// it carries the output gain, the gain scale, and whatever each dynamic band is
/// applying at the moment of the call. Disabled, bypassed and — when anything is
/// soloed — unsoloed bands drop out, and a soloed band is drawn as the band pass
/// it is heard as. Linear-phase bands are included: the phase mode changes the
/// phase, not the magnitude.
/// @param placement Which signal path the curve is for: 0 Stereo, 1 Left,
/// 2 Right, 3 Mid, 4 Side. A band placed on Stereo is on every path; one placed
/// elsewhere appears only on its own, a mid band having no per-channel magnitude
/// to fold into a left or right curve.
/// @param frequencies_hz @p count frequencies, each clamped to [0 Hz, Nyquist].
/// @param out_db Receives @p count values; may alias nothing else.
SonareError sonare_eq_magnitude_response(const SonareEq* eq, int placement,
                                         const float* frequencies_hz, size_t count, float* out_db);

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
/// (e.g. unknown key, non-streaming stage enabled). When the params enable the
/// loudness stage this throws (the streaming chain cannot measure whole-signal
/// integrated LUFS); use @ref sonare_streaming_mastering_chain_create_ex to run
/// a loudness-enabled chain with a caller-precomputed static gain. A non-NULL
/// handle is owned by the caller and stays valid until
/// @ref sonare_streaming_mastering_chain_destroy, which must be called exactly
/// once.
SonareStreamingMasteringChain* sonare_streaming_mastering_chain_create(
    const SonareMasteringParam* params, size_t param_count);

/// @brief Create a streaming chain with a precomputed loudness static gain.
/// @details Identical to @ref sonare_streaming_mastering_chain_create except
/// that when the params enable the loudness stage, @p loudness_static_gain_db
/// (e.g. `target_lufs - measured_integrated_lufs`, measured offline) is applied
/// per block before the loudness stage's true-peak limiter instead of throwing.
/// Pass NaN to reproduce the throw-on-loudness behaviour of the non-_ex create.
/// @param loudness_static_gain_peak_db Offline-measured true-peak (dBFS) of the
///        source the static gain was computed for. When finite, the static gain
///        is clamped to `(ceiling_db - peak_db) +
///        max(max_limiter_gain_reduction_db, 0)` so the streaming preview does
///        not overdrive the loudness limiter harder than the offline chain
///        (which applies the same clamp). Pass NaN to apply the static gain
///        verbatim (no clamp). Returns NULL on error; a non-NULL handle is
///        owned by the caller and stays valid until
///        @ref sonare_streaming_mastering_chain_destroy.
SonareStreamingMasteringChain* sonare_streaming_mastering_chain_create_ex(
    const SonareMasteringParam* params, size_t param_count, float loudness_static_gain_db,
    float loudness_static_gain_peak_db);

/// @brief Prepare with sample rate, max block size, and channel count (1 or 2).
SonareError sonare_streaming_mastering_chain_prepare(SonareStreamingMasteringChain* handle,
                                                     int sample_rate, int max_block_size,
                                                     int num_channels);

/// @brief Process one mono block in place. @p samples length must be <= max_block_size.
/// @details A @p num_samples above the prepared max block size returns
/// SONARE_ERROR_INVALID_PARAMETER, and a chain that has never been prepared
/// returns SONARE_ERROR_INVALID_STATE, in both cases without reading @p samples
/// at all. A zero @p num_samples is a no-op success.
SonareError sonare_streaming_mastering_chain_process_mono(SonareStreamingMasteringChain* handle,
                                                          float* samples, size_t num_samples);

/// @brief Process one stereo block in place. @p left, @p right same length.
/// @details Same block-size and prepared-state rejections as
/// @ref sonare_streaming_mastering_chain_process_mono.
SonareError sonare_streaming_mastering_chain_process_stereo(SonareStreamingMasteringChain* handle,
                                                            float* left, float* right,
                                                            size_t num_samples);

/// @brief Emit delayed audio and finite processor tails after the final mono
/// input block.
/// @param capacity Maximum samples available in @p samples (must not exceed the
///        max block size supplied to prepare).
/// @param samples_written Receives the number of samples written. Call again
///        until it receives zero.
/// @details The first emitted samples include @ref
/// sonare_streaming_mastering_chain_latency_samples leading delayed samples.
/// Concatenate process and flush output, then discard that many leading samples
/// when a time-aligned result is required.
SonareError sonare_streaming_mastering_chain_flush_mono(SonareStreamingMasteringChain* handle,
                                                        float* samples, size_t capacity,
                                                        size_t* samples_written);

/// @brief Stereo counterpart of @ref sonare_streaming_mastering_chain_flush_mono.
SonareError sonare_streaming_mastering_chain_flush_stereo(SonareStreamingMasteringChain* handle,
                                                          float* left, float* right,
                                                          size_t capacity, size_t* samples_written);

/// @brief Reset processor state without rebuilding.
SonareError sonare_streaming_mastering_chain_reset(SonareStreamingMasteringChain* handle);

/// @brief Returns total latency in samples (0 if not prepared).
int sonare_streaming_mastering_chain_latency_samples(const SonareStreamingMasteringChain* handle);

/// @brief Returns the realized stage names in processing order, separated by
///        '\n' (empty string when @p handle is NULL or the chain has not been
///        prepared yet — stage selection happens in
///        @ref sonare_streaming_mastering_chain_prepare, since stereo-only
///        stages depend on the channel count).
/// @details For tools/UIs that want to introspect which stages a config
///   actually realizes. The returned pointer is thread-local valid only until
///   the next API call on the same thread; the caller must NOT free it.
const char* sonare_streaming_mastering_chain_stage_names(
    const SonareStreamingMasteringChain* handle);

/// @brief Number of samples the chain's stages replaced with a finite in-domain
///        one, keeping the output finite and in range.
/// @details A non-finite sample supplied by the caller is rejected before any
///   stage runs, so a replacement is always of a value a stage itself produced.
///
///   Only the true-peak limiters replace anything, so with the maximizer's
///   limiter and the loudness stage both disabled a zero here means no stage was
///   able to replace anything rather than that nothing needed replacing.
///
///   Aggregated over every substituting stage, so a caller learns it happened
///   without having to ask which stage produced it. Cumulative over every block
///   since @ref sonare_streaming_mastering_chain_prepare, which rebuilds the
///   stages and so clears it. Realtime-safe.
/// @param out_count Receives the count; must not be NULL.
SonareError sonare_streaming_mastering_chain_non_finite_substitution_count(
    const SonareStreamingMasteringChain* handle, uint32_t* out_count);
/// @brief Processing calls in which a stage discarded its own recursive state
///        because a non-finite value had reached it.
/// @details The companion to
///   @ref sonare_streaming_mastering_chain_non_finite_substitution_count, and
///   not the same measurement: that one counts SAMPLES a stage replaced and so
///   sums across stages, while a discard is a whole stage returning to its
///   post-reset value and is counted once per call however many stages did it.
///   A stage may run more than once per call, which is why the number is a
///   delta over the call and never a sum.
///
///   Non-finite input is rejected before any stage runs, so what a stage
///   discards is always state it produced itself -- a finite sample large
///   enough to overflow inside a filter, most often. Unlike the substitution
///   count every stage can contribute, so a zero here means no stage discarded
///   rather than that none could.
///
///   Both process and flush entries count, since a flush drives the same
///   stages. Cumulative since
///   @ref sonare_streaming_mastering_chain_prepare, which clears it, so the two
///   counters on one handle share an epoch.
///   @ref sonare_streaming_mastering_chain_reset returns the stages' audio state
///   but neither count: the numbers describe what has happened, which resetting
///   does not undo. Realtime-safe.
/// @param out_count Receives the count; must not be NULL.
SonareError sonare_streaming_mastering_chain_non_finite_discard_count(
    const SonareStreamingMasteringChain* handle, uint32_t* out_count);

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
#define SONARE_DENOISE_NOISE_ESTIMATOR_SPP \
  3  // Speech-presence-probability PSD,
     // tracking no minima and so carrying
     // no minimum-statistics bias correction

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
  float reduction_db;               // deepest attenuation in dB, >= 0 (default 26)
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

/// @brief Flat POD mirror of @c mastering::repair::ClickDetection. Counts runs,
///        not samples.
typedef struct {
  size_t count;                // runs meeting the repair criteria
  size_t rejected;             // runs the criteria excluded as outliers
  size_t longest_run_samples;  // over the counted runs
  float per_second;            // count divided by the input duration
} SonareClickDetection;

/// @brief Measures clicks without repairing.
/// @details Runs the same LPC analysis the repair runs, so a run counted here is
///   one the repair would act on; a cheaper threshold-only scan would report runs
///   it leaves alone.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_clicks(const float* samples, size_t length,
                                                  int sample_rate,
                                                  const SonareDeclickConfig* config,
                                                  SonareClickDetection* out);

/// @brief What one channel's declick pass found and what it did to it.
/// @details A large @c detected.rejected says the configured run length or
///   neighbour ratio is too tight for this material, not that the material is
///   clean.
typedef struct {
  SonareClickDetection detected;  // this channel's own analysis of the input
  size_t repaired_runs;           // runs interpolated
  size_t repaired_samples;        // samples overwritten by interpolation
  size_t linked_runs;             // of repaired_runs, those this channel's own
                                  // detection did not produce; always 0 from
                                  // the mono entry point
  int lpc_model_used;             // 0 when the input was too short for lpc_order,
                                  // which reduces every fill to linear
} SonareDeclickReport;

/// @brief A declicked stereo pair and what each channel's pass did.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareDeclickReport left_report;
  SonareDeclickReport right_report;
} SonareDeclickStereoResult;

/// @brief Declicks a stereo pair, repairing the union of both channels' runs.
/// @details A common-mode click repaired on one side only moves the image, so a
///   run either channel selects is repaired in both. Only the selection is
///   shared: each channel's fill comes from its own samples and its own model,
///   which is why the two reports can differ. Merged runs can leave a repaired
///   region longer than @c max_click_samples -- that cap governs what may be
///   selected, not how far a selection reaches once both channels agree.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_declick_stereo(const float* left, const float* right,
                                                   size_t length, int sample_rate,
                                                   const SonareDeclickConfig* config,
                                                   SonareDeclickStereoResult* out);

/// @brief Offline STFT-domain classical denoiser
///        (LogMMSE / MMSE-STSA / SpectralSubtraction).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_denoise_classical(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareDenoiseClassicalConfig* config,
                                                      float** out, size_t* out_length);

// Bands a denoise noise floor is reported in, and the length of
// SonareNoiseDetection::band_floor_dbfs. A geometric grid from 20 Hz to Nyquist,
// the same axis the mastering report's band_energy_delta_db uses.
#define SONARE_REPAIR_NOISE_BAND_COUNT 32

// Indices sonare_mastering_repair_noise_band_bins writes: the first bin of every
// band plus the one-past-the-end bin of the last.
#define SONARE_REPAIR_NOISE_BAND_EDGE_COUNT (SONARE_REPAIR_NOISE_BAND_COUNT + 1)

/// @brief Flat POD mirror of @c mastering::repair::NoiseDetection.
/// @details These are absolute levels, which makes them the one part of a stereo
///   denoise report that depends on how many channels were passed: the estimator
///   runs on the channel-summed power, so two identical channels read about 3 dB
///   above the same material through the mono entry point. Compare a stereo floor
///   against another stereo floor, never against a mono one.
typedef struct {
  float floor_dbfs;                                       // broadband estimated noise floor
  float band_floor_dbfs[SONARE_REPAIR_NOISE_BAND_COUNT];  // per band, low to high
} SonareNoiseDetection;

/// @brief Measures the noise floor without denoising.
/// @details Runs the STFT and the configured noise estimator -- the two stages the
///   repair runs -- and stops before the gain mask, which is why the attenuation
///   figures live on @ref SonareDenoiseReport rather than here. Needs at least
///   @c config.n_fft samples and refuses a shorter buffer, unlike
///   @ref sonare_mastering_repair_detect_reverb, which pads one.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_noise_floor(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDenoiseClassicalConfig* config,
                                                       SonareNoiseDetection* out);

/// @brief Bin boundaries of the grid @c SonareNoiseDetection::band_floor_dbfs is
///        reported on.
/// @details Band @c k covers the one-sided STFT bins @c [out_bins[k], out_bins[k+1]),
///   and bin @c b sits at @c b * sample_rate / n_fft Hz. The geometric edges are
///   rounded to bins, so a band narrower than the bin spacing comes out empty --
///   @c out_bins[k] equals @c out_bins[k+1] -- and its level is the floor sentinel
///   because no bin landed in it rather than because that region was quiet. That
///   rounding is why the grid is worth asking for instead of recomputing from the
///   band count. Nothing but the analysis geometry decides it, so no config is taken.
///
///   @c SonareMasteringReport::band_energy_delta_db shares these 32 cells but not
///   these bins: it samples each cell's centre out of a long-term spectrum rather
///   than aggregating STFT bins, so it has no bin-narrower-than-a-band case.
/// @param n_fft FFT size the bins belong to; a positive power of two, the rule
///   @ref sonare_mastering_repair_detect_noise_floor applies to its config, so every
///   grid returned here is one that entry can report on.
/// @param sample_rate Sample rate the bins belong to; positive.
/// @param out_bins Receives @c SONARE_REPAIR_NOISE_BAND_EDGE_COUNT indices, low to
///   high. Zeroed before a rejected call returns.
SonareError sonare_mastering_repair_noise_band_bins(int n_fft, int sample_rate, int* out_bins);

/// @brief What a denoise pass found and what it removed.
typedef struct {
  SonareNoiseDetection detected;  // analysis of the input, before the mask
  float mean_reduction_db;        // mean attenuation the gain mask applied. Zero
                                  // reads the same whether the mask was
                                  // transparent or no mask ran at all
  float max_reduction_db;         // deepest attenuation any cell applied; at
                                  // config.reduction_db the floor set the depth
  float floor_limited_fraction;   // cells sitting on that floor. Always 0 for
                                  // SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,
                                  // which floors on spectral_floor instead, so 0
                                  // from that mode is the mode and not a
                                  // measurement
} SonareDenoiseReport;

/// @brief A denoised stereo pair and the one mask that produced it.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareDenoiseReport report;
} SonareDenoiseStereoResult;

/// @brief Denoises a stereo pair with one channel-linked gain mask.
/// @details The mask is built from the channel-summed power and applied unchanged
///   to both channels, so the pass cannot move an interchannel level or phase
///   difference. That is also why there is one report rather than one per
///   channel: a pair would be two copies of one measurement and would read as
///   though the two could differ.
///
///   Needs at least @c config->n_fft samples and rejects a shorter input, unlike
///   @ref sonare_mastering_repair_dereverb_classical_stereo, which pads one.
///
///   Which config fields are live depends on @c mode: @c over_subtraction and
///   @c spectral_floor are read only by SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,
///   and @c speech_presence_gain and @c gain_smoothing only by the other two, so
///   at the default mode the first pair does nothing.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_denoise_classical_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareDenoiseClassicalConfig* config, SonareDenoiseStereoResult* out);

/// @brief Denoises any number of channels with one channel-linked gain mask.
/// @details The N-channel form of
///   @ref sonare_mastering_repair_denoise_classical_stereo, carrying the same
///   guarantee for the whole set: one mask over the channel-summed power, applied
///   unchanged to every channel, so no interchannel level or phase difference
///   moves however many channels there are. A @p channel_count of 1 reproduces
///   @ref sonare_mastering_repair_denoise_classical bit for bit.
///
///   Needs at least @c config->n_fft samples and rejects a shorter input, unlike
///   @ref sonare_mastering_repair_dereverb_classical_linked, which pads one.
///
///   @ref SonareNoiseDetection carries absolute levels, and they are the SET's:
///   the floor is referred to the summed mean square of every channel, so the
///   same material as a pair reads about 3 dB above the same material as one
///   channel. The attenuation figures on @ref SonareDenoiseReport are fractions
///   and do not move.
/// @param channels @p channel_count buffers of @p length samples each; none NULL.
/// @param channel_count Number of channels; at least one.
/// @param length Samples per channel. One length for the set: the C form has no
///        way to express the disagreement the core rejects.
/// @param sample_rate Sample rate in Hz, shared by every channel.
/// @param config Pass NULL to use library defaults.
/// @param out_channels @p channel_count caller-owned buffers of @p length floats
///        each, written in place. Unlike the stereo entry nothing is allocated
///        here, so nothing needs releasing; a rejected call leaves them untouched.
/// @param out_report Receives the one report the set shares. Must not be NULL.
SonareError sonare_mastering_repair_denoise_classical_linked(
    const float* const* channels, size_t channel_count, size_t length, int sample_rate,
    const SonareDenoiseClassicalConfig* config, float* const* out_channels,
    SonareDenoiseReport* out_report);

/// @brief Flat POD mirror of @c mastering::repair::DeclipConfig.
typedef struct {
  float clip_threshold;  // amplitude above which a sample is considered clipped (default 0.98)
  int lpc_order;         // LPC order used for prediction (default 36)
  int iterations;        // LPC reconstruction iterations (default 2)
  float lpc_blend;       // LPC vs interpolation blend (default 0.65)
} SonareDeclipConfig;

/// @brief Offline LPC-based declipper.
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
///
/// Only clipped runs of at most 512 consecutive samples are reconstructed with the LPC
/// solver. The cap is a fixed sample count: it is not derived from @c lpc_order, from
/// @c sample_rate, or from any other field, so its duration depends on the rate
/// (~10.7 ms at 48 kHz). A longer run is filled with cubic / linear interpolation
/// instead, which keeps the solver's dense matrices bounded by the cap rather than by
/// the input. Exceeding the cap silently changes the reconstruction method rather than
/// failing: the call still returns @c SONARE_OK, and @c lpc_order, @c iterations and
/// @c lpc_blend have no effect on the interpolated run.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_declip(const float* samples, size_t length, int sample_rate,
                                           const SonareDeclipConfig* config, float** out,
                                           size_t* out_length);

/// @brief Flat POD mirror of @c mastering::repair::ClipDetection.
typedef struct {
  size_t sample_count;         // samples at or past clip_threshold
  float sample_fraction;       // sample_count divided by the input length
  size_t run_count;            // runs of consecutive clipped samples
  size_t longest_run_samples;  // a run past the 512-sample cap takes the
                               // interpolation fallback instead of the solver
  // Flat tops: runs of bit-identical samples at the signal's peak. These answer
  // a different question from the four fields above, which are read against
  // clip_threshold and therefore both count the apex of any waveform that
  // reaches it and miss material clipped before it was attenuated.
  size_t flat_run_count;
  size_t longest_flat_run_samples;
  size_t flat_sample_count;
  float flat_level;  // magnitude the counted runs sit at; 0 when there are none
} SonareClipDetection;

/// @brief Measures clipping without repairing.
/// @details Counts samples at or past @c config.clip_threshold; no other config
///   field reaches the result, and @p sample_rate is validated without being read,
///   since no field here is a rate. The flat-top fields do not read
///   @c clip_threshold at all, so they report clipping that a later gain change
///   has carried below it -- and, in the other direction, do not fire on an
///   unclipped waveform whose peak merely reaches the threshold. A genuinely
///   flat-topped waveform, such as a square or pulse train, counts as clipped
///   here and cannot be told apart from it in the time domain. The reverse error
///   matters more: anything that moves samples independently erases a real flat
///   top, so zero is not proof the material was never clipped. Resampling and
///   lossy coding do it, and so does a stereo downmix -- detect each channel
///   before mixing them, not after.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_clipping(const float* samples, size_t length,
                                                    int sample_rate,
                                                    const SonareDeclipConfig* config,
                                                    SonareClipDetection* out);

/// @brief What one channel's declip pass found and what it did to it.
typedef struct {
  SonareClipDetection detected;   // this channel's own analysis of the input
  size_t lpc_reconstructed_runs;  // runs the solver filled
  size_t interpolated_runs;       // runs past the cap, filled by interpolation:
                                  // for these lpc_order, iterations and
                                  // lpc_blend had no effect
  size_t repaired_samples;        // samples overwritten by either fill
  size_t linked_runs;             // of the repaired runs, those reaching past
                                  // this channel's own clipped samples because
                                  // the other channel's run was wider; always 0
                                  // from the mono entry point
} SonareDeclipReport;

/// @brief A declipped stereo pair and what each channel's pass did.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareDeclipReport left_report;
  SonareDeclipReport right_report;
} SonareDeclipStereoResult;

/// @brief Declips a stereo pair over the union of both channels' clipped runs.
/// @details One clipped plateau rarely ends on the same sample in both
///   channels, and a run a single sample splits on one side only reconstructs
///   differently there, which moves the image. Each channel therefore
///   reconstructs the whole of every union run it has at least one clipped
///   sample in. A channel with none in a run is left untouched there --
///   reconstructing unclipped audio to match the other side would replace real
///   samples with an estimate, so this is not the declicker's behaviour.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_declip_stereo(const float* left, const float* right,
                                                  size_t length, int sample_rate,
                                                  const SonareDeclipConfig* config,
                                                  SonareDeclipStereoResult* out);

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

/// @brief Flat POD mirror of @c mastering::repair::CrackleDetection.
/// @details Measured by the median criterion whatever mode is configured:
///   wavelet shrinkage removes crackle without ever deciding a sample is
///   crackle, so these counts do not describe what wavelet mode repaired.
typedef struct {
  size_t sample_count;    // samples deviating from the local median by more
                          // than threshold
  float sample_fraction;  // sample_count divided by the input length
  float per_second;
} SonareCrackleDetection;

/// @brief Measures crackle without repairing.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_crackle(const float* samples, size_t length,
                                                   int sample_rate,
                                                   const SonareDecrackleConfig* config,
                                                   SonareCrackleDetection* out);

/// @brief What one channel's decrackle pass found and what it did to it.
/// @details The two modes report through different fields; the other mode's
///   fields read zero because that mode did not run, which the caller knows
///   from the config it passed rather than from the value.
typedef struct {
  SonareCrackleDetection detected;  // this channel's own analysis of the input
  size_t replaced_samples;          // median mode: samples the filter overwrote,
                                    // equal to detected.sample_count since the
                                    // detector and the repair share a criterion
  size_t detail_coefficients;       // wavelet mode: detail coefficients examined by
                                    // the unshifted pass, not by every pass the
                                    // mode averages
  size_t shrunk_coefficients;       // wavelet mode: of those, driven to zero
  float noise_sigma;                // wavelet mode: the MAD noise estimate that
                                    // set every level's threshold, which the
                                    // configured threshold only caps
} SonareDecrackleReport;

/// @brief A decrackled stereo pair and what each channel's pass did.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareDecrackleReport left_report;
  SonareDecrackleReport right_report;
} SonareDecrackleStereoResult;

/// @brief Decrackles a stereo pair, each channel on its own.
/// @details Crackle is surface damage: the two channels carry different
///   scratches at different instants, so there is no common event for a shared
///   decision to agree about, and neither mode carries state across channels.
///   Unlike the declicker and the declipper, no run is ever widened to match
///   the other side and no report field counts such a widening.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_decrackle_stereo(const float* left, const float* right,
                                                     size_t length, int sample_rate,
                                                     const SonareDecrackleConfig* config,
                                                     SonareDecrackleStereoResult* out);

#define SONARE_DEHUM_MODE_SUBTRACT 0  // Subtracts the tracked harmonic series
#define SONARE_DEHUM_MODE_NOTCH 1     // Cascaded RBJ notches, hum or programme

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
  int mode;               // SONARE_DEHUM_MODE_* (default SUBTRACT, so a
                          // zero-initialized config selects it)
} SonareDehumConfig;

/// @brief Offline mains-hum remover (cascaded notch filters with optional PLL tracking).
/// @details Output buffer is heap-allocated; release with @ref sonare_free_floats.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_dehum(const float* samples, size_t length, int sample_rate,
                                          const SonareDehumConfig* config, float** out,
                                          size_t* out_length);

// Longest harmonic series a dehum pass tracks, and the length of
// SonareHumDetection::harmonic_dbfs. 16 harmonics reach 800 Hz from a 50 Hz
// fundamental, past where mains hum carries energy worth notching.
#define SONARE_DEHUM_MAX_HARMONICS 16

/// @brief Flat POD mirror of @c mastering::repair::HumDetection.
/// @details Always measured through the estimation path, whatever
///   @c SonareDehumConfig::adaptive says: the fixed path notches the configured
///   frequency without ever looking for hum, so a detector following the flag
///   would hand back its own input.
typedef struct {
  float fundamental_hz;                             // tracked fundamental; the configured value
                                                    // when adaptive tracking is off
  float fundamental_prominence;                     // winning candidate's projected energy over the
                                                    // median candidate; 1.0 means no peak was found
                                                    // at all. Not a lock flag
  int harmonics;                                    // harmonics above the floor, not necessarily a
                                                    // contiguous run from the first
  float harmonic_dbfs[SONARE_DEHUM_MAX_HARMONICS];  // input level at each k*f0,
                                                    // k ascending. Measured for
                                                    // every k the sample rate
                                                    // carries, not only the
                                                    // notched ones; a k*f0 at or
                                                    // past Nyquist reads the dB
                                                    // floor because nothing is
                                                    // there to measure
} SonareHumDetection;

/// @brief Measures hum without filtering.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_hum(const float* samples, size_t length, int sample_rate,
                                               const SonareDehumConfig* config,
                                               SonareHumDetection* out);

/// @brief What one channel's dehum pass found and what it did to it.
typedef struct {
  SonareHumDetection detected;   // this channel's own analysis, before filtering
  int notched_harmonics;         // harmonics the cascade reached; fewer than
                                 // config.harmonics once k*f0 hits Nyquist
  float applied_fundamental_hz;  // frequency the last notch refresh used
  float fundamental_drift_hz;    // largest excursion of the tracked frequency
                                 // from the configured one. Zero without
                                 // adaptive tracking, which is the measurement
                                 // rather than an unset field. Bounded by
                                 // config.search_range_hz, and a hum inside a
                                 // narrow range drives it to that bound
                                 // exactly, so at small ranges the value
                                 // reports the range rather than the signal
} SonareDehumReport;

/// @brief A dehummed stereo pair and what each channel's pass did.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareDehumReport left_report;
  SonareDehumReport right_report;
} SonareDehumStereoResult;

/// @brief Dehums a stereo pair, sharing the tracked fundamental when tracking is on.
/// @details Mains hum is one physical source, so with @c config->adaptive set the
///   tracker reads the channel mean and both cascades follow the one frequency it
///   finds: tracking the channels apart would put the notches at two frequencies
///   differing by whatever each channel's programme material pulled its own search
///   to, an image shift the hum itself never had. Only the frequency is shared --
///   each channel keeps its own filter state, so neither channel's transient rings
///   through the other, and each report's @c detected measures that channel's own
///   input. With @c adaptive clear, which is the default, nothing is shared and
///   the two channels are filtered independently at the configured frequency.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_dehum_stereo(const float* left, const float* right,
                                                 size_t length, int sample_rate,
                                                 const SonareDehumConfig* config,
                                                 SonareDehumStereoResult* out);

/// @brief Flat POD mirror of @c mastering::repair::DereverbClassicalConfig.
typedef struct {
  float threshold;         // late-reverb detection threshold (default 0, no gate)
  float attenuation;       // suppression amount, linear (default 1, full)
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

/// @brief Flat POD mirror of @c mastering::repair::ReverbDetection.
/// @details NOT an ISO 3382 reverberation time: no Schroeder integration, no
///   noise-floor truncation, STFT bins rather than octave bands, and music is not
///   a free decay. Use @ref sonare_detect_acoustic for a graded RT60; this reports
///   what the module itself measured while deciding how much to subtract.
typedef struct {
  float late_decay_ratio_db;  // decay across the module's own late lag. Less
                              // negative means the material sustains across it,
                              // which a late tail does and a dry offset does not,
                              // so a reverberant input reads HIGHER here than the
                              // same material dry
  float late_predictability;  // mean WPE predictor norm before the clamp. Zero
                              // when the WPE stage did not run, which is the case
                              // whenever config.wpe_enabled is clear -- the
                              // default
} SonareReverbDetection;

/// @brief Measures reverberation without dereverberating.
/// @details Runs the STFT and the module's own late-lag decay statistic. A buffer
///   shorter than @c config.n_fft is padded for analysis, as the repair pads it.
///   The WPE analysis runs only under @c config.wpe_enabled, and then only its
///   covariance and solve -- the prediction is never subtracted.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_reverb(const float* samples, size_t length,
                                                  int sample_rate,
                                                  const SonareDereverbClassicalConfig* config,
                                                  SonareReverbDetection* out);

/// @brief What a dereverb pass found and what it removed.
typedef struct {
  SonareReverbDetection detected;  // analysis of the input
  float mean_reduction_db;         // mean attenuation the subtraction applied
  float suppressed_fraction;       // cells the config.threshold gate admitted as
                                   // late reverberation. The only observation of
                                   // that knob: 0 alongside a nonzero mean says
                                   // the gate admitted nothing
  float wpe_predictor_norm;        // mean predictor norm actually applied, after
                                   // the clamp. Below detected.late_predictability
                                   // says the clamp acted, an otherwise silent
                                   // branch. Zero when the WPE stage did not run
} SonareDereverbReport;

/// @brief A dereverberated stereo pair and the one mask that produced it.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareDereverbReport report;
} SonareDereverbStereoResult;

/// @brief Dereverberates a stereo pair with one channel-linked mask.
/// @details The mask is built from the channel-summed power, and the WPE stage
///   accumulates over both channels and applies one predictor set to each, so
///   neither stage can move an interchannel level or phase difference. That is
///   also why there is one report rather than one per channel.
///
///   Every field of that report is a ratio or a fraction, so unlike
///   @ref SonareNoiseDetection nothing here shifts with the channel count and a
///   stereo figure is comparable against a mono one.
///
///   An input shorter than @c config->n_fft is padded for analysis rather than
///   rejected, which is the opposite of the denoise pair.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_dereverb_classical_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareDereverbClassicalConfig* config, SonareDereverbStereoResult* out);

/// @brief Dereverberates any number of channels with one channel-linked mask.
/// @details The N-channel form of
///   @ref sonare_mastering_repair_dereverb_classical_stereo. Both stages are
///   shared across the whole set rather than just a pair: one mask over the
///   channel-summed power, and one WPE predictor set fitted over every channel's
///   statistics, so neither can move an interchannel level or phase difference
///   however many channels there are. A @p channel_count of 1 reproduces
///   @ref sonare_mastering_repair_dereverb_classical bit for bit.
///
///   An input shorter than @c config->n_fft is padded for analysis rather than
///   rejected, which is the opposite of
///   @ref sonare_mastering_repair_denoise_classical_linked.
///
///   Every field of the report is a ratio or a fraction, so unlike the denoise
///   entry nothing here shifts with @p channel_count and a figure measured over
///   a set is comparable against a mono one.
/// @param channels @p channel_count buffers of @p length samples each; none NULL.
/// @param channel_count Number of channels; at least one.
/// @param length Samples per channel. One length for the set: the C form has no
///        way to express the disagreement the core rejects.
/// @param sample_rate Sample rate in Hz, shared by every channel.
/// @param config Pass NULL to use library defaults.
/// @param out_channels @p channel_count caller-owned buffers of @p length floats
///        each, written in place. Unlike the stereo entry nothing is allocated
///        here, so nothing needs releasing; a rejected call leaves them untouched.
/// @param out_report Receives the one report the set shares. Must not be NULL.
SonareError sonare_mastering_repair_dereverb_classical_linked(
    const float* const* channels, size_t channel_count, size_t length, int sample_rate,
    const SonareDereverbClassicalConfig* config, float* const* out_channels,
    SonareDereverbReport* out_report);

/// @brief Applies a room estimate to a dereverb config IN PLACE: overwrites the
///        two fields a measurement determines and leaves the rest alone.
/// @details The pair to @ref sonare_estimate_room, which measures a recording
///   blind. What the room decides is WHERE the tail is -- @c t60_sec from the
///   mid-frequency reverberation time (the 500 Hz and 1 kHz octave average an
///   ISO 3382 room is quoted by) and @c late_delay_ms from Polack's mixing
///   time, sqrt(V) in milliseconds, past which the response is a diffuse tail
///   rather than separable reflections. How MUCH to remove is taste rather
///   than measurement, so @c attenuation, @c threshold, @c over_subtraction
///   and @c spectral_floor are left as the caller set them.
///
///   Only three fields of @p estimate are read: @c volume, @c rt60_bands and
///   @c band_count. @c confidence, @c drr_db, @c absorption_bands and the
///   three dimensions are ignored, so a caller assembling an estimate by hand
///   rather than passing one @ref sonare_estimate_room produced may leave them
///   zero.
///
///   When NEITHER mid band is usable, @c t60_sec falls back to the average of
///   whatever bands did converge, so a low-band-only estimate configures
///   something rather than nothing. That is no longer a mid-frequency figure,
///   and a caller that needs one should check @c band_count and the finiteness
///   of bands 2 and 3 itself -- band @c b is centred at 125 * 2^b Hz.
///
///   @p config is read AND written, and every field of it is taken literally:
///   unlike the @c config argument of the dereverb call itself, which stands in
///   for the library defaults when it is NULL, a struct passed HERE has no
///   zero-is-default rule, so zero-initializing it and calling this leaves the
///   taste fields at zero rather than at their defaults. Fill in what the
///   render needs first, then call this to point it at the room.
///
///   A field whose measurement did not converge is left alone, so a partial
///   estimate still configures the half it measured -- and an estimate whose
///   @c confidence is low is still applied, because whether to trust it is the
///   caller's call and this reports no opinion on it.
/// @return SONARE_ERROR_INVALID_PARAMETER for a NULL @p estimate or @p config,
///         or SONARE_ERROR_OUT_OF_MEMORY if the band copy it takes fails.
SonareError sonare_mastering_repair_dereverb_apply_room_estimate(
    const SonareRoomEstimate* estimate, SonareDereverbClassicalConfig* config);

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

/// @brief One half-open sample range, in input-buffer coordinates.
typedef struct {
  size_t first;           // first kept sample
  size_t last_exclusive;  // one past the last kept sample
} SonareTrimRange;

/// @brief Measures the range a trim pass would keep, without trimming.
/// @details The padding @c config.padding_samples asks for is already inside the
///   returned range, so this is the range the repair would cut to rather than the
///   detected extent of the signal. A buffer with nothing above the threshold
///   reports (length, length).
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_trim_range(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareTrimSilenceConfig* config,
                                                      SonareTrimRange* out);

/// @brief Measures the one range a stereo trim pass would cut both channels to.
/// @details Each channel is scanned on its own and the two ranges are unioned, so
///   the pair keeps whatever either channel calls signal. A channel with nothing
///   above the threshold contributes no edge at all rather than an edge at the
///   buffer's end, so one silent channel does not widen the range. A downmix is
///   not read: summing to mono halves material carried by one channel alone and
///   cancels an antiphase pair outright, either of which would read full-level
///   audio as silence.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_detect_trim_range_stereo(const float* left, const float* right,
                                                             size_t length, int sample_rate,
                                                             const SonareTrimSilenceConfig* config,
                                                             SonareTrimRange* out);

/// @brief What a trim pass kept and what it dropped.
/// @details A pass that kept nothing reports the range (length, length), which
///   counts the whole buffer as removed head and leaves removed tail at 0. The
///   two still sum to the input length, so a caller reporting how much went
///   reads the right total; only the split between the ends is arbitrary there.
typedef struct {
  SonareTrimRange range;        // the kept range, padding included
  size_t removed_head_samples;  // samples dropped before range.first
  size_t removed_tail_samples;  // samples dropped after range.last_exclusive
} SonareTrimReport;

/// @brief A trimmed stereo pair, the range both channels were cut to, and the
///        two per-channel ranges that range is the union of.
/// @details @c left and @c right are heap-allocated; release each with
///   @ref sonare_free_floats.
///
///   @c length is an OUTPUT length here, not an echo of the call's @c length
///   argument as it is on the other repair stereo entries: trimming shortens the
///   pair, so this field is the only thing that says how much came back.
///
///   When neither channel carries signal both pointers are NULL and @c length is
///   0 -- no zero-length allocation is made. @ref sonare_free_floats accepts
///   NULL, so a caller can still free unconditionally.
typedef struct {
  float* left;
  float* right;
  size_t length;
  SonareTrimReport report;
  SonareTrimRange left_range;
  SonareTrimRange right_range;
} SonareTrimSilenceStereoResult;

/// @brief Trims a stereo pair to one shared range.
/// @details Each channel is scanned on its own and the two ranges are unioned,
///   so the pair keeps whatever EITHER channel calls signal and both outputs
///   come back the same length. The scan never reads a downmix: 0.5 * (left +
///   right) halves material carried by one channel alone, which can drop it
///   under the gate, and cancels an antiphase pair to exactly zero, which would
///   read full-level audio in both channels as silence. Trimming is destructive,
///   so the rule errs toward keeping.
///
///   @c report.range is the union that was applied. @c left_range and
///   @c right_range are the per-channel scans it was formed from, so a caller
///   can see which channel decided each edge. A channel carrying nothing reports
///   an empty range and contributes nothing to the union.
///
///   Which config fields are live depends on @c mode: @c threshold is read only
///   by SONARE_TRIM_SILENCE_MODE_PEAK, and @c gate_lufs and @c window_ms only by
///   SONARE_TRIM_SILENCE_MODE_LUFS_GATED.
///
///   That gated mode compares an UNWEIGHTED RMS over a window centred on each
///   sample against @c gate_lufs, so the figure it gates on is dBFS rather than
///   a BS.1770 loudness, and @c window_ms sizes that window and does nothing
///   else. The window is clipped at the buffer ends, so a sample near either
///   edge is judged on a shorter one.
///
///   @c padding_samples widens the kept range in both directions and is clamped
///   to the buffer, so it can never reach past either end; a pass that kept
///   nothing is not padded. A count above SIZE_MAX/2 is rejected, which is where
///   a negative one that crossed a language boundary lands.
/// @param config Pass NULL to use library defaults.
SonareError sonare_mastering_repair_trim_silence_stereo(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareTrimSilenceConfig* config,
                                                        SonareTrimSilenceStereoResult* out);

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
