#include "engine/meter_telemetry.h"

#include <algorithm>

namespace sonare::engine {
namespace {

size_t next_power_of_two(size_t value) {
  size_t out = 1;
  while (out < value) {
    out <<= 1u;
  }
  return out;
}

}  // namespace

void MeterTelemetryTap::prepare(double sample_rate, int max_block_size, uint32_t target_id,
                                size_t telemetry_capacity, const mixing::MeterConfig& config) {
  target_id_ = target_id;
  dropped_records_ = 0;
  meter_.emplace(config);
  meter_->prepare(sample_rate, max_block_size);
  telemetry_.reserve(next_power_of_two(std::max<size_t>(telemetry_capacity, 1)));
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
