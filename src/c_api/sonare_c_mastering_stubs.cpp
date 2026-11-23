// Definitions of the mastering C ABI for a build without mastering support. The library lists this
// file in place of the mastering translation units, so the exported symbol set does not depend on
// the build options: every entry refuses with SONARE_ERROR_NOT_SUPPORTED and clears its outputs.

#include <sonare/sonare_c.h>

#include <cstddef>
#include <cstdint>
#include <limits>

#include "sonare_c_internal.h"
#include "util/constants.h"

using namespace sonare_c_detail;

namespace {

// The value the real entries publish before a measurement exists.
const SonareStreamingLoudnessGain kUndefinedLoudnessGain = {
    0.0f, sonare::constants::kFloorDb, -std::numeric_limits<float>::infinity()};

}  // namespace

void sonare_mastering_config_init(SonareMasteringConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_process(const float* samples, size_t length, int sample_rate,
                                     const SonareMasteringConfig* config,
                                     SonareMasteringResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_apply_processor(const char* processor, const float* samples,
                                             size_t length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             SonareMasteringResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, processor, samples, length, sample_rate,
                                params, param_count);
}

SonareError sonare_mastering_apply_processor_stereo(
    const char* processor, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* params, size_t param_count, SonareMasteringStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, processor, left, right, length, sample_rate,
                                params, param_count);
}

const char* sonare_mastering_preset_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

SonareError sonare_mastering_preset_params_json(const char* preset, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, preset);
}

SonareError sonare_mastering_chain_config_from_json(const char* document, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, document);
}

void sonare_master_audio_options_init(SonareMasterAudioOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_master_audio(const char* preset, const float* samples, size_t length,
                                int sample_rate, const char* overrides_json,
                                const SonareMasterAudioOptions* options,
                                SonareMasteringChainResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, preset, samples, length, sample_rate,
                                overrides_json, options);
}

SonareError sonare_master_audio_stereo(const char* preset, const float* left, const float* right,
                                       size_t length, int sample_rate, const char* overrides_json,
                                       const SonareMasterAudioOptions* options,
                                       SonareMasteringChainStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, preset, left, right, length, sample_rate,
                                overrides_json, options);
}

const char* sonare_mastering_processor_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

const char* sonare_mastering_pair_processor_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

const char* sonare_mastering_pair_analysis_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

const char* sonare_mastering_stereo_analysis_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

SonareError sonare_mastering_processor_catalog(char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  return refuse_not_compiled_in(Feature::kMastering);
}

SonareError sonare_mastering_amp_preset_catalog(char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  return refuse_not_compiled_in(Feature::kMastering);
}

SonareError sonare_capability_catalog_json(char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  return refuse_not_compiled_in(Feature::kMastering);
}

const char* sonare_mastering_insert_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

const char* sonare_mastering_insert_param_names(const char* insert) {
  SONARE_C_API_ENTRY;
  (void)insert;
  return "";
}

SonareError sonare_mastering_insert_param_info(const char* insert, int sample_rate,
                                               char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
  return refuse_not_compiled_in(Feature::kMastering, insert, sample_rate);
}

SonareError sonare_mastering_insert_timing(const char* insert, const char* params, int sample_rate,
                                           int* out_latency_samples, int* out_tail_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_latency_samples, out_tail_samples);
  return refuse_not_compiled_in(Feature::kMastering, insert, params, sample_rate);
}

SonareError sonare_mastering_enum_value(const char* processor, const char* key, const char* name,
                                        int* out_is_enum, double* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_is_enum, out_value);
  return refuse_not_compiled_in(Feature::kMastering, processor, key, name);
}

SonareError sonare_mastering_apply_pair_processor(const char* processor, const float* source,
                                                  size_t source_length, const float* reference,
                                                  size_t reference_length,
                                                  int reference_sample_rate, int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, SonareMasteringResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, processor, source, source_length, reference,
                                reference_length, reference_sample_rate, sample_rate, params,
                                param_count);
}

SonareError sonare_mastering_apply_pair_processor_stereo(
    const char* processor, const float* source_left, const float* source_right,
    size_t source_length, const float* reference_left, const float* reference_right,
    size_t reference_length, int reference_sample_rate, int sample_rate,
    const SonareMasteringParam* params, size_t param_count, SonareMasteringStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, processor, source_left, source_right,
                                source_length, reference_left, reference_right, reference_length,
                                reference_sample_rate, sample_rate, params, param_count);
}

