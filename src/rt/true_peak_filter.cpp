#include "rt/true_peak_filter.h"

#include <algorithm>
#include <cmath>

#include "rt/true_peak_fir.h"
#include "util/exception.h"

namespace sonare::rt {
namespace {

void validate_buffers(const float* const* input, int num_channels, int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (input == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "input must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (input[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "input channel must not be null");
    }
  }
}

}  // namespace

TruePeakFilter::TruePeakFilter(int num_channels, int factor)
    : factor_(factor), fir_(true_peak_fir_for(factor)) {
  if (num_channels < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "num_channels must be non-negative");
  }
  prepare(num_channels, 0);
}

void TruePeakFilter::prepare(int num_channels, int max_block_size) {
  const size_t channels = static_cast<size_t>(std::max(0, num_channels));
  const size_t history_size = static_cast<size_t>(std::max(0, fir_.taps_per_phase));
  const size_t extended_size = history_size + static_cast<size_t>(std::max(0, max_block_size));
  internal_history_.assign(channels, std::vector<float>(history_size, 0.0f));
  internal_scratch_.assign(channels, std::vector<float>(extended_size, 0.0f));
}

void TruePeakFilter::reset() noexcept {
  for (auto& channel : internal_history_) std::fill(channel.begin(), channel.end(), 0.0f);
  for (auto& channel : internal_scratch_) std::fill(channel.begin(), channel.end(), 0.0f);
}

float TruePeakFilter::process(const float* const* input, int num_channels, int num_samples) const {
  validate_buffers(input, num_channels, num_samples);
  float peak = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      peak = std::max(peak, std::abs(input[ch][i]));
      for (int phase = 0; phase < factor_; ++phase) {
        const float sample = interpolate_polyphase_sample(
            input[ch], static_cast<size_t>(num_samples), static_cast<size_t>(i), phase, fir_);
        peak = std::max(peak, std::abs(sample));
      }
    }
  }
  return peak;
}

void TruePeakFilter::upsample(const float* const* input, float* const* output_oversampled,
                              int num_channels, int num_samples) const {
  validate_buffers(input, num_channels, num_samples);
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (output_oversampled == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "output must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "output channel must not be null");
    }
    for (int i = 0; i < num_samples; ++i) {
      for (int phase = 0; phase < factor_; ++phase) {
        output_oversampled[ch][i * factor_ + phase] = interpolate_polyphase_sample(
            input[ch], static_cast<size_t>(num_samples), static_cast<size_t>(i), phase, fir_);
      }
    }
  }
}

void TruePeakFilter::upsample_with_history(const float* const* input,
                                           float* const* output_oversampled, int num_channels,
                                           int num_samples) const {
  // Fully internal path: history and scratch are member-owned and pre-sized by
  // prepare(), so this is allocation-free on the audio thread for any channel
  // count / block size within the prepared bounds.
  upsample_with_history(input, output_oversampled, num_channels, num_samples, internal_history_,
                        internal_scratch_);
}

void TruePeakFilter::upsample_with_history(const float* const* input,
                                           float* const* output_oversampled, int num_channels,
                                           int num_samples,
                                           std::vector<std::vector<float>>& history) const {
  // Route through the scratch-aware overload using a member-backed scratch so
  // this audio-thread-callable path performs no per-call allocation once the
  // scratch has grown to the working size.
  upsample_with_history(input, output_oversampled, num_channels, num_samples, history,
                        internal_scratch_);
}

