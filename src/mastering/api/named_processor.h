#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/result_types.h"

namespace sonare::mastering::api {

struct Param {
  std::string key;
  double value = 0.0;
};

// Per-processor results share the same shape as the shared audio-result base
// types. They are exposed as aliases so callers (binding layers, tests) can
// keep using `MonoResult` / `StereoResult` while the field definitions live
// in a single place (@ref result_types.h).
using MonoResult = MonoAudioResult;
using StereoResult = StereoAudioResult;

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
