#pragma once

/// @file linear_phase.h
/// @brief Linear-phase FIR equalizer built from an FFT-domain response.

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/common/partitioned_convolver.h"
#include "mastering/common/processor_base.h"
#include "mastering/eq/parametric.h"

namespace sonare::mastering::eq {

struct LinearPhaseEqConfig {
  int fft_size = 2048;
  int kernel_size = 513;
  bool use_partitioned_convolution = true;
  /// 0 means use the `max_block_size` passed to prepare().
  int partition_size = 0;
};

class LinearPhaseEq : public common::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = ParametricEq::kMaxBands;

  explicit LinearPhaseEq(LinearPhaseEqConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_band(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();

  const EqBand& band(size_t index) const;
  const std::vector<float>& kernel() const { return kernel_; }
  int latency_samples() const noexcept override { return latency_samples_; }

 private:
  struct ChannelState {
    std::vector<float> history;
    size_t write_index = 0;
    std::unique_ptr<common::PartitionedConvolver> convolver;
  };

  void rebuild_kernel();
  void ensure_channel_state(int num_channels);
  void process_direct(float* samples, int num_samples, ChannelState& state) const;
  int active_partition_size() const noexcept;
  void validate_config() const;
  static void validate_band_index(size_t index);
  static float band_magnitude(const EqBand& band, double frequency_hz, double sample_rate);

  LinearPhaseEqConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int latency_samples_ = 0;
  bool prepared_ = false;
  std::array<EqBand, kMaxBands> bands_{};
  std::vector<float> kernel_;
  std::vector<ChannelState> states_;
};

}  // namespace sonare::mastering::eq
