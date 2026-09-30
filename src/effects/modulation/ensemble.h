#pragma once

/// @file ensemble.h
/// @brief Band-limited three-tap ensemble chorus (string-machine style, 3-phase).
///
/// Three interpolated delay lines per channel are modulated by a SLOW and a
/// FAST LFO simultaneously, with the three taps 120 degrees apart on both —
/// the dual-rate 3-phase scheme is what turns a single voice into a
/// "section". The structure is digital, not a bucket-brigade emulation: the
/// wet path is band-limited by a gentle one-pole lowpass (`tone_hz`) and the
/// source spreads into stereo (the right channel reads the same 3-phase
/// pattern with inverted LFO polarity).

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct EnsembleConfig {
  /// Dual LFO rates (Hz): the slow "chorale" sweep and the fast shimmer.
  float rate_slow_hz = 0.6f;
  float rate_fast_hz = 5.5f;
  /// Modulation depths (ms) for each LFO.
  float depth_slow_ms = 1.8f;
  float depth_fast_ms = 0.25f;
  /// Nominal bucket delay (ms).
  float center_delay_ms = 5.0f;
  /// BBD bandwidth: one-pole lowpass on the wet path (Hz).
  float tone_hz = 6500.0f;
  float dry_wet = 0.5f;
  /// Single-rate drive (Hz): > 0 sets the slow LFO and derives the fast one at
  /// the rate_fast_hz / rate_slow_hz ratio; 0 leaves the two rates independent.
  float rate_hz = 0.0f;
  /// Per-voice spread: voice i of N is offset by an evenly spaced value from
  /// -dev to +dev. Pre-delay in ms (clamped to [0, 20]), depth as a fraction of
  /// the voice's depth (clamped to [-1, 1]), pan in [-1, 1] balance units.
  float pre_delay_dev_ms = 0.0f;
  float depth_dev = 0.0f;
  float pan_dev = 0.0f;
  /// How the delay lines read between samples; see DelayInterpolation.
  DelayInterpolation interpolation = DelayInterpolation::kLinear;
};

class Ensemble : public rt::ProcessorBase {
 public:
  explicit Ensemble(EnsembleConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_slow_hz   (>= 0)
  //   1 = rate_fast_hz   (>= 0)
  //   2 = depth_slow_ms  (clamped to [0, 10])
  //   3 = depth_fast_ms  (clamped to [0, 10])
  //   4 = center_delay_ms (clamped to [0, 25])
  //   5 = tone_hz        (clamped to [500, 20000])
  //   6 = dry_wet        (clamped to [0, 1])
  //   7 = rate_hz        (>= 0; 0 = independent slow/fast rates)
  //   8 = pre_delay_dev_ms (clamped to [0, 20]; every voice stays inside the
  //                        delay line prepare() sized)
  //   9 = depth_dev      (clamped to [-1, 1])
  //  10 = pan_dev        (clamped to [0, 1])
  //  11 = interpolation  (0 linear, 1 Lagrange3)
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Returns the tone filter to rest once a non-finite value has reached it,
  /// once per block (see util/non_finite_state.h).
  void discard_non_finite() noexcept;

  /// Applies the slow/fast rates, or the single-rate drive when rate_hz > 0.
  void apply_rates() noexcept;

  EnsembleConfig config_{};
  double sample_rate_ = 48000.0;
  /// 3 taps x 2 channels.
  std::array<ModDelayLine, 6> delays_;
  /// 3-phase (0 / 120 / 240 degree) slow and fast LFO banks.
  std::array<Lfo, 3> slow_lfos_;
  std::array<Lfo, 3> fast_lfos_;
  std::array<float, 2> tone_state_{};
};

}  // namespace sonare::effects::modulation
