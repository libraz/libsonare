#include "mastering/maximizer/adaptive_release.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::maximizer {
namespace {

// Smallest strictly-positive crest window / crest factor accepted by
// validate_config; reused to clamp the matching automation parameters.
constexpr float kMinPositiveCrest = 1.0e-4f;

}  // namespace

AdaptiveRelease::AdaptiveRelease(AdaptiveReleaseConfig config) : config_(config) {
  validate_config(config_);
}

void AdaptiveRelease::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  current_release_ms_ = config_.min_release_ms;
  current_crest_factor_ = 0.0f;
  peak_envelope_ = 0.0f;
  rms_square_envelope_ = 0.0f;
  configure_limiter();
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void AdaptiveRelease::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "AdaptiveRelease");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    limiter_.process(channels, num_channels, num_samples);
    return;
  }
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");

  current_crest_factor_ = compute_crest_factor(channels, num_channels, num_samples);

  // Map crest factor onto [0, 1] then onto [max_release, min_release]:
  // high crest (transient) -> short release, low crest (sustained) -> long release.
  const float crest_span = std::max(0.0001f, config_.crest_high - config_.crest_low);
  const float norm =
      std::clamp((current_crest_factor_ - config_.crest_low) / crest_span, 0.0f, 1.0f);
  const float target_release_ms =
      config_.min_release_ms + (config_.max_release_ms - config_.min_release_ms) * (1.0f - norm);
  const float smoothing_seconds = config_.release_smoothing_ms * 0.001f;
  const float smoothing =
      smoothing_seconds <= 0.0f
          ? 1.0f
          : 1.0f - std::exp(-static_cast<float>(num_samples) /
                            static_cast<float>(sample_rate_ * smoothing_seconds));
  current_release_ms_ += smoothing * (target_release_ms - current_release_ms_);

  // Per-block release automation runs on the audio thread, so use the
  // allocation-free in-place setter (set_release_ms() would publish a new
  // config snapshot, allocating a shared_ptr every block). Same coefficient
  // math, no malloc.
  limiter_.set_release_ms_in_place(current_release_ms_);
  limiter_.process(channels, num_channels, num_samples);
}

void AdaptiveRelease::reset() {
  limiter_.reset();
  current_release_ms_ = config_.min_release_ms;
  current_crest_factor_ = 0.0f;
  peak_envelope_ = 0.0f;
  rms_square_envelope_ = 0.0f;
}

void AdaptiveRelease::set_config(const AdaptiveReleaseConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

bool AdaptiveRelease::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.ceiling_db = std::min(0.0f, value);
      // Forward the ceiling to the inner true-peak limiter in place (param 0);
      // does not disturb the crest-factor / release smoothing state held here.
      if (prepared_) limiter_.set_parameter(0, config_.ceiling_db);
      return true;
    case 1:
      // Read directly by the per-block release mapping; no recompute needed.
      config_.min_release_ms = std::max(0.0f, value);
      config_.max_release_ms = std::max(config_.max_release_ms, config_.min_release_ms);
      return true;
    case 2:
      config_.max_release_ms = std::max(config_.min_release_ms, value);
      return true;
    case 3:
      config_.crest_window_ms = std::max(kMinPositiveCrest, value);
      return true;
    case 4:
      config_.crest_low = std::max(kMinPositiveCrest, value);
      config_.crest_high = std::max(config_.crest_high, config_.crest_low + kMinPositiveCrest);
      return true;
    case 5:
      config_.crest_high = std::max(config_.crest_low + kMinPositiveCrest, value);
      return true;
    case 6:
      config_.release_smoothing_ms = std::max(0.0f, value);
      return true;
    default:
      return false;
  }
}

void AdaptiveRelease::validate_config(const AdaptiveReleaseConfig& config) {
  if (config.lookahead_ms < 0.0f || config.min_release_ms < 0.0f ||
      config.max_release_ms < config.min_release_ms || config.crest_window_ms <= 0.0f ||
      config.crest_low <= 0.0f || config.crest_high <= config.crest_low ||
      config.release_smoothing_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid adaptive release configuration");
  }
}

void AdaptiveRelease::configure_limiter() {
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, current_release_ms_, 4});
}

float AdaptiveRelease::compute_crest_factor(float* const* channels, int num_channels,
                                            int num_samples) {
  float peak = 0.0f;
  double sum_sq = 0.0;
  long count = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) continue;
    for (int i = 0; i < num_samples; ++i) {
      const float s = channels[ch][i];
      peak = std::max(peak, std::abs(s));
      sum_sq += static_cast<double>(s) * static_cast<double>(s);
      ++count;
    }
  }
  if (count == 0) return 0.0f;
  const float block_rms_square = static_cast<float>(sum_sq / static_cast<double>(count));

  const float window_seconds = config_.crest_window_ms * 0.001f;
  const float alpha =
      1.0f - std::exp(-static_cast<float>(num_samples) /
                      static_cast<float>(std::max(1.0, sample_rate_ * window_seconds)));
  peak_envelope_ = std::max(peak, peak_envelope_ * (1.0f - alpha));
  rms_square_envelope_ += alpha * (block_rms_square - rms_square_envelope_);

  const float block_rms = std::sqrt(std::max(block_rms_square, 0.0f));
  const float running_rms = std::sqrt(std::max(rms_square_envelope_, 0.0f));
  const float block_crest = block_rms < 1.0e-9f ? 0.0f : peak / block_rms;
  const float running_crest = running_rms < 1.0e-9f ? 0.0f : peak_envelope_ / running_rms;
  return std::max(block_crest, running_crest);
}

}  // namespace sonare::mastering::maximizer
