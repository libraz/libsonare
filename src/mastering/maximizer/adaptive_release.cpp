#include "mastering/maximizer/adaptive_release.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::maximizer {
namespace {

// Smallest strictly-positive crest window / crest factor accepted by
// validate_config; reused to clamp the matching automation parameters and to
// floor the crest_high - crest_low span, which set_parameter keeps at least
// this wide.
constexpr float kMinPositiveCrest = 1.0e-4f;

// Smoothed RMS below which the crest factor is reported as zero rather than
// dividing by (near) silence.
constexpr float kRmsFloor = 1.0e-9f;

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
  control_phase_ = 0;
  last_gain_reduction_db_ = 0.0f;
  update_envelope_coefficients();
  // The two-argument prepare() below leaves the inner limiter at the realtime
  // channel capacity, so the chunk pointer array is sized to match it once here
  // rather than on the audio thread.
  chunk_channels_.assign(dynamics::kRealtimePreparedChannels, nullptr);
  configure_limiter();
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void AdaptiveRelease::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "AdaptiveRelease");
  if (!validate_block_size(num_channels, num_samples)) {
    limiter_.process(channels, num_channels, num_samples);
    return;
  }
  validate_channel_buffers(channels, num_channels);
  if (num_samples > max_block_size_ || num_channels > static_cast<int>(chunk_channels_.size())) {
    // A block the inner limiter was never prepared for must still be rejected:
    // handing it over whole raises the established error, whereas splitting it
    // into control chunks would let it through as a series of small ones.
    limiter_.process(channels, num_channels, num_samples);
    return;
  }

  last_gain_reduction_db_ = 0.0f;
  int offset = 0;
  while (offset < num_samples) {
    if (control_phase_ == 0) {
      // Release automation runs on the audio thread, so use the allocation-free
      // in-place setter (set_release_ms() would publish a new config snapshot,
      // allocating a shared_ptr each time). Same coefficient math, no malloc.
      // The grid this fires on is anchored to the stream position, not to the
      // caller's block boundaries.
      limiter_.set_release_ms_in_place(current_release_ms_);
    }
    const int count = std::min(num_samples - offset, kControlIntervalSamples - control_phase_);
    // The envelopes read the input, so they must run before the limiter
    // overwrites the buffer in place.
    advance_envelopes(channels, num_channels, offset, count);
    for (int ch = 0; ch < num_channels; ++ch) {
      chunk_channels_[static_cast<std::size_t>(ch)] = channels[ch] + offset;
    }
    limiter_.process(chunk_channels_.data(), num_channels, count);
    last_gain_reduction_db_ = std::min(last_gain_reduction_db_, limiter_.last_gain_reduction_db());
    control_phase_ = (control_phase_ + count) % kControlIntervalSamples;
    offset += count;
  }
}

void AdaptiveRelease::reset() {
  limiter_.reset();
  current_release_ms_ = config_.min_release_ms;
  current_crest_factor_ = 0.0f;
  peak_envelope_ = 0.0f;
  rms_square_envelope_ = 0.0f;
  control_phase_ = 0;
  last_gain_reduction_db_ = 0.0f;
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
      // Read directly by the per-sample release mapping; no recompute needed.
      config_.min_release_ms = std::max(0.0f, value);
      config_.max_release_ms = std::max(config_.max_release_ms, config_.min_release_ms);
      return true;
    case 2:
      config_.max_release_ms = std::max(config_.min_release_ms, value);
      return true;
    case 3:
      config_.crest_window_ms = std::max(kMinPositiveCrest, value);
      update_envelope_coefficients();
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
      update_envelope_coefficients();
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> AdaptiveRelease::parameter_descriptors() const {
  return {{"ceilingDb", 0}, {"minReleaseMs", 1}, {"maxReleaseMs", 2},      {"crestWindowMs", 3},
          {"crestLow", 4},  {"crestHigh", 5},    {"releaseSmoothingMs", 6}};
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

void AdaptiveRelease::update_envelope_coefficients() noexcept {
  // Both envelopes advance once per input sample, so their coefficients are the
  // per-sample leaky-integrator rates for the configured milliseconds. Deriving
  // them from num_samples instead — as a per-block update must — collapses the
  // crest window onto the block for any block longer than a few window lengths
  // (exp() underflows to a rate of exactly 1) and silently disables both knobs
  // on an offline render.
  crest_coeff_ = time_to_attack_release_rate_f(sample_rate_, config_.crest_window_ms);
  release_smoothing_coeff_ =
      time_to_attack_release_rate_f(sample_rate_, config_.release_smoothing_ms);
}

void AdaptiveRelease::advance_envelopes(float* const* channels, int num_channels, int offset,
                                        int count) noexcept {
  const float inv_channels = 1.0f / static_cast<float>(num_channels);
  // Map crest factor onto [0, 1] then onto [max_release, min_release]:
  // high crest (transient) -> short release, low crest (sustained) -> long release.
  const float crest_span = std::max(kMinPositiveCrest, config_.crest_high - config_.crest_low);
  const float release_span = config_.max_release_ms - config_.min_release_ms;
  for (int i = 0; i < count; ++i) {
    // Channel-linked detector: the peak is the widest channel at this sample,
    // the mean square is averaged across them, matching the linked gain the
    // inner limiter applies.
    float linked_peak = 0.0f;
    float mean_square = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      const float s = channels[ch][offset + i];
      linked_peak = std::max(linked_peak, std::abs(s));
      mean_square += s * s;
    }
    mean_square *= inv_channels;

    peak_envelope_ = std::max(linked_peak, peak_envelope_ * (1.0f - crest_coeff_));
    rms_square_envelope_ += crest_coeff_ * (mean_square - rms_square_envelope_);

    const float running_rms = std::sqrt(std::max(rms_square_envelope_, 0.0f));
    current_crest_factor_ = running_rms < kRmsFloor ? 0.0f : peak_envelope_ / running_rms;
    const float norm =
        std::clamp((current_crest_factor_ - config_.crest_low) / crest_span, 0.0f, 1.0f);
    const float target_release_ms = config_.min_release_ms + release_span * (1.0f - norm);
    current_release_ms_ += release_smoothing_coeff_ * (target_release_ms - current_release_ms_);
  }
}

}  // namespace sonare::mastering::maximizer
