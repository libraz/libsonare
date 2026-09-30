#pragma once

/// @file binaural_panner.h
/// @brief Binaural placement of one source on the horizontal plane.
///
/// The input pair is folded to mono and rendered through a measured HRTF ring
/// (the elevation-0 row of the shipped SADIE II D1 set, 5 degree steps, minimum
/// phase with a separate ITD; `binaural_ring.inc`). The two ring points either
/// side of the azimuth are mixed per sample with weights summing to one, and the
/// ITD is a third-order Lagrange fractional delay on the far ear. The speakers
/// output adds a mid/side crosstalk canceller for a +/-30 degree pair, built in
/// prepare() as a Tikhonov-regularised inverse of the same ring and crossed over
/// to the uncancelled signal below 200 Hz.
///
/// Latency is the canceller's modelling delay in both outputs, so switching the
/// output does not move the stream in time.

#include <cstddef>
#include <vector>

#include "rt/biquad_design.h"
#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

/// Where the rendered pair is heard. Ordinals follow the GS 3D `Out` byte (0 Speaker, 1 Phones).
enum class BinauralOutput {
  kSpeakers,  ///< crosstalk-cancelled for a +/-30 degree loudspeaker pair.
  kPhones,    ///< the binaural pair as is; the default.
};
inline constexpr int kBinauralOutputCount = 2;

struct BinauralPannerConfig {
  float azimuth_deg = 0.0f;   ///< 0 front, positive to the right; any angle wraps.
  bool auto_turn = false;     ///< rotate the source instead of holding azimuth_deg.
  float turn_rate_hz = 1.0f;  ///< revolutions per second while auto_turn, [0, 10].
  bool clockwise = true;      ///< rotation seen from above: front, right, back, left.
  BinauralOutput output = BinauralOutput::kPhones;
  float dry_wet = 1.0f;  ///< the panner is an insert, so wet by default.
};

/// Stereo-pair processor: planes beyond the pair pass through.
class BinauralPanner : public rt::ProcessorBase {
 public:
  explicit BinauralPanner(BinauralPannerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override { return xtc_delay_; }
  int tail_samples() const noexcept override;

  const BinauralPannerConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = azimuth_deg (wrapped to [-180, 180); glides over 20 ms)
  //   1 = auto_turn (0 or 1)
  //   2 = turn_rate_hz (clamped to [0, 10])
  //   3 = clockwise (0 or 1)
  //   4 = output (0 speakers, 1 phones; a fractional or unnamed value is refused; fades over 20 ms)
  //   5 = dry_wet (clamped to [0, 1])
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const BinauralPannerConfig& config);
  void design_canceller();
  void advance_position() noexcept;

  BinauralPannerConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;

  /// Right-ear ring at the host rate, [azimuth][tap]; the left ear is mirrored.
  std::vector<float> ring_;
  int taps_ = 0;
  /// ITD in host samples for ring points 0 .. 180 degrees (positive: left lags).
  std::vector<float> itd_;

  /// Mono input history for the fractional ITD reads (power-of-two ring).
  std::vector<float> mono_;
  size_t mono_write_ = 0;
  /// Per-ear delayed input, stored twice so a FIR reads one contiguous window.
  std::vector<float> ear_history_[2];
  int ear_pos_ = 0;

  /// Source position in degrees, [0, 360).
  double position_deg_ = 0.0;
  double ramp_step_deg_ = 0.0;
  int ramp_remaining_ = 0;
  int ramp_samples_ = 1;
  float ramp_target_deg_ = 0.0f;
  bool was_turning_ = false;
  bool primed_ = false;

  /// Canceller: mid and side inverses, the modelling delay and its history.
  std::vector<float> xtc_mid_;
  std::vector<float> xtc_side_;
  int xtc_taps_ = 0;
  int xtc_delay_ = 0;
  std::vector<float> ms_history_[2];
  int ms_pos_ = 0;
  /// LR4 crossover: [channel][stage], low-pass on the uncancelled path.
  rt::BiquadState low_[2][2];
  rt::BiquadState high_[2][2];
  float speakers_gain_ = 0.0f;

  /// Dry input delayed by the latency, per channel.
  std::vector<float> dry_[2];
  size_t dry_pos_ = 0;
};

}  // namespace sonare::mastering::stereo
