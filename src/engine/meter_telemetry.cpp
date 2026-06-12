#include "engine/meter_telemetry.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/math_utils.h"

namespace sonare::engine {

void MeterTelemetryTap::prepare(double sample_rate, int max_block_size, uint32_t target_id,
                                size_t telemetry_capacity, const mixing::MeterConfig& config) {
  target_id_ = target_id;
  dropped_records_ = 0;
  meter_.emplace(config);
  meter_->prepare(sample_rate, max_block_size);
  telemetry_.reserve(next_power_of_2(std::max<size_t>(telemetry_capacity, 1)));
  goniometer_.reset();
}

void MeterTelemetryTap::reset() noexcept {
  if (meter_.has_value()) {
    meter_->reset();
  }
  goniometer_.reset();
  dropped_records_ = 0;
}

void MeterTelemetryTap::process(float* const* channels, int num_channels, int num_frames,
                                int64_t render_frame) noexcept {
  if (!meter_.has_value()) {
    return;
  }
  meter_->process(channels, num_channels, num_frames);
  push_goniometer(channels, num_channels, num_frames);
  publish(meter_->snapshot(), render_frame);
}

void MeterTelemetryTap::process_lightweight(float* const* channels, int num_channels,
                                            int num_frames, int64_t render_frame,
                                            uint32_t target_id) noexcept {
  if (channels == nullptr || num_channels <= 0 || num_frames <= 0) return;

  MeterTelemetryRecord record{};
  record.target_id = target_id;
  record.render_frame = render_frame;
  record.seq = meter_.has_value() ? meter_->snapshot().seq : 0;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  record.true_peak_db = {nan, nan};
  record.max_true_peak_db = nan;
  record.momentary_lufs = nan;
  record.short_term_lufs = nan;
  record.integrated_lufs = nan;
  record.gain_reduction_db = nan;
  record.correlation = nan;
  record.mono_compat_width = nan;

  std::array<double, 2> sum_sq{};
  std::array<float, 2> peak{};
  for (int ch = 0; ch < std::min(num_channels, 2); ++ch) {
    const float* channel = channels[ch];
    if (!channel) continue;
    for (int i = 0; i < num_frames; ++i) {
      const float value = channel[i];
      peak[static_cast<size_t>(ch)] = std::max(peak[static_cast<size_t>(ch)], std::abs(value));
      sum_sq[static_cast<size_t>(ch)] += static_cast<double>(value) * static_cast<double>(value);
    }
  }

  for (size_t ch = 0; ch < 2; ++ch) {
    const float rms = std::sqrt(sum_sq[ch] / static_cast<double>(num_frames));
    record.peak_db[ch] = peak[ch] > 0.0f ? 20.0f * std::log10(peak[ch]) : -120.0f;
    record.rms_db[ch] = rms > 0.0f ? 20.0f * std::log10(rms) : -120.0f;
  }
  if (num_channels >= 2 && channels[0] && channels[1]) {
    double cross = 0.0;
    for (int i = 0; i < num_frames; ++i) {
      cross += static_cast<double>(channels[0][i]) * static_cast<double>(channels[1][i]);
    }
    const double denom = std::sqrt(sum_sq[0] * sum_sq[1]);
    if (denom > 0.0) {
      record.correlation = static_cast<float>(std::clamp(cross / denom, -1.0, 1.0));
      record.mono_compat_width = 1.0f - std::abs(record.correlation);
    }
  }

  publish(record);
}

size_t MeterTelemetryTap::read_goniometer(mixing::GoniometerPoint* out,
                                          size_t max_points) const noexcept {
  return goniometer_.read_latest(out, max_points);
}

void MeterTelemetryTap::publish(const mixing::MeterSnapshot& snapshot,
                                int64_t render_frame) noexcept {
  MeterTelemetryRecord record{};
  record.target_id = target_id_;
  record.render_frame = render_frame;
  record.seq = snapshot.seq;
  record.peak_db = snapshot.peak_db;
  record.rms_db = snapshot.rms_db;
  record.true_peak_db = snapshot.true_peak_db;
  record.max_true_peak_db = snapshot.max_true_peak_db;
  record.correlation = snapshot.correlation;
  record.mono_compat_width = snapshot.mono_compat_width;
  record.momentary_lufs = snapshot.momentary_lufs;
  record.short_term_lufs = snapshot.short_term_lufs;
  record.integrated_lufs = snapshot.integrated_lufs;
  record.gain_reduction_db = snapshot.gain_reduction_db;
  record.dropped_records = dropped_records_;

  publish(record);
}

void MeterTelemetryTap::publish(MeterTelemetryRecord record) noexcept {
  record.dropped_records = dropped_records_;
  if (telemetry_.push(record)) {
    return;
  }

  // Queue full: just account for the drop. The producer (audio thread) must
  // never pop -- pop() is the consumer role owned by the host via
  // pop_meter_telemetry(), and a producer-side pop would race the consumer on
  // the queue tail. The running dropped_records_ count is propagated to the
  // host on the next record that pushes successfully.
  ++dropped_records_;
}

void MeterTelemetryTap::push_goniometer(float* const* channels, int num_channels,
                                        int num_frames) noexcept {
  if (channels == nullptr || num_channels < 2 || channels[0] == nullptr || channels[1] == nullptr ||
      num_frames <= 0) {
    return;
  }

  const int stride = std::max(1, num_frames / 16);
  for (int i = 0; i < num_frames; i += stride) {
    goniometer_.push(channels[0][i], channels[1][i]);
  }
}

}  // namespace sonare::engine
