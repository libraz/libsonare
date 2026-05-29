#include "mastering/stereo/imager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "mastering/stereo/mid_side.h"
#include "util/db.h"

namespace sonare::mastering::stereo {

float Imager::Allpass::process(float input) noexcept {
  const float output = -coefficient * input + x1 + coefficient * y1;
  x1 = input;
  y1 = output;
  return output;
}

void Imager::Allpass::reset() noexcept {
  x1 = 0.0f;
  y1 = 0.0f;
}

Imager::Imager(ImagerConfig config) : config_(config) { validate_config(config_); }

void Imager::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  allpass_[0].coefficient = 0.63f;
  allpass_[1].coefficient = -0.51f;
  allpass_[2].coefficient = 0.42f;
  allpass_[3].coefficient = -0.34f;
  prepared_ = true;
  reset();
}

void Imager::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Imager");
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
    const float original_energy = ms.mid * ms.mid + ms.side * ms.side;
    float decorated_side = ms.side;
    for (auto& stage : allpass_) {
      decorated_side = stage.process(decorated_side);
    }
    ms.side = ms.side * config_.width;
    if (config_.decorrelation_amount > 0.0f && config_.width > 1.0f) {
      const float extra_width = std::min(config_.width - 1.0f, 1.0f);
      const float mix = config_.decorrelation_amount * extra_width;
      ms.side = (1.0f - mix) * ms.side + mix * decorated_side * config_.width;
    }
    if (config_.preserve_energy && config_.width > 1.0f) {
      const float widened_energy = ms.mid * ms.mid + ms.side * ms.side;
      if (widened_energy > 0.0f && original_energy > 0.0f) {
        const float scale = std::sqrt(original_energy / widened_energy);
        ms.mid *= scale;
        ms.side *= scale;
      }
    }
    const auto lr = decode_sample(ms.mid, ms.side);
    channels[0][i] = lr.mid * output;
    channels[1][i] = lr.side * output;
  }
}

void Imager::reset() {
  for (auto& stage : allpass_) {
    stage.reset();
  }
}

void Imager::set_config(const ImagerConfig& config) {
  validate_config(config);
  config_ = config;
}

bool Imager::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.width = std::max(0.0f, value);
      return true;
    case 1:
      config_.output_gain_db = value;
      return true;
    case 2:
      config_.decorrelation_amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void Imager::validate_config(const ImagerConfig& config) {
  if (config.width < 0.0f || config.decorrelation_amount < 0.0f ||
      config.decorrelation_amount > 1.0f) {
    throw std::invalid_argument("imager width must be non-negative");
  }
}

}  // namespace sonare::mastering::stereo
