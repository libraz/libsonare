#include "mastering/multiband/multiband_compressor.h"

#include <algorithm>
#include <stdexcept>

namespace sonare::mastering::multiband {

MultibandCompressor::MultibandCompressor(MultibandCompressorConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandCompressor::prepare(double sample_rate, int max_block_size) {
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
  for (auto& compressor : compressors_) {
    compressor.prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandCompressor::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("MultibandCompressor must be prepared before processing");
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
    std::vector<float*> band_channels(static_cast<size_t>(num_channels));
    for (int ch = 0; ch < num_channels; ++ch) {
      band_channels[static_cast<size_t>(ch)] =
          split.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)].data();
    }
    compressors_[static_cast<size_t>(band)].process(band_channels.data(), num_channels,
                                                    num_samples);
    last_gain_reductions_db_[static_cast<size_t>(band)] =
        compressors_[static_cast<size_t>(band)].last_gain_reduction_db();
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

void MultibandCompressor::reset() {
  crossover_.reset();
  for (auto& compressor : compressors_) {
    compressor.reset();
  }
  std::fill(last_gain_reductions_db_.begin(), last_gain_reductions_db_.end(), 0.0f);
}

void MultibandCompressor::set_config(const MultibandCompressorConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  rebuild_processors();
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

void MultibandCompressor::validate_config(const MultibandCompressorConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw std::invalid_argument("multiband compressor band count must match crossover");
  }
}

void MultibandCompressor::rebuild_processors() {
  compressors_.clear();
  compressors_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    compressors_.emplace_back(band_config);
  }
  last_gain_reductions_db_.assign(config_.bands.size(), 0.0f);
}

}  // namespace sonare::mastering::multiband
