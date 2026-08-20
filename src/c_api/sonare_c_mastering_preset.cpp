#include <sonare/sonare_c.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <string>

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

SonareError sonare_master_audio(const char* preset_name, const float* samples, size_t length,
                                int sample_rate, const SonareMasteringParam* overrides,
                                size_t override_count, SonareMasteringChainResult* out) {
  SONARE_C_API_ENTRY;
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
  zero_chain_metrics(out);

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
  // Match the mono paths: reject non-finite samples and out-of-range
  // sample_rate/length, not just null pointers.
  SonareError verr = validate_audio_params(left, length, sample_rate);
  if (verr != SONARE_OK) return verr;
  verr = validate_audio_params(right, length, sample_rate);
  if (verr != SONARE_OK) return verr;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;
  zero_chain_metrics(out);

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
  zero_chain_metrics(out);

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
  if (cancel_cb) {
    chain.set_cancel_callback(
        [cancel_cb, cancel_user_data]() { return cancel_cb(cancel_user_data) != 0; });
    auto result = chain.process_mono_cancellable(samples, length, sample_rate);
    if (!result) return SONARE_ERROR_CANCELLED;
    fill_mono_chain_result(*result, out);
    return SONARE_OK;
  }
  auto result = chain.process_mono(samples, length, sample_rate);
  fill_mono_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_stereo_with_progress_ex(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out, SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_API_ENTRY;
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  // Match the mono paths: reject non-finite samples and out-of-range
  // sample_rate/length, not just null pointers.
  SonareError verr = validate_audio_params(left, length, sample_rate);
  if (verr != SONARE_OK) return verr;
  verr = validate_audio_params(right, length, sample_rate);
  if (verr != SONARE_OK) return verr;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;
  zero_chain_metrics(out);

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
  if (cancel_cb) {
    chain.set_cancel_callback(
        [cancel_cb, cancel_user_data]() { return cancel_cb(cancel_user_data) != 0; });
    auto result = chain.process_stereo_cancellable(left, right, length, sample_rate);
    if (!result) return SONARE_ERROR_CANCELLED;
    fill_stereo_chain_result(*result, out);
    return SONARE_OK;
  }
  auto result = chain.process_stereo(left, right, length, sample_rate);
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
