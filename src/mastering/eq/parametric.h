#pragma once

/// @file parametric.h
/// @brief RBJ Cookbook style parametric equalizer.

#include <array>
#include <cstddef>
#include <vector>

#include "mastering/eq/eq_band.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

/// @brief Builds the band parameter descriptors shared by every equalizer that
///        exposes the parametric block-of-3 layout: band `b` occupies ids
///        `3*b .. 3*b+2` under the keys `band<b>.frequencyHz`, `band<b>.gainDb`
///        and `band<b>.q`. The keys mirror the construction-time band prefix
///        (configure_parametric).
std::vector<rt::ParamDescriptor> band_parameter_descriptors(size_t band_count);

/// @brief Builds `band<b>.<keys[f]>` descriptors for a processor whose band `b`
///        occupies ids `b*keys_per_band .. b*keys_per_band + keys_per_band-1`.
std::vector<rt::ParamDescriptor> banded_parameter_descriptors(size_t band_count,
                                                              const char* const* keys,
                                                              unsigned int keys_per_band);

/// @brief One biquad section, normalized so a0 == 1.
struct BiquadCoefficients {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// @brief Designs the section a single band is applied through.
/// @details The one place a band becomes coefficients, so anything that has to
/// agree with what is audible — the response curve a caller draws, most of all —
/// asks here rather than reimplementing the design. A disabled band returns the
/// identity. Composite band types (TiltShelf, FlatTilt) are not single sections
/// and throw here; they reach the audio path already expanded into shelves, and
/// anything asking for coefficients has to expand them the same way first.
/// @throws SonareException on a non-positive sample rate, a frequency outside
///         (0 Hz, Nyquist), or a band type with no single-section design.
BiquadCoefficients design_eq_biquad(const EqBand& band, double sample_rate);

/// @brief @p band with its frequency lowered to the highest one @p sample_rate carries.
/// @details For a band whose frequency was chosen without this rate in view (stored
///          before a re-prepare, or held by an owner at a fixed value): it is designed
///          at the design domain's ceiling rather than refused. Lowers only.
EqBand band_at_rate(EqBand band, double sample_rate) noexcept;

class ParametricEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = 24;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void prepare_channels(int num_channels);

  /// @brief Installs @p band, designed for the current rate.
  /// @details Before the first prepare() the rate is not known yet, so a band
  ///          above that rate's Nyquist is refused by prepare() instead; once
  ///          accepted at a rate, a later prepare() at a lower one designs it at
  ///          that rate's ceiling.
  /// @throws SonareException for a band design_eq_biquad refuses at the current
  ///         rate; nothing changes then.
  void set_band(size_t index, const EqBand& band);
  /// @brief As set_band, but a frequency the current rate cannot carry is designed
  ///        through band_at_rate instead of refused. The band is stored as given, so
  ///        a later prepare at a higher rate designs the frequency it asked for.
  void set_band_at_rate(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();

  // Automatable parameters (RT-safe: recomputes the affected band's biquad
  // coefficients in place, preserves filter state). Bands are laid out in
  // blocks of 3, so band `b` occupies ids `3*b .. 3*b+2`:
  //   3*b + 0 = frequency_hz (clamped to (0 Hz, rt::max_design_frequency_hz))
  //   3*b + 1 = gain_db
  //   3*b + 2 = Q (clamped to > 0)
  // Only bands that are currently enabled produce audible coefficient changes;
  // band type and coefficient mode are not automatable. Ids for b >= kMaxBands
  // are rejected (return false).
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: per band `b` (0 .. kMaxBands-1), id 3*b+0 = "band<b>.frequencyHz",
  // 3*b+1 = "band<b>.gainDb", 3*b+2 = "band<b>.q" (keys match the construction-time band prefix).
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  const EqBand& band(size_t index) const;
  double sample_rate() const { return sample_rate_; }

 private:
  using Coefficients = BiquadCoefficients;

  struct State {
    float z1 = 0.0f;
    float z2 = 0.0f;
  };

  void install_band(size_t index, const EqBand& band, const EqBand& designed);
  static void validate_band_index(size_t index);
  void ensure_prepared() const;

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int num_channels_ = 0;
  bool prepared_ = false;
  std::array<EqBand, kMaxBands> bands_{};
  // Whether prepare() may lower a band's frequency to the new rate's ceiling: set
  // once the band has been accepted at a known rate, or by set_band_at_rate.
  std::array<bool, kMaxBands> resolvable_{};
  std::array<Coefficients, kMaxBands> coefficients_{};
  std::array<std::vector<State>, kMaxBands> states_{};
};

}  // namespace sonare::mastering::eq
