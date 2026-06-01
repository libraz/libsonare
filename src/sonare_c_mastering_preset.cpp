#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include "mastering/api/chain.h"
#include "mastering/api/presets.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

// ============================================================================
// Built-in mastering presets
// ============================================================================

const char* sonare_mastering_preset_names(void) {
  static std::string names;
  if (names.empty()) {
    std::ostringstream stream;
    auto presets = sonare::mastering::api::preset_names();
    for (size_t index = 0; index < presets.size(); ++index) {
      if (index > 0) stream << '\n';
      stream << presets[index];
    }
    names = stream.str();
  }
  return names.c_str();
}

SonareError sonare_master_audio(const char* preset_name, const float* samples, size_t length,
                                int sample_rate, const SonareMasteringParam* overrides,
                                size_t override_count, SonareMasteringChainResult* out) {
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto cpp_overrides = to_params(overrides, override_count);
  auto result = sonare::mastering::api::master_audio_mono(
      preset, samples, length, sample_rate, cpp_overrides.data(), cpp_overrides.size());
  fill_mono_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_stereo(const char* preset_name, const float* left,
                                       const float* right, size_t length, int sample_rate,
                                       const SonareMasteringParam* overrides, size_t override_count,
                                       SonareMasteringChainStereoResult* out) {
  if (!out || !preset_name || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto cpp_overrides = to_params(overrides, override_count);
  auto result = sonare::mastering::api::master_audio_stereo(
      preset, left, right, length, sample_rate, cpp_overrides.data(), cpp_overrides.size());
  fill_stereo_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_with_progress(const char* preset_name, const float* samples,
                                              size_t length, int sample_rate,
                                              const SonareMasteringParam* overrides,
                                              size_t override_count,
                                              SonareMasteringProgressCallback callback,
                                              void* user_data, SonareMasteringChainResult* out) {
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto config = sonare::mastering::api::preset_config(preset);
  auto cpp_overrides = to_params(overrides, override_count);
  if (!cpp_overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, cpp_overrides.data(),
                                                         cpp_overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_mono(samples, length, sample_rate);
  fill_mono_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_stereo_with_progress(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out) {
  if (!out || !preset_name || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto config = sonare::mastering::api::preset_config(preset);
  auto cpp_overrides = to_params(overrides, override_count);
  if (!cpp_overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, cpp_overrides.data(),
                                                         cpp_overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_stereo(left, right, length, sample_rate);
  fill_stereo_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}
