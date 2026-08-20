#pragma once

#include <vector>

#include "mastering/saturation/waveshaper.h"
#include "rt/adaa.h"
#include "rt/delay_line.h"
#include "rt/nonlinearities.h"
#include "rt/oversampler.h"

namespace sonare::mastering::saturation {

struct SoftClipperConfig {
  float drive_db = 0.0f;
  float ceiling = 1.0f;
  float mix = 1.0f;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

class SoftClipper : public rt::ProcessorBase {
 public:
  explicit SoftClipper(SoftClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SoftClipperConfig& config);
  const SoftClipperConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset). Both are
  // read per sample in process_sample() with no precomputed coefficients:
  //   0 = drive_db
  //   1 = mix (clamped to [0, 1])
  // ceiling is NOT automatable: it normalizes the ADAA input, so changing it
  // would require clearing the antiderivative history. aliasing is an enum.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=driveDb, 1=mix
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// @brief None and Adaa1 add no integer latency; Oversample4x adds the
  ///   oversampler's streaming round-trip latency.
  int latency_samples() const noexcept override;

 private:
  static void validate_config(const SoftClipperConfig& config);
  void ensure_state(int num_channels);
  float process_sample(float sample, int channel);

  SoftClipperConfig config_{};
  bool prepared_ = false;
  int max_block_size_ = 0;
  static constexpr int kOversampleFactor = 4;
  static constexpr int kOversampleTapsPerPhase = 24;
  sonare::rt::Oversampler oversampler_{kOversampleFactor, kOversampleTapsPerPhase};
  // Oversample4x scratch and the dry-path delay that keeps the wet
  // oversampled signal time-aligned with the dry mix; preallocated in
  // prepare() so the audio-thread process() path never allocates.
  std::vector<sonare::rt::Oversampler::StreamingState> oversampler_states_;
  std::vector<sonare::rt::DelayLine> dry_delays_;
  std::vector<float> up_scratch_;
  std::vector<float> down_scratch_;
  std::vector<sonare::rt::Adaa1<sonare::rt::TanhNonlinearity>> tanh_adaa_;
};

}  // namespace sonare::mastering::saturation
