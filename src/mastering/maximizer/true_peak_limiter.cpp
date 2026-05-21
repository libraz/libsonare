#include "mastering/maximizer/true_peak_limiter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "analysis/meter/true_peak.h"

namespace sonare::mastering::maximizer {

TruePeakLimiter::TruePeakLimiter(TruePeakLimiterConfig config) : config_(config) {
  validate_config(config_);
}

void TruePeakLimiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
  reset();
}

void TruePeakLimiter::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("TruePeakLimiter must be prepared before processing");
  limiter_.process(channels, num_channels, num_samples);
  const float ceiling = db_to_linear(config_.ceiling_db);
  float peak = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    peak = std::max(peak, analysis::meter::true_peak(channels[ch], static_cast<size_t>(num_samples),
                                                     config_.oversample_factor));
  }
  if (peak > ceiling && peak > 0.0f) {
    const float gain = ceiling / peak;
    for (int ch = 0; ch < num_channels; ++ch)
      for (int i = 0; i < num_samples; ++i) channels[ch][i] *= gain;
    last_gain_reduction_db_ = std::min(limiter_.last_gain_reduction_db(), linear_to_db(gain));
  } else {
    last_gain_reduction_db_ = limiter_.last_gain_reduction_db();
  }
}

void TruePeakLimiter::reset() {
  limiter_.reset();
  last_gain_reduction_db_ = 0.0f;
}

void TruePeakLimiter::set_config(const TruePeakLimiterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

void TruePeakLimiter::set_release_ms(float release_ms) {
  if (release_ms < 0.0f) {
    throw std::invalid_argument("true peak limiter release must be non-negative");
  }
  config_.release_ms = release_ms;
  limiter_.set_release_ms(release_ms);
}

void TruePeakLimiter::validate_config(const TruePeakLimiterConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f || config.oversample_factor < 1) {
    throw std::invalid_argument("invalid true peak limiter configuration");
  }
}

float TruePeakLimiter::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float TruePeakLimiter::linear_to_db(float value) {
  return value <= 0.0f ? -120.0f : 20.0f * std::log10(value);
}

}  // namespace sonare::mastering::maximizer
