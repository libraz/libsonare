#include "mastering/final/dither.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::final {

namespace {

// 9-tap F-weighted noise-shaping filter (Lipshitz/Vanderkooy/Wannamaker
// "Minimally Audible Noise Shaping", JAES 1991). Coefficients shape the
// quantization noise to follow the inverse F-weighting curve, pushing energy
// out of the ear's most sensitive 1-5 kHz band.
constexpr std::array<float, 9> kLvNoiseShapingCoeffs = {2.412f,  -3.370f, 3.937f,  -4.174f, 3.353f,
                                                        -2.205f, 1.281f,  -0.569f, 0.0847f};

}  // namespace

Audio dither(const Audio& audio, const DitherConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (config.target_bits < 2 || config.target_bits > 32) {
    throw std::invalid_argument("target_bits must be in [2, 32]");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  if (config.type == DitherType::None) {
    return Audio::from_vector(std::move(samples), audio.sample_rate());
  }

  const float lsb = 1.0f / static_cast<float>(int64_t{1} << (config.target_bits - 1));
  std::mt19937 rng(config.seed);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);

  if (config.type == DitherType::Rpdf) {
    for (auto& sample : samples) {
      sample += dist(rng) * lsb;
    }
    return Audio::from_vector(std::move(samples), audio.sample_rate());
  }

  if (config.type == DitherType::Tpdf) {
    for (auto& sample : samples) {
      sample += (dist(rng) + dist(rng)) * lsb;
    }
    return Audio::from_vector(std::move(samples), audio.sample_rate());
  }

  // NoiseShaped: TPDF dither plus F-weighted feedback of the quantization error.
  std::array<float, 9> error_history{};
  for (auto& sample : samples) {
    float feedback = 0.0f;
    for (size_t k = 0; k < kLvNoiseShapingCoeffs.size(); ++k) {
      feedback += kLvNoiseShapingCoeffs[k] * error_history[k];
    }

    const float dithered = sample + (dist(rng) + dist(rng)) * lsb + feedback * lsb;
    // Noise-shaping feedback can push the dithered value beyond full scale;
    // clamp before quantizing so the output never leaves [-1, 1] regardless of
    // the downstream clamp setting. The error feedback uses the clamped output
    // so the shaper accounts for the clamping as part of the quantization step.
    const float clamped = std::clamp(dithered, -1.0f, 1.0f);
    // Quantize the dithered signal to the target LSB resolution.
    const float quantized = std::round(clamped / lsb) * lsb;
    const float quant_error = (dithered - quantized) / lsb;

    // Shift the error history (newest at index 0).
    for (size_t k = kLvNoiseShapingCoeffs.size() - 1; k > 0; --k) {
      error_history[k] = error_history[k - 1];
    }
    error_history[0] = quant_error;

    sample = quantized;
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::final
