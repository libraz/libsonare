#include "rt/oversampler.h"

#include "util/exception.h"

namespace sonare::rt {
namespace {

bool is_supported_factor(int factor) { return factor == 2 || factor == 4 || factor == 8; }

}  // namespace

Oversampler::Oversampler(int factor) { set_factor(factor); }

void Oversampler::set_factor(int factor) {
  SONARE_CHECK(is_supported_factor(factor), ErrorCode::InvalidParameter);
  factor_ = factor;
  decimation_taps_ = design_windowed_sinc_lowpass(12 * factor_, factor_, 7.85726, true);
  fir_ = build_polyphase(decimation_taps_, factor_);
}

std::vector<float> Oversampler::upsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> output(size * static_cast<size_t>(factor_));
  for (size_t i = 0; i < size; ++i) {
    for (int phase = 0; phase < factor_; ++phase) {
      output[i * static_cast<size_t>(factor_) + static_cast<size_t>(phase)] =
          interpolate_polyphase_sample(input, size, i, phase, fir_);
    }
  }
  return output;
}

std::vector<float> Oversampler::upsample(const std::vector<float>& input) const {
  return upsample(input.data(), input.size());
}

std::vector<float> Oversampler::downsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);

  const size_t out_size = size / static_cast<size_t>(factor_);
  std::vector<float> output(out_size);
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
  return output;
}

std::vector<float> Oversampler::downsample(const std::vector<float>& input) const {
  return downsample(input.data(), input.size());
}

}  // namespace sonare::rt
