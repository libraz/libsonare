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
  static thread_local const std::string names = join_names(sonare::mastering::api::preset_names());
  return names.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
}

SonareError sonare_mastering_preset_params_json(const char* preset, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  SONARE_C_REQUIRE_NONNULL(json_out);

  SONARE_C_TRY
  const auto config = sonare::mastering::api::preset_config(
      sonare::mastering::api::preset_from_string(preset != nullptr ? preset : ""));
  *json_out = copy_string(sonare::mastering::api::chain_config_to_json(config));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_config_from_json(const char* document, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  SONARE_C_REQUIRE_NONNULL(document);
  SONARE_C_REQUIRE_NONNULL(json_out);

  SONARE_C_TRY
  const auto config = sonare::mastering::api::chain_config_from_json(document);
  *json_out = copy_string(sonare::mastering::api::chain_config_to_json(config));
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mastering_platform_names(void) {
  SONARE_C_TRY
  static thread_local const std::string names =
      join_names(sonare::mastering::assistant::platform_names());
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

namespace {

SonareError master_mono(const char* preset, const float* samples, size_t length, int sample_rate,
                        const char* overrides_json, SonareMasteringProgressCallback callback,
                        void* user_data, SonareMasteringChainResult* out,
                        SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;
  SonareError err = validate_audio_params("samples", samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  sonare::mastering::api::MasteringChain chain(preset_chain_config(preset, overrides_json));
  attach_chain_callbacks(chain, callback, user_data, cancel_cb, cancel_user_data);
  return process_mastering_chain_mono(chain, samples, length, sample_rate, cancel_cb, out);
  SONARE_C_CATCH
}

SonareError master_stereo(const char* preset, const float* left, const float* right, size_t length,
                          int sample_rate, const char* overrides_json,
                          SonareMasteringProgressCallback callback, void* user_data,
                          SonareMasteringChainStereoResult* out, SonareCancelCallback cancel_cb,
                          void* cancel_user_data) {
  SONARE_C_ZERO_OUTS(out);
  if (out != nullptr) out->sample_rate = sample_rate;
  SonareError err = validate_stereo_audio_params("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  sonare::mastering::api::MasteringChain chain(preset_chain_config(preset, overrides_json));
  attach_chain_callbacks(chain, callback, user_data, cancel_cb, cancel_user_data);
  return process_mastering_chain_stereo(chain, left, right, length, sample_rate, cancel_cb, out);
  SONARE_C_CATCH
}

}  // namespace

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
  SonareMasterAudioOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_master_audio_options_init,
                                              "sonare_master_audio_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  return master_mono(preset, samples, length, sample_rate, overrides_json,
                     resolved.progress_callback, resolved.progress_user_data, out,
                     resolved.cancel_callback, resolved.cancel_user_data);
}

SonareError sonare_master_audio_stereo(const char* preset, const float* left, const float* right,
                                       size_t length, int sample_rate, const char* overrides_json,
                                       const SonareMasterAudioOptions* options,
                                       SonareMasteringChainStereoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareMasterAudioOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_master_audio_options_init,
                                              "sonare_master_audio_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  return master_stereo(preset, left, right, length, sample_rate, overrides_json,
                       resolved.progress_callback, resolved.progress_user_data, out,
                       resolved.cancel_callback, resolved.cancel_user_data);
}
