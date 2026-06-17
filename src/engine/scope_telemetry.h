#pragma once

/// @file scope_telemetry.h
/// @brief Realtime-safe spectrum + goniometer (vectorscope) tap that publishes
///        fixed-size, target-addressed telemetry records.

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "core/fft.h"
#include "mixing/goniometer_buffer.h"
#include "rt/spsc_queue.h"

namespace sonare::engine {

/// One spectrum + vectorscope snapshot for a single mix target (lane / bus /
/// master), published from the audio thread and drained by the host. The layout
/// is fixed-size so the record stays trivially copyable through the SPSC queue;
/// @c band_count / @c point_count report how many leading entries of @c bands /
/// @c points are valid.
struct ScopeTelemetryRecord {
  static constexpr size_t kMaxBands = 64;
  static constexpr size_t kMaxPoints = 32;

  uint32_t target_id = 0;
  int64_t render_frame = 0;
  uint64_t seq = 0;
  uint32_t dropped_records = 0;
  /// Linear FFT magnitude bands in dBFS, spanning [0, Nyquist]; band i covers a
  /// contiguous slice of the FFT bins, so its center frequency is
  /// (i + 0.5) / band_count * (sample_rate / 2).
  uint32_t band_count = 0;
  std::array<float, kMaxBands> bands{};
  /// Decimated goniometer scatter points (raw left/right) for the vectorscope.
  uint32_t point_count = 0;
  std::array<mixing::GoniometerPoint, kMaxPoints> points{};
};

static_assert(std::is_trivially_copyable_v<ScopeTelemetryRecord>,
              "Scope telemetry records must stay trivially copyable");

class ScopeTelemetryTap {
 public:
  /// Prepares the FFT scratch + ring. @p band_count is clamped to
  /// [1, kMaxBands]; @p n_fft is rounded up to a power of two and bounds the
  /// frequency resolution. @p telemetry_capacity is the per-process ring depth.
  void prepare(double sample_rate, int max_block_size, size_t telemetry_capacity = 16,
               int n_fft = 2048, uint32_t band_count = 48);
  void reset() noexcept;

  /// Number of FFT magnitude bands each record carries.
  uint32_t band_count() const noexcept { return band_count_; }

  /// Audio-thread interval gate. Call once per render block before the render
  /// pass; returns true on the blocks where process() should publish. interval
  /// 0 disables capture entirely. begin_block must run on the audio thread.
  bool begin_block(int interval_frames, int num_frames) noexcept;

  /// Publishes one target's spectrum + goniometer snapshot, but only when the
  /// current block is "due" (see begin_block). Allocation-free; audio-thread
  /// only. @p target_id tags the record (lane / bus / master addressing).
  void process(float* const* channels, int num_channels, int num_frames, int64_t render_frame,
               uint32_t target_id) noexcept;

  bool pop(ScopeTelemetryRecord& out) noexcept { return telemetry_.pop(out); }
  uint32_t dropped_count() const noexcept { return dropped_records_; }

 private:
  void publish(ScopeTelemetryRecord record) noexcept;

  double sample_rate_ = 48000.0;
  int n_fft_ = 2048;
  uint32_t band_count_ = 48;
  bool capture_due_ = false;
  int frame_accum_ = 0;
  uint64_t seq_ = 0;
  uint32_t dropped_records_ = 0;

  std::vector<float> window_;                  // Hann window, length n_fft_
  std::vector<float> fft_input_;               // windowed + zero-padded mono frame
  std::vector<std::complex<float>> spectrum_;  // n_fft_/2 + 1 bins
  std::unique_ptr<FFT> fft_;

  rt::SpscQueue<ScopeTelemetryRecord> telemetry_{};
  mixing::GoniometerBuffer<512> goniometer_{};
};

}  // namespace sonare::engine
