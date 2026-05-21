#include "mastering/saturation/waveshaper.h"

#include <cmath>
#include <stdexcept>

namespace sonare::mastering::saturation {

namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

Waveshaper::Waveshaper(WaveshaperConfig config) : config_(config) { validate_config(config_); }

void Waveshaper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
}

void Waveshaper::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("Waveshaper must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) channels[ch][i] = shape(channels[ch][i], config_);
  }
}

void Waveshaper::reset() {}

void Waveshaper::set_config(const WaveshaperConfig& config) {
  validate_config(config);
  config_ = config;
}

float Waveshaper::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float Waveshaper::shape(float sample, const WaveshaperConfig& config) {
  const float driven = sample * db_to_linear(config.drive_db) + config.bias;
  float wet = driven;
  switch (config.curve) {
    case WaveshaperCurve::Tanh:
      wet = std::tanh(driven);
      break;
    case WaveshaperCurve::Arctan:
      wet = (2.0f / kPi) * std::atan(driven);
      break;
    case WaveshaperCurve::Asymmetric:
      wet = std::tanh(driven + 0.35f * driven * driven);
      break;
  }
  wet *= db_to_linear(config.output_gain_db);
  return sample * (1.0f - config.mix) + wet * config.mix;
}

void Waveshaper::validate_config(const WaveshaperConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("waveshaper mix must be in [0, 1]");
  }
}

}  // namespace sonare::mastering::saturation
