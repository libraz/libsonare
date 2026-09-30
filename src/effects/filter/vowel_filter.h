#pragma once

/// @file vowel_filter.h
/// @brief Vowel (formant) filter: three parallel band-passes beside a direct path.
///
/// The bank is three constant-peak TPT state-variable band-passes with signed
/// weights, summed with a weighted direct path; an optional antialiased tanh
/// drive sits in front of it. The continuous vowel position selects a
/// (frequency, q, weight) triple per band by interpolating between adjacent
/// vowels in log frequency; the bank glides to that target with a one-pole
/// (accel) rather than passing through the vowels in between.

#include <array>
#include <vector>

#include "effects/modulation/svf_bandpass.h"
#include "rt/adaa.h"
#include "rt/nonlinearities.h"
#include "rt/processor_base.h"

namespace sonare::effects::filter {

/// Number of formant bands in the bank.
inline constexpr int kVowelBandCount = 3;
/// Number of vowels; the position runs 0 .. kVowelCount - 1 in the order a, i, u, e, o.
inline constexpr int kVowelCount = 5;

/// Centre frequency (Hz) of @p band (0..2) of @p vowel (0..4) in the shipped table.
float vowel_table_hz(int vowel, int band) noexcept;

struct VowelFilterConfig {
  float vowel = 0.0f;  ///< position 0..4 (a, i, u, e, o); fractions interpolate in log frequency.
  float accel_ms = 50.0f;  ///< time constant of the glide to the target vowel (0 = jump).
  float drive = 0.5f;      ///< 0..1 pre-gain of the saturator ahead of the bank.
  bool drive_on = false;   ///< puts the saturator in the path.
  float dry_wet = 1.0f;    ///< the filter is an insert, so wet by default.
};

/// The formant bank. Stereo-pair processor: planes beyond the pair pass through.
class VowelFilter : public rt::ProcessorBase {
 public:
  explicit VowelFilter(VowelFilterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = vowel (clamped to [0, 4])
  //   1 = accel_ms (>= 0; rebuilds the glide coefficient)
  //   2 = drive (clamped to [0, 1])
  //   3 = drive_on (0 or 1)
  //   4 = dry_wet (clamped to [0, 1])
  bool set_parameter_impl(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static constexpr int kPlanes = 2;

  /// Band state that glides: log frequency, log q and the (signed) weight.
  struct Triple {
    float log_hz = 0.0f;
    float log_q = 0.0f;
    float weight = 0.0f;
  };

  std::array<Triple, kVowelBandCount> target_for(float vowel) const noexcept;
  void update_bank();
  void update_glide_coefficient();

  VowelFilterConfig config_{};
  double sample_rate_ = 48000.0;
  float glide_ = 1.0f;       ///< one-pole coefficient per sub-block.
  float drive_gain_ = 1.0f;  ///< linear pre-gain, from config_.drive.
  bool snap_ = true;         ///< next bank update lands on the target with no glide.
  int countdown_ = 0;        ///< samples until the next bank update.
  std::array<Triple, kVowelBandCount> current_{};
  std::array<float, kVowelBandCount> band_gain_{};
  float direct_gain_ = 0.0f;
  std::array<std::array<modulation::SvfBandpass, kVowelBandCount>, kPlanes> bands_;
  std::array<rt::Adaa1<rt::TanhNonlinearity>, kPlanes> adaa_;
  std::array<bool, kPlanes> adaa_primed_{};
};

}  // namespace sonare::effects::filter
