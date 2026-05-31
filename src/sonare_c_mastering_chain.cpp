#include <cstring>
#include <memory>

#include "mastering/api/chain.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

SonareError sonare_mastering_chain(const float* samples, size_t length, int sample_rate,
                                   const SonareMasteringParam* params, size_t param_count,
                                   SonareMasteringChainResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  auto cpp_params = to_params(params, param_count);
  auto result = sonare::mastering::api::run_chain_mono_params(cpp_params.data(), cpp_params.size(),
                                                              samples, length, sample_rate);

  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
    out->samples = release_array(processed);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_stereo(const float* left, const float* right, size_t length,
                                          int sample_rate, const SonareMasteringParam* params,
                                          size_t param_count,
                                          SonareMasteringChainStereoResult* out) {
  if (!out || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
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
  auto cpp_params = to_params(params, param_count);
  auto result = sonare::mastering::api::run_chain_stereo_params(
      cpp_params.data(), cpp_params.size(), left, right, length, sample_rate);

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
    out->left = release_array(left_out);
    out->right = release_array(right_out);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_with_progress(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareMasteringParam* params,
                                                 size_t param_count,
                                                 SonareMasteringProgressCallback callback,
                                                 void* user_data, SonareMasteringChainResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  auto cpp_params = to_params(params, param_count);
  auto config =
      sonare::mastering::api::parse_chain_config_params(cpp_params.data(), cpp_params.size());
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_mono(samples, length, sample_rate);

  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
    out->samples = release_array(processed);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_stereo_with_progress(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareMasteringParam* params,
                                                        size_t param_count,
                                                        SonareMasteringProgressCallback callback,
                                                        void* user_data,
                                                        SonareMasteringChainStereoResult* out) {
  if (!out || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
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
  auto cpp_params = to_params(params, param_count);
  auto config =
      sonare::mastering::api::parse_chain_config_params(cpp_params.data(), cpp_params.size());
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_stereo(left, right, length, sample_rate);

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
    out->left = release_array(left_out);
    out->right = release_array(right_out);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_mastering_chain_result(SonareMasteringChainResult* result) {
  if (!result) return;
  delete[] result->samples;
  result->samples = nullptr;
  result->length = 0;
  if (result->stages) {
    for (size_t i = 0; i < result->stages_count; ++i) {
      delete[] result->stages[i];
    }
    delete[] result->stages;
  }
  result->stages = nullptr;
  result->stages_count = 0;
}

void sonare_free_mastering_chain_stereo_result(SonareMasteringChainStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  result->left = nullptr;
  result->right = nullptr;
  result->length = 0;
  if (result->stages) {
    for (size_t i = 0; i < result->stages_count; ++i) {
      delete[] result->stages[i];
    }
    delete[] result->stages;
  }
  result->stages = nullptr;
  result->stages_count = 0;
}
