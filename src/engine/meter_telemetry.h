#pragma once

/// @file meter_telemetry.h
/// @brief Realtime-safe meter tap that publishes fixed-size telemetry records.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mixing/goniometer_buffer.h"
#include "mixing/meter.h"
#include "rt/spsc_queue.h"

namespace sonare::engine {

struct MeterTelemetryRecord {
  uint32_t target_id = 0;
  int64_t render_frame = 0;
  uint64_t seq = 0;
  // Per-plane meters up to the surround width. The legacy stereo C-ABI drain
  // marshals planes 0/1 (byte-identical); the wide drain marshals
  // [0, channel_count). This is an internal in-process queue record, not a
  // C-ABI type, so widening it costs only a few KB of queue memory natively.
  std::array<float, mixing::kMaxMeterChannels> peak_db{};
  std::array<float, mixing::kMaxMeterChannels> rms_db{};
  std::array<float, mixing::kMaxMeterChannels> true_peak_db{};
  float max_true_peak_db = 0.0f;
  float correlation = 0.0f;
  float mono_compat_width = 0.0f;
  float momentary_lufs = 0.0f;
  float short_term_lufs = 0.0f;
  float integrated_lufs = 0.0f;
  float gain_reduction_db = 0.0f;
  // Number of valid per-plane meters (1..kMaxMeterChannels).
  int channel_count = 0;
  uint32_t dropped_records = 0;
};

static_assert(std::is_trivially_copyable_v<MeterTelemetryRecord>,
              "Meter telemetry records must stay trivially copyable");

class MeterTelemetryTap {
 public:
  static constexpr size_t kGoniometerCapacity = 512;

  void prepare(double sample_rate, int max_block_size, uint32_t target_id,
               size_t telemetry_capacity = 64,
               const mixing::MeterConfig& config = mixing::MeterConfig{});
  void reset() noexcept;

  void process(float* const* channels, int num_channels, int num_frames,
               int64_t render_frame) noexcept;
  void process_lightweight(float* const* channels, int num_channels, int num_frames,
                           int64_t render_frame, uint32_t target_id) noexcept;

  bool pop(MeterTelemetryRecord& out) noexcept { return telemetry_.pop(out); }
  mixing::MeterSnapshot snapshot() const noexcept {
    return meter_.has_value() ? meter_->snapshot() : mixing::MeterSnapshot{};
  }
  uint32_t dropped_count() const noexcept { return dropped_records_; }
  size_t read_goniometer(mixing::GoniometerPoint* out, size_t max_points) const noexcept;

 private:
  void publish(const mixing::MeterSnapshot& snapshot, int64_t render_frame) noexcept;
  void publish(MeterTelemetryRecord record) noexcept;
  void push_goniometer(float* const* channels, int num_channels, int num_frames) noexcept;

  std::optional<mixing::MeterProcessor> meter_{};
  rt::SpscQueue<MeterTelemetryRecord> telemetry_{};
  mixing::GoniometerBuffer<kGoniometerCapacity> goniometer_{};
  uint32_t target_id_ = 0;
  uint32_t dropped_records_ = 0;
};

}  // namespace sonare::engine
