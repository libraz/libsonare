#include "engine/scope_telemetry.h"

#include <algorithm>
#include <cmath>

#include "core/window.h"
#include "util/math_utils.h"

namespace sonare::engine {

void ScopeTelemetryTap::prepare(double sample_rate, int max_block_size, size_t telemetry_capacity,
                                int n_fft, uint32_t band_count) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  band_count_ =
      static_cast<uint32_t>(std::clamp<size_t>(band_count, 1, ScopeTelemetryRecord::kMaxBands));
  const int min_fft = std::max({n_fft, max_block_size, 64});
  n_fft_ = static_cast<int>(next_power_of_2(static_cast<size_t>(min_fft)));

  fft_ = std::make_unique<FFT>(n_fft_);
  fft_input_.assign(static_cast<size_t>(n_fft_), 0.0f);
  spectrum_.assign(static_cast<size_t>(fft_->n_bins()), std::complex<float>{});

  capture_due_ = false;
  frame_accum_ = 0;
  seq_ = 0;
  dropped_records_ = 0;
  telemetry_.reserve(next_power_of_2(std::max<size_t>(telemetry_capacity, 1)));
  goniometer_.reset();
}

void ScopeTelemetryTap::reset() noexcept {
  capture_due_ = false;
  frame_accum_ = 0;
  dropped_records_ = 0;
  goniometer_.reset();
}

bool ScopeTelemetryTap::begin_block(int interval_frames, int num_frames) noexcept {
  if (interval_frames <= 0 || num_frames <= 0) {
    capture_due_ = false;
    return false;
  }
  frame_accum_ += num_frames;
  if (frame_accum_ >= interval_frames) {
    frame_accum_ = 0;
    capture_due_ = true;
  } else {
    capture_due_ = false;
  }
  return capture_due_;
}

void ScopeTelemetryTap::process(float* const* channels, int num_channels, int num_frames,
                                int64_t render_frame, uint32_t target_id) noexcept {
  if (!capture_due_ || channels == nullptr || num_channels <= 0 || num_frames <= 0 ||
      fft_ == nullptr) {
    return;
  }

  // Build a windowed, zero-padded mono frame of up to n_fft_ samples. The Hann
  // window is applied over the actual sample count so a short block is a valid
  // windowed segment (zero-padding only interpolates the spectrum).
  const int m = std::min(num_frames, n_fft_);
  const float inv_channels = 1.0f / static_cast<float>(std::min(num_channels, 2));
  for (int i = 0; i < m; ++i) {
    float mono = 0.0f;
    for (int ch = 0; ch < std::min(num_channels, 2); ++ch) {
      if (channels[ch]) mono += channels[ch][i];
    }
    fft_input_[static_cast<size_t>(i)] = mono * inv_channels * hann_value(i, m, true);
  }
  for (int i = m; i < n_fft_; ++i) {
    fft_input_[static_cast<size_t>(i)] = 0.0f;
  }

  fft_->forward(fft_input_.data(), spectrum_.data());

  ScopeTelemetryRecord record{};
  record.target_id = target_id;
  record.render_frame = render_frame;
  record.seq = seq_++;
  record.band_count = band_count_;

  // Aggregate the single-sided FFT bins into band_count_ linear bands spanning
  // [0, Nyquist], averaging power and converting to dBFS.
  const size_t n_bins = spectrum_.size();
  const float norm = 2.0f / static_cast<float>(n_fft_);
  for (uint32_t band = 0; band < band_count_; ++band) {
    const size_t begin = static_cast<size_t>(band) * n_bins / band_count_;
    size_t end = static_cast<size_t>(band + 1) * n_bins / band_count_;
    if (end <= begin) end = begin + 1;
    if (end > n_bins) end = n_bins;
    double power = 0.0;
    size_t count = 0;
    for (size_t bin = begin; bin < end; ++bin) {
      const float mag = std::abs(spectrum_[bin]) * norm;
      power += static_cast<double>(mag) * static_cast<double>(mag);
      ++count;
    }
    const double mean_power = count > 0 ? power / static_cast<double>(count) : 0.0;
    record.bands[band] =
        mean_power > 0.0 ? static_cast<float>(10.0 * std::log10(mean_power)) : -120.0f;
  }

  // Capture this target's decimated goniometer points for the vectorscope. Push
  // a bounded set this block, then read exactly those back so the record carries
  // only this target's scatter (the buffer is shared across targets per block).
  if (num_channels >= 2 && channels[0] && channels[1]) {
    const size_t want = ScopeTelemetryRecord::kMaxPoints;
    const int stride = std::max(1, num_frames / static_cast<int>(want));
    size_t pushed = 0;
    for (int i = 0; i < num_frames && pushed < want; i += stride) {
      goniometer_.push(channels[0][i], channels[1][i]);
      ++pushed;
    }
    record.point_count =
        static_cast<uint32_t>(goniometer_.read_latest(record.points.data(), pushed));
  }

  publish(record);
}

void ScopeTelemetryTap::publish(ScopeTelemetryRecord record) noexcept {
  record.dropped_records = dropped_records_;
  if (telemetry_.push(record)) {
    return;
  }
  // Queue full: account for the drop. The producer (audio thread) never pops;
  // the running count rides along on the next record that pushes successfully.
  ++dropped_records_;
}

}  // namespace sonare::engine
