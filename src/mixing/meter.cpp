#include "mixing/meter.h"

#include <algorithm>
#include <cmath>

#include "util/db.h"

namespace sonare::mixing {

void MeterProcessor::prepare(double, int) { reset(); }

void MeterProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  MeterSnapshot next;
  double sum_l = 0.0;
  double sum_r = 0.0;
  double sum_lr = 0.0;

  const int meters = std::min(num_channels, 2);
  for (int ch = 0; ch < meters; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    float peak = 0.0f;
    double sum_square = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      const float sample = channels[ch][i];
      peak = std::max(peak, std::abs(sample));
      sum_square += static_cast<double>(sample) * sample;
    }
    next.peak_db[static_cast<size_t>(ch)] = linear_to_db(peak);
    next.rms_db[static_cast<size_t>(ch)] =
        linear_to_db(std::sqrt(sum_square / static_cast<double>(num_samples)));
  }

  if (num_channels >= 2 && channels[0] != nullptr && channels[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      const double left = channels[0][i];
      const double right = channels[1][i];
      sum_l += left * left;
      sum_r += right * right;
      sum_lr += left * right;
    }
    const double denom = std::sqrt(sum_l * sum_r);
    next.correlation = denom > 1.0e-12 ? static_cast<float>(sum_lr / denom) : 0.0f;
  }

  next.seq = ++seq_;
  const int next_index = 1 - active_index_.load(std::memory_order_relaxed);
  snapshots_[static_cast<size_t>(next_index)] = next;
  active_index_.store(next_index, std::memory_order_release);
}

void MeterProcessor::reset() {
  snapshots_[0] = MeterSnapshot{};
  snapshots_[1] = MeterSnapshot{};
  active_index_.store(0, std::memory_order_release);
  seq_ = 0;
}

MeterSnapshot MeterProcessor::snapshot() const noexcept {
  return snapshots_[static_cast<size_t>(active_index_.load(std::memory_order_acquire))];
}

}  // namespace sonare::mixing
