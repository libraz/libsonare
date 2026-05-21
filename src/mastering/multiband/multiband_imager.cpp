#include "mastering/multiband/multiband_imager.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sonare::mastering::multiband {

MultibandImager::MultibandImager(MultibandImagerConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
}

void MultibandImager::prepare(double sample_rate, int max_block_size) {
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

void MultibandImager::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("MultibandImager must be prepared before processing");
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
  if (num_channels >= 2) {
    for (int band = 0; band < split.num_bands(); ++band) {
      const auto& band_config = config_.bands[static_cast<size_t>(band)];
      if (!band_config.enabled || band_config.width == 1.0f) {
        continue;
      }

      auto& left = split.bands[static_cast<size_t>(band)][0];
      auto& right = split.bands[static_cast<size_t>(band)][1];
      for (int i = 0; i < num_samples; ++i) {
        const size_t index = static_cast<size_t>(i);
        const float mid = 0.5f * (left[index] + right[index]);
        const float side = 0.5f * (left[index] - right[index]) * band_config.width;
        left[index] = mid + side;
        right[index] = mid - side;
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

void MultibandImager::reset() { crossover_.reset(); }

void MultibandImager::set_config(const MultibandImagerConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

void MultibandImager::validate_config(const MultibandImagerConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw std::invalid_argument("multiband imager band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.width < 0.0f) {
      throw std::invalid_argument("imager width must be non-negative");
    }
  }
}

}  // namespace sonare::mastering::multiband
