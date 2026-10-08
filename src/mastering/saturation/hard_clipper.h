#pragma once

#include <cstdint>
#include <vector>

#include "rt/adaa.h"
#include "rt/aliasing_control.h"
#include "rt/nonlinearities.h"
#include "rt/overflow_counter.h"
#include "rt/oversampler.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct HardClipperConfig {
  /// @brief Clip level. The None, Adaa1 and Adaa2 modes bound every emitted
  ///   sample to it. Oversample4x bounds the signal at 4x and decimates, and the
  ///   decimation lowpass rings above the bound it was fed: measured +1.6 dB
  ///   over the ceiling on a 0.36 fs sine clipped 5 dB deep and +3.0 dB on
  ///   full-scale white noise. Re-bounding the decimated output would fold the
  ///   aliases the mode exists to remove back in (a per-sample clamp measured
  ///   -19.5 dB against the mode's -87 dB), so a hard sample bound after this
  ///   mode is the true-peak limiter's job, or the other modes' promise.
  float ceiling = 1.0f;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

class HardClipper : public rt::ProcessorBase {
 public:
  explicit HardClipper(HardClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const HardClipperConfig& config);
  const HardClipperConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation):
  //   0 = ceiling (clamped to > 0)
  // In the None and Oversample4x aliasing modes the ceiling is read per
  // sample, so the change is applied with no state reset. In the Adaa1/Adaa2
  // modes the ceiling is baked into the ADAA nonlinearity objects, which
  // expose no in-place threshold mutator; updating it therefore reconstructs
  // those objects, clearing their 1-2 sample antiderivative history. For a
  // clipper this momentary discontinuity is inaudible and acceptable.
  // aliasing is an enum (not exposed).
  bool set_parameter_impl(unsigned int param_id, float value) override;

  // Automatable parameters: 0=ceiling
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// @brief Returns the processing latency for the active mode.
  /// @details Adaa1 adds half a sample (Q8 128), Adaa2 one sample, Oversample4x
  ///   the oversampler's streaming round-trip latency. The clipper is fully wet,
  ///   so there is no parallel path to align.
  int latency_samples() const noexcept override;
  int tail_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;

  /// @brief Infinities this stage replaced with the ceiling.
  /// @details Monotonic since @ref prepare, which clears it; @ref reset does not.
  ///          An infinity leaves as the ceiling, indistinguishable from a sample
  ///          the clipper meant to produce, so this count is the only thing that
  ///          separates such a stream from a clean one. A NaN is NOT counted: the
  ///          clamp's two comparisons are both false for it whatever the ceiling
  ///          and the aliasing mode, so it passes through and is already visible
  ///          to the caller. The Adaa1/Adaa2 modes substitute nothing at all and
  ///          never move this count.
  std::uint32_t non_finite_substitution_count() const noexcept {
    return non_finite_substitution_count_.load();
  }

 private:
  static void validate_config(const HardClipperConfig& config);
  void ensure_state(int num_channels);
  void rebuild_adaa();
  float process_sample(float sample, int channel, std::uint32_t& substituted);

  HardClipperConfig config_{};
  bool prepared_ = false;
  int max_block_size_ = 0;
  static constexpr int kOversampleFactor = 4;
  sonare::rt::Oversampler oversampler_{kOversampleFactor};
  // Oversample4x scratch: preallocated in prepare() so the audio-thread
  // process() path never allocates. Blocks wider than max_block_size_ are
  // rejected rather than resized.
  std::vector<sonare::rt::Oversampler::StreamingState> oversampler_states_;
  std::vector<float> up_scratch_;
  std::vector<float> down_scratch_;
  std::vector<sonare::rt::Adaa1<sonare::rt::HardClipNonlinearity>> hard_clip_adaa_;
  std::vector<sonare::rt::Adaa2<sonare::rt::HardClipNonlinearity>> hard_clip_adaa2_;
  sonare::rt::OverflowCounter non_finite_substitution_count_{};
};

}  // namespace sonare::mastering::saturation