SonareError sonare_mastering_analyze_pair(const char* analysis, const float* source,
                                          size_t source_length, const float* reference,
                                          size_t reference_length, int reference_sample_rate,
                                          int sample_rate, const SonareMasteringParam* params,
                                          size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, analysis, source, source_length, reference,
                                reference_length, reference_sample_rate, sample_rate, params,
                                param_count);
}

SonareError sonare_mastering_analyze_pair_stereo(
    const char* analysis, const float* source_left, const float* source_right, size_t source_length,
    const float* reference_left, const float* reference_right, size_t reference_length,
    int reference_sample_rate, int sample_rate, const SonareMasteringParam* params,
    size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, analysis, source_left, source_right,
                                source_length, reference_left, reference_right, reference_length,
                                reference_sample_rate, sample_rate, params, param_count);
}

SonareError sonare_mastering_analyze_stereo(const char* analysis, const float* left,
                                            const float* right, size_t length, int sample_rate,
                                            const SonareMasteringParam* params, size_t param_count,
                                            char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, analysis, left, right, length, sample_rate,
                                params, param_count);
}

SonareError sonare_mastering_ab_match_loudness(const float* source, size_t source_length,
                                               const float* reference, size_t reference_length,
                                               int reference_sample_rate, int sample_rate,
                                               float** out, size_t* out_length,
                                               SonareLoudnessMatch* out_match) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_match);
  return refuse_not_compiled_in(Feature::kMastering, source, source_length, reference,
                                reference_length, reference_sample_rate, sample_rate);
}

SonareError sonare_mastering_ab_match_loudness_stereo(
    const float* source_left, const float* source_right, size_t source_length,
    const float* reference_left, const float* reference_right, size_t reference_length,
    int reference_sample_rate, int sample_rate, float** out_left, float** out_right,
    size_t* out_length, SonareLoudnessMatch* out_match) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_left, out_right, out_length, out_match);
  return refuse_not_compiled_in(Feature::kMastering, source_left, source_right, source_length,
                                reference_left, reference_right, reference_length,
                                reference_sample_rate, sample_rate);
}

const char* sonare_mastering_platform_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

int sonare_mastering_platform_from_name(const char* name) {
  SONARE_C_API_ENTRY;
  (void)name;
  return -1;
}

int sonare_mastering_preset_from_name(const char* name) {
  SONARE_C_API_ENTRY;
  (void)name;
  return -1;
}

void sonare_free_mastering_result(SonareMasteringResult* result) { (void)result; }

void sonare_free_mastering_stereo_result(SonareMasteringStereoResult* result) { (void)result; }

void sonare_free_mastering_chain_result(SonareMasteringChainResult* result) { (void)result; }

void sonare_free_mastering_chain_stereo_result(SonareMasteringChainStereoResult* result) {
  (void)result;
}

void sonare_compressor_config_init(SonareCompressorConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_dynamics_compressor(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareCompressorConfig* config, float** out,
                                                 size_t* out_length, int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_latency_samples);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_gate_config_init(SonareGateConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_dynamics_gate(const float* samples, size_t length, int sample_rate,
                                           const SonareGateConfig* config, float** out,
                                           size_t* out_length, int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_latency_samples);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_transient_shaper_config_init(SonareTransientShaperConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_dynamics_transient_shaper(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareTransientShaperConfig* config,
                                                       float** out, size_t* out_length,
                                                       int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_latency_samples);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_streaming_preview(const float* samples, size_t length, int sample_rate,
                                               const SonareStreamingPlatform* platforms,
                                               size_t platform_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, platforms,
                                platform_count);
}

SonareError sonare_mastering_streaming_preview_stereo(const float* left, const float* right,
                                                      size_t length, int sample_rate,
                                                      const SonareStreamingPlatform* platforms,
                                                      size_t platform_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, platforms,
                                platform_count);
}

SonareError sonare_streaming_loudness_gain(const float* samples, size_t length, int sample_rate,
                                           const char* config_json,
                                           SonareStreamingLoudnessGain* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = kUndefinedLoudnessGain;
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config_json);
}

SonareError sonare_streaming_loudness_gain_stereo(const float* left, const float* right,
                                                  size_t length, int sample_rate,
                                                  const char* config_json,
                                                  SonareStreamingLoudnessGain* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = kUndefinedLoudnessGain;
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config_json);
}

