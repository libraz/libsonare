#pragma once

/// @file bitcrusher.h
/// @brief A quantizer and a sample-and-hold, either of which can be left out of
///        the path entirely.
///
/// The hold rate is kept in hertz and turned into a phase increment in
/// prepare(), so its aperture null stays at one frequency whatever rate the host
/// runs at. A hold stored as a count of samples moves that null every time the
/// rate changes, which is the whole reason the rate is not stored that way.

#include <array>
#include <cstdint>
#include <vector>

#include "mastering/final/dither.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

/// Whether the quantizer is in the path at all.
enum class QuantizerMode {
  kFixedDepth,  ///< Rounds to bit_depth levels. What every existing caller has.
  kOff,         ///< The sample reaches the hold exactly as it arrived.
};

struct BitCrusherConfig {
  int bit_depth = 12;
  int downsample_factor = 1;
  float mix = 1.0f;
  final::DitherType dither_type = final::DitherType::None;
  uint32_t dither_seed = 0x51A7E5u;
  /// Sample-and-hold rate, in hertz. Zero leaves the cadence to
  /// downsample_factor, which is what every existing caller has; a positive rate
  /// replaces it, so only ever one of the two is counting.
  float hold_hz = 0.0f;
  QuantizerMode quantizer_mode = QuantizerMode::kFixedDepth;
};

class BitCrusher : public rt::ProcessorBase {
 public:
  explicit BitCrusher(BitCrusherConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const BitCrusherConfig& config);
  const BitCrusherConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = bit_depth (rounded and clamped to [1, 24]; quantization resolution)
  //   1 = mix (clamped to [0, 1])
  //   2 = hold_hz (re-derives the phase increment in place; the phase runs on)
  // downsample_factor changes the hold cadence in samples, dither_type is an
  // enum, and quantizer_mode selects whether the quantizer is in the path at
  // all, so none of the three is exposed here.
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  // Automatable parameters: 0=bitDepth, 1=mix, 2=holdHz
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const BitCrusherConfig& config);
  /// @brief Derives the per-sample phase increment from @c config_.hold_hz and
  ///        the prepared rate. The rate is what the null is fixed against.
  void update_hold_increment() noexcept;
  /// @brief Returns this channel's cells to rest when a non-finite value has
  ///        reached them (see util/non_finite_state.h). Called once per block.
  /// @return true when any cell was discarded.
  bool discard_non_finite_state(size_t channel) noexcept;
  float quantize(float sample, int bit_depth, int channel);
  float dither_noise(int channel);
  void ensure_state(int num_channels);

  BitCrusherConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  double hold_increment_ = 0.0;
  std::vector<float> held_;
  std::vector<int> counters_;
  /// Per-channel hold phase, in periods of the hold rate. Carries no audio --
  /// only the increment feeds it -- so it is not one of the cells
  /// discard_non_finite_state() returns to rest.
  std::vector<double> hold_phase_;
  std::vector<uint32_t> rng_state_;
  std::vector<std::array<float, 9>> error_history_;
};

}  // namespace sonare::mastering::saturation
