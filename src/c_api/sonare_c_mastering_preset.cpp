#include <sonare/sonare_c.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/platform_targets.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

// ============================================================================
// Built-in mastering presets
// ============================================================================

const char* sonare_mastering_preset_names(void) {
  SONARE_C_TRY
  // Gate on a write-once flag, not on names.empty(): the header promises the
  // returned pointer stays valid across later API calls on the thread, so the
  // thread_local must be built exactly once. An empty-string test would recompute
  // (and reassign, invalidating a previously-returned pointer) every call if the
  // name set were ever empty. Matches the sibling *_names getters in
  // sonare_c_mastering_apply.cpp.
  static thread_local std::string names;
  static thread_local bool built = false;
  if (!built) {
    join_names(sonare::mastering::api::preset_names(), names);
    built = true;
  }
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

SonareError sonare_mastering_preset_params_json(const char* preset, char** json_out) {
  SONARE_C_API_ENTRY;
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  SONARE_C_TRY
  const auto config = sonare::mastering::api::preset_config(
      sonare::mastering::api::preset_from_string(preset != nullptr ? preset : ""));
  *json_out = copy_string(sonare::mastering::api::chain_config_to_json(config));
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mastering_platform_names(void) {
  SONARE_C_TRY
  // Same write-once thread_local contract as sonare_mastering_preset_names: the
  // returned pointer must stay valid across later calls on the thread.
  static thread_local std::string names;
  static thread_local bool built = false;
  if (!built) {
    join_names(sonare::mastering::assistant::platform_names(), names);
    built = true;
  }
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

int sonare_mastering_platform_from_name(const char* name) {
  SONARE_C_TRY
  return sonare::mastering::assistant::platform_index_from_name(name);
  SONARE_C_CATCH_RETURN(-1)
}

int sonare_mastering_preset_from_name(const char* name) {
  SONARE_C_TRY
  if (name == nullptr) return -1;
  const std::vector<std::string> names = sonare::mastering::api::preset_names();
  const auto found = std::find(names.begin(), names.end(), name);
  if (found == names.end()) return -1;
  return static_cast<int>(found - names.begin());
  SONARE_C_CATCH_RETURN(-1)
}

SonareError sonare_master_audio(const char* preset_name, const float* samples, size_t length,
                                int sample_rate, const SonareMasteringParam* overrides,
                                size_t override_count, SonareMasteringChainResult* out) {
  SONARE_C_API_ENTRY;
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  clear_chain_result(out, sample_rate);

  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

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
  SONARE_C_API_ENTRY;
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  clear_chain_result(out, sample_rate);

  // Match the mono paths: reject non-finite samples and out-of-range
  // sample_rate/length, not just null pointers.
  SonareError verr = validate_stereo_audio_params(left, right, length, sample_rate);
  if (verr != SONARE_OK) return verr;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto cpp_overrides = to_params(overrides, override_count);
  auto result = sonare::mastering::api::master_audio_stereo(
      preset, left, right, length, sample_rate, cpp_overrides.data(), cpp_overrides.size());
  fill_stereo_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_with_progress_ex(
    const char* preset_name, const float* samples, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data, SonareMasteringChainResult* out,
    SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_API_ENTRY;
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  clear_chain_result(out, sample_rate);

  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto config = sonare::mastering::api::preset_config(preset);
  auto cpp_overrides = to_params(overrides, override_count);
  if (!cpp_overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, cpp_overrides.data(),
                                                         cpp_overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  attach_chain_callbacks(chain, callback, user_data, cancel_cb, cancel_user_data);
  return process_mastering_chain_mono(chain, samples, length, sample_rate, cancel_cb, out);
  SONARE_C_CATCH
}

SonareError sonare_master_audio_stereo_with_progress_ex(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out, SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_API_ENTRY;
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  clear_chain_result(out, sample_rate);

  // Match the mono paths: reject non-finite samples and out-of-range
  // sample_rate/length, not just null pointers.
  SonareError verr = validate_stereo_audio_params(left, right, length, sample_rate);
  if (verr != SONARE_OK) return verr;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  const auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto config = sonare::mastering::api::preset_config(preset);
  auto cpp_overrides = to_params(overrides, override_count);
  if (!cpp_overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, cpp_overrides.data(),
                                                         cpp_overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  attach_chain_callbacks(chain, callback, user_data, cancel_cb, cancel_user_data);
  return process_mastering_chain_stereo(chain, left, right, length, sample_rate, cancel_cb, out);
  SONARE_C_CATCH
}

SonareError sonare_master_audio_with_progress(const char* preset_name, const float* samples,
                                              size_t length, int sample_rate,
                                              const SonareMasteringParam* overrides,
                                              size_t override_count,
                                              SonareMasteringProgressCallback callback,
                                              void* user_data, SonareMasteringChainResult* out) {
  return sonare_master_audio_with_progress_ex(preset_name, samples, length, sample_rate, overrides,
                                              override_count, callback, user_data, out, nullptr,
                                              nullptr);
}

SonareError sonare_master_audio_stereo_with_progress(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out) {
  return sonare_master_audio_stereo_with_progress_ex(preset_name, left, right, length, sample_rate,
                                                     overrides, override_count, callback, user_data,
                                                     out, nullptr, nullptr);
}
