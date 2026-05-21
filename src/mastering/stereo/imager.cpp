#include "mastering/stereo/imager.h"

#include <cmath>
#include <stdexcept>

#include "mastering/stereo/mid_side.h"

namespace sonare::mastering::stereo {

Imager::Imager(ImagerConfig config) : config_(config) { validate_config(config_); }

void Imager::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  prepared_ = true;
}

void Imager::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("Imager must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }
  if (num_channels < 2) {
    return;
  }

  const float output = db_to_linear(config_.output_gain_db);
  for (int i = 0; i < num_samples; ++i) {
    auto ms = encode_sample(channels[0][i], channels[1][i]);
    ms.side *= config_.width;
    const auto lr = decode_sample(ms.mid, ms.side);
    channels[0][i] = lr.mid * output;
    channels[1][i] = lr.side * output;
  }
}

void Imager::reset() {}

void Imager::set_config(const ImagerConfig& config) {
  validate_config(config);
  config_ = config;
}

void Imager::validate_config(const ImagerConfig& config) {
  if (config.width < 0.0f) {
    throw std::invalid_argument("imager width must be non-negative");
  }
}

float Imager::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace sonare::mastering::stereo
