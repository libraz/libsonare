#pragma once

/// @file wah.h
/// @brief LFO-swept resonant-bandpass wah.

#include <array>
#include <cmath>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/svf_bandpass.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

/// Which tap of the sweeping state-variable filter is heard.
enum class WahFilterType {
  kBandpass,  ///< resonant bandpass (the classic wah).
  kLowpass,   ///< resonant lowpass.
};
inline constexpr int kWahFilterTypeCount = 2;

/// How the sweep position (0..1) maps to the centre frequency between the corners.
enum class WahSweepLaw {
  kLinearHz,      ///< frequency is linear in the position.
  kLinearOctave,  ///< pitch is linear in the position: an exponential in hertz.
};
inline constexpr int kWahSweepLawCount = 2;

/// Centre frequency for a sweep @p position in [0, 1] between @p lo and @p hi.
inline float wah_sweep_hz(WahSweepLaw law, float lo, float hi, float position) noexcept {
  if (law == WahSweepLaw::kLinearOctave) return lo * std::pow(hi / lo, position);
  return lo + (hi - lo) * position;
}

struct WahConfig {
  float rate_hz = 1.5f;    ///< sweep LFO rate (0 = a fixed mid-sweep wah).
  float min_hz = 400.0f;   ///< sweep lower bound.
  float max_hz = 2000.0f;  ///< sweep upper bound.
  float resonance = 4.0f;  ///< bandpass Q (the wah "peakiness").
  float dry_wet = 1.0f;    ///< the wah is an insert, so wet by default.
  WahFilterType filter_type = WahFilterType::kBandpass;
  WahSweepLaw sweep_law = WahSweepLaw::kLinearHz;
};

/// A resonant bandpass whose centre frequency is swept between min_hz and max_hz
/// by an LFO — the classic auto-wah / pedal-wah timbre. The two channels share
/// the sweep phase so the stereo image stays coherent.
class Wah : public rt::ProcessorBase {
 public:
  explicit Wah(WahConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int tail_samples() const noexcept override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = rate_hz (updates the LFO in place)
  //   1 = min_hz
  //   2 = max_hz
  //   3 = resonance
  //   4 = dry_wet
  //   5 = filter_type (WahFilterType; a fractional or unnamed value is refused)
  //   6 = sweep_law (WahSweepLaw; same refusal)
  bool set_parameter_impl(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  WahConfig config_{};
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  Lfo lfo_;
  std::array<SvfBandpass, 2> filters_;
};

}  // namespace sonare::effects::modulation