SonareError sonare_mastering_assistant_suggest(const float* samples, size_t length, int sample_rate,
                                               const SonareMasteringParam* params,
                                               size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, params,
                                param_count);
}

SonareError sonare_mastering_assistant_suggest_stereo(const float* left, const float* right,
                                                      size_t length, int sample_rate,
                                                      const SonareMasteringParam* params,
                                                      size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, params,
                                param_count);
}

SonareError sonare_mastering_audio_profile(const float* samples, size_t length, int sample_rate,
                                           const SonareMasteringParam* params, size_t param_count,
                                           char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, params,
                                param_count);
}

SonareError sonare_mastering_audio_profile_stereo(const float* left, const float* right,
                                                  size_t length, int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, params,
                                param_count);
}

SonareError sonare_eq_create(int sample_rate, int max_block_size, SonareEq** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, sample_rate, max_block_size);
}

void sonare_eq_destroy(SonareEq* eq) { (void)eq; }

SonareError sonare_eq_set_band(SonareEq* eq, int index, const char* band_json) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, index, band_json);
}

void sonare_eq_clear(SonareEq* eq) {
  SONARE_C_API_ENTRY;
  (void)eq;
}

SonareError sonare_eq_set_phase_mode(SonareEq* eq, int mode) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, mode);
}

SonareError sonare_eq_match(SonareEq* eq, const float* source, size_t source_length,
                            int sample_rate, const float* reference, size_t reference_length,
                            int reference_sample_rate, int max_bands) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, source, source_length, sample_rate,
                                reference, reference_length, reference_sample_rate, max_bands);
}

void sonare_eq_set_auto_gain(SonareEq* eq, int enabled) {
  (void)eq;
  (void)enabled;
}

float sonare_eq_last_auto_gain_db(const SonareEq* eq) {
  (void)eq;
  return 0.0f;
}

SonareError sonare_eq_set_gain_scale(SonareEq* eq, float scale) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, scale);
}

SonareError sonare_eq_set_output_gain_db(SonareEq* eq, float gain_db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, gain_db);
}

SonareError sonare_eq_set_output_pan(SonareEq* eq, float pan) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, pan);
}

int sonare_eq_latency_samples(const SonareEq* eq) {
  SONARE_C_API_ENTRY;
  (void)eq;
  return 0;
}

SonareError sonare_eq_max_block_size(const SonareEq* eq, int* out_max_block_size) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_max_block_size);
  return refuse_not_compiled_in(Feature::kMastering, eq);
}

SonareError sonare_eq_non_finite_discard_count(const SonareEq* eq, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMastering, eq);
}

SonareError sonare_eq_set_sidechain(SonareEq* eq, const float* const* channels, int num_channels,
                                    int num_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, channels, num_channels, num_samples);
}

void sonare_eq_clear_sidechain(SonareEq* eq) {
  SONARE_C_API_ENTRY;
  (void)eq;
}

SonareError sonare_eq_process(SonareEq* eq, float* const* channels, int num_channels,
                              int num_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, channels, num_channels, num_samples);
}

SonareError sonare_eq_spectrum(const SonareEq* eq, SonareEqSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, eq);
}

SonareError sonare_eq_magnitude_response(const SonareEq* eq, int placement,
                                         const float* frequencies_hz, size_t count, float* out_db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, eq, placement, frequencies_hz, count, out_db);
}

void sonare_streaming_mastering_chain_options_init(SonareStreamingMasteringChainOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->loudness_static_gain_db = std::numeric_limits<float>::quiet_NaN();
  options->loudness_static_gain_peak_dbtp = std::numeric_limits<float>::quiet_NaN();
}

SonareError sonare_streaming_mastering_chain_create(
    const char* config_json, const SonareStreamingMasteringChainOptions* options,
    SonareStreamingMasteringChain** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, config_json, options);
}

SonareError sonare_streaming_mastering_chain_prepare(SonareStreamingMasteringChain* handle,
                                                     int sample_rate, int max_block_size,
                                                     int num_channels) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, handle, sample_rate, max_block_size,
                                num_channels);
}

SonareError sonare_streaming_mastering_chain_process_mono(SonareStreamingMasteringChain* handle,
                                                          float* samples, size_t num_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, handle, samples, num_samples);
}

SonareError sonare_streaming_mastering_chain_process_stereo(SonareStreamingMasteringChain* handle,
                                                            float* left, float* right,
                                                            size_t num_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, handle, left, right, num_samples);
}

