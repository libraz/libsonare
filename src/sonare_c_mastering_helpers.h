#ifndef SONARE_C_MASTERING_HELPERS_H_
#define SONARE_C_MASTERING_HELPERS_H_

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

namespace sonare_c_mastering_detail {

inline sonare::mastering::maximizer::LoudnessOptimizeConfig to_cpp_config(
    const SonareMasteringConfig* config) {
  sonare::mastering::maximizer::LoudnessOptimizeConfig cpp;
  if (config) {
    cpp.target_lufs = config->target_lufs;
    cpp.ceiling_db = config->ceiling_db;
    cpp.true_peak_oversample = config->true_peak_oversample;
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
  sonare::mastering::assistant::AssistantConfig config;
  for (size_t index = 0; index < count; ++index) {
    if (!params[index].key) continue;
    const std::string key = params[index].key;
    if (key == "targetLufs" || key == "target_lufs") {
      config.target_lufs = static_cast<float>(params[index].value);
    } else if (key == "ceilingDb" || key == "ceiling_db") {
      config.ceiling_db = static_cast<float>(params[index].value);
    } else if (key == "enableRepair" || key == "enable_repair") {
      config.enable_repair = params[index].value != 0.0;
    } else if (key == "preferStreamingSafe" || key == "prefer_streaming_safe") {
      config.prefer_streaming_safe = params[index].value != 0.0;
    } else if (key == "speechMonoAmount" || key == "speech_mono_amount") {
      config.speech_mono_amount = static_cast<float>(params[index].value);
    }
  }
  return config;
}

inline sonare::mastering::assistant::AudioProfileConfig to_audio_profile_config(
    const SonareMasteringParam* params, size_t count) {
  sonare::mastering::assistant::AudioProfileConfig config;
  for (size_t index = 0; index < count; ++index) {
    if (!params[index].key) continue;
    const std::string key = params[index].key;
    if (key == "nFft" || key == "n_fft") {
      config.n_fft = static_cast<int>(params[index].value);
    } else if (key == "hopLength" || key == "hop_length") {
      config.hop_length = static_cast<int>(params[index].value);
    } else if (key == "truePeakOversample" || key == "true_peak_oversample") {
      config.true_peak_oversample = static_cast<int>(params[index].value);
    }
  }
  return config;
}

inline void set_mastering_result(const sonare::mastering::api::MonoResult& result,
                                 SonareMasteringResult* out) {
  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
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
  for (size_t i = 0; i < stages.size(); ++i) {
    out[i] = sonare_c_detail::copy_string(stages[i]);
  }
  return out.release();
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
}

}  // namespace sonare_c_mastering_detail

#endif  // SONARE_C_MASTERING_HELPERS_H_
