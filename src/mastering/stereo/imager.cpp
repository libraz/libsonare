#include "mastering/stereo/imager.h"

#include <algorithm>
#include <cmath>

#include "mastering/stereo/constant_power_width.h"
#include "mastering/stereo/mid_side.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

using sonare::constants::kPi;

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
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  // Decorrelator tuning. The four first-order allpass break frequencies are
  // anchored at the historical 48 kHz coefficients (0.63 / -0.51 / 0.42 /
  // -0.34) and re-derived for the prepared rate via
  //   c = (1 - tan(pi*f/sr)) / (1 + tan(pi*f/sr)),
  // so the decorrelation timbre no longer depends on the sample rate. At
  // 48 kHz this reproduces the historical coefficients (within float rounding).
  static constexpr float kBreakHz[4] = {3410.4f, 19206.0f, 5924.7f, 17007.6f};
  for (size_t i = 0; i < 4; ++i) {
    // Clamp just below Nyquist so tan() stays finite at low sample rates.
    const double f = std::min(static_cast<double>(kBreakHz[i]), 0.497 * sample_rate);
    const double t = std::tan(static_cast<double>(kPi) * f / sample_rate);
    allpass_[i].coefficient = static_cast<float>((1.0 - t) / (1.0 + t));
  }
  prepared_ = true;
  reset();
}

void Imager::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Imager");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }
  if (num_channels < 2) {
    return;
  }

  const float output = db_to_linear(config_.output_gain_db);
  const float energy_scale =
      config_.preserve_energy ? constant_power_width_gain(config_.width) : 1.0f;
  for (int i = 0; i < num_samples; ++i) {
    auto ms = encode_sample(channels[0][i], channels[1][i]);
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
    // Signal-independent constant-power compensation keeps this operation
    // linear. An instantaneous original/widened energy ratio amplitude-modulates
    // every sample and creates intermodulation products.
    ms.mid *= energy_scale;
    ms.side *= energy_scale;
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

std::vector<rt::ParamDescriptor> Imager::parameter_descriptors() const {
  return {{"width", 0}, {"outputGainDb", 1}, {"decorrelationAmount", 2}};
}

void Imager::validate_config(const ImagerConfig& config) {
  if (!std::isfinite(config.width) || config.width < 0.0f || config.decorrelation_amount < 0.0f ||
      config.decorrelation_amount > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "imager width must be non-negative");
  }
}

}  // namespace sonare::mastering::stereo