void TruePeakFilter::upsample_with_history(const float* const* input,
                                           float* const* output_oversampled, int num_channels,
                                           int num_samples,
                                           std::vector<std::vector<float>>& history,
                                           std::vector<std::vector<float>>& scratch) const {
  validate_buffers(input, num_channels, num_samples);
  if (num_channels == 0 || num_samples == 0) return;
  if (output_oversampled == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "output must not be null");
  }
  const size_t history_size = static_cast<size_t>(std::max(0, fir_.taps_per_phase));
  const size_t requested_channels = static_cast<size_t>(num_channels);
  if (history.size() < requested_channels) {
    history.resize(requested_channels, std::vector<float>(history_size, 0.0f));
  }
  if (scratch.size() < requested_channels) {
    scratch.resize(requested_channels);
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "output channel must not be null");
    }
    auto& channel_history = history[static_cast<size_t>(ch)];
    if (channel_history.size() != history_size) {
      channel_history.assign(history_size, 0.0f);
    }

    auto& extended = scratch[static_cast<size_t>(ch)];
    const size_t extended_size = history_size + static_cast<size_t>(num_samples);
    if (extended.size() < extended_size) {
      extended.resize(extended_size, 0.0f);
    }
    std::copy(channel_history.begin(), channel_history.end(), extended.begin());
    std::copy(input[ch], input[ch] + num_samples,
              extended.begin() + static_cast<std::ptrdiff_t>(history_size));
    for (int i = 0; i < num_samples; ++i) {
      const size_t index = history_size + static_cast<size_t>(i);
      for (int phase = 0; phase < factor_; ++phase) {
        output_oversampled[ch][i * factor_ + phase] =
            interpolate_polyphase_sample(extended.data(), extended_size, index, phase, fir_);
      }
    }

    const size_t keep = std::min(history_size, extended_size);
    std::copy(extended.begin() + static_cast<std::ptrdiff_t>(extended_size - keep),
              extended.begin() + static_cast<std::ptrdiff_t>(extended_size),
              channel_history.end() - static_cast<std::ptrdiff_t>(keep));
    if (keep < history_size) {
      std::fill(channel_history.begin(), channel_history.end() - static_cast<std::ptrdiff_t>(keep),
                0.0f);
    }
  }
}

void TruePeakFilter::upsample_with_history_delayed(const float* const* input,
                                                   float* const* output_oversampled,
                                                   int num_channels, int num_samples,
                                                   std::vector<std::vector<float>>& history,
                                                   std::vector<std::vector<float>>& scratch) const {
  validate_buffers(input, num_channels, num_samples);
  if (num_channels == 0 || num_samples == 0) return;
  if (output_oversampled == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "output must not be null");
  }
  const size_t history_size = static_cast<size_t>(std::max(0, fir_.taps_per_phase));
  const size_t requested_channels = static_cast<size_t>(num_channels);
  if (history.size() < requested_channels) {
    history.resize(requested_channels, std::vector<float>(history_size, 0.0f));
  }
  if (scratch.size() < requested_channels) scratch.resize(requested_channels);

  const size_t delay = static_cast<size_t>(latency_samples());
  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "output channel must not be null");
    }
    auto& channel_history = history[static_cast<size_t>(ch)];
    if (channel_history.size() != history_size) channel_history.assign(history_size, 0.0f);
    auto& extended = scratch[static_cast<size_t>(ch)];
    const size_t extended_size = history_size + static_cast<size_t>(num_samples);
    if (extended.size() < extended_size) extended.resize(extended_size, 0.0f);
    std::copy(channel_history.begin(), channel_history.end(), extended.begin());
    std::copy(input[ch], input[ch] + num_samples,
              extended.begin() + static_cast<std::ptrdiff_t>(history_size));
    for (int i = 0; i < num_samples; ++i) {
      const size_t index = history_size + static_cast<size_t>(i) - delay;
      for (int phase = 0; phase < factor_; ++phase) {
        output_oversampled[ch][i * factor_ + phase] =
            interpolate_polyphase_sample(extended.data(), extended_size, index, phase, fir_);
      }
    }
    const size_t keep = std::min(history_size, extended_size);
    std::copy(extended.begin() + static_cast<std::ptrdiff_t>(extended_size - keep),
              extended.begin() + static_cast<std::ptrdiff_t>(extended_size),
              channel_history.end() - static_cast<std::ptrdiff_t>(keep));
    if (keep < history_size) {
      std::fill(channel_history.begin(), channel_history.end() - static_cast<std::ptrdiff_t>(keep),
                0.0f);
    }
  }
}

}  // namespace sonare::rt
