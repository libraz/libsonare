#include "engine/meter_telemetry.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/math_utils.h"

namespace sonare::engine {

using constants::kFloorDb;

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
  block_active_ = false;
  staged_count_ = 0;
}

void MeterTelemetryTap::begin_block() noexcept {
  block_active_ = true;
  staged_count_ = 0;
}

void MeterTelemetryTap::end_block() noexcept {
  if (!block_active_) return;
  for (size_t i = 0; i < staged_count_; ++i) {
    publish(staged_records_[i]);
  }
  staged_count_ = 0;
  block_active_ = false;
}

void MeterTelemetryTap::process(float* const* channels, int num_channels, int num_frames,
                                int64_t render_frame) noexcept {
  if (!meter_.has_value()) {
    return;
  }
  meter_->process(channels, num_channels, num_frames);
  push_goniometer(channels, num_channels, num_frames);
  publish(meter_->snapshot(), render_frame, num_frames);
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
    record.peak_db[c] = peak[c] > 0.0f ? 20.0f * std::log10(peak[c]) : kFloorDb;
    record.rms_db[c] = rms > 0.0f ? 20.0f * std::log10(rms) : kFloorDb;
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

  stage(record, num_frames);
}

size_t MeterTelemetryTap::read_goniometer(mixing::GoniometerPoint* out,
                                          size_t max_points) const noexcept {
  return goniometer_.read_latest(out, max_points);
}

void MeterTelemetryTap::publish(const mixing::MeterSnapshot& snapshot, int64_t render_frame,
                                int num_frames) noexcept {
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

  stage(record, num_frames);
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

void MeterTelemetryTap::stage(MeterTelemetryRecord record, int num_frames) noexcept {
  if (!block_active_) {
    publish(record);
    return;
  }
  const int64_t frames = std::max(0, num_frames);
  for (size_t i = 0; i < staged_count_; ++i) {
    if (staged_records_[i].target_id != record.target_id) {
      continue;
    }
    // A second (or later) sub-block for a target already staged this host
    // block: the staged record must describe the WHOLE block, not just the
    // most recent fragment, so peak/true-peak merge as an element-wise max
    // and RMS is recomputed from accumulated energy rather than overwritten.
    MeterTelemetryRecord& staged = staged_records_[i];
    StagedEnergy& energy = staged_energy_[i];
    const int channel_count = std::clamp(record.channel_count, 0, mixing::kMaxMeterChannels);
    for (int ch = 0; ch < channel_count; ++ch) {
      const size_t c = static_cast<size_t>(ch);
      staged.peak_db[c] = std::max(staged.peak_db[c], record.peak_db[c]);
      staged.true_peak_db[c] = std::max(staged.true_peak_db[c], record.true_peak_db[c]);
      const double mean_square = std::pow(10.0, static_cast<double>(record.rms_db[c]) / 10.0);
      energy.sum_sq[c] += mean_square * static_cast<double>(frames);
    }
    energy.frames += frames;
    if (energy.frames > 0) {
      for (int ch = 0; ch < channel_count; ++ch) {
        const size_t c = static_cast<size_t>(ch);
        const double mean_square = energy.sum_sq[c] / static_cast<double>(energy.frames);
        staged.rms_db[c] =
            mean_square > 0.0 ? static_cast<float>(10.0 * std::log10(mean_square)) : kFloorDb;
      }
    }
    staged.max_true_peak_db = std::max(staged.max_true_peak_db, record.max_true_peak_db);
    // seq/render_frame and the already-integrating fields (LUFS, gain
    // reduction, correlation, mono-compat width) describe the meter's running
    // state as of the most recent sub-block, so the latest value is correct
    // for them (no merge needed).
    staged.seq = record.seq;
    staged.render_frame = record.render_frame;
    staged.channel_count = record.channel_count;
    staged.correlation = record.correlation;
    staged.mono_compat_width = record.mono_compat_width;
    staged.momentary_lufs = record.momentary_lufs;
    staged.short_term_lufs = record.short_term_lufs;
    staged.integrated_lufs = record.integrated_lufs;
    staged.gain_reduction_db = record.gain_reduction_db;
    return;
  }
  if (staged_count_ < staged_records_.size()) {
    const int channel_count = std::clamp(record.channel_count, 0, mixing::kMaxMeterChannels);
    StagedEnergy energy{};
    for (int ch = 0; ch < channel_count; ++ch) {
      const size_t c = static_cast<size_t>(ch);
      energy.sum_sq[c] = std::pow(10.0, static_cast<double>(record.rms_db[c]) / 10.0) *
                         static_cast<double>(frames);
    }
    energy.frames = frames;
    staged_energy_[staged_count_] = energy;
    staged_records_[staged_count_++] = record;
  } else {
    ++dropped_records_;
  }
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
