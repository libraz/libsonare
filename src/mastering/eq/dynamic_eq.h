#pragma once

/// @file dynamic_eq.h
/// @brief Level-dependent dynamic equalizer.

#include <array>
#include <cstddef>

#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

struct DynamicEqBand {
  EqBandType type = EqBandType::Peak;
  float frequency_hz = 1000.0f;
  float static_gain_db = 0.0f;
  float q = 1.0f;
  float threshold_db = -24.0f;
  float ratio = 2.0f;
  float range_db = -6.0f;
  bool enabled = false;
};

class DynamicEq : public common::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = 8;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_band(size_t index, const DynamicEqBand& band);
  void clear_band(size_t index);
  void clear();

  const DynamicEqBand& band(size_t index) const;
  float last_detector_db() const { return last_detector_db_; }
  float last_applied_gain_db(size_t index) const;

 private:
  static void validate_index(size_t index);
  static void validate_band(const DynamicEqBand& band);
  static float detector_db(float* const* channels, int num_channels, int num_samples);
  static float dynamic_gain_delta(const DynamicEqBand& band, float detector_db);
  void rebuild(float detector_db);

  ParametricEq eq_;
  bool prepared_ = false;
  std::array<DynamicEqBand, kMaxBands> bands_{};
  std::array<float, kMaxBands> last_applied_gain_db_{};
  float last_detector_db_ = -120.0f;
};

}  // namespace sonare::mastering::eq
