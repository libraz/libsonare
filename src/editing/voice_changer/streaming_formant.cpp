#include "editing/voice_changer/streaming_formant.h"

#include <algorithm>
#include <cmath>

#include "editing/voice_changer/formant_bounds.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer {
namespace {

float clamp_factor(float value) noexcept {
  return std::clamp(value, kFormantFactorMin, kFormantFactorMax);
}

float effective_factor(float factor, float amount) noexcept {
  return clamp_factor(1.0f + (clamp_factor(factor) - 1.0f) * std::clamp(amount, 0.0f, 1.0f));
}

float effective_factor(const StreamingFormantConfig& config) noexcept {
  return effective_factor(config.factor, config.amount);
}

}  // namespace

StreamingFormant::StreamingFormant(StreamingFormantConfig config) : config_(config) {}

void StreamingFormant::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  factor_alpha_ = rt::one_pole_lowpass_alpha_matched(60.0f, sample_rate_);
  smoothed_factor_ = effective_factor(config_);
  filter_factor_ = smoothed_factor_;
  factor_smoother_.prepare(sample_rate_, 12.0f);
  amount_smoother_.prepare(sample_rate_, 12.0f);
  body_smoother_.prepare(sample_rate_, 12.0f);
  brightness_smoother_.prepare(sample_rate_, 12.0f);
  nasal_smoother_.prepare(sample_rate_, 12.0f);
  factor_smoother_.set_target(config_.factor);
  amount_smoother_.set_target(config_.amount);
  body_smoother_.set_target(config_.body);
  brightness_smoother_.set_target(config_.brightness);
  nasal_smoother_.set_target(config_.nasal);
  update_filters();
  reset();
}

void StreamingFormant::reset() {
  for (auto& filter : filters_) filter.reset();
  smoothed_factor_ = effective_factor(config_);
  filter_factor_ = smoothed_factor_;
  factor_smoother_.reset(config_.factor);
  amount_smoother_.reset(config_.amount);
  body_smoother_.reset(config_.body);
  brightness_smoother_.reset(config_.brightness);
  nasal_smoother_.reset(config_.nasal);
  filter_body_ = config_.body;
  filter_brightness_ = config_.brightness;
  filter_nasal_ = config_.nasal;
  filter_update_countdown_ = 0;
  update_filters();
}

void StreamingFormant::set_config(const StreamingFormantConfig& config) {
  config_ = config;
  factor_smoother_.set_target(config_.factor);
  amount_smoother_.set_target(config_.amount);
  body_smoother_.set_target(config_.body);
  brightness_smoother_.set_target(config_.brightness);
  nasal_smoother_.set_target(config_.nasal);
  // Before prepare() no rendering state exists, so calculating the initial
  // coefficients immediately is safe. Prepared processors advance the
  // targets sample-by-sample in process_block().
  if (!(sample_rate_ > 0.0)) update_filters();
}

void StreamingFormant::update_filters() noexcept {
  // No-op until prepare() has supplied a real sample rate. update_filters()
  // could otherwise compute biquad coefficients against a stale or
  // uninitialized rate and emit garbage on the first block.
  if (!(sample_rate_ > 0.0)) return;
  // `amount` controls only the formant-frequency displacement. Body,
  // brightness, and nasal are independent tonal controls and must still work
  // at amount=0 (the neutral-monitor preset intentionally relies on this).
  const float factor = clamp_factor(filter_factor_);
  const float shift = factor - 1.0f;
  const float body = std::clamp(filter_body_, -1.0f, 1.0f);
  const float brightness = std::clamp(filter_brightness_, -1.0f, 1.0f);
  const float nasal = std::clamp(filter_nasal_, -1.0f, 1.0f);

  const float body_freq = std::clamp(210.0f / factor, 90.0f, 420.0f);
  const float vowel_freq = std::clamp(900.0f * factor, 420.0f, 1800.0f);
  const float nasal_freq = std::clamp(1350.0f * factor, 700.0f, 2600.0f);

  filters_[0].set(rt::rbj_low_shelf(rt::frequency_to_w0(body_freq, sample_rate_), 0.75f,
                                    body * 3.0f - shift * 4.5f));
  filters_[1].set(rt::rbj_peak(rt::frequency_to_w0(vowel_freq, sample_rate_), 0.9f,
                               shift * 7.0f + body * 1.5f));
  filters_[2].set(rt::rbj_peak(rt::frequency_to_w0(nasal_freq, sample_rate_), 1.6f,
                               nasal * 4.0f + shift * 2.0f));
  filters_[3].set(rt::rbj_high_shelf(rt::frequency_to_w0(5200.0f, sample_rate_), 0.8f,
                                     brightness * 5.0f + shift * 3.0f));
}

void StreamingFormant::process_block(const float* input, float* output, int num_samples) noexcept {
  if (num_samples <= 0 || input == nullptr || output == nullptr) return;
  if (!(sample_rate_ > 0.0)) {
    // prepare() not called yet. Pass-through rather than producing garbage from
    // uninitialized biquad coefficients.
    for (int i = 0; i < num_samples; ++i) output[i] = input[i];
    return;
  }
  // Per-sample smoothing of the formant factor, with periodic filter rebuilds.
  // Updating the biquad coefficients every sample would be CPU-expensive; the
  // 32-sample interval gives ~1.5 ms granularity at 22 kHz, which is below the
  // audible block-rate stepping that the previous once-per-block update produced
  // when the factor changed quickly across consecutive blocks. The countdown is
  // a member (not a local) so it carries across block boundaries — otherwise
  // every block would force a rebuild on sample 0 regardless of factor drift.
  constexpr int kFilterUpdateInterval = 32;
  constexpr float kRebuildEpsilon = 1.0e-4f;
  for (int i = 0; i < num_samples; ++i) {
    const float target_factor =
        effective_factor(factor_smoother_.process(), amount_smoother_.process());
    smoothed_factor_ += factor_alpha_ * (target_factor - smoothed_factor_);
    const float body = body_smoother_.process();
    const float brightness = brightness_smoother_.process();
    const float nasal = nasal_smoother_.process();
    if (--filter_update_countdown_ <= 0) {
      if (std::abs(smoothed_factor_ - filter_factor_) > kRebuildEpsilon ||
          std::abs(body - filter_body_) > kRebuildEpsilon ||
          std::abs(brightness - filter_brightness_) > kRebuildEpsilon ||
          std::abs(nasal - filter_nasal_) > kRebuildEpsilon) {
        filter_factor_ = smoothed_factor_;
        filter_body_ = body;
        filter_brightness_ = brightness;
        filter_nasal_ = nasal;
        update_filters();
      }
      filter_update_countdown_ = kFilterUpdateInterval;
    }
    float y = input[i];
    for (auto& filter : filters_) y = filter.process(y);
    output[i] = y;
  }
  // Settle: rebuild once more if the tail of the ramp diverged from filter_factor_.
  if (std::abs(smoothed_factor_ - filter_factor_) > kRebuildEpsilon ||
      std::abs(body_smoother_.current() - filter_body_) > kRebuildEpsilon ||
      std::abs(brightness_smoother_.current() - filter_brightness_) > kRebuildEpsilon ||
      std::abs(nasal_smoother_.current() - filter_nasal_) > kRebuildEpsilon) {
    filter_factor_ = smoothed_factor_;
    filter_body_ = body_smoother_.current();
    filter_brightness_ = brightness_smoother_.current();
    filter_nasal_ = nasal_smoother_.current();
    update_filters();
  }
}

}  // namespace sonare::editing::voice_changer
