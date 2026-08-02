#include "rt/oversampler.h"

#include <algorithm>

#include "rt/true_peak_fir.h"
#include "util/exception.h"

namespace sonare::rt {
Oversampler::Oversampler(int factor, int taps_per_phase) : taps_per_phase_(taps_per_phase) {
  SONARE_CHECK(taps_per_phase_ > 0, ErrorCode::InvalidParameter);
  set_factor(factor);
}

void Oversampler::set_factor(int factor) {
  SONARE_CHECK(is_supported_polyphase_oversample_factor(factor), ErrorCode::InvalidParameter);
  factor_ = factor;
  if (factor_ == 1) {
    decimation_taps_ = {1.0f};
    fir_ = {};
    return;
  }
  decimation_taps_ =
      design_windowed_sinc_lowpass(taps_per_phase_ * factor_, factor_, 7.85726, true);
  fir_ = build_polyphase(decimation_taps_, factor_);
}

std::vector<float> Oversampler::upsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> output(size * static_cast<size_t>(factor_));
  upsample_to(input, size, output.data(), output.size());
  return output;
}

std::vector<float> Oversampler::upsample(const std::vector<float>& input) const {
  return upsample(input.data(), input.size());
}

void Oversampler::upsample_to(const float* input, size_t size, float* output,
                              size_t output_size) const {
  if (size == 0) return;
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(output != nullptr, ErrorCode::InvalidParameter);
  const size_t out_size = size * static_cast<size_t>(factor_);
  SONARE_CHECK(output_size >= out_size, ErrorCode::InvalidParameter);
  for (size_t i = 0; i < size; ++i) {
    for (int phase = 0; phase < factor_; ++phase) {
      output[i * static_cast<size_t>(factor_) + static_cast<size_t>(phase)] =
          interpolate_polyphase_sample(input, size, i, phase, fir_);
    }
  }
}

std::vector<float> Oversampler::downsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);

  const size_t out_size = size / static_cast<size_t>(factor_);
  std::vector<float> output(out_size);
  downsample_to(input, size, output.data(), output.size());
  return output;
}

void Oversampler::downsample_to(const float* input, size_t size, float* output,
                                size_t output_size) const {
  if (size == 0) return;
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(output != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);
  const size_t out_size = size / static_cast<size_t>(factor_);
  SONARE_CHECK(output_size >= out_size, ErrorCode::InvalidParameter);
  if (factor_ == 1) {
    std::copy_n(input, out_size, output);
    return;
  }
  const int half = static_cast<int>(decimation_taps_.size() / 2);
  for (size_t i = 0; i < out_size; ++i) {
    const long center = static_cast<long>(i * static_cast<size_t>(factor_));
    double accum = 0.0;
    for (size_t tap = 0; tap < decimation_taps_.size(); ++tap) {
      const long src = center + static_cast<long>(tap) - static_cast<long>(half);
      if (src < 0 || src >= static_cast<long>(size)) {
        continue;
      }
      accum += static_cast<double>(input[static_cast<size_t>(src)]) *
               static_cast<double>(decimation_taps_[tap]);
    }
    // The decimation taps share the interpolation kernel (DC gain = factor_), so
    // dividing by factor_ normalizes the decimated output to unity DC gain.
    output[i] = static_cast<float>(accum / static_cast<double>(factor_));
  }
}

void Oversampler::prepare_streaming(StreamingState* state, size_t max_input_samples) const {
  SONARE_CHECK(state != nullptr, ErrorCode::InvalidParameter);
  const size_t up_history_size = static_cast<size_t>(std::max(0, fir_.taps_per_phase));
  const size_t down_history_size = decimation_taps_.size();
  state->up_history.assign(up_history_size, 0.0f);
  state->up_scratch.assign(up_history_size + max_input_samples, 0.0f);
  const size_t max_oversampled = max_input_samples * static_cast<size_t>(factor_);
  state->down_history.assign(down_history_size, 0.0f);
  state->down_scratch.assign(down_history_size + max_oversampled, 0.0f);
}

