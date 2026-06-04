#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

SonareError sonare_mastering_process(const float* samples, size_t length, int sample_rate,
                                     const SonareMasteringConfig* config,
                                     SonareMasteringResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    auto result = sonare::mastering::maximizer::loudness_optimize(audio, to_cpp_config(config));

    out->length = result.audio.size();
    out->sample_rate = result.audio.sample_rate();
    out->input_lufs = result.input_lufs;
    out->output_lufs = result.output_lufs;
    out->applied_gain_db = result.applied_gain_db;
    out->latency_samples = result.latency_samples;

    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.audio.data(), out->length * sizeof(float));
    out->samples = release_array(processed);
    return SONARE_OK;
  });
}

SonareError sonare_mastering_apply_processor(const char* processor_name, const float* samples,
                                             size_t length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             SonareMasteringResult* out) {
  if (!out || !processor_name || processor_name[0] == '\0') {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_processor(
      processor_name, samples, length, sample_rate, to_params(params, param_count));
  set_mastering_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_processor_stereo(const char* processor_name, const float* left,
                                                    const float* right, size_t length,
                                                    int sample_rate,
                                                    const SonareMasteringParam* params,
                                                    size_t param_count,
                                                    SonareMasteringStereoResult* out) {
  if (!out || !processor_name || processor_name[0] == '\0') {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_processor_stereo(
      processor_name, left, right, length, sample_rate, to_params(params, param_count));
  out->length = result.left.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mastering_processor_names(void) {
  static thread_local std::string names;
  if (names.empty()) {
    std::ostringstream stream;
    auto processors = sonare::mastering::api::processor_names();
    for (size_t index = 0; index < processors.size(); ++index) {
      if (index > 0) stream << '\n';
      stream << processors[index];
    }
    names = stream.str();
  }
  return names.c_str();
}

const char* sonare_mastering_pair_processor_names(void) {
  static thread_local std::string names;
  return join_names(sonare::mastering::api::pair_processor_names(), names);
}

const char* sonare_mastering_pair_analysis_names(void) {
  static thread_local std::string names;
  return join_names(sonare::mastering::api::pair_analysis_names(), names);
}

const char* sonare_mastering_stereo_analysis_names(void) {
  static thread_local std::string names;
  return join_names(sonare::mastering::api::stereo_analysis_names(), names);
}

const char* sonare_mastering_insert_names(void) {
  static thread_local std::string names;
  return join_names(sonare::mastering::api::insert_factory_names(), names);
}

SonareError sonare_mastering_apply_pair_processor_ex(
    const char* processor_name, const float* source, size_t source_length, const float* reference,
    size_t reference_length, int sample_rate, const SonareMasteringParam* params,
    size_t param_count, SonareMasteringResult* out) {
  if (!out || !processor_name || processor_name[0] == '\0') {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareError err = validate_audio_params(source, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(reference, reference_length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_pair_processor(
      processor_name, source, reference, source_length, reference_length, sample_rate,
      to_params(params, param_count));
  set_mastering_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_pair_processor(const char* processor_name, const float* source,
                                                  const float* reference, size_t length,
                                                  int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, SonareMasteringResult* out) {
  return sonare_mastering_apply_pair_processor_ex(processor_name, source, length, reference, length,
                                                  sample_rate, params, param_count, out);
}

SonareError sonare_mastering_analyze_pair_ex(const char* analysis_name, const float* source,
                                             size_t source_length, const float* reference,
                                             size_t reference_length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             char** json_out) {
  if (!json_out || !analysis_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(source, source_length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(reference, reference_length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
  SONARE_C_TRY
  auto json = sonare::mastering::api::analyze_named_pair(
      analysis_name, source, reference, source_length, reference_length, sample_rate,
      to_params(params, param_count));
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_analyze_pair(const char* analysis_name, const float* source,
                                          const float* reference, size_t length, int sample_rate,
                                          const SonareMasteringParam* params, size_t param_count,
                                          char** json_out) {
  return sonare_mastering_analyze_pair_ex(analysis_name, source, length, reference, length,
                                          sample_rate, params, param_count, json_out);
}

SonareError sonare_mastering_analyze_stereo(const char* analysis_name, const float* left,
                                            const float* right, size_t length, int sample_rate,
                                            const SonareMasteringParam* params, size_t param_count,
                                            char** json_out) {
  if (!json_out || !analysis_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
  SONARE_C_TRY
  auto json = sonare::mastering::api::analyze_named_stereo(
      analysis_name, left, right, length, sample_rate, to_params(params, param_count));
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_mastering_result(SonareMasteringResult* result) {
  if (!result) return;
  delete[] result->samples;
  result->samples = nullptr;
  result->length = 0;
}

void sonare_free_mastering_stereo_result(SonareMasteringStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  result->left = nullptr;
  result->right = nullptr;
  result->length = 0;
}
