#pragma once

/// @file cut_filter.h
/// @brief Dedicated high-pass and low-pass cut filters.

#include <array>
#include <vector>

#include "mastering/common/biquad_design.h"
#include "mastering/common/processor_base.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/linear_phase.h"
#include "util/constants.h"

namespace sonare::mastering::eq {

enum class CutFilterSlope {
  Db12PerOct,
  Db24PerOct,
  Db6PerOct,
  Db18PerOct,
  Db30PerOct,
  Db36PerOct,
  Db42PerOct,
  Db48PerOct,
  Db54PerOct,
  Db60PerOct,
  Db66PerOct,
  Db72PerOct,
  Db78PerOct,
  Db84PerOct,
  Db90PerOct,
  Db96PerOct,
  Brickwall,
};

class CutFilter : public common::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;
  void prepare_channels(int num_channels);

  void set_high_pass(float frequency_hz, float q = sonare::constants::kButterworthQ,
                     CutFilterSlope slope = CutFilterSlope::Db12PerOct, bool enabled = true);
  void set_low_pass(float frequency_hz, float q = sonare::constants::kButterworthQ,
                    CutFilterSlope slope = CutFilterSlope::Db12PerOct, bool enabled = true);
  void clear_high_pass();
  void clear_low_pass();
  void clear();

  CutFilterSlope high_pass_slope() const { return high_pass_slope_; }
  CutFilterSlope low_pass_slope() const { return low_pass_slope_; }

  // Automatable parameters (RT-safe: recomputes affected biquad coefficients in
  // place via apply_high_pass()/apply_low_pass(), preserves filter state):
  //   0 = high-pass frequency_hz (clamped to (0 Hz, Nyquist))
  //   1 = high-pass Q (clamped to > 0; resonance is applied to the final
  //       second-order stage; 6 dB/oct uses its first-order Butterworth stage)
  //   2 = low-pass frequency_hz (clamped to (0 Hz, Nyquist))
  //   3 = low-pass Q (same resonance rule as id 1)
  // The slope enum is not automatable.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  struct State {
    float z1 = 0.0f;
    float z2 = 0.0f;
  };

  struct Section {
    common::BiquadCoeffs coeffs{};
    bool enabled = false;
  };

  static constexpr size_t kMaxOrder = 16;
  static constexpr size_t kMaxSections = 1 + kMaxOrder / 2;

  void apply_high_pass();
  void apply_low_pass();
  void build_sections(std::array<Section, kMaxSections>& sections, EqBandType type,
                      float frequency_hz, float q, bool enabled, CutFilterSlope slope);
  void rebuild_brickwall();
  void ensure_channel_state(int num_channels);
  bool high_pass_is_brickwall() const noexcept;
  bool low_pass_is_brickwall() const noexcept;
  void process_stage(const std::array<Section, kMaxSections>& sections,
                     std::array<std::vector<State>, kMaxSections>& states, float* samples,
                     int channel, int num_samples) const;

  EqBand high_pass_{EqBandType::HighPass, 20.0f, 0.0f, sonare::constants::kButterworthQ, false};
  EqBand low_pass_{EqBandType::LowPass, 20000.0f, 0.0f, sonare::constants::kButterworthQ, false};
  CutFilterSlope high_pass_slope_ = CutFilterSlope::Db12PerOct;
  CutFilterSlope low_pass_slope_ = CutFilterSlope::Db12PerOct;
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  int num_channels_ = 0;
  LinearPhaseEq brickwall_{{8192, 2049, true, 0}};
  std::array<Section, kMaxSections> high_pass_sections_{};
  std::array<Section, kMaxSections> low_pass_sections_{};
  std::array<std::vector<State>, kMaxSections> high_pass_states_{};
  std::array<std::vector<State>, kMaxSections> low_pass_states_{};
};

}  // namespace sonare::mastering::eq
