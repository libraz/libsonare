#include "mastering/repair/dereverb_streaming.h"

#include <algorithm>
#include <cmath>

#include "core/window.h"
#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/repair/dereverb_internal.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

using sonare::constants::kSpectrumEpsilon;

namespace {

DereverbClassicalConfig streaming_config(const DereverbClassicalConfig& config) {
  const auto validated = Validated<DereverbClassicalConfig>::make(config);
  if (validated->wpe_enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb wpe_enabled must be false when dereverberating a stream: the "
                          "WPE predictor is fitted over the whole signal");
  }
  return validated.get();
}

}  // namespace

StreamingDereverb::StreamingDereverb(const DereverbClassicalConfig& config)
    : config_(streaming_config(config)),
      n_fft_(config_.n_fft),
      hop_length_(config_.hop_length),
      n_bins_(config_.n_fft / 2 + 1),
      prefix_(config_.n_fft / 2),
      fft_(config_.n_fft) {}

void StreamingDereverb::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void StreamingDereverb::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "StreamingDereverb");
  max_block_size_ = max_block_size;
  max_channels_ = max_channels;
  const int rate = static_cast<int>(std::lround(sample_rate));
  delay_frames_ = detail::dereverb_late_delay_frames(config_, rate);
  decay_ = detail::dereverb_late_decay(config_, delay_frames_, rate);

  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto bins = static_cast<std::size_t>(n_bins_);
  const auto channels = static_cast<std::size_t>(max_channels_);

  // The window pair Spectrogram::to_audio accumulates: periodic analysis, symmetric synthesis.
  const auto periodic = get_window_cached(WindowType::Hann, n_fft_, true);
  const auto symmetric = get_window_cached(WindowType::Hann, n_fft_, false);
  analysis_window_.assign(periodic->begin(), periodic->end());
  synthesis_window_.assign(symmetric->begin(), symmetric->end());
  window_product_.assign(fft, 0.0f);
  for (std::size_t i = 0; i < fft; ++i) {
    window_product_[i] = analysis_window_[i] * synthesis_window_[i];
  }

  input_ring_.assign(fft * channels, 0.0f);
  frame_.assign(fft, 0.0f);
  spectra_.assign(bins * channels, std::complex<float>{});
  masked_.assign(bins, std::complex<float>{});
  power_.assign(bins, 0.0f);
  gains_.assign(bins, 1.0);
  power_history_.assign(bins * static_cast<std::size_t>(delay_frames_), 0.0f);
  synthesis_ring_.assign(fft * channels, 0.0f);
  window_sum_ring_.assign(fft, 0.0f);

  queue_capacity_ = latency_samples() + max_block_size_;
  output_queue_.assign(static_cast<std::size_t>(queue_capacity_) * channels, 0.0f);
  fft_.prepare(/*real_forward=*/true, /*real_inverse=*/true, /*complex_forward=*/false);

  prepared_ = true;
  reset();
}

void StreamingDereverb::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "StreamingDereverb");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  if (num_channels > max_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared StreamingDereverb capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared StreamingDereverb block size");
  }
  if (active_channels_ == 0) active_channels_ = num_channels;
  if (num_channels != active_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingDereverb channel count must not change between resets: one "
                          "mask is built from every channel of the block at once");
  }

  const auto fft = static_cast<std::size_t>(n_fft_);
  for (int i = 0; i < num_samples; ++i) {
    for (int ch = 0; ch < num_channels; ++ch) {
      input_ring_[fft * static_cast<std::size_t>(ch) + static_cast<std::size_t>(input_write_)] =
          channels[ch][i];
    }
    input_write_ = input_write_ + 1 == n_fft_ ? 0 : input_write_ + 1;
    if (--samples_to_next_frame_ == 0) {
      analyze_frame();
      samples_to_next_frame_ = hop_length_;
    }
  }

  if (queue_size_ < num_samples) {
    throw SonareException(ErrorCode::InvalidState,
                          "StreamingDereverb output queue underran its reported latency");
  }
  const auto capacity = static_cast<std::size_t>(queue_capacity_);
  for (int i = 0; i < num_samples; ++i) {
    for (int ch = 0; ch < num_channels; ++ch) {
      channels[ch][i] = output_queue_[capacity * static_cast<std::size_t>(ch) +
                                      static_cast<std::size_t>(queue_read_)];
    }
    queue_read_ = queue_read_ + 1 == queue_capacity_ ? 0 : queue_read_ + 1;
    --queue_size_;
  }
}

