#pragma once

/// @file pedal.h
/// @brief Overdrive and distortion pedal inserts.
///
/// Both pedals share one frame: a 16 Hz input high-pass, a clipping core run at
/// twice the host rate behind first-order antiderivative antialiasing, a
/// one-pole tone low-pass and an output level. The cores differ:
/// - overdrive: a 720 Hz high-passed clip path, gained and soft-clipped by
///   x / (1 + |x|^2.5)^(1/2.5), summed with the direct signal;
/// - distortion: gain, the op-amp's gain-bandwidth low-pass (1 MHz over the
///   gain, at most 20 kHz), then two hard clips with a 30 Hz coupling
///   high-pass between them.
///
/// Every coefficient is built from its corner in hertz once the rate is known.
/// The reported latency includes the 2x oversampler round trip and each core's
/// ADAA delay, expressed in Q8 so the fractional part is preserved.

#include <cstddef>
#include <vector>

#include "rt/adaa.h"
#include "rt/biquad_design.h"
#include "rt/nonlinearities.h"
#include "rt/oversampler.h"
#include "rt/parallel_paths.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct OverdriveConfig {
  float gain_db = 20.0f;    ///< Clip-path gain, [0, 41] dB.
  float tone_hz = 3000.0f;  ///< Tone low-pass corner, [500, 8000] Hz.
  float level_db = 0.0f;    ///< Output level.
};

struct DistortionConfig {
  float gain_db = 30.0f;    ///< Gain ahead of the clippers, [0, 60] dB.
  float tone_hz = 4000.0f;  ///< Tone low-pass corner, [475, 20000] Hz.
  float level_db = 0.0f;    ///< Output level.
};

/// @brief Symmetric soft clip x / (1 + |x|^2.5)^(1/2.5).
/// @details Its antiderivative has no elementary closed form, so it is read
///   from a table integrated once per process, with the large-argument tail in
///   closed form.
struct PedalSoftClipNonlinearity {
  float apply(float x) const noexcept;
  float antiderivative(float x) const noexcept;
};

namespace detail {

/// Overdrive clipping core, run at the oversampled rate.
class OverdriveCore {
 public:
  using Config = OverdriveConfig;
  static constexpr float kMaxGainDb = 41.0f;
  static constexpr float kMinToneHz = 500.0f;
  static constexpr float kMaxToneHz = 8000.0f;
  static constexpr int kLatencySamplesQ8 = rt::kAdaa1LatencySamplesQ8;
  static constexpr const char* kName = "Overdrive";

  void prepare(double oversampled_rate);
  void set_gain_db(float gain_db, double oversampled_rate) noexcept;
  void process(float* samples, size_t count) noexcept;
  void reset() noexcept;
  bool state_finite() const noexcept;

 private:
  rt::BiquadState clip_highpass_;
  rt::Adaa1<PedalSoftClipNonlinearity> clip_;
  float gain_ = 1.0f;
  // Path 0 is the direct signal, path 1 the clip path.
  rt::ParallelPaths paths_;
};

/// Distortion clipping core, run at the oversampled rate.
class DistortionCore {
 public:
  using Config = DistortionConfig;
  static constexpr float kMaxGainDb = 60.0f;
  static constexpr float kMinToneHz = 475.0f;
  static constexpr float kMaxToneHz = 20000.0f;
  static constexpr int kLatencySamplesQ8 = 256;
  static constexpr const char* kName = "Distortion";

  void prepare(double oversampled_rate);
  void set_gain_db(float gain_db, double oversampled_rate) noexcept;
  void process(float* samples, size_t count) noexcept;
  void reset() noexcept;
  bool state_finite() const noexcept;

 private:
  rt::BiquadState bandwidth_;
  rt::Adaa1<rt::HardClipNonlinearity> first_clip_;
  rt::BiquadState coupling_;
  rt::Adaa1<rt::HardClipNonlinearity> second_clip_;
  float gain_ = 1.0f;
};

/// @brief The shared pedal frame around one clipping core.
template <typename Core>
class Pedal : public rt::ProcessorBase {
 public:
  using Config = typename Core::Config;

  explicit Pedal(Config config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  void set_config(const Config& config);
  const Config& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset). A changed
  // gain or tone rebuilds its coefficients at the start of the next block:
  //   0 = gain_db (clamped to the core's range)
  //   1 = tone_hz (clamped to the core's range)
  //   2 = level_db (must produce a finite linear gain)
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: 0=gainDb, 1=toneHz, 2=levelDb.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  struct ChannelState {
    rt::BiquadState input_highpass;
    Core core;
    rt::BiquadState tone;
    rt::Oversampler::StreamingState oversampling;
  };

  static constexpr int kOversampleFactor = 2;

  static void validate_config(const Config& config);
  void build_coefficients() noexcept;
  bool channel_finite(const ChannelState& state) const noexcept;
  void reset_channel(ChannelState& state) noexcept;

  Config config_{};
  bool prepared_ = false;
  double sample_rate_ = 0.0;
  float built_gain_db_ = 0.0f;
  float built_tone_hz_ = 0.0f;
  rt::Oversampler oversampler_{kOversampleFactor};
  std::vector<ChannelState> states_;
  // Preallocated in prepare() so process() never allocates.
  std::vector<float> base_scratch_;
  std::vector<float> up_scratch_;
};

extern template class Pedal<OverdriveCore>;
extern template class Pedal<DistortionCore>;

}  // namespace detail

using Overdrive = detail::Pedal<detail::OverdriveCore>;
using Distortion = detail::Pedal<detail::DistortionCore>;

}  // namespace sonare::mastering::saturation
