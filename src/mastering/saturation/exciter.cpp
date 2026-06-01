#include "mastering/saturation/exciter.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mastering/common/scoped_no_denormals.h"
#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

namespace {
using sonare::constants::kPiD;
}

float Exciter::Biquad::process(float x) {
  const float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void Exciter::Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

Exciter::Exciter(ExciterConfig config) : config_(config) { validate_config(config_); }

void Exciter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  prepared_ = true;
  update_coeff();
  reset();
}

void Exciter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Exciter");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);
  const float drive = db_to_linear(config_.drive_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    auto& bandpass = bandpass_[static_cast<size_t>(ch)];
    auto& allpass = allpass_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float band = bandpass.process(channels[ch][i]);
      const float aligned = allpass.process(band);
      const float even = (band * band) * (band < 0.0f ? -1.0f : 1.0f);
      const float odd = std::tanh(band * drive);
      const float harmonic =
          (1.0f - config_.even_odd_mix) * even * drive + config_.even_odd_mix * odd;
      channels[ch][i] += aligned * 0.05f * config_.amount + harmonic * config_.amount;
    }
  }
}

void Exciter::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& filter : allpass_) filter.reset();
}

void Exciter::set_config(const ExciterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    update_coeff();
  }
}

void Exciter::validate_config(const ExciterConfig& config) {
  if (!(config.frequency_hz > 0.0f) || config.amount < 0.0f || !(config.q > 0.0f) ||
      config.even_odd_mix < 0.0f || config.even_odd_mix > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid exciter configuration");
  }
}

void Exciter::compute_coeffs() {
  const float cutoff =
      std::clamp(config_.frequency_hz, 10.0f, static_cast<float>(sample_rate_ * 0.49));
  const float w0 = static_cast<float>(2.0 * kPiD * cutoff / sample_rate_);
  const auto coeffs = rt::rbj_bandpass(w0, config_.q);
  bandpass_coeffs_.b0 = coeffs.b0;
  bandpass_coeffs_.b1 = coeffs.b1;
  bandpass_coeffs_.b2 = coeffs.b2;
  bandpass_coeffs_.a1 = coeffs.a1;
  bandpass_coeffs_.a2 = coeffs.a2;
  allpass_coeffs_.b0 = coeffs.a2;
  allpass_coeffs_.b1 = coeffs.a1;
  allpass_coeffs_.b2 = 1.0f;
  allpass_coeffs_.a1 = coeffs.a1;
  allpass_coeffs_.a2 = coeffs.a2;
}

void Exciter::update_coeff() {
  // Rebuild the prototype coefficients and reset every live filter (clears the
  // delay state). Used by prepare()/set_config(), where resetting is intended.
  compute_coeffs();
  for (auto& filter : bandpass_) filter = bandpass_coeffs_;
  for (auto& filter : allpass_) filter = allpass_coeffs_;
}

bool Exciter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.frequency_hz = std::max(value, std::numeric_limits<float>::min());
      if (prepared_) update_coeff_preserving_state();
      return true;
    case 1:
      config_.drive_db = value;
      return true;
    case 2:
      config_.amount = std::max(0.0f, value);
      return true;
    case 3:
      config_.q = std::max(value, std::numeric_limits<float>::min());
      if (prepared_) update_coeff_preserving_state();
      return true;
    case 4:
      config_.even_odd_mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void Exciter::update_coeff_preserving_state() {
  // RT-safe automation path: recompute the prototype coefficients, then copy only
  // the coefficient fields into each live filter, leaving the delay state (z1/z2)
  // untouched. No allocation, so this is safe to call from the audio thread.
  compute_coeffs();
  for (auto& filter : bandpass_) {
    filter.b0 = bandpass_coeffs_.b0;
    filter.b1 = bandpass_coeffs_.b1;
    filter.b2 = bandpass_coeffs_.b2;
    filter.a1 = bandpass_coeffs_.a1;
    filter.a2 = bandpass_coeffs_.a2;
  }
  for (auto& filter : allpass_) {
    filter.b0 = allpass_coeffs_.b0;
    filter.b1 = allpass_coeffs_.b1;
    filter.b2 = allpass_coeffs_.b2;
    filter.a1 = allpass_coeffs_.a1;
    filter.a2 = allpass_coeffs_.a2;
  }
}

void Exciter::ensure_state(int num_channels) {
  if (bandpass_.size() != static_cast<size_t>(num_channels)) {
    bandpass_.assign(static_cast<size_t>(num_channels), bandpass_coeffs_);
    allpass_.assign(static_cast<size_t>(num_channels), allpass_coeffs_);
  }
}

}  // namespace sonare::mastering::saturation
