#pragma once

#include <vector>

#include "rt/biquad_design.h"
#include "rt/oversampler.h"
#include "rt/parallel_paths.h"
#include "rt/processor_base.h"
#include "rt/tail_budget.h"

namespace sonare::mastering::spectral {

struct AirBandConfig {
  float amount = 0.25f;
  float shelf_frequency_hz = 12000.0f;
  float dynamic_threshold_db = -36.0f;
  float dynamic_range_db = 3.0f;
};

class AirBand : public rt::ProcessorBase {
 public:
  explicit AirBand(AirBandConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  // The dry shelf path is aligned to the harmonic oversampling round trip by
  // paths_, so the processor reports that round trip as its I/O latency.
  int latency_samples() const noexcept override { return paths_.latency_samples(); }
  int latency_samples_q8() const noexcept override { return paths_.latency_samples_q8(); }
  /// The detector, the oversampled harmonic path's FIR ring and the dynamic shelf, in series.
  int tail_samples() const noexcept override {
    rt::TailBudget tail;
    if (!detector_.empty()) tail.section(detector_.front().c);
    tail.delay(harmonic_oversampler_.streaming_round_trip_latency_samples());
    if (!shelf_.empty()) tail.section(shelf_.front().c);
    return tail.samples();
  }
  void set_config(const AirBandConfig& config);

  // Automatable parameters (RT-safe: updates config in place; the shelf gain is
  // refreshed at a short control interval and coefficient-interpolated, so the
  // change takes effect on the next block without resetting filter/envelope state). Ids follow the
  // AirBandConfig declaration order:
  //   0 = amount (clamped to [0, 1])
  //   1 = shelf_frequency_hz (clamped to > 0; rebuilds the detector highpass)
  //   2 = dynamic_threshold_db
  //   3 = dynamic_range_db (clamped to >= 0)
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: 0=amount, 1=shelfFrequencyHz, 2=dynamicThresholdDb, 3=dynamicRangeDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  using Biquad = rt::BiquadState;

  static void validate_config(const AirBandConfig& config);

 private:
  void ensure_state(int num_channels);
  void rebuild_filters(int num_channels);
  /// @brief Returns the channel's recursive cells to rest when a non-finite
  ///        value has reached them (see util/non_finite_state.h).
  /// @return true when any cell was discarded.
  bool discard_non_finite_state(size_t channel) noexcept;

  AirBandConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int max_working_channels_ = 0;
  static constexpr int kHarmonicOversampleFactor = 4;
  static constexpr int kShelfControlInterval = 8;
  sonare::rt::Oversampler harmonic_oversampler_{kHarmonicOversampleFactor};
  std::vector<sonare::rt::Oversampler::StreamingState> harmonic_oversampler_states_;
  // Path 0 is the dry shelf output, path 1 the oversampled harmonic content.
  sonare::rt::ParallelPaths paths_;
  std::vector<float> band_scratch_;
  std::vector<float> oversampled_scratch_;
  std::vector<float> harmonic_scratch_;
  std::vector<float> envelope_;
  std::vector<float> shelf_gain_db_;
  std::vector<float> band_rms_sq_;
  std::vector<float> harmonic_rms_sq_;
  std::vector<float> harmonic_gain_;
  std::vector<int> shelf_control_samples_;
  float envelope_alpha_ = 0.005f;
  float normalization_rms_alpha_ = 0.0f;
  float harmonic_gain_alpha_ = 0.0f;
  std::vector<Biquad> shelf_;
  std::vector<Biquad> detector_;
  std::vector<Biquad> harmonic_filter_;
};

}  // namespace sonare::mastering::spectral
