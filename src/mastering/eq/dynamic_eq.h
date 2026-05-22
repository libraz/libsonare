#pragma once

/// @file dynamic_eq.h
/// @brief Level-dependent dynamic equalizer.

#include <array>
#include <cstddef>
#include <vector>

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
  float sidechain_q = 1.0f;
  float sidechain_freq_hz = -1.0f;
  float attack_ms = 5.0f;
  float release_ms = 50.0f;
  float lookahead_ms = 0.0f;
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
  /// Borrows sidechain buffers until the next set/clear/process call.
  void set_sidechain(const float* const* channels, int num_channels, int num_samples);
  void clear_sidechain();

  const DynamicEqBand& band(size_t index) const;
  float last_detector_db() const { return last_detector_db_; }
  float last_band_detector_db(size_t index) const;
  float last_applied_gain_db(size_t index) const;

 private:
  static void validate_index(size_t index);
  static void validate_band(const DynamicEqBand& band);
  static float detector_db(const float* const* channels, int num_channels, int num_samples);
  static float band_detector_db(const float* const* channels, int num_channels, int num_samples,
                                double sample_rate, const DynamicEqBand& band);
  static float dynamic_gain_delta(const DynamicEqBand& band, float detector_db);
  void rebuild(int num_samples = 0);
  void validate_sidechain(int expected_samples) const;

  ParametricEq eq_;
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::array<DynamicEqBand, kMaxBands> bands_{};
  std::array<float, kMaxBands> last_band_detector_db_{};
  std::array<float, kMaxBands> last_applied_gain_db_{};
  std::array<float, kMaxBands> smoothed_gain_db_{};
  float last_detector_db_ = sonare::constants::kFloorDb;
  const float* const* sidechain_channels_ = nullptr;
  int sidechain_num_channels_ = 0;
  int sidechain_num_samples_ = 0;
};

}  // namespace sonare::mastering::eq
