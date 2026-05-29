#include "mastering/stereo/phase_align.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"

namespace sonare::mastering::stereo {

PhaseAlign::PhaseAlign(PhaseAlignConfig config) : config_(config) { validate_config(config_); }

void PhaseAlign::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  prepared_ = true;
  rebuild_delay();
}

void PhaseAlign::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "PhaseAlign");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }
  if (num_channels < 2 || total_delay_samples() == 0.0f) {
    return;
  }

  const int delayed_ch = config_.delay_right ? 1 : 0;
  for (int i = 0; i < num_samples; ++i) {
    channels[delayed_ch][i] = process_delay(channels[delayed_ch][i]);
  }
}

void PhaseAlign::reset() {
  std::fill(delay_.begin(), delay_.end(), 0.0f);
  delay_index_ = 0;
}

void PhaseAlign::set_config(const PhaseAlignConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    rebuild_delay();
  }
}

bool PhaseAlign::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Keep within [0, 1): the whole-sample delay (delay_samples_) is fixed, so
      // the delay-line size is unchanged and process_delay() reads the new
      // fraction directly without reallocating or clearing buffered samples.
      config_.fractional_delay_samples = std::clamp(value, 0.0f, std::nextafter(1.0f, 0.0f));
      return true;
    default:
      return false;
  }
}

void PhaseAlign::validate_config(const PhaseAlignConfig& config) {
  if (config.delay_samples < 0 || config.fractional_delay_samples < 0.0f ||
      config.fractional_delay_samples >= 1.0f) {
    throw std::invalid_argument("phase align delay must be non-negative");
  }
}

void PhaseAlign::rebuild_delay() {
  const int whole_delay = static_cast<int>(std::floor(total_delay_samples()));
  delay_.assign(static_cast<size_t>(std::max(whole_delay + 5, 1)), 0.0f);
  delay_index_ = 0;
}

float PhaseAlign::process_delay(float input) {
  const float delay = total_delay_samples();
  const int whole_delay = static_cast<int>(std::floor(delay));
  const float fraction = delay - static_cast<float>(whole_delay);

  delay_[delay_index_] = input;
  const size_t size = delay_.size();
  float delayed = 0.0f;
  for (int tap = 0; tap < 5; ++tap) {
    float weight = 1.0f;
    for (int other = 0; other < 5; ++other) {
      if (other == tap) {
        continue;
      }
      weight *= (fraction - static_cast<float>(other)) / static_cast<float>(tap - other);
    }
    const size_t read_index = (delay_index_ + size - static_cast<size_t>(whole_delay + tap)) % size;
    delayed += weight * delay_[read_index];
  }
  delay_[delay_index_] = input;
  delay_index_ = (delay_index_ + 1) % delay_.size();
  return delayed;
}

float PhaseAlign::total_delay_samples() const noexcept {
  return static_cast<float>(config_.delay_samples) + config_.fractional_delay_samples;
}

float PhaseAlign::estimate_delay_samples(const float* reference, const float* target,
                                         int num_samples, int max_abs_delay) {
  if (num_samples < 0 || max_abs_delay < 0) {
    throw std::invalid_argument("invalid delay estimation dimensions");
  }
  if (num_samples == 0 || max_abs_delay == 0) {
    return 0.0f;
  }
  if (reference == nullptr || target == nullptr) {
    throw std::invalid_argument("delay estimation buffers must not be null");
  }

  const auto score_lag = [&](int lag) {
    double cross = 0.0;
    double ref_energy = 0.0;
    double target_energy = 0.0;
    int count = 0;
    const int ref_start = lag < 0 ? -lag : 0;
    const int target_start = lag > 0 ? lag : 0;
    const int count_limit = num_samples - std::abs(lag);
    for (int i = 0; i < count_limit; ++i) {
      const float ref = reference[ref_start + i];
      const float tar = target[target_start + i];
      cross += static_cast<double>(ref) * tar;
      ref_energy += static_cast<double>(ref) * ref;
      target_energy += static_cast<double>(tar) * tar;
      ++count;
    }
    if (count == 0 || ref_energy <= 0.0 || target_energy <= 0.0) {
      return -std::numeric_limits<double>::infinity();
    }
    return cross / std::sqrt(ref_energy * target_energy);
  };

  int best_lag = 0;
  double best_score = -std::numeric_limits<double>::infinity();
  for (int lag = -max_abs_delay; lag <= max_abs_delay; ++lag) {
    const double score = score_lag(lag);
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }

  if (best_lag <= -max_abs_delay || best_lag >= max_abs_delay) {
    return static_cast<float>(best_lag);
  }
  const double left = score_lag(best_lag - 1);
  const double center = score_lag(best_lag);
  const double right = score_lag(best_lag + 1);
  const double denominator = left - 2.0 * center + right;
  if (std::abs(denominator) < 1.0e-12) {
    return static_cast<float>(best_lag);
  }
  const double offset = 0.5 * (left - right) / denominator;
  return static_cast<float>(static_cast<double>(best_lag) + std::clamp(offset, -0.5, 0.5));
}

}  // namespace sonare::mastering::stereo
