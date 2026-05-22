#include "mastering/saturation/multiband_exciter.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sonare::mastering::saturation {

MultibandExciter::MultibandExciter(MultibandExciterConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandExciter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_);
  for (auto& exciter : exciters_) exciter.prepare(sample_rate_, max_block_size_);
  reset();
}

void MultibandExciter::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("MultibandExciter must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
  }

  auto split = crossover_.split(channels, num_channels, num_samples);
  for (int band = 0; band < split.num_bands(); ++band) {
    std::vector<float*> band_channels(static_cast<size_t>(num_channels));
    for (int ch = 0; ch < num_channels; ++ch) {
      band_channels[static_cast<size_t>(ch)] =
          split.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)].data();
    }
    exciters_[static_cast<size_t>(band)].process(band_channels.data(), num_channels, num_samples);
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
    for (int band = 0; band < split.num_bands(); ++band) {
      const auto& samples = split.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) channels[ch][i] += samples[static_cast<size_t>(i)];
    }
  }
}

void MultibandExciter::reset() {
  crossover_.reset();
  for (auto& exciter : exciters_) exciter.reset();
}

void MultibandExciter::set_config(const MultibandExciterConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  rebuild_processors();
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

void MultibandExciter::validate_config(const MultibandExciterConfig& config) {
  if (config.bands.size() != config.crossover.cutoffs_hz.size() + 1) {
    throw std::invalid_argument("multiband exciter band count must match crossover");
  }
}

void MultibandExciter::rebuild_processors() {
  exciters_.clear();
  exciters_.reserve(config_.bands.size());
  for (const auto& band : config_.bands) exciters_.emplace_back(band);
}

}  // namespace sonare::mastering::saturation
