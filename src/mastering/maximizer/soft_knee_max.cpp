#include "mastering/maximizer/soft_knee_max.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::maximizer {

SoftKneeMax::SoftKneeMax(SoftKneeMaxConfig config) : config_(config) { validate_config(config_); }

void SoftKneeMax::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  maximizer_.set_config({0.0f, config_.ceiling_db, 1.0f, config_.release_ms});
  maximizer_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void SoftKneeMax::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "SoftKneeMax");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  const float drive = db_to_linear(config_.input_gain_db);
  const float knee = db_to_linear(config_.ceiling_db - config_.knee_db);
  // Apply the soft-knee shaping as a full pre-stage over the ENTIRE block first,
  // in place, before handing the buffer to the maximizer. This keeps the knee
  // and the maximizer in the same time reference: the maximizer's lookahead
  // delay and its detector both observe the same knee-shaped signal, so the
  // gain envelope and the audio it scales stay aligned (no lookahead_ms skew
  // between the knee shape and the limiting).
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      float x = channels[ch][i] * drive;
      const float ax = std::abs(x);
      if (ax > knee && knee > 0.0f) {
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        x = sign * (knee + std::tanh((ax - knee) / knee) * knee);
      }
      channels[ch][i] = x;
    }
  }
  maximizer_.process(channels, num_channels, num_samples);
}

void SoftKneeMax::reset() { maximizer_.reset(); }

void SoftKneeMax::set_config(const SoftKneeMaxConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

bool SoftKneeMax::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Drive applied per sample in process(); no coefficients to recompute.
      config_.input_gain_db = value;
      return true;
    case 1:
      config_.ceiling_db = std::min(0.0f, value);
      // Routes to Maximizer ceiling (param 1), which re-prepares the inner
      // limiter and clears its lookahead/gain state. Unavoidable reset.
      if (prepared_) {
        maximizer_.set_parameter(1, config_.ceiling_db);
      }
      return true;
    case 2:
      // Soft-knee width applied per sample in process(); no coefficients.
      config_.knee_db = std::max(0.0f, value);
      return true;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      // Routes to Maximizer release (param 2): in-place, preserves audio state.
      if (prepared_) {
        maximizer_.set_parameter(2, config_.release_ms);
      }
      return true;
    default:
      return false;
  }
}

bool SoftKneeMax::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id != 1u;
}

void SoftKneeMax::validate_config(const SoftKneeMaxConfig& config) {
  if (config.knee_db < 0.0f || config.release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid soft knee maximizer configuration");
  }
}

}  // namespace sonare::mastering::maximizer