SonareError sonare_streaming_mastering_chain_flush_mono(SonareStreamingMasteringChain* handle,
                                                        float* samples, size_t capacity,
                                                        size_t* samples_written) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(samples_written);
  return refuse_not_compiled_in(Feature::kMastering, handle, samples, capacity);
}

SonareError sonare_streaming_mastering_chain_flush_stereo(SonareStreamingMasteringChain* handle,
                                                          float* left, float* right,
                                                          size_t capacity,
                                                          size_t* samples_written) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(samples_written);
  return refuse_not_compiled_in(Feature::kMastering, handle, left, right, capacity);
}

SonareError sonare_streaming_mastering_chain_reset(SonareStreamingMasteringChain* handle) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, handle);
}

SonareError sonare_streaming_mastering_chain_set_parameter(SonareStreamingMasteringChain* handle,
                                                           const char* key, double value) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, handle, key, value);
}

int sonare_streaming_mastering_chain_latency_samples(const SonareStreamingMasteringChain* handle) {
  SONARE_C_API_ENTRY;
  (void)handle;
  return 0;
}

SonareError sonare_streaming_mastering_chain_max_block_size(
    const SonareStreamingMasteringChain* handle, int* out_max_block_size) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_max_block_size);
  return refuse_not_compiled_in(Feature::kMastering, handle);
}

const char* sonare_streaming_mastering_chain_stage_names(
    const SonareStreamingMasteringChain* handle) {
  SONARE_C_API_ENTRY;
  (void)handle;
  return "";
}

SonareError sonare_streaming_mastering_chain_non_finite_substitution_count(
    const SonareStreamingMasteringChain* handle, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMastering, handle);
}

SonareError sonare_streaming_mastering_chain_non_finite_discard_count(
    const SonareStreamingMasteringChain* handle, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMastering, handle);
}

void sonare_streaming_mastering_chain_destroy(SonareStreamingMasteringChain* handle) {
  (void)handle;
}

