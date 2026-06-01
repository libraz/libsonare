#include "mastering/stereo/phase_align.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

PhaseAlign::PhaseAlign(PhaseAlignConfig config) : config_(config) { validate_config(config_); }

void PhaseAlign::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  prepared_ = true;
  rebuild_delay();
}

void PhaseAlign::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "PhaseAlign");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
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
    throw SonareException(ErrorCode::InvalidParameter, "phase align delay must be non-negative");
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

  // Fractional delay via a 5-point Lagrange interpolator. The fraction in [0, 1)
  // is evaluated at the edge of the node grid {0, 1, 2, 3, 4} (taps
  // whole_delay+0 .. whole_delay+4). A centered stencil (taps
  // whole_delay-2 .. whole_delay+2, evaluating at 2 + fraction) would lower the
  // interpolation ripple, but it shifts the impulse-response peak location and
  // is therefore deferred: the existing "PhaseAlign supports fractional sample
  // delay" regression encodes the current edge-stencil group-delay shape, and
  // re-centering cannot be done without updating that test. See the audit note
  // for item 3 (documented + skipped) rather than risk a silent behavior change.
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
  delay_index_ = (delay_index_ + 1) % delay_.size();
  return delayed;
}

float PhaseAlign::total_delay_samples() const noexcept {
  return static_cast<float>(config_.delay_samples) + config_.fractional_delay_samples;
}

float PhaseAlign::estimate_delay_samples(const float* reference, const float* target,
                                         int num_samples, int max_abs_delay) {
  if (num_samples < 0 || max_abs_delay < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid delay estimation dimensions");
  }
  if (num_samples == 0 || max_abs_delay == 0) {
    return 0.0f;
  }
  if (reference == nullptr || target == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "delay estimation buffers must not be null");
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
