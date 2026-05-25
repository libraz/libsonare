#include "rt/true_peak_filter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::rt {
namespace {

// Self-designed Kaiser-windowed-sinc polyphase low-pass filters for true-peak
// interpolation. ITU-R BS.1770 frames its coefficient table as merely an
// example and invites deriving equivalent coefficients matching the target
// frequency response, so no copyrighted literal table is shipped here.
//
// Each factor keeps 12 taps per phase (total = factor * 12). The factor-2 path
// retains its original Kaiser beta; the higher oversampling factors use a
// slightly larger beta (9.5) which trades a marginally wider transition band
// for deeper stop-band attenuation, preventing aliased images from inflating
// the reconstructed inter-sample peaks at 4x/8x.
PolyphaseFir make_true_peak_fir(int factor) {
  constexpr double kKaiserBeta = 9.5;
  if (factor == 2) return design_polyphase_lowpass(2, 24, 7.85726, true);
  if (factor == 4) return design_polyphase_lowpass(4, 48, kKaiserBeta, true);
  if (factor == 8) return design_polyphase_lowpass(8, 96, kKaiserBeta, true);
  throw std::invalid_argument("TruePeakFilter factor must be 2, 4, or 8");
}

void validate_buffers(const float* const* input, int num_channels, int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (input == nullptr) {
    throw std::invalid_argument("input must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (input[ch] == nullptr) {
      throw std::invalid_argument("input channel must not be null");
    }
  }
}

}  // namespace

TruePeakFilter::TruePeakFilter(int num_channels, int factor)
    : factor_(factor), fir_(make_true_peak_fir(factor)) {
  if (num_channels < 0) {
    throw std::invalid_argument("num_channels must be non-negative");
  }
}

float TruePeakFilter::process(const float* const* input, int num_channels, int num_samples) const {
  validate_buffers(input, num_channels, num_samples);
  float peak = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
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
    throw std::invalid_argument("output must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw std::invalid_argument("output channel must not be null");
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
                                           int num_samples,
                                           std::vector<std::vector<float>>& history) const {
  std::vector<std::vector<float>> scratch;
  upsample_with_history(input, output_oversampled, num_channels, num_samples, history, scratch);
}

void TruePeakFilter::upsample_with_history(const float* const* input,
                                           float* const* output_oversampled, int num_channels,
                                           int num_samples,
                                           std::vector<std::vector<float>>& history,
                                           std::vector<std::vector<float>>& scratch) const {
  validate_buffers(input, num_channels, num_samples);
  if (num_channels == 0 || num_samples == 0) return;
  if (output_oversampled == nullptr) {
    throw std::invalid_argument("output must not be null");
  }
  const size_t history_size = static_cast<size_t>(std::max(0, fir_.taps_per_phase));
  if (history.size() != static_cast<size_t>(num_channels)) {
    history.assign(static_cast<size_t>(num_channels), std::vector<float>(history_size, 0.0f));
  }
  if (scratch.size() != static_cast<size_t>(num_channels)) {
    scratch.assign(static_cast<size_t>(num_channels), {});
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw std::invalid_argument("output channel must not be null");
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

}  // namespace sonare::rt
