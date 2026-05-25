#include "mastering/maximizer/true_peak_limiter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include "mastering/common/sliding_max.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::maximizer {
namespace {

float sanitize_sample(float sample, float ceiling) {
  if (std::isnan(sample)) return 0.0f;
  if (sample == std::numeric_limits<float>::infinity()) return ceiling;
  if (sample == -std::numeric_limits<float>::infinity()) return -ceiling;
  return sample;
}

}  // namespace

TruePeakLimiter::TruePeakLimiter(TruePeakLimiterConfig config)
    : config_(config), true_peak_filter_(1, config.oversample_factor) {
  validate_config(config_);
}

void TruePeakLimiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  lookahead_samples_ = static_cast<int>(std::round(sample_rate_ * config_.lookahead_ms * 0.001));
  update_time_constants();
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  true_peak_filter_ = common::TruePeakFilter(0, config_.oversample_factor);
  downsampler_.set_factor(config_.oversample_factor);
  oversampled_peak_window_.prepare(
      static_cast<size_t>(std::max(1, lookahead_samples_ + 1) * true_peak_filter_.factor()));
  const size_t max_oversampled_samples = static_cast<size_t>(std::max(0, max_block_size_)) *
                                         static_cast<size_t>(true_peak_filter_.factor());
  input_ptrs_.assign(2, nullptr);
  oversampled_ptrs_.assign(2, nullptr);
  oversampled_buffers_.assign(2, std::vector<float>(max_oversampled_samples, 0.0f));
  limited_oversampled_buffers_.assign(2, std::vector<float>(max_oversampled_samples, 0.0f));
  true_peak_scratch_.assign(2, {});
  const size_t true_peak_history_size =
      static_cast<size_t>(std::max(0, true_peak_filter_.latency_samples() * 2));
  true_peak_history_.assign(2, std::vector<float>(true_peak_history_size, 0.0f));
  for (auto& scratch : true_peak_scratch_) {
    scratch.assign(true_peak_history_size + static_cast<size_t>(std::max(0, max_block_size_)),
                   0.0f);
  }
  linked_abs_.assign(max_oversampled_samples, 0.0f);
  input_rate_gain_.assign(static_cast<size_t>(std::max(0, max_block_size_)), 1.0f);
  downsampled_.assign(static_cast<size_t>(std::max(0, max_block_size_)), 0.0f);
  prepared_ = true;
  reset();
}

void TruePeakLimiter::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("TruePeakLimiter must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
  }

  // All supported oversampling factors (2, 4, 8) use the sample-accurate
  // polyphase brickwall path; validate_config rejects any other value.
  common::ScopedNoDenormals no_denormals;
  process_polyphase(channels, num_channels, num_samples);
}