void Oversampler::reset_streaming(StreamingState* state) const noexcept {
  if (!state) return;
  std::fill(state->up_history.begin(), state->up_history.end(), 0.0f);
  std::fill(state->up_scratch.begin(), state->up_scratch.end(), 0.0f);
  std::fill(state->down_history.begin(), state->down_history.end(), 0.0f);
  std::fill(state->down_scratch.begin(), state->down_scratch.end(), 0.0f);
}

void Oversampler::upsample_to_streaming(const float* input, size_t size, float* output,
                                        size_t output_size, StreamingState* state) const {
  if (size == 0) return;
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(output != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(state != nullptr, ErrorCode::InvalidParameter);
  const size_t out_size = size * static_cast<size_t>(factor_);
  SONARE_CHECK(output_size >= out_size, ErrorCode::InvalidParameter);
  if (factor_ == 1) {
    std::copy_n(input, size, output);
    return;
  }

  const size_t history_size = static_cast<size_t>(fir_.taps_per_phase);
  SONARE_CHECK(
      state->up_history.size() == history_size && state->up_scratch.size() >= history_size + size,
      ErrorCode::InvalidParameter);
  std::copy(state->up_history.begin(), state->up_history.end(), state->up_scratch.begin());
  std::copy_n(input, size, state->up_scratch.begin() + static_cast<std::ptrdiff_t>(history_size));
  const int delay = latency_samples();
  const size_t extended_size = history_size + size;
  for (size_t i = 0; i < size; ++i) {
    const size_t index = history_size + i - static_cast<size_t>(delay);
    for (int phase = 0; phase < factor_; ++phase) {
      output[i * static_cast<size_t>(factor_) + static_cast<size_t>(phase)] =
          interpolate_polyphase_sample(state->up_scratch.data(), extended_size, index, phase, fir_);
    }
  }
  std::copy(state->up_scratch.begin() + static_cast<std::ptrdiff_t>(size),
            state->up_scratch.begin() + static_cast<std::ptrdiff_t>(history_size + size),
            state->up_history.begin());
}

void Oversampler::downsample_to_streaming(const float* input, size_t size, float* output,
                                          size_t output_size, StreamingState* state) const {
  if (size == 0) return;
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(output != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(state != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);
  const size_t out_size = size / static_cast<size_t>(factor_);
  SONARE_CHECK(output_size >= out_size, ErrorCode::InvalidParameter);
  if (factor_ == 1) {
    std::copy_n(input, out_size, output);
    return;
  }

  const size_t history_size = decimation_taps_.size();
  SONARE_CHECK(state->down_history.size() == history_size &&
                   state->down_scratch.size() >= history_size + size,
               ErrorCode::InvalidParameter);
  std::copy(state->down_history.begin(), state->down_history.end(), state->down_scratch.begin());
  std::copy_n(input, size, state->down_scratch.begin() + static_cast<std::ptrdiff_t>(history_size));
  const size_t half = history_size / 2;
  for (size_t i = 0; i < out_size; ++i) {
    const size_t center = history_size + i * static_cast<size_t>(factor_) - half;
    double accum = 0.0;
    for (size_t tap = 0; tap < decimation_taps_.size(); ++tap) {
      const size_t src = center + tap - half;
      accum += static_cast<double>(state->down_scratch[src]) *
               static_cast<double>(decimation_taps_[tap]);
    }
    output[i] = static_cast<float>(accum / static_cast<double>(factor_));
  }
  std::copy(state->down_scratch.begin() + static_cast<std::ptrdiff_t>(size),
            state->down_scratch.begin() + static_cast<std::ptrdiff_t>(history_size + size),
            state->down_history.begin());
}

std::vector<float> Oversampler::downsample(const std::vector<float>& input) const {
  return downsample(input.data(), input.size());
}

}  // namespace sonare::rt
