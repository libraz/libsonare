#pragma once

/// @file parametric.h
/// @brief RBJ Cookbook style parametric equalizer.

#include <array>
#include <cstddef>
#include <vector>

#include "mastering/eq/eq_band.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

class ParametricEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = 24;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void prepare_channels(int num_channels);

  void set_band(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();

  // Automatable parameters (RT-safe: recomputes the affected band's biquad
  // coefficients in place, preserves filter state). Bands are laid out in
  // blocks of 3, so band `b` occupies ids `3*b .. 3*b+2`:
  //   3*b + 0 = frequency_hz (clamped to (0 Hz, Nyquist))
  //   3*b + 1 = gain_db
  //   3*b + 2 = Q (clamped to > 0)
  // Only bands that are currently enabled produce audible coefficient changes;
  // band type and coefficient mode are not automatable. Ids for b >= kMaxBands
  // are rejected (return false).
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: per band `b` (0 .. kMaxBands-1), id 3*b+0 = "band<b>.frequencyHz",
  // 3*b+1 = "band<b>.gainDb", 3*b+2 = "band<b>.q" (keys match the construction-time band prefix).
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  const EqBand& band(size_t index) const;
  double sample_rate() const { return sample_rate_; }

 private:
  struct Coefficients {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
  };

  struct State {
    float z1 = 0.0f;
    float z2 = 0.0f;
  };

  static Coefficients make_coefficients(const EqBand& band, double sample_rate);
  void update_coefficients(size_t index);
  static void validate_band_index(size_t index);
  void ensure_prepared() const;

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int num_channels_ = 0;
  bool prepared_ = false;
  std::array<EqBand, kMaxBands> bands_{};
  std::array<Coefficients, kMaxBands> coefficients_{};
  std::array<std::vector<State>, kMaxBands> states_{};
};

}  // namespace sonare::mastering::eq