void sonare_declick_config_init(SonareDeclickConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

void sonare_denoise_classical_config_init(SonareDenoiseClassicalConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_repair_detect_clicks(const float* samples, size_t length,
                                                  int sample_rate,
                                                  const SonareDeclickConfig* config,
                                                  SonareClickDetection* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_declick_stereo(const float* left, const float* right,
                                                   size_t length, int sample_rate,
                                                   const SonareDeclickConfig* config,
                                                   SonareDeclickStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_declick_stereo_result(SonareDeclickStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_declick(const float* samples, size_t length, int sample_rate,
                                            const SonareDeclickConfig* config,
                                            SonareDeclickResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_declick_result(SonareDeclickResult* result) { (void)result; }

SonareError sonare_mastering_repair_detect_noise_floor(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDenoiseClassicalConfig* config,
                                                       SonareNoiseDetection* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_noise_band_bins(int n_fft, int sample_rate, int* out_bins) {
  SONARE_C_API_ENTRY;
  if (out_bins != nullptr) {
    for (int i = 0; i < SONARE_REPAIR_NOISE_BAND_EDGE_COUNT; ++i) out_bins[i] = 0;
  }
  return refuse_not_compiled_in(Feature::kMastering, n_fft, sample_rate);
}

SonareError sonare_mastering_repair_denoise_classical_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareDenoiseClassicalConfig* config, SonareDenoiseStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_denoise_stereo_result(SonareDenoiseStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_denoise_classical(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareDenoiseClassicalConfig* config,
                                                      SonareDenoiseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_denoise_result(SonareDenoiseResult* result) { (void)result; }

SonareError sonare_mastering_repair_denoise_classical_linked(
    const float* const* channels, size_t num_channels, size_t length, int sample_rate,
    const SonareDenoiseClassicalConfig* config, SonareDenoiseLinkedResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, channels, num_channels, length, sample_rate,
                                config);
}

void sonare_free_denoise_linked_result(SonareDenoiseLinkedResult* result) { (void)result; }

void sonare_declip_config_init(SonareDeclipConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_repair_detect_clipping(const float* samples, size_t length,
                                                    int sample_rate,
                                                    const SonareDeclipConfig* config,
                                                    SonareClipDetection* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_declip_stereo(const float* left, const float* right,
                                                  size_t length, int sample_rate,
                                                  const SonareDeclipConfig* config,
                                                  SonareDeclipStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_declip_stereo_result(SonareDeclipStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_declip(const float* samples, size_t length, int sample_rate,
                                           const SonareDeclipConfig* config,
                                           SonareDeclipResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_declip_result(SonareDeclipResult* result) { (void)result; }

void sonare_decrackle_config_init(SonareDecrackleConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_repair_detect_crackle(const float* samples, size_t length,
                                                   int sample_rate,
                                                   const SonareDecrackleConfig* config,
                                                   SonareCrackleDetection* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_decrackle_stereo(const float* left, const float* right,
                                                     size_t length, int sample_rate,
                                                     const SonareDecrackleConfig* config,
                                                     SonareDecrackleStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_decrackle_stereo_result(SonareDecrackleStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_decrackle(const float* samples, size_t length, int sample_rate,
                                              const SonareDecrackleConfig* config,
                                              SonareDecrackleResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_decrackle_result(SonareDecrackleResult* result) { (void)result; }

void sonare_dehum_config_init(SonareDehumConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_repair_detect_hum(const float* samples, size_t length, int sample_rate,
                                               const SonareDehumConfig* config,
                                               SonareHumDetection* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_dehum_stereo(const float* left, const float* right,
                                                 size_t length, int sample_rate,
                                                 const SonareDehumConfig* config,
                                                 SonareDehumStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_dehum_stereo_result(SonareDehumStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_dehum(const float* samples, size_t length, int sample_rate,
                                          const SonareDehumConfig* config, SonareDehumResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_dehum_result(SonareDehumResult* result) { (void)result; }

void sonare_dereverb_classical_config_init(SonareDereverbClassicalConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_repair_detect_reverb(const float* samples, size_t length,
                                                  int sample_rate,
                                                  const SonareDereverbClassicalConfig* config,
                                                  SonareReverbDetection* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_dereverb_classical_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareDereverbClassicalConfig* config, SonareDereverbStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_dereverb_stereo_result(SonareDereverbStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_dereverb_classical(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDereverbClassicalConfig* config,
                                                       SonareDereverbResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_dereverb_result(SonareDereverbResult* result) { (void)result; }

SonareError sonare_mastering_repair_dereverb_classical_linked(
    const float* const* channels, size_t num_channels, size_t length, int sample_rate,
    const SonareDereverbClassicalConfig* config, SonareDereverbLinkedResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, channels, num_channels, length, sample_rate,
                                config);
}

void sonare_free_dereverb_linked_result(SonareDereverbLinkedResult* result) { (void)result; }

SonareError sonare_mastering_repair_dereverb_apply_room_estimate(
    const SonareRoomEstimate* estimate, SonareDereverbClassicalConfig* config) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMastering, estimate, config);
}

void sonare_trim_silence_config_init(SonareTrimSilenceConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mastering_repair_detect_trim_range(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareTrimSilenceConfig* config,
                                                      SonareTrimRange* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

SonareError sonare_mastering_repair_detect_trim_range_stereo(const float* left, const float* right,
                                                             size_t length, int sample_rate,
                                                             const SonareTrimSilenceConfig* config,
                                                             SonareTrimRange* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

SonareError sonare_mastering_repair_trim_silence_stereo(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareTrimSilenceConfig* config,
                                                        SonareTrimSilenceStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, left, right, length, sample_rate, config);
}

void sonare_free_trim_silence_stereo_result(SonareTrimSilenceStereoResult* result) { (void)result; }

SonareError sonare_mastering_repair_trim_silence(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareTrimSilenceConfig* config,
                                                 SonareTrimSilenceResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMastering, samples, length, sample_rate, config);
}

void sonare_free_trim_silence_result(SonareTrimSilenceResult* result) { (void)result; }

SonareError sonare_mastering_repair_analyze(const float* const* channels, size_t num_channels,
                                            size_t length, int sample_rate,
                                            const char* request_json, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMastering, channels, num_channels, length, sample_rate,
                                request_json);
}

SonareError sonare_mastering_repair_apply(const float* const* channels, size_t num_channels,
                                          size_t length, int sample_rate, const char* stages_json,
                                          SonareMasteringProgressCallback callback, void* user_data,
                                          float* const* out_channels, char** reports_json_out,
                                          SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(reports_json_out);
  return refuse_not_compiled_in(Feature::kMastering, channels, num_channels, length, sample_rate,
                                stages_json, callback, user_data, out_channels, cancel_cb,
                                cancel_user_data);
}
