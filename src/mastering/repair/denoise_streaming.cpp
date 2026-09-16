#include "mastering/repair/denoise_streaming.h"

#include <algorithm>
#include <cmath>

#include "core/window.h"
#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

using sonare::constants::kSpectrumEpsilon;

namespace {

/// @brief Validates @p config against every offline rule plus the streaming one.
/// @details Quantile is refused rather than swapped for a recursive estimator:
///   a substituted value becomes an in-domain value nothing downstream can
///   separate from a deliberate one.
DenoiseClassicalConfig streaming_config(const DenoiseClassicalConfig& config) {
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);
  if (validated->noise_estimator == DenoiseNoiseEstimator::Quantile) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "denoise noise_estimator must not be Quantile when denoising a stream: it ranks every "
        "frame of the whole signal by energy, which a stream never reaches the end of. Set "
        "noise_estimator to Mcra, Imcra or Spp, the three that track recursively.");
  }
  return validated.get();
}

}  // namespace

StreamingDenoise::StreamingDenoise(const DenoiseClassicalConfig& config)
    : config_(streaming_config(config)),
      n_fft_(config_.n_fft),
      hop_length_(config_.hop_length),
      n_bins_(config_.n_fft / 2 + 1),
      fft_(config_.n_fft) {
  // Built here rather than in prepare() so latency_samples() answers from the
  // configuration alone, which is what a host asks before it prepares anything.
  stage_ = std::make_unique<detail::GainStage>(n_bins_, config_);
  mask_latency_frames_ = stage_->latency();
  samples_to_next_frame_ = n_fft_;
}

void StreamingDenoise::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void StreamingDenoise::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "max_channels exceeds StreamingDenoise capacity");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_channels_ = max_channels;

  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto bins = static_cast<std::size_t>(n_bins_);
  const auto channels = static_cast<std::size_t>(max_channels_);

  // Analysis periodic, synthesis symmetric, normalizer their product: the pair
  // Spectrogram::to_audio accumulates. win_length is n_fft here, so neither
  // window needs the zero padding a shorter one would sit in.
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
  held_spectra_.assign(bins * channels, std::complex<float>{});
  masked_.assign(bins, std::complex<float>{});
  power_f_.assign(bins, 0.0f);
  power_d_.assign(bins, 0.0);
  channel_power_f_.assign(bins, 0.0f);
  channel_power_d_.assign(bins, 0.0);
  tracker_input_.assign(bins, 0.0f);
  noise_frame_.assign(bins, 0.0);
  synthesis_ring_.assign(fft * channels, 0.0f);
  window_sum_ring_.assign(fft, 0.0f);

  // The queue holds the reported delay plus at most one block, which is the
  // most that can sit in it: a block's own output is queued before it drains.
  queue_capacity_ = latency_samples() + max_block_size_;
  output_queue_.assign(static_cast<std::size_t>(queue_capacity_) * channels, 0.0f);

  tracker_ = std::make_unique<common::NoiseTracker>(
      n_bins_, static_cast<int>(std::lround(sample_rate_)),
      detail::tracker_mode_for(config_.noise_estimator), hop_length_);
  // A transform kind builds its backend state on its first use; build both here
  // so no process() block is the one that pays for it.
  fft_.prepare(/*real_forward=*/true, /*real_inverse=*/true, /*complex_forward=*/false);

  prepared_ = true;
  reset();
}

