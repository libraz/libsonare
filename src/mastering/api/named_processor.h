#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sonare::mastering::api {

struct Param {
  std::string key;
  double value = 0.0;
};

struct MonoResult {
  std::vector<float> samples;
  int sample_rate = 0;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

struct StereoResult {
  std::vector<float> left;
  std::vector<float> right;
  int sample_rate = 0;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

std::vector<std::string> processor_names();

MonoResult apply_named_processor(const std::string& name, const float* samples, std::size_t length,
                                 int sample_rate, const std::vector<Param>& params = {});

StereoResult apply_named_processor_stereo(const std::string& name, const float* left,
                                          const float* right, std::size_t length, int sample_rate,
                                          const std::vector<Param>& params = {});

std::vector<std::string> pair_processor_names();
std::vector<std::string> pair_analysis_names();
std::vector<std::string> stereo_analysis_names();

MonoResult apply_named_pair_processor(const std::string& name, const float* source,
                                      const float* reference, std::size_t length, int sample_rate,
                                      const std::vector<Param>& params = {});

std::string analyze_named_pair(const std::string& name, const float* source, const float* reference,
                               std::size_t length, int sample_rate,
                               const std::vector<Param>& params = {});

std::string analyze_named_stereo(const std::string& name, const float* left, const float* right,
                                 std::size_t length, int sample_rate,
                                 const std::vector<Param>& params = {});

}  // namespace sonare::mastering::api
