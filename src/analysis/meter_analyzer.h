#pragma once

/// @file meter_analyzer.h
/// @brief Multi-comb meter analysis for time-signature estimation.

#include <vector>

#include "analysis/beat_analyzer.h"

namespace sonare {

/// @brief Configuration for multi-comb meter analysis.
struct MeterConfig {
  std::vector<int> candidate_numerators = {3, 4, 6};
  int denominator = 4;
  float downbeat_weight = 1.0f;
  float measure_weight = 0.5f;
  float subdivision_weight = 0.15f;
  float compound_subdivision_threshold = 0.85f;
};

/// @brief Result from meter analysis.
struct MeterResult {
  TimeSignature time_signature{4, 4, 0.0f};
  int downbeat_phase = 0;
  std::vector<float> candidate_scores;
};

/// @brief Estimates meter from beat-aligned onset strengths using a multi-comb score.
class MeterAnalyzer {
 public:
  MeterAnalyzer(const std::vector<float>& onset_strength, const std::vector<Beat>& beats,
                const MeterConfig& config = MeterConfig());

  const MeterResult& result() const { return result_; }
  TimeSignature time_signature() const { return result_.time_signature; }

 private:
  void analyze(const std::vector<float>& onset_strength, const std::vector<Beat>& beats);

  MeterConfig config_;
  MeterResult result_;
};

MeterResult estimate_meter(const std::vector<float>& onset_strength, const std::vector<Beat>& beats,
                           const MeterConfig& config = MeterConfig());

}  // namespace sonare
