#include "engine/meter_telemetry.h"

#include <algorithm>
#include <cmath>

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
  // Own monotonic counter -- the full meter's seq only advances inside the full
  // publish() path, so reusing it here would stamp every lightweight record
  // with a stale/zero seq.
  record.seq = ++lightweight_seq_;
  // Fields the lightweight path does not measure (true-peak / LUFS / gain
  // reduction) keep their floor/zero defaults from MeterTelemetryRecord, which
  // are finite and JSON-safe. Earlier this path stamped NaN, which serialized
  // to an invalid JSON `NaN` token on the Python host and to a type-violating
  // `null` on Node/WASM.

  // Per-plane peak/RMS up to the surround width. Stereo stays bit-identical
  // (meters == 2); a surround lane/bus now fills all of its planes.
  const int meters = std::min(num_channels, mixing::kMaxMeterChannels);
  record.channel_count = meters;
  std::array<double, mixing::kMaxMeterChannels> sum_sq{};
  std::array<float, mixing::kMaxMeterChannels> peak{};
  for (int ch = 0; ch < meters; ++ch) {
    const float* channel = channels[ch];
    if (!channel) continue;
    for (int i = 0; i < num_frames; ++i) {
      const float value = channel[i];
      peak[static_cast<size_t>(ch)] = std::max(peak[static_cast<size_t>(ch)], std::abs(value));
      sum_sq[static_cast<size_t>(ch)] += static_cast<double>(value) * static_cast<double>(value);
    }
  }

  for (int ch = 0; ch < meters; ++ch) {
    const size_t c = static_cast<size_t>(ch);
    const float rms = std::sqrt(sum_sq[c] / static_cast<double>(num_frames));
    record.peak_db[c] = peak[c] > 0.0f ? 20.0f * std::log10(peak[c]) : -120.0f;
    record.rms_db[c] = rms > 0.0f ? 20.0f * std::log10(rms) : -120.0f;
  }
  if (num_channels >= 2 && channels[0] && channels[1]) {
    double cross = 0.0;
    for (int i = 0; i < num_frames; ++i) {
      cross += static_cast<double>(channels[0][i]) * static_cast<double>(channels[1][i]);
    }
    const double denom = std::sqrt(sum_sq[0] * sum_sq[1]);
    if (denom > 0.0) {
      record.correlation = static_cast<float>(std::clamp(cross / denom, -1.0, 1.0));
    }
    // Derive the mid/side energy from the per-channel sums already accumulated
    // above (mid = (L+R)/sqrt2, side = (L-R)/sqrt2), so the lightweight tap
    // reports the SAME mono_compat_width formula as the full MeterProcessor with
    // no extra per-sample work. Earlier this path used the cheaper but divergent
    // 1 - |correlation| proxy.
    const double channel_energy = sum_sq[0] + sum_sq[1];
    const double mid_energy = 0.5 * channel_energy + cross;
    const double side_energy = 0.5 * channel_energy - cross;
    record.mono_compat_width = mixing::mono_compat_width_from_energy(mid_energy, side_energy);
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
  record.channel_count = snapshot.channel_count;
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
