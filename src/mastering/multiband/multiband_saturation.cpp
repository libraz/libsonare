#include "mastering/multiband/multiband_saturation.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"

namespace sonare::mastering::multiband {

MultibandSaturation::MultibandSaturation(MultibandSaturationConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
}

void MultibandSaturation::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_);
  reset();
}

void MultibandSaturation::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) {
    throw std::logic_error("MultibandSaturation must be prepared before processing");
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

  auto split = crossover_.split(channels, num_channels, num_samples);
  for (int band = 0; band < split.num_bands(); ++band) {
    const auto& band_config = config_.bands[static_cast<size_t>(band)];
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& band_samples = split.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (auto& sample : band_samples) {
        sample = saturate_sample(sample, band_config);
      }
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
    for (int band = 0; band < split.num_bands(); ++band) {
      const auto& band_samples = split.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] += band_samples[static_cast<size_t>(i)];
      }
    }
  }
}

void MultibandSaturation::reset() { crossover_.reset(); }

void MultibandSaturation::set_config(const MultibandSaturationConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

bool MultibandSaturation::set_parameter(unsigned int param_id, float value) {
  const size_t band = param_id / kBandStride;
  if (band >= config_.bands.size()) {
    return false;
  }
  auto& band_config = config_.bands[band];
  switch (param_id % kBandStride) {
    case 0:
      band_config.drive_db = value;
      return true;
    case 1:
      band_config.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 2:
      band_config.output_gain_db = value;
      return true;
    default:
      return false;
  }
}

void MultibandSaturation::validate_config(const MultibandSaturationConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw std::invalid_argument("multiband saturation band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.mix < 0.0f || band.mix > 1.0f) {
      throw std::invalid_argument("saturation mix must be in [0, 1]");
    }
  }
}

float MultibandSaturation::saturate_sample(float sample, const SaturationBandConfig& config) {
  if (!config.enabled || config.mix == 0.0f) {
    return sample;
  }

  const float drive = db_to_linear(config.drive_db);
  const float output = db_to_linear(config.output_gain_db);
  const float wet = std::tanh(sample * drive) * output;
  return sample * (1.0f - config.mix) + wet * config.mix;
}

}  // namespace sonare::mastering::multiband
