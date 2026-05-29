#include "editing/voice_changer/streaming_formant.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::editing::voice_changer {
namespace {

float clamp_factor(float value) noexcept { return std::clamp(value, 0.55f, 1.65f); }

}  // namespace

StreamingFormant::StreamingFormant(StreamingFormantConfig config) : config_(config) {}

void StreamingFormant::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  factor_alpha_ = rt::one_pole_lowpass_alpha_matched(60.0f, sample_rate_);
  smoothed_factor_ = clamp_factor(config_.factor);
  filter_factor_ = smoothed_factor_;
  update_filters();
  reset();
}

void StreamingFormant::reset() {
  for (auto& filter : filters_) filter.reset();
  smoothed_factor_ = clamp_factor(config_.factor);
  filter_factor_ = smoothed_factor_;
  filter_update_countdown_ = 0;
  update_filters();
}

void StreamingFormant::set_config(const StreamingFormantConfig& config) {
  config_ = config;
  update_filters();
}

void StreamingFormant::update_filters() noexcept {
  // No-op until prepare() has supplied a real sample rate. update_filters()
  // could otherwise compute biquad coefficients against a stale or
  // uninitialized rate and emit garbage on the first block.
  if (!(sample_rate_ > 0.0)) return;
  const float factor = clamp_factor(filter_factor_);
  const float amount = std::clamp(config_.amount, 0.0f, 1.0f);
  const float shift = (factor - 1.0f) * amount;
  const float body = std::clamp(config_.body, -1.0f, 1.0f);
  const float brightness = std::clamp(config_.brightness, -1.0f, 1.0f);
  const float nasal = std::clamp(config_.nasal, -1.0f, 1.0f);

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
  const float wet = std::clamp(config_.amount, 0.0f, 1.0f);
  const float target_factor = clamp_factor(config_.factor);
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
    smoothed_factor_ += factor_alpha_ * (target_factor - smoothed_factor_);
    if (--filter_update_countdown_ <= 0) {
      if (std::abs(smoothed_factor_ - filter_factor_) > kRebuildEpsilon) {
        filter_factor_ = smoothed_factor_;
        update_filters();
      }
      filter_update_countdown_ = kFilterUpdateInterval;
    }
    float y = input[i];
    for (auto& filter : filters_) y = filter.process(y);
    output[i] = input[i] * (1.0f - wet) + y * wet;
  }
  // Settle: rebuild once more if the tail of the ramp diverged from filter_factor_.
  if (std::abs(smoothed_factor_ - filter_factor_) > kRebuildEpsilon) {
    filter_factor_ = smoothed_factor_;
    update_filters();
  }
}

}  // namespace sonare::editing::voice_changer