void TruePeakLimiter::process_polyphase(float* const* channels, int num_channels, int num_samples) {
  prepare_buffers(num_channels);
  if (config_.apply_gain_at_input_rate) {
    process_polyphase_detect_only(channels, num_channels, num_samples);
    return;
  }

  const int factor = true_peak_filter_.factor();
  const size_t oversampled_samples = static_cast<size_t>(num_samples * factor);
  for (int ch = 0; ch < num_channels; ++ch) {
    input_ptrs_[static_cast<size_t>(ch)] = channels[ch];
    std::fill_n(oversampled_buffers_[static_cast<size_t>(ch)].begin(),
                static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
    std::fill_n(limited_oversampled_buffers_[static_cast<size_t>(ch)].begin(),
                static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
    oversampled_ptrs_[static_cast<size_t>(ch)] =
        oversampled_buffers_[static_cast<size_t>(ch)].data();
  }
  true_peak_filter_.upsample_with_history(input_ptrs_.data(), oversampled_ptrs_.data(),
                                          num_channels, num_samples, true_peak_history_,
                                          true_peak_scratch_);

  std::fill_n(linked_abs_.begin(), static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
  for (int ch = 0; ch < num_channels; ++ch) {
    const auto& channel = oversampled_buffers_[static_cast<size_t>(ch)];
    for (size_t i = 0; i < oversampled_samples; ++i) {
      linked_abs_[i] = std::max(linked_abs_[i], std::abs(channel[i]));
    }
  }

  const float ceiling = db_to_linear(config_.ceiling_db);
  float min_gain = 1.0f;
  for (size_t os = 0; os < oversampled_samples; ++os) {
    oversampled_peak_window_.push(linked_abs_[os]);

    const float peak = oversampled_peak_window_.max();
    const float target_gain = peak > ceiling && peak > 0.0f ? ceiling / peak : 1.0f;
    const float release_coeff = adaptive_release_coeff(peak);
    if (target_gain < fast_gain_) {
      fast_gain_ = fast_attack_coeff_ * fast_gain_ + (1.0f - fast_attack_coeff_) * target_gain;
    } else {
      fast_gain_ = release_coeff * fast_gain_ + (1.0f - release_coeff) * target_gain;
    }
    if (target_gain < slow_gain_) {
      slow_gain_ = slow_attack_coeff_ * slow_gain_ + (1.0f - slow_attack_coeff_) * target_gain;
    } else {
      slow_gain_ = release_coeff * slow_gain_ + (1.0f - release_coeff) * target_gain;
    }
    const float gain = std::min(fast_gain_, slow_gain_);
    min_gain = std::min(min_gain, gain);

    for (int ch = 0; ch < num_channels; ++ch) {
      const float delayed = oversampled_lookahead_[static_cast<size_t>(ch)].process(
          sanitize_sample(oversampled_buffers_[static_cast<size_t>(ch)][os], ceiling));
      float output = sanitize_sample(delayed * gain, ceiling);
      const float abs_output = std::abs(output);
      if (abs_output > ceiling && abs_output > 0.0f) {
        const float hard_gain = ceiling / abs_output;
        output *= hard_gain;
        min_gain = std::min(min_gain, gain * hard_gain);
      }
      limited_oversampled_buffers_[static_cast<size_t>(ch)][os] = output;
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    downsampler_.downsample_to(limited_oversampled_buffers_[static_cast<size_t>(ch)].data(),
                               oversampled_samples, downsampled_.data(),
                               static_cast<size_t>(num_samples));
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = downsampled_[static_cast<size_t>(i)];
    }
  }

  last_gain_reduction_db_ = std::min(0.0f, linear_to_db(min_gain));
}

void TruePeakLimiter::process_polyphase_detect_only(float* const* channels, int num_channels,
                                                    int num_samples) {
  const int factor = true_peak_filter_.factor();
  const size_t oversampled_samples = static_cast<size_t>(num_samples * factor);
  for (int ch = 0; ch < num_channels; ++ch) {
    input_ptrs_[static_cast<size_t>(ch)] = channels[ch];
    std::fill_n(oversampled_buffers_[static_cast<size_t>(ch)].begin(),
                static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
    oversampled_ptrs_[static_cast<size_t>(ch)] =
        oversampled_buffers_[static_cast<size_t>(ch)].data();
  }
  true_peak_filter_.upsample_with_history(input_ptrs_.data(), oversampled_ptrs_.data(),
                                          num_channels, num_samples, true_peak_history_,
                                          true_peak_scratch_);

  std::fill_n(linked_abs_.begin(), static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
  for (int ch = 0; ch < num_channels; ++ch) {
    const auto& channel = oversampled_buffers_[static_cast<size_t>(ch)];
    for (size_t i = 0; i < oversampled_samples; ++i) {
      linked_abs_[i] = std::max(linked_abs_[i], std::abs(channel[i]));
    }
  }

  const float ceiling = db_to_linear(config_.ceiling_db);
  float min_gain = 1.0f;
  std::fill_n(input_rate_gain_.begin(), num_samples, 1.0f);
  for (size_t os = 0; os < oversampled_samples; ++os) {
    oversampled_peak_window_.push(linked_abs_[os]);

    const float peak = oversampled_peak_window_.max();
    const float target_gain = peak > ceiling && peak > 0.0f ? ceiling / peak : 1.0f;
    const float release_coeff = adaptive_release_coeff(peak);
    if (target_gain < fast_gain_) {
      fast_gain_ = fast_attack_coeff_ * fast_gain_ + (1.0f - fast_attack_coeff_) * target_gain;
    } else {
      fast_gain_ = release_coeff * fast_gain_ + (1.0f - release_coeff) * target_gain;
    }
    if (target_gain < slow_gain_) {
      slow_gain_ = slow_attack_coeff_ * slow_gain_ + (1.0f - slow_attack_coeff_) * target_gain;
    } else {
      slow_gain_ = release_coeff * slow_gain_ + (1.0f - release_coeff) * target_gain;
    }
    const float gain = std::min(fast_gain_, slow_gain_);
    min_gain = std::min(min_gain, gain);
    input_rate_gain_[os / static_cast<size_t>(factor)] =
        std::min(input_rate_gain_[os / static_cast<size_t>(factor)], gain);
  }

  for (int i = 0; i < num_samples; ++i) {
    const float gain = input_rate_gain_[static_cast<size_t>(i)];
    for (int ch = 0; ch < num_channels; ++ch) {
      const float delayed =
          lookahead_[static_cast<size_t>(ch)].process(sanitize_sample(channels[ch][i], ceiling));
      float output = sanitize_sample(delayed * gain, ceiling);
      const float abs_output = std::abs(output);
      if (abs_output > ceiling && abs_output > 0.0f) {
        const float hard_gain = ceiling / abs_output;
        output *= hard_gain;
        min_gain = std::min(min_gain, gain * hard_gain);
      }
      channels[ch][i] = output;
    }
  }

  last_gain_reduction_db_ = std::min(0.0f, linear_to_db(min_gain));
}

void TruePeakLimiter::reset() {
  limiter_.reset();
  for (auto& buffer : lookahead_) buffer.reset();
  for (auto& buffer : oversampled_lookahead_) buffer.reset();
  fast_gain_ = 1.0f;
  slow_gain_ = 1.0f;
  crest_peak_ = 0.0f;
  crest_rms_ = 0.0f;
  oversampled_peak_window_.reset();
  for (auto& history : true_peak_history_) {
    std::fill(history.begin(), history.end(), 0.0f);
  }
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
  update_time_constants();
  limiter_.set_release_ms(release_ms);
}

bool TruePeakLimiter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.ceiling_db = std::min(0.0f, value);
      // The polyphase path reads config_.ceiling_db directly each block (RT-safe).
      // Forward to the inner brickwall limiter to keep its reported ceiling
      // consistent; that forward re-prepares the inner limiter, which does not
      // affect the polyphase gain envelopes used by the active processing path.
      if (prepared_) {
        limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
      }
      return true;
    case 1:
      // In-place: recomputes time constants and forwards to inner limiter
      // without clearing lookahead or gain-envelope state.
      set_release_ms(std::max(0.0f, value));
      return true;
    default:
      return false;
  }
}

bool TruePeakLimiter::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id != 0u;
}

void TruePeakLimiter::validate_config(const TruePeakLimiterConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f ||
      (config.oversample_factor != 2 && config.oversample_factor != 4 &&
       config.oversample_factor != 8)) {
    throw std::invalid_argument("invalid true peak limiter configuration");
  }
}

int TruePeakLimiter::latency_samples() const noexcept {
  // Signal-path delay is lookahead_samples_ at base rate for every mode:
  //  - polyphase: oversampled_lookahead_ holds lookahead_samples_*factor OS samples
  //    (= lookahead_samples_ base-rate); the upsampler (centered FIR + history pre-fill)
  //    and the centered batch downsampler add zero group delay.
  //  - detect_only: the lookahead buffer / inner limiter delay by lookahead_samples_.
  return limiter_.latency_samples();
}

void TruePeakLimiter::prepare_buffers(int num_channels) {
  if (lookahead_.size() == static_cast<size_t>(num_channels)) return;
  lookahead_.assign(static_cast<size_t>(num_channels), {});
  oversampled_lookahead_.assign(static_cast<size_t>(num_channels), {});
  for (auto& buffer : lookahead_) {
    buffer.prepare(static_cast<size_t>(std::max(lookahead_samples_, 0)));
  }
  for (auto& buffer : oversampled_lookahead_) {
    buffer.prepare(static_cast<size_t>(std::max(lookahead_samples_, 0) *
                                       std::max(true_peak_filter_.factor(), 1)));
  }
  fast_gain_ = 1.0f;
  slow_gain_ = 1.0f;
  crest_peak_ = 0.0f;
  crest_rms_ = 0.0f;
  oversampled_peak_window_.reset();
}

void TruePeakLimiter::update_time_constants() {
  fast_attack_coeff_ = time_to_coefficient(sample_rate_, 0.1f);
  slow_attack_coeff_ = time_to_coefficient(sample_rate_, 1.0f);
  release_coeff_ = time_to_coefficient(sample_rate_, config_.release_ms);
  crest_coeff_ = time_to_coefficient(sample_rate_, 200.0f);
}

float TruePeakLimiter::adaptive_release_coeff(float linked_peak) {
  crest_peak_ =
      std::max(linked_peak, crest_coeff_ * crest_peak_ + (1.0f - crest_coeff_) * linked_peak);
  crest_rms_ = crest_coeff_ * crest_rms_ + (1.0f - crest_coeff_) * linked_peak * linked_peak;
  const float rms = std::sqrt(std::max(crest_rms_, 1e-12f));
  const float crest = crest_peak_ / rms;
  const float transient = std::clamp((crest - 2.0f) / 8.0f, 0.0f, 1.0f);
  const float release_scale = 1.0f - 0.75f * transient;
  return time_to_coefficient(sample_rate_, config_.release_ms * release_scale);
}

}  // namespace sonare::mastering::maximizer