void StreamingDereverb::analyze_frame() {
  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto bins = static_cast<std::size_t>(n_bins_);

  std::fill(power_.begin(), power_.end(), 0.0f);
  for (int ch = 0; ch < active_channels_; ++ch) {
    const float* ring = input_ring_.data() + fft * static_cast<std::size_t>(ch);
    // The write cursor points at the oldest sample, which is this frame's first.
    for (int i = 0; i < n_fft_; ++i) {
      const int pos = input_write_ + i < n_fft_ ? input_write_ + i : input_write_ + i - n_fft_;
      frame_[static_cast<std::size_t>(i)] =
          ring[pos] * analysis_window_[static_cast<std::size_t>(i)];
    }
    std::complex<float>* spectrum = spectra_.data() + bins * static_cast<std::size_t>(ch);
    fft_.forward(frame_.data(), spectrum);
    for (int b = 0; b < n_bins_; ++b) power_[static_cast<std::size_t>(b)] += std::norm(spectrum[b]);
  }

  // The frame delay_frames_ back, which the offline mask reads as power[t - delay].
  const bool has_late = frames_taken_ >= static_cast<std::size_t>(delay_frames_);
  float* late = power_history_.data() + bins * static_cast<std::size_t>(power_history_read_);
  for (int b = 0; b < n_bins_; ++b) {
    const auto bin = static_cast<std::size_t>(b);
    const double late_psd = has_late ? static_cast<double>(late[bin]) * decay_ : 0.0;
    bool suppressed = false;
    gains_[bin] = detail::dereverb_subtraction_gain(static_cast<double>(power_[bin]), late_psd,
                                                    config_, &suppressed);
  }
  std::copy(power_.begin(), power_.end(), late);
  power_history_read_ = power_history_read_ + 1 == delay_frames_ ? 0 : power_history_read_ + 1;
  ++frames_taken_;

  const std::size_t base = next_frame_start_ % fft;
  for (int ch = 0; ch < active_channels_; ++ch) {
    const std::complex<float>* channel = spectra_.data() + bins * static_cast<std::size_t>(ch);
    for (int b = 0; b < n_bins_; ++b) {
      const double gain = gains_[static_cast<std::size_t>(b)];
      masked_[static_cast<std::size_t>(b)] = {static_cast<float>(channel[b].real() * gain),
                                              static_cast<float>(channel[b].imag() * gain)};
    }
    fft_.inverse(masked_.data(), frame_.data());
    float* ring = synthesis_ring_.data() + fft * static_cast<std::size_t>(ch);
    for (int i = 0; i < n_fft_; ++i) {
      const std::size_t offset = base + static_cast<std::size_t>(i);
      const std::size_t pos = offset < fft ? offset : offset - fft;
      const float contribution =
          frame_[static_cast<std::size_t>(i)] * synthesis_window_[static_cast<std::size_t>(i)];
      ring[pos] += contribution;
    }
  }
  for (int i = 0; i < n_fft_; ++i) {
    const std::size_t offset = base + static_cast<std::size_t>(i);
    const std::size_t pos = offset < fft ? offset : offset - fft;
    window_sum_ring_[pos] += window_product_[static_cast<std::size_t>(i)];
  }
  next_frame_start_ += static_cast<std::size_t>(hop_length_);
  finalize_before(next_frame_start_);
}

void StreamingDereverb::finalize_before(std::size_t end) {
  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto capacity = static_cast<std::size_t>(queue_capacity_);
  while (ring_base_ < end) {
    const std::size_t pos = ring_base_ % fft;
    const float window_sum = window_sum_ring_[pos];
    // The centred padding ahead of sample zero is trimmed, as to_audio trims it.
    const bool enqueue = ring_base_ >= static_cast<std::size_t>(prefix_);
    const auto write =
        enqueue ? static_cast<std::size_t>((queue_read_ + queue_size_) % queue_capacity_) : 0;
    for (int ch = 0; ch < active_channels_; ++ch) {
      const std::size_t lane = fft * static_cast<std::size_t>(ch) + pos;
      const float raw = synthesis_ring_[lane];
      if (enqueue) {
        output_queue_[capacity * static_cast<std::size_t>(ch) + write] =
            window_sum > kSpectrumEpsilon ? raw / std::max(window_sum, kSpectrumEpsilon) : raw;
      }
      synthesis_ring_[lane] = 0.0f;
    }
    window_sum_ring_[pos] = 0.0f;
    ++ring_base_;
    if (enqueue) ++queue_size_;
  }
}

void StreamingDereverb::reset() {
  std::fill(input_ring_.begin(), input_ring_.end(), 0.0f);
  // The ring starts holding the half window of padding ahead of sample zero.
  input_write_ = prefix_;
  samples_to_next_frame_ = n_fft_ - prefix_;
  std::fill(spectra_.begin(), spectra_.end(), std::complex<float>{});
  std::fill(power_history_.begin(), power_history_.end(), 0.0f);
  power_history_read_ = 0;
  frames_taken_ = 0;
  std::fill(synthesis_ring_.begin(), synthesis_ring_.end(), 0.0f);
  std::fill(window_sum_ring_.begin(), window_sum_ring_.end(), 0.0f);
  ring_base_ = 0;
  next_frame_start_ = 0;
  std::fill(output_queue_.begin(), output_queue_.end(), 0.0f);
  queue_read_ = 0;
  queue_size_ = std::min(latency_samples(), queue_capacity_);
  active_channels_ = 0;
}

int StreamingDereverb::latency_samples() const noexcept { return n_fft_ - 1; }

int StreamingDereverb::tail_samples() const noexcept { return latency_samples(); }

}  // namespace sonare::mastering::repair
