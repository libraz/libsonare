#include "mixing/gain.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/db.h"
#include "util/numeric_validation.h"

namespace sonare::mixing {
namespace {

// Largest dB whose float linear gain is finite; fader, trim and VCA sums clamp here.
const float kMaxFiniteGainDb = [] {
  float db = linear_to_db(std::numeric_limits<float>::max());
  while (!numeric::finite(db_to_linear(db))) db = std::nextafter(db, 0.0f);
  return db;
}();

// The floor is the dB image of silence (linear_to_db(0)), so a gain at or below it is exactly zero.
float summed_db_to_linear(float db) noexcept {
  if (db <= constants::kFloorDb) return 0.0f;
  return db_to_linear(std::min(db, kMaxFiniteGainDb));
}

// A glide toward silence lands on zero once it is below the floor's own gain.
const float kFloorGain = db_to_linear(constants::kFloorDb);

}  // namespace

GainProcessor::GainProcessor(GainConfig config)
    : smoothing_ms_(std::isfinite(config.smoothing_ms) && config.smoothing_ms >= 0.0f
                        ? config.smoothing_ms
                        : 5.0f),
      gain_db_(std::isfinite(config.gain_db) ? config.gain_db : 0.0f) {}

void GainProcessor::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  smoother_.prepare(sample_rate_, smoothing_ms_);
  smoother_.reset(summed_db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db()));
}

void GainProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  const float target =
      summed_db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db());
  // Zero smoothing snaps: a full one-pole step away from a near-FLT_MAX gain cancels the target.
  if (smoothing_ms_ > 0.0f) {
    smoother_.set_target(target);
  } else {
    smoother_.reset(target);
  }
  const bool to_silence = target == 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    const float gain = to_silence ? smoother_.process_snapping(kFloorGain) : smoother_.process();
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        channels[ch][i] *= gain;
      }
    }
  }
}

void GainProcessor::reset() {
  smoother_.reset(summed_db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db()));
}

void GainProcessor::settle() noexcept {
  // Snap straight to the current steady-state gain (the same target process()
  // would smooth toward) so the next render block opens at that gain with no
  // ramp-in, keeping an offline bounce deterministic.
  smoother_.reset(summed_db_to_linear(gain_db_.load(std::memory_order_relaxed) + vca_offset_db()));
}

void GainProcessor::set_gain_db(float gain_db) noexcept {
  if (!std::isfinite(gain_db)) return;
  gain_db_.store(gain_db, std::memory_order_relaxed);
}

void GainProcessor::set_vca_offset_db(float offset_db) noexcept {
  if (!std::isfinite(offset_db)) return;
  vca_trim_offset_db_.store(offset_db, std::memory_order_relaxed);
}

void GainProcessor::add_vca_group_offset_db(float delta_db) noexcept {
  if (!std::isfinite(delta_db)) return;
  // VCA-group contributions accumulate (a strip may belong to several groups).
  // Group membership changes are serialized on the control thread, but use an
  // atomic fetch_add rather than a separate load/store read-modify-write so the
  // accumulation stays correct even if two group moves overlap, at no extra cost
  // on the control path.
  //
  // The offset is maintained as a running sum of dB deltas, so repeated group
  // gain moves accumulate float32 rounding error. The drift is bounded by
  // ~epsilon * (number of moves) in dB and is inaudible for any realistic
  // session; a member's exact offset could be recomputed from the owning groups
  // if a registry existed, but that precision is not worth the coupling here.
  float expected = vca_group_offset_db_.load(std::memory_order_relaxed);
  while (!vca_group_offset_db_.compare_exchange_weak(expected, expected + delta_db,
                                                     std::memory_order_relaxed)) {
  }
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
