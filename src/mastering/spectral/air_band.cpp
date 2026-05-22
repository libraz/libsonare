#include "mastering/spectral/air_band.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::spectral {
namespace {

using sonare::constants::kPiD;

AirBand::Biquad make_highpass(double frequency_hz, double sample_rate, double q) {
  const double w0 = 2.0 * kPiD * std::clamp(frequency_hz, 20.0, sample_rate * 0.49) / sample_rate;
  const double c = std::cos(w0);
  const double s = std::sin(w0);
  const double alpha = s / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double inv = 1.0 / a0;
  AirBand::Biquad b;
  b.b0 = static_cast<float>((1.0 + c) * 0.5 * inv);
  b.b1 = static_cast<float>(-(1.0 + c) * inv);
  b.b2 = static_cast<float>((1.0 + c) * 0.5 * inv);
  b.a1 = static_cast<float>(-2.0 * c * inv);
  b.a2 = static_cast<float>((1.0 - alpha) * inv);
  return b;
}

AirBand::Biquad make_high_shelf(double frequency_hz, double sample_rate, float gain_db) {
  const double a = std::sqrt(db_to_linear(gain_db));
  const double w0 = 2.0 * kPiD * std::clamp(frequency_hz, 20.0, sample_rate * 0.49) / sample_rate;
  const double c = std::cos(w0);
  const double s = std::sin(w0);
  const double alpha = s * std::sqrt(0.5);  // shelf slope S=1
  const double beta = 2.0 * std::sqrt(a) * alpha;
  const double a0 = (a + 1.0) - (a - 1.0) * c + beta;
  const double inv = 1.0 / a0;
  AirBand::Biquad b;
  b.b0 = static_cast<float>(a * ((a + 1.0) + (a - 1.0) * c + beta) * inv);
  b.b1 = static_cast<float>(-2.0 * a * ((a - 1.0) + (a + 1.0) * c) * inv);
  b.b2 = static_cast<float>(a * ((a + 1.0) + (a - 1.0) * c - beta) * inv);
  b.a1 = static_cast<float>(2.0 * ((a - 1.0) - (a + 1.0) * c) * inv);
  b.a2 = static_cast<float>(((a + 1.0) - (a - 1.0) * c - beta) * inv);
  return b;
}

}  // namespace

AirBand::AirBand(AirBandConfig config) : config_(config) { validate_config(config_); }

void AirBand::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  rebuild_filters(static_cast<int>(previous_.size()));
}

void AirBand::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("AirBand must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    float previous = previous_[static_cast<size_t>(ch)];
    float envelope = envelope_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float high = channels[ch][i] - previous;
      previous = channels[ch][i];
      const float detected = std::abs(detector_[static_cast<size_t>(ch)].process(channels[ch][i]));
      envelope = 0.995f * envelope + 0.005f * detected;
      const float over_db = std::max(0.0f, linear_to_db(envelope) - config_.dynamic_threshold_db);
      const float dynamic_gain_db =
          std::min(config_.dynamic_range_db, over_db * 0.25f) * config_.amount;
      shelf_[static_cast<size_t>(ch)] =
          make_high_shelf(config_.shelf_frequency_hz, sample_rate_, dynamic_gain_db);
      const float shelved = shelf_[static_cast<size_t>(ch)].process(channels[ch][i]);
      const float harmonic = std::tanh(high * 4.0f) * config_.amount;
      channels[ch][i] = std::clamp(shelved + harmonic, -1.5f, 1.5f);
    }
    previous_[static_cast<size_t>(ch)] = previous;
    envelope_[static_cast<size_t>(ch)] = envelope;
  }
}

void AirBand::reset() {
  std::fill(previous_.begin(), previous_.end(), 0.0f);
  std::fill(envelope_.begin(), envelope_.end(), 0.0f);
  for (auto& f : shelf_) f.reset();
  for (auto& f : detector_) f.reset();
}

void AirBand::set_config(const AirBandConfig& config) {
  validate_config(config);
  config_ = config;
}

void AirBand::validate_config(const AirBandConfig& config) {
  if (!(config.amount >= 0.0f && config.amount <= 1.0f) || !(config.shelf_frequency_hz > 0.0f) ||
      config.dynamic_range_db < 0.0f) {
    throw std::invalid_argument("invalid air band configuration");
  }
}

void AirBand::ensure_state(int num_channels) {
  if (previous_.size() != static_cast<size_t>(num_channels)) {
    previous_.assign(static_cast<size_t>(num_channels), 0.0f);
    envelope_.assign(static_cast<size_t>(num_channels), 0.0f);
    rebuild_filters(num_channels);
  }
}

void AirBand::rebuild_filters(int num_channels) {
  if (num_channels <= 0) return;
  shelf_.assign(static_cast<size_t>(num_channels),
                make_high_shelf(config_.shelf_frequency_hz, sample_rate_, 0.0f));
  detector_.assign(
      static_cast<size_t>(num_channels),
      make_highpass(config_.shelf_frequency_hz, sample_rate_, sonare::constants::kButterworthQD));
}

float AirBand::Biquad::process(float x) {
  const float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void AirBand::Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

}  // namespace sonare::mastering::spectral
