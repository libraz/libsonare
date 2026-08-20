#pragma once

/// @file linear_phase.h
/// @brief Linear-phase FIR equalizer built from an FFT-domain response.

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/eq/parametric.h"
#include "rt/partitioned_convolver.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

struct LinearPhaseEqConfig {
  enum class Resolution {
    Custom = 0,
    Low,
    Medium,
    High,
    VeryHigh,
    Maximum,
  };

  int fft_size = 2048;
  int kernel_size = 513;
  bool use_partitioned_convolution = true;
  /// 0 means use the `max_block_size` passed to prepare().
  int partition_size = 0;
  Resolution resolution = Resolution::Custom;
};

class LinearPhaseEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = ParametricEq::kMaxBands;

  explicit LinearPhaseEq(LinearPhaseEqConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void prepare_channels(int num_channels);

  void set_band(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();

  // Automatable parameters. Bands use the same block-of-3 layout as ParametricEq
  // (band b -> ids 3*b freq, 3*b+1 gain_db, 3*b+2 Q):
  //   3*b + 0 = frequency_hz (clamped to (0 Hz, Nyquist))
  //   3*b + 1 = gain_db
  //   3*b + 2 = Q (clamped to > 0)
  // NOTE: unlike the IIR EQs, this rebuilds the FIR kernel and clears filter
  // history. It is not audio-thread safe; ChannelStrip insert automation rejects
  // these params via parameter_is_realtime_safe().
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: per band `b` (0 .. kMaxBands-1), id 3*b+0 = "band<b>.frequencyHz",
  // 3*b+1 = "band<b>.gainDb", 3*b+2 = "band<b>.q" (keys match the construction-time band prefix).
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

  const EqBand& band(size_t index) const;
  const std::vector<float>& kernel() const { return kernel_; }
  int latency_samples() const noexcept override { return latency_samples_; }

 private:
  struct ChannelState {
    std::vector<float> history;
    size_t write_index = 0;
    std::unique_ptr<sonare::rt::PartitionedConvolver> convolver;
    bool convolver_kernel_current = false;
    // Set when the convolver's internal state no longer describes the stream at
    // the current position: either a ragged (non-partition-aligned) block went
    // to the direct path and was never fed to it, or a fresh impulse response
    // reset it. The next partition-aligned block replays the FIR history
    // through it before using it again.
    bool convolver_stale = false;
    // Scratch block for that replay, sized to the partition so process() stays
    // allocation-free.
    std::vector<float> prime_scratch;
  };

  void rebuild_kernel();
  // Non-destructive band/parameter reconfiguration: recomputes the FIR taps and
  // refreshes each convolver without zeroing the FIR history, so a change does
  // not introduce a ~kernel-length silence gap. (All params are non-realtime;
  // these mutators are prepare-time/offline.)
  void reconfigure();
  void ensure_channel_state(int num_channels);
  void process_direct(float* samples, int num_samples, ChannelState& state) const;
  // Push a block of inputs into the time-domain FIR history ring (no output),
  // keeping it in lock-step with the partitioned convolver so process_direct
  // can take over seamlessly on a later ragged block.
  void feed_history(const float* samples, int num_samples, ChannelState& state) const;
  // Rebuild the partitioned convolver's frequency-domain ring and overlap-add
  // tail from the FIR history so it resumes in step with the stream at the
  // current position, whatever block sizes preceded it. Outputs are discarded.
  void reprime_convolver(ChannelState& state) const;
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
  // prepare() fills this to the common realtime channel capacity. A larger
  // explicit prepare_channels() call may grow it on the control thread.
  std::vector<ChannelState> states_;
};

}  // namespace sonare::mastering::eq