void StreamingDenoise::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "StreamingDenoise");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  if (num_channels > max_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared StreamingDenoise capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared StreamingDenoise block size");
  }
  if (active_channels_ == 0) active_channels_ = num_channels;
  if (num_channels != active_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingDenoise channel count must not change between resets: one "
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

  // Reads the arithmetic above rather than trusting it: an underrun here would
  // otherwise leave the host a block of stale samples and a silent desync.
  if (queue_size_ < num_samples) {
    throw SonareException(ErrorCode::InvalidState,
                          "StreamingDenoise output queue underran its reported latency");
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

void StreamingDenoise::analyze_frame() {
  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto bins = static_cast<std::size_t>(n_bins_);

  std::fill(power_f_.begin(), power_f_.end(), 0.0f);
  std::fill(power_d_.begin(), power_d_.end(), 0.0);
  for (int ch = 0; ch < active_channels_; ++ch) {
    const float* ring = input_ring_.data() + fft * static_cast<std::size_t>(ch);
    // The write cursor points at the oldest ring sample, which is this frame's
    // first: the frame is taken the moment the ring holds exactly its span.
    for (int i = 0; i < n_fft_; ++i) {
      const int pos = input_write_ + i < n_fft_ ? input_write_ + i : input_write_ + i - n_fft_;
      frame_[static_cast<std::size_t>(i)] =
          ring[pos] * analysis_window_[static_cast<std::size_t>(i)];
    }
    std::complex<float>* spectrum = spectra_.data() + bins * static_cast<std::size_t>(ch);
    fft_.forward(frame_.data(), spectrum);
    detail::frame_powers(spectrum, n_bins_, channel_power_f_.data(), channel_power_d_.data());
    for (int b = 0; b < n_bins_; ++b) {
      power_f_[static_cast<std::size_t>(b)] += channel_power_f_[static_cast<std::size_t>(b)];
      power_d_[static_cast<std::size_t>(b)] += channel_power_d_[static_cast<std::size_t>(b)];
    }
  }

  for (int b = 0; b < n_bins_; ++b) {
    tracker_input_[static_cast<std::size_t>(b)] =
        std::max(power_f_[static_cast<std::size_t>(b)], 0.0f);
  }
  tracker_->update(tracker_input_.data());
  const float* tracked = tracker_->noise_psd();
  for (int b = 0; b < n_bins_; ++b) noise_frame_[static_cast<std::size_t>(b)] = tracked[b];

  const double* gains = stage_->push(power_d_.data(), noise_frame_.data());
  const bool lagged = mask_latency_frames_ != 0;
  if (gains != nullptr) emit_frame(gains, lagged ? held_spectra_.data() : spectra_.data());
  if (lagged) {
    const auto span =
        static_cast<std::ptrdiff_t>(bins * static_cast<std::size_t>(active_channels_));
    std::copy(spectra_.begin(), spectra_.begin() + span, held_spectra_.begin());
  }
}

void StreamingDenoise::emit_frame(const double* gains, const std::complex<float>* source) {
  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto bins = static_cast<std::size_t>(n_bins_);
  const std::size_t base = next_frame_start_ % fft;

  for (int ch = 0; ch < active_channels_; ++ch) {
    const std::complex<float>* channel = source + bins * static_cast<std::size_t>(ch);
    for (int b = 0; b < n_bins_; ++b) {
      masked_[static_cast<std::size_t>(b)] = {static_cast<float>(channel[b].real() * gains[b]),
                                              static_cast<float>(channel[b].imag() * gains[b])};
    }
    fft_.inverse(masked_.data(), frame_.data());
    float* ring = synthesis_ring_.data() + fft * static_cast<std::size_t>(ch);
    for (int i = 0; i < n_fft_; ++i) {
      const std::size_t offset = base + static_cast<std::size_t>(i);
      const std::size_t pos = offset < fft ? offset : offset - fft;
      // Kept as two statements so the multiply and the add stay unfused, as they
      // are in IstftAccumulator and in to_audio's Eigen expression before it.
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
  // No later frame starts below the next frame's start, so one hop is now final.
  finalize_before(next_frame_start_);
}

void StreamingDenoise::finalize_before(std::size_t end) {
  const auto fft = static_cast<std::size_t>(n_fft_);
  const auto capacity = static_cast<std::size_t>(queue_capacity_);
  while (ring_base_ < end) {
    const std::size_t pos = ring_base_ % fft;
    const float window_sum = window_sum_ring_[pos];
    const auto write = static_cast<std::size_t>((queue_read_ + queue_size_) % queue_capacity_);
    for (int ch = 0; ch < active_channels_; ++ch) {
      const std::size_t lane = fft * static_cast<std::size_t>(ch) + pos;
      const float raw = synthesis_ring_[lane];
      // to_audio's select verbatim: the guard is a strict > eps and the divisor
      // is still clamped, so a lane that fails it keeps its un-normalized sample.
      output_queue_[capacity * static_cast<std::size_t>(ch) + write] =
          window_sum > kSpectrumEpsilon ? raw / std::max(window_sum, kSpectrumEpsilon) : raw;
      synthesis_ring_[lane] = 0.0f;
    }
    window_sum_ring_[pos] = 0.0f;
    ++ring_base_;
    ++queue_size_;
  }
}

void StreamingDenoise::reset() {
  std::fill(input_ring_.begin(), input_ring_.end(), 0.0f);
  input_write_ = 0;
  samples_to_next_frame_ = n_fft_;
  std::fill(spectra_.begin(), spectra_.end(), std::complex<float>{});
  std::fill(held_spectra_.begin(), held_spectra_.end(), std::complex<float>{});
  std::fill(synthesis_ring_.begin(), synthesis_ring_.end(), 0.0f);
  std::fill(window_sum_ring_.begin(), window_sum_ring_.end(), 0.0f);
  ring_base_ = 0;
  next_frame_start_ = 0;
  std::fill(output_queue_.begin(), output_queue_.end(), 0.0f);
  queue_read_ = 0;
  // The delay a host compensates for starts as that many zeros already queued,
  // so every block finds a block's worth waiting from the first one onwards.
  queue_size_ = std::min(latency_samples(), queue_capacity_);
  active_channels_ = 0;
  if (tracker_ != nullptr) tracker_->reset();
  // GainStage carries no reset of its own; rebuilding is what returns the
  // decision-directed recursion and the median smoother to frame zero.
  stage_ = std::make_unique<detail::GainStage>(n_bins_, config_);
}

int StreamingDenoise::latency_samples() const noexcept {
  return n_fft_ - 1 + mask_latency_frames_ * hop_length_;
}

int StreamingDenoise::tail_samples() const noexcept { return latency_samples(); }

}  // namespace sonare::mastering::repair
