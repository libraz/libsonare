#include <algorithm>
#include <atomic>
#include <cmath>

#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_registry.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;

EqualizerSpectrumSnapshot EqualizerProcessor::spectrum_snapshot() const noexcept {
  EqualizerSpectrumSnapshot copy;
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint32_t before = spectrum_guard_.load(std::memory_order_acquire);
    // Spin while a write is in progress without consuming the retry budget.
    while ((before & 1U) != 0U) {
      before = spectrum_guard_.load(std::memory_order_acquire);
    }
    copy = spectrum_snapshot_;
    // Ensure the payload read completes before observing the trailing guard.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t after = spectrum_guard_.load(std::memory_order_acquire);
    if (before == after && (after & 1U) == 0U) {
      return copy;
    }
  }
  return copy;
}

void EqualizerProcessor::capture_stream(const float* const* channels, int num_channels,
                                        int num_samples,
                                        std::array<SpectrumPoint, kSpectrumStreamCapacity>& stream,
                                        size_t& count) noexcept {
  count = 0;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  const int step = std::max(1, (num_samples + static_cast<int>(kSpectrumStreamCapacity) - 1) /
                                   static_cast<int>(kSpectrumStreamCapacity));
  for (int src = 0; src < num_samples && count < kSpectrumStreamCapacity; src += step) {
    const float left = channels[0] != nullptr ? channels[0][src] : 0.0f;
    const float right = num_channels > 1 && channels[1] != nullptr ? channels[1][src] : left;
    stream[count++] = {left, right};
  }
}

void EqualizerProcessor::publish_spectrum_snapshot(const EqualizerSpectrumSnapshot& pre_snapshot,
                                                   const float* const* channels, int num_channels,
                                                   int num_samples) noexcept {
  EqualizerSpectrumSnapshot next = pre_snapshot;
  capture_stream(channels, num_channels, num_samples, next.post, next.post_count);
  next.band_gain_db.fill(0.0f);
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& band = bands_[i];
    if (band.enabled && !band.bypassed) {
      next.band_gain_db[i] = band.dyn.enabled ? last_applied_gain_db_[i] : band.gain_db;
    }
  }
  next.profile_db.fill(kFloorDb);
  next.seq = ++spectrum_seq_;

  double sum = 0.0;
  size_t count = 0;
  for (size_t i = 0; i < next.post_count; ++i) {
    const double left = next.post[i].left;
    const double right = next.post[i].right;
    sum += left * left + right * right;
    count += 2;
  }
  const float level_db =
      count == 0 ? kFloorDb : linear_to_db(static_cast<float>(std::sqrt(sum / count)));
  bool has_profile_band = false;
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& band = bands_[i];
    if (!band.enabled || band.bypassed || !(band.frequency_hz > 0.0f)) {
      continue;
    }
    const double clamped_freq = std::clamp(static_cast<double>(band.frequency_hz), 20.0, 20000.0);
    const double normalized = std::log2(clamped_freq / 20.0) / std::log2(20000.0 / 20.0);
    const size_t bucket = std::min(
        kSpectrumProfileBands - 1,
        static_cast<size_t>(std::max(0.0, std::floor(normalized * kSpectrumProfileBands))));
    const float activity_db = level_db + std::max(0.0f, std::abs(last_applied_gain_db_[i]));
    next.profile_db[bucket] = std::max(next.profile_db[bucket], activity_db);
    has_profile_band = true;
  }
  if (!has_profile_band && count > 0) {
    next.profile_db.fill(level_db);
  }

  const uint32_t guard = spectrum_guard_.load(std::memory_order_relaxed);
  spectrum_guard_.store(guard + 1U, std::memory_order_release);
  // Publish the odd guard before mutating the payload so readers observe it.
  std::atomic_thread_fence(std::memory_order_release);
  spectrum_snapshot_ = next;
  spectrum_guard_.store(guard + 2U, std::memory_order_release);

  if (config_.spectrum_instance_id != 0) {
    SpectrumProfile profile;
    profile.instance_id = config_.spectrum_instance_id;
    profile.band_db = next.profile_db;
    profile.seq = next.seq;
    profile.active = true;
    SpectrumRegistry::instance().publish(profile);
  }
}
}  // namespace sonare::mastering::eq
