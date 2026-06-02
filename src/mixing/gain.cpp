#include "mixing/gain.h"

#include "util/db.h"

namespace sonare::mixing {

GainProcessor::GainProcessor(GainConfig config)
    : smoothing_ms_(config.smoothing_ms), gain_db_(config.gain_db) {}

void GainProcessor::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  smoother_.prepare(sample_rate_, smoothing_ms_);
  smoother_.reset(db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db()));
}

void GainProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  smoother_.set_target(db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db()));
  for (int i = 0; i < num_samples; ++i) {
    const float gain = smoother_.process();
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        channels[ch][i] *= gain;
      }
    }
  }
}

void GainProcessor::reset() {
  smoother_.reset(db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db()));
}

void GainProcessor::set_gain_db(float gain_db) noexcept {
  gain_db_.store(gain_db, std::memory_order_relaxed);
}

void GainProcessor::set_vca_offset_db(float offset_db) noexcept {
  vca_trim_offset_db_.store(offset_db, std::memory_order_relaxed);
}

void GainProcessor::add_vca_group_offset_db(float delta_db) noexcept {
  // VCA-group contributions accumulate (a strip may belong to several groups).
  // Group membership changes are serialized on the control thread, so a plain
  // load/store read-modify-write is sufficient here.
  vca_group_offset_db_.store(vca_group_offset_db_.load(std::memory_order_relaxed) + delta_db,
                             std::memory_order_relaxed);
}

float GainProcessor::vca_offset_db() const noexcept {
  return vca_trim_offset_db_.load(std::memory_order_relaxed) +
         vca_group_offset_db_.load(std::memory_order_relaxed);
}

float GainProcessor::vca_trim_offset_db() const noexcept {
  return vca_trim_offset_db_.load(std::memory_order_relaxed);
}

float GainProcessor::vca_group_offset_db() const noexcept {
  return vca_group_offset_db_.load(std::memory_order_relaxed);
}

}  // namespace sonare::mixing
