#include <sonare/sonare_c.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/streaming_preview.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

SonareError sonare_mastering_streaming_preview(const float* samples, size_t length, int sample_rate,
                                               const SonareStreamingPlatform* platforms,
                                               size_t platform_count, char** json_out) {
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  if (!platforms && platform_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<sonare::mastering::maximizer::StreamingPlatform> cpp_platforms;
    cpp_platforms.reserve(platform_count);
    for (size_t index = 0; index < platform_count; ++index) {
      if (!platforms[index].name) return SONARE_ERROR_INVALID_PARAMETER;
      cpp_platforms.push_back(
          {platforms[index].name, platforms[index].target_lufs, platforms[index].ceiling_db});
    }

    const auto results =
        platform_count == 0 ? sonare::mastering::maximizer::streaming_preview(audio)
                            : sonare::mastering::maximizer::streaming_preview(audio, cpp_platforms);

    *json_out = copy_string(sonare::mastering::maximizer::streaming_preview_to_json(results));
    return SONARE_OK;
  });
}

SonareError sonare_mastering_assistant_suggest(const float* samples, size_t length, int sample_rate,
                                               const SonareMasteringParam* params,
                                               size_t param_count, char** json_out) {
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  SONARE_C_TRY
  const auto config = to_assistant_config(params, param_count);
  const auto result =
      sonare::mastering::assistant::suggest_chain(samples, length, sample_rate, config);
  *json_out = copy_string(sonare::mastering::assistant::assistant_result_to_json(result));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_audio_profile(const float* samples, size_t length, int sample_rate,
                                           const SonareMasteringParam* params, size_t param_count,
                                           char** json_out) {
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  SONARE_C_TRY
  const auto config = to_audio_profile_config(params, param_count);
  const auto profile =
      sonare::mastering::assistant::analyze_audio_profile(samples, length, sample_rate, config);
  *json_out = copy_string(sonare::mastering::assistant::audio_profile_to_json(profile));
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Streaming mastering chain
// ============================================================================

struct SonareStreamingMasteringChain {
  std::unique_ptr<sonare::mastering::api::StreamingMasteringChain> chain;
};

namespace {

bool all_finite(const float* samples, size_t num_samples) noexcept {
  if (!samples) return num_samples == 0;
  for (size_t i = 0; i < num_samples; ++i) {
    if (!std::isfinite(samples[i])) return false;
  }
  return true;
}

}  // namespace

SonareStreamingMasteringChain* sonare_streaming_mastering_chain_create_ex(
    const SonareMasteringParam* params, size_t param_count, float loudness_static_gain_db,
    float loudness_static_gain_peak_db) {
  if (!params && param_count > 0) return nullptr;
  try {
    auto cpp_params = to_params(params, param_count);
    auto config =
        sonare::mastering::api::parse_chain_config_params(cpp_params.data(), cpp_params.size());
    std::unique_ptr<sonare::mastering::api::StreamingMasteringChain> chain;
    if (std::isnan(loudness_static_gain_db)) {
      // Reproduce the throw-on-loudness behaviour of the non-_ex create.
      chain = std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config));
    } else {
      sonare::mastering::api::StreamingMasteringChainOptions options;
      options.loudness_static_gain_db = loudness_static_gain_db;
      options.loudness_static_gain_peak_db = loudness_static_gain_peak_db;
      chain = std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config),
                                                                                options);
    }
    auto* handle = new SonareStreamingMasteringChain;
    handle->chain = std::move(chain);
    return handle;
  } catch (const std::exception& e) {
    sonare_c_detail::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)");
    return nullptr;
  }
}

SonareStreamingMasteringChain* sonare_streaming_mastering_chain_create(
    const SonareMasteringParam* params, size_t param_count) {
  return sonare_streaming_mastering_chain_create_ex(params, param_count,
                                                    std::numeric_limits<float>::quiet_NaN(),
                                                    std::numeric_limits<float>::quiet_NaN());
}

SonareError sonare_streaming_mastering_chain_prepare(SonareStreamingMasteringChain* handle,
                                                     int sample_rate, int max_block_size,
                                                     int num_channels) {
  if (!handle || !handle->chain) return SONARE_ERROR_INVALID_PARAMETER;
  if (sample_rate <= 0 || max_block_size <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  handle->chain->prepare(static_cast<double>(sample_rate), max_block_size, num_channels);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_mastering_chain_process_mono(SonareStreamingMasteringChain* handle,
                                                          float* samples, size_t num_samples) {
  if (!handle || !handle->chain || (!samples && num_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) return SONARE_OK;
  if (!all_finite(samples, num_samples)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  float* channels[] = {samples};
  handle->chain->process_block(channels, 1, static_cast<int>(num_samples));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_mastering_chain_process_stereo(SonareStreamingMasteringChain* handle,
                                                            float* left, float* right,
                                                            size_t num_samples) {
  if (!handle || !handle->chain || (!left && num_samples > 0) || (!right && num_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) return SONARE_OK;
  if (!all_finite(left, num_samples) || !all_finite(right, num_samples)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  float* channels[] = {left, right};
  handle->chain->process_block(channels, 2, static_cast<int>(num_samples));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_mastering_chain_reset(SonareStreamingMasteringChain* handle) {
  if (!handle || !handle->chain) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  handle->chain->reset();
  return SONARE_OK;
  SONARE_C_CATCH
}

int sonare_streaming_mastering_chain_latency_samples(const SonareStreamingMasteringChain* handle) {
  if (!handle || !handle->chain) return 0;
  return handle->chain->latency_samples();
}

void sonare_streaming_mastering_chain_destroy(SonareStreamingMasteringChain* handle) {
  delete handle;
}
