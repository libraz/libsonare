#pragma once

#include <vector>

#include "rt/adaa.h"
#include "rt/aliasing_control.h"
#include "rt/biquad_design.h"
#include "rt/oversampler.h"
#include "rt/parallel_paths.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct ExciterConfig {
  float frequency_hz = 3000.0f;
  float drive_db = 6.0f;
  float amount = 0.25f;
  float q = 1.0f;
  float even_odd_mix = 0.5f;
  sonare::rt::AliasingControl aliasing = sonare::rt::AliasingControl::None;
};

/// @brief Whether an exciter config would add harmonics.
/// @details The exciter is a color stage whose `amount` scales the added
/// harmonics; at zero amount it is a no-op. Parsers use this to auto-engage the
/// stage only when it would do something, unless an explicit `enabled` flag is
/// supplied. See tape_engages_color for the shared rationale.
inline bool exciter_engages_color(const ExciterConfig& config) { return config.amount > 0.0f; }

class Exciter : public rt::ProcessorBase {
 public:
  explicit Exciter(ExciterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const ExciterConfig& config);
  const ExciterConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = frequency_hz (clamped to > 0; recomputes biquad coeffs in place)
  //   1 = drive_db (read per sample)
  //   2 = amount (clamped to >= 0; read per sample)
  //   3 = q (clamped to > 0; recomputes biquad coeffs in place)
  //   4 = even_odd_mix (clamped to [0, 1]; read per sample)
  // Coefficient updates preserve the per-channel biquad delay state.
  // aliasing is an enum (not exposed).
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: 0=frequencyHz, 1=driveDb, 2=amount, 3=q, 4=evenOddMix
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// @brief None adds no latency; Oversample4x adds the harmonic
  ///   oversampler's streaming round-trip latency, to which the dry and
  ///   allpass paths are aligned.
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;

  static void validate_config(const ExciterConfig& config);

 private:
  void compute_coeffs();
  void update_coeff();
  void update_coeff_preserving_state();
  void ensure_state(int num_channels);
  void declare_path_latencies();
  using Biquad = rt::BiquadState;

  ExciterConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  Biquad bandpass_coeffs_;
  Biquad allpass_coeffs_;
  float even_dc_coefficient_ = 0.0f;
  // Same 20 Hz DC-block time constant, expressed for the
  // oversampled rate used inside the Oversample4x harmonic path.
  float even_dc_coefficient_oversampled_ = 0.0f;
  bool prepared_ = false;
  std::vector<Biquad> bandpass_;
  std::vector<Biquad> allpass_;
  std::vector<float> even_dc_;

  // Oversample4x support: the bandpass/allpass filters stay at the base rate
  // (they are linear and do not generate harmonics), but the squaring/tanh
  // harmonic-generation stage that would otherwise fold its high-order
  // products below Nyquist runs on the oversampled band signal. paths_ keeps
  // the untouched input (path 0) and the allpass "aligned" contribution
  // (path 1) time-matched with the harmonic content (path 2) before the three
  // are summed.
  static constexpr int kHarmonicOversampleFactor = 4;
  sonare::rt::Oversampler harmonic_oversampler_{kHarmonicOversampleFactor};
  std::vector<sonare::rt::Oversampler::StreamingState> harmonic_oversampler_states_;
  sonare::rt::ParallelPaths paths_;
  std::vector<float> band_scratch_;
  std::vector<float> aligned_scratch_;
  std::vector<float> oversampled_scratch_;
  std::vector<float> harmonic_scratch_;
};

}  // namespace sonare::mastering::saturation
