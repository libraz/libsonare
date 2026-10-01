#pragma once

/// @file bitcrusher.h
/// @brief A quantizer and a sample-and-hold, either of which can be left out of
///        the path entirely, with optional filters around them and four noise
///        sources beside them.
///
/// The hold rate is kept in hertz and turned into a phase increment in
/// prepare(), so its aperture null stays at one frequency whatever rate the host
/// runs at. A hold stored as a count of samples moves that null every time the
/// rate changes, which is the whole reason the rate is not stored that way.
///
/// Every filter corner and generator rate is kept in hertz and turned into a
/// coefficient in prepare(). The noise sources are shared by all channels and
/// use fixed seeds, so a render is repeatable; a level of 0 leaves a source out.

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

/// What the post-hold filter (@c post_filter_hz) does with its corner.
enum class BitCrusherFilterType {
  kOff,       ///< The corner is ignored.
  kLowpass,   ///< One pole low-pass. The default, so a corner alone is a low-pass.
  kHighpass,  ///< One pole high-pass.
};

/// The disc's record type, as the Lo-Fi "Disc Type" states print it; the value
/// indexes kDiscClickRateHz.
enum class BitCrusherDiscType {
  kLp,   ///< 4 clicks a second.
  kEp,   ///< 8.
  kSp,   ///< 16.
  kRnd,  ///< 32.
};

/// Hold rates of the type ladder, in hertz, indexed by @c type_ladder - 1: the
/// clock and its two-thirds and one-half, over nine states.
inline constexpr std::array<float, 9> kTypeLadderHoldHz = {16000.0f, 10666.666666666668f, 8000.0f,
                                                           16000.0f, 10666.666666666668f, 8000.0f,
                                                           16000.0f, 10666.666666666668f, 8000.0f};

/// Clicks a second of the four disc types, indexed by @c disc_type.
inline constexpr std::array<float, 4> kDiscClickRateHz = {4.0f, 8.0f, 16.0f, 32.0f};
inline constexpr int kBitCrusherDiscTypeCount = static_cast<int>(kDiscClickRateHz.size());

/// One-pole corner the radio generator is shaped by before its own low-pass.
inline constexpr float kRadioNoiseCornerHz = 16000.0f;

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
  /// Level of each noise source, linear, [0, 1]. Zero leaves the source out.
  float radio_noise_level = 0.0f;
  float wp_noise_level = 0.0f;
  float disc_noise_level = 0.0f;
  float hum_level = 0.0f;
  /// How far the radio is off station, [0, 1]; scales the radio source.
  float noise_detune = 1.0f;
  /// Whether the white/pink source is pink.
  bool wp_noise_pink = false;
  /// Index into kDiscClickRateHz, 0-3.
  BitCrusherDiscType disc_type = BitCrusherDiscType::kLp;
  /// Mains frequency of the hum source, in hertz.
  float hum_hz = 50.0f;
  /// One-pole low-pass on each noise source, in hertz. Zero bypasses it.
  float noise_lpf_hz = 0.0f;  ///< The radio source.
  float wp_noise_lpf_hz = 0.0f;
  float disc_noise_lpf_hz = 0.0f;
  float hum_lpf_hz = 0.0f;
  /// One-pole low-pass ahead of the quantizer, in hertz. Zero leaves it out.
  float pre_filter_hz = 0.0f;
  /// One-pole filter after the hold, in hertz, of the kind @c filter_type names.
  /// Zero leaves it out.
  float post_filter_hz = 0.0f;
  BitCrusherFilterType filter_type = BitCrusherFilterType::kLowpass;
  /// Sums the output to one channel written to all of them.
  bool mono = false;
  /// 0 leaves the hold cadence to hold_hz / downsample_factor; 1-9 selects the
  /// hold rate kTypeLadderHoldHz[type_ladder - 1] in their place.
  int type_ladder = 0;
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
  //   3-6 = radio / wp / disc / hum noise level (clamped to [0, 1])
  //   7 = noise_detune (clamped to [0, 1])
  //   8-11 = noise_lpf_hz / wp / disc / hum (refused when negative or non-finite)
  //   12 = pre_filter_hz, 13 = post_filter_hz
  //   14 = hum_hz, 15 = wp_noise_pink, 16 = disc_type, 17 = filter_type, 18 = mono,
  //   19 = type_ladder
  // downsample_factor changes the hold cadence in samples, dither_type is an
  // enum and quantizer_mode selects whether the quantizer is in the path at all,
  // so none of the three is exposed. type_ladder is exposed: every ladder rate is
  // known before prepare().
  bool set_parameter_impl(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  // Automatable parameters: 0=bitDepth, 1=mix, 2=holdHz, 3=radioNoiseLevel,
  // 4=wpNoiseLevel, 5=discNoiseLevel, 6=humLevel, 7=noiseDetune, 8=noiseLpfHz,
  // 9=wpNoiseLpfHz, 10=discNoiseLpfHz, 11=humLpfHz, 12=preFilterHz,
  // 13=postFilterHz, 14=humHz, 15=wpNoisePink, 16=discType, 17=filterType, 18=mono,
  // 19=typeLadder
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const BitCrusherConfig& config);
  /// @brief Derives the per-sample phase increment from @c config_.hold_hz and
  ///        the prepared rate. The rate is what the null is fixed against.
  void update_hold_increment() noexcept;
  /// @brief Derives every filter and generator coefficient from the configured
  ///        corners and the prepared rate.
  void update_coefficients() noexcept;
  /// @brief Fills @c noise_ with the summed noise sources for @p count frames.
  void render_noise(int count) noexcept;
  /// @brief Returns this channel's cells to rest when a non-finite value has
  ///        reached them (see util/non_finite_state.h). Called once per block.
  /// @return true when any cell was discarded.
  bool discard_non_finite_state(size_t channel) noexcept;
  float quantize(float sample, int bit_depth, int channel);
  float dither_noise(int channel);
  void ensure_state(int num_channels);

  /// One noise generator: its random state and whatever memory its shape needs.
  struct NoiseSource {
    uint32_t rng = 0;
    double mem[3] = {0.0, 0.0, 0.0};
  };
  /// Generators, one seed each. The white/pink source is two generators of which
  /// one is drawn; the four low-passes belong to the sources, not the generators.
  enum SourceIndex : size_t { kRadio, kWhite, kPink, kDisc, kHum, kSourceCount };
  enum LpfIndex : size_t { kRadioLpf, kWpLpf, kDiscLpf, kHumLpf, kLpfCount };
  /// One TPT integrator step of a low-pass with gain @p g (see update_coefficients()).
  static float lowpass_tick(double g, float x, float& state) noexcept;
  /// TPT gain tan(pi fc / sr) / (1 + tan(pi fc / sr)) for a corner in hertz, or
  /// zero for a corner of zero (bypass).
  double one_pole_gain(float corner_hz) const noexcept;

  BitCrusherConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  double hold_increment_ = 0.0;
  double pre_gain_ = 0.0;
  double post_gain_ = 0.0;
  double lpf_gain_[kLpfCount] = {};
  float lpf_state_[kLpfCount] = {};
  double radio_shape_pole_ = 0.0;
  double disc_chance_ = 0.0;
  double hum_step_ = 0.0;
  NoiseSource sources_[kSourceCount];
  std::vector<float> noise_;
  std::vector<float> pre_state_;
  std::vector<float> post_state_;
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
