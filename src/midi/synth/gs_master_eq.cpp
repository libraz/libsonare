#include "midi/synth/gs_master_eq.h"

#include <algorithm>

#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kButterworthQ;

/// GAIN centre. `34`-`4C` reads as -12..+12 dB in 1 dB steps around it.
constexpr int kGsEqGainCenter = 0x40;
constexpr float kGsEqGainLimitDb = 12.0f;

}  // namespace

float gs_eq_low_freq_hz(uint8_t value) noexcept { return kGsEqLowFreqHz[value == 1 ? 1 : 0]; }

float gs_eq_high_freq_hz(uint8_t value) noexcept { return kGsEqHighFreqHz[value == 1 ? 1 : 0]; }

float gs_eq_gain_db(uint8_t value) noexcept {
  const float db = static_cast<float>(static_cast<int>(value & 0x7Fu) - kGsEqGainCenter);
  return std::clamp(db, -kGsEqGainLimitDb, kGsEqGainLimitDb);
}

bool gs_master_eq_is_flat(const GsMasterEq& eq) noexcept {
  return gs_eq_gain_db(eq.low_gain) == 0.0f && gs_eq_gain_db(eq.high_gain) == 0.0f;
}

void GsMasterEqFilter::prepare(double sample_rate) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  reset();
  set(GsMasterEq{});
}

void GsMasterEqFilter::set(const GsMasterEq& eq) noexcept {
  const float low_db = gs_eq_gain_db(eq.low_gain);
  const float high_db = gs_eq_gain_db(eq.high_gain);
  low_active_ = low_db != 0.0f;
  high_active_ = high_db != 0.0f;
  if (low_active_) {
    const rt::BiquadCoeffs c = rt::rbj_low_shelf(
        rt::frequency_to_w0(gs_eq_low_freq_hz(eq.low_freq), sample_rate_), kButterworthQ, low_db);
    for (rt::BiquadState& state : low_) state.set(c);
  }
  if (high_active_) {
    const rt::BiquadCoeffs c =
        rt::rbj_high_shelf(rt::frequency_to_w0(gs_eq_high_freq_hz(eq.high_freq), sample_rate_),
                           kButterworthQ, high_db);
    for (rt::BiquadState& state : high_) state.set(c);
  }
}

void GsMasterEqFilter::reset() noexcept {
  for (rt::BiquadState& state : low_) state.reset();
  for (rt::BiquadState& state : high_) state.reset();
}

void GsMasterEqFilter::process(float* left, float* right, int n) noexcept {
  if (n <= 0) return;
  float* channels[2] = {left, right};
  for (int ch = 0; ch < 2; ++ch) {
    float* buf = channels[ch];
    if (buf == nullptr) continue;
    if (low_active_) {
      rt::BiquadState& state = low_[ch];
      for (int i = 0; i < n; ++i) buf[i] = state.process(buf[i]);
    }
    if (high_active_) {
      rt::BiquadState& state = high_[ch];
      for (int i = 0; i < n; ++i) buf[i] = state.process(buf[i]);
    }
  }
}

}  // namespace sonare::midi::synth
