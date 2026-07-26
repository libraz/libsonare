#ifndef SONARE_C_MASTERING_HELPERS_H_
#define SONARE_C_MASTERING_HELPERS_H_

#include <sonare/sonare_c.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "sonare_c_internal.h"

namespace sonare_c_mastering_detail {

inline sonare::mastering::maximizer::LoudnessOptimizeConfig to_cpp_config(
    const SonareMasteringConfig* config) {
  sonare::mastering::maximizer::LoudnessOptimizeConfig cpp;
  if (config) {
    cpp.target_lufs = config->target_lufs;
    cpp.ceiling_db = config->ceiling_db;
    // true_peak_oversample == 0 keeps the C++ default; only a positive value
    // overrides it, so a zero-initialized config uses the library default
    // instead of failing validation (which rejects oversample not in
    // {1, 2, 4, 8, 16}).
    if (config->true_peak_oversample > 0) cpp.true_peak_oversample = config->true_peak_oversample;
    // release_ms == 0 keeps the C++ default (50 ms); only a positive value
    // overrides it, so a zero-initialized config behaves as before.
    if (config->release_ms > 0.0f) cpp.release_ms = config->release_ms;
    cpp.apply_gain_at_input_rate = config->apply_gain_at_input_rate != 0;
  }
  return cpp;
}

inline std::vector<sonare::mastering::api::Param> to_params(const SonareMasteringParam* params,
                                                            size_t count) {
  std::vector<sonare::mastering::api::Param> out;
  out.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    if (params[index].key) {
      out.push_back({params[index].key, params[index].value});
    }
  }
  return out;
}

inline sonare::mastering::assistant::AssistantConfig to_assistant_config(
    const SonareMasteringParam* params, size_t count) {
  const std::vector<sonare::mastering::api::Param> parsed = to_params(params, count);
  return sonare::mastering::assistant::assistant_config_from_params(parsed.data(), parsed.size());
}

inline sonare::mastering::assistant::AudioProfileConfig to_audio_profile_config(
    const SonareMasteringParam* params, size_t count) {
  const std::vector<sonare::mastering::api::Param> parsed = to_params(params, count);
  return sonare::mastering::assistant::audio_profile_config_from_params(parsed.data(),
                                                                        parsed.size());
}

inline void set_mastering_result(const sonare::mastering::api::MonoResult& result,
                                 SonareMasteringResult* out) {
  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
  out->loudness_target_limited = 0;
  std::unique_ptr<float[]> processed(new float[out->length]);
  std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
  out->samples = sonare_c_detail::release_array(processed);
}

inline char** copy_stage_array(const std::vector<std::string>& stages) {
  if (stages.empty()) return nullptr;
  std::unique_ptr<char*[]> out(new char*[stages.size()]);
  for (size_t i = 0; i < stages.size(); ++i) {
    out[i] = nullptr;
  }
  // Free the partially-copied strings if a copy_string throws (bad_alloc):
  // the unique_ptr above only releases the pointer array, not its elements.
  try {
    for (size_t i = 0; i < stages.size(); ++i) {
      out[i] = sonare_c_detail::copy_string(stages[i]);
    }
  } catch (...) {
    for (size_t i = 0; i < stages.size(); ++i) {
      delete[] out[i];
    }
    throw;
  }
  return out.release();
}

// Zero the chain-metric fields (true peak, LRA, per-stage gain reductions) so a
// validation-failure early return leaves them safe to pass to the matching free
// function. Works for both mono and stereo chain-result structs (identical
// field names). @c stages / @c stages_count are zeroed separately by callers.
template <typename ChainResultT>
inline void zero_chain_metrics(ChainResultT* out) {
  out->output_true_peak_dbtp = 0.0f;
  out->output_lra = 0.0f;
  out->loudness_target_limited = 0;
  out->stage_gain_reduction_stages = nullptr;
  out->stage_gain_reduction_values = nullptr;
  out->stage_gain_reductions_count = 0;
}

// Copy the chain-metric fields from the C++ result into the C struct. The
// per-stage gain reductions become two parallel arrays (stage names + dB
// values) mirroring @ref sonare::mastering::api::ChainMetrics::stage_gain_reductions.
template <typename ChainResultT>
inline void set_chain_metrics(const sonare::mastering::api::ChainMetrics& metrics,
                              ChainResultT* out) {
  out->output_true_peak_dbtp = metrics.output_true_peak_dbtp;
  out->output_lra = metrics.output_lra;
  out->loudness_target_limited = metrics.loudness_target_limited ? 1 : 0;
  const auto& reductions = metrics.stage_gain_reductions;
  out->stage_gain_reductions_count = reductions.size();
  if (reductions.empty()) {
    out->stage_gain_reduction_stages = nullptr;
    out->stage_gain_reduction_values = nullptr;
    return;
  }
  std::vector<std::string> names;
  names.reserve(reductions.size());
  std::unique_ptr<float[]> values(new float[reductions.size()]);
  for (size_t i = 0; i < reductions.size(); ++i) {
    names.push_back(reductions[i].stage);
    values[i] = reductions[i].gain_reduction_db;
  }
  // copy_stage_array may throw (bad_alloc); do it before releasing values so a
  // throw does not leak the float array.
  out->stage_gain_reduction_stages = copy_stage_array(names);
  out->stage_gain_reduction_values = sonare_c_detail::release_array(values);
}

// Release the parallel per-stage gain-reduction arrays and reset the metric
// fields. Safe to call on a zero-initialized struct.
template <typename ChainResultT>
inline void free_chain_metrics(ChainResultT* out) {
  if (out->stage_gain_reduction_stages) {
    for (size_t i = 0; i < out->stage_gain_reductions_count; ++i) {
      delete[] out->stage_gain_reduction_stages[i];
    }
    delete[] out->stage_gain_reduction_stages;
  }
  delete[] out->stage_gain_reduction_values;
  out->stage_gain_reduction_stages = nullptr;
  out->stage_gain_reduction_values = nullptr;
  out->stage_gain_reductions_count = 0;
}

inline void fill_mono_chain_result(const sonare::mastering::api::MonoChainResult& result,
                                   SonareMasteringChainResult* out) {
  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
    out->samples = sonare_c_detail::release_array(processed);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  set_chain_metrics(result, out);
}

inline void fill_stereo_chain_result(const sonare::mastering::api::StereoChainResult& result,
                                     SonareMasteringChainStereoResult* out) {
  out->length = result.left.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> left_out(new float[out->length]);
    std::unique_ptr<float[]> right_out(new float[out->length]);
    std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
    std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
    out->left = sonare_c_detail::release_array(left_out);
    out->right = sonare_c_detail::release_array(right_out);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  set_chain_metrics(result, out);
}

}  // namespace sonare_c_mastering_detail

#endif  // SONARE_C_MASTERING_HELPERS_H_
