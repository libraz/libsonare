#include "mastering/multiband/multiband_expander.h"

#include <algorithm>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"

namespace sonare::mastering::multiband {

MultibandExpander::MultibandExpander(MultibandExpanderConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandExpander::prepare(double sample_rate, int max_block_size) {
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
  // Pre-size split scratch for stereo so the steady-state audio path is
  // allocation-free; it is grown on demand only if a wider block arrives.
  crossover_.prepare_scratch(scratch_, 2, max_block_size_);
  for (auto& expander : expanders_) {
    expander.prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandExpander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandExpander");
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

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    expanders_[static_cast<size_t>(band)].process(
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
    last_gain_reductions_db_[static_cast<size_t>(band)] =
        expanders_[static_cast<size_t>(band)].last_gain_reduction_db();
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
    for (int band = 0; band < num_bands; ++band) {
      const auto& band_samples = scratch_.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] += band_samples[static_cast<size_t>(i)];
      }
    }
  }
}

void MultibandExpander::reset() {
  crossover_.reset();
  for (auto& expander : expanders_) {
    expander.reset();
  }
  std::fill(last_gain_reductions_db_.begin(), last_gain_reductions_db_.end(), 0.0f);
}

void MultibandExpander::set_config(const MultibandExpanderConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  rebuild_processors();
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

bool MultibandExpander::set_parameter(unsigned int param_id, float value) {
  const unsigned int band = param_id / kBandStride;
  if (band >= expanders_.size()) {
    return false;
  }
  const unsigned int band_param = param_id % kBandStride;
  if (expanders_[band].set_parameter(band_param, value)) {
    config_.bands[band] = expanders_[band].config();
    return true;
  }
  return false;
}

void MultibandExpander::validate_config(const MultibandExpanderConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw std::invalid_argument("multiband expander band count must match crossover");
  }
}

void MultibandExpander::rebuild_processors() {
  expanders_.clear();
  expanders_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    expanders_.emplace_back(band_config);
  }
  last_gain_reductions_db_.assign(config_.bands.size(), 0.0f);
}

}  // namespace sonare::mastering::multiband
