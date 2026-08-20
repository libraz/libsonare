#pragma once

#include <vector>

#include "rt/aliasing_control.h"
#include "rt/biquad_design.h"
#include "rt/delay_line.h"
#include "rt/oversampler.h"
#include "rt/processor_base.h"

namespace sonare::mastering::spectral {

struct PresenceEnhancerConfig {
  float amount = 0.2f;
  float drive = 2.0f;
  float center_frequency_hz = 3200.0f;
  float q = 1.2f;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

class PresenceEnhancer : public rt::ProcessorBase {
 public:
  explicit PresenceEnhancer(PresenceEnhancerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const PresenceEnhancerConfig& config);
  const PresenceEnhancerConfig& config() const { return config_; }

  // Automatable parameters (RT-safe: amount/drive are applied per-sample; the
  // bandpass center/Q recompute the cached biquad coefficients in place,
  // preserving filter state). Ids follow the PresenceEnhancerConfig declaration
  // order:
  //   0 = amount (clamped to [0, 1])
  //   1 = drive (clamped to > 0)
  //   2 = center_frequency_hz (clamped to > 0; recomputes bandpass coefficients)
  //   3 = q (clamped to > 0; recomputes bandpass coefficients)
  // aliasing is an enum (not exposed).
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=amount, 1=drive, 2=centerFrequencyHz, 3=q
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// @brief None adds no latency; Oversample4x adds the harmonic
  ///   oversampler's streaming round-trip latency.
  int latency_samples() const noexcept override;

  using Biquad = rt::BiquadState;

 private:
  static void validate_config(const PresenceEnhancerConfig& config);
  void ensure_state(int num_channels);

  PresenceEnhancerConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  std::vector<Biquad> bandpass_;

  // Oversample4x support: the bandpass filter stays at the base rate (it is
  // linear and does not generate harmonics), but the tanh harmonic-generation
  // stage that would otherwise fold its high-order odd harmonics below
  // Nyquist runs on the oversampled band signal. dry_delays_ keeps the
  // untouched input time-matched with the delayed harmonic content before
  // the two are summed.
  static constexpr int kHarmonicOversampleFactor = 4;
  static constexpr int kHarmonicTapsPerPhase = 24;
  sonare::rt::Oversampler harmonic_oversampler_{kHarmonicOversampleFactor, kHarmonicTapsPerPhase};
  std::vector<sonare::rt::Oversampler::StreamingState> harmonic_oversampler_states_;
  std::vector<sonare::rt::DelayLine> dry_delays_;
  std::vector<float> band_scratch_;
  std::vector<float> oversampled_scratch_;
  std::vector<float> harmonic_scratch_;
};

}  // namespace sonare::mastering::spectral
