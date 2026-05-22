#include "mastering/saturation/tube.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"
#include "util/db.h"

namespace sonare::mastering::saturation {

using sonare::constants::kTwoPi;

namespace {

struct Dempwolf12Ax7 {
  // Dempwolf & Zoelzer, "A Physically-motivated Triode Model for Circuit
  // Simulations", DAFx-11, equations (10)-(12), Table 1 first fitted 12AX7
  // system. Voltages are Vg/Va relative to cathode; fitted currents are used
  // in the same milliampere scale as the paper's figures.
  static constexpr float G = 2.242e-3f;
  static constexpr float mu = 103.2f;
  static constexpr float gamma = 1.26f;
  static constexpr float C = 3.40f;
  static constexpr float Gg = 6.177e-4f;
  static constexpr float xi = 1.314f;
  static constexpr float Cg = 9.901f;
  static constexpr float Ig0 = 8.025e-8f;
};

float smooth_positive(float c, float x) {
  const float z = c * x;
  if (z > 30.0f) return x;
  if (z < -30.0f) return std::exp(z) / c;
  return std::log1p(std::exp(z)) / c;
}

float cathode_current_ma(float vg, float va) {
  const float effective = va / Dempwolf12Ax7::mu + vg;
  return Dempwolf12Ax7::G *
         std::pow(smooth_positive(Dempwolf12Ax7::C, effective), Dempwolf12Ax7::gamma);
}

float grid_current_ma(float vg) {
  return Dempwolf12Ax7::Gg * std::pow(smooth_positive(Dempwolf12Ax7::Cg, vg), Dempwolf12Ax7::xi) +
         Dempwolf12Ax7::Ig0;
}

float plate_current_ma(float vg, float va) {
  return cathode_current_ma(vg, va) - grid_current_ma(vg);
}

}  // namespace

Tube::Tube(TubeConfig config) : tube_config_(config) {
  validate_config(tube_config_);
  if (tube_config_.oversample_factor != 1) {
    oversampler_.set_factor(tube_config_.oversample_factor);
  }
}

void Tube::set_config(const TubeConfig& config) {
  validate_config(config);
  tube_config_ = config;
  if (tube_config_.oversample_factor != 1) {
    oversampler_.set_factor(tube_config_.oversample_factor);
  }
}

void Tube::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  prepared_ = true;
  reset();
}

void Tube::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("Tube must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    if (tube_config_.oversample_factor == 1) {
      for (int i = 0; i < num_samples; ++i) {
        const float wet = apply_miller_filter(ch, process_model(channels[ch][i], tube_config_));
        channels[ch][i] = channels[ch][i] * (1.0f - tube_config_.mix) + wet * tube_config_.mix;
      }
      continue;
    }

    const auto upsampled = oversampler_.upsample(channels[ch], static_cast<size_t>(num_samples));
    scratch_.resize(upsampled.size());
    for (size_t i = 0; i < upsampled.size(); ++i) {
      scratch_[i] = process_model(upsampled[i], tube_config_);
    }
    const auto downsampled = oversampler_.downsample(scratch_);
    for (int i = 0; i < num_samples; ++i) {
      const float wet = apply_miller_filter(ch, downsampled[static_cast<size_t>(i)]);
      channels[ch][i] = channels[ch][i] * (1.0f - tube_config_.mix) + wet * tube_config_.mix;
    }
  }
}

void Tube::reset() {
  scratch_.clear();
  std::fill(miller_state_.begin(), miller_state_.end(), 0.0f);
}

float Tube::process_model(float sample, const TubeConfig& config) {
  const float drive = db_to_linear(config.drive_db);
  const float grid_bias_v = config.bias_v + config.bias * 2.0f;
  const float grid_signal_v = sample * drive * 1.2f;
  const float plate_v = 250.0f;
  const float idle = plate_current_ma(grid_bias_v, plate_v);
  const float current_delta = plate_current_ma(grid_bias_v + grid_signal_v, plate_v) - idle;
  return std::tanh(current_delta * 2.5f);
}

void Tube::validate_config(const TubeConfig& config) {
  if (config.mix < 0.0f || config.mix > 1.0f || !std::isfinite(config.bias_v) ||
      config.oversample_factor < 1 ||
      (config.oversample_factor != 1 && config.oversample_factor != 2 &&
       config.oversample_factor != 4 && config.oversample_factor != 8)) {
    throw std::invalid_argument("tube mix must be in [0, 1]");
  }
}

void Tube::ensure_state(int num_channels) {
  if (miller_state_.size() != static_cast<size_t>(num_channels)) {
    miller_state_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
}

float Tube::apply_miller_filter(int channel, float sample) {
  // Dempwolf/Zoelzer DAFx-11 section 5.6 lists 12AX7 parasitic capacitances
  // (Cak=0.9 pF, Cgk=2.3 pF, Cag=2.4 pF). We approximate their Miller effect
  // as a drive-dependent one-pole low-pass in the audio output path.
  const float drive = db_to_linear(tube_config_.drive_db);
  const float cutoff = std::clamp(22000.0f / (1.0f + 0.08f * drive), 2500.0f,
                                  static_cast<float>(sample_rate_ * 0.45));
  const float coeff =
      1.0f - std::exp(-kTwoPi * cutoff /
                      static_cast<float>(sample_rate_));
  auto& state = miller_state_[static_cast<size_t>(channel)];
  state += coeff * (sample - state);
  return state;
}

}  // namespace sonare::mastering::saturation
