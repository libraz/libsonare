#include "mastering/maximizer/maximizer.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::maximizer {

Maximizer::Maximizer(MaximizerConfig config) : config_(config) { validate_config(config_); }

void Maximizer::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void Maximizer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Maximizer");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  const float gain = db_to_linear(config_.input_gain_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) channels[ch][i] *= gain;
  }
  limiter_.process(channels, num_channels, num_samples);
}

void Maximizer::reset() { limiter_.reset(); }

void Maximizer::set_config(const MaximizerConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

bool Maximizer::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Applied per block as a linear gain; no coefficients to recompute.
      config_.input_gain_db = value;
      return true;
    case 1:
      config_.ceiling_db = std::min(0.0f, value);
      // The inner limiter ceiling lives in its config; the only public path is
      // set_config, which re-prepares (clears lookahead/gain state). Unavoidable.
      if (prepared_) {
        limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
      }
      return true;
    case 2:
      config_.release_ms = std::max(0.0f, value);
      // RT-safe in-place release coefficient update: recomputes the inner
      // limiter's release coefficient without publishing a snapshot (no
      // allocation), preserving the limiter audio state. Matches the
      // parameter_is_realtime_safe(2) == true contract.
      if (prepared_) {
        limiter_.set_release_ms_in_place(config_.release_ms);
      }
      return true;
    default:
      return false;
  }
}

bool Maximizer::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id != 1u;
}

void Maximizer::validate_config(const MaximizerConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "maximizer timing values must be non-negative");
  }
}

}  // namespace sonare::mastering::maximizer
