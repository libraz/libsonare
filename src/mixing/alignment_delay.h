#pragma once

/// @file alignment_delay.h
/// @brief Integer and fractional-sample channel alignment delay.

#include <algorithm>
#include <vector>

#include "rt/delay_line.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

enum class FractionalDelayMode {
  None,
  // Default fractional-delay mode for alignment/PDC. The implementation is a
  // 3rd-order Lagrange FIR: stable and predictable for delay changes, with a
  // deliberate high-frequency magnitude droop for fractional delays.
  Lagrange3,
};

class AlignmentDelay : public rt::ProcessorBase {
 public:
  explicit AlignmentDelay(int delay_samples = 0);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override { return delay_samples_; }
  // Reports the exact requested Q8 delay. latency_samples() intentionally
  // returns the integer floor for legacy callers; graph/mixing PDC should use
  // latency_samples_q8() to preserve the fractional part.
  int latency_samples_q8() const noexcept override { return delay_samples_q8_; }

  // Number of channels the delay should preallocate storage for. Must be set
  // (control thread) before prepare() so process() can run allocation-free for
  // the full channel count the host will pass. Defaults to a stereo pair.
  void set_prepared_channels(int num_channels) noexcept {
    prepared_channels_ = std::max(1, num_channels);
  }

  void set_delay_samples(int delay_samples);
  void set_delay_samples_q8(int delay_samples_q8,
                            FractionalDelayMode mode = FractionalDelayMode::Lagrange3);
  int delay_samples() const noexcept { return delay_samples_; }
  int delay_samples_q8() const noexcept { return delay_samples_q8_; }
  FractionalDelayMode fractional_mode() const noexcept { return fractional_mode_; }

 private:
  struct FractionalState {
    std::vector<float> buffer{0.0f};
    size_t write_index = 0;
  };

  void prepare_storage();
  float process_fractional(FractionalState& state, float input) const noexcept;

  int delay_samples_ = 0;
  int delay_samples_q8_ = 0;
  int prepared_channels_ = 0;
  FractionalDelayMode fractional_mode_ = FractionalDelayMode::None;
  std::vector<rt::DelayLine> delays_;
  std::vector<FractionalState> fractional_;
};

}  // namespace sonare::mixing
